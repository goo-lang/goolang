# QPU Project 1 — Native IBM Submission from Goo, Scope A (Design)

**Date:** 2026-11-27
**Status:** Draft design spec. No implementation in this commit.
**Base:** `origin/main` b3916266.
**Builds on:** `docs/superpowers/specs/2026-11-24-quantum-qir-linear-qubits-design.md`
(linear `qir.Qubit`, `src/types/qir_ownership.c`, `src/runtime/qir_sim.c`,
`src/codegen/qasm3_codegen.c`).

Fixed decisions (user, 2026-11-25) that this document does not reopen:

1. Goo replaces QRMI and Python for QPU submission. Scope A is a compile-time
   target check and a native-gate translation against a **pinned** backend
   description, with **no routing**: the user maps logical qubits to physical
   qubits by hand.
2. A thin C `http.Do` shim over libcurl. IAM, REST and JSON live in Goo source.
3. Circuits are embedded as constants. A QPU lease is linear.
4. Every hardware or execution modifier is spelled with `@` on the
   declaration (`@qpu func`), never a contextual keyword.
5. Rule 0: every README or commit claim maps to a gate. A live job ID is
   labelled "external, not gated". No install of libqrmi, qiskit, qirrunner,
   verilator or circt, and no live quota job, without fresh approval.
6. Superset rule: new syntax changes the meaning of no valid Go program.

Every "measured" or "probed" statement below was taken on 2026-11-27 in this
worktree with `bin/goo` built by `make -j16 lexer` from b3916266. The probe
programs are listed in Appendix A with the one construct each one tests.

---

## 0. Findings that shape the design

These were found while reading the inputs. Each one changes a decision below.

| # | Finding | Evidence | Consequence |
|---|---|---|---|
| F1 | The `@` token never reaches the parser. `lexer.c:316` emits `TOKEN_DEREF`, but `map_token_to_bison` in `src/parser/lexer_bridge.c` has no case for it, and `bridge_next_mapped` **silently skips** unknown tokens. | Probe `t21`: `m := @n` compiles and prints the value of `n`. Probe `a01`: `@qpu func bell()` fails with `syntax error, unexpected identifier` at `qpu`. | The `attribute` / `attribute_list` rules in `parser.y:3017-3042` are dead grammar. The `@` "comptime intrinsics" (`@fields`, `@format`, `@struct_fields`) are unreachable from source. |
| F2 | The comptime `@` intrinsics are mocks. `comptime_intrinsic_struct_fields` returns the fixed list `"id","name","value"`; `@format` formats the fixed `"Value: %d"` with `123`. | `src/comptime/comptime_intrinsics.c:274-303, 494-533` (linked: `nm bin/goo` finds both symbols). | Mapping `@` (F1) would make these mocks reachable. The grammar change must refuse expression-position `@` until real intrinsics exist. No comptime reflection exists, so a comptime JSON codec is not available. |
| F3 | The existing attributed `func_decl` arms (`attribute_list FUNC identifier func_signature block`, `parser.y:526-571`) assign `func_signature`'s value to `params`. For a function with a result, `func_signature` returns the **result** node, so the result becomes the parameter list. | Read of `parser.y:626-660` (`LPAREN RPAREN func_result { $$ = $3; }`). | Replace those arms. Do not reuse them. |
| F4 | `--emit-qasm3` is an abstract interpreter with name heuristics and silent defaults: an angle that is not a bare literal prints as `0.0`; an unresolved qubit reference becomes `q[0]`; unknown statements are skipped; the names `shot`, `syndromeMask` and `injectPerturbation` change the output. | Probe `w02`: `qir.Rz(theta, q)` with `const theta = 0.5` emits `rz(0.0) q[0];`, exit 0. Read of `qasm3_codegen.c:178-191, 213-410, 515-536`. | A target check over this emitter would prove nothing. Scope A needs a new, fail-closed circuit extractor (Section 3.1). The old emitter stays for the old probes. |
| F5 | `qir_sim.c` masks a qubit pointer with `& 15`. `qir.QubitAt(16)` is qubit 0. | Probe `w01`: `X(QubitAt(16)); Mz(QubitAt(0))` reads `true`. | Physical indices on a 156-qubit device alias in the local simulator. The simulator must relabel densely and refuse a 17th distinct qubit (Section 4.4). |
| F6 | `bin/goo` links libcurl, but **user programs do not**. The user link line is `<obj> lib/libgoo_runtime.a [-l<user>]* -lm -lpthread` (`src/codegen/codegen.c:1955-2000`). | `ldd bin/goo` lists libcurl, `ldd` of a compiled hello-world does not. | The `http` shim needs `-lcurl` on the user link line, added only when the program imports the shim (Section 6.5). |
| F7 | Every live IBM row in the README went through Qiskit transpilation (`scripts/qasm3_submit_ibm.py:112-125`, preset pass manager, optimisation level 3). Goo has never submitted its own native-gate text. | Read of the script and README table. | "IBM accepts the Goo text" is not shown by any earlier run. It is external, not gated, until the first live run (Section 9). |
| F8 | The client code must avoid several Go constructs that do not compile or that print a different result today (three-name assignment, `case a, b:`, `pkg.T{}` literals, user error types as `error`, `select` with `time.After`). | Section 8. | The client is writable today in a restricted dialect. Section 8 ranks the fixes that would let it read like ordinary Go. |

---

## 1. Intent and scope

### 1.1 Intent

A Goo program declares a circuit as a `@qpu("<backend>")` function, the
compiler checks the circuit against a pinned description of that backend,
translates it to the backend's native gates, and embeds the result as a
constant. The same program obtains an IAM token, submits one Sampler job over
the Qiskit Runtime REST API, polls it, and decodes the counts — all in Goo
source, over one thin C HTTP primitive. No Python, Qiskit or QRMI runs at any
step.

### 1.2 What scope A replaces in `scripts/qasm3_submit_ibm.py`

| Python step | Scope A |
|---|---|
| `qasm3.loads` of `--emit-qasm3` output | Replaced: the compiler owns the circuit IR (Section 3.1). |
| `generate_preset_pass_manager(optimization_level=3)`: layout, routing, optimisation, basis translation | **Basis translation only** (Section 4). Layout is the user's `qir.QubitAt(n)`. No routing, no optimisation. A circuit that needs routing is refused at compile time. |
| `QiskitRuntimeService(token=...)` | IAM API-key exchange in Goo (Section 7.2). |
| `service.least_busy(...)` | Not replaced. The backend is fixed by the pin. |
| `sampler.options.max_execution_time = 10` | The lease's `maxSeconds`, sent as the job's top-level `cost` (Section 7.3). |
| `sampler.run(isa_circuits, shots=...)` | `POST /v1/jobs` built by Goo (Section 7.3). |
| `job.result()[i].data.c.get_counts()` | Poll `GET /v1/jobs/{id}`, then `GET /v1/jobs/{id}/results`, decoded in Goo (Section 7.4). |
| `IBM_MODE=aer` / `IBM_MODE=fake` (noise-model simulation) | **Not replaced.** The local ideal simulator is `qir_sim.c`. Noise simulation is out of scope; see open question Q7. |

### 1.3 Out of scope

- Routing, SWAP insertion, layout search, and any circuit optimisation.
- Error mitigation, twirling, dynamical decoupling (Sampler options are sent
  empty).
- More than one backend in a program, and backend selection at run time.
- Fractional gates (`rx`, `rzz`) and any basis other than `{cz, rz, sx, x}`.
- Sessions (the Open Plan cannot submit session jobs, per the execution-modes
  guide) and batch mode.
- Slurm, a site scheduler, or any queue other than IBM's.
- Quantinuum / Azure (`scripts/qir_submit_azure.py` stays as it is).
- Parameterised circuits (PUB parameter values are always `null`).

---

## 2. The pinned backend description

### 2.1 Format and location

One canonical JSON file for each backend:
`catalogue/qpu/<backend_name>.json`. `catalogue/` already holds the
diagnostics catalogue, and pins are data that the compiler reads, so they
belong beside it. The compiler parses it with json-c, which `bin/goo` already
links (`Makefile:31`).

```json
{
  "schema": "goo-qpu-pin/1",
  "provider": "ibm",
  "backend_name": "ibm_fez",
  "provenance": {
    "source": "GET /api/v1/backends/ibm_fez/configuration",
    "ibm_api_version": "2026-04-15",
    "fetched_utc": "2026-11-27T00:00:00Z",
    "backend_version": "x.y.z",
    "synthetic": false
  },
  "n_qubits": 156,
  "basis_gates": ["cz", "id", "rz", "sx", "x"],
  "supported_instructions": ["cz", "delay", "id", "if_else", "measure", "reset", "rz", "sx", "x"],
  "coupling": [[0, 1], [1, 2], [1, 3]],
  "control_flow": { "nested": false }
}
```

Canonical form, so that two pins of the same device are byte-identical:

- `basis_gates` and `supported_instructions`: sorted, unique, `LC_ALL=C`
  byte order (memory note: derived files need a pinned locale).
