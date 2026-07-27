# CLI readiness — findings from building a real tool

**Date:** 2026-07-27
**Method:** write an ordinary command-line tool in Goo (`examples/cli/googrep`,
a grep-shaped tool) and record every gap hit, in the order hit. No gap was
fixed during this exercise; the deliverable is the sequenced list.

**Why this way.** A gap list read off `shim_signatures.c` names what is
*absent*. It does not name what actually stops a person. Two of the four
findings below — the `package main` test collision and the slice-element
miscompile — are invisible from the shim table and were only found by writing
the tool.

## Verdict

A CLI tool in Goo **compiles, runs, and behaves correctly for its happy path
today**. `googrep` implements `-n`, `-c`, multi-file name prefixes, and grep's
exit-status convention (0 matched / 1 no match / 2 error), and all of it works.

Two findings block calling it *ready*: a tool cannot report errors correctly
(GAP 2), and a tool cannot be tested at all (GAP 3).

## GAP 1 — `for` with an omitted init or post clause does not parse

```go
for ; i < len(args); i++ { }   // syntax error
for i := 0; i < n; { }         // syntax error
for i := 0; i < n; i++ { }     // OK
for i < n { }                  // OK
```

Ordinary Go, and the natural shape when the loop variable is already bound —
which is exactly what argument parsing does after consuming flags.

**Severity:** low. The workaround (a condition-only loop with a manual
increment) is mechanical, and `googrep` uses it.

**Note:** the conformance matrix's `stmt_for` row says "for (3 forms)". These
omitted-clause variants are a fourth and fifth shape that row does not cover,
so this is a genuine coverage hole, not a known-and-accepted gap.

## GAP 2 — no program can write to stderr

> **RESOLVED**, same day. `fmt.Fprint`/`Fprintln`/`Fprintf` plus `os.Stdout`
> and `os.Stderr`. See the resolution note at the end of this section.

There is no `os.Stderr`, `fmt.Fprintln` or `fmt.Fprintf`. Every `fmt` row
prints to stdout. Verified: no user-facing stderr path exists anywhere in
`src/types/shim_signatures.c`.

Measured, same input, `googrep` versus real grep:

| | exit | stdout | stderr |
|---|---|---|---|
| `googrep alpha missing.txt` | 2 | `googrep: missing.txt: cannot read file` | *(empty)* |
| `grep alpha missing.txt` | 2 | *(empty)* | `grep: missing.txt: No such file...` |

**Severity: HIGH — this makes a tool incorrect, not merely limited.** The
diagnostic lands in the tool's own data stream. Anything consuming
`googrep ... | sort` receives the error text as input. The exit status is
right, so a caller can detect failure; a *pipeline* still gets corrupted.

## GAP 3 — a `package main` tool cannot be tested

> **RESOLVED**, same day, by renaming the program's own `main` in test mode.
> See the resolution note at the end of this section.

```
$ cd examples/cli/googrep && goo test .
Type error at _testmain.goo:5:6: Function 'main' already declared
```

The synthesized `_testmain.goo` joins the package under test and declares
`main`, which collides with the tool's own `main`. **A CLI tool is package
main, so no CLI tool can have tests.**

