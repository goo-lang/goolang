# Peak RSS at 400,000 requests — ARC against its own control and against Go

Taken 2026-08-02 on `main` @ `28db3fa`, immediately after PR #286 merged.
`verify-core` ALL GREEN on that tree, exit 0.

**All three builds were measured in ONE sitting.** `.handoff.md` records a
control that moved 5.6% between two sittings, which made every percentage in
that file non-comparable across sessions. Every figure here comes from the run
below, and the script is `scripts/`-free on purpose — it is reproduced verbatim
at the bottom so the numbers can be re-taken rather than inherited.

## The measurement

```bash
GOO_ARC_RELEASE=0 ./bin/goo -O2 -o $W/daemon_of bench/daemon/daemon.goo
                  ./bin/goo -O2 -o $W/daemon_on bench/daemon/daemon.goo
go build          -o $W/daemon_go bench/daemon/daemon.go
/usr/bin/time -f '%M' $W/daemon_XX 400000      # peak RSS in KB
```

The three output paths are the SAME LENGTH on purpose. `main` reads `os.Args`,
the runtime copies `argv[0]` to the heap, and nothing frees it, so a longer
path changes the byte total. See `baseline-before.md`.

## Peak RSS at 400,000 requests

| Build | Peak RSS | Output |
|---|---|---|
| `GOO_ARC_RELEASE=0` — the control | 793,248 KB | `23` |
| **default, ARC on** | **26,276 KB** | `23` |
| Go, `go build` on `daemon.go` | 8,232 KB | `23` |

**766,972 KB reclaimed, 96.7% of the control's peak.** All three print `23`.

Goo now sits at **3.19x Go's peak RSS** on this program. The figure ADR 0002
was written against was 651 MB against Go's flat 8 MB, which is 81x.

## In use at exit, 2,000 requests, under valgrind

| Build | In use at exit | Blocks | Errors |
|---|---|---|---|
| `GOO_ARC_RELEASE=0` | 2,866,207 bytes | 88,003 | 0 |
| **default, ARC on** | **82,207 bytes** | **4,003** | **0** |

1,433 bytes for each request without release, and **41 bytes for each request
with it**.

**READ THE BLOCK COUNT FIRST.** 4,003 blocks agrees exactly with
`result-after.md`. The byte total differs by 2 from the 82,205 recorded there,
and that difference is the output path, not the compiler — which is the
`argv[0]` trap doing precisely what `baseline-before.md` says it does.

## What this retires

Three claims in the tree contradicted these figures before this file existed.

- `CLAUDE.md` said the successor model is "DECIDED but not implemented". ARC
  ships, and `GOO_ARC_RELEASE=0` is the kill switch, not the default.
- `CLAUDE.md` said "a long-running service is not expressible" and gave 651 MB
  at 400,000 requests against Go's flat 8 MB. The measured figure is 25.7 MB
  against 8.0 MB.
- `docs/2026-07-08-v1-roadmap.md` recorded ADR 0002 as "status: proposed". The
  ADR says "accepted (2026-07-28)".

## What this does NOT claim

- **This is one program.** It is the shape ADR 0002 was written for, and it is
  not evidence about a corpus. The CALLEE_VALUE limit
  (`include/escape_core.h:128`) makes each local with a method set
  unreleasable, and nobody has measured how far that reaches.
- **Reference cycles still leak.** ADR 0002 names this and it has no cheap fix.
- **The 82,207 bytes that stay are a conditional hand-over.** `f` reaches
  `counts[strings.ToUpper(f)]` on one arm only, so its buffer goes to nobody on
  the other arm. One release site cannot express that.

## The script, verbatim

```bash
#!/usr/bin/env bash
set -u
W=$(mktemp -d); REQ=400000
GOO_ARC_RELEASE=0 ./bin/goo -O2 -o "$W/daemon_of" bench/daemon/daemon.goo
                  ./bin/goo -O2 -o "$W/daemon_on" bench/daemon/daemon.goo
go build -o "$W/daemon_go" bench/daemon/daemon.go
for b in daemon_of daemon_on daemon_go; do
    out=$("$W/$b" "$REQ")
    kb=$(/usr/bin/time -f '%M' "$W/$b" "$REQ" 2>&1 >/dev/null | tail -1)
    printf '%-10s output=%-4s peakRSS=%s KB\n' "$b" "$out" "$kb"
done
for b in daemon_of daemon_on; do
    valgrind "$W/$b" 2000 2>"$W/$b.vg" >/dev/null
    grep -oE 'in use at exit: [0-9,]+ bytes in [0-9,]+ blocks' "$W/$b.vg"
done
```