- `coupling`: **undirected** pairs `[a, b]` with `a < b`, sorted, unique. IBM's
  `coupling_map` lists both directions for `cz`. Scope A only emits `cz`,
  which is symmetric (checked numerically, Section 4.2), so direction carries
  no information.
- Fractional entries (`rx`, `rzz`, `xslow`) are **dropped** from `basis_gates`
  when the pin is made. The OpenAPI example configuration lists
  `["cz","id","rx","rz","rzz","sx","x","xslow"]`. Scope A never emits those,
  and keeping them would let a later edit start to emit them unchecked.
- `control_flow.nested` is `false` for IBM. It is not in the configuration
  response. It comes from IBM's documented limit (the existing emitter cites
  "Error 1524, nested control flow not supported"). It is a pin field so that
  the check reads data, not a constant in C.
- Two-space indent, one key per line, keys in the order above, newline at
  end of file. One writer (Section 2.2) produces this, and the probe
  (Section 9, `qpu-pin-probe`) refuses any other byte sequence.

The **canonical summary** of a pin is the single line
`n=<n_qubits>;basis=<b1,b2,...>;instr=<i1,...>;edges=<a-b,a-b,...>`.
The compiler embeds this text in every circuit constant (Section 5.3). The Goo
client builds the same text from a live configuration response and compares
the two strings (Section 2.3). No hash is used, because Goo has no SHA-256
today and a text compare needs none. For 156 qubits the summary is about 1.5 KB.

### 2.2 How a pin is made

A Goo program, `tools/qpu_pin/main.goo`, that uses the same `ibmq` package as
the submission path:

- `goo run tools/qpu_pin -- --backend ibm_fez` does a credentialed
  `GET /v1/backends/ibm_fez/configuration`. This costs no QPU time, but it
  uses credentials, so the first run needs the user's approval (Q1).
- `goo run tools/qpu_pin -- --from config.json` canonicalises a configuration
  file that the user supplies. No network.

Both write the canonical file to stdout. The tool reads only `n_qubits`,
`basis_gates`, `supported_instructions`, `coupling_map`, `backend_name` and
`backend_version` from the response. It refuses a response that lacks one of
them, a `coupling_map` index outside `[0, n_qubits)`, a self-edge, or a basis
that lacks one of `cz`, `rz`, `sx`, `x`.

For the gates, a **synthetic** pin `catalogue/qpu/goo_test_heavyhex.json`
(`"synthetic": true`, a 27-qubit heavy-hex graph written by hand) lets every
local gate run with no credentials. Tests never depend on the real pin's
content, except one fixture that names the real backend.

### 2.3 Stale detection

Three layers, from cheapest to strongest:

1. **Compile time, offline.** The compiler refuses a pin that fails the
   schema or the canonical form. It does not read the clock: a build must not
   depend on the date (reproducible-build gates).
2. **Run time, live, before submission.** `ibmq.Run` fetches the live
   configuration, builds its canonical summary with the same rules, and
   compares it with the summary embedded in the circuit constant. A mismatch
   stops the run before any job is created:
   `ibmq: backend 'ibm_fez' no longer matches its pin (edges differ: live lacks 41-42); re-pin with tools/qpu_pin`.
   The message names the first difference so that the user can judge whether
   the circuit is affected.
3. **Gate, offline.** `qpu-pin-probe` recomputes the canonical bytes of every
   pin. A hand edit that breaks the canonical form fails the gate.

Not covered, and stated so: a qubit or coupler that IBM marks non-operational
in the **properties** endpoint, while the configuration still lists it. The
structural compare does not see calibration. Q4 asks whether scope A also
checks `GET /v1/backends/{id}/properties` for the qubits and couplers that the
circuit uses (the compiler can embed that list).

---

## 3. The compile-time target check

### 3.1 The circuit extractor (prerequisite)

A new pass, `src/types/qpu_circuit.c`, runs after `qir_ownership_check_program`
for each `@qpu` function. It produces a flat IR:

```c
typedef enum { QPU_OP_GATE, QPU_OP_MEASURE, QPU_OP_RESET, QPU_OP_IF_BIT } QpuOpKind;
typedef struct QpuOp {
    QpuOpKind kind;
    QpuGate   gate;          // H, X, Y, Z, S, SDG, T, TDG, RX, RY, RZ, CX, CZ, SWAP, CCX, SX
    uint32_t  q[3];          // physical indices
    uint8_t   nq;
    double    theta;         // folded at compile time; RX/RY/RZ only
    uint32_t  clbit;         // MEASURE target, IF_BIT condition bit
    struct QpuOp* then_ops; size_t then_n;   // IF_BIT bodies: gates only
    struct QpuOp* else_ops; size_t else_n;
    Position  pos;           // the source call, for every diagnostic
} QpuOp;
```

It accepts exactly this subset and **refuses everything else** with a
positioned error (fail closed, the opposite of F4):

- `x := qir.QubitAt(<const int>)` and `r := qir.ResultAt(<const int>)`.
  `qir_ownership.c` already proves the id is a constant and that no two live
  bindings share one.
- Gate calls on those bindings. Angles fold at compile time from float
  literals, unary minus, `+ - * /`, `math.Pi`, and `const` identifiers bound to
  such expressions. Anything else is refused (Section 3.3, E6).
- `qir.Mz(q, r)`, `qir.Reset(q)`, `qir.FreeZero(q)` (emits nothing; the
  ownership pass already demands it), and `_ = qir.MeasureReset(q)` (measure
  into a fresh bit numbered after the largest `ResultAt` id, then reset — the
  old emitter numbers it from a counter that can collide with a later
  `ResultAt`).
- `if qir.ReadResult(r) { ... } else { ... }`, one level deep, bodies made of
  gate calls, `Mz` and `Reset`.
- `for i := 0; i < N; i++ { ... }` with a constant `N` ≤ 1024, fully unrolled.
  The loop variable cannot select a qubit, because `QubitAt` needs a constant.
- A call to another `@qpu` function with `qir.Qubit` / `qir.Result` arguments,
  inlined. Recursion is refused (a call-stack depth limit of 64 makes the
  refusal deterministic).
- `qir.Alloc()` is refused in scope A (E4): it has no physical qubit.

The old `--emit-qasm3` path is not changed, so the existing probes and the
README rows that cite it keep their meaning. Q8 asks whether to rebuild it on
the extractor later.

### 3.2 What the check refuses

The check runs on the **translated** IR (Section 4), so a coupler is checked
for every `cz` that the translation produces, and each error points at the
source call that produced it. It refuses:

1. A physical index outside `[0, n_qubits)`.
2. A gate that has no translation to the pin's basis (for example any
   two-qubit gate when `cz` is missing from the basis).
3. A two-qubit gate on two qubits that the coupling map does not join. For
   `qir.CCX` all three pairs must be joined. Heavy-hex has no triangle, so
   `qir.CCX` is always refused on IBM Heron. The message says why.
4. An instruction that `supported_instructions` lacks (`measure`, `reset`,
   `if_else`).
5. Nested measurement-conditioned control flow when `control_flow.nested` is
   false.
6. Two `@qpu` functions in one program that name different backends.

### 3.3 Diagnostics

All go through `diag_emit` in the project format
`<file>:<line>:<col>: error: <message>`, followed by the snippet and caret,
then an optional `  help: ` line that is paste-ready or absent. Every format
string is registered in `catalogue/diagnostics.tsv` (the drift probe), and
each help line gets a `tests/diag_format/help/NAME.fixed.goo` fixture that
must compile.

Example of the full shape (E2):

```
bell.goo:9:2: error: qir.CX on physical qubits 3 and 9: backend 'ibm_fez' has no coupler between them (qubit 3 couples to 2, 4, 16)
    9 | 	qir.CX(a, b)
      | 	^
```

| Id | When | Message | help |
|---|---|---|---|
| E1 | Index out of range | `qir.QubitAt(%u): backend '%s' has physical qubits 0 to %u` (caret on the argument) | none: no correct index can be inferred |
| E2 | Pair not joined | `qir.%s on physical qubits %u and %u: backend '%s' has no coupler between them (qubit %u couples to %s)` | none: the fix is a re-mapping or routing, and routing is out of scope. The neighbour list is in the message, not in `help`, because it is not a paste-ready fix. |
| E3 | CCX without a triangle | `qir.CCX on physical qubits %u, %u and %u needs couplers between all three pairs: backend '%s' lacks %s` | none |
| E4 | `qir.Alloc` in `@qpu` | `qir.Alloc in a @qpu function has no physical qubit: scope A needs qir.QubitAt(<index>)` | none: the index is the user's mapping |
| E5 | No basis translation | `qir.%s cannot run on backend '%s': the basis (%s) lacks %s` | none |
| E6 | Angle not constant | `rotation angle in a @qpu function must be a compile-time constant` | none |
| E7 | Construct outside the subset | `%s is not allowed in a @qpu function` (for example `a call to 'fmt.Println'`, `a variable declaration of type int`) | for a call to a plain function that only takes qubits and results: `@qpu("%s")` + `\nplace this on the line before func %s` (same two-line form as `qir_error_overwrite_live`, `qir_ownership.c:342-353`). Otherwise none. |
| E8 | Nested control flow | `backend '%s' does not support nested control flow: this if is inside an if on a measurement result` | none |
| E9 | Unsupported instruction | `backend '%s' does not list '%s' in its supported instructions` | none |
| E10 | Two backends | `@qpu("%s") differs from @qpu("%s") at %s:%d: scope A allows one backend in each program` | none |
| E11 | No pin file | `no pinned description for backend '%s' (looked in %s)` | none |
| E12 | Bad pin file | `pinned description %s is invalid: %s` | none |
| E13 | Discarded measurement value used | `the value of qir.MeasureReset in a @qpu function must be discarded: measure with qir.Mz into a qir.ResultAt slot to branch on it` | none |