Go does not hit this because its generated testmain is a SEPARATE package that
*imports* the package under test, leaving the user's `main` an ordinary
function rather than the entry point. Goo's `_testmain.goo` is one more file of
the same package (PR #227), which is what makes it collide.

**Severity: HIGH.** It directly contradicts "testable" for this class of
program.

**The usual Go workaround does not rescue it here.** Splitting into a library
package plus a thin `main` is the normal structure, and the library half does
test fine — but the split immediately runs into GAP 4, because a library
package is a vendored package and text-processing code calls `strings.*` on
slice elements.

`examples/cli/googrep/main_test.goo` is committed and currently cannot run. It
is deliberate: it becomes the regression pin the moment GAP 3 is closed.

### Resolution

In test mode only, the program's own `main` is renamed before the synthesized
file is parsed, so the generated entry point is the only `main` in the package.
The chosen name is not a legal Goo identifier, so it cannot collide with a user
declaration.

**This reaches Go's observable behaviour without Go's architecture.** `go test`
on package main compiles the package as a library and puts its testmain in a
SEPARATE package, so the program's `main` is compiled but never called. The
rename produces the same result — verified: `goo test` on a package whose main
prints `42` outputs only `ok`, with no `42`. The package-architecture change
remains the more faithful option, and is the right move if `package foo_test`
isolation is ever wanted; it was not worth days here for the same observable
result.

Normal builds are untouched: `goo build` and `goo run` on the same package
still work, because the rename happens only under `goo test`.

Pinned by two CLI rows — `test-package-main` (the tests run) and
`test-package-main-builds` (the program still builds and runs). Both are needed:
the first alone would still pass if the rename leaked into ordinary builds.
`examples/cli/googrep` now tests too, which is what its committed test file was
waiting for.

## GAP 4 — arguments crossing into a vendored-package function miscompile

> **RESOLVED**, same day. The fix is below the original write-up. Fixing it
> also widened it: the defect was not only slice elements.

```go
xs := []string{"ab"}
_ = strings.Index(xs[0], "a")   // Module verification failed
```

```
Call parameter type does not match function signature!
  %slice_elem13 = getelementptr { ptr, i64 }, ptr %slice_ptr8, i64 %i7
 { ptr, i64 }  %2 = call i64 @goo_pkg__strings__Index(ptr %slice_elem13, ...)
```

The element GEP yields an address, and the callee wants a loaded `{ptr, i64}`
string value. The lvalue is never loaded.

Scope, established by probe:

| Shape | Result |
|---|---|
| `strings.Index(xs[0], p)` — vendored package, direct | **FAIL** |
| `s := xs[0]; strings.Index(s, p)` — bound first | OK |
| `f(xs[0])` — plain local call, direct | OK |
| `fmt.Println(xs[0])` — shim call, direct | OK |

**Root cause: a FOURTH argument loop.** `src/codegen/call_codegen.c:165-172`
is the vendored-package call path, and it does not route through
`codegen_emit_fixed_call_arg` (which the plain-call, method-call and
interface-dispatch loops all use, at :2142, :2531, :2766). Its comment states
the assumption that fails:

> codegen_generate_expression loads scalar lvalues to a value, so the raw
> llvm_value already matches the callee's parameter type

That holds for a scalar. It does not hold for a slice-element expression whose
element type is not scalar — the GEP is an address.

**This falsifies a claim in `.handoff.md`**, which says `call_codegen.c`'s
argument handling "should now be considered DONE as a drift source — there is
no fourth loop left to find there." This is that fourth loop. It is the same
two-loops-drift shape that produced PRs #220, #224 and #225, and the same fix
applies: route it through the shared helper.

**Severity: HIGH.** It is a loud failure rather than a silent miscompile, which
is the good case — but `strings.Index(lines[i], pat)` is exactly what
text-processing code writes, and the workaround (bind to a local first) is
non-obvious from the diagnostic.

### Resolution, and how the gap widened

Routing the loop through `codegen_emit_fixed_call_arg` fixed it. Probing the
fix first revealed a SECOND missing step that the slice-element case had
hidden — **interface boxing**:

```go
var s sort.IntSlice = x
sort.Sort(s)          // Module verification failed
```
```
{ ptr, ptr }  call void @goo_pkg__sort__Sort({ ptr, i64, i64 } %s4)
```

A concrete value passed to an interface-typed parameter arrived unboxed: a raw
slice where the callee wants `{ptr, ptr}`. That means `sort.Sort` — whose whole
signature is an interface — could not be called from another package at all,
and the package shipped that way in PR #229. Its own tests never caught it
because an internal test calls `Sort(s)` as a PLAIN call, not through the
package path.

So the loop was missing two of the five steps, which is the same shape the
interface-DISPATCH loop had before PR #225.

**The fix needs the declared Goo signature, not just LLVM types.** Interface
boxing and the nullable auto-wrap are driven by the parameter's Goo KIND, and
an interface parameter is only `{ptr, ptr}` in LLVM — indistinguishable from
other pairs. `type_check_expression(checker, call->function)` supplies it, which
is the same re-entry the result type at the end of the function already makes.
LLVM parameter types are still read off the resolved callee for width coercion,
matching the method loop.

Pinned by `examples/pkg_arg_shapes_probe.goo`, whose expected output came from
`go run` on the equivalent Go program. Verified RED first, with both failure
shapes visible in the diagnostic.

## GAP 5 — calling `main()` emits raw LLVM verifier noise

Found while checking GAP 3's resolution, and **pre-existing and independent of
it**:

```go
func helper() { main() }
func main()   { fmt.Println("hi") }
```
```
Module verification failed: Incorrect number of arguments passed to called function!
  %call = call i32 @main()
```

That is an ORDINARY `goo build`, with no test mode involved. Codegen gives the
entry point an `i32` return that no call site expects, and nothing rejects the
call earlier, so the user sees verifier output.

Two reasons it matters beyond the shape itself:

- The golden-reject harness enforces a project-wide rule that no LLVM verifier
  noise reaches users. This path violates it, and no fixture covers it.
- It should be a clean positioned diagnostic. Go permits `main()` to be called
  like any function, so the honest options are to support it or reject it by
  name — not to fail in the backend.

**Severity: low** in practice (calling `main()` is rare), but it is a real
defect and the diagnostic is the worst kind.

## What worked well, and is worth recording

- `os.Args`, `os.Exit`, `os.ReadFile`, `os.Getenv` cover the basics.
- Exit-status propagation is exact: 0/1/2 all arrived at the shell correctly.
- The `!T` error union with `catch => ""` is genuinely pleasant for a tool —
  `os.ReadFile(name) catch => ""` reads well and needs no error plumbing.
- The everyday `strings` surface is present: `TrimSpace`, `ToLower`, `ToUpper`,
  `Contains`, `Split`, `Join`, `Fields`, `HasPrefix`, `Index`, `Replace`.
  Only `Title` is absent, which Go itself deprecated.
- `sort` (PR #229) covers output ordering.
- Multi-file packages, struct-free tuple returns (`([]string, int)`), slices of
  strings, and `append` all behaved.

## Recommended order

The plan's original ordering put stderr first. These findings change it:

1. **GAP 4 (miscompile)** — first. It is a defect in shipped behaviour, not a
   missing feature, and it blocks the library-plus-thin-main structure that
   GAP 3 forces people into. Well understood: route the fourth loop through
   `codegen_emit_fixed_call_arg`.
2. **GAP 2 (stderr)** — the correctness blocker for tools.
3. **GAP 3 (`package main` tests)** — needs a design decision, so it should not
   block the two above. Options: compile the package under test as a library
   and put `_testmain` in its own package (Go's model), or rename/skip the
   user's `main` in test mode. The first is Go-faithful; the second is smaller.
4. **GAP 1 (`for` clauses)** — mechanical, and a conformance row should come
   with it.
