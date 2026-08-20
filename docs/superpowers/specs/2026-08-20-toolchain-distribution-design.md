# Toolchain distribution: `goop` and the release pipeline

Status: **PARTLY IMPLEMENTED** 2026-08-20. Packaging, `make install`, and
`release-package-probe` have landed and are in `verify-core`. Still design
only: channel manifests, the R2 layout, the release workflow, signing, and
`goop` itself.
Sub-project A of `docs/adr/0006-toolchain-distribution-and-package-ecosystem.md`.

## The problem, measured before it was designed for

Nobody can install Goo. That is not an overstatement of a rough edge — it is
the measured state of the tree.

| Step a user needs | What exists today |
|---|---|
| Download a build | Nothing. No tarball, no hash, no signature |
| Install it | `make install` (Makefile:4858-4859), which is `cp $(COMPILER) /usr/local/bin/` |
| Know what they have | `GOO_VERSION "0.1.0"`, hardcoded at `src/compiler/goo.c:27`, read by no build step |
| Run more than one version | Nothing |
| Verify what they got | Nothing |

**`make install` does not produce a working installation.** It copies
`bin/goo` and neither `lib/libgoo_runtime.a` nor `goostd/`. After it runs,
`/usr/local/lib/goostd` and `/usr/local/lib/libgoo_runtime.a` do not exist, so
both of the compiler's lookups fall to their cwd fallbacks. The installed
compiler works only when it is invoked from the repository root.

## What already works, and what had never run

Verified against source before this design was written, because the whole
layout rests on it.

### The resolver already looks for the layout an installer would create

`goo_gooroot_dir()` (`src/package/import_resolver.c:40-66`):

| Tier | Path | Line |
|---|---|---|
| 1 | `$GOOROOT/goostd` | 42-43 |
| 2 | `<exe-dir>/../lib/goostd` — **the installed layout** | 49 |
| 3 | `<exe-dir>/../goostd` — the dev tree | 51-52 |
| 4 | `./goostd` | 58 |

`goo_runtime_archive_path()` (`src/codegen/codegen.c:1637-1657`) runs the same
ladder for the runtime archive: `$GOO_RUNTIME`, then
`<exe-dir>/../lib/libgoo_runtime.a`, then `lib/libgoo_runtime.a`.

`<exe-dir>` comes from `readlink("/proc/self/exe")` (`import_resolver.c:45-47`),
which **resolves symlinks**. So a symlink at `~/.goo/bin/goo` into a versioned
toolchain directory makes the compiler find that toolchain's standard library
*and* that toolchain's runtime archive, with no environment variable.

**One `<root>/{bin,lib}` layout satisfies both lookups. Multi-version
switching needs no compiler change.**

### Three qualifications, and the third is the reason this design has teeth

1. **Tiers 2 and 3 are inside `#ifdef __linux__`** — `import_resolver.c:44`,
   `codegen.c:1641`. On any other platform they do not compile in, and the
   lookup drops from the environment variable straight to the cwd fallback.
2. **The linker is external.** `bin/goo` invokes `"gcc"` on Linux
   (`codegen.c:1809`) with `execvp()` (line 1911), resolved through `PATH`.
   Code generation is in-process through the LLVM C API
   (`LLVMTargetMachineEmitToFile`, lines 1620 and 1773), so no `llc` or `opt`
   subprocess exists. **A toolchain is not self-contained.**
3. **Nothing in the tree had ever executed tier 2.** `lib/goostd` does not
   exist in this checkout and `goostd/` sits at the repository root, so the
   dev tree resolves through **tier 3**. No probe in `verify-core` builds a
   tier-2 layout.

So the gate below is not a regression net — **it is the first automated
execution.**
That made the layout the one assumption worth measuring before writing any
code, and it was measured. See the next section.

## Measured, 2026-08-20, before any code was written

A tier-2 tree was built by hand from the existing `bin/goo`,
`lib/libgoo_runtime.a` and the nine `GOOSTD_PKG_DIRS` packages, then run from
an unrelated working directory with `GOOROOT` and `GOO_RUNTIME` unset.

**Tier 2 works.** The claim this design rests on is confirmed rather than
assumed, and `strace` shows the linker receiving the tier-2 path literally:

```
"/…/tier2/root/bin/../lib/libgoo_runtime.a"
```

