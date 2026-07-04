# Pointer-Concrete Interface Boxing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `var i Iface = &b` works with Go-exact aliasing (the interface data word IS the pointer, reusing the pointee's vtable), and `*Outer` satisfies interfaces via promoted methods (gate lift), fixing the #109 miscompile pair.

**Architecture:** C-representation in the single boxing helper (`codegen_interface_box`) — pointer concretes normalize to their pointee for vtable construction and skip the heap box entirely; thunks are untouched (in both shapes `data` points at the concrete struct). A defensive guard in `build_thunk` prevents un-normalized pointers ever reaching it again. The typecheck-side `TYPE_STRUCT` gate lifts strictly AFTER the boxing goldens are green.

**Tech Stack:** C23, LLVM-C API. Zero parser/grammar changes (run `./scripts/grammar-tripwire.sh` once as a no-op sanity per the goo-grammar skill).

## Global Constraints

- Branch: `fix/ptr-iface-boxing` (exists, base main @ bf63344). Commit `--no-gpg-sign`; pre-commit hook runs `make test`.
- Spec: `docs/superpowers/specs/2026-07-04-ptr-iface-boxing-design.md`. Scope frozen to its §§1–3; nil-in-interface behavior and interface→interface permissiveness are RECORDED, not fixed.
- ORDERING RULE: Task 2 (gate lift) must not start until Task 1's goldens are committed green — lifting first converts a clean error into wrong output.
- Baselines entering: golden 221/0 (`bash scripts/run_golden.sh`), `make test` 76 passed/1 skipped, bison 81/256.
- Every golden's expected output produced by `go run` on an equivalent `.go` program (go toolchain installed) — never hand-written.
- Real exit codes only — never a pipeline's `$?` for a gate.
- No header changes expected. If one becomes necessary: tail-append only + `make clean && make lexer`, justified in the report.
- Value-boxed interface path must be byte-for-byte unchanged (regression golden pins copy semantics).

---

### Task 1: C representation + thunk guard + goldens

**Files:**
- Modify: `src/codegen/interface_codegen.c` (`codegen_interface_box` at :206-230, `build_thunk` entry at :53-60)
- Create: `examples/ptr_iface_probe.goo` + `.expected.txt`
- Create: `examples/iface_copy_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: `codegen_interface_vtable(codegen, checker, iface, concrete)` (interface_codegen.c:168) — already name-caches per (concrete, iface); calling it with the POINTEE type is the whole normalization. `type_receiver_name(Type*)` returns the method-owner name or NULL.
- Produces: `codegen_interface_box` now accepts `TYPE_POINTER` concretes correctly at every existing call site (var-decl init function_codegen.c:1363, assignment expression_codegen.c:1063, multi-assign statement_codegen.c:215, call args call_codegen.c P4-5 arm, map values #110 I1 sites). Task 2 relies on pointer-boxed dispatch working; Task 3 probes the sinks.

- [ ] **Step 1: Write the failing goldens**

`examples/ptr_iface_probe.goo` (aliasing pins — verify expected with `go run` first):

```go
// ptr_iface_probe: pointer-concrete interface boxing (the #109 pair, part 1).
// Go semantics: `var i Counter = &b` stores the POINTER in the interface data
// word — mutations of b are visible through i, and a pointer-receiver method
// called through i mutates b itself. The old code heap-boxed the pointer and
// the thunk then treated the box as the pointee (LLVM verifier failure).
package main

import "fmt"

type Counter interface {
	Get() int
	Bump()
}

type Box struct {
	N int
}

func (b Box) Get() int {
	return b.N
}

func (b *Box) Bump() {
	b.N = b.N + 1
}

func read(c Counter) int {
	return c.Get()
}

