# Reproducible Builds with Podman — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `bin/goo` and `lib/libgoo_runtime.a` byte-identical across machines by pinning the toolchain in a podman image and removing the one real source of nondeterminism in the build.

**Architecture:** A digest-pinned `ubuntu:24.04` image carries the exact CI toolchain. Builds run at a fixed `/src` path, so DWARF `comp_dir` is constant regardless of host checkout location. One Makefile change makes `ar` stop stamping member mtimes. A probe builds twice from `git archive HEAD` in fresh containers and compares hashes.

**Tech Stack:** podman 5.8.4 (rootless), GNU make, GNU binutils `ar`/`ranlib`, bash, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-08-19-reproducible-builds-podman-design.md`

## Global Constraints

- Base image is pinned **by digest, not tag**: `docker.io/library/ubuntu@sha256:1e0a86e57d247923571b75e0aaf48a1449cf8c543d51fb3e07a4a7d7bfa79316`
- In-container source path is always `/src`. Never parameterise it — a varying path re-introduces the 58 `DW_AT_comp_dir` entries the fixed path eliminates.
- Every build invocation passes `CCACHE=` explicitly. `Makefile:7` is `CCACHE ?= $(shell command -v ccache)`; with ccache active a second build replays cached objects and reports IDENTICAL regardless of compiler behaviour.
- Toolchain packages, matching `.github/workflows/tests.yml` exactly: `make gcc-14 bison llvm-dev clang libblocksruntime-dev valgrind cmake libjson-c-dev libcurl4-openssl-dev zlib1g-dev`, plus `python3`.
- `ccache` must NOT be installed in the image.
- Builds use `CC=gcc-14`, matching CI.
- Source reaches the gate container via `git archive HEAD`, never `cp -r .`. A recursive copy carries `build/`, `bin/` and `lib/` in, and make then treats stale artifacts as up to date.
- Commit style: conventional commits, imperative mood. Commit with `git -c commit.gpgsign=false commit` — the 1Password SSH signing agent fails in this environment.
- **Every git write operation in this repo must be backgrounded.** `git commit` runs `make test` in a pre-commit hook and `git push` runs `make verify-core` in a pre-push hook. Each exceeds a 2-minute foreground window and leaves the operation silently undone.

---

### Task 1: Deterministic archives

`lib/libgoo_runtime.a` differs between two clean builds by 206 bytes. All 102 members extract byte-identical, so the difference is entirely `ar` metadata: the recipe pipes an MRI script to `ar -M`, which records each member's real mtime, uid, gid and mode.

This task needs no container and runs in seconds, so its probe belongs in `verify-core`.

**Files:**
- Create: `scripts/archive_determinism_probe.sh`
- Modify: `Makefile` — the `$(RUNTIME_LIB)` recipe, and `VERIFY_ALL_DEPS`

**Interfaces:**
- Consumes: nothing.
- Produces: make target `archive-determinism-probe`; `lib/libgoo_runtime.a` becomes reproducible.

- [ ] **Step 1: Write the failing probe**

Create `scripts/archive_determinism_probe.sh`:

```bash
#!/bin/bash
# archive-determinism probe — building lib/libgoo_runtime.a twice from the same
# objects must give a byte-identical archive.
#
# WHAT WENT WRONG. The recipe pipes an MRI script to `ar -M`, which stores each
# member's real mtime, uid, gid and mode. Two builds seconds apart produced
# archives differing by 206 bytes, while all 102 members extracted
# byte-identical. Every compiled object was already deterministic; the wrapper
# around them was not.
#
# This probe re-archives the ALREADY-BUILT objects rather than rebuilding them,
# so it costs seconds and needs no compiler. It touches the objects' mtimes
# between the two archives, which is exactly the condition that made the real
# builds differ.
set -u

PROBE="archive-determinism-probe"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIB="$ROOT/lib/libgoo_runtime.a"

if [ ! -f "$LIB" ]; then
    echo "$PROBE: FAIL (lib/libgoo_runtime.a is missing — run 'make runtime-lib' first)"
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/m"
( cd "$WORK/m" && ar x "$LIB" ) || { echo "$PROBE: FAIL (could not extract $LIB)"; exit 1; }

