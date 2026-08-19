# Reproducible builds with podman

Status: design, approved 2026-08-19. Implementation not started.

## The problem, measured before it was designed for

`make` produces a different `bin/goo` on a developer's machine than on the CI
runner, and nothing records which toolchain produced a given binary.

| | Local (Fedora 44) | CI (ubuntu-24.04) |
|---|---|---|
| gcc | 16.1.1 | gcc-14 (pinned by the workflow) |
| clang / LLVM | 22.1.8 | `llvm-dev`, about 18 |
| bison | 3.8.2 | whatever ubuntu-24.04 ships |
| `Makefile:8` default | `CC = gcc` -> gcc 16 | overridden to `CC=gcc-14` |

`CFLAGS` also carries `-I/opt/homebrew/include`, so a third environment
(macOS) is implied by the build files.

## What is, and is not, already deterministic

Measured, not assumed, and the two shipped artifacts do not agree.

**`bin/goo` is already deterministic on one machine.** Two clean builds with
ccache bypassed produced byte-identical output:

```
build A: 7b965be2143a2ea272d350e8870323a52e731b53ac80d3d56c14d1c83c7a2d29  bin/goo
build B: 7b965be2143a2ea272d350e8870323a52e731b53ac80d3d56c14d1c83c7a2d29  bin/goo
verdict: IDENTICAL
```

**`lib/libgoo_runtime.a` is NOT deterministic.** Two clean builds, with `lib/`
removed between them, differ by 206 bytes:

```
A: 9107158a19d8d485aae16df57e0b6fef74c69416c0d15becb26e08d0f6be40ad
B: 57ad4d0cb6813a5251d7ca40affd0e9eec7898461860842b5ef98703b263d018
```

All 102 members extract byte-identical, so every compiled object is
deterministic and the difference is entirely `ar` metadata. The recipe at
`Makefile:252` pipes an MRI script to `ar -M`, which stores each member's real
mtime, uid, gid and mode, and the two builds ran seconds apart.

This reframes the work in both directions. There is no nondeterminism to hunt
through the 71 source files, and environment pinning is most of the job — but
one real source change is required, below.

**The ccache bypass is load-bearing and any re-measurement must keep it.**
`Makefile:7` sets `CCACHE ?= $(shell command -v ccache)`, and ccache is
installed on the developer machine. With it active, build B replays cached
objects and prints IDENTICAL whatever the compiler does. The measurement would
then prove only that ccache works. Re-take with `make CCACHE= ...`.

Two further findings from the same audit:

- No source uses `__DATE__` or `__TIME__`.
- The checkout path is embedded 58 times, as `DW_AT_comp_dir`, with exactly one
  distinct value. No `-ffile-prefix-map` or `-fdebug-prefix-map` appears in the
  Makefile.

## Required source change: deterministic archives

`Makefile:252` becomes `ar -D -M`, and the following `ranlib $@` becomes
`ranlib -D $@`. `ar --help` documents `[D]` as "use zero for timestamps and
uids/gids".

Measured in the recipe's own shape, because the fix does not obviously transfer
from the `ar rcs` form to the MRI form and was tested in both:

| Invocation | Two archives, identical members, mtimes touched between |
|---|---|
| `ar rcs` | DIFFER |
| `ar rcsD` | IDENTICAL |
| `ar -M` + `ranlib` (the real recipe) | DIFFER |
| `ar -D -M` + `ranlib -D` | IDENTICAL |

## Decision

Approach A: a digest-pinned image, a fixed in-container build path, and a gate
that builds twice and compares hashes.

Rejected, with reasons, so that choosing one later needs no re-derivation:

- **Nix or Guix.** The state of the art for this property, and a larger change
  than the problem justifies. It would replace the toolchain story rather than
  pin the existing one.
- **Snapshot-mirror package pinning.** Addresses limit 2 below. Deferred, not
  dismissed.
- **Committing `parser.tab.c`.** Removes bison from the input set, and adds a
  generated file that can silently diverge from `parser.y`. The grammar
  tripwire would then guard a stale artifact.
- **`-ffile-prefix-map`.** Not needed for this approach, because a fixed `/src`
  makes `comp_dir` constant. Revisit only if a native build ever has to match a
  container build.

`ar` deterministic mode was on this rejected list when the design was written,
on the assumption that the archive behaved like `bin/goo`. Measuring it moved
it to a requirement. See "Required source change" above.

## The image

`Containerfile` at the repository root. Base `ubuntu:24.04`, pinned **by
digest, not by tag**, because CI already runs `runs-on: ubuntu-24.04` and this
reproduces the environment CI validates instead of adding a fourth toolchain.

Installed: the CI list exactly — `make gcc-14 bison llvm-dev clang
libblocksruntime-dev valgrind cmake libjson-c-dev libcurl4-openssl-dev
zlib1g-dev` — plus `python3`, which 12 of the 34 probe scripts need.

**`ccache` is deliberately absent.** It gives nothing to a one-shot build, and
its presence lets a determinism gate pass for the wrong reason. That is the
trap this design had to bypass to take its own baseline.

The image writes its resolved package versions to `/etc/goo-toolchain.txt`, so
a build can state what produced it.

## The build entry point

The repository is at `/src` inside the container, always. All 58 `comp_dir`
entries then read `/src` whatever the host path is, so path independence needs
no compiler flag.

`SOURCE_DATE_EPOCH` comes from `git log -1 --format=%ct`. **This is defensive.**
Nothing in the tree embeds a timestamp today.

