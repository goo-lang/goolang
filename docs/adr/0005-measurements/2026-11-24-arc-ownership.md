# Precision cost of the ARC ownership fixes (audit Track 3)

Taken 2026-11-24 on branch `fix/audit-arc-ownership` @ `e1663104`, against
`origin/main` @ `d2aa8864` as the control. Both builds ran in ONE sitting, with
the same host and the same corpus (`examples/*.goo`, `bench/*/*.goo`).

## What changed

- Task 3.1: a bare `return` reports the named results.
- Task 3.2: a call result is owned only if `returns_fresh && !return_escapes`.
  A Goo multi-value result is never owned. A func variable or a method never
  matches a top-level name. `has_alias` sees locals inside a composite literal.
- Task 3.3: the loop release follows views through locals, and it treats a
  loop-header variable and a parameter callee correctly.

## Commands

```bash
# in each tree (main build dir, then this worktree):
GOOROOT=$PWD GOO=$PWD/bin/goo ./scripts/arc_reason_census.sh <out>
# per-local rows, compared line by line (one row per local, same order):
GOOROOT=$PWD GOO_ARC_DEBUG=1 ./bin/goo -o /dev/null <prog>.goo
# daemon, output dirs of equal length (argv[0] is copied to the heap):
./bin/goo -O2 -o <W>/ma/daemon bench/daemon/daemon.goo     # main
./bin/goo -O2 -o <W>/br/daemon bench/daemon/daemon.goo     # branch
valgrind <W>/XX/daemon 2000 ; /usr/bin/time -f '%M' <W>/XX/daemon 400000
```

## Census: the released count falls by 40

Both runs read 14,816 rows and compiled the same programs.

| Row | main | branch | Delta |
|---|---|---|---|
| released (`RELEASE_OK`), all and user code | 365 | 325 | -40 |
| `RELEASE_NO_NOT_OWNED` | 586 | 632 | +46 |
| `RELEASE_NO_BLOCK_ESCAPE` | 13 | 7 | -6 |
| every other verdict | same | same | 0 |
| reason `GLOBAL_STORE` | 18 | 19 | +1 |
| every other reason (`CALL_OPAQUE`, `CALL_RETAIN` and so on) | same | same | 0 |

Every changed row is user code. No vendored `goo_pkg__` row changed, and no
`bench/` row changed.

Per-local comparison, 50 rows differ:

| Transition | Rows |
|---|---|
| `RELEASE_OK` to `RELEASE_NO_NOT_OWNED` (precision loss) | 43 |
| `RELEASE_NO_BLOCK_ESCAPE` to `RELEASE_OK` (Task 3.3 gain) | 3 |
| `RELEASE_NO_BLOCK_ESCAPE` to `RELEASE_NO_NOT_OWNED` | 3 |
| `RELEASE_NO_ESCAPES`, one extra reason (`GLOBAL_STORE`) | 1 |

Check: 365 - 43 + 3 = 325. 586 + 43 + 3 = 632.

## The 43 locals that ARC no longer releases

The census reason set does not name the Task 3.2 rule that refused a local, so
this grouping comes from reading each program, not from the tool. Treat the
group sizes as the best reading, not as a measured attribution.

| Group | Locals | Programs | Reason the local is not owned |
|---|---|---|---|
| Goo multi-value result | 27 | `multiret_narrow_probe` (12), `erru_destructure_probe` (4), `forward_ref_multireturn` (4), `error_roundtrip_probe` (3), `tuple_assign_from_call` (2), `multiret_iface_probe` (1), `blank_ident_probe` (1) | A multi-value result is never owned (Task 3.2). |
| Named-result function | 6 | `named_return_probe` (6): `a`, `b`, `v`, `xs`, `gx`, `gy` | `a`, `b`, `gx`, `gy` are multi-value. `v` comes from `inc(n int) (r int)`, which has an int parameter. `xs` comes from `firstTwo() (xs []int)`. The cause of `xs` was not isolated. |
| Func value result | 5 | `closure_probe` (`c`, `d`, `outer`), `funclit_probe` (`g`), `funcval_probe` (`g`) | The callee returns a closure or a func variable. A func variable never matches a top-level name (Task 3.2). |
| Global-backed result | 3 | `arc_release_global_probe` (`useErr`, `useLate`, `usePtr`) | `getErr`, `getLate` and `getPtr` return a global. The result is not fresh. main released a global alias here, so this loss is a correct fix and not a cost. |
| Pointer or reference parameter | 2 | `arc_release_split_escape_probe` (`got`), `pkg_type_probe` (`l`) | `build(req string) []string` and `makeLine(a, b Point) Line` take a parameter that may flow to the result, so `return_escapes` is set. This is the known loss, see below. |

