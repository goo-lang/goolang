# T4 emission, measured: 398 MB reclaimed, and 0% on the daemon

Measured 2026-07-29 against the T4 emission branch.

## What landed

The first memory this compiler has ever reclaimed. `goo_release` is emitted at
each function exit for every local the release plan approves.

Proven differentially, with `GOO_ARC_RELEASE=0` as the control:

| `examples/arc_release_probe.goo` | allocs | frees | definitely lost | valgrind |
|---|---|---|---|---|
| `GOO_ARC_RELEASE=0` | 10,000 | 0 | 240,000 B in 10,000 blocks | rc=99 |
| default | 10,000 | 10,000 | 0 | rc=0, 0 errors |

**And on a real program, 398 MB.** The no-arena control of
`arena_return_reclaim_probe` runs 200,000 iterations building a 2 KB object:

| no-arena variant | peak RSS |
|---|---|
| `GOO_ARC_RELEASE=0` | 407,636 KB |
| default | 1,956 KB |

A 208x reduction, and it is independent evidence rather than a purpose-built
probe. It also broke `arena-rss-probe`, which is worth recording: that probe
asserted "a plain block heap-leaks", which was true before ARC and is now false.
Its control now compiles with `GOO_ARC_RELEASE=0` so it isolates the ARENA rather
than comparing two things that both reclaim.

## And 0% on the daemon, which was the target

`bench/daemon/daemon.goo`, 400,000 requests:

| | peak RSS |
|---|---|
| `GOO_ARC_RELEASE=0` | 750,764 KB |
| default | 751,260 KB |

Identical within noise. The projected 9.3% did not arrive.

**The plan is not at fault.** It approves exactly the local the spike chose:

```
func handle — 8 locals
  fields       RELEASE_OK             <- RELEASE
  counts       RELEASE_NO_ESCAPES
  total        RELEASE_NO_REBOUND
  f            RELEASE_NO_ESCAPES
  err          RELEASE_NO_LOOP_SCOPE
  ...
```

**The EMISSION is.** From the daemon's IR:

```llvm
%fields = alloca { ptr, i64, i64 }, align 8
```

A slice is a FAT VALUE — pointer, length, capacity, held inline. It is not a
pointer, so `codegen_arc_note_local`'s type guard skips it and no release is
emitted. `goo_release` appears 0 times in the daemon's IR.

## UPDATE 2026-07-29: slice release landed, and the daemon moved

`TYPE_SLICE` now releases field 0, the data buffer. Measured on
`bench/daemon/daemon.goo` at 400,000 requests:

| | peak RSS |
|---|---|
| `GOO_ARC_RELEASE=0` | 750,500 KB |
| default | **694,372 KB** |

**54.8 MB reclaimed**, which is 140 B for each request against a projection of
128 B. The 0% below is the state BEFORE that change, and it is kept because the
diagnosis is what led to the fix.

`arc_release_slice_probe.goo` goes from 880,000 bytes lost in 20,000 blocks to 0,
valgrind clean.

**The sub-slice negative is the important half.** `s := ys[1:3]` is an INTERIOR
pointer into a buffer another local owns, so a release of it hands
`interior - 16` to `free()`. Condition 2 refuses it, measured before the emission
was written, and `arc_release_subslice_probe.goo` now gates it with zero invalid
operations.

**Teeth on the new target.** Changing the slice case from `field = 0` to
`field = 1` gives `goo_release` the LENGTH as a pointer. The probe segfaults:
rc=139, `Invalid read`, SIGSEGV. So the gate guards the field index and not merely
the presence of a release.

`append` is still refused. `xs = append(xs, n)` marks `xs` escaping, because
`append` has no `param_escape` summary and an unresolved callee is conservative.
That is why the daemon gains `fields` (from `strings.Split`) and not `parts`.

Still refused, and unchanged: `TYPE_STRING` (the next increment) and
`TYPE_INTERFACE` (field 0 is the VTABLE, so it must never be released).

## UPDATE 2026-07-29: the self-append idiom, and the daemon halves again

| daemon, 400,000 requests | peak RSS |
|---|---|
| `GOO_ARC_RELEASE=0` | 750,996 KB |
| default | **632,464 KB** |

