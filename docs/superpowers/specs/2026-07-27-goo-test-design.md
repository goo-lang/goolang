# `goo test` — design

**Date:** 2026-07-27
**Status:** approved, not yet implemented.
**Unblocked by:** PR #226 (multi-file packages). A `_test` file is by definition a
second file of its package, so `goo test` was not expressible before that landed.

## Context

Goo has no test runner. The compiler's own corpus is Makefile probes plus golden
fixtures — that is how the COMPILER is tested and it is not changing here. This
document is about how a USER tests Goo code they wrote.

The goal is narrow and specific: **run a real Go test file unmodified.** That is
what makes existing Go test corpora usable, and it is why the design keeps Go's
names, signatures and output format rather than inventing a lighter shape.

## Scope

**In:** `goo test [dir]` (default `.`), one package per invocation. Discovery of
`func TestXxx(t *testing.T)` in `*_test.go` / `*_test.goo` files declaring the
SAME package as the code under test. Go's per-test and summary output. Exit 1 on
any failure. `testing.T` with `Error`, `Errorf`, `Fatal`, `Fatalf`, `Log`,
`Logf`, `Fail`, `FailNow`, `Helper`.

**Out, deliberately, each independently addable later:** subtests (`t.Run`),
benchmarks, `-run` filtering, `t.Parallel`, `./...` recursion, external
`package mypkg_test` files, `TestMain`, example tests, coverage.

## Verified constraints

Each of these was measured against `bin/goo` before the design was fixed, not
assumed. They are the reason the design has the shape it does.

| probe | result | consequence |
|---|---|---|
| slice of structs holding func values, ranged and called | WORKS | a table-of-tests testmain is possible |
| `fmt.Sprintf(format, args...)` with a non-literal format | **REJECTED** — "format must be a string literal" | `testing` cannot be written as Goo source that forwards to fmt |
| `fmt.Println(args...)` spread | **WRONG** — prints `[got 3 want 2]`, i.e. the slice, where Go prints `got 3 want 2` | even non-format forwarding to fmt is unusable |
| entry package whose clause is not `main`, with a `func main()` | WORKS | the test binary needs no package rename |

The second and third together are decisive: **the `testing` package cannot be
ordinary Goo source in `goostd/`.** Its formatting has to be reached another way.

## Architecture