The cwd fallback cannot reach that file, so the path taken is unambiguous.

| Case | Exit | Meaning |
|---|---|---|
| Full tree, program imports `strings` | **0** | Both tier-2 lookups resolve |
| `lib/goostd` removed, same program | **1** | `Error: cannot resolve import "strings"` |
| `lib/libgoo_runtime.a` removed | **1** | Link fails, falls back to a cwd-relative path |
| Launched through a **symlink** into the tree | **0** | `/proc/self/exe` resolves it, so the `~/.goo/bin/goo` design holds |
| `lib/goostd` removed, **`fmt`-only program** | **0** | **A FALSE PASS** |

### The last row is a requirement, not a curiosity

A `fmt`-only hello-world **compiles and runs with `lib/goostd` entirely
absent**, because the eight shim packages
(`is_stdlib_shim_import()`, `src/compiler/goo.c:719-732`) are compiled into
`bin/goo` and never touch `goo_gooroot_dir()`.

**So the obvious probe — package a toolchain, run hello-world — passes
forever without ever testing the standard library lookup.** That is precisely
the false-pass shape the reproducible-build design had to bypass to take its
own baseline.

**The probe's test program must import a vendored `goostd` package.**
`strings` is the choice, because `strings.ToUpper` also exercises the runtime
archive at link time, so one program covers both lookups.

### Sizes, measured

| Artifact | Bytes |
|---|---|
| `bin/goo` | 2,065,376 |
| `lib/libgoo_runtime.a` | 1,465,986 |
| `goostd/` | about 260 KB |

About 3.7 MiB raw, so a gzip tarball lands near 1 MiB. Small enough that R2
storage and egress are rounding errors.

## Decision

A versioned toolchain directory, a static R2 layout, a signed release
pipeline, and one gate that proves an extracted tarball works with no
environment variable set.

Rejected alternatives are in ADR 0006 rather than repeated here.

## The layout

```
~/.goo/
  bin/goop                                     the installer
  bin/goo     -> ../toolchains/<active>/bin/goo
  toolchains/
    0.1.0-x86_64-unknown-linux-gnu/
      bin/goo
      lib/goostd/
      lib/libgoo_runtime.a
      lib/goo-toolchain.txt                    the twelve pinned versions
  settings.json                                the default toolchain
  downloads/                                   verified tarball cache
```

`settings.json`, not TOML — json-c is already linked and a TOML parser would
be a dependency serving no need.

Triples use the LLVM spelling (`x86_64-unknown-linux-gnu`). The compiler is
already LLVM-based, so the triple is meaningful in-tree, and it separates
`gnu` from `musl` where a `linux-x86_64` spelling cannot.

## What ships in a tarball, and what must not

`goostd/` holds **15 packages, and only 9 are standard library.** The other
six — `cpkg`, `fwdref`, `pkgcheck`, `kinds`, `shapes`, `mypkg` — are compiler
test fixtures, and each says so in its own header comment.

`scripts/check_stdlib_coverage.sh:296` already names the real nine in
`GOOSTD_PKG_DIRS`:

```
strings strconv utf8 bits lanes sort filepath io bytes
```

**The packaging step reads that variable. It does not copy `goostd/`
wholesale.** Shipping the fixtures would let user code `import "shapes"` and
have it resolve, which makes a test fixture part of the public surface.

## The wire format

```
dist/channel-goo-stable.json                 mutable, short TTL
dist/channel-goo-0.1.0.json                  immutable
dist/<date>/goo-<ver>-<triple>.tar.gz        immutable, cached forever
dist/<date>/goo-<ver>-<triple>.tar.gz.minisig
```

Everything under `dist/<date>/` is write-once. The channel manifests are the
only mutable objects in the system, which is what lets one cache rule cover
the rest.

```json
{
  "manifest_version": 1,
  "date": "2026-08-20",
  "version": "0.1.0",
  "git_commit": "b696f56...",
  "targets": {
    "x86_64-unknown-linux-gnu": {
      "available": true,
      "url": "https://dist.goolang.org/dist/2026-08-20/goo-0.1.0-x86_64-unknown-linux-gnu.tar.gz",
      "size": 1048576,
      "sha256": "....",
      "sig": "https://dist.goolang.org/dist/2026-08-20/goo-0.1.0-x86_64-unknown-linux-gnu.tar.gz.minisig"
    }
  },
  "build": {
    "image": "docker.io/library/ubuntu@sha256:....",
    "source_date_epoch": 1755000000,
    "toolchain": { "gcc-14": "...", "bison": "...", "llvm-dev": "..." }
  }
}
```

