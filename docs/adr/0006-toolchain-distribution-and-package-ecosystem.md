# ADR 0006 — toolchain distribution before package management, and a read path with no compute

Date: 2026-08-20
Status: **accepted** (2026-08-20). Design only, no code in this pass.
Sub-project A is specified in
`docs/superpowers/specs/2026-08-20-toolchain-distribution-design.md`.

**Prerequisite, not yet on `main`.** This document cites `Containerfile`,
`scripts/podman_build.sh`, `scripts/record_toolchain.sh`,
`scripts/repro_build_probe.sh` and
`docs/superpowers/specs/2026-08-19-reproducible-builds-podman-design.md`.
Every one of them lives on the `docs/repro-builds-podman` branch and lands
with it. **Sub-project A cannot start until that branch merges.**

## Context

ADR 0003 grades Goo on eight axes against Rust. Seven carry a "win" or a
"parity target". One carries **"Concede openly"** — *Ecosystem: 17 files,
3,533 lines*. It is the only conceded axis, and the only one with neither an
owner nor a plan.

Three facts decide that this is the moment to write one.

### 1. The last attempt was deleted, and the roadmap says why

`docs/2026-07-08-v1-roadmap.md:287-289` records that P5.5 removed the `gmod`
package-manager CLI because it did not compile, and that the IPFS, registry
and p2p modules under `src/package` "are compiled by nothing". That code now
sits in `attic/src/package/`. The Makefile's own note on the P5.5 quarantine
is blunter: each quarantined tool "fabricated its results".

The roadmap's instruction for Phase 8 is literally **"design fresh"**. This
ADR is that design, and it does not revive the attic.

Three artefacts of the deleted attempt survive in the live tree and are
relevant, because they fix a name rather than a design:

- `tools/project_wizard/main.c:204-218` **writes** a `goo.mod` file containing
  `module <name>` and `goo 1.0`. Nothing reads it back. The filename is
  settled by a shipping tool; only the reader is missing.
- `include/package/goo_mod.h`, `include/package/module.h` and
  `include/package/package_manager.h` declare the shape — `fetch_package()`,
  and a `LockFile` with create, load and save. No `.c` file in the live tree
  implements them.
- `tools/pkg/main.c` carries a `download_package()` and a default
  `registry_url` of `https://registry.goolang.org`. The Makefile does not
  build it.

Header contracts may be reused where they are sound. **The implementations
must not be.**

### 2. There is no release process at all

`make install` (Makefile:4858-4859) is one line: `cp $(COMPILER)
/usr/local/bin/`. `GOO_VERSION "0.1.0"` is hardcoded at
`src/compiler/goo.c:27`, read only by the `version` subcommand at line 184,
and consumed by no build step or script. Nothing in the repository produces a
tarball, a hash, or a signature.

A language nobody can install has no ecosystem to concede. Distribution was
never designed, rather than designed and deferred.

### 3. The reproducible-build branch built the substrate and stops one step short

`docs/repro-builds-podman` makes two builds of one commit, in a digest-pinned
image, yield byte-identical `bin/goo` and `lib/libgoo_runtime.a`. Its own
design document then names the gap:

> **Limit 5, THE MOST IMPORTANT LIMIT.** The gate proves SAME-HOST,
> SAME-IMAGE determinism only. No expected hash is published anywhere.

> **Limit 7.** `scripts/repro_build_probe.sh` records no image ID in its PASS
> line. So a PASS carries no evidence of which toolchain produced it.

A release pipeline is exactly the consumer that closes both. It publishes the
hash and it names the image. The determinism property then changes from
"same-host" to "anyone can check", without changing the gate.

## What "match cargo" can mean here, and what it cannot

**Goo has no separate compilation.** ADR 0003 records that imported-package
functions moved to *internal* linkage precisely because "Goo compiles a whole
program into one module and has no separate compilation".

Cargo's hardest machinery exists to serve separate compilation: rlibs,
metadata hashes, per-crate artifact caches, cross-crate ABI stability. **None
of it applies to Goo.** Two consequences follow, and they shape every
sub-project below.

- **A Goo dependency is source, not a compiled artifact.** A package is a
  tarball of `.goo` and `.go` files. A lock file pins a source checksum and
  nothing else. There is no ABI to break, and no build-metadata hash to
  compute.