func main() {
	b := Box{N: 40}
	var i Counter = &b
	fmt.Println(i.Get())
	b.N = 41
	fmt.Println(i.Get())
	i.Bump()
	fmt.Println(b.N)
	fmt.Println(read(i))
	b2 := Box{N: 7}
	list := []Counter{&b, &b2}
	list[1].Bump()
	fmt.Println(b2.N)
	fmt.Println(list[0].Get())
}
```

`examples/ptr_iface_probe.expected.txt` (paste from `go run /tmp/pip.go` — expected shape):

```
40
41
42
42
8
42
```

`examples/iface_copy_probe.goo` (regression pin: value-boxed stays COPY, plus both boxing shapes of the SAME type sharing one vtable — value-receiver-only interface so the `.go` equivalent is legal Go):

```go
// iface_copy_probe: value-boxed interfaces COPY (Go-exact) — pins that the
// pointer-boxing change did not leak aliasing into the value path — and a
// []Getter mixing value-boxed and pointer-boxed Box exercises the shared
// pointee vtable (the *Box "collision" is intentional reuse).
package main

import "fmt"

type Getter interface {
	Get() int
}

type Box struct {
	N int
}

func (b Box) Get() int {
	return b.N
}

func main() {
	b := Box{N: 5}
	var iv Getter = b
	var ip Getter = &b
	b.N = 9
	fmt.Println(iv.Get())
	fmt.Println(ip.Get())
	list := []Getter{b, &b}
	b.N = 12
	fmt.Println(list[0].Get())
	fmt.Println(list[1].Get())
}
```

`examples/iface_copy_probe.expected.txt` (verify with `go run`):

```
5
9
9
12
```

- [ ] **Step 2: Verify current failure**

Run: `bin/goo -o build/pip examples/ptr_iface_probe.goo`
Expected: FAIL — `Module verification failed: Call parameter type does not match function signature!` (the thunk/receiver mismatch). If it PASSES, stop and report — baseline assumption broken.
`iface_copy_probe` may partially work today (value path) — run it; if the mixed list line fails the same way, note it; the golden gates the END state.

- [ ] **Step 3: Implement the C representation in `codegen_interface_box`**

At the TOP of `codegen_interface_box` (interface_codegen.c:206, before the existing vtable call), insert:

```c
    // C-representation for pointer concretes (Go's own layout): the interface
    // data word IS the pointer, and the vtable is the POINTEE's — *T
    // deliberately REUSES T's thunks, because in both boxing shapes `data`
    // ends up pointing at a T (value-boxed: at the heap copy; pointer-boxed:
    // at the caller's object). No heap box: boxing a pointer must alias, and
    // storing it in a box was the #109 miscompile (thunks treated the box as
    // the pointee). See docs/superpowers/specs/2026-07-04-ptr-iface-boxing-design.md.
    if (concrete && concrete->kind == TYPE_POINTER &&
        concrete->data.pointer.pointee_type &&
        type_receiver_name(concrete->data.pointer.pointee_type)) {
        Type* pointee = concrete->data.pointer.pointee_type;
        LLVMValueRef pvt = codegen_interface_vtable(codegen, checker, iface, pointee);
        if (!pvt) return NULL;
        LLVMTypeRef pifacety = codegen_type_to_llvm(codegen, iface);
        if (!pifacety) return NULL;
        LLVMValueRef piv = LLVMGetUndef(pifacety);
        piv = LLVMBuildInsertValue(codegen->builder, piv, pvt, 0, "iface.vt");
        piv = LLVMBuildInsertValue(codegen->builder, piv, value, 1, "iface.data");
        return piv;
    }
```

The existing body (heap-box path for value concretes) stays byte-for-byte unchanged below it. `value` is already the loaded pointer at every call site (the helper's contract: "value is the loaded concrete LLVM value").

- [ ] **Step 4: Add the thunk guard**

At the TOP of `build_thunk` (interface_codegen.c:53, before the name snprintf), insert:

```c
    // After the C-representation normalization in codegen_interface_box, a
    // pointer concrete must never reach the thunk builder — its thunks are
    // the pointee's. A future direct caller of codegen_interface_vtable with
    // a raw *T would otherwise re-create the #109 verifier failure.
    if (concrete && concrete->kind == TYPE_POINTER) {
        codegen_error(codegen, (Position){0},
                      "internal: pointer concrete reached thunk builder un-normalized");
        return NULL;
    }