count=$(find "$WORK/m" -name '*.o' | wc -l)
if [ "$count" -eq 0 ]; then
    echo "$PROBE: FAIL (archive extracted zero members — the probe would pass vacuously)"
    exit 1
fi

# Rebuild the archive twice in the recipe's own shape, touching mtimes between.
mri() {  # $1 = output archive
    rm -f "$1"
    { echo "create $1"
      for o in "$WORK"/m/*.o; do echo "addmod $o"; done
      echo "save"; echo "end"; } | ar -D -M
    ranlib -D "$1"
}

mri "$WORK/a1.a"
touch "$WORK"/m/*.o
mri "$WORK/a2.a"

if cmp -s "$WORK/a1.a" "$WORK/a2.a"; then
    echo "$PROBE: PASS ($count members, archive is byte-identical across mtime changes)"
    exit 0
fi

echo "$PROBE: FAIL — two archives of the same $count members differ."
echo "  \`ar\` is recording member mtime/uid/gid. The recipe needs 'ar -D -M' and 'ranlib -D'."
exit 1
```

- [ ] **Step 2: Prove the probe can go red**

The probe as written already uses `-D`, so it passes. Prove it detects the defect by temporarily removing `-D` and confirming a red result:

```bash
chmod +x scripts/archive_determinism_probe.sh
make runtime-lib                                   # ensure lib/ exists
sed -i 's/| ar -D -M/| ar -M/; s/ranlib -D "\$1"/ranlib "$1"/' scripts/archive_determinism_probe.sh
grep -n 'ar -M' scripts/archive_determinism_probe.sh   # CONFIRM the mutation landed
./scripts/archive_determinism_probe.sh; echo "exit=$?"
```

Expected: `archive-determinism-probe: FAIL`, `exit=1`.

A mutation that fails to apply reads exactly like a passing probe, so the `grep` is not optional.

- [ ] **Step 3: Restore the probe and confirm green**

```bash
git checkout scripts/archive_determinism_probe.sh 2>/dev/null || \
  sed -i 's/| ar -M/| ar -D -M/; s/ranlib "\$1"/ranlib -D "$1"/' scripts/archive_determinism_probe.sh
./scripts/archive_determinism_probe.sh; echo "exit=$?"
```

Expected: `PASS`, `exit=0`.

- [ ] **Step 4: Fix the real recipe**

In `Makefile`, find the `$(RUNTIME_LIB)` recipe:

```make
$(RUNTIME_LIB): $(RUNTIME_OBJS) $(NNG_LIB) | $(LIBDIR)
	rm -f $@
	{ echo "create $@"; \
	  for o in $(RUNTIME_OBJS); do echo "addmod $$o"; done; \
	  echo "addlib $(NNG_LIB)"; \
	  echo "save"; echo "end"; } | ar -M
	ranlib $@
```

Replace the last two lines so it reads:

```make
$(RUNTIME_LIB): $(RUNTIME_OBJS) $(NNG_LIB) | $(LIBDIR)
	rm -f $@
	# -D: zero member mtime/uid/gid/mode. Without it two builds seconds apart
	# produce archives differing by 206 bytes while every member is identical.
	{ echo "create $@"; \
	  for o in $(RUNTIME_OBJS); do echo "addmod $$o"; done; \
	  echo "addlib $(NNG_LIB)"; \
	  echo "save"; echo "end"; } | ar -D -M
	ranlib -D $@
```

- [ ] **Step 5: Verify the real build is now reproducible end to end**

```bash
rm -rf lib build bin
make CCACHE= -j"$(nproc)" lib/libgoo_runtime.a >/dev/null 2>&1
sha256sum lib/libgoo_runtime.a > /tmp/lib.A
rm -rf lib build bin
make CCACHE= -j"$(nproc)" lib/libgoo_runtime.a >/dev/null 2>&1
sha256sum lib/libgoo_runtime.a > /tmp/lib.B
diff <(cut -d' ' -f1 /tmp/lib.A) <(cut -d' ' -f1 /tmp/lib.B) && echo "ARCHIVE REPRODUCIBLE"
```

Expected: `ARCHIVE REPRODUCIBLE`. Note `rm -rf lib` is required — `make clean` removes `build/` and `bin/` but NOT `lib/`.

- [ ] **Step 6: Wire the probe into verify-core**

In `Makefile`, add the target near the other probe targets:

```make
# The Makefile's own archive step was the only measured nondeterminism in the
# build. Needs no compiler and no container, so it belongs in verify-core.
archive-determinism-probe: runtime-lib
	@bash scripts/archive_determinism_probe.sh
.PHONY: archive-determinism-probe
```

Then add `archive-determinism-probe` to the `VERIFY_ALL_DEPS := \` list (starts at `Makefile:3146`), following the existing one-per-line continuation style.

- [ ] **Step 7: Confirm the gate runs and passes**

```bash
make archive-determinism-probe; echo "exit=$?"
make -n verify-core | grep -c archive_determinism_probe
```

Expected: `PASS`, `exit=0`, and a non-zero count proving it is reachable from `verify-core`.

- [ ] **Step 8: Commit**

```bash
git add scripts/archive_determinism_probe.sh Makefile
git -c commit.gpgsign=false commit -F - <<'MSG'
fix(build): make lib/libgoo_runtime.a reproducible

Two clean builds produced archives differing by 206 bytes. All 102 members
extracted byte-identical, so every compiled object was already deterministic
and the difference was entirely `ar` metadata: the recipe pipes an MRI script
to `ar -M`, which records each member's real mtime, uid, gid and mode.

`ar -D -M` plus `ranlib -D` zeroes those fields. Measured in the recipe's own
MRI shape rather than assumed to transfer from the `ar rcs` form, because the
two invocations take the flag differently.

New gate archive-determinism-probe re-archives the already-built objects with
their mtimes touched, which is the condition that made the real builds differ.
It needs no compiler and no container, so it runs in verify-core. Proved red by
removing -D before it was trusted.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

Run this backgrounded — the pre-commit hook runs `make test` and exceeds a 2-minute foreground window.

---

### Task 2: The pinned image

**Files:**
- Create: `Containerfile`
- Create: `scripts/podman_image_probe.sh`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: image tag `goolang-build:local`, and `/etc/goo-toolchain.txt` inside it listing resolved package versions.

- [ ] **Step 1: Write the failing image probe**

Create `scripts/podman_image_probe.sh`:

```bash
#!/bin/bash
# podman-image probe — the build image must carry the exact CI toolchain, must
# NOT carry ccache, and must record what it resolved.
#
# ccache is excluded deliberately. It gives nothing to a one-shot build, and its
# presence lets a determinism gate pass for the wrong reason: with ccache active
# a second build replays cached objects and reports IDENTICAL whatever the
# compiler does.
set -u

PROBE="podman-image-probe"
IMAGE="${GOO_BUILD_IMAGE:-goolang-build:local}"

if ! command -v podman >/dev/null 2>&1; then
    echo "$PROBE: SKIPPED (podman not on PATH)"
    exit 0
fi

if ! podman image exists "$IMAGE" 2>/dev/null; then
    echo "$PROBE: FAIL ($IMAGE is not built — run 'make podman-image')"
    exit 1
fi

fail=0
need() {  # $1 = command that must exist in the image
    if podman run --rm "$IMAGE" sh -c "command -v $1 >/dev/null 2>&1"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"; fail=1
    fi
}
for c in make gcc-14 bison clang valgrind cmake python3; do need "$c"; done

if podman run --rm "$IMAGE" sh -c 'command -v ccache >/dev/null 2>&1'; then
    echo "  PRESENT BUT MUST NOT BE: ccache"
    fail=1
else
    echo "  ok: ccache absent"
fi

if podman run --rm "$IMAGE" test -s /etc/goo-toolchain.txt; then
    echo "  ok: /etc/goo-toolchain.txt recorded"
else
    echo "  MISSING: /etc/goo-toolchain.txt"; fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "$PROBE: FAIL"
    exit 1
fi
echo "$PROBE: PASS (toolchain present, ccache absent, versions recorded)"
exit 0
```

- [ ] **Step 2: Run it to verify it fails**

```bash
chmod +x scripts/podman_image_probe.sh
./scripts/podman_image_probe.sh; echo "exit=$?"
```

Expected: `FAIL (goolang-build:local is not built ...)`, `exit=1`.

- [ ] **Step 3: Write the Containerfile**

Create `Containerfile` at the repository root:

```dockerfile
# Build image for reproducible goolang builds.
#
# PINNED BY DIGEST, NOT TAG. A tag is a moving pointer, so `ubuntu:24.04` today
# and in six months are different images and would produce different binaries.
#
# The package list matches .github/workflows/tests.yml exactly, so this image
# reproduces the environment CI already validates rather than adding a fourth
# toolchain to reason about.
FROM docker.io/library/ubuntu@sha256:1e0a86e57d247923571b75e0aaf48a1449cf8c543d51fb3e07a4a7d7bfa79316

# ccache is deliberately ABSENT. See scripts/podman_image_probe.sh for why.
RUN apt-get update && apt-get install -y --no-install-recommends \
        make gcc-14 bison llvm-dev clang libblocksruntime-dev \
        valgrind cmake python3 \
        libjson-c-dev libcurl4-openssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Record what actually resolved. A build that cannot say what produced it is
# hard to audit later.
RUN { echo "# resolved $(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
      gcc-14 --version | head -1; \
      clang --version | head -1; \
      bison --version | head -1; \
      valgrind --version; \
      cmake --version | head -1; \
      ls /usr/bin/llvm-config-* 2>/dev/null | sort -V | tail -1 | xargs -r -I{} {} --version | sed 's/^/llvm-config /'; \
      dpkg-query -W -f='${Package}=${Version}\n' make gcc-14 bison llvm-dev clang cmake python3; \
    } > /etc/goo-toolchain.txt

WORKDIR /src
```

- [ ] **Step 4: Add the image build target**

In `Makefile`, add:

```make
# Reproducible-build image. Digest-pinned in the Containerfile.
podman-image:
	podman build -t goolang-build:local -f Containerfile .
.PHONY: podman-image

podman-image-probe:
	@bash scripts/podman_image_probe.sh
.PHONY: podman-image-probe
```

- [ ] **Step 5: Build the image and verify the probe passes**

```bash
make podman-image
./scripts/podman_image_probe.sh; echo "exit=$?"
podman run --rm goolang-build:local cat /etc/goo-toolchain.txt
```

Expected: `PASS`, `exit=0`, and a version listing showing `gcc-14`.

- [ ] **Step 6: Commit**

```bash
git add Containerfile scripts/podman_image_probe.sh Makefile
git -c commit.gpgsign=false commit -F - <<'MSG'
feat(build): add a digest-pinned podman build image

The repository builds against three different toolchains today: gcc 16 with
LLVM 22 on a Fedora developer box, gcc-14 with LLVM 18 on the CI runner, and a
third implied by the /opt/homebrew include path in CFLAGS. Nothing records
which one produced a given binary.

The image pins ubuntu:24.04 BY DIGEST and installs the package list from
tests.yml verbatim, so it reproduces the environment CI already validates
instead of adding a fourth. It writes its resolved versions to
/etc/goo-toolchain.txt.

ccache is deliberately absent: it gives nothing to a one-shot build, and with it
active a second build replays cached objects and reports IDENTICAL whatever the
compiler does. That is the trap the baseline measurement had to bypass.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

Run backgrounded.

---

### Task 3: The build wrapper

**Files:**
- Create: `scripts/podman_build.sh`

**Interfaces:**
- Consumes: image `goolang-build:local` from Task 2.
- Produces: `scripts/podman_build.sh gate <outdir>` writes `bin/goo` and `lib/libgoo_runtime.a` into `<outdir>`; `scripts/podman_build.sh dev [make-args...]` runs make against a mounted tree.

- [ ] **Step 1: Write the wrapper**

Create `scripts/podman_build.sh`:

```bash
#!/bin/bash
# Run a goolang build inside the pinned image.
#
# TWO MODES, because a gate and a person want different things.
#
#   gate <outdir>  Source is COPIED IN from `git archive HEAD`. No host writes,
#                  so rootless-podman file ownership and SELinux labelling never
#                  arise. `git archive` also excludes build/, bin/ and lib/,
#                  which matters: a recursive copy would carry stale artifacts in
#                  and make would treat them as up to date, so two "clean" builds
#                  would compare the same untouched files and pass forever.
#
#   dev [args...]  Source is MOUNTED. Iteration without a rebuild. `:z` is
#                  required on a Fedora host for SELinux; --userns=keep-id stops
#                  root-owned files landing in build/ and bin/.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${GOO_BUILD_IMAGE:-goolang-build:local}"
MODE="${1:-}"

# CCACHE= is passed on every make line. Makefile:7 is
# `CCACHE ?= $(shell command -v ccache)`, so a future image that gained ccache
# would silently start replaying cached objects.
MAKEVARS='CCACHE= CC=gcc-14'

case "$MODE" in
  gate)
    OUT="${2:-}"
    if [ -z "$OUT" ]; then echo "usage: $0 gate <outdir>" >&2; exit 2; fi
    mkdir -p "$OUT"
    TAR="$(mktemp)"; trap 'rm -f "$TAR"' EXIT
    ( cd "$ROOT" && git archive HEAD ) > "$TAR"

    # `podman cp` feeds the tarball in, so the container's own stdin stays free.
    # Piping the script to `sh -s` AND the tar to stdin cannot both work.
    CID="$(podman create \
      --env SOURCE_DATE_EPOCH="$(cd "$ROOT" && git log -1 --format=%ct)" \
      --volume "$OUT:/out:z" \
      "$IMAGE" sh -c '
        cd /src
        make CCACHE= CC=gcc-14 -j"$(nproc)" bin/goo lib/libgoo_runtime.a >/tmp/build.log 2>&1 || {
          echo "build FAILED"; tail -40 /tmp/build.log; exit 1; }
        cp bin/goo /out/goo
        cp lib/libgoo_runtime.a /out/libgoo_runtime.a')"
    podman cp - "$CID:/src" < "$TAR"
    podman start -a "$CID"; rc=$?
    podman rm -f "$CID" >/dev/null 2>&1
    exit $rc
    ;;

  dev)
    shift
    podman run --rm -it \
      --volume "$ROOT:/src:z" \
      --userns=keep-id \
      --env SOURCE_DATE_EPOCH="$(cd "$ROOT" && git log -1 --format=%ct)" \
      "$IMAGE" make $MAKEVARS "$@"
    ;;

  *)
    echo "usage: $0 {gate <outdir>|dev [make-args...]}" >&2
    exit 2
    ;;
esac
```

- [ ] **Step 2: Run a gate build and verify artifacts appear**

```bash
chmod +x scripts/podman_build.sh
rm -rf /tmp/gate1 && ./scripts/podman_build.sh gate /tmp/gate1
ls -la /tmp/gate1/
file /tmp/gate1/goo | head -1
```

Expected: `goo` and `libgoo_runtime.a` present; `goo` reported as an ELF executable.

- [ ] **Step 3: Verify the fixed path took effect**

```bash
readelf --debug-dump=info /tmp/gate1/goo 2>/dev/null | grep -oE 'DW_AT_comp_dir.*: .*' | sed 's/.*: //' | sort -u
```

Expected: exactly one line, `/src`. Any host path here means the fixed-path design failed and the binary is not portable between checkouts.

- [ ] **Step 4: Commit**

```bash
git add scripts/podman_build.sh
git -c commit.gpgsign=false commit -F - <<'MSG'
feat(build): add the podman build wrapper

One wrapper, two modes, so a person and the gate run the same command instead
of two that drift apart.

`gate` copies the source in from `git archive HEAD`. That is not a stylistic
choice: `git archive` excludes build/, bin/ and lib/, and a recursive copy would
carry stale artifacts in so make would treat them as up to date. Two "clean"
builds would then compare the same untouched files and pass forever.

`dev` mounts instead, with :z for SELinux on a Fedora host and --userns=keep-id
so root-owned files do not land in build/ and bin/.

Both pass CCACHE= explicitly, because Makefile:7 resolves CCACHE from PATH.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

Run backgrounded.

---

### Task 4: The reproducibility gate

**Files:**
- Create: `scripts/repro_build_probe.sh`
- Modify: `Makefile` — add `repro-build-probe`, add `HEAVY_DEPS`, change `VERIFY_CORE_DEPS`

**Interfaces:**
- Consumes: `scripts/podman_build.sh gate <outdir>` from Task 3.
- Produces: make target `repro-build-probe`, reachable from `verify` but NOT from `verify-core`.

- [ ] **Step 1: Write the probe**

Create `scripts/repro_build_probe.sh`:

```bash
#!/bin/bash
# repro-build probe — two builds of the same commit, in two fresh containers,
# must produce byte-identical bin/goo and lib/libgoo_runtime.a.
#
# THE FAILURE THIS GATE IS MOST LIKELY TO HAVE is that both builds write the
# same path and the script hashes one file twice, which passes forever. That is
# what --self-test exists to catch: it injects real nondeterminism and asserts
# this probe reports DIFFER.
#
# BOTH artifacts are compared. bin/goo does not depend on the runtime archive
# (`$(COMPILER): $(GOO_OBJS) $(COMPILER_SRCS)`), so building one proves nothing
# about the other. The archive is also where `ar` nondeterminism hid.
set -u

PROBE="repro-build-probe"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v podman >/dev/null 2>&1; then
    echo "$PROBE: SKIPPED (podman not on PATH)"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

build() {  # $1 = output dir
    if ! "$ROOT/scripts/podman_build.sh" gate "$1" >"$WORK/build.log" 2>&1; then
        echo "$PROBE: FAIL (build into $1 errored)"
        tail -30 "$WORK/build.log"
        return 1
    fi
}

build "$WORK/A" || exit 1
build "$WORK/B" || exit 1

rc=0
for art in goo libgoo_runtime.a; do
    a="$WORK/A/$art"; b="$WORK/B/$art"
    if [ ! -f "$a" ] || [ ! -f "$b" ]; then
        echo "  MISSING: $art was not produced by both builds"; rc=1; continue
    fi
    # Guard against the same-file bug: the two paths must be distinct inodes.
    if [ "$(stat -c %i "$a")" = "$(stat -c %i "$b")" ]; then
        echo "  FAIL: $art is the SAME FILE in both builds — the comparison is vacuous"; rc=1; continue
    fi
    if cmp -s "$a" "$b"; then
        echo "  ok: $art identical ($(sha256sum "$a" | cut -c1-16)...)"
    else
        echo "  DIFFER: $art"
        echo "    A $(sha256sum "$a" | cut -d' ' -f1)"
        echo "    B $(sha256sum "$b" | cut -d' ' -f1)"
        rc=1
    fi
done

if [ "$rc" -ne 0 ]; then
    echo "$PROBE: FAIL — the build is not reproducible."
    exit 1
fi
echo "$PROBE: PASS (bin/goo and lib/libgoo_runtime.a byte-identical across two containers)"
exit 0
```

- [ ] **Step 2: Run it and verify it passes**

```bash
chmod +x scripts/repro_build_probe.sh
./scripts/repro_build_probe.sh; echo "exit=$?"
```

Expected: two `ok:` lines and `PASS`, `exit=0`. This takes two full builds — several minutes.

- [ ] **Step 3: Add the self-test teeth**

Insert this block into `scripts/repro_build_probe.sh` immediately after the `ROOT=` and `PROBE=` assignments, and before the `command -v podman` check.

It operates on a **throwaway clone**, never the working repository. An earlier draft committed the mutation to the real repo and undid it with `git reset --hard`; that risks the user's work if anything between the two steps fails, and a probe must never be able to destroy the tree it is testing.

```bash
if [ "${1:-}" = "--self-test" ]; then
    # TEETH. Inject real nondeterminism and require this probe to notice.
    # A mutation that fails to apply reads exactly like a passing probe, so the
    # injection is verified before the red result is trusted.
    ST="$(mktemp -d)"; trap 'rm -rf "$ST"' EXIT
    CLONE="$ST/clone"
    VICTIM="src/runtime/runtime.c"

    # A local clone is hardlinked, so this is cheap. The working repository is
    # never written to.
    if ! git clone -q "$ROOT" "$CLONE" 2>"$ST/clone.log"; then
        echo "$PROBE --self-test: FAIL (could not clone the repository)"
        cat "$ST/clone.log"; exit 1
    fi

    printf '\nconst char *goo_selftest_stamp(void) { return __TIME__; }\n' >> "$CLONE/$VICTIM"

    # VERIFY THE INJECTION LANDED. A mutation that fails to apply reads exactly
    # like a passing probe, so a red result is only meaningful once this holds.
    if ! grep -q '__TIME__' "$CLONE/$VICTIM"; then
        echo "$PROBE --self-test: FAIL (the injection did not land; a red result here would be meaningless)"
        exit 1
    fi

    # The gate builds from `git archive HEAD`, so the mutation must be committed
    # to reach the container. --no-verify skips the clone's pre-commit hook.
    git -C "$CLONE" add "$VICTIM"
    git -C "$CLONE" -c commit.gpgsign=false commit -q --no-verify -m "TEMP: self-test injection"

    ( cd "$CLONE" && ./scripts/repro_build_probe.sh ) >"$ST/out.log" 2>&1
    st=$?
    if [ "$st" -eq 0 ]; then
        echo "$PROBE --self-test: FAIL (probe reported PASS on a deliberately nondeterministic build)"
        sed 's/^/    /' "$ST/out.log"
        exit 1
    fi
    echo "$PROBE --self-test: PASS (injected __TIME__ was detected; probe can go red)"
    exit 0
fi
```

- [ ] **Step 4: Run the self-test**

```bash
./scripts/repro_build_probe.sh --self-test; echo "exit=$?"
git status --short    # MUST be clean — the temp commit is reset
```

Expected: `--self-test: PASS`, `exit=0`, and a clean tree.

- [ ] **Step 5: Wire into verify but not verify-core**

In `Makefile`, add the target:

```make
# Two full builds in two containers. Costs minutes and needs podman, so it is
# NOT in verify-core — CLAUDE.md records that target as "safe for pre-push on
# any machine", and a podman dependency would break that promise.
repro-build-probe:
	@bash scripts/repro_build_probe.sh
.PHONY: repro-build-probe
```

Add `repro-build-probe` to `VERIFY_ALL_DEPS`. Then replace this line:

```make
VERIFY_CORE_DEPS := $(filter-out v2-bootstrap-pilot,$(VERIFY_ALL_DEPS))
```

with:

```make
# Gates that need a toolchain verify-core deliberately does not assume:
# v2-bootstrap-pilot needs an opam CompCert switch, repro-build-probe needs
# podman. Both belong in `verify`, never in `verify-core`.
HEAVY_DEPS       := v2-bootstrap-pilot repro-build-probe
VERIFY_CORE_DEPS := $(filter-out $(HEAVY_DEPS),$(VERIFY_ALL_DEPS))
```

- [ ] **Step 6: Prove the wiring is right in both directions**

```bash
make -n verify-core | grep -c repro_build_probe   # expect 0
make -n verify      | grep -c repro_build_probe   # expect >= 1
```

Expected: `0` then a non-zero number. Both assertions matter — the first proves `verify-core` stayed podman-free, the second proves the gate is actually reachable and not orphaned.

- [ ] **Step 7: Commit**

```bash
git add scripts/repro_build_probe.sh Makefile
git -c commit.gpgsign=false commit -F - <<'MSG'
feat(build): gate build reproducibility with repro-build-probe

Two builds of the same commit, in two fresh containers, must produce identical
bin/goo and lib/libgoo_runtime.a. Both are compared: bin/goo does not depend on
the runtime archive, so building one proves nothing about the other, and the
archive is where the only measured nondeterminism actually was.

The probe guards its own vacuity twice. It refuses to pass if the two artifacts
share an inode, and --self-test injects a __TIME__ stamp, verifies the injection
landed, and requires the probe to report DIFFER. Without that, a script bug that
hashes one file twice would pass forever and look exactly like success.

Lands in `verify`, not `verify-core`. It costs two full builds and needs podman,
and CLAUDE.md records verify-core as safe for pre-push on any machine. The
filter that built the core list now names a HEAVY_DEPS set, because a second
entry makes the intent worth stating.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

Run backgrounded.

---

### Task 5: CI job

**Files:**
- Create: `.github/workflows/repro.yml`

**Interfaces:**
- Consumes: `make repro-build-probe` from Task 4.
- Produces: a CI job that fails if the build stops being reproducible, and fails if the probe skipped.

- [ ] **Step 1: Write the workflow**

Create `.github/workflows/repro.yml`:

```yaml
name: repro

# Separate from `tests` on purpose. This job runs two full builds, where
# verify-core takes about 3.5 minutes, and it needs podman which verify-core
# deliberately does not assume.
on:
  pull_request:
    paths:
      - 'Containerfile'
      - 'scripts/podman_build.sh'
      - 'scripts/repro_build_probe.sh'
      - 'Makefile'
  workflow_dispatch:

concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: ${{ github.ref != 'refs/heads/main' }}

jobs:
  repro-build:
    runs-on: ubuntu-24.04
    timeout-minutes: 60
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0   # the probe builds from `git archive HEAD`

      - name: Tool versions
        run: podman --version

      - name: Build the pinned image
        run: make podman-image

      - name: Prove the image is right
        run: bash scripts/podman_image_probe.sh | tee image.log

      - name: repro-build-probe
        run: bash scripts/repro_build_probe.sh 2>&1 | tee repro.log

      # A GATE THAT SILENTLY SKIPS IS NOT A GATE. The probe returns 0 when
      # podman is absent, so a runner without it would report green while
      # testing nothing. Require a positive PASS line.
      - name: Prove the probe did not skip
        if: always()
        run: |
          if grep -q "^repro-build-probe: PASS" repro.log; then
            echo "ok: repro-build-probe ran and passed"
          else
            echo "::error::repro-build-probe did not report PASS — it skipped or never ran"
            exit 1
          fi
```

- [ ] **Step 2: Verify the workflow references only real targets**

```bash
bash scripts/workflow_targets_probe.sh; echo "exit=$?"
```

Expected: `PASS` with the target count increased, `exit=0`. This is the gate added in PR #303 — every `make <target>` in a workflow must name a real Makefile rule.

- [ ] **Step 3: Verify the YAML parses**

```bash
python3 - <<'PY'
import sys
try:
    import yaml
except ImportError:
    print("PyYAML absent — falling back to structural checks only")
    yaml = None
src = open('.github/workflows/repro.yml').read()
assert '\t' not in src, "YAML must not contain tabs"
if yaml:
    doc = yaml.safe_load(src)
    steps = doc['jobs']['repro-build']['steps']
    names = [s.get('name', '') for s in steps]
    assert 'repro-build-probe' in names, names
    assert names[-1] == 'Prove the probe did not skip', names[-1]
    print("workflow: parses, probe step present, anti-skip step is last")
else:
    assert 'repro-build-probe' in src
    print("workflow: no tabs, probe referenced")
PY
```

Expected: `workflow: parses, probe step present, anti-skip step is last` (or the fallback line if PyYAML is absent).

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/repro.yml
git -c commit.gpgsign=false commit -F - <<'MSG'
ci: run the reproducibility gate on build-infrastructure changes

Separate workflow from `tests`, because this job runs two full builds where
verify-core takes about 3.5 minutes, and it needs podman which verify-core
deliberately does not assume.

Path-filtered to the files that can break reproducibility, so ordinary source
changes do not pay for it.

The probe returns 0 when podman is absent, so a runner without it would report
green while testing nothing. A final step requires a positive PASS line, which
is the same anti-vacuity pattern tests.yml already uses for the valgrind and
tsan gates.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

Run backgrounded.

---

## Verification of the whole plan

After all five tasks:

```bash
make verify-core                                  # must stay green, and podman-free
make -n verify-core | grep -c repro_build_probe   # expect 0
make repro-build-probe                            # expect PASS
./scripts/repro_build_probe.sh --self-test        # expect PASS (teeth work)
git status --short                                # expect clean
```

## Known limits carried from the spec

1. Reproducible only inside this image. A native gcc-16 build will differ, and that is expected.
2. Only as reproducible as the Ubuntu archive. `apt-get install gcc-14` may resolve differently in a year. The mitigation, deferred, is snapshot-mirror pinning with exact package versions.
3. `v2-bootstrap-pilot` is out of scope — it needs an opam CompCert switch this image does not carry.
4. Nothing here detects a stale `parser.tab.c`. Bison is pinned inside the container; a native build with a different bison is not covered.