- **There is no per-package build cache, and one must not be added on
  intuition.** ADR 0003 withdrew that proposal in its own work order:
  *"The cache proposal is withdrawn: it addressed a cost that was never
  there."* Hello-world compiles in 0.05 s, against rustc's 0.07 s and Go's
  0.11 s. A future build cache needs a measurement first.

So this ADR matches cargo's **user experience** while deleting roughly half
its architecture. That is a consequence of Goo's compilation model, and it is
recorded as a saving rather than a gap.

## Decision

**Three sub-projects, designed together and built in a fixed order.**

| | Sub-project | Compiler change | Status |
|---|---|---|---|
| **A** | Toolchain distribution: `goop` plus the release pipeline | **None** | Specified now |
| **B** | Package manager: `goo mod` and `goo get` | **Substantial** | Design only |
| **C** | Registry: static index on R2, one publish Worker | None | Design only |

**Order rationale.** A needs no compiler change and consumes the
reproducible-build work while it is fresh. B depends on A, because a lock file
that cannot pin a compiler version is a broken lock file. C delivers nothing
to a user until B can consume it.

### Decision 1 — two binaries: `goo` and `goop`

`goo` keeps the compiler, `build`, `run` and `test`. `goop` takes the `rustup`
role: install, verify, list and switch compiler versions.

The split exists for one reason: **a tool must not update the artifact it is
running from.** `goop` is small, static and rarely changed, so it can replace
a `goo` that is not executing. Folding toolchain management into `goo` would
make self-update a special case in the binary being replaced.

### Decision 2 — the layout is the one the resolver already looks for

`goo_gooroot_dir()` (`src/package/import_resolver.c:40-66`) probes, in order:
`$GOOROOT`, then `<exe-dir>/../lib/goostd` at line 49, then
`<exe-dir>/../goostd` at lines 51-52, then `./goostd` at line 58.
`goo_runtime_archive_path()` (`src/codegen/codegen.c:1637-1657`) runs the same
three-tier ladder for `lib/libgoo_runtime.a`.

`<exe-dir>` comes from `readlink("/proc/self/exe")`, which **resolves
symlinks**. A symlink at `~/.goo/bin/goo` pointing into
`~/.goo/toolchains/<version>/bin/goo` therefore makes the compiler find *that
toolchain's* standard library and *that toolchain's* runtime archive, with no
environment variable and no wrapper.

**Multi-version toolchain switching, normally the hard part, already works.**
A single `<root>/{bin,lib}` layout satisfies both lookups. This ADR adopts it
rather than inventing one.

Three qualifications are load-bearing and are recorded so they are not
rediscovered.

1. **Tiers 2 and 3 are inside `#ifdef __linux__`** (`import_resolver.c:44`,
   `codegen.c:1641`). On macOS or Windows they do not compile in, and the
   lookup drops from `$GOOROOT` straight to the cwd fallback. **Adding a
   non-Linux platform is a compiler change, not a CI change.**
2. **Nothing in the tree had ever executed tier 2.** `lib/goostd` does not
   exist in the checkout and `goostd/` sits at the repository root, so the dev
   tree resolves through tier 3, and no probe constructs a tier-2 layout.
   **Measured by hand on 2026-08-20 before this ADR was accepted: tier 2
   works, including through a symlink.** `strace` shows the linker receiving
   `<exe-dir>/../lib/libgoo_runtime.a` literally. The measurement also found a
   false-pass trap that shapes the gate — see the spec.
3. **`make install` produces a broken installation today.** It copies neither
   `lib/libgoo_runtime.a` nor `goostd/`, so after it runs both lookups fall to
   their cwd fallbacks and `goo` works only from the repository root.

### Decision 3 — no compute on the read path

Install, resolve and download are **plain GETs against R2 behind a custom
domain and a cache rule**. A Worker exists only for publish, and D1 holds only
metadata that the critical path never reads.

```
      install / resolve / download                    publish
                 |                                       |
         dist.goolang.org                        publish.goolang.org
        (R2 + Cache Rules)                            (Worker)
                 |                                       | auth, validate
                 +------------- R2 bucket <--------------+
                                goo-dist                 |
                                                         v
                                                        D1
                                              (owners, yanks, counts)
```

Registry outages are overwhelmingly failures of the dynamic side. If every
read is a cached GET, a broken publish Worker cannot stop anyone from
building. **The property is free at this size and expensive to retrofit**, so
it is recorded as a constraint rather than a preference.