```

- [ ] **Step 5: Build and run the goldens**

```bash
make lexer                                   # exit 0
bin/goo -o build/pip examples/ptr_iface_probe.goo && ./build/pip   # six lines, exit 0
bin/goo -o build/icp examples/iface_copy_probe.goo && ./build/icp  # four lines, exit 0
bash scripts/run_golden.sh                   # 223 passed, 0 failed
make test                                    # 76/1, exit 0
```

Outputs must match the `.expected.txt` files byte-for-byte (which match `go run`).

- [ ] **Step 6: Commit**

```bash
git add src/codegen/interface_codegen.c examples/ptr_iface_probe.* examples/iface_copy_probe.*
git commit --no-gpg-sign -m "fix(iface): pointer-concrete boxing — data word IS the pointer, pointee vtable reuse

The #109 miscompile: codegen_interface_box heap-boxed the pointer value
while thunks treated the data slot as the pointee (on current main: LLVM
verifier failure). Adopt Go's own representation for pointer concretes —
skip the box, store the pointer as the data word, build the vtable against
the pointee. Thunks unchanged (data points at the concrete struct in both
shapes); build_thunk gains an internal-error guard against un-normalized
pointer concretes. Aliasing now Go-exact (goldens differential vs go run);
value-boxed copy semantics pinned unchanged by iface_copy_probe."
```

---

### Task 2: Gate lift — promoted satisfaction through `*Outer`

DO NOT START until Task 1 is committed with goldens green (ordering rule: the gate currently shields users from the miscompile Task 1 fixes).

**Files:**
- Modify: `src/types/type_checker.c` (`type_interface_satisfied` embed fallback, :813-822)
- Verify (expect NO edit): `src/codegen/interface_codegen.c:76` (`build_thunk`'s promoted-method resolution — after Task 1, only pointee types reach it; the Task 1 guard backstops)
- Create: `examples/embed_ptr_satisfaction_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: Task 1's pointer boxing (dispatch through a pointer-boxed `*Outer` must already work for DIRECT methods). `embedding_resolve(checker, struct_type, name)` returns `EmbedResult` with `.kind == EMBED_METHOD` on success.
- Produces: `check_interface_assign` / `type_interface_satisfied` accept `*S` where S's embedding graph provides the method — consumed by every interface sink.

- [ ] **Step 1: Write the failing golden**

`examples/embed_ptr_satisfaction_probe.goo` (verify expected with `go run` — pointer-embedded inner at depth 2, mutation visible through the shared *Base):

```go
// embed_ptr_satisfaction_probe: the #109 pair, part 2. *Outer satisfies an
// interface via a PROMOTED method (Go: *T's method set includes promoted
// methods). Depth 2 with a pointer-embedded inner hop; the shared *Base
// means mutations are visible through the interface.
package main

import "fmt"

type Namer interface {
	Name() string
}

type Base struct {
	Tag string
}

func (b Base) Name() string {
	return b.Tag
}

type Wrap struct {
	*Base
}

type Outer struct {
	Wrap
	X int
}

func main() {
	b := Base{Tag: "hi"}
	o := Outer{Wrap: Wrap{Base: &b}, X: 1}
	var n Namer = &o
	fmt.Println(n.Name())
	b.Tag = "yo"
	fmt.Println(n.Name())
	fmt.Println(o.X)
}
```

`examples/embed_ptr_satisfaction_probe.expected.txt` (from `go run`):

```
hi
yo
1
```

- [ ] **Step 2: Verify it fails today with the CLEAN error**

Run: `bin/goo -o build/eps examples/embed_ptr_satisfaction_probe.goo`
Expected: type error containing `does not implement Namer (missing method Name)` — the gate. If it fails any OTHER way (verifier error, crash), STOP and report: Task 1 didn't fully land.

- [ ] **Step 3: Lift the gate**

In `type_interface_satisfied` (type_checker.c:813-822), replace the embed-fallback block:

