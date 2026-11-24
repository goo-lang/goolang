# Verification tooling: the SPIN, Cobra and Power of Ten tasks

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for tracking. Each task is one commit.

**Goal:** On 2026-09-02 we examined SPIN, Cobra, the Power of Ten and the GrammaTech tools. This plan turns those results into gates with teeth, or into records with a decision. Nothing stays as an opinion in a chat record.

**Sequence of value:** The pthread return ratchet comes first. It has no new tool, and it applies to the largest single slice of the MISRA backlog. The SPIN model comes second, because it can find a defect in the deadlock detector, or show that there is none. The Power of Ten record and the diffoscope report are small. The Cobra track is a spike, and the spike makes the decision on a gate.

**Track F** is the spine that the JPL slide shows: one capture of the compiler calls, one database of results, one review surface. It can start in parallel with track A. Tasks 15 and 17 come after tasks 1 and 2.

**Track G** comes from the talk. Holzmann names two rules to keep if you keep only two: assertions, and 0 warnings with the build stopped on one warning. Holzmann names metrics as the most important practice of all. Tasks 21 and 23 are cheap gates on numbers measured today, and they can start immediately.

**Spec sources:** the four messages of 2026-09-02, `docs/superpowers/specs/2026-06-28-m8c-mt-scheduler-design.md` section 8a, `docs/misra/README.md`, `docs/misra/enforcement-plan.md`.

## Decisions

**1. Where a tool comes from.** No gate reads the network. CI runs one target, `make verify-core`. Fedora has no package for SPIN and no package for Cobra. Ubuntu 24.04 has a package for SPIN only.

| Alternative | For | Against |
|---|---|---|
| (a) Vendor a pinned source tarball in `third_party/` and build it in the probe | A clean checkout works, and CI has teeth. Precedent: `nng-1.12.0.tar.gz` | The repo becomes larger by about 1 MB for each tool, and the gate pays a build of 6 s to 10 s |
| (b) The tool must be on PATH, and the gate stays out of `verify-core` | Nothing is vendored | The gate does not run in CI, which is the condition of the cppcheck MISRA gate today |
| (c) Install the tool with apt in CI | Nothing is vendored | Fedora has no package, so a local pre-push run skips the gate |

Decision: (a) for SPIN. The Cobra spike (task 12) makes the decision for Cobra.

**2. The companion edits for a new gate.** Each new probe costs three edits in the same commit, or `verify-core` goes red:

- a `--self-test` in the script, because `probe-teeth-probe` examines each `scripts/*.sh`
- an increase of the count in `tools/parity-gate-count.txt` (224 today) for each new `VERIFY_ALL_DEPS` entry
- a regenerated `tests/probes/census.txt` where the base branch has it, because the census reads the Makefile. Do not edit the census by hand. On `main` today the census is not merged, so this edit applies after phase 4 merges.

**3. The Bazel branch owns the Bazel files.** Phase 4 task 14 is not complete. A new probe goes into the Makefile and `verify-core` only. Its `goo_script_probe` declaration is a different commit after task 14 merges, so this plan touches no file of that branch. The regenerated census is the one file that the two share.

**4. Branches.** The base is `main` at de24a23, the phase 3b merge. The phase 4 work is on `build/bazel-phase4-probes` and is not in `main`.

Each track starts from the latest `main` in its own worktree. Tracks A, C, D and F can run in parallel. Track B is sequential. Track E runs after track A, because the two tracks edit the MISRA docs.

**5. The integration shape, from JPL.** The Holzmann slide shows one pipeline:

- the build log gives all compiler calls
- each analyser reads that one list
- all results go into one database, and the suppressed warnings stay in it
- one review tool shows the results.

This repo has the parts and not the spine. The Makefile and the census are the build log. The cppcheck gate has a baseline and a suppression list, for cppcheck only. Each probe prints its own verdict, and nothing collects them.

| Alternative for the compiler-call capture | For | Against |
|---|---|---|
| (a) Parse the output of `make -Bnw` into `compile_commands.json` | No new tool. A derivation with a regeneration gate, the census pattern | `make -n` runs the `$(shell)` functions, and a recipe that hides its command line is missed |
| (b) `bear -- make` | The commands of the build itself | One more package on each machine and in CI |
| (c) Bazel `hedron_compile_commands` | Correct for Bazel | Bazel is not the authority until phase 4 closes |

Decision: (a), and (b) is the check that (a) is complete. The two lists must agree on the set of translation units.

