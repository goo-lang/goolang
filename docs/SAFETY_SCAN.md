# Goo compiler safety scan

The Goo compiler and runtime are hand-written C with no garbage collector, so
memory-safety defects are the dominant risk. [snare](../../semgrep-competitor) —
a graph-native SAST tool — scans the C source with a memory-safety rule pack.

Run it locally:

```sh
make safety                       # gate: fails on findings not in the baseline
scripts/safety-scan.sh --update-baseline   # accept current findings as baseline
```

The gate needs the snare checkout at `../semgrep-competitor` (override with
`SNARE_DIR=`) and `jq`. It is **local-only** — there is no CI — matching snare's
own verification workflow.

The scan runs snare with `--workers 1` (serial ingest). At full-tree scale
parallel ingest intermittently drops findings on large files, which makes the
baseline unstable; serial ingest is slower (a few minutes over ~230 files) but
deterministic, so the gate compares like with like.

## Scope

Scanned: `src/`, `include/` (plus `lib/` and `kernel/`, currently empty of C).
The scan is **triage-grade, not a proof**: it is intraprocedural (no tracking
across function calls), by-name (no pointer aliasing / points-to), and
recall-biased. A green gate is a **regression guard** — "no new findings" — not a
certificate of memory safety.

### Coverage caveat

**19 files failed to parse** (tree-sitter's C grammar does not recognize every
C23 construct Goo uses). Findings in those files are incomplete — this is a
recall floor, not a clean bill. The count is reported on every run
(`parse-error files: N`).

## Rules

| Rule | CWE | Severity | What it flags |
| --- | --- | --- | --- |
| `c-use-after-free` | 416 | critical | a freed pointer used again on the CFG |
| `c-double-free` | 415 | critical | `free(p)` reaching another `free(p)` |
| `c-unchecked-alloc` | 690 | medium | `p = malloc()` dereferenced with no null check |
| `c-unsafe-string-api` | 120 | medium | `strcpy`/`strcat`/`sprintf`/`vsprintf` (unbounded) |
| `c-format-string` | 134 | high | printf-family with a non-literal format argument |
| `c-backdoor` | 78 | critical | untrusted input reaching `system`/`exec*` |

## First-run findings (2026-07-11)

**392 findings baselined** (`scripts/safety-baseline.txt`), against Goo `main`.

| Rule | Count | Triage |
| --- | --- | --- |
| `c-unchecked-alloc` | 220 | Mostly real unchecked `malloc`/`calloc` returns (`T *x = malloc(); *x = …`). Genuinely relevant for a no-GC compiler that should handle allocation failure. A subset are *field-target* mis-attributions: `obj->field = malloc()` is tracked as the base `obj` (see Known limitations), giving a misleading variable name. High volume → grandfathered, review in bulk. |
| `c-unsafe-string-api` | 166 | Real unbounded string operations. Whether each is exploitable depends on whether the destination is correctly sized — a per-site review, not an automatic bug. Triage-grade prompts to prefer the `n`-variants. |
| `c-format-string` | 3 | **All true positives.** `snprintf(result, size, fmt, …)` in `comptime/comptime_intrinsics.c` with a *variable* `fmt` — an attacker-influenced format is a real CWE-134 risk. |
| `c-backdoor` | 2 | The compiler invoking its own toolchain — `system(link_command)` (linker) and `execvp(argv[0], argv)` (gcc) in `codegen/codegen.c`. Command-injection *shaped* (a hostile output filename could inject), so worth ensuring `filename` is sanitized, but expected behavior, not a backdoor. |
| `c-use-after-free` | 1 | **True positive.** `concurrency/numa_scheduling.c:151` — `free(topology)` then `if (topology == g_numa_topology)` reads the freed pointer's value in a comparison, which is undefined behavior under C11 §6.2.4. |
| `c-double-free` | 0 | Clean. |

### Highest-value items to fix

1. **`numa_scheduling.c:151`** — null out or capture `topology == g_numa_topology`
   *before* `free(topology)`; comparing a freed pointer is UB.
2. **`comptime_intrinsics.c` variable-format `snprintf`s** — if `fmt` can carry
   user/macro-controlled `%` directives, pass a fixed format and the value as an
   argument.
3. **`codegen.c` `system`/`execvp`** — confirm the output filename reaching the
   link command cannot inject shell/argv.

## Baseline policy

`scripts/safety-baseline.txt` holds accepted findings as `rule-id:dir/relpath:line`
fingerprints. The gate fails only on findings **not** in the baseline. After
fixing or triaging, refresh with:

```sh
scripts/safety-scan.sh --update-baseline
```

Line-based fingerprints churn when code shifts above a finding, so an unrelated
edit can surface a "new" finding that is really a moved old one — review the diff
before committing a refreshed baseline, and prune fingerprints for code you fixed.

## Known limitations

- **Intraprocedural.** A free in one function and a use in another are not
  connected.
- **By-name, no aliasing.** `q = p; free(p); use(q);` is not caught.
- **Field/subscript allocation targets.** `obj->field = malloc()` and
  `arr[i] = malloc()` are attributed to the base identifier (`obj`/`arr`), so
  `c-unchecked-alloc` can name the wrong variable in its message even when it
  points at a real unchecked allocation.
- **Parse gaps.** See the coverage caveat — unparsed C23 constructs are silently
  skipped.