```c
            // Not directly declared — promoted method via embedding? Also
            // through a POINTER to a struct: Go's *Outer method set includes
            // Outer's promoted methods (the #109 pair, part 2 — safe only now
            // that codegen boxes pointer concretes correctly; before that fix
            // this gate was the shield between users and the miscompile).
            Type* embed_root = concrete;
            if (embed_root->kind == TYPE_POINTER &&
                embed_root->data.pointer.pointee_type &&
                embed_root->data.pointer.pointee_type->kind == TYPE_STRUCT) {
                embed_root = embed_root->data.pointer.pointee_type;
            }
            Type* impl_via_embed = NULL;
            if (embed_root->kind == TYPE_STRUCT) {
                EmbedResult er = embedding_resolve(checker, embed_root, im->name);
                if (er.kind == EMBED_METHOD) impl_via_embed = er.type;
            }
            if (!impl_via_embed) {
                *method_out = im->name; *reason_out = "missing"; return 0;
            }
            impl = impl_via_embed;
```

(This replaces the existing `if (concrete->kind == TYPE_STRUCT) { EmbedResult er = embedding_resolve(checker, concrete, ...); ... }` lines — same structure, rooted at `embed_root`.)

- [ ] **Step 4: Verify the build_thunk mirror needs no edit**

Read `src/codegen/interface_codegen.c:76` (`if (!real_fn && concrete->kind == TYPE_STRUCT)`): after Task 1's normalization, `build_thunk` only ever receives pointee types, so this arm already fires for the `*Outer` case (concrete arrives as `Outer`). Confirm by running the golden (Step 5) — if it fails with the Task 1 guard's message ("un-normalized"), a sink is bypassing `codegen_interface_box`: STOP and report which call path (the error position/stack), do not patch the guard.

- [ ] **Step 5: Build, run, gates**

```bash
make lexer                                       # exit 0
bin/goo -o build/eps examples/embed_ptr_satisfaction_probe.goo && ./build/eps  # hi / yo / 1
bash scripts/run_golden.sh                       # 224 passed, 0 failed
make test                                        # 76/1
```

Also confirm the NEGATIVE stays intact: a pointer to a struct that does NOT satisfy still gets the clean message. Quick check (not a committed probe — `examples/` reject coverage for interfaces already exists as embed-iface-reject-probe):

```bash
printf 'package main\ntype I interface {\n\tM()\n}\ntype S struct {\n\tX int\n}\nfunc main() {\n\tvar s S\n\tvar i I = &s\n\t_ = i\n}\n' > build/neg.goo
bin/goo -o build/neg build/neg.goo   # must FAIL: "S does not implement I (missing method M)"
```

- [ ] **Step 6: Commit**

```bash
git add src/types/type_checker.c examples/embed_ptr_satisfaction_probe.*
git commit --no-gpg-sign -m "fix(iface): lift TYPE_STRUCT gate — *Outer satisfies via promoted methods

Part 2 of the #109 pair, landing strictly after the boxing fix it was
shielding users from. type_interface_satisfied's embed fallback now roots
at the pointee for pointer-to-struct concretes; build_thunk's mirror needs
no change (post-normalization it only sees pointee types; the Task 1 guard
backstops). Golden: depth-2 promotion with a pointer-embedded hop,
mutation-through-shared-*Base pinned differential vs go run."
```

---

### Task 3: Sink probes + full sweep + handoff

