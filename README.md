# Goo

**A compatible superset of Go — and an open experiment in how much of a real compiler large language models can build.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Language: C23](https://img.shields.io/badge/Language-C23-blue.svg)](https://en.cppreference.com/w/c/23)
[![Backend: LLVM 22](https://img.shields.io/badge/Backend-LLVM%2022-orange.svg)](https://llvm.org/)

Goo keeps Go's syntax and semantics as a faithful base, then adds the power
features Go left out — positioned as *"the C++ to Go's C."* Your Go compiles
unchanged; you opt into the extras where you want them. Underneath the familiar
surface is a different engine: **no garbage collector**, an ownership-based
runtime inspired by Rust and Zig, Zig-style `comptime`, and serious parallelism
ambitions.

Much of this compiler — and this README — was written by AI. Goo is a research
experiment, not a production language (yet).

> Goo is an independent project. It is **not** affiliated with, sponsored by, or
> endorsed by Google or the Go project. "Go" and the Go gopher are trademarks of
> Google LLC.

## What Goo adds

| Feature | Syntax | State |
| --- | --- | --- |
| **Error unions** — typed failure in the signature | `func f() !int` + `catch` | ✅ works |
| **Nullable types** — explicit optionality | `var x ?int` + `if let` | ✅ works |
| **Compile-time execution** — Zig-style | `comptime const N = fib(10)` | ✅ works |
| **Interfaces** — dispatch, embedding, type switches/assertions on `any` | Go syntax | ✅ works |
| **Concurrency** — goroutines, channels, `select` | Go syntax | ✅ works |
| **Ownership / no-GC runtime** — move semantics, escape analysis | inspired by Rust & Zig | 🚧 emerging |
| **Parallelism beyond CSP** — actors (Erlang), messaging (ZeroMQ), data-parallel loops (OpenMP) | — | 🗺️ roadmap |

## Quick start

Prerequisites: a **C23** toolchain (GCC/Clang), **LLVM 22**, **Bison**, and
**Make**. (`Go` is optional — only needed to regenerate the stdlib-coverage
report.)

```bash
git clone https://github.com/goo-lang/goolang.git
cd goolang
make lexer          # builds the compiler -> bin/goo
```

Write a program:

```goo
// hello.goo
package main

import "fmt"

func greet(name string) !string {   // !string = a string, or an error
    return "hello, " + name
}

func main() {
    msg := greet("Goo") catch err {
        fmt.Println("error:", err)
        return
    }
    fmt.Println(msg)                 // hello, Goo
}
```

Compile and run it:

```bash
./bin/goo hello.goo -o hello && ./hello
# hello, Goo
```

## Language at a glance

All examples below compile and run today. See the `examples/` directory for the
full, tested corpus.

### Error unions — `!T`

A function returns a value *or* a typed error; `catch` is the only way through.

```goo
func fetch() !int {
    return 42
}

x := fetch() catch err {
    fmt.Println("failed:", err)
    return
}
fmt.Println(x)   // 42
```

### Nullable types — `?T`

An optional value the compiler makes you unwrap.

```goo
var port ?int = 42

if let p = port {
    fmt.Println("using", p)
} else {
    fmt.Println("no port set")
}
```

### Compile-time execution — `comptime`

Real code runs at compile time; the result is baked into the binary. Goo's own
evaluator handles cases LLVM won't fold on its own, like recursion.

```goo
func fib(n int) int {
    if n < 2 {
        return n
    }
    return fib(n-1) + fib(n-2)
}

comptime const FIB10 int = fib(10)   // evaluated at compile time -> 55
```

### Interfaces and type switches

```goo
func describe(x interface{}) {
    switch v := x.(type) {
    case int, string:
        fmt.Println(v)
    case nil:
        fmt.Println("nil")
    default:
        fmt.Println("other")
    }
}
```

### Concurrency

Goroutines and channels, in Go's style.

```goo
func worker(ch chan int) {
    ch <- 20
    ch <- 22
}

func main() {
    ch := make(chan int, 2)
    go worker(ch)
    a := <-ch
    b := <-ch
    fmt.Println(a + b)   // 42
}
```

## Standard library

Goo vendors real Go standard-library source and shims the rest. Coverage is
**measured, not estimated** — `scripts/stdlib-coverage.py` scores supported
symbols against Go's own API manifest (`$GOROOT/api/go1*.txt`) and writes
`docs/stdlib-coverage.json`.

```bash
make stdlib-coverage
```

At last run: **0.68%** of the Go standard library by exported symbol (77 / 11,286),
across 8 packages — `math/bits` (88%) leading, then `unicode/utf8`, `errors`,
`strings`, `fmt`, `strconv`, `math`, `os`. Small by design, and climbing as the
compiler grows.

## Status

Goo is a **working compiler** — it lexes, parses, type-checks, and lowers to
LLVM IR and native binaries — but it is **early and not production-ready**.

**Runs today:** functions, structs, methods, multi/named returns; interfaces
with dynamic dispatch, embedding, and type switches/assertions on `any`; slices,
maps, arrays, closures, iota; error unions and nullable types; `comptime`;
goroutines, channels, and `select`; a growing stdlib subset (fmt, strings,
strconv, math/bits, unicode/utf8, errors, os); LLVM codegen to native binaries.

**In progress (v1):** broader standard library; generics for ordinary functions;
the deterministic no-GC runtime (ownership and escape analysis); parallelism
beyond CSP; remaining corners of Go syntax.

## Building and testing

```bash
make lexer                   # build the compiler -> bin/goo
make test                    # unit test suite
make verify                  # full gate: probes + golden suite (+ CompCert link check)
bash scripts/run_golden.sh   # 273 golden programs, compiled + run + diffed
```

The golden suite (`examples/<name>.goo` with a sibling `.expected.txt`) is the
primary regression net: each program is compiled with `bin/goo`, run, and its
output diffed against the expected file.

## Project structure

```
src/
  lexer/      Lexical analysis
  parser/     Grammar (Bison) and AST construction
  ast/        Abstract syntax tree
  types/      Type system and type checking
  comptime/   Compile-time evaluation engine
  codegen/    LLVM IR code generation
  runtime/    Runtime system (concurrency, channels, strings, ...)
  errors/     Error handling
examples/     Goo programs used as golden tests
goostd/       Vendored Go standard-library packages
scripts/      Build, test, and coverage tooling
docs/         Design specs, plans, and the stdlib-coverage report
```

## Contributing

1. Fork the repo and create a feature branch.
2. Follow test-driven development — add a golden probe or unit test first.
3. Keep the gate green: `make verify` and `make test` must pass, and the Bison
   conflict baseline must be unchanged (`./scripts/grammar-tripwire.sh`).
4. Open a pull request.

Grammar changes (`src/parser/parser.y`, lexer token emission) have extra rules —
see `CLAUDE.md` and the `goo-grammar` skill.

## License

MIT — see [LICENSE](LICENSE).

## Acknowledgments

- **Go** — the language Goo is a superset of, and the source of its core design.
- **LLVM** — the code-generation backend.
- **Rust** and **Zig** — inspiration for the ownership model, no-GC runtime, and `comptime`.
- **Erlang**, **ZeroMQ**, and **OpenMP** — the paradigms shaping Goo's parallelism roadmap.
