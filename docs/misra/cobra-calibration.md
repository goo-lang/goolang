# Cobra calibration results

Spec: [docs/superpowers/specs/2026-09-03-cobra-calibration.md](../superpowers/specs/2026-09-03-cobra-calibration.md).
Status: measured. The track is closed by the user's decision of 2026-11-08 (task 13). No Cobra gate exists. Rule 3.2 has a grep gate instead, `misra-r3-2-probe`.

Every number below has one instrument: Cobra 5.4 (13 April 2026), built from
commit `eebff89637542b0bfdc76739fbad4f6a365c51ae`, measured on 2026-11-08, over the 68
git-tracked `src/*.c` files at repo commit `a42a39d6`. The controls are in
`tests/misra/cobra/`.

## How it ran

- Build: `make` then `make install BINDIR=<prefix>/bin MAN=<prefix>/man1` in
  `src/`, with the prefix inside the scratch directory. Single thread; `-N` was never used.
- Controls: `cobra -terse -f misra2012 <file>`, from a scratch directory,
  with `C_BASE` set to the Cobra `rules/` directory.
- `src/`: `cobra -terse -f misra2012 -F <68 tracked files> -I<repo>/include
  -I<repo> -D_GNU_SOURCE`, again from a scratch directory.
- Proof that Cobra read each control file: rule `M12_R4_6` (it fires on every
  `int`) fired on each control. A run with no `M12_R4_6` finding is a harness
  failure, not a clean file.
- Calibration commands (the scratch Cobra prefix is `$S`; `C_BASE` is `$S/src/rules`):

  ```sh
  export PATH=$S/prefix/bin:$PATH C_BASE=$S/src/rules
  mkdir -p "$S/build/run" && cd "$S/build/run"     # Cobra writes result files into the cwd
  cobra -terse -f misra2012 /abs/path/tests/misra/cobra/r<N>_<M>_bad.c   # then _ok.c
  # findings are in _Misra2012_.txt; count the line "M12_R<N>_<M>, <k> patterns"
  ```

  This runs for each pair `2_1 2_6 3_2 5_3 17_4 18_1 18_2 18_3 18_6` and for the
  four `*_known_bad_report.c` files. Check that the file exists first: a wrong
  path aborts Cobra, and an empty result then reads as "no finding".
- A rule is counted by its pattern-set name (`M12_R<N>_<M>` in `ps list`), not
  by caption text. Reason: see "Defects found" below.

## Table

Controls: the violating control must be reported (V), and the compliant twin
must not be (C). The count is the number of patterns Cobra reports on `src/`.
The rate is incorrect over hand-checked, on the first ten reports in file order.

| Rule | V | C | Count on `src/` | Hand-checked | Incorrect rate | Verdict |
|---|---|---|---|---|---|---|
| 1.3 | not run | not run | 0 | 0 | n/a | dropped by spec section 2 (author doubts the block) |
| 2.1 | FAIL | pass | 29 | 10 | 10/10 | cannot see |
| 2.6 | FAIL | pass | 1 | 1 | 1/1 | cannot see |
| 3.2 | pass | pass | 0 | 0 | n/a (no reports) | passes both controls; nothing to gate |
| 5.3 | FAIL | pass | 0 | 0 | n/a (no reports) | cannot see |
| 17.4 | FAIL | pass | 0 | 0 | n/a (no reports) | cannot see |
| 18.1 | FAIL | pass | 87 | 10 | 10/10 | cannot see |
| 18.2 | FAIL | pass | 1 | 1 | 1/1 | cannot see |
| 18.3 | FAIL | pass | 0 | 0 | n/a (no reports) | cannot see |
| 18.6 | FAIL | pass | 2 | 2 | 2/2 | cannot see |

Pass on both controls: 1 rule of 9 (rule 3.2). A "pass" for the compliant
twin of a rule that fails its violating control carries no weight: a tool that
reports nothing passes every compliant twin.

### The four known incorrect reports (negative controls of the harness)