Attribute diagnostics are in Section 5.4.

---

## 4. The native-gate translation

### 4.1 Rules

Target basis: `{rz, sx, x, cz}` with `measure`, `reset`, `if_else`. Circuit
order (first line applied first). `≅` means equal up to a global phase.

| Goo gate | Translation | Couplers needed |
|---|---|---|
| `H q` | `rz(π/2) q; sx q; rz(π/2) q` | — |
| `X q` | `x q` | — |
| `Y q` | `rz(π) q; x q` (Y = iXZ) | — |
| `Z q` | `rz(π) q` | — |
| `S`, `SAdj`, `T`, `TAdj` | `rz(π/2)`, `rz(-π/2)`, `rz(π/4)`, `rz(-π/4)` | — |
| `Rz(θ)` | `rz(θ)` | — |
| `Rx(θ)` | `rz(π/2); sx; rz(θ+π); sx; rz(π/2)` | — |
| `Ry(θ)` | `sx; rz(θ+π); sx; rz(π)` | — |
| `CX c,t` | `H t; cz c,t; H t` (H expanded as above) | c–t |
| `CZ a,b` | `cz a,b` | a–b |
| `Swap a,b` | `CX a,b; CX b,a; CX a,b` (expanded) | a–b |
| `CCX a,b,t` | the standard 6-CX Toffoli: `H t; CX b,t; TAdj t; CX a,t; T t; CX b,t; TAdj t; CX a,t; T b; T t; H t; CX a,b; T a; TAdj b; CX a,b` (expanded) | a–b, a–t, b–t |
| `Mz q,r` | `c[r] = measure q[q];` | — |
| `Reset q` | `reset q[q];` | — |
| `if ReadResult(r) {A} else {B}` | `if (c[r] == true) { A' } else { B' }` | — |

No peephole step merges adjacent `rz` gates or removes `rz(0)`. Every output
gate traces to one rule application, so a diagnostic and a proof can name its
source. Angles print with `%.17g` (exact `double` round trip) after folding
`θ+π` in `double` arithmetic.

### 4.2 Correctness argument

1. **Each rule is an identity of unitaries up to a global phase.** Checked
   numerically on 2026-11-27 (`decomp_check.py`, numpy 2.5.5, tolerance
   1e-12): every row above passes, `Rx`/`Ry` at eight angles including 0, π,
   −2π+0.1 and 7.5, `CX` in both orientations, `cz(0,1) = cz(1,0)`, `Swap`,
   and `CCX` on 3 qubits. A negative control (H with one `rz` dropped) is
   reported unequal, so the comparison can report a failure.
2. **A global phase on one gate is a global phase of the whole circuit.**
   Gates are applied without control in scope A (no controlled sub-circuits).
   Inside an `if_else` branch, the branches act on orthogonal classical
   outcomes after a measurement, so a per-branch phase cannot be observed.
3. **The translation is local.** It maps each op independently, preserves
   op order, leaves `measure`, `reset` and the condition bits unchanged, and
   adds no qubit. So equality of each op's channel gives equality of the
   circuit's output distribution.
4. **The coupling check runs on the translated IR,** so the claim "every
   `cz` in the output is on a coupler" holds by construction, and the check
   is the same code that the gate tests.

### 4.3 How the local gates prove it

- `qpu-translate-test` (C unit test, `tests/unit/qpu/`): repeats point 1 in
  C for every rule (2×2, 4×4, 8×8 matrices, tolerance 1e-12), with the same
  negative control. Rule-level proof, independent of any circuit.
- `qpu-equiv-probe`: for each `examples/qpu_*.goo`, dumps the source IR and
  the translated IR (`--emit-qpu-ir`), relabels physical indices to dense
  ones, and computes the **exact** probability of every classical outcome
  (branch on each measurement, no sampling) for both. The two distributions
  must agree to 1e-12. Defence in depth for point 3. Limit: ≤ 16 distinct
  qubits, which covers every existing circuit (the largest, the 12-qubit GHZ,
  uses 12).
- `qpu-qasm-golden`: byte-exact expected OpenQASM 3 text for each fixture,
  and a second compile must give identical bytes.
- Structural check in the unit test: the translated IR contains only `rz`,
  `sx`, `x`, `cz`, `measure`, `reset`, `if_else`.

What none of these proves: that IBM's compiler accepts the text, and that the
device runs it with the expected fidelity. Both are external (Section 9).

### 4.4 Simulator fix (F5)

`qir_sim.c` keeps a 16-entry table that maps the first 16 distinct pointer
values to dense slots 0..15. A 17th distinct qubit aborts with
`qir_sim: more than 16 distinct qubits (pointer %zu)` and exit status 2,
instead of aliasing. Relabelling is a permutation of qubits, so the simulated
distribution does not change. This lets a `@qpu` function that uses physical
qubit 120 run locally when it is called directly from `main`. A runtime golden
pins the abort message and status.

Also add `sx` to the simulator (`__quantum__qis__sx__body`), used only by the
equivalence probe. `src/` rule: it is exercised by a gate.

---

## 5. The `@` attribute syntax

### 5.1 Is new syntax needed?

Yes, for one construct: a modifier on a `func` declaration that names the
backend. Decision 4 fixes the spelling as `@qpu(...)`. The alternatives, for
the record:

- A magic comment (`//goo:qpu ibm_fez`, like `//go:build`): no grammar change,
  but a comment that changes meaning breaks the "comments explain why" rule
  and is invisible to the AST dump. Ruled out by decision 4.
- A package-level `const qpuBackend = "ibm_fez"` convention: no syntax, but a
  name-based convention is the same failure class as F4's `shot` heuristic.
- A function call wrapper (`qpu.Target("ibm_fez", bell)`): no syntax, but the
  check would attach to a call site, not the declaration, so one function
  could be checked against two backends.

Expression-position `@` (`@circuit(bell)`) is **not** needed. The circuit
constant is obtained with an ordinary compile-time call, `qpu.CircuitOf(bell)`,
in the same family as `qir.QubitAt(comptime id)` (Section 7.1). So scope A
changes the grammar for declaration attributes only.

### 5.2 Grammar sketch

```
top_level_decl:
      ...existing arms...
    | attribute_list func_decl            { attach $1 to ((FuncDeclNode*)$2)->annotations }
    | attribute_list func_decl SEMICOLON  { same }

attribute_list:
      attribute_item
    | attribute_list attribute_item

attribute_item:
      attribute
    | attribute SEMICOLON     /* ASI: `@qpu` or `@qpu("x")` ends a line */

attribute:                      /* parser.y:3017, unchanged */
      DEREF identifier
    | DEREF identifier LPAREN expression_list RPAREN
```

Plus:

- `lexer_bridge.c`: `case TOKEN_DEREF: return DEREF;`. After this, `@n` in an
  expression is `syntax error, unexpected '@'`, not a silent `n` (F1). A
  reject golden pins that.
- Delete the three `attribute_list ... func_signature block` arms in
  `func_decl` (F3). `func_decl` already covers `COMPTIME FUNC` and
  `UNSAFE FUNC`, so `attribute_list func_decl` keeps those forms, with the
  correct result.
- The `AST_ATTRIBUTE` list is already emitted by `program_dump.c:476`
  (`"annotations"`), so the program dump needs no schema change. The dump
  probe gets one fixture with an attribute.

Both forms parse:

```goo
@qpu("ibm_fez") func bell() { ... }

@qpu("ibm_fez")
func bell() { ... }
```

### 5.3 Meaning

`@qpu("<backend>")` on a function:

1. The function body must be in the extractor subset (Section 3.1).
2. The circuit it describes passes the target check against
   `catalogue/qpu/<backend>.json`.
3. If the function has no parameters and no results, `qpu.CircuitOf(f)` is
   legal and yields a constant (Section 7.1). A `@qpu` function with
   `qir.Qubit` / `qir.Result` parameters is a helper that other `@qpu`
   functions inline.
4. The function remains an ordinary Goo function: calling `bell()` from `main`
   runs the **source** circuit on `qir_sim`. That is how a user tests a circuit
   locally.

### 5.4 Attribute diagnostics

