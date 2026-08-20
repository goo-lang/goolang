# Package management: `goo.mod`, `goo.lock`, and a module cache

Status: design, approved 2026-08-20. Implementation not started.
Sub-project B of `docs/adr/0006-toolchain-distribution-and-package-ecosystem.md`.

Sub-project A must land first. A lock file that cannot pin the compiler that
reads it is a broken lock file.

## What exists today, measured

Every row was checked against source for this document. The greps were run
with a positive control first, because an earlier claim in ADR 0006 came from
a grep that had died on a shell error and printed nothing.

| Piece | State |
|---|---|
| `goo.mod` | **Written, never read** — and TWO already exist in the tree. See the blocking hazard below |
| Import resolution | `resolve_import()` (`src/package/import_resolver.c:248-292`): `./name` against the source dir only; a bare name against GOOROOT, then the source dir |
| Nested paths | Pass through unchanged. `normalize_import_path()` maps three Go stdlib spellings and returns everything else as written |
| Package identity | The graph keys on the FULL path — `PkgEntry.import_path`, commented "registry key" (`src/compiler/goo.c:589`) |
| Renamed imports | **Work, since PR #307.** The alias binds and the original name is unbound |
| Shim packages | Eight names short-circuit before resolution (`is_stdlib_shim_import()`, `goo.c:719-740`) |
| Version resolution | Does not exist |
| Fetch, lock file, module cache | Do not exist |

So the pieces missing are: a manifest reader, a version algorithm, a fetch
path, a cache, and one resolver tier.

## BLOCKING HAZARD: `examples/goo.mod` already exists, and all 495 goldens sit beside it

Two `goo.mod` files are in the tree today, in two different and incompatible
formats.

- `demo-project/goo.mod` is what `tools/project_wizard/main.c:208-218` writes:
  `module <name>`, `goo 1.0`, `require ( … )`. Line-based, Go-shaped, and the
  format this document adopts.
- **`examples/goo.mod` is 190 lines of a brace-based format that nothing has
  ever parsed.** It declares `intelligence: { predict_compatibility, semantic
  caching }`, `security: { sandbox_analysis, supply_chain_analysis: "deep" }`,
  and registries on `ipfs://` and `arweave://`. It is the same fabricated
  vocabulary as the attic headers.

**`examples/` holds all 495 golden fixtures.** So the moment a manifest reader
walks up from an entry file to find a module root, every golden fixture
becomes a file inside a module whose manifest is unparseable — and 495
fixtures fail at once, for a reason that has nothing to do with what they
test.

**Deleting `examples/goo.mod` is therefore a PREREQUISITE task of this
sub-project, not a cleanup.** It must land, on its own, before the reader
exists. Nothing reads the file, so removing it is inert today and impossible
to do safely later.

Check it has not come back before believing any manifest work:

```bash
find . -name goo.mod -not -path './attic/*'   # expect demo-project/goo.mod only
```

## Do not reuse the attic headers

ADR 0006 says to "reuse the header contracts where they are sound". Having now
read them, that advice is too generous and this document overrides it.

`include/package/goo_mod.h` declares `IntelligenceConfig` with
`predict_compatibility`, `suggest_alternatives` and `semantic_caching`, and a
`SecurityConfig` with `sandbox_analysis`. That is the vocabulary of the tools
P5.5 quarantined for fabricating their results.

`include/package/package_manager.h` is more sober and still wrong for this
design, in two specific ways:

- It declares `check_version_conflicts()` and `ResolveResult`. **MVS has no
  version conflicts.** A solver-shaped API would import the model this ADR
  rejected.
- `LockFile` carries `time_t generated_at`. **A timestamp in a lock file
  defeats byte-comparison**, in a repository that has just spent a branch
  making its artifacts reproducible. Go's `go.sum` carries no timestamp, for
  this reason.

Write the contracts fresh. The attic headers stay unbuilt, and
`include/package/ipfs_package.h` and its siblings should follow their `.c`
files into `attic/` rather than sit in `include/` implying a shipped feature.

## Decisions taken in ADR 0006

- **Version resolution is MVS**, Go's minimal version selection.
- **The short-name collision uses rename imports**, which now work.

### MVS, stated precisely

For each module in the graph, the selected version is the **greatest of the
minimum versions that anything requires**. Equivalently: the oldest version
that satisfies every requirement.

Three properties follow, and they are the reason for the choice:

- **No solver and no backtracking.** Resolution is one walk over the
  requirement graph, taking a maximum per module.
- **Deterministic from the manifests alone.** `goo.lock` therefore *records*
  the selection rather than *causing* it. Deleting the lock file changes
  nothing about which versions are chosen — it only removes the checksums.
- **Adding a dependency cannot silently upgrade an unrelated one.** An upgrade
  happens when a `require` line changes, never as a side effect of a build.

## `goo.mod`

The format the wizard already writes, with `require` given meaning:

```
module example.com/myapp

goo 1.0

require (
    example.com/text v1.2.0
    example.com/json v0.4.1
)
```

- `module` declares this module's own path, and is the prefix under which its
  packages are importable.
- `goo` is the language version. Sub-project A makes a toolchain version
  meaningful, which is why B waits on it.
- `require` names a module path and a **minimum** version. Under MVS there are
  no ranges to express, so there is no range syntax to design.