**The `build` block is not decoration. It closes repro Limit 7.** A published
artifact then names the image digest and the resolved package versions that
produced it, so a third party can rebuild in the same image and compare
hashes. `scripts/record_toolchain.sh` already collects exactly those twelve
values.

An unavailable target sets `"available": false` and carries no url, so `goop`
can say "no build for your platform, build from source" rather than fail on a
missing key.

## The release pipeline

`.github/workflows/release.yml`, triggered on a `v*` tag.

1. Build with `scripts/podman_build.sh gate` — **the same digest-pinned image
   the repro gate uses.** Not a second toolchain.
2. Run `make verify-core`. A release that cannot pass the core gate is not a
   release.
3. Package per `GOOSTD_PKG_DIRS`, sign with minisign, upload to R2.
4. Write the channel manifest last, so a half-uploaded release is never
   pointed at.

**This is the step that closes repro Limit 5.** That limit reads "No expected
hash is published anywhere." After this, CI's hash *is* the published expected
hash, and anyone with podman can reproduce it locally and compare. The gate's
property changes from same-host determinism to anyone-can-check determinism
without the gate itself changing.

## Version injection, and a reproducibility hazard

`GOO_VERSION` must come from the build rather than the literal at
`src/compiler/goo.c:27`.

**Do not derive it from `git describe` inside the build.** The repro probe
builds `git archive HEAD`, which carries no `.git`, so the version would
resolve differently inside and outside the container and break byte identity —
turning a green determinism gate red for a reason that has nothing to do with
determinism.

Pass it the way `SOURCE_DATE_EPOCH` is already passed.
`scripts/podman_build.sh` computes that on the host from
`git log -1 --format=%ct` and hands it in. `GOO_VERSION` follows the identical
path, and the literal becomes the fallback when the variable is unset.

## `goop` commands

Deliberately small. Every command here is needed to install and switch a
toolchain, and nothing else is included.

| Command | Does |
|---|---|
| `goop install <channel\|version>` | Fetch, verify signature and hash, extract, register |
| `goop default <toolchain>` | Repoint the `~/.goo/bin/goo` symlink |
| `goop list` | Installed toolchains, marking the default |
| `goop uninstall <toolchain>` | Remove one toolchain directory |
| `goop update` | Re-read the channel manifest, install if newer |
| `goop which` | Print the resolved `goo` path and version |

`goop install` runs a **preflight check for a C toolchain on `PATH`** and
refuses with a named message if none is found. Per Qualification 2 the linker
is external, so without this the failure surfaces much later as an obscure
`execvp` error from inside codegen.

Not in this cut, and named so their absence is a decision: no directory-local
toolchain override (it needs a re-exec shim, not a symlink), no `goop self
update`, no proxy or mirror configuration, no nightly channel.

## The gate, with teeth

**Corrected on implementation, 2026-08-20.** This section originally put
`release-package-probe` in `verify` beside the podman gates, reasoning that "it
costs a full build and it would make `verify-core` depend on podman". **Both
halves are wrong.** The probe packages artifacts `verify-core` has already
built and never invokes podman. Measured end to end: **4.8 seconds.**

It is therefore in **`verify-core`**, which is where it belongs — the tier-2
path it exercises should be checked on every pre-push, not only on a machine
with a container runtime.

### What it asserts

Build a tarball, extract it to a temporary directory, then:

1. **unset `GOOROOT` and `GOO_RUNTIME`**,
2. **`cd` to an unrelated directory**,
3. assert `<tmp>/bin/goo` compiles **and runs** a program that
   **imports `strings`**, never a `fmt`-only hello-world.

One assertion covers both tier-2 lookups. Step 3's import is load-bearing and
is measured above: a `fmt`-only program passes with `lib/goostd` deleted.

**Step 2 is the teeth of the teeth.** A probe that ran from the repository
root would pass through tier 4 while proving nothing about tier 2. That is the
same false-pass shape the repro design caught in itself, where `make clean`
left `lib/` in place and a naive gate would have hashed one untouched archive
twice.