Each produced a finding, and the hand check classes each as incorrect. The
harness can therefore record "incorrect".

| Control file | Finding | Why incorrect |
|---|---|---|
| `r2_1_known_bad_report.c` | M12_R2_1 | `do { } while (0)` in a macro is the idiomatic compliant form |
| `r18_6_known_bad_report.c` | M12_R18_6 | the buffer is `static` inside the function |
| `r13_5_known_bad_report.c` | M12_R13_5 | `strchr` has no side effect; the `-+` in its string literal reads as `++`/`--` |
| `r13_6_known_bad_report.c` | M12_R13_6 | `sizeof x` has no side effect |

### Hand checks on `src/`

- 2.1, 10 of 29: all ten are `ENTRY_CLEANUP();` in `src/compiler/goo.c`
  (lines 1139, 1144, 1157, 1171, 1176, 1216, 1234, 1254, 1291, 1312). The
  macro ends in `while (0)`. All 29 are `ENTRY_CLEANUP` or
  `PKG_FILE_SCOPE_CLEANUP` uses. Incorrect 10/10. Without `-cpp` the count is 2.
- 2.6, 1 of 1: `src/codegen/function_codegen.c:3113` `fallback:;`. Line 3076
  has `goto fallback`, so the label is used. Incorrect.
- 18.1, first 10 of 87: `codegen.c:187`, `:315`, `:1646`, `:2077`
  (`strstr`/`strrchr` calls), `composite_codegen.c:1346` (a cast), `goo.c:444`,
  `:449`, `:546`, `:850`, `:1072` (`strrchr`/`strchr` calls). None has
  arithmetic that leaves its array. `goo.c:449` carries `slash + 1` on the
  next line, which stays inside the string. Incorrect 10/10. The pattern is
  `( .* * ) ( .* + .* )`: it matches a cast followed by a plus, not a bound.
- 18.2, 1 of 1: `src/runtime/runtime.c:71`,
  `(unsigned char*)payload - GOO_OBJ_HEADER_SIZE`. That is pointer minus
  integer, not pointer minus pointer. Incorrect for 18.2.
- 18.6, 2 of 2: `src/package/import_resolver.c:40..66` and
  `src/codegen/codegen.c:1637..1657`. Both return a function-local `static
  char buf[4096]`, which persists. Incorrect (the known incorrect report).

One reader made these classifications. The finding files are in the task 12
evidence directory so that a second reader can disagree.

### Why the violating controls fail

Cobra's `misra2012.def` does not implement these rules the way the
spec's control table describes them. Supplementary controls in the shape
each block really matches did fire (measured the same day):

| Rule | What the block matches | Supplementary result |
|---|---|---|
| 2.1 | `if (0)`, `while (0)`, `for (;0;)` | `if (0)` reported, twin clean. Statements after `return` are not seen. |
| 2.6 | labels; the script searches for `?` and `goto` | a file with a `?` and no `goto` was silent. With a `goto`, the label that a `goto` targets was reported too (twin not clean). |
| 5.3 | `m97_rule21` script | silent on both a bare block and an `if` block. |
| 17.4 | a bare `return;` in a non-void function | `return;` reported, twin clean. A missing return is not seen. |
| 18.1 | `( T * ) ( x + y )`, a cast then a plus | reported, twin clean. A loop that walks past the end is not seen. |
| 18.2 | `( T * ) ( x - y )` | reported only with `int *` in the cast. Plain `p - q` is not seen. |
| 18.3 | `( T * ) ( x < y )` | reported, twin clean. Plain `p < q` is not seen. |
| 18.6 | local declared, then `return &local` or `return array` | reported, twin clean. A store to a static is not seen. |

These are pattern checks on token shape. None tracks a value or an object.
That matches the spec's stated risk: Cobra is a tokeniser, not a front end.

### Tier 2 (the review-only Rules with no Cobra definition)