Two things fall out of the layout instead of needing mechanism:

- Everything under `dist/<date>/` is **write-once**, so it caches forever. The
  channel manifests are the only mutable objects.
- **A yank is a manifest edit, never a delete.** A lock file pinning a yanked
  version keeps building, which is the correct behaviour.

**Sub-project A needs no Worker and no D1 at all** — a bucket, a domain, a
cache rule, and CI credentials. Compute enters the system only when public
publishing does.

### Decision 4 — minisign, with the public key compiled into `goop`

Ed25519, offline-verifiable, no network at verify time. The private key lives
in CI secrets.

### Decision 5 — design every triple, populate one

Triples use the LLVM spelling (`x86_64-unknown-linux-gnu`), not
`linux-x86_64`. The compiler is already LLVM-based, so the triple is
meaningful in-tree, and it separates `gnu` from `musl`, which the short
spelling cannot.

Only `x86_64-unknown-linux-gnu` is published. That is what CI builds and what
the podman image can prove. Everything else builds from source, and `goop`
says so plainly rather than failing obscurely.

### Decision 6 — `goop` is C23 plus embedded Lua, and Lua never touches the wire

| Layer | Language |
|---|---|
| `goop-init.sh`, the `curl \| sh` bootstrap | POSIX shell |
| HTTPS, gzip, tar, Ed25519, filesystem | C23 |
| Channel selection, install policy, subcommand logic, error text | Lua |

**The choice adds no new build dependency.** The Containerfile already
installs `libcurl4-openssl-dev` (HTTPS), `libjson-c-dev` (manifests) and
`zlib1g-dev` (decompression). Lua vendors the way NNG already does — a pinned
tarball with a `sha256sum` check, the pattern at Makefile:144-154.

Two simplifications follow, and both are decisions:

- **Artifacts are `.tar.gz`, not `.tar.zst`.** zstd is not in the image, zlib
  is. A toolchain is 3.7 MiB, so the difference is well under a megabyte — not
  worth a new dependency inside a reproducibility claim.
- **Channel manifests are JSON, not TOML.** json-c is already linked.

**The security rule: Lua is `goop`'s inside, never its wire format.** A
manifest arrives from the network, so a Lua manifest would be remote code
execution by construction, in the one tool whose whole job is to establish
trust. Manifests are therefore JSON, data only. The Lua state is built without
`io`, `os`, `package`, `debug`, `load` and `dofile`, and its scripts are
compiled into the binary rather than read from disk.

**What Lua does not buy, stated plainly:** it reduces orchestration code,
where most installer bugs live. It does **not** reduce untrusted-parsing risk.
tar, gzip and Ed25519 stay in C either way, and that risk is answered by
keeping those parsers small and under the existing ASan, valgrind and MISRA
gates.

## What sub-project B will cost

Recorded here so this ADR does not imply the package manager is as cheap as
`goop`. Sub-project A needs no compiler change. **B needs four.**

1. `resolve_import()` (`import_resolver.c:248-292`) has **no tier for a
   fetched module cache**. One must be added, after GOOROOT and before the
   source-directory fallback.
2. `normalize_import_path()` (lines 142-150) flattens nested import paths
   through **three hardcoded cases** — `unicode/utf8`, `math/bits`,
   `path/filepath`. Registry names look like `github.com/user/pkg`. This
   function is incompatible with them as written.
3. Nothing reads `goo.mod`. A parser and a resolver are new.
4. Eight package names resolve through a hardcoded C shim
   (`is_stdlib_shim_import()`, `src/compiler/goo.c:719-732`). A fetched
   package must never be able to shadow one.

## Why not the alternatives

**Package manager first — rejected.** It is what "match cargo" suggests, and
it is the wrong order. It needs four compiler changes and a place to fetch
from, and it produces a lock file that cannot pin the compiler that reads it.

**Revive `attic/src/package/` — rejected.** The IPFS, p2p and hybrid-registry
code was quarantined for fabricating results, and a content-addressed p2p
registry answers a distribution problem this project does not have. Reuse the
header contracts, not the code.

**One binary, `goo toolchain install` — rejected, and it is the closest
call.** It matches the Go-flavoured identity and needs no second release
train. Rejected because self-update then modifies the running binary.

**A Worker in front of downloads — rejected.** It buys download counts and
rate limiting, and it puts compute and a failure point on the path that must
work. Counts can come from R2 access logs.