| Id | When | Message | help |
|---|---|---|---|
| A1 | Unknown name | `unknown attribute '@%s'` | none. Unknown attributes are refused, never ignored. |
| A2 | Not on a func | `@%s applies only to a func declaration` | none |
| A3 | `@qpu` with no backend | `@qpu needs a backend name, for example @qpu("<name>")` | `@qpu("<name>")` only when exactly one non-synthetic pin exists; otherwise none |
| A4 | Bad argument | `@qpu takes one string literal naming a pinned backend` | none |
| A5 | Repeated | `@%s is repeated on func %s` | none |
| A6 | On a method | `@qpu applies only to a top-level func, not a method` | none |

### 5.5 Conflict risk and superset argument

- `DEREF` starts only `attribute`, and `attribute` appears only at the start of
  a top-level declaration. After `attribute`, lookahead `SEMICOLON` can only
  shift into `attribute SEMICOLON`. Lookahead `FUNC`, `COMPTIME`, `UNSAFE` or
  `DEREF` reduces. So the expected delta is **0 S/R and 0 R/R** against the
  baseline measured today: `grammar-tripwire: PASS (31 S/R + 0 R/R)`. Any
  delta stops the line (goo-grammar skill).
- Risk: removing the old `func_decl` attribute arms. They are unreachable
  today (F1), so no accepted program changes. The golden suite and the
  superset ratchet prove that.
- Risk: mapping `DEREF` makes the mock intrinsics of F2 reachable in the
  grammar only if an expression rule uses `DEREF`. None does. A reject golden
  for `@struct_fields(T)` pins that it stays a syntax error.
- Superset: Go's lexer has no `@` token, so no valid Go program contains one.
  The change gives meaning only to programs that are invalid Go. The one
  behaviour change for existing Goo input is that a stray `@` is now an error
  instead of being dropped, which fixes a silent acceptance.
- Also worth a separate ticket (not in this project): `bridge_next_mapped`
  drops **every** unmapped token, not only `@`.

---

## 6. The `http.Do` C shim

### 6.1 Package and Goo signature

```goo
import "goo/http"   // package name: http (see Q3 for the path)

type Response struct {   // seeded by the compiler, fields only, no methods
	Status int           // HTTP status code; a 4xx or 5xx is NOT an error
	Header []string      // "Name: value" lines, in the order received
	Body   string        // binary-safe, capped at 16 MiB
}

// method: "GET", "POST" or "DELETE"; url: must start with "https://"
func Do(method string, url string, headers []string, body string, timeoutSeconds int) !Response
```

Why flat parameters and not a `Request` struct: a `http.Request{...}` literal is
a qualified composite literal, which does not parse today (probes `t10`,
`u10`). Flat parameters also use only existing shim parameter kinds
(`SHIM_PARAM_STRING`, `SHIM_PARAM_STRING_SLICE`, `SHIM_PARAM_INT64`).

Why a seeded struct with **fields and no methods**: a seeded type with a method
set needs a bespoke seeder plus an adapter, and every local of such a type is
unreleasable (CLAUDE.md, CALLEE_VALUE ceiling). A field-only struct needs one
new tag, `SHIM_RET_HTTP_RESULT`, beside `SHIM_RET_STRING_RESULT`. Fallback if
the struct-in-error-union ABI costs too much: return `!string` with a framed
payload (`<status>\n<header lines>\n\n<body>`) and parse it in Goo. That is
simpler in C but puts a framing format between the two halves.

### 6.2 Error model

`!Response` (an error union with a string error), the same convention as
`os.ReadFile` and `strconv.Atoi` in `shim_signatures.c`. Probe `s08` shows that
Go-style code can destructure it: `data, err := os.ReadFile(p)` compiles and
runs. So `ibmq` reads like Go (`resp, err := http.Do(...)`) while the shim uses
the existing machinery.

| Option | For | Against |
|---|---|---|
| **`!Response`** (recommended) | Matches the shim precedent; destructures as `(v, err)`; no new error ABI. | Not Go's `net/http` shape. |
| `(Response, error)` | Go shape. | Shims have no multi-result return kind today; a new ABI for one call. |
| Go's `net/http` API | Full Go compatibility. | Needs `context`, `io.Reader`, qualified literals, method sets: far beyond "thin". |

Error text is `http: <class>: <detail>`, where `<class>` is stable and tests
match on it:

| class | libcurl result |
|---|---|
| `invalid request` | refused before libcurl: bad method, a URL that is not `https://`, a header with CR, LF or NUL, a header with no `": "`, timeout outside 1..600 |
| `timeout` | `CURLE_OPERATION_TIMEDOUT` |
| `connect` | `CURLE_COULDNT_RESOLVE_HOST`, `CURLE_COULDNT_CONNECT` |
| `tls` | `CURLE_PEER_FAILED_VERIFICATION`, `CURLE_SSL_*` |
| `response too large` | write callback abort at 16 MiB |
| `transport` | any other `CURLcode` (text from `curl_easy_strerror`) |

The detail names the URL's scheme and host, never a header or the body,
because headers carry the bearer token.

### 6.3 C side (`src/runtime/http_shim.c`)

One easy handle per call (no connection reuse in scope A; one job makes about
10 requests). `curl_global_init` once through `pthread_once`. Options set
explicitly, so that a gate can grep for each:

- `CURLOPT_PROTOCOLS_STR "https"`, `CURLOPT_FOLLOWLOCATION 0` (a redirect must
  not carry the bearer token to another host; a 3xx comes back as a status).
- `CURLOPT_SSL_VERIFYPEER 1`, `CURLOPT_SSL_VERIFYHOST 2`,
  `CURLOPT_SSLVERSION CURL_SSLVERSION_TLSv1_2` as the minimum.
- `CURLOPT_CAINFO` from `GOO_HTTP_CAFILE` only when that variable is set.
  This **adds** a trust anchor for the loopback test. It does not turn
  verification off. No option or variable turns verification off.
- `CURLOPT_CONNECTTIMEOUT_MS min(10000, timeout)`, `CURLOPT_TIMEOUT_MS timeout`,
  `CURLOPT_NOSIGNAL 1` (the runtime is multi-threaded).
- `CURLOPT_POSTFIELDS` with `CURLOPT_POSTFIELDSIZE_LARGE` (binary-safe body),
  `CURLOPT_CUSTOMREQUEST` for `DELETE`.
- Write and header callbacks into growable buffers, abort at 16 MiB.
- `CURLOPT_USERAGENT "goo-http/1"`.

The call blocks its OS thread. In scope A one goroutine calls it, so the
blocked scheduler worker is acceptable. This is stated as a limit, not hidden.

### 6.4 Testing without the network

`http-shim-probe` (`scripts/http_shim_probe.sh`) starts a loopback HTTPS server
(`python3 -I` with the `ssl` and `http.server` standard modules, both present;
OpenSSL 4.0.3) on port 0 and passes the port to a Goo test program. The TLS
material is a committed test CA and server certificate in
`tests/http/tls/` (30-year validity, `CN=127.0.0.1`, SAN `IP:127.0.0.1`).

| Case | Expected |
|---|---|
| GET 200 with a body that contains NUL bytes | `Status == 200`, body bytes equal |
| POST echo | server sees the exact body bytes and header lines |
| 401 | `Status == 401`, `err == nil` |
| server sleeps longer than the timeout | `http: timeout` within timeout + 2 s |
| CA file not set (system trust only) | `http: tls` (proves verification is on) |
| certificate for another name | `http: tls` |
| `http://` URL | `http: invalid request` |
| 302 to another port | `Status == 302`, the second server sees no request |
| header with `\r\n` | `http: invalid request`, server sees no request |
| 17 MiB body | `http: response too large` |

`--self-test`: the positive control. The TLS case with `GOO_HTTP_CAFILE` set
must **succeed**, and the probe must fail when the expected class strings are
swapped. Without the control, a server that never starts reads the same as
"TLS refused" (memory note: a negative result needs a positive control).

### 6.5 Linking

The driver adds `curl` to `codegen->link_libs` only when the program imports
the shim, so programs that do not import it keep today's link line and their
reproducible-build hashes. `src/runtime/BUILD` (Bazel) needs the same
conditional. CI already builds `bin/goo` with `-lcurl`, so the CI image has
the library.

---

## 7. IAM, REST and JSON in Goo source

### 7.1 The compiler-seeded `qpu` package

Small, compiler-known, Goo-only:

```goo
import "qpu"

type Circuit   // opaque, copyable constant: no ARC (RELEASE_NO_NOT_OWNED, like qir.Result)
type Lease     // LINEAR: must be consumed exactly once
type Ticket    // copyable: what a consumed lease leaves

func CircuitOf(f func()) Circuit               // compile time: f must be a @qpu func with no params/results
func NewLease(c Circuit, comptime maxSeconds int) Lease   // maxSeconds in [1, 10800], else error
func Spend(l Lease) Ticket                     // consumes l
func Release(l Lease)                          // consumes l without a job
func Source(c Circuit) string                  // translated OpenQASM 3 text
func Backend(c Circuit) string
func PinSummary(c Circuit) string              // canonical summary, Section 2.1
func NumClbits(c Circuit) int
func TicketCircuit(t Ticket) Circuit
func TicketSeconds(t Ticket) int
```