`CCACHE=` is passed on the make command line, so the Makefile's `?=` resolves
empty even if a future image gains ccache.

Two modes, behind one wrapper `scripts/podman_build.sh`, so a person and the
gate run the same command:

| Mode | Source reaches the container by | Why |
|---|---|---|
| gate | copy | No host writes, so rootless-podman file ownership and SELinux labelling never arise |
| dev | mount, `--volume $PWD:/src:z --userns=keep-id` | Iteration without a rebuild. `:z` is required on a Fedora host. `keep-id` stops root-owned files landing in `build/` and `bin/` |

## The gate

`repro-build-probe`, in `verify`, **not** in `verify-core`.

Concretely: add the target to `VERIFY_ALL_DEPS`, and extend the filter that
already builds the core list. Today that filter names one target:

```make
VERIFY_CORE_DEPS := $(filter-out v2-bootstrap-pilot,$(VERIFY_ALL_DEPS))
```

It becomes a named set, because a second entry makes the intent worth stating:

```make
HEAVY_DEPS       := v2-bootstrap-pilot repro-build-probe
VERIFY_CORE_DEPS := $(filter-out $(HEAVY_DEPS),$(VERIFY_ALL_DEPS))
```

Three reasons. It costs two full builds, where `verify-core` takes about 3.5
minutes on the runner. It would make `verify-core` depend on podman, which
breaks the promise CLAUDE.md records for that target: "safe for pre-push on any
machine". Its failure mode is a toolchain question, not a correctness one.

It compares **`bin/goo` and `lib/libgoo_runtime.a`**. The baseline above
covered only `bin/goo`. The archive is where `ar` timestamp nondeterminism
hides, and it is a shipped artifact, so leaving it unmeasured assumes the thing
under test.

### Two build-system facts the gate must not get wrong

Both were found while reviewing this design, and either one silently defeats
the gate.

**`make clean` does not remove `lib/`.** The recipe is
`rm -rf $(BUILDDIR) $(BINDIR)`, so `lib/libgoo_runtime.a` survives it. A gate
built as "clean, build, hash — twice" would hash the same untouched archive
both times and report IDENTICAL. That is a false pass, and it is the exact
failure the teeth below exist to catch.

**`bin/goo` does not depend on the runtime archive.** The rule is
`$(COMPILER): $(GOO_OBJS) $(COMPILER_SRCS)`. The two artifacts are
independent, so building one proves nothing about the other, and each must be
built and compared on its own.

Together these decide how the source reaches the container: the gate copies
from **`git archive HEAD`**, never `cp -r .`. A recursive copy would carry
`build/`, `bin/` and `lib/` in, and make would then treat stale artifacts as up
to date — reproducing the same false pass inside the container.

### Teeth

The failure this gate is most likely to have is that both builds write the same
path and the script hashes one file twice. That passes forever.

`--self-test` therefore injects real nondeterminism — a source touched to
contain `__TIME__` — and asserts the probe reports DIFFER. It greps that the
injection landed before trusting the red result, because a mutation that fails
to apply reads exactly like a passing probe.

### Skips are loud

If podman is absent the probe prints a named `SKIPPED` line and the CI job
asserts it did not skip, following the pattern `tests.yml` already uses for the
valgrind and tsan gates.

## Limits, named rather than minimised

1. **Reproducible only inside this image.** A native gcc-16 build will differ.
   That is expected and is not a defect.
2. **Only as reproducible as the Ubuntu archive.** `apt-get install gcc-14`
   resolves against mirrors that remove old versions. Approach A does not fix
   this. The mitigation is snapshot-mirror pinning with exact package versions,
   deferred above.
3. **`v2-bootstrap-pilot` is out of scope.** It needs an opam CompCert switch,
   which this image does not carry.
4. **Nothing here detects a stale `parser.tab.c`.** Bison is pinned inside the
   container, so the generated parser is reproducible there. A native build with
   a different bison is not covered.
5. **THE MOST IMPORTANT LIMIT. The gate proves SAME-HOST, SAME-IMAGE
   determinism only.** It does not prove the cross-machine byte identity that
   the opening problem statement of this document describes. No expected hash
   is published anywhere, and no step compares a CI hash against a local one.
   `-j$(nproc)` also differs between machines, which is itself an untested
   source of difference.
6. **The gate builds `git archive HEAD`, never the working tree.** A modified
   working tree still returns PASS, about different source, with no warning.
7. **The image itself is not reproducible, and a PASS names no image.**
   `scripts/record_toolchain.sh:35` stamps a build date into the toolchain
   record. apt is not version-pinned. `scripts/repro_build_probe.sh` records
   no image ID in its PASS line. So a PASS carries no evidence of which
   toolchain produced it.
8. **CI runs the gate on a path filter, not on every change.** The filter now
   covers `src/**`, `scripts/record_toolchain.sh`, `scripts/podman_image_probe.sh`,
   and the workflow file itself, in addition to the four paths it started
   with. A change under `include/` or a script outside this list can still
   alter the build without starting the gate.
9. **The digest pin is enforced now, not merely documented.**
   `scripts/podman_image_probe.sh` reads the Containerfile's `FROM` line and
   fails the probe if it lacks `@sha256:`. Before this fix, nothing enforced
   the pin, and swapping the digest for a plain tag passed every gate on this
   branch.

## How to verify the claim

```bash
make CCACHE= -j"$(nproc)" bin/goo && sha256sum bin/goo   # twice, clean between
./scripts/repro_build_probe.sh                            # once implemented
./scripts/repro_build_probe.sh --self-test                # must go red
```
