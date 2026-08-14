#!/usr/bin/env bash
# Run the cppcheck MISRA addon over the Goo compiler sources.
#
#   docs/misra/tools/scan.sh <output-file> [extra cppcheck args...] -- <files...>
#
# Prerequisite: a rule-text file built from your own licensed copy of
# MISRA C:2012, and a misra.json pointing at it. See docs/misra/tools/README.md.
#
# The flags mirror Makefile CFLAGS so cppcheck sees the same view the build
# does, minus -std=c23, which cppcheck does not support (its ceiling is c11).
set -u -o pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
CONFIG="${MISRA_JSON:-$HERE/misra.json}"
LLVM_INC="$(llvm-config --includedir 2>/dev/null || echo /usr/include)"

if [ ! -f "$CONFIG" ]; then
  echo "scan.sh: no addon config at $CONFIG" >&2
  echo "scan.sh: see docs/misra/tools/README.md; set MISRA_JSON to override" >&2
  exit 2
fi

OUT="$1"; shift

ARGS=(
  "--addon=$CONFIG"
  --std=c11
  --enable=style
  --inline-suppr
  --library=posix
  --library=gnu
  -I.
  -Iinclude
  "-I$LLVM_INC"
  -D_GNU_SOURCE
  -DLLVM_AVAILABLE=1
  --include=include/xalloc.h
  --include=include/goo_assert.h
  --suppress=missingIncludeSystem
  '--template={file}|{line}|{id}|{severity}|{message}'
)

while [ $# -gt 0 ]; do
  if [ "$1" = "--" ]; then shift; break; fi
  ARGS+=("$1"); shift
done

cd "$ROOT"
cppcheck "${ARGS[@]}" "$@" > "$OUT" 2>&1
rc=$?
echo "SCAN_EXIT=$rc" >> "$OUT"

# cppcheck exits 0 even when it reports violations. Read the report, never $?.
echo "cppcheck exit=$rc  misra findings=$(grep -c 'misra-c2012' "$OUT")  -> $OUT"