**Module root discovery:** walk up from the entry file's directory to the
first `goo.mod`. No `goo.mod` means today's behaviour exactly — GOOROOT and
source-dir resolution, no fetch, no cache. **A tree with no manifest must
compile exactly as it does now.**

## `goo.lock`

```
example.com/json v0.4.1 h1:<sha256-base64>
example.com/text v1.2.0 h1:<sha256-base64>
```

Sorted by module path, one line each, **no timestamp and no file-level
checksum**. The file is then byte-stable for a given requirement graph, which
is what lets it be diffed and committed. The hash covers the package tarball,
which under ADR 0006 is source only — there is no compiled artifact to hash,
because Goo has no separate compilation.

## The module cache and the new resolver tier

```
~/.goo/pkg/example.com/text@v1.2.0/
```

`resolve_import()` gains **one** tier, and its position is the whole design:

| Order | Tier | Why here |
|---|---|---|
| 1 | `$GOOROOT/goostd/<path>` | Unchanged |
| 2 | **module cache** | **New** |
| 3 | `<source_dir>/<path>` | Unchanged, stays last |

The cache goes **after GOOROOT** so a fetched package can never shadow a
standard library package, and **before the source directory** so a
dependency is preferred over an accidentally same-named local directory.

The eight shim names never reach the resolver at all —
`walk_program_imports()` skips them before `walk_import()` is called — so
`fmt` cannot be shadowed by construction. **A `require` naming a shim package
is a hard error at manifest load**, rather than a silently ignored line.

## Fetch

`goo get` and an implicit fetch during build both do the same thing:

1. Resolve versions by MVS over the requirement graph.
2. For each selected module absent from the cache, GET
   `<registry>/mod/<path>/@v/<version>.tar.gz` — a static object, per ADR
   0006's no-compute-on-the-read-path rule.
3. Verify the SHA-256 against `goo.lock`. **A mismatch is a hard failure**,
   never a warning and never a silent re-fetch.
4. Extract into the cache. Cache entries are immutable once written.

Absent a `goo.lock` entry, record the hash on first fetch — that is
trust-on-first-use, and the spec says so plainly rather than implying the
checksum proves provenance. A checksum database in the shape of Go's `sumdb`
is named in ADR 0006 as open, and would be what upgrades this.

## The gate, with teeth

`package-fetch-probe`, in `verify-core` — it needs no podman and no network.

The fixture registry is a **local directory of `.tar.gz` files** served by
`file://`, so the probe tests resolution, verification and caching without a
server. Three assertions:

1. A module with one dependency builds, and the dependency resolves from the
   cache rather than from the source directory.
2. MVS picks the expected version where two requirements name different
   minimums for the same module. **The fixture must require a NON-lowest
   version somewhere**, or the assertion passes for any algorithm that picks
   the only candidate.
3. A corrupted tarball, whose hash no longer matches `goo.lock`, **fails the
   build**.

`--self-test` mutates one byte of a cached tarball and requires assertion 3 to
go red. It greps that the mutation landed **in the file the build reads**, not
merely on disk somewhere — the surface mismatch that made
`repro_build_probe.sh --self-test` unable to run on CI.

**Assertion 1 must import a package that is genuinely only in the cache.** A
fixture whose dependency also exists under the source directory would pass
through tier 3 and prove nothing about tier 2 — the same false pass shape that
a `fmt`-only program produces against `goo_gooroot_dir()`.

## Limits, named rather than minimised

1. **No per-file import scope.** Markers seed into one global scope, so where
   a diamond reaches one package both aliased and plain, the first alias wins
   (`walk_program_imports`' `top_level` flag). Rename imports are the
   workaround; per-file scope is the fix, and it is not in this cut.
2. **No `replace`, no `exclude`, no vendoring, no workspaces.** Each is a real
   Go feature and none is needed to build a dependency graph.
3. **Trust on first use.** The lock file proves the bytes have not changed
   since you first saw them. It does not prove they are the bytes the author
   published.
4. **No `goo get -u` semantics beyond "raise this require line".** MVS gives
   up implicit upgrades deliberately, so tooling to find available upgrades is
   a separate piece of work.
5. **A module is fetched whole.** There is no per-package granularity within a
   module, matching Go.
6. **The `goo` directive is not enforced.** Recording a language version is
   useful before anything can act on it, and acting on it needs `goop`.
7. **Nothing here deletes `attic/src/package/` or the orphaned headers in
   `include/package/`.** They stay unbuilt, and stay misleading.

## How to verify the claim

```bash
make package-fetch-probe              # must PASS
make package-fetch-probe SELFTEST=1   # must go red on the corrupted tarball
make verify-core                      # the probe is inside this
```

Before writing any code, run the prerequisite check, because the first attempt
at this premise got it wrong:

```bash
find . -name goo.mod -not -path './attic/*'   # must list demo-project only
make test-golden                              # 495, unaffected by manifest work
```

The naive premise check — "a tree with no `goo.mod` behaves as today" — is
true and useless here, because `examples/` **has** one. That is what makes the
hazard above blocking rather than cosmetic, and it was found only by running
`find` instead of assuming the tree was clean.

(Unrelated, and noted so the next person does not chase it:
`examples/hello.goo` does not compile today — it leaves an error union
unhandled. It carries no `.expected.txt`, so it is not one of the 495 and no
gate covers it.)