### Self-test

`--self-test` runs **two** injections, not one:

| Injection | Must report |
|---|---|
| Remove `lib/goostd` from the extracted tree | FAIL |
| Remove `lib/libgoo_runtime.a` from the extracted tree | FAIL |

Two, because the stdlib lookup passing does not prove the runtime-archive
lookup runs at all — a probe that only ever removed `goostd` would stay green
forever if `goo_runtime_archive_path()` silently resolved elsewhere.

Each injection greps that the removal landed before trusting the red result,
following `scripts/repro_build_probe.sh`. A mutation that fails to apply reads
exactly like a passing probe.

### Skips are loud

If podman is absent the probe prints a named `SKIPPED` line, and the CI job
asserts it did not skip — the pattern `tests.yml` already uses for the
valgrind and tsan gates.

## Limits, named rather than minimised

1. **`x86_64-unknown-linux-gnu` only.** Adding a platform is a **compiler
   change**, not a CI change, because the exe-directory lookup is inside
   `#ifdef __linux__` at two sites. Extract one `goo_exe_dir()` helper first,
   or the platform ladder gets written twice.
2. **The pipeline's first real run is its first test.** It cannot be exercised
   without R2 credentials and a tag push. `release-package-probe` covers
   packaging and the layout, and covers neither upload nor signing.
3. ~~**`.tar.gz` determinism is unproven.**~~ **CLOSED 2026-08-20.**
   `tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@$EPOCH` piped
   through `gzip -n` is byte-identical across runs, and
   `release-package-probe` asserts it by packaging twice and comparing.
   Negative control run first: a naive `tar -czf` DIFFERS under the same
   conditions, so the assertion has teeth rather than being vacuously true.
4. **A signed artifact is not a reproducible one.** The signature proves who
   published; it does not prove the bytes match the source. The `build` block
   lets a third party check the second claim, and nothing forces them to.
5. **Key rotation means shipping a new `goop`.** The public key is compiled
   in. There is no transparency log, so a stolen private key is undetectable
   until someone notices a bad artifact.
6. **`goop` is unverified by anything it ships with.** `goop-init.sh` fetches
   `goop` over TLS and checks a hash the same script carries. Trust bottoms
   out at the TLS connection to `dist.goolang.org`. This is the same bootstrap
   problem rustup has, and naming it is the only honest treatment.
7. **The toolchain still needs a system C compiler.** `goop install`
   preflights for it, so the failure is early and named rather than absent.
8. **Nothing in this design deletes the old attempt.** `attic/src/package/`
   and `tools/pkg/main.c` stay where they are, compiled by nothing.

## How to verify the claim

```bash
# once implemented
make release-package-probe            # must PASS
make release-package-probe SELFTEST=1 # must go red, twice, for two reasons
```

The layout claim itself was already checked by hand on 2026-08-20, and the
results are in "Measured" above. Re-take it like this — the three controls
matter as much as the positive case:

```bash
T=$(mktemp -d); mkdir -p "$T/root/bin" "$T/root/lib/goostd" "$T/work"
cp bin/goo "$T/root/bin/"; cp lib/libgoo_runtime.a "$T/root/lib/"
for d in strings strconv utf8 bits lanes sort filepath io bytes; do
  cp -r "goostd/$d" "$T/root/lib/goostd/"; done

cat > "$T/work/hello.goo" <<'EOF'
package main
import ("fmt"; "strings")
func main() { fmt.Println(strings.ToUpper("tier two works")) }
EOF

cd "$T/work"
env -u GOOROOT -u GOO_RUNTIME "$T/root/bin/goo" run hello.goo   # expect 0
mv "$T/root/lib/goostd" "$T/hidden"
env -u GOOROOT -u GOO_RUNTIME "$T/root/bin/goo" run hello.goo   # expect nonzero
```

Three rules for anyone re-taking it:

1. **`cd` out of the repository first.** From the repository root the cwd
   fallback answers and the measurement proves nothing.
2. **Unset both variables.** `GOOROOT` alone leaves the runtime archive
   lookup untested.
3. **Import a vendored package.** A `fmt`-only program passes with
   `lib/goostd` deleted, which is measured above and is the trap this whole
   section exists to prevent.
