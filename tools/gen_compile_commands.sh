#!/usr/bin/env bash
# gen_compile_commands.sh -- derives build/compile_commands.json from the
# Makefile's own recipes. Task 14 of the 2026-09-02 verification tooling
# plan, decision 5: "the build log gives all compiler calls", so every other
# analyser (task 15's cppcheck, task 16's gcc/clang-tidy lanes) reads ONE
# list instead of each re-deriving its own `find src -name '*.c'`.
#
# SCOPE: one entry for each file in GOO_OBJS -- the translation units the
# Makefile links into bin/goo (see $(COMPILER)'s prerequisite list). This is
# NOT every *.c the "lexer" target's dry run touches: RUNTIME_OBJS (the
# libgoo_runtime.a archive $(COMPILER) itself needs to run) compiles two
# further files, src/runtime/io.c and src/runtime/far_transport.c, that are
# not in GOO_OBJS and so are not captured here. This scope choice is the
# task's own acceptance line: "each src/*.c in GOO_OBJS is in the list one
# time".
#
# WHY `make -Bn lexer`, not a hand-written flag list: -n (--just-print,
# a.k.a. dry run) means make PRINTS every recipe it would run without
# running a single one -- no compiler invoked, no file written, no bison
# rerun, no vendored NNG rebuild. That makes this safe to call from
# anywhere, at any time, including from inside a concurrent
# `make -j verify-core` -- unlike an actual `-B` rebuild (see
# scripts/warning_count_probe.sh's "NO -B" note for why that IS unsafe
# there). -B (--always-make) is what makes -n useful here: without it, `-n`
# only prints recipes for OUT-OF-DATE targets, so a tree with cached .o
# files would print nothing at all for a target that "lexer" considers
# already built. `lexer` is used (not `bin/goo`/`goo`, which name the same
# recipe) because it is the target CLAUDE.md and this repo's own docs use
# for "build the compiler".
#
# WHY GOO_SRCS/GOO_OBJS are read FROM THE MAKEFILE, not hand-copied here: a
# small print-% rule is injected with `--eval`, and make expands the real
# variable for us. If a source is ever added to or removed from GOO_SRCS,
# this script sees the new list on its very next run -- there is no second
# copy of the file list to fall out of sync.
#
# THE ARGUMENTS ARRAY is the real argv gcc would receive, including the
# ccache wrapper this build already runs under and every include/define
# flag -- not a hand-trimmed subset, and never a single joined "command"
# string (a flag containing a space, like the DGOO_VERSION quoting below,
# would not survive a naive re-split of that string). It is derived with
# Python's shlex, which applies the same quote-removal a POSIX shell would:
# -DGOO_VERSION='"0.1.0"' becomes the one token -DGOO_VERSION="0.1.0", with
# the embedded double quotes kept, exactly as gcc receives it.
#
# THE "directory" FIELD is "." (a relative path), not this checkout's
# absolute path. An absolute directory baked in at generation time would
# make the derivation itself non-portable -- two checkouts of the identical
# commit, or the same checkout built from two working directories, would
# derive two different files. Every "file" and every path inside
# "arguments" is already repo-root-relative (the Makefile invokes the
# compiler from the repo root with relative paths), so "." is correct as
# long as a consumer of this file is invoked from the repo root -- which
# scripts/compile_commands_current_probe.sh and (task 15)
# scripts/misra-scan.sh both already are.
#
# OUTPUT ORDER: entries are sorted by "file" under LC_ALL=C, so the derived
# file is byte-stable across runs on an unchanged tree -- required for the
# self-consistency check in scripts/compile_commands_current_probe.sh, and
# the precedent scripts/assertion_density_probe.sh already sets for a
# committed/derived listing.
#
# build/ is git-ignored (.gitignore line 2), so this file is NEVER
# committed -- it is regenerated on demand, by the probe and by task 15's
# cppcheck lane, and its exact bytes are allowed to vary by machine (the
# ccache path, the LLVM include directory, whether ccache is installed at
# all). What must NOT vary between two runs on the SAME machine and the
# SAME tree is its shape and its count -- see the probe.
#
# Usage:
#   tools/gen_compile_commands.sh [output-path]   # default: build/compile_commands.json
set -euo pipefail

TOOL="gen-compile-commands"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

OUT="${1:-build/compile_commands.json}"
MAKE_TARGET="${GEN_CC_MAKE_TARGET:-lexer}"