**115.8 MB**, up from 54.8 MB with `fields` alone. `parts` releases now too, and
the output is identical (`23` either way).

**Neither half of this was worth a byte on its own**, and that was measured rather
than assumed before either half was built:

| state | verdict for `xs = append(xs, n)` |
|---|---|
| before | `RELEASE_NO_ESCAPES` |
| after `append` gets a non-retaining slice argument | `RELEASE_NO_REBOUND` |
| after the self-append rule as well | `RELEASE_OK` |

To build append-only would have produced a correct-looking change worth 0%, which
is the trap the pointer-only emission fell into.

The hazard is `t := append(s, x)`, where `t.data` can equal `s.data`. Condition 2
refuses it, so the self-append rule matches arg 0 against the target by NAME.
`a = append(b, x)` is a real rebind and stays refused.

## The limitation, stated precisely

T4 releases a local whose SLOT is a bare pointer. That covers `new(T)` and
`&T{}`, which is what the 398 MB above came from.

It releases nothing for a slice, a string, a map or an interface, because each is
a multi-field value whose heap buffer sits behind an INNER pointer field. Those
are where the daemon's bytes live: `goo_map_set_sv` 20.3%,
`goo_strings_trim_space` 19.0%, `goo_strings_split` 17.9%, `goo_slice_append`
10.5%.

**A blanket "release field 0 of any struct" would be UNSOUND.** For a slice,
field 0 is the data array. For an interface value it is plausibly a type
descriptor, and handing that to `free()` is catastrophic. Doing this safely needs
the Goo `Type*` that `ValueInfo` already carries, not the LLVM type, so the rule
becomes "if this local's Goo type is a slice, release its data pointer" — one
type at a time, each with its own rows and its own valgrind probe.

That is the next increment, and it is where the daemon's bytes are.

## Why the probe cannot return a value read through the object

`escape_expr_taint`'s `AST_UNARY_EXPR` arm returns the operand's taint for EVERY
unary operator. That is correct for `&x`, where the address aliases `x`, and
over-conservative for `*p`, where the deref copies the pointee out. So
`return *p` marks `p` escaping and the release is refused:

| shape | verdict |
|---|---|
| `p := new(int); _ = p` | RELEASE_OK |
| `p := new(int); *p = n; _ = p` | RELEASE_OK |
| `p := new(int); return *p` | RELEASE_NO_ESCAPES |

Sound, and it has never caused a defect. It is also why
`examples/arc_release_probe.goo` increments a global rather than returning
something read through `p`. Fixing it means changing the arm all three escape
passes share, which is what the arm-coverage matrix exists to protect.

The daemon's `fields` is unaffected: it survives because `strings.TrimSpace` is
whitelisted and EMPTIES the result taint, which is what PR #256 bought.

## Instrument notes

- **The gate proves it can report the unreclaimed state first.**
  `arc-release-probe` fails if `GOO_ARC_RELEASE=0` does NOT leak, because a probe
  that cannot see the leak cannot prove the reclamation.
- **The gate has teeth against an UNSAFE release.**
  `examples/arc_release_escape_probe.goo` returns its local. With `decide()`
  forced to return `RELEASE_OK` unconditionally, it reported three
  `Invalid read of size 8` and exited 99. So the probe detects a wrong release,
  not merely the absence of a right one.
- **Reading the value table at exit found nothing.** The first emission attempt
  walked `codegen->value_table` at the exit point and emitted no release at all.
  The function body is a block statement, so `vscope_exit` truncates its locals
  before any exit path runs — measured: at `work`'s exit the slice held only the
  parameter. The release list is therefore captured at DECLARATION time, in
  `vscope_add`, the one choke point every binding passes through.
- `GOO_ARC_DEBUG=1` prints which locals will be released, and it is what found
  the defect above.

## Reproducing

```sh
make arc-release-probe                       # the differential gate
GOO_ARC_DEBUG=1 bin/goo --emit-llvm -o /tmp/x.ll examples/arc_release_probe.goo
./dump_release_plan bench/daemon/daemon.goo  # see README.md for the build line
```