Accessors are package functions, not methods: they fit the declarative shim
table and avoid the method-set ceiling. The cost is that the API reads less
like Go.

`qpu.CircuitOf(bell)` lowers to a pointer to a private constant global:
`{ qasm text, length, backend name, pin summary, n_clbits }`. This is decision
3's "circuits are embedded as constants". The QASM text is the output of
Section 4, so the text that is sent is the text that was checked.

### 7.2 The linear lease

`qir_ownership.c` gains a second linear kind. The binding table keeps its
states (`LIVE`, `MOVED`, `CONSUMED`). What differs is the parameter rule:

| | `qir.Qubit` | `qpu.Lease` |
|---|---|---|
| Producer | `qir.Alloc`, `qir.QubitAt` | `qpu.NewLease` |
| Consumer | `qir.FreeZero`, `qir.MeasureReset` | `qpu.Spend`, `qpu.Release` |
| Passed to a user function | borrowed (stays live in the caller) unless the callee returns it | **moved**: the caller's binding is consumed, the callee must consume it on every path |
| Container, closure, `go`, `defer` | refused | refused (same obligations 1 and 6) |
| Consumed in a loop | refused for an outer binding | refused for an outer binding |

What the lease proves, and what it does not:

- Proven at compile time: each lease is either spent once or released once,
  on every path. So one `NewLease` site gives at most one job per execution
  of that site, and a lease cannot be dropped by mistake.
- Proven at compile time: the circuit, its backend and its `maxSeconds` travel
  together from the compiler-made constant to the request body. The client
  cannot send a checked circuit to a backend that it was not checked against.
- **Not** proven: a bound on jobs per program run, because a `NewLease` inside
  a loop or a recursive function runs many times. Q5 asks whether to refuse
  `qpu.NewLease` inside a loop body.

### 7.3 Request flow

Package `ibmq` (Goo source, `goostd/ibmq/`; path in Q3). Entry point:

```goo
// Run consumes lease. With GOO_QPU_LIVE unset or not "1" it is a dry run.
func Run(lease qpu.Lease, shots int) (Result, error)

type Result struct {
	DryRun  bool
	JobID   string
	Backend string
	Shots   int
	Counts  []Count   // sorted by bitstring, c[n-1]...c[0] left to right
}
type Count struct { Bits string; N int }
```

User program:

```goo
package main

import (
	"fmt"
	"ibmq"
	"os"
	"qir"
	"qpu"
)

// Assumes the pin joins physical qubits 3 and 4; if it does not, E2 refuses
// qir.CX at compile time.
@qpu("ibm_fez")
func bell() {
	a := qir.QubitAt(3)
	b := qir.QubitAt(4)
	ra := qir.ResultAt(0)
	rb := qir.ResultAt(1)
	qir.H(a)
	qir.CX(a, b)
	qir.Mz(a, ra)
	qir.Mz(b, rb)
	qir.Reset(a)
	qir.FreeZero(a)
	qir.Reset(b)
	qir.FreeZero(b)
}

func main() {
	lease := qpu.NewLease(qpu.CircuitOf(bell), 10)
	res, err := ibmq.Run(lease, 1024)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	for _, c := range res.Counts {
		fmt.Println(c.Bits, c.N)
	}
}
```

Steps inside `Run` (live mode). Environment: `IBM_QUANTUM_TOKEN` (API key) and
`IBM_QUANTUM_INSTANCE` (the instance CRN), the same names as the Python script.

1. **Read the key** from the environment. Never from argv, never logged.
   Missing → `qpu.Release(lease)` and an error.
2. **IAM.** `POST https://iam.cloud.ibm.com/identity/token`,
   `Content-Type: application/x-www-form-urlencoded`, `Accept: application/json`,
   body `grant_type=urn:ibm:params:oauth:grant-type:apikey&apikey=<percent-encoded key>`.
   Read `access_token`, `expires_in` (3600 per the docs) and `expiration` if
   present. The token is refreshed when it has less than 60 s left, and once
   after a 401.
3. **Stale check.** `GET https://quantum.cloud.ibm.com/api/v1/backends/<b>/configuration`,
   canonical summary, compare (Section 2.3). Mismatch → release, error.
4. **Spend.** `t := qpu.Spend(lease)`. From here no path releases.
5. **Create.** `POST https://quantum.cloud.ibm.com/api/v1/jobs`. Headers on
   every API call: `Authorization: Bearer <token>`, `Service-CRN: <crn>`,
   `IBM-API-Version: 2026-04-15`, `Accept: application/json`, and
   `Content-Type: application/json` on a POST. Body bytes (keys in this order,
   no spaces):
   ```
   {"program_id":"sampler","backend":"ibm_fez","cost":10,"params":{"version":2,"support_qiskit":false,"pubs":[["<qasm text, JSON-escaped>",null,1024]]}}
   ```
   `program_id` and `backend` are the required fields of `JobRunParamsBase`.
   `cost` is the job's time "in seconds" per the jobs page. `version: 2` is
   required for SamplerV2. `support_qiskit: false` asks for plain JSON instead
   of the Qiskit result-decoder format. The PUB form is
   `(Circuit, Parameters, Shots)`.
6. **Poll.** `GET /v1/jobs/{id}` from 2 s, interval × 1.5 to a cap of 30 s.
   Read `state.status`, else `status`. Terminal states: `Completed`,
   `Cancelled`, `Cancelled - Ran too long`, `Failed` (the `JobStatus` enum).
   A 429 waits for `Retry-After` (parsed from `Response.Header`) or 30 s. A
   5xx retries the poll three times. The local deadline (default 30 min,
   `GOO_QPU_DEADLINE_S`) sends `POST /v1/jobs/{id}/cancel`, so that a queued job
   does not run later and spend quota, then returns an error with the job ID.
   Timing uses `time.Now().UnixNano()` and `time.Sleep`, not `select` with
   `time.After` (Section 8, item 22).
7. **Results.** `GET /v1/jobs/{id}/results`, decode, count (Section 7.4).

**Dry run** (the default, as in the Python script): steps 2, 3 and 5–7 make no
connection. `Run` prints the exact planned requests to stdout (method, URL,
header names, and the job body bytes; the IAM body prints as
`apikey=<redacted>`), releases the lease, and returns `Result{DryRun: true}`.
The stale check prints `stale check: skipped (dry run)`. A dry run needs no
credentials.

Base URLs can be overridden for the replay gate with `GOO_IBMQ_IAM_URL` and
`GOO_IBMQ_API_URL`. Both must be `https://`. The dry run prints the effective
hosts, so a redirected key cannot go unnoticed.

### 7.4 JSON, results and counts

No `encoding/json` exists (struct tags do not parse, and there is no
reflection, Section 8). `ibmq` carries a small JSON package of its own,
`goostd/ibmq/json.go`, written in the subset that compiles today:

```goo
type Kind int
const ( Null Kind = iota; Bool; Number; String; Array; Object )
type Value struct {
	Kind  Kind
	B     bool
	Num   string     // the number's source text, like json.Number
	Str   string
	Items []*Value
	Keys  []string   // objects keep key order: deterministic re-encoding
	Vals  []*Value
}
func Parse(s string) (*Value, error)
func (v *Value) Get(key string) *Value      // nil when absent or not an object
func (v *Value) Int() (int, error)          // exact; refuses fractions and overflow
```

- Decoder: strict RFC 8259. Depth limit 64. Refuses trailing data. Handles
  `\uXXXX` with surrogate pairs and encodes UTF-8 by hand (`utf8.AppendRune`
  is missing). Numbers stay text, so no float parser is needed
  (`strconv.ParseFloat` is missing).