| Rule | Cobra definition | Could a structural analyser express it |
|---|---|---|
| 8.13 | none | plausible |
| 12.5 | none | plausible |
| 13.2 | none | needs semantics a token pattern cannot see |
| 14.3 | none | plausible |
| 17.5 | none | needs semantics a token pattern cannot see |
| 20.6 | none | plausible |
| 21.13 | none | plausible |
| 22.1 | none | needs semantics a token pattern cannot see |
| 22.3 | none | needs semantics a token pattern cannot see |

2.6, 17.4 and 18.3 are in the table above through tier 1.

## Second instrument: `rules/refmod/func_patterns.cobra`

Both run over the 68 tracked `src/*.c` files on 2026-11-08.

| Instrument | Version | Count |
|---|---|---|
| `scripts/pthread_return_probe.sh` (task 1 ratchet) | repo `a42a39d6` | 40 unchecked `pthread_*` returns, 6 files |
| `func_patterns.cobra`, `pthread_*` rows only | Cobra `eebff896` | 1 (`pthread_create`, `src/runtime/concurrency.c:112`, "1 out of 2 times") |
| `func_patterns.cobra`, all functions | Cobra `eebff896` | 47 functions, 525 unchecked call sites |

Reading: the two disagree by 39 calls, and the disagreement is by design.
`func_patterns.cobra` reports a function only when some calls check the return
value and some do not (`val_used > 0 && val_not_used > 0`). A function that
nothing ever checks (`pthread_mutex_lock`, `pthread_cond_wait`, `pthread_join`)
is silent. The ratchet counts those. The two instruments answer different
questions, so the disagreement is not a bug in either one. The Cobra script
cannot replace the ratchet. It runs in 0.3 s.

## Defects found in the documents and in Cobra

- Spec section 3 says rule 18.6 findings carry the caption `M12_R18_5`. The
  measured fact is the reverse. In `ps list`, set `M12_R18_5` (real rule 18.5
  findings) carries rule 18.6's caption text, and set `M12_R18_6` carries the
  old rule 18.6 text from `misra1997.def`. Counting by caption text would
  attribute rule 18.5 findings (4 here) to rule 18.6. Counting by set name
  gives 2.
- With `-I` or `-D` options Cobra preprocesses the files (`-cpp`). In that mode it
  skips `#define` rules (it prints "error: -cpp removes preprocessing
  directives") and a file that does not preprocess is reported on stderr:
  `src/runtime/far_transport.c` (no `nng/protocol/pair1/pair.h`) and one file
  that includes `src/parser/parser.tab.h` by a relative path. Without these
  options the counts differ widely (2.1: 2, 18.1: 13).
- `find src -name '*.c'` returns the ignored generated file
  `src/parser/parser.tab.c`. It added 7 rule 2.1 patterns, 71 rule 18.1 and 62
  rule 18.2 patterns, all reported at `parser.y` line numbers. The file list
  must come from `git ls-files`.
- A wrong input path makes Cobra abort on an assertion (`add_file:
  read_stdin`), and a harness that only greps for findings then reads "no
  finding". A harness must test that the file exists and that a known rule
  (4.6 here, which fires on every `int`) fires.

## Decision (task 13)

Closed on 2026-11-08 by the user: no Cobra gate, Cobra not vendored, and a grep
gate for Rule 3.2 (`misra-r3-2-probe`, `scripts/misra_r3_2_probe.sh`).

## Decision input for task 13

By the spec's letter (section 6), the spike passes: one rule, 3.2, passes both
controls. That rule has 0 reports on `src/`, and `git ls-files 'src/*.c' | xargs
grep -n '//.*\\$'` also returns 0, so a gate on it would guard a rule the tree
already satisfies, and one `grep` expresses the same check. Every rule that
reports on `src/` (2.1, 2.6, 18.1, 18.2, 18.6) fails its violating control and has an
incorrect-report rate of 100% on the sample. No rule meets the adoption bar of
2 incorrect in 10 with a non-empty report set.