**6. A rule with no checker has no effect.** The talk measured it: the rules that JPL developers agreed with most were the rules they did not follow, because nobody examined the code. So the Power of Ten record in task 10 names a checker for each rule, or the word unchecked. Rules 5, 7 and 10 get gates in this plan, in tasks 23, 1 and 21.

**7. Metrics first.** The talk puts the collection of defect metrics above all other practices: in which phase a defect was made, in which phase it was found, and how far it travelled. This repo records fixes in commits and PRs and not in a table. Task 25 starts the table.

**Measured on 2026-09-02, gcc 16.2.1, a full rebuild of `bin/goo`:**

| Measurement | Value |
|---|---|
| Warnings with `-Wall -Wextra -std=c23` | 85, of which 77 from C and 8 from bison |
| Warnings with `-Wpedantic` added | 88, of which 80 from C |
| Largest sources | `include/runtime.h` 29, `src/runtime/concurrency.c` 13, `src/codegen/lowlevel_codegen.c` 9 |
| Largest kinds | unknown pragmas 12, deprecated declarations 11, unused variables 7 |
| Assertion lines in `src/`, generated files not included | 23 |
| Executable lines in `src/` | 40,417 |
| Assertion density | 0.057 percent, against the target of 2 percent, which is 808 assertions |
| `GOO_ASSERT` in the shipped compiler | compiled out, on only with `GOO_DEBUG` |

CI builds with gcc-14, so the warning set there can be different. The gate in task 21 names the compiler version, and CI is the authority.

## Global constraints

- Each probe prints the artefact it read in its verdict line, for example the file count and the tree.
- Each probe has a positive control and a negative control, and the self-test shows the negative.
- Each number in a doc has its instrument, the instrument version and the date.
- A doc says that a gate is there only when the gate is there. `doc_claims_probe.sh` examines this.
- Work: S is less than half a day, M is a day, L is more than a day.

## Tasks

### A. The pthread return ratchet (Power of Ten rule 7, MISRA Rule 17.7)

- [x] **1. The counter and its baseline.** `scripts/pthread_return_probe.sh` counts each call to a `pthread_*` function that discards the return value. The scope is `src/*.c` without the generated parser files. It writes `scripts/pthread-return-baseline.txt`, one line for each file, shrink-only, in the shape of `probe-teeth-baseline.txt`. The script has a `--self-test`. The self-test puts one unchecked `pthread_mutex_lock` into a copy of a source file, and the result must be red. Then it puts a checked call in, and the result must be green. Task 2 connects the script to the Makefile.
      *Accepts:* the verdict line names the tree and the file count. A count that increases in a file exits 1. The header of the baseline records the total of the probe next to the 840 of cppcheck. It gives the cause of the difference. A call on more than one line is counted, or the header records the miss. The self-test shows the two directions.
      *Work:* M.

- [x] **2. Connect the ratchet.** Makefile targets `pthread-return-probe` and its self-test, the two in `VERIFY_ALL_DEPS`. Increase the count in `tools/parity-gate-count.txt`. Regenerate `tests/probes/census.txt`.
      *Accepts:* `make verify-core` is green. In that run, `probe-teeth-probe`, `tools/parity_test.sh` and `census-current-probe` all pass.
      *Work:* S.

- [x] **3. Record the enforcement.** In `docs/misra/README.md`, the Rule 17.7 row and D-02 name the ratchet as the enforcement for the pthread slice. In `docs/misra/enforcement-plan.md`, the same.
      *Accepts:* `doc_claims_probe.sh` passes. The text gives the baseline total with the date.
      *Work:* S.

### B. The SPIN model of the deadlock detector

- [x] **4. Vendor SPIN.** `third_party/spin-<tag>.tar.gz` with its sha256 in `third_party/README.md`, and `scripts/spin_bootstrap.sh`. The bootstrap uses `spin` from PATH when its version is 6.5 or newer. If not, it builds the tarball into `build/tools/spin`. It has a `--self-test`.
      *Accepts:* with no `spin` on PATH, the bootstrap builds it, and `spin -V` prints the pinned version. An incorrect sha256 exits non-zero with a message that names the file. The self-test shows the incorrect-sha256 path.
      *Work:* S.

