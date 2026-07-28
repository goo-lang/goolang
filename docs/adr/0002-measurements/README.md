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

## The one number that matters

About **1.63 KB retained per request, forever**. At 1,000 requests per second
a Goo service passes 8 GB in roughly 80 minutes and is killed. Go holds flat
at about 8 MB on the identical program.