- Encoder: an ordered writer over `strings.Builder` (works, probe `s01`). Its
  own string escaper (`"`, `\`, control bytes as `\u00XX`). `strconv.Quote`
  works but produces Go escapes (`\x..`, `\a`), which are not JSON.
- Percent-encoding for the IAM form: its own function over the RFC 3986
  unreserved set, because `net/url` is missing.

Result decoding, **assumed** shape (see risk R1):

```json
{"results":[{"data":{"c":{"samples":["0x0","0x3"],"num_bits":2}},"metadata":{}}],"metadata":{}}
```

`c` is the register that the extractor always emits. Each sample is a hex
string, parsed by hand (probe `t24`), and rendered as a `num_bits`-wide
bitstring with `c[n-1]` first (Qiskit's convention, Q10). A response with
`"schema_version"` or base64 tensor data (both appear in the 2026-04-15 jobs
reference) is refused with `ibmq: unsupported results schema '<v>'`, never
guessed at.

### 7.5 Canonical summary in two languages

The compiler (C, json-c) and `ibmq` (Goo) both build the canonical summary.
`qpu-pin-parity-probe` feeds the same configuration fixtures through
`bin/goo --qpu-pin-summary <file>` and `goo run tools/qpu_pin -- --summary <file>`
and requires equal bytes. Without this gate, a stale check could compare two
different canonical forms and never fire (memory note: a differential harness
must check its producers' exit status first).

---

## 8. The stdlib that this needs

Method: one small program for each construct, compiled with
`GOOROOT=<repo> ./bin/goo -o <out> <file>` and run. **Every program was also
run with Go 1.27.4 (`go run`), and all exit 0 under Go**, so each failure below
is a Goo gap and not a mistake in the probe. Gap kinds: **P** parser (or
lexer), **T** type checker, **M** missing package or member, **C** codegen or
runtime gives a different result from Go (silent, the most dangerous kind).

`scripts/check_stdlib_coverage.sh` passes today: 202 symbols (61 shim rows,
87 goostd functions, 26 goostd methods, 15 seeded methods, and so on). It
counts what exists. It does not test the members below that do not exist.

### 8.1 Needed by this design, works today

| # | Go package / feature | Probe | Result |
|---|---|---|---|
| 1 | `strings.Builder` `WriteString`, `String` | s01 | PASS |
| 2 | `strings.Split`, `Join`, `Contains`, `HasPrefix`, `HasSuffix`, `Index`, `TrimSpace`, `ReplaceAll`, `TrimPrefix`, `Repeat`, `Fields`, `ToLower`, `EqualFold`, `Count` | s01, x02 | PASS |
| 3 | `strconv.Itoa`, `Atoi`, `Quote`, `FormatInt(i, 16)` | t19 | PASS |
| 4 | `bytes.Buffer` `Write`, `WriteString`, `String`, `Len` | t18 | PASS |
| 5 | `fmt.Sprintf` verbs `%d %s %v %t %%` | x01 | PASS |
| 6 | `fmt.Errorf("%w")`, `errors.New`, `errors.Is`, `errors.Unwrap` | t04 | PASS |
| 7 | `os.Getenv`, `os.ReadFile` (as `data, err :=`), `os.Stderr`, `os.Args`, `os.Exit` | s08 | PASS |
| 8 | `time.Now`, `time.Sleep`, `time.Millisecond`, `Time.UnixNano`, `<-time.After(d)` alone | u03 | PASS |
| 9 | `sort.Strings`, `sort.Sort` on a user type | s15, t28 | PASS |
| 10 | `unicode/utf8.DecodeRuneInString`, `RuneCountInString`, non-ASCII string literal | t15 | PASS |
| 11 | `any` type switch over `nil`, `bool`, `float64`, `string`, `[]any`, `map[string]any` | t17 | PASS |
| 12 | map comma-ok, type-assertion comma-ok, `delete` | v06, t23 | PASS |
| 13 | multi-return `(T, error)`, named results, bare `return` | s22, v05 | PASS (print differs, item 27) |
| 14 | `const` + `iota`, methods on a named int | s21 | PASS (print differs, item 26) |
| 15 | pointer-receiver methods, `&T{}` local literal, recursion, labelled `break`/`continue`, variadic and `xs...` | s23, u17, u14, s24 | PASS |
| 16 | `[]byte(s)`, `string(b[i:j])`, `s[i]`, `copy`, `append(a, b...)`, `append(a, "s"...)` | t13, u12, t12 | PASS |
| 17 | local struct literals with slice and map fields; slice of `*Node` | u11, s28, u16 | PASS |
| 18 | multi-line raw string `const` (the circuit constant shape) | s19 | PASS |
| 19 | user interface satisfied by a pointer-receiver method | v04 | PASS |
| 20 | error union `!T` with `catch e { }`; `return error("...")` | s26 | PASS |
| 21 | `math.Pi`, `math.Abs` | t20 | PASS |

### 8.2 Gaps the design works around (fixing them makes the client read like Go)

Ranked by how much each costs the client code. "Workaround" is what `ibmq`
does instead.

| # | Go feature | Probe | Kind | Observed | Workaround in this design |
|---|---|---|---|---|---|
| 22 | `select` on a ready channel and `time.After` | t26, u01 | **C** | Goo takes the **timer** branch after the full 2 s, although the sender was ready after 5 ms. Go takes `done`. | Poll with `Sleep` + `UnixNano` deadline. No `select` in `ibmq`. |
| 23 | user type with `Error()` used as `error` (return, assign) | t27, u05, u06 | **T** | `return type mismatch: cannot return *HTTPError ...`; `Cannot assign *HTTPError to error`. Value receivers fail too. | Status stays in `Response.Status`; errors are `fmt.Errorf` with `%w` on sentinel errors. |
| 24 | three or more names in `:=` / `=` (`a, b, ok := f()`, `a, b, c := 1, 2, 3`) | u04, v01, v02, t30 | **P** | `syntax error, unexpected ','` at the second comma. Two names work. | Functions return at most two values, or a struct. |
| 25 | `case a, b:` in `switch` | v03, u15 | **P** | `syntax error, unexpected ','`. | `if`/`else if` chains, or one `case` each. |
| 26 | `fmt.Println` / `%v` of a `Stringer` | s21, u07, t05 | **C** / **T** | Prints the underlying int (`1`), Go prints `String()` (`Running`). `%s` on a Stringer is refused at compile time. | Call `.String()` explicitly. |
| 27 | `fmt.Println` of a nil `error` among several arguments | s22 | **C** | Prints nothing for it; Go prints `<nil>`. Alone it prints `<nil>` (u07). | Do not print errors that can be nil. |
| 28 | `fmt.Println` / `%v` / `%f` of `float64` | t25, u09, x01 | **C** | `0.785398` where Go prints `0.785398163397448`; `%f` gives `3.14159` where Go gives `3.141590`. | No float printing in `ibmq`. Angles are printed by the compiler in C. |
| 29 | qualified composite literal `pkg.T{}`, `&pkg.T{}` | t10, u10, s11 | **P** | `syntax error, unexpected '{', expecting '}'`. | `var b strings.Builder`; flat `http.Do` parameters. |
| 30 | `append(s, x, y)` with two or more elements | t11, s20 | **T** | `append expects exactly two arguments (slice, element)`. | One `append` per element. |
| 31 | `fmt` verbs `%x %q %g %e %c` and any width or precision (`%5.2f`, `%.2f`) | t01, t02, t03, x01, x04 | **C** (compile-time verb table in `call_codegen.c`, which handles `% d f s t v w` only) | `unknown format verb`. | Own hex parsing, own JSON escaper. |
| 32 | `strconv.ParseInt(s, base, bits)` | s02 | **M** (API differs) | The vendored function is `ParseInt(s string) !int64` (`goostd/strconv/strconv.go:195`). A Go call does not compile. | `Atoi`, own hex parse. |
| 33 | `strconv.ParseUint`, `ParseFloat`, `FormatFloat` | s03, s04 | **M** | `Package 'strconv' has no member`. | Numbers kept as text. |
| 34 | `time.Since`, `Time.Sub`, `Time.Unix`, `Time.Add`, `Time.Before`/`After`; `time.Duration(n)` conversion | s09, t06, t07, t08, t09, u08 | **M** / **T** | `has no member 'Since'`; `Struct has no field or method 'Sub'`; `Cannot call non-function type Duration`. | `UnixNano()` arithmetic in `int64`. |
| 35 | `errors.As` | s07 | **M** | `has no member 'As'`. | Not needed with item 23's workaround. |
| 36 | `bytes.Buffer.WriteByte` | s05 | **M** | `Struct has no field or method 'WriteByte'`. | `strings.Builder` + `WriteString`. |
| 37 | `strings.IndexByte`, `LastIndex` | t29, x03 | **M** | `has no member`. `strings.Cut` is untested (masked by item 24). | `Index` with a one-byte string. |
| 38 | non-ASCII rune literal `'é'` | t14, s13 | **P** (lexer) | `invalid character literal`. A non-ASCII **string** literal works. | Not needed. |
| 39 | `utf8.ValidString`, `utf8.AppendRune`, `utf8.RuneError` | t16, s14 | **M** | `has no member`. | Own UTF-8 encoder (about 15 lines). |
| 40 | `sort.Slice` | s16 | **M** | `has no member 'Slice'`. | `sort.Sort` on a named slice type. |

### 8.3 Gaps the design avoids entirely

| # | Go package | Probe | Kind | Observed | Why this design does not need it |
|---|---|---|---|---|---|
| 41 | `encoding/json` (`Marshal`, `Unmarshal`) | s10 | **P** first, then **M** | Struct field tags fail to parse (s17: `unexpected string literal`); the package does not exist; it also needs reflection. | `ibmq/json.go` (Section 7.4). |
| 42 | struct field tags `` `json:"x"` `` | s17 | **P** | `syntax error, unexpected string literal`. | No tags used. |
| 43 | `net/url` (`QueryEscape`, `Values`) | s11 | **M** (+ item 29) | `cannot resolve import`. | Own percent-encoder. |
| 44 | `net/http` | s12 | **M** | `cannot resolve import "net/http"`. | The Goo-only `http.Do` shim (Section 6). |
| 45 | `encoding/base64` | s25 | **M** | `cannot resolve import`. | Plain-JSON results (`support_qiskit: false`). Needed later if IBM moves Sampler results to base64 tensors (R1). |
| 46 | `math.Floor`, `math.Mod` | s29 | **M** | `has no member 'Floor'`. | Angle arithmetic is in the compiler. |
| 47 | comptime reflection (`@struct_fields`, `@fields`) | s27 | **P** + mock | `Undefined variable 'struct_fields'` (the `@` is dropped, F1); the implementation is a mock (F2). | Hand-written JSON. |
| 48 | `@` declaration attributes | a01–a03 | **P** | `syntax error, unexpected identifier` at the name after `@`. | Fixed by Section 5 (this project). |

### 8.4 Recommendation for the Go-compatibility work

None of items 22–48 **blocks** scope A: `ibmq` can be written in the subset of
8.1. The cost of each workaround is client code that reads unlike Go. If the
Go-compatibility track does some of these first, this is the order that helps
both that track and this project most:

1. Item 22 (`select` + `time.After`) and items 26–28 (silent print
   differences): a valid Go program compiles and gives a **different** result.
   That is the worst failure under the superset rule.
2. Items 24, 25, 29 (parser): common Go shapes. They also gate a large part of
   the superset corpus.
3. Item 23 (user error types): blocks idiomatic error handling in every
   package.
4. Items 30, 32, 34 (`append` arity, `ParseInt` signature, `time` methods).
5. Items 41–42 (`encoding/json` with tags): large. Not needed here.

Each fix needs a golden whose expected output comes from `go run`
(goo-grammar skill, step 6). The probe programs in Appendix A are ready to
become those goldens.

---

## 9. Gates

Every claim of the feature, the local gate that proves it, and what only a
live run shows. All new probes have a `--self-test` and a parity-count bump
in the same commit (memory note: adding a goolang gate).

| Claim | Gate | Kind |
|---|---|---|
| `@qpu` parses on one line and before a line break; both give the same AST | `test-golden` fixtures + `program-dump-probe` fixture | local |
| `@x` in an expression and an unknown `@name` are refused | `test-golden-reject` fixtures `attr_*_reject.goo` | local |
| The grammar change adds no conflict | `scripts/grammar-tripwire.sh` (31 S/R + 0 R/R) | local |
| Every pin file is valid and canonical | `qpu-pin-probe` (self-test: three corrupted copies must fail) | local |
| C and Goo produce the same canonical summary | `qpu-pin-parity-probe` | local |
| Out-of-range qubit, missing coupler, CCX without triangle, Alloc, non-constant angle, nested if, two backends, bad pin are refused with the exact diagnostics E1–E13, A1–A6 | `test-golden-reject` fixtures `qpu_*_reject.goo` + `.err.txt`; `diagnostics-drift-probe`; `diag-format-probe` with `help/*.fixed.goo` for E7 and A3 | local |
| Each translation rule is exact up to global phase | `qpu-translate-test` (C unit, negative control) | local |
| The translated circuit gives the same outcome distribution as the source | `qpu-equiv-probe` (exact enumeration, ≤ 16 qubits; self-test: a fixture with one swapped gate must differ) | local |
| The emitted OpenQASM 3 text is exact and deterministic | `qpu-qasm-golden` | local |
| The simulator no longer aliases qubit 16 onto 0 | runtime golden (`w01` shape) + abort-message golden | local |
| Lease: dropped, spent twice, spent in a loop, captured, sent to `go`, stored — all refused | `test-golden-reject` fixtures `qpu_lease_*_reject.goo` | local |
| `http.Do`: TLS verification is on, timeouts fire, no redirect, header injection refused, size cap | `http-shim-probe` (loopback HTTPS, positive control) | local |
| Only programs that import the shim link libcurl | probe: `ldd` of a non-importing program lists no libcurl, of an importing one lists it (positive control both ways) | local |
| The IAM form body and the job body are the exact bytes of Section 7.3 | `ibmq-body-probe`: dry-run stdout golden for the fixtures | local |
| JSON parse of recorded IAM, job-create, job-status and results responses; counts decode | `goo test goostd/ibmq` + `ibmq-replay-probe` (loopback HTTPS replays the sequence and checks every request it receives: method, path, header lines, body) | local, **on synthetic recordings until Q2** |
| Token refresh after 401, `Retry-After` on 429, cancel at the local deadline, `Failed` and `Cancelled - Ran too long` reported | `ibmq-replay-probe` scenarios | local, synthetic |
| A dry run makes no connection | replay server counts zero connections for a run without `GOO_QPU_LIVE=1` | local |

Each recorded response file carries a `"_provenance"` line: `synthetic, from
docs <URL> read <date>` or `recorded from job <id> on <date>`. A gate on a
synthetic recording proves the client agrees with the **documentation**, not
with the server.

**External, not gated** (only a live run can show these; each needs fresh
approval):

- IBM's IAM accepts the form body, and the API accepts the headers and the
  `IBM-API-Version: 2026-04-15` value.
- IBM accepts Goo's native-gate OpenQASM 3 text (F7: never shown).
- The server enforces `cost` as the time limit (the `Cancelled - Ran too long`
  status suggests so; no run has shown it).
- The live results response has the assumed shape (R1).
- Fidelity of any circuit on hardware, and every job ID.
- The stale check fires on a real configuration change.

README rows from this project get the label "external, not gated" next to the
job ID, as decision 5 requires.

---

## 10. Risks and open questions

### 10.1 Risks, ranked

| Rank | Risk | Likelihood / impact | Mitigation |
|---|---|---|---|
| R1 | **Results schema.** The Sampler result format is not confirmed from a live response. The guide that showed it now returns 404. The 2026-04-15 jobs reference mentions `schema_version` and base64 tensors. | High / high: the client can submit and then fail to read its own result. | Refuse unknown schemas loudly (Section 7.4). Record one real results response (Q2) before the first submission. Keep `encoding/base64` on the Go-compat list. |
| R2 | **IBM rejects the Goo text** (F7). | Medium / high | First live run is one 2-qubit Bell circuit with `cost` 10 s. `if (c[k] == true)` is the spelling of the existing emitter. |
| R3 | **Silent Go divergences in the client** (items 22, 26–28). | Medium / high: wrong output with exit 0. | Client avoids those constructs. The replay gate checks outputs byte for byte, so a new divergence in a construct the client uses shows up as a gate failure. |
| R4 | **Pin staleness that the structure compare does not see** (a faulty qubit or coupler). | Medium / medium: valid job, bad data. | Q4 (properties check). Results always print backend and pin provenance. |
| R5 | **Grammar**: ASI after an attribute, and the removal of the old arms. | Low / medium | Section 5.5; tripwire; goldens for both line forms. |
| R6 | **Quota**: a client bug that submits in a loop. | Low / high (600 s a month). | Linear lease, dry run by default, `GOO_QPU_LIVE=1`, `cost`, Q5 loop rule. |
| R7 | **Secrets**: API key in a log, token sent to another host. | Low / high | Key only from the environment; no header in any error text; no redirects; `https` only; dry run redacts the key and prints hosts. |
| R8 | **Linking libcurl** into user programs changes reproducible-build hashes or the Bazel graph. | Low / medium | Link only when imported (Section 6.5); `repro-build-probe` unchanged for other programs. |
| R9 | **Two emitters** (`--emit-qasm3` and the extractor) drift. | Medium / low | Q8. The README says which path made each row. |
| R10 | **API version deprecation** (`IBM-API-Version` pinned). | Medium / medium over months | One constant in `ibmq`; the replay fixtures carry the version; a 4xx naming the version maps to a clear error. |

### 10.2 Open questions for the user

- **Q1.** First real pin: may one credentialed `GET /v1/backends/<b>/configuration`
  run (no QPU time), or will you supply the configuration JSON? Which backend:
  `ibm_fez` (the most recent live rows) or another?
- **Q2.** May one credentialed `GET /v1/jobs/dc2o7nts2omc738cabp0/results` run
  (no QPU time) to record a real results response for R1?
- **Q3.** Import paths. Proposed: `qpu` (bare, like `qir`), `goo/http`
  (a bare `http` would shadow a user's local package named `http`, because a
  bare import resolves GOOROOT first), and `ibmq` for the client. Shim paths
  are single words today (`goo.c:851`), so `goo/http` needs a small resolver
  change.
- **Q4.** Should scope A also check the live **properties** for the qubits and
  couplers the circuit uses, and refuse a non-operational one?
- **Q5.** Refuse `qpu.NewLease` inside a loop body, so that the number of jobs
  for each run is bounded at compile time?
- **Q6.** May `verify-core` use the Go toolchain for a cross-check lane (Go's
  `encoding/json` parses the body bytes that `ibmq` writes)? Go 1.27.4 is on
  this machine; CI availability is unknown.
- **Q7.** Decision 1 says Python is replaced "entirely". Scope A does not
  replace the `fake` mode (noise-model simulation) that made the `Fake*` README
  rows. Delete `scripts/qasm3_submit_ibm.py` after the first live run, or keep
  it for those rows only?
- **Q8.** Should `--emit-qasm3` later move onto the new extractor (and lose its
  heuristics), or stay frozen for the existing probes?
- **Q9.** Confirm that fractional gates (`rx`, `rzz`) stay out of scope A.
- **Q10.** Confirm the bit order of printed counts: `c[n-1]` first (Qiskit's
  convention, matching the Python script's `get_counts()`).
- **Q11.** Instance region: `quantum.cloud.ibm.com` (us) or
  `eu-de.quantum.cloud.ibm.com`? The CRN decides; a config field or the
  environment?

---

## 11. Order of work

Each step ends with its gates green in `verify-core`. Steps 1–5 need no
network and no credentials.

0. **(Optional, other track)** Go-compat items 22, 24, 25, 29 from
   Section 8.4. Not a prerequisite.
1. **Attribute grammar** (goo-grammar skill): bridge maps `DEREF`, new
   `attribute_item` arms, old arms removed, attribute validation pass, reject
   goldens for A1–A6 and for `@x` in an expression. Tripwire before and after.
2. **Pins**: pin schema, json-c loader, `--qpu-pin-summary`, synthetic
   heavy-hex pin, `qpu-pin-probe`.
3. **Extractor**: `src/types/qpu_circuit.c`, the IR, `--emit-qpu-ir`, E4, E6,
   E7, E13 goldens. Simulator relabelling fix and `sx` (Section 4.4).
4. **Target check + translation**: `qpu_target.c`, `qpu_translate.c`, QASM
   printer, `qpu-translate-test`, `qpu-equiv-probe`, `qpu-qasm-golden`, E1–E3,
   E5, E8–E12 goldens, `diagnostics.tsv` rows.
5. **`qpu` package and the lease**: seeding, `CircuitOf` constant emission,
   the second linear kind in `qir_ownership.c`, lease reject goldens.
6. **`http.Do`**: shim row and `SHIM_RET_HTTP_RESULT`, `http_shim.c`,
   conditional `-lcurl` (Make and Bazel), test CA, `http-shim-probe`, link
   probe.
7. **`ibmq`**: JSON package, percent-encoder, IAM, REST, polling, dry run,
   `tools/qpu_pin`; `goo test goostd/ibmq`, `ibmq-body-probe`,
   `ibmq-replay-probe` on synthetic recordings, `qpu-pin-parity-probe`.
8. **Recorded fixtures (needs approval, Q1 and Q2)**: real pin, real results
   response; replace the synthetic recordings where a real one exists.
9. **First live run (needs approval)**: the Bell fixture, 1024 shots,
   `cost` 10. Record the job ID in the README as "external, not gated".
10. **Retire the Python path** for what scope A covers (Q7), and update the
    README quantum section and stdlib coverage counts.

---

## Appendix A — Probe programs (2026-11-27)

Each probe is one `package main` file. Unless the row says otherwise, the
probe's `main` prints the value of the construct. All pass under Go 1.27.4.

| Id | Construct |
|---|---|
| a01 / a02 / a03 | `@qpu` / `@qpu("ibm_fez")` / `@qpu` on a func with an `int` result |
| s01 | `strings.Builder`, `Split`, `Join`, `Contains`, `HasPrefix`, `HasSuffix`, `Index`, `TrimSpace`, `ReplaceAll`, `TrimPrefix`, `Repeat`, `Fields` |
| s02 | `strconv.Atoi`, `Itoa`, `Quote`, `ParseInt("ff", 16, 64)`, `FormatInt(255, 16)` |
| s03 | `strconv.ParseUint("3", 16, 64)` |
| s04 | `strconv.ParseFloat("1.5e3", 64)`, `FormatFloat(x, 'g', 17, 64)` |
| s05 | `bytes.Buffer.WriteByte` |
| s06 | `fmt.Errorf %w` + `Sprintf("%q %x %d %s %v %5.2f")` |
| s07 | `errors.As` with a `*HTTPError` |
| s08 | `os.Getenv`, `data, err := os.ReadFile`, `fmt.Fprintln(os.Stderr)`, `os.Exit` |
| s09 | `time.Since`, `Time.Unix`, `Time.Add`, `Time.Before` |
| s10 / s17 | `encoding/json` with tags / a struct with tags only |
| s11 | `net/url.QueryEscape`, `url.Values{}` |
| s12 | `net/http.StatusOK` |
| s13 / s14 / t14 | `utf8` on `'é'` / `utf8.AppendRune`, `RuneError` / `r := 'é'` |
| s15 / s16 / t28 | `sort.Strings` / `sort.Slice` / `sort.Sort` |
| s18 / t17 | `any` type switch (s18 also uses `%g`) |
| s19 | multi-line raw string `const` |
| s20 / t11 / t12 | `append(out, '\\', 'u')` / same / `append(a, b...)`, `append(a, "ef"...)` |
| s21 | `iota`, methods on a named int, `Println(Running)` |
| s22 | named results; `Println(t.Value, t.Expiry, err)` with `err == nil` |
| s23, s24, u14, u16, u17 | pointer methods, variadic, labels, slices of pointers, recursion |
| s25 | `encoding/base64` |
| s26 | `!string` with `catch e { }` |
| s27 | `@struct_fields(Tok)` |
| s28, u11 | local struct literals with slice and map fields |
| s29, t20 | `math.Floor`, `math.Mod` / `math.Pi`, `math.Abs` |
| t01–t03, x01, x04 | `%x %d %s %v %5.2f %t %c` / `%q` / `%g %e %.17g` / `%d %s %v %t %f %%` / `%.2f` |
| t04 | `fmt.Errorf %w`, `errors.Is`, `errors.Unwrap` |
| t05, u07 | `Stringer` with `Println`, `%v`, `%s`; `Println(nilError)` alone |
| t06–t09, u08 | `Time.Sub`, `Time.Unix`, `Time.Add`/`Before`/`After`, `time.Duration(n)` |
| t10, u10 | `strings.Builder{}`, `&bytes.Buffer{}` |
| t13, u12 | `[]byte`, slicing, `range` over a string, `copy` |
| t15, t16 | non-ASCII string with `DecodeRuneInString` / `utf8.ValidString` |
| t21 | `m := @n` |
| t22, v06, t23 | assertion to struct, comma-ok forms, `delete` |
| t24, u18 | hand hex parse, hand integer parse |
| t25, u09 | `Println(0.785398163397448)`, `Println(1.0/3.0)` |
| t26, u01, u02, u03 | `select` on `done` and `time.After(2s)` / timing of it / plain receive / `time.After` alone |
| t27, u05, u06, v04 | user error type (pointer, value, assign) / user interface |
| t29, x02, x03 | `strings.IndexByte`, `LastIndex` / `ToLower`, `EqualFold`, `Count` / `LastIndex` |
| t30, u04, v01, v02 | `strings.Cut` with three names / `a, b, ok := f()` / `a, b, ok = f()` / `a, b, c := 1, 2, 3` |
| u15, v03 | `case "Queued", "Running":` / `case 1, 2:` |
| v05, v07, v08 | `(Response, error)` return / `const` block of URLs / `switch {` with boolean cases |
| w01 | `X(QubitAt(16)); Mz(QubitAt(0))` on `qir_sim` |
| w02 | `--emit-qasm3` of `Rz(theta)` with `const theta = 0.5` |

## Appendix B — Sources (read 2026-11-27)

The IBM API changes. Re-read these before steps 7–9.

| URL | Used for |
|---|---|
| https://quantum.cloud.ibm.com/docs/en/guides/cloud-setup-rest-api | IAM token exchange (URL, form body, `expires_in: 3600`), `Authorization: Bearer`, `Service-CRN` |
| https://quantum.cloud.ibm.com/docs/en/api/qiskit-runtime-rest | Base URL `https://quantum.cloud.ibm.com/api/v1`, `eu-de` host, `IBM-API-Version: 2026-04-15` in the examples |
| https://quantum.cloud.ibm.com/api/openapi.json (`info.version` 0.57.2) | Endpoint paths (configuration, properties, status, jobs, results, cancel); `JobRunParamsBase` required `program_id`, `backend`; `JobStatus` enum; SamplerV2 `pubs`, `version`, `support_qiskit`, `shots`; `IBM-API-Version` header (required, `YYYY-MM-DD`, default `2024-01-01`); example `basis_gates` with fractional gates |
| https://quantum.cloud.ibm.com/docs/en/api/qiskit-runtime-rest/tags/jobs | Top-level `cost` ("estimated time ... in seconds"); `state.status`; results with `schema_version` (R1) |
| https://quantum.cloud.ibm.com/docs/en/guides/execution-modes-rest-api | "Open Plan users cannot submit session jobs" |
| https://quantum.cloud.ibm.com/docs/en/guides/sampler-options | `max_execution_time` range `[1, 10800]` seconds |
| https://quantum.cloud.ibm.com/docs/en/guides/fractional-gates (search summary only) | Heron default basis `cz, id, rz, sx, x`; `rx`, `rzz` only with fractional gates |

Not reachable on 2026-11-27: `.../docs/guides/sampler-rest-api` (404),
`.../docs/en/guides/native-gates` (404),
`https://cloud.ibm.com/docs/account?topic=account-iamtoken_from_apikey` (500).