- [x] **5. The model of the protocol as it is.** `tests/models/deadlock_detector.pml`. It models `goo_scheduler_wait`, `goo_sched_block_begin` and `goo_sched_block_end`, one channel, and N goroutines with N from 2 to 4. The streak threshold is a parameter. The claim: an abort occurs only in a deadlock, that is, when no goroutine is signalled and not awake. The hypothesis to test: an abort with no deadlock is possible for each finite threshold. The cause is that the wake interval has no limit in the model.
      *Accepts:* `spin -a`, `cc -DSAFETY`, and `./pan` complete for each N. The result is recorded in each condition. When there is a counterexample, the trail file is committed with an explanation in prose. The number of states and the run time are recorded. A positive control: with the detector disabled, `pan` reports 0 errors. That shows that the model does not fail by itself.
      *Risk:* a model made to confirm a belief. The positive control and the teeth check in task 6 are the answer.
      *Work:* M.

- [x] **6. The model of the corrected protocol.** The candidate correction moves the blocked-count decrement to the signaller, in the scheduler mutex, at signal time. If the model of task 5 points at a different correction, model that one. Prove the claim with threshold 1.
      *Accepts:* `pan` reports 0 errors in full mode, not bitstate, for N = 2, 3 and 4. The number of states is recorded. Teeth: a copy of the model with the wake-time decrement put back makes `pan` report an error.
      *Work:* M.

- [x] **7. Connect the model as a gate.** Makefile target `spin-deadlock-model-probe` in `VERIFY_ALL_DEPS`. It runs the corrected model, and 0 errors are necessary. Then it runs the as-is model as a negative control, and the error is necessary. Make the three companion edits from decision 2.
      *Accepts:* `make verify-core` is green. The probe runs in less than 30 s on this machine, bootstrap not included.
      *Work:* S.

- [ ] **8. Apply the correction to the runtime, test first.** A C test makes the abort with no deadlock occur each time. A test hook makes a pause between the signal and `goo_sched_block_end`. The test is red before the correction. Then change `src/runtime/concurrency.c` and `src/runtime/channels.c` as task 6 proved. Set the poll threshold to the value the model proved, or remove it. Update section 8a item 1 of the M8c design doc.
      *Accepts:* the new test is green. `mt-scheduler-stress` and the fanin stress are green for a minimum of 50 iterations at N = 4. valgrind reports 0 errors. `make verify-core` is green.
      *Risk:* the highest in the plan. The change touches the scheduler on the default path. The stress gates and the model are two instruments with no shared part.
      *Work:* L.

- [ ] **9. Optional: the select liveness model.** `tests/models/select_poll.pml` for `goo_select`, which polls with a sleep and sets no wake. Properties: a select with a ready case fires at some time, and no case starves with a fair scheduler.
      *Accepts:* the result is recorded. A counterexample, if there is one, goes into section 8a of the M8c design doc.
      *Work:* M.

### C. The Power of Ten record

- [x] **10. Write the record.** `docs/misra/power-of-ten.md`, with a link from the MISRA README. For each of the ten rules: the rule, the measured count on `src/`, and a decision. The instrument is the `p10` script of Cobra 5.4 on 2026-09-02. Rule 10 is not met: the flags are on, and a full rebuild prints 85 warnings, so it is a target until task 22 closes. Rules 5 and 7 are targets, and each names the work that applies to it: task 23 and task 1 of this plan. Rules 1, 3 and 4 are declined by design, each with the cause or the deviation record. The record names the two instrument defects: the `expr` syntax error in the script, and rule 6, for which the `scope_check` helper is necessary. It gives the command that measures the numbers again. For each rule it names the checker, or the word unchecked, as decision 6 says.
      *Accepts:* each rule has a decision and a checker column. Each number has its instrument and date. The doc says that there is no gate, and `doc_claims_probe.sh` passes.
      *Work:* S.

### D. A report on a reproducible-build mismatch

- [x] **11. Name the different member.** In `scripts/repro_build_probe.sh`, when `cmp` fails, run `diffoscope` when it is available, with a limit on its output. If it is not available, run `llvm-objdump -d` for the binary, and compare the archive member by member. Print the first different member or section. The cmp verdict and the two hashes stay as they are.
      *Accepts:* the current `--self-test`, which puts a nonce into `runtime.o`, prints `runtime.o` as the different member. With no tool available, the probe continues to fail on cmp. No new dependency is necessary, because `llvm-objdump` comes with `llvm-dev` in CI.
      *Work:* S.

### E. The Cobra spike