# --- 1. GOO_SRCS and GOO_OBJS, as the Makefile computes them today. --------
# `make --eval` injects a throwaway rule so we can ask make to expand its
# own variable, rather than parsing the Makefile's text by hand (which
# would need to re-implement make's own substitution-reference syntax,
# $(GOO_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o), to get GOO_OBJS right).
# `tail -1` drops the $(info)/$(warning) lines the Makefile prints while
# choosing its LLVM config -- those go to the same stdout, ahead of our
# recipe's own output, on every invocation.
# --no-print-directory matters here, not just for tidiness: this script is
# invoked from INSIDE a make recipe (the compile-commands-current-probe
# target calls the gate script, which calls this one), so MAKELEVEL is
# already >= 1 in the environment this nested `make` inherits. GNU make
# auto-enables its "Entering/Leaving directory" announcements whenever it
# detects it is a sub-make, WITH NO -w NEEDED to trigger it -- and prints
# them to the SAME stdout our recipe's own echo uses, with "Leaving
# directory" landing AFTER it. A bare `tail -1` then silently returns that
# announcement instead of the real value: this script worked from an
# interactive shell (MAKELEVEL=0, no announcement) and failed only when
# `make compile-commands-current-probe` ran it for real -- proof the two
# invocation modes are not interchangeable for this kind of nested-make
# call. The GOO_MAKE_VAR= sentinel is the second, independent guard: even
# with the announcement suppressed, sed only accepts a line we ourselves
# tagged, so any OTHER future noise on this stdout (a $(warning), a second
# $(info)) cannot be mistaken for the value either.
read_make_var() { # $1 = variable name
	make --no-print-directory -f Makefile \
		--eval="print-$1: ; @echo GOO_MAKE_VAR=\$($1)" \
		"print-$1" 2>/dev/null \
		| sed -n 's/^GOO_MAKE_VAR=//p' \
		| tail -1
}

goo_srcs_raw="$(read_make_var GOO_SRCS)"
goo_objs_raw="$(read_make_var GOO_OBJS)"

if [ -z "$goo_srcs_raw" ] || [ -z "$goo_objs_raw" ]; then
	echo "$TOOL: FAIL (could not read GOO_SRCS/GOO_OBJS from the Makefile)" >&2
	exit 1
fi

read -r -a SRCS <<<"$goo_srcs_raw"
read -r -a OBJS <<<"$goo_objs_raw"

if [ "${#SRCS[@]}" -ne "${#OBJS[@]}" ]; then
	echo "$TOOL: FAIL (GOO_SRCS has ${#SRCS[@]} entries, GOO_OBJS has ${#OBJS[@]} -- the Makefile's own substitution should keep these equal)" >&2
	exit 1
fi
if [ "${#SRCS[@]}" -eq 0 ]; then
	echo "$TOOL: FAIL (GOO_SRCS is empty)" >&2
	exit 1
fi

# --- 2. The dry-run build log: every command `make $MAKE_TARGET` would run,
#        including the two RUNTIME_OBJS-only files this script deliberately
#        excludes (see header). -B forces every recipe to print regardless
#        of whether the objects are already up to date; -n means none of
#        them actually run. ---------------------------------------------
DRYRUN="$(make -Bn "$MAKE_TARGET" 2>&1)"

# --- 3. One (file, compile-line) pair for each GOO_SRCS entry. ------------
pairs_file="$(mktemp)"
trap 'rm -f "$pairs_file"' EXIT

missing=0
for i in "${!SRCS[@]}"; do
	src="${SRCS[$i]}"
	obj="${OBJS[$i]}"
	# Fixed-string match on " -c <src> -o <obj>": this is the exact
	# substring the generic $(BUILDDIR)/%.o pattern rule's recipe always
	# ends with, whatever compiler and flags precede it.
	line="$(printf '%s\n' "$DRYRUN" | grep -F -- " -c $src -o $obj" | head -1)"
	if [ -z "$line" ]; then
		echo "$TOOL: FAIL (no compile line for $src -> $obj in \`make -Bn $MAKE_TARGET\` -- make -n may have skipped a translation unit)" >&2
		missing=1
		continue
	fi
	printf '%s\t%s\n' "$src" "$line" >>"$pairs_file"
done

if [ "$missing" -ne 0 ]; then
	exit 1
fi

# --- 4. Emit the compilation database. Python's shlex does the one part a
#        shell script should not hand-roll: quote-aware tokenizing of each
#        recipe line into the real argv gcc receives. -----------------
mkdir -p "$(dirname "$OUT")"
tmp_out="$(mktemp "$(dirname "$OUT")/.compile_commands.XXXXXX.json")"
trap 'rm -f "$pairs_file" "$tmp_out"' EXIT

python3 - "$pairs_file" "$tmp_out" <<'PYEOF'
import json
import shlex
import sys

pairs_path, out_path = sys.argv[1], sys.argv[2]

entries = []
with open(pairs_path, encoding="utf-8") as f:
    for raw_line in f:
        raw_line = raw_line.rstrip("\n")
        if not raw_line:
            continue
        src, cmd = raw_line.split("\t", 1)
        entries.append({
            "directory": ".",
            "file": src,
            "arguments": shlex.split(cmd),
        })

# LC_ALL=C order: plain code-point comparison on these ASCII repo-relative
# paths is byte-for-byte what `sort` under LC_ALL=C would give.
entries.sort(key=lambda e: e["file"])

with open(out_path, "w", encoding="utf-8") as f:
    json.dump(entries, f, indent=2)
    f.write("\n")

print(f"wrote {len(entries)} entries", file=sys.stderr)
PYEOF

mv "$tmp_out" "$OUT"
trap 'rm -f "$pairs_file"' EXIT

count="$(python3 -c "import json,sys; print(len(json.load(open(sys.argv[1]))))" "$OUT")"
echo "$TOOL: wrote $count entries to $OUT"
