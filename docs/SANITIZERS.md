# Sanitizer configs — recorded evidence

A gate that never fails and a gate that never runs look identical from the
outside. So no config here is trusted until a deliberate defect has been shown
to turn it red, AND shown to leave the build green when the config is off.

Both halves matter. A defect that fails with *and* without the sanitizer proves
only that the code is broken; it says nothing about the sanitizer.

## Verdict

Measured 2026-08-30. Clang 22.1.8, Bazel 9.2.0, Fedora, x86-64.

| Config | Red under its own config | Green without it | Verdict |
|---|---|---|---|
| `--config=asan`  | yes | yes | **has teeth** |
| `--config=ubsan` | yes | yes | **has teeth** |
| `--config=tsan`  | yes | yes | **has teeth** |

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

**1. src/runtime is NOT instrumented.** `src/codegen/codegen.c` hardcodes `gcc`
to link a compiled Goo program and passes no sanitizer flag (`grep -c
fsanitize` returns 0). An instrumented `libgoo_runtime.a` fails that link with
undefined `__asan_report_*` symbols. Measured: without the opt-out, 46 of 81
tests go red, every one a link failure rather than a defect. The opt-out is
`NO_SANITIZE_COPTS` on each target in `src/runtime/BUILD`, where a reader will
look for it.

So **a green tsan run says nothing about the goroutine scheduler**, which is
where a race would actually live. Closing the codegen link-flag gap is what
would extend the sanitizers to the runtime.

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
