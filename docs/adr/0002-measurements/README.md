# Measurements behind ADR 0002

Kept so the numbers in the ADR can be re-run rather than trusted. None of
these is wired into a gate: they are evidence for a design decision, not a
regression net.

Peak resident set size comes from `/usr/bin/time -v`, the same method
`scripts/arena_rss_probe.sh` uses.

## The programs

| File | What it shows |
|---|---|
| `daemonN.goo` | The daemon shape: a loop that allocates per request and holds nothing. Takes the request count as `os.Args[1]`. RSS grows linearly and without bound. |
| `daemon_arena.goo` | The same program with the per-request work wrapped in `arena { }`. Measures identically, which is the finding. |
| `a_new.goo` | `new(int)` and `&Pair{}` inside an arena — the shape arenas DO reclaim. |
| `a_append.goo` | `[]int{}` and `append` inside an arena — the shape they do not. |

## Reproducing

```bash
GOO=bin/goo
$GOO docs/adr/0002-measurements/daemonN.goo -o /tmp/daemonN
for n in 50000 100000 200000 400000; do
  /usr/bin/time -v /tmp/daemonN $n 2>&1 >/dev/null \
    | grep -F "Maximum resident set size"
done
```

For the arena contrast, compile each of `a_new.goo` / `a_append.goo` twice —
once as written, and once with `arena {` replaced by `{` — and compare peak
RSS:

```bash
sed 's/arena {/{/' docs/adr/0002-measurements/a_new.goo > /tmp/a_new_plain.goo
```

The Go reference figures come from `go run` / `go build` on the same source
with the package clause unchanged (`go1.26.1`, `/usr/local/go/bin/go`).

## The C probes, and how to build them

`dump_local_escape.c` prints every local's escape verdict for a `.goo` file.
`dump_ownership.c` prints the per-CALLEE `param_escape` summaries AND the
per-local verdicts side by side, because T4's ownership question is answered at
the callee level and its escape question at the local level — comparing them in
one run beats reconciling two by hand.

Neither has a Makefile target, because neither is a gate. Link them against the
same objects the unit suites use:

```bash
# Take the object list from a unit-suite LINK, not from a glob over build/.
# `ls build/**/*.o` pulls in objects the suites do not link (far_transport.o
# wants -lnng) and the link fails with undefined references to nng_*.
make release_decision_test > /tmp/link.log 2>&1
OBJS=$(grep -oP 'build/\S+\.o' /tmp/link.log | sort -u | tr '\n' ' ')
gcc -Wall -std=c23 -g -I. -Iinclude -D_GNU_SOURCE \
    -include include/xalloc.h -I/usr/lib64/llvm22/include -DLLVM_AVAILABLE=1 \
    -o dump_ownership docs/adr/0002-measurements/dump_ownership.c $OBJS \
    -lm -pthread -ljson-c -lcurl -lz -L/usr/lib64/llvm22/lib64 -lLLVM-22
```

Run `make param_escape_test` first if `build/` is empty, and note that **zsh does
not word-split an unquoted `$OBJS`** — use bash for that line or the whole object
list arrives as one filename.

`ownership_shapes.goo` is the T4 condition-2 fixture and it is DELIBERATELY
IMPORT-FREE. `.handoff.md` records a local_escape table that looked confident and
was conservative for an unrelated reason: the imports had not resolved, and the
tell was `i`, a plain int loop counter, reading as escaping. One cause per
verdict needs a reproduction with nothing to resolve.

`dump_release_plan.c` prints the T4 release plan for a `.goo` file, which is how
you tell a plan that refused from an emission that missed.

Findings: `t4_condition2_findings.md` and `t4_emission_findings.md`.

## The one number that matters

About **1.63 KB retained per request, forever**. At 1,000 requests per second
a Goo service passes 8 GB in roughly 80 minutes and is killed. Go holds flat
at about 8 MB on the identical program.