- [ ] **12. Calibrate the ten rules that cppcheck cannot examine.** The spec file is `docs/superpowers/specs/2026-09-03-cobra-calibration.md`. For each rule, make a minimal C file that violates the rule. Cobra must report it. Make a minimal compliant file of the same shape. Cobra must not report it. Then record the count on `src/`. Do a hand check of the first ten reports. Run Cobra from a scratch build pinned by commit SHA, with a single thread. Run it from a build directory, because Cobra writes result files into the working directory. Build the back-end tools too, with `make install` in `src/`, because rule 8.9 calls `scope_check`. The Cobra book, second edition, says that `-N` parallelism serves the token read and interactive queries. That agrees with the `-N` hang on a rule file. Use `rules/refmod/func_patterns.cobra` as a second instrument next to the pthread ratchet of task 1. The book gives it in section 9.5, and it finds return values that are not tested. Record the two counts side by side.
      *Rules:* `1.3 2.1 2.6 3.2 5.3 17.4 18.1 18.2 18.3 18.6`.
      *Accepts:* the four known incorrect reports are negative controls, and they fail. Two are the static buffer for 18.6 and the `while (0)` for 2.1. Two are the strchr call for 13.5 and the `sizeof` with no parentheses for 13.6. The spec ends with a table, one row for each rule. Each row gives pass or fail for the two controls, and the incorrect-report rate on the sample.
      *Work:* M.

- [ ] **13. Make the decision, then do the work.** If a minimum of one rule passes the two controls, vendor Cobra as in decision 1(a). Add `cobra-misra-probe` for those rules with a shrink-only baseline, and make the three companion edits. If no rule passes, close the track. In the two conditions, add the outcome to the "Known limits of the checker" section of the MISRA README. Record the `-N` hang there.
      *Accepts:* the decision is in the README with a link to the spec. If a probe is added, `make verify-core` is green, and the probe runs single-threaded from `build/`.
      *Work:* S to M.

### F. The spine: one capture, one database, one review surface

- [ ] **14. Capture the compiler calls.** `tools/gen_compile_commands.sh` derives `build/compile_commands.json` from the Makefile. A regeneration gate, `compile-commands-current-probe`, keeps it a derivation. Make the three companion edits.
      *Accepts:* each `src/*.c` in `GOO_OBJS` is in the list one time, with the flags and include paths of the build. The entry count equals the object count in the Makefile. Positive control: remove one entry, and the gate goes red. A `bear -- make` run on this machine gives the same set of translation units.
      *Work:* M.

- [ ] **15. Give cppcheck the capture.** `scripts/misra-scan.sh` reads `--project=build/compile_commands.json` and not the `find src` list.
      *Accepts:* the Rule 17.3 count, 660 today, is measured again and recorded. The baseline is regenerated, and no count increases.
      *Work:* S.

- [ ] **16. Add the gcc strict lane.** A Makefile target compiles each captured translation unit with `-Wall -Wextra -Wpedantic -fanalyzer`, with SARIF output to `build/sarif/gcc/`. clang-tidy is the second lane, with the same output shape. Make the three companion edits.
      *Accepts:* a positive control file with a null dereference gives one analyser result in SARIF. The result count on `src/` is recorded with the gcc version.
      *Work:* M.

- [ ] **17. One results database.** `tools/sarif_merge.py` merges the SARIF of cppcheck, gcc and clang-tidy into `build/findings.sarif`. A converter adds the probe verdicts and the pthread ratchet in the same format. One shrink-only baseline, with rule, file and fingerprint as the key, replaces the per-tool baselines. It is a generalisation of `docs/misra/tools/gate.py`. One suppression list. Each entry must name a deviation record or a checker defect, which is the policy of `misra-suppressions.txt` today.
      *Accepts:* the merged file validates against the SARIF 2.1.0 schema. A suppression with no justification makes the gate red. A count that increases makes the gate red. The verdict line prints the counts and the artefact.
      *Work:* L.

- [ ] **18. The review surface.** The repo is public, so GitHub code scanning accepts the SARIF upload at no cost. A workflow step uploads `build/findings.sarif` on each push to `main` and on each PR.
      *Accepts:* the Security tab shows a result at its source line. A PR shows the new results only. Suppressed results show as suppressed, not removed.
      *Work:* S.

- [ ] **19. The nightly lane.** A scheduled workflow runs `make verify` and the heavy analysers, and uploads the SARIF.
      *Accepts:* the workflow runs on a schedule. Its last run is green, read from `gh run view --json conclusion`, not from a pipe.
      *Work:* S.

