# Sanitizer configs — recorded evidence

A gate that never fails and a gate that never runs look identical from the
outside. So no config here is trusted until a deliberate defect has been shown
to turn it red, AND shown to leave the build green when the config is off.

Both halves matter. A defect that fails with *and* without the sanitizer proves
only that the code is broken; it says nothing about the sanitizer.

## Verdict

Measured 2026-08-30. Clang 22.1.8, Bazel 9.2.0, Fedora, x86-64.

| Config | Red under its own config | Green under `--config=clang` | Verdict |
|---|---|---|---|
| `--config=asan`  | yes | yes | **has teeth** |
| `--config=ubsan` | yes | yes | **has teeth** |
| `--config=tsan`  | yes | yes | **has teeth** |

THE CONTROL IS `--config=clang`, NOT "no config", and that is a correction.
The first version built the control with no config, which means this file's
default gcc. Two variables then separated the control from the test, the
sanitizer AND the compiler, so a difference could be attributed to neither.

It also failed on CI while passing here. This machine's `/usr/bin/gcc` is
16.2.1 and accepts `-std=c23`; the `ubuntu-24.04` runner's is gcc-13 and
rejects it, so the control would not BUILD there. `verify_sanitizers.sh`
reported it as a TOOL FAILURE with exit 2, which is the right answer: a control
that cannot build is not evidence that a sanitizer lacks teeth.

Reproduce with:

```sh
./tools/verify_sanitizers.sh    # exit 0 = every config has teeth
```

The deliberate defects live in `//testing/teeth`, tagged `manual` so
`bazel test //...` never expands to them and red is never the normal state.
They stay in the tree rather than being deleted after a one-off check, because
a claim in a commit message is not reproducible and a target is.

Whole-suite results on the same day:

```
bazel test //... --config=asan     80 of 80 pass
bazel test //... --config=ubsan    80 of 80 pass
bazel test //... --config=tsan     80 of 80 pass
bazel test //...                   81 of 81 pass
```

The sanitizer runs show 80 rather than 81 because `poison_test` is tagged
`nosan`: it reads freed memory deliberately, which is exactly the defect asan
reports, so the two cannot both run.

## Why clang, when this repo pins gcc

`/usr/bin/gcc` on the development machine cannot link `-fsanitize=address` at
all — `cannot find /usr/lib64/libasan.so.8.0.0`. Clang has the runtimes. Every
sanitizer config therefore pins clang, which inverts `.bazelrc`'s default and
costs a re-analysis when alternating configs. That is the trade.

## THREE LIMITS, and each is load-bearing

**1. src/runtime is NOT instrumented, though the linker gap is now closed.**
`src/codegen/codegen.c` used to hardcode `gcc` to link a compiled Goo program
and pass no sanitizer flag, so an instrumented `libgoo_runtime.a` failed that
link with undefined `__asan_report_*` symbols. Measured: without the opt-out,
46 of 81 tests went red, every one a link failure rather than a defect. The
opt-out is `NO_SANITIZE_COPTS` on each target in `src/runtime/BUILD`, where a
reader will look for it.

`--linker` and `--link-flag` now exist, gated by
`scripts/link_flags_probe.sh`. Measured 2026-08-30 on `hello_world.goo`:

    goo h.goo -o h --linker=clang --link-flag=-fsanitize=thread

links and runs, and the binary carries **160** defined `__tsan_*` symbols
against **0** for the same program with no flag (98 KB to 1.65 MB). asan and
ubsan behave the same way. So the linker can now resolve an instrumented
archive.

What remains is two edits, neither done here: make `NO_SANITIZE_COPTS`
conditional on the sanitizer configs via `select()`, and pass the two flags
from the probe runner. **Until both land, a green tsan run still says nothing
about the goroutine scheduler**, which is where a race would actually live.
Expect the flip to surface real reports in `concurrency.c`, `deadlock.c` and
`sync.c`; that work is unbounded until it is tried.

**2. Leak detection is OFF under asan.** Measured on
`examples/hello_world.goo` with an asan-built compiler: *1928 byte(s) leaked in
38 allocation(s)*, with **zero** frames in LLVM — every one is goolang's own
(`type_checker.c`, `goo.c`, `types.c`, `ast_constructors.c`, `parser.y`). Those
leaks are real, and they are the documented AST-free-leak class, which has its
own gate. A leak is not a memory-safety violation; asan is here for
use-after-free, buffer overflow and double free. Give leaks a floor of their
own before switching `detect_leaks` on.

**3. msan is absent.** It needs every linked library instrumented, glibc and
the host LLVM included, and this build links an LLVM it does not compile.

## What this does and does not replace

These configs sit beside, not instead of, the existing memory gates:
`test-golden-poison` (SQLite's poison-on-free, 495 fixtures),
`ast-free-leak-probe`, `arena-valgrind-probe`, and `obj-header-tsan`.