Five pieces. Two reuse paths that already exist: the directory entry-package
machinery (PR #226) collects the files, and `goo run`'s temp-binary + exec +
exit-code path runs the result.

### 1. File set — `src/package/import_resolver.c`

`is_buildable_source` gains an `include_tests` parameter. Test mode includes
`*_test.go` and `*_test.goo`; every other caller is unchanged, so `goo build`
still excludes them.

One predicate with a flag, NOT a second scan: the whole point of that function is
that the entry package and `import "./p"` can never disagree about what a package
contains, and a parallel test-mode scanner would reintroduce exactly that.

### 2. Discovery

Walk the top-level declarations of the package's parsed files. A test is:

- name begins with `Test`, AND the character after `Test` is not a lowercase
  letter (Go's exact rule). So `TestAdd` and `Test_add` are tests; `Testify` is
  an ordinary function and is left alone. Bare `Test` is also a test.
- exactly one parameter, of type `*testing.T`,
- no results.

A function that passes the NAME rule but not the shape rule is a **compile
error** naming its file, line and the expected signature — never a silent skip. A
silently-skipped test is worse than no test, because it reports success.

The name rule is what keeps that error honest: without the lowercase-letter
exclusion, an ordinary helper called `Testable` would be dragged in and then
rejected for having the wrong signature.

### 3. Synthesized entry — `_testmain.goo`

Generated as text in memory and parsed as one more file of the package. This is
the operation PR #226 made natural: appending an AST to the package's file list
is now ordinary. It is never written to disk; a debug flag prints it.

```go
package <same as the package under test>

func main() {
	testing.Run("TestAdd", TestAdd)
	testing.Run("TestSub", TestSub)
	testing.Summary()
}
```

**Why the test function is passed as a VALUE, not just its name.** This is the
load-bearing choice in the whole design. `t.Fatal` and `t.FailNow` must stop the
running test, and nothing in Goo can unwind another function's frame. If the
runtime owns the call, it can `setjmp` before invoking the test and `longjmp` out
of `Fatal`. If the generated main called each test directly and only handed the
runtime a name, `Fatal` could set a flag but could not stop anything — it would
silently degrade to `Error`, and a test that "fails fast" would keep running past
its own failure. Passing the function value is what buys correct `Fatal`.

One call per test rather than a slice of them: a slice needs an `InternalTest`
struct type in the shim exports plus a func-typed struct field, where the unrolled
form needs only a func-typed PARAMETER. The table shape was verified to work, so
this is a simplicity choice, not a capability one.

The package clause is whatever the package under test declares (`mypkg`), not
`main` — verified to compile and run as an entry package.

### 4. The `testing` shim — `src/types/` + `src/runtime/testing.c`

`testing` joins the shim list in `is_stdlib_shim_import` (goo.c) and
`type_checker_is_plain_shim_import` (type_checker.c). It follows the
`sync.Mutex` pattern — a bespoke shim with a synthesized struct and methods
(`seed_sync_package_exports` is the model), because `stdlib_package_lookup`'s
per-symbol table cannot express a type with a method set.

Exports:

- opaque `T`: one pointer field holding a runtime handle.
- `Run(name string, fn func(*T))`, `Summary()`.
- methods on `*T`: `Error`, `Errorf`, `Fatal`, `Fatalf`, `Log`, `Logf`, `Fail`,
  `FailNow`, `Helper` (`Helper` is a no-op that exists so real Go test files
  compile).

**Formatting.** The `-f` methods lower AT THE CALL SITE using the same
compile-time formatter `fmt.Sprintf` already uses, then pass the finished string
to `goo_testing_log` / `goo_testing_fail`. In a real test file the format IS a
literal (`t.Errorf("got %d, want %d", got, want)`), so the existing machinery can
see it — the constraint measured above only bites when a format is forwarded
through a variable.

This is deliberate: a second implementation of `%`-verb semantics, one in the
compile-time formatter and one in a runtime formatter for `testing`, is precisely
the two-things-that-must-agree drift class PRs #220, #224 and #225 were each spent
fixing. One formatter.

A general runtime formatter (dynamic `fmt.Sprintf`, which also unblocks
`fmt.Errorf("%w")`) remains worth building — as its own arc, not inside this one.

### 5. Output and exit status — runtime

The C runtime owns the whole reporting surface: `=== RUN`, `--- PASS` / `--- FAIL`
with durations, indented log lines, the trailing `ok` / `FAIL <pkg> <time>`, and
the process exit status. Exit 1 if any test failed, 0 otherwise.

Putting the format in the runtime rather than the generated source keeps the
generated source trivial and keeps the output format in one place.

```
=== RUN   TestAdd
--- PASS: TestAdd (0.00s)
=== RUN   TestSub
    sub_test.goo:14: got 3, want 2
--- FAIL: TestSub (0.00s)
FAIL
FAIL  mypkg  0.01s
```

A package with no test files exits 0 and prints Go's `?   pkg  [no test files]`.

## Verification

- **Durations are not deterministic**, so a golden cannot match `(0.00s)`
  directly. The probe normalizes every duration (`s/[0-9]+\.[0-9]+s/X.XXs/`)
  before diffing, and asserts the exit code separately from stdout.
- Expected output is produced by real `go test` on the equivalent Go package
  (go1.26.1), normalized identically — the project convention that accept goldens
  are never hand-written applies here too.
- Cases to pin: an all-pass package; a package with one failure (exit 1); `Errorf`
  formatting; `Fatal` stopping its test but not the run; a package with no test
  files; a `Test`-shaped function with a wrong signature (reject fixture).
- Gates unchanged and all must stay green: goldens 473/473 at both levels,
  golden-reject 151/151, spec-conformance 53/53, `make test`, `verify-core` exit
  0, grammar tripwire 31/0.

## Risks

- **The shim signature table must accept a func-typed parameter** for
  `testing.Run`. Function values work as ordinary arguments and as struct fields
  (both measured), but not yet verified in a shim row. **This is the first thing
  to probe** — it decides whether §3 stands as written or has to fall back to a
  table built in the generated source.
- **`setjmp`/`longjmp` across the runtime/generated-code boundary** must not
  corrupt the arena or defer stacks. A `Fatal` inside a function holding an open
  `arena { }` block is the case to think about, since the arena's free is emitted
  at block exit and a longjmp skips it.
- **Test files change the package's build set**, so a mistake in the
  `include_tests` flag would silently pull `_test` files into `goo build`. The
  existing 473 goldens are the net.

## Explicitly not in this arc

`t.Run` subtests, benchmarks, `-run`, `t.Parallel`, `./...`, external test
packages, `TestMain`, examples, coverage. Also not here: the general runtime
formatter, which is its own arc and is the thing that would let `testing` be
written as ordinary Goo source later.