The three `BLOCK_ESCAPE` to `NOT_OWNED` rows are `switchBreak: keep`
(`arc_loop_carried_probe`), `main: g` (`closure_probe`) and `main: delta`
(`lanes_jacobi_probe`). They were not released before either.

The 3 gains are `main: s` (`arc_concat_operand_probe`), `main: echoed`
(`arc_release_borrowed_elem_probe`) and `main: joined`
(`arc_release_split_escape_probe`). The loop release now follows a view through
a local.

## Known loss: a scalar parameter sets `return_escapes`

`mk(n int) *Box { return &Box{v: n} }` is, by source reading, not owned because the
int parameter sets `return_escapes`. No measurement confirms this cause. By
the same reading, the rule cannot tell an int that flows to the result from a
pointer that flows to the result. The census reasons do not isolate this
case, and the only corpus members I could tie to a parameter are the two rows in
the last group plus `v` in the named-result group. I did NOT measure the bytes
that this rule leaks. A synthetic `mk` program did not discriminate the two
builds, because `main` already refused the local for reason `RETURN`
(both builds, 24,000 bytes in 1,000 blocks). A rule that ignores non-pointer parameters might recover this loss. It is
not tested and it is not part of this track.

## Daemon benchmark: no change

`bench/daemon/daemon.goo`, `-O2`, output dirs `ma` and `br` of equal length.

| Build | In use at exit, 2,000 req | Blocks | Peak RSS, 400,000 req (two runs) | Output |
|---|---|---|---|---|
| `origin/main` d2aa8864 | 82,204 bytes | 4,003 | 25,732 / 25,464 KB | `23` |
| branch e1663104 | 82,204 bytes | 4,003 | 25,988 / 25,704 KB | `23` |
| branch, `GOO_ARC_RELEASE=0` | 2,866,205 bytes | 88,003 | not measured | not measured |

Block delta: 0. Valgrind: 0 errors in both. The RSS difference (256 KB) is
smaller than the run-to-run spread, so it is noise. No `bench/` census row
changed, which agrees with this result.

## Safety evidence (controller measurement)

Not taken by this task. At `e1663104`, 85 ARC probe programs from the audit and
three review rounds give 0 valgrind errors. The same programs give 35 errors on
`origin/main`. The programs are in the controller scratchpad and are not in the
repository.

## Recount of the 216 no-evidence locals

CLAUDE.md quotes 216 user-code locals refused ONLY by a no-evidence mark. The
census script does not print this figure. The method is the one in
`reason-census.md`: user-code rows, not `RELEASE_OK`, with a reason set made
only of `CALL_OPAQUE` and `CALL_VARIADIC`. An awk filter over the per-local rows
gives, on both builds:

| Build | Locals | Of those, `RELEASE_NO_ESCAPES` |
|---|---|---|
| `origin/main` d2aa8864 | 216 | 216 |
| branch e1663104 | 216 | 216 |

The figure is unchanged, so CLAUDE.md keeps 216. The one `RELEASE_NO_ESCAPES`
row that gained `GLOBAL_STORE` (`main: a` in `arena_named_result_probe`) already
carried `CALL_RETAIN`, an evidenced reason, so it was never in the 216 set.
The main build reproduces the documented 216, which is the positive control for
the filter.

## Daemon bytes

CLAUDE.md quotes 82,207 bytes. Both builds here read 82,204. The byte total
moves with the output path length, so the block count (4,003) is the figure to
compare.

## Verdict

What each piece of evidence shows, and no more:

- Census: 43 locals changed from released to not owned, and 3 gained a release.
  All are in `examples/` probe programs.
- Daemon: the same 4,003 blocks on both builds. This is one program.
- Source reading, not measurement: the grouping of the 43 locals by cause, and
  the statement that 3 of them (`arc_release_global_probe`) came from a callee
  that returns a global.
- Controller measurement: 85 probe programs give 0 valgrind errors on the
  branch and 35 on `origin/main`. This covers those programs as a whole. No
  per-local link to the 43 locals exists, so it does not show that any one lost
  release was unsound.

## Not done

- No byte measurement of the 43 locals. The census counts locals, not bytes.
- No isolation of the rule that refused each local.
- The `mk` loss has no positive control.