**Files:**
- Create: none committed (probes run from a temp dir; the nil-pointer probe result is DOCUMENTED in the handoff, not golden'd)
- Modify: `.handoff.md`

**Interfaces:** consumes Tasks 1-2; produces the branch's ship gates + updated queue.

- [ ] **Step 1: Sink probes (temp dir, each differential vs `go run` where legal Go)**

Write and run these four in `/home/ddowney/.claude/jobs/3d88f1bb/tmp` (or any temp dir), compiled with `bin/goo`:

Probe A — call-arg sink:

```go
package main

import "fmt"

type Getter interface {
	Get() int
}

type Box struct {
	N int
}

func (b Box) Get() int {
	return b.N
}

func take(g Getter) int {
	return g.Get()
}

func main() {
	b := Box{N: 3}
	fmt.Println(take(&b))
	b.N = 4
	fmt.Println(take(&b))
}
```

Expected (go run): `3` then `4`.

Probe B — map-value sink (composes with #110):

```go
package main

import "fmt"

type Getter interface {
	Get() int
}

type Box struct {
	N int
}

func (b Box) Get() int {
	return b.N
}

func main() {
	b := Box{N: 6}
	m := map[string]Getter{}
	m["p"] = &b
	m["v"] = b
	b.N = 7
	fmt.Println(m["p"].Get())
	fmt.Println(m["v"].Get())
}
```

Expected (go run): `7` then `6` (pointer aliases, value copied at insert).

Probe C — var-decl + reassignment sink:

```go
package main

import "fmt"

type Getter interface {
	Get() int
}

type Box struct {
	N int
}

func (b Box) Get() int {
	return b.N
}

func main() {
	b1 := Box{N: 1}
	b2 := Box{N: 2}
	var g Getter = &b1
	g = &b2
	b2.N = 20
	fmt.Println(g.Get())
	g = b1
	b1.N = 10
	fmt.Println(g.Get())
}
```

Expected (go run): `20` then `1`.

Probe D — nil pointer in interface (DOCUMENT ONLY, do not golden; the value-receiver case crashes where Go panics cleanly — the spec's recorded deviation):

```go
package main

import "fmt"

type Getter interface {
	Get() int
}

type Box struct {
	N int
}

func (b Box) Get() int {
	return b.N
}

func main() {
	var p *Box
	var g Getter = p
	fmt.Println(g.Get())
}
```

Go: panics `invalid memory address or nil pointer dereference`. Goo: expected to crash (SIGSEGV, rc=139) — RECORD the observed behavior verbatim in the handoff. If Goo instead REJECTS at compile time or prints something, record that instead; any outcome except wrong output is acceptable.

Any probe A-C mismatch vs go run = a Task 1/2 defect: STOP, report BLOCKED with the transcript. Do not adjust expectations.

- [ ] **Step 2: Full sweep (real exit codes)**

```bash
make clean && make lexer                     # exit 0
make test                                    # 76/1
bash scripts/run_golden.sh                   # 224 passed, 0 failed
eval "$(opam env --switch=default)"          # REQUIRED for ccomp
make verify                                  # ALL GREEN GATES PASSED
make ccomp-link                              # PASS
./scripts/grammar-tripwire.sh                # PASS (81 S/R + 256 R/R) — no-op sanity, no grammar changes on this branch
```

- [ ] **Step 3: Update `.handoff.md`**

- Mark the ptr-boxing pair SHIPPED (this branch): C representation (data word = pointer, pointee vtable reuse), thunk guard, gate lifted — `*Outer` promoted satisfaction works; queue item 1 CLOSED, item's shield note obsolete.
- Record Probe D's observed nil behavior as the documented deviation (crash vs Go's clean panic; ptr-receiver-on-nil untested until a ptr-receiver-only interface case arises).
- Promote the next queue head (type assertions/switches now unblocked on a sound interface rep; non-string map keys).
- Keep all other queue items intact.

- [ ] **Step 4: Commit**

```bash
git add .handoff.md
git commit --no-gpg-sign -m "docs(handoff): ptr-iface boxing pair shipped; queue advanced"
```

After Task 3: push, PR, fresh-context whole-branch review before merge (mandatory — this exact step caught the original finding on #109 and I1 on #110).

---

## Execution notes (controller)

- SDD economy mode: Sonnet implementers, Fable controller review + independent probes between tasks.
- Task 1 is the risk center (codegen); its reviewer should specifically check that the early-return path can't fire for NON-pointer concretes and that no call site passes an UN-loaded pointer (lvalue) as `value`.
- Task 2's Step 2 expectation (clean "does not implement" error BEFORE the lift) doubles as the ordering-rule enforcement — an implementer who sees anything else has caught a Task 1 gap.
- Zero grammar changes: any parser.y touch on this branch is an automatic BLOCKED (goo-grammar skill governs if it ever becomes necessary).
