<div align="center">

# Goo Programming Language

**A deterministic, no-GC superset of Go that molds to the hardware it runs on — across CPUs, QPUs, and FPGAs.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Implementation: C23](https://img.shields.io/badge/Implementation-C23-00599C.svg)](https://en.cppreference.com/w/c/23)
[![Backend: LLVM 22+](https://img.shields.io/badge/Backend-LLVM%2022%2B-orange.svg)](https://llvm.org/)
[![Quantum: QIR 1.0 & OpenQASM 3](https://img.shields.io/badge/Quantum-QIR%201.0%20%7C%20OpenQASM%203-6929C4.svg)](#quantum-computing--hardware-verification)
[![Verification: 187 Probes | 497 Goldens](https://img.shields.io/badge/Verified-187%20Probes%20%C2%B7%20497%20Goldens%20%C2%B7%20165%20Rejects-2ea44f.svg)](#verification-architecture)

[Overview](#overview) · [Features](#what-goo-adds-to-go) · [Quick Start](#quick-start) · [Language Tour](#language-tour) · [Quantum Computing](#quantum-computing--hardware-verification) · [Stdlib Coverage](#standard-library-coverage) · [Verification](#verification-architecture)

</div>

---

## Overview

**Goo** takes Go's clean syntax, fast compilation model, and CSP concurrency as a strict foundation, then replaces the stop-the-world garbage collector and fills the expressive gaps required for hard real-time systems, high-throughput parallel pipelines, and hybrid quantum–classical control:

- **Strict Go Superset:** Standard `.go` files and vendored Go standard-library packages (`goostd/`) compile unmodified alongside `.goo` sources.
- **Deterministic No-GC Runtime:** Automatic Reference Counting (ARC) paired with compile-time parameter, block, and local escape analysis (`src/types/*_escape.c`) elides retain/release traffic on non-escaping values, complemented by zero-overhead lexical `arena {}` regions.
- **Compile-Time Metaprogramming (`comptime`):** Zig-style compile-time function evaluation and value-specialized monomorphization (`func kernel[T any](comptime N int, ...)`), folding recursive and structural computations before LLVM IR emission.
- **Type-Safe Error Unions & Nullables:** Explicit `!T` error unions (`try` / `catch`) and `?T` optional types (`if let`) eliminate unchecked `nil` dereferences and boilerplate `if err != nil` chains.
- **First-Class Hybrid Quantum–Classical Compilation:** Compile-time linear `qir.Qubit` ownership (`src/types/qir_ownership.c`) enforces the quantum no-cloning theorem, distinct multi-qubit operands, and safe uncomputation, lowering directly to **QIR 1.0 Adaptive Profile** (`--emit-qir`), **OpenQASM 3.0** (`--emit-qasm3`), or native binaries linked against an embedded 16-qubit deterministic statevector simulator (`src/runtime/qir_sim.c`).

### One Front-End, Poured Into Each Hardware Mold

Instead of stitching together Python for circuits, C++ for host decoders, Verilog/HLS for FPGAs, and OpenQASM/QIR for the QPU, Goo enforces target-specific physical and memory contracts once at the front-end and lowers cleanly to each hardware backend:

| Target Hardware | Goo Backend | Compile-Time Contract Enforced |
| :--- | :--- | :--- |
| **CPU** *(Host / ms-cycle QEC)* | LLVM 22+ native (`-O2`) | No-GC ARC + escape-analysis elision + lexical `arena {}` regions |
| **QPU** *(Trapped-Ion / Superconducting)* | `--emit-qir` / `--emit-qasm3` | Linear `qir.Qubit` (no cloning, no aliasing, safe uncomputation) |
| **FPGA** *(Sub-µs QEC / Control — roadmap)* | `--emit-mlir` → CIRCT → `.sv` | Zero-alloc, fixed arrays, statically bounded loops |
| **Laptop** *(Pre-Flight Verification)* | Embedded `qir_sim.c` & `AerSimulator` | Deterministic native & noisy-ISA execution before touching hardware |

> [!NOTE]
> Goo is an independent compiler engineering project. It is **not** affiliated with, sponsored by, or endorsed by Google LLC or the Go project. "Go" and the Go gopher are trademarks of Google LLC.

---

## What Goo Adds to Go

Every capability below is enforced by automated probes in `make verify-core`:

| Capability | Surface Syntax | Compiler Pass / Lowering | Status |
| :--- | :--- | :--- | :---: |
| **Error Unions** | `func parse(s string) !int`, `try`, `catch err { ... }` | `src/codegen/error_union_codegen.c` | ✅ Verified |
| **Nullable Types** | `var p ?int`, `if let v = p { ... }` | `src/codegen/nullable_codegen.c` | ✅ Verified |
| **Compile-Time Evaluation** | `comptime const`, `func f(comptime N int)` | `src/comptime/`, `src/codegen/monomorphize.c` | ✅ Verified |
| **Generics** | `func Map[T, U any](s []T, fn func(T) U) []U` | `src/codegen/monomorphize.c` | ✅ Verified |
| **No-GC Ownership & Escape Elision** | Automatic ARC + `owned` / `borrowed` + `arena {}` | `src/types/param_escape.c`, `block_escape.c`, `release_decision.c` | ✅ Verified |
| **CSP & Data-Parallel Lanes** | `go`, `chan`, `select`, `lanes.Map`, `lanes.RunFar` | `src/runtime/concurrency.c`, `far_transport.c`, `goostd/lanes/` | ✅ Verified |
| **Linear Quantum Types & QIR/QASM3** | `import "qir"`, `--emit-qir`, `--emit-qasm3` | `src/types/qir_ownership.c`, `call_codegen.c`, `qasm3_codegen.c` | ✅ Verified |

---

## Quick Start

### Prerequisites

- **C23 compiler** (`gcc >= 14` or `clang >= 18`)
- **LLVM** (`22` or `23`, with `llvm-config`)
- **Bison** (`>= 3.8`) and **GNU Make**

### Build the Compiler

```bash
git clone https://github.com/goo-lang/goolang.git
cd goolang
make -j$(nproc) lexer   # builds bin/goo and lib/libgoo_runtime.a
```

### Compile, Run, and Test

```bash
# Compile and execute a single program
./bin/goo run examples/hello.goo

# Build an optimized native binary (-O2 runs LLVM's new pass manager pipeline)
./bin/goo -O2 examples/qir_bitflip_qec_probe.goo -o bitflip_qec
./bitflip_qec

# Run a package's *_test.go / *_test.goo test suite
./bin/goo test ./goostd/sort

# Emit QIR 1.0 Adaptive Profile LLVM IR or OpenQASM 3.0
./bin/goo --emit-qir   examples/qir_teleport_probe.goo -o teleport.ll
./bin/goo --emit-qasm3 examples/qir_teleport_probe.goo -o teleport.qasm
```

---

## Language Tour

All examples below compile and run today; see [`examples/`](examples/) for the full 497-program golden suite.

### 1. Error Unions (`!T`) and Nullables (`?T`)

Functions that can fail return `!T`; callers must handle failure via `catch` or propagate it via `try`. Optional values use `?T` and unwrap safely with `if let`:

```goo
package main

import "fmt"

func lookupPort(env string) !int {
    if env == "prod" {
        return 8443
    }
    return error("unknown environment")
}

func main() {
    port := lookupPort("prod") catch err {
        fmt.Println("error:", err)
        return
    }

    var override ?int = port
    if let p = override {
        fmt.Println("listening on port:", p)
    }
}
```

### 2. Compile-Time Execution (`comptime`) and Value Specialization

Goo's AST evaluator executes pure functions, loops, and recursion at compile time and monomorphizes functions parameterized by `comptime` constants:

```goo
package main

import "fmt"

func syndromeMask(b0 int, b1 int) int {
    return b0 + b1*2
}

comptime const SYNDROME_Q0 int = syndromeMask(1, 0) // folded to 1
comptime const SYNDROME_Q1 int = syndromeMask(1, 1) // folded to 3
comptime const SYNDROME_Q2 int = syndromeMask(0, 1) // folded to 2

func main() {
    fmt.Println(SYNDROME_Q0, SYNDROME_Q1, SYNDROME_Q2)
}
```

### 3. Deterministic Memory (`arena {}` + Escape-Elided ARC)

Without a garbage collector, sub-microsecond hot loops stay pause-free. Values that do not escape a function or block have their reference-count traffic elided at compile time, and `arena {}` regions bulk-free temporary allocations on scope exit:

```goo
package main

import "fmt"

func processBatch() int {
    total := 0
    arena {
        // Temporary composite allocations inside the arena are reclaimed
        // deterministically at the closing brace with zero GC pause.
        items := []int{10, 20, 12}
        for _, v := range items {
            total = total + v
        }
    }
    return total
}

func main() {
    fmt.Println(processBatch()) // 42
}
```

---

## Quantum Computing & Hardware Verification

Goo treats quantum control as a first-class systems programming problem: real-time Quantum Error Correction (QEC) decoders cannot tolerate stop-the-world GC pauses, and quantum circuits require compile-time linear type safety.

### Compile-Time Linear `qir.Qubit` Ownership (`src/types/qir_ownership.c`)

Importing `"qir"` activates a dedicated linear dataflow verifier after type checking. Classical Go code remains 100% unchanged, while `qir.Qubit` handles obey five strict compile-time laws:

1. **No-Cloning (Move on Copy):** Assigning a live `qir.Qubit` to another variable moves ownership; reading the source variable afterward is a compile-time error (`use of moved linear qubit`).
2. **No-Aliasing in Multi-Qubit Gates:** Passing the same `qir.Qubit` handle twice to a multi-qubit gate (`qir.CX(q, q)`, `qir.CZ(q, q)`, `qir.Swap(q, q)`, `qir.CCX(q, a, q)`) is rejected at compile time (`cannot alias qubit 'q' in multi-qubit gate`).
3. **Mandatory Uncomputation / Explicit Termination:** Every allocated qubit (`qir.Alloc()` or `qir.QubitAt(id)`) must be explicitly consumed via `qir.MeasureReset(q)` or `qir.FreeZero(q)` before scope exit or `return` (`linear qubit 'q' leaves scope without being consumed`).
4. **Control-Flow Join Agreement & Loop Invariance:** `if`/`else` branches must agree on the liveness of every qubit, and loops may neither consume an outer-scope qubit nor leave a loop-local qubit unconsumed.
5. **No Heap or Concurrency Escape:** `qir.Qubit` handles cannot be captured by closures, spawned into `go` statements or `defer`, or stored inside slices, arrays, maps, structs, or channels.

### Real-Time 3-Qubit Bit-Flip QEC Example (`examples/qir_bitflip_qec_probe.goo`)

```goo
func correctBitFlip(d0 qir.Qubit, d1 qir.Qubit, d2 qir.Qubit, a0 qir.Qubit, a1 qir.Qubit) {
    r0 := qir.ResultAt(0)
    r1 := qir.ResultAt(1)

    // Parity checks Z0*Z1 -> a0 and Z1*Z2 -> a1
    qir.CX(d0, a0)
    qir.CX(d1, a0)
    qir.Mz(a0, r0)

    qir.CX(d1, a1)
    qir.CX(d2, a1)
    qir.Mz(a1, r1)

    // Real-time classical syndrome decoding & feed-forward recovery
    syn := syndromeMask(boolBit(qir.ReadResult(r0)), boolBit(qir.ReadResult(r1)))
    if syn == SYNDROME_Q0 {
        qir.X(d0)
    } else if syn == SYNDROME_Q1 {
        qir.X(d1)
    } else if syn == SYNDROME_Q2 {
        qir.X(d2)
    }

    qir.Reset(a0)
    qir.Reset(a1)
}
```

### Backends & Verification Matrix

| Program | Backend | Target Format | Shots | Observed Fidelity / Pass Rate | Job ID |
| :--- | :--- | :--- | :---: | :---: | :--- |
| `qir_teleport_probe.goo` | Embedded `qir_sim.c` (`-O0` / `-O2`) | Native LLVM | 16 | **100.0%** (16/16) | `local` |
| `qir_bitflip_qec_probe.goo` | Embedded `qir_sim.c` (`-O0` / `-O2`) | Native LLVM | 4 fault modes | **100.0%** (4/4) | `local` |
| `qir_comptime_stabilizer_probe.goo` | Embedded `qir_sim.c` (`-O0` / `-O2`) | Native LLVM | 4 fault modes | **100.0%** (4/4) | `local` |
| `qir_teleport_probe.goo` | Qiskit `AerSimulator` (`SamplerV2`) | OpenQASM 3.0 | 100 | **100.0%** (100/100) | `a3294c9e-7486-4666-9403-08ce345259a8` |
| `qir_comptime_stabilizer_probe.goo` | Qiskit `AerSimulator` (`SamplerV2`) | OpenQASM 3.0 | 100 | **100.0%** (100/100) | `12993239-a815-4f46-b0cc-79e20b896de6` |
| `qir_teleport_probe.goo` | IBM Heron r2 `ibm_marrakesh` (`FakeMarrakesh`, `17 cz`) | OpenQASM 3.0 | 100 | **99.0%** (99/100) | `d66d29ad-0520-4d93-8c3a-bdc44acebd6d` |
| `qir_bitflip_qec_probe.goo` | IBM Heron r2 `ibm_marrakesh` (`FakeMarrakesh`, `83 cz`) | OpenQASM 3.0 | 100 (400 rounds) | **94.5%** per-round (79% all-4) | `d66d29ad-0520-4d93-8c3a-bdc44acebd6d` |
| `qir_comptime_stabilizer_probe.goo` | IBM Heron r2 `ibm_marrakesh` (`FakeMarrakesh`, `80 cz`) | OpenQASM 3.0 | 100 (400 rounds) | **95.0%** per-round (80% all-4) | `d66d29ad-0520-4d93-8c3a-bdc44acebd6d` |
| `qir_teleport_probe.goo` | IBM Heron r2 `ibm_kingston` (`FakeKingston`, `17 cz`) | OpenQASM 3.0 | 100 | **99.0%** (99/100) | `7889e58c-b159-4856-8e98-ed4a01335dc7` |
| `qir_comptime_stabilizer_probe.goo` | IBM Heron r2 `ibm_kingston` (`FakeKingston`, `104 cz`) | OpenQASM 3.0 | 100 (400 rounds) | **94.3%** per-round (78% all-4) | `7889e58c-b159-4856-8e98-ed4a01335dc7` |
| `qir_teleport_probe.goo` | IBM Heron r2 `ibm_fez` (`FakeFez`, `17 cz`) | OpenQASM 3.0 | 100 | **95.0%** (95/100) | `5af7af30-1509-4991-abd8-60e6cd966496` |
| `qir_comptime_stabilizer_probe.goo` | IBM Heron r2 `ibm_fez` (`FakeFez`, `83 cz`) | OpenQASM 3.0 | 100 (400 rounds) | **92.5%** per-round (71% all-4) | `5af7af30-1509-4991-abd8-60e6cd966496` |
| `qir_teleport_probe.goo` | IBM Heron r1 `ibm_torino` (`FakeTorino`, `17 cz`) | OpenQASM 3.0 | 100 | **97.0%** (97/100) | `6a2f07ad-7282-4739-bc9c-c4f755bd344e` |
| `qir_bitflip_qec_probe.goo` | Quantinuum `h1-1sc` / `h1-1e` (pre-flight) | QIR 1.0 Adaptive | 50 | **Verified QIR** (`0.00` / `6.37 HQC`) | `pre-flight` |

### Quota-Safe QPU Submission Workflows

Both cloud submission scripts default to **dry-run pre-flight mode (`0` QPU seconds / `0` HQC spent)** and enforce hard server-side execution/credit caps:

```bash
# Azure Quantum (Quantinuum): offline HQC gate count + 0-HQC cloud syntax checker (h1-1sc)
./bin/goo --emit-qir examples/qir_bitflip_qec_probe.goo -o build/qir_qec.ll
python3 scripts/qir_submit_azure.py build/qir_qec.ll

# IBM Quantum Open Plan (600s/month limit): local ISA transpilation pre-flight +
# single-job PUB batching + 10s hard max_execution_time circuit breaker
./bin/goo --emit-qasm3 examples/qir_teleport_probe.goo -o build/qir_teleport.qasm
./bin/goo --emit-qasm3 examples/qir_bitflip_qec_probe.goo -o build/qir_qec.qasm
python3 scripts/qasm3_submit_ibm.py build/qir_teleport.qasm build/qir_qec.qasm
```

---

## Standard Library Coverage

Goo vendors unmodified Go standard-library source in [`goostd/`](goostd/) (`bytes`, `errors`, `io`, `lanes`, `math/bits`, `path/filepath`, `sort`, `strconv`, `strings`, `unicode/utf8`) and provides low-level compiler shims for nine runtime-integrated packages (`fmt`, `os`, `math`, `errors`, `sync`, `time`, `far`, `testing`, `qir`).

Coverage against Go's official API manifest (`$GOROOT/api/go1*.txt`) is continuously measured and gated by `make stdlib-coverage-drift` and `make stdlib-smoke-coverage`:

- **202 / 202 exported stdlib + runtime symbols** verified across **497 golden fixtures** and **5 `goostd` package test suites**.
- **112 Go standard-library API symbols** across **12 Go packages** (`docs/stdlib-coverage.json`), led by `math/bits` (**88.0%**), `sort` (**63.6%**), `strings` (**31.7%**), `unicode/utf8` (**31.6%**), `errors` (**28.6%**), `path/filepath` (**22.2%**), `strconv` (**15.8%**), and `fmt` (**13.8%**).

---

## Verification Architecture

Goo enforces a strict engineering invariant: **`src/` contains only what ships in `bin/goo` / `libgoo_runtime.a` or what an automated gate in `make verify-core` exercises.** Every compiler subsystem is guarded by teeth-checked probes (probes that include a `--self-test` proving they turn red when a defect is injected):

```bash
make lexer                  # Build compiler (bin/goo) and deterministic runtime archive
make verify-core            # Run the full 187-probe verification net + golden suites
make test-golden            # 497 golden execution fixtures at -O0
make test-golden-o2         # 497 golden execution fixtures at -O2 (differential parity)
make test-golden-reject     # 165 compile-reject fixtures with exact diagnostic checks
bash tools/verify_layering.sh # Bazel strict header-layering and package visibility gate
```

| Verification Gate | What It Enforces |
| :--- | :--- |
| `test-golden` & `test-golden-o2` | 497 end-to-end programs compiled, executed, and diffed at `-O0` and `-O2` |
| `test-golden-reject` | 165 negative fixtures verifying exact line/column rejection diagnostics |
| `escape-teeth` & `release-decision-teeth` | Mutation-tested unit suites for parameter/block/local escape and ARC elision |
| `ast-free-leak-probe` | Valgrind zero-leak verification over AST construction and teardown |
| `alloc-doors-probe` | Enforces `xmalloc`/`xcalloc`/`xrealloc`/`xstrdup` across all 76 `src/` files |
| `archive-determinism-probe` | Verifies zeroed mtime/uid/gid across all 104 members of `lib/libgoo_runtime.a` |
| `tools/verify_layering.sh` | Bazel `layering_check` ensuring zero undeclared header dependencies across 22 headers |
| `doc-claims-probe` | Re-verifies architectural claims in `docs/adr/` against live source files |

---

## Repository Layout

```text
src/
  lexer/        Hand-written lexer with targeted Go Automatic Semicolon Insertion (ASI)
  parser/       Bison LALR(1) grammar (conflict-ratcheted via scripts/grammar-tripwire.sh)
  ast/          Typed AST nodes and deterministic teardown (ast_node_free)
  types/        Type checker, generics, escape analysis, ARC release decisions, linear qir_ownership
  comptime/     Compile-time AST interpreter and constant folder
  codegen/      LLVM 22+ IR backend, QIR 1.0 Adaptive emitter, and OpenQASM 3.0 emitter
  runtime/      No-GC ARC allocator, lexical arenas, CSP scheduler/channels, NNG transport, 16-qubit QIR simulator
  package/      Multi-package import resolver and symbol export filter
goostd/         Vendored Go standard-library packages compiled from source
examples/       497 golden test programs (.goo + .expected.txt)
tests/golden/   165 compile-reject fixtures (tests/golden/reject/)
scripts/        Verification probes, coverage ratchets, and QPU submission utilities
docs/           Language specification, ADRs, MISRA C:2023 deviations, and design specs
```

---

## Contributing

1. Create a feature branch from `main`.
2. Add a golden probe (`examples/<name>.goo` + `.expected.txt`) or negative reject test (`tests/golden/reject/<name>.goo` + `.err.txt`) first.
3. Ensure all verification gates pass before opening a pull request:
   ```bash
   make verify-core
   ./scripts/grammar-tripwire.sh
   bash tools/verify_layering.sh
   ```

---

## License

Released under the [MIT License](LICENSE).