**Rust or Go for `goop` — rejected, and worth revisiting.** Both are safer for
parsing hostile archives, in much less code. Rejected because each adds a
second toolchain to CI **and to the digest-pinned reproducible-build image**,
in a project that today needs only a C compiler. Go's trivial
cross-compilation is a real loss against the deferred platform matrix, and is
the strongest argument for reopening this.

**Sigstore cosign instead of minisign — rejected for now.** Keyless signing
with a transparency log is the stronger supply-chain story and pairs naturally
with reproducible builds. Rejected because verification wants network access
and it pulls a large dependency into a tool meant to be tiny. Revisit when
publishing goes public.

## Consequences

### Positive

- The conceded axis gets an owner and an order.
- Repro Limits 5 and 7 close as a side effect: CI's hash becomes the published
  expected hash, and a manifest entry names the image digest and the twelve
  package versions that produced it.
- `make install`'s broken installation is fixed on the way past.
- Half of cargo's architecture is deleted rather than ported, with a recorded
  reason.
- The read path costs approximately nothing and stays up when publish does not.

### Negative, named rather than minimised

- **Tier 2 has no automated test.** It was confirmed by hand once, on one
  machine, on 2026-08-20. `release-package-probe` is its first *repeatable*
  execution, which is why that probe is load-bearing rather than
  nice-to-have. The hand check also proved the obvious probe would be a false
  pass: a `fmt`-only hello-world compiles and runs with `lib/goostd` deleted,
  because the eight shim packages (`src/compiler/goo.c:719-732`) never call
  `goo_gooroot_dir()`. The probe's program must import a vendored package.
- **A Goo toolchain is not self-contained.** `bin/goo` shells out to the
  system linker — `"gcc"` on Linux (`codegen.c:1809`), `"clang"` on macOS
  (line 1807) — through `execvp()` at line 1911, resolved via `PATH`. So
  `goop install` yields a compiler that fails at link time unless a C
  toolchain is present. Rust has the same dependency on a system `cc`, so this
  is normal, but it must be a named prerequisite and a preflight check in
  `goop install`, not a confusing linker error later.
- **Linux only, and non-Linux is a compiler change**, per Qualification 1.
- **The release pipeline's first real run is its first test.** It cannot be
  exercised without R2 credentials and a tag push.
- **`.tar.gz` creation is a new input to a reproducibility claim** and is not
  yet proven deterministic. gzip stamps an mtime by default and tar records
  mtime, uid, gid and member order.
- **Signing introduces a private key**, which is a new class of incident for
  this project, with no transparency log to detect misuse.

## Three defects found while writing this ADR

Recorded so they are not rediscovered.

1. **`make install` is broken** (Makefile:4858-4859). It copies only
   `bin/goo`.
2. **The `/proc/self/exe` block is duplicated** at `import_resolver.c:44-52`
   and `codegen.c:1641-1653`. Extracting one `goo_exe_dir()` helper is the
   named prerequisite for any non-Linux platform — otherwise the platform
   ladder gets written twice.
3. **Six compiler test fixtures live inside `goostd/`** — `cpkg`, `fwdref`,
   `pkgcheck`, `kinds`, `shapes`, `mypkg` — beside the nine real standard
   library packages that `scripts/check_stdlib_coverage.sh:296` names in
   `GOOSTD_PKG_DIRS`. They are importable as if they were standard library.
   The packaging step must read that existing list rather than copy `goostd/`
   wholesale.

## What this changes elsewhere

- **`docs/01-VISION.md:178-181` is corrected.** "Rust Crate Compatibility —
  import existing Rust crates directly" is dropped. It contradicts ADR 0003's
  ecosystem concession, and no design in this ADR accommodates it.
- **`docs/2026-07-08-v1-roadmap.md:287-289`** now points at this ADR for the
  "design fresh" instruction.

## Open, and deliberately not decided here

- Whether `goop` ever gains a directory-local toolchain override, in the shape
  of Rust's `rust-toolchain.toml`. It needs a re-exec shim rather than a
  symlink, and sub-project A ships neither.
- Whether the registry authenticates publishers with GitHub OIDC or with
  issued tokens. It is sub-project C's first question.
- Whether a source checksum database, in the shape of Go's sumdb, is worth its
  cost for a registry whose per-version objects are already immutable.
- Whether binary size stays a claim once a toolchain tarball exists to measure
  it against, which ADR 0003 left open.