- [ ] **20. Optional: Coverity Scan.** Coverity Scan is free for a public repo. It is the nearest thing to the coverity lane on the slide. Add the repo to Coverity Scan, and add its results to the database of task 17.
      *Accepts:* one scan completes, and its results are in `build/findings.sarif` with the tool name.
      *Work:* S.

### G. Metrics and the build, from the talk

- [x] **21. The warning-count gate.** `scripts/warning_count_probe.sh` rebuilds `bin/goo` from clean into its own build directory, with the flags of the Makefile. It counts the C warnings and the bison warnings as two numbers. The baseline is shrink-only. The verdict names the compiler version and the flags. Make the three companion edits.
      *Accepts:* the count today is 77 C warnings and 8 bison warnings with gcc 16.2.1. One new warning makes the gate red. The self-test puts one unused variable into a copy and shows red. The bison count belongs to the grammar tripwire, and the gate records that, not a second count.
      *Work:* M.

- [ ] **22. No warnings, then stop the build on one.** Remove the 77 C warnings, one kind for each commit, largest kind first. When the count is 0, add `-Werror` to `CFLAGS` and to the CI build. Then add `-Wpedantic`, remove its 3 warnings, and keep it.
      *Accepts:* a full rebuild prints no warning. `make verify-core` is green with `-Werror`. CI is green with gcc-14, read from `gh run view --json conclusion`.
      *Work:* L.

- [x] **23. The assertion-density gate.** `scripts/assertion_density_probe.sh` counts the assertion lines and the executable lines in `src/`, generated files not included, with the counting rule in its header. The baseline can only increase. The verdict prints the density and the two counts. Make the three companion edits.
      *Accepts:* the number today is 23 of 40,417, which is 0.057 percent. A removed assertion makes the gate red, and the self-test shows that. The target, 2 percent, is in the header with the talk as the source.
      *Work:* S.

- [x] **24. Assertions on in the shipped compiler.** `GOO_ASSERT` is compiled out unless `GOO_DEBUG` is defined. The talk keeps assertions on in flight. Measure the golden suite time with `GOO_DEBUG` on and off, in one sitting. If the cost is below 2 percent, make `GOO_ASSERT` on by default, with a kill switch. Record the decision in the MISRA README.
      *Accepts:* the two times are given from one sitting. The decision is recorded. If the default changes, `make verify-core` is green with it.
      *Work:* S.

- [x] **25. The defect ledger.** `docs/defects.tsv` has one row for each corrected defect. Fill in the last 20 `fix:` commits. Add one line to the PR template that asks for the row. There is no PR template today, so create `.github/PULL_REQUEST_TEMPLATE.md`.
      *Columns:* `date  pr  phase_made  phase_found  instrument  gate_added`. The two phase columns take `spec`, `design`, `code` or `test`. The instrument column takes a gate name or `none`.
      *Accepts:* the table has 20 rows with each column filled. A summary by phase pair is at the top of the file. It gives the count of defects that went on after the phase that made them.
      *Work:* M.

## Out of scope

- The GrammaTech tools. None reads an artefact that the gates of this repo read today. DDisasm and GTIRB read the binaries that the linker makes. The one binary reader here is task 11, and diffoscope does that work. SEL is a mutation engine in Common Lisp. Mull is the lighter first candidate if broad mutation tests become a goal. Tbdisasm and Mnemosyne do not apply.
- The 840 unchecked pthread returns themselves. Task 1 stops the count from an increase. The fix backlog in the MISRA README owns the decrease.
- A Bazel `goo_script_probe` for each new probe. That is one commit after phase 4 task 14 merges.

## Risks

| Risk | Mitigation |
|---|---|
| The SPIN model proves what we believe today | The positive control in task 5, the teeth check in task 6 |
| Task 8 causes a regression in the scheduler | The stress gates at N = 4, valgrind, and the model as a second instrument |
| Census and parity churn collides with the Bazel branch | Rebase on `main`, regenerate, do not hand-edit |
| Vendored tarballs make the repo larger | One tarball for each tool, about 1 MB, with sha256 |
| diffoscope output fills the log | Limit the lines, print the first different member only |
| The results database becomes a list that nobody reads | A PR gate on new results only, and a baseline that can only shrink |
| `make -n` misses a translation unit | The `bear` check in task 14 |
| gcc-14 in CI and gcc 16 here give different warning sets | The gate names the compiler, and CI is the authority |
| The 77 warnings hide a defect that a correction changes | One kind for each commit, and `make verify-core` after each |
