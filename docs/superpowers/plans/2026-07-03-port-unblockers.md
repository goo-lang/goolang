# Port Unblockers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the top items from the 2026-07-03 re-probe (gating-gaps memory): cwd-independent goostd imports, `string(rune)`/`string(byte)` conversion, selector/index postfix `++/--`, `os.Args`, and the ARROW precedence correctness bug.

**Architecture:** Five independent small tasks, risk-ascending: (T1) the import resolver's exe-relative probe already exists but only matches the INSTALLED layout (`<exe>/../lib/goostd`) — add the dev-tree layout (`<exe>/../goostd`) and a `GOOROOT` env override to the documented precedence chain. (T2) `string(rune)` / `string(byte)`: a runtime helper (`goo_string_from_rune` — UTF-8 encode; byte = 1-byte string) + the checker conversion arm + codegen lowering. (T3) selector/index postfix `++/--`: the PARSER already accepts these (the rejection at expression_codegen.c:66 is codegen-level) — lower via the existing selector/index LVALUE paths (load, add 1, store). (T4) `os.Args` as `[]string`: runtime captures argc/argv at startup into a goo_slice of goo_strings; the os package registers the member. (T5) ARROW precedence: `%left ARROW` (parser.y:200) sits ABOVE `+ -` (:198; later = higher in bison), so `ch <- n + 1` parses `(ch <- n) + 1` — move ARROW below the binary operators (above OR, :195) so a send's RHS absorbs arithmetic; full bison discipline (this is the branch's only grammar-precedence change).

**Tech Stack:** C23, LLVM-C, Bison (T5 precedence only). Resolver, checker, codegen, runtime.

## Global Constraints

- Branch: `fix/port-unblockers` off main @52608df. Do NOT commit on main.
- Commits: conventional, imperative, `--no-gpg-sign`. Stage only named files; never stage `.superpowers/` or `.handoff.md`.
- **Bison discipline (T5 only): baseline is 81 shift/reduce + 256 reduce/reduce** (the post-#107 justified baseline). Precedence-declaration moves CAN legitimately change conflict counts — record exact deltas, produce the shape-diff analysis (the #107 methodology), full-suite differential. Unexplained delta = STOP.
- Gate per task: `make lexer` (clean-first when parser.y/headers touched), probes, then `eval "$(opam env --switch=default)"`, `make verify` (ALL PASS; golden 196/0 grows per probe; 30 probes stay green) and `make test` (76/1) and `make ccomp-link` (PASS). STOP/BLOCKED on any regression.
- Go conformance: `go run`-verify probes; record.
- Pre-commit hook runs `make test`.

## Reference: verified landmarks (2026-07-03 late, main @52608df)

- Import resolver: `goo_gooroot_dir()` (src/package/import_resolver.c:24-47) — precedence today: `<exe>/../lib/goostd` (:41, INSTALLED layout — the dev tree is `<repo>/goostd`, so this misses) then `./goostd` (:47, cwd — why root-only works). It documents mirroring `goo_runtime_archive_path()` (src/codegen/codegen.c:~690) — keep the mirror-comment accurate when extending BOTH? NO — check whether the runtime archive has the same dev-tree gap (`<exe>/../lib/libgoo_runtime.a`); if linking out-of-tree ALSO fails, T1 fixes both chains identically (probe it in Step 2 and scope accordingly).
- String conversion rejections: expression_checker.c:2120 (`cannot convert to %s (only numeric conversions ...)`) and :2139 — the conversion-call arm from #103; the T2 arm goes beside them. Conversion codegen: find where numeric conversion calls lower (call_codegen.c `codegen_numeric_convert` region).
- utf8 encode logic exists in goostd (Go source) — the RUNTIME needs a C helper; simplest self-contained: `goo_string_t goo_string_from_rune(int32_t r)` in runtime.c (UTF-8 encode up to 4 bytes, mirror goo_string_new's allocation conventions; NOTE the >16-byte ABI rule — goo_string is {ptr,i64} 16 bytes, returned by value like goo_string_new; VERIFY goo_string_new's return convention and mirror exactly).
- Postfix rejection: expression_codegen.c:66 ("postfix ++/-- requires a simple identifier") and :71 — the postfix arm already receives parsed selector/index operands; selector/index LVALUE machinery (address-producing) exists throughout (the #91 lvalue work).
- os package: find its registration (grep `"os"` across src/types + src/package + goostd/ — os.Getenv works per m12_probe, so a registration point exists); `os.Args` = `[]string` — runtime capture: main() wrapper/entry already exists (goo runtime init at main entry — find where argc/argv are available; C main is generated? READ how the executable's main is emitted and where argc/argv could be stashed into runtime globals).
- ARROW: parser.y:200 `%left ARROW` — above `%left PLUS MINUS ...` (:198) and `%left MULTIPLY ...` (:199). Target position: between :194 and `%left OR` (:195). Go reference: send is a STATEMENT; `ch <- n + 1` sends n+1. Receive `<-ch` is UNARY (check whether the unary receive production uses %prec — if so, verify the move doesn't disturb it).
- Repro (2026-07-03): out-of-tree `import "strings"` → "cannot resolve import"; `string(nc)` → "cannot convert to string"; `c.n++` → codegen rejection; `os.Args` → "Package 'os' has no member 'Args'"; `ch <- n + 1` → misparse (send then add).

---

### Task 1: cwd-independent goostd resolution (+ runtime archive if same gap)

**Files:**
- Modify: `src/package/import_resolver.c` (`goo_gooroot_dir` precedence), `src/codegen/codegen.c` (`goo_runtime_archive_path` — ONLY if Step 2 shows the same gap)
- Test: Makefile `outoftree-probe` (a probe target that compiles a goostd-importing program FROM A TEMP CWD)

- [ ] **Step 1: Probe target** — Makefile `outoftree-probe`: writes a small `import "strings"` program to `build/oot/prog.goo`, `cd build/oot && <abs-path-to>/bin/goo prog.goo -o prog.out` (cwd ≠ repo root), asserts compile rc=0 AND run output. Wire into `verify`.
- [ ] **Step 2: Verify today** — the probe fails at import resolution (RED). ALSO test whether LINKING out-of-tree fails on the runtime archive (compile a no-import program from the temp cwd — if it links, the archive chain is fine and codegen.c stays untouched; record).
- [ ] **Step 3: Implement** — precedence in `goo_gooroot_dir`: (1) `GOOROOT` env var if set (documented: points at the goostd PARENT? pick: GOOROOT points at the directory CONTAINING goostd, i.e. the repo root or install prefix — document the choice in the comment and the probe), (2) `<exe>/../lib/goostd` (installed), (3) `<exe>/../goostd` (dev tree — NEW), (4) `./goostd` (cwd fallback, last). Resolve the exe path the way :41's existing code does (READ it — /proc/self/exe presumably). Mirror into `goo_runtime_archive_path` only if Step 2 showed the gap.
- [ ] **Step 4: Gate** — outoftree-probe PASS; full verify (196/0 + 31 probes); test 76/1; ccomp PASS.
- [ ] **Step 5: Commit** — "fix(resolver): goostd resolution works from any cwd (dev-tree exe-relative + GOOROOT env)".

---

### Task 2: `string(rune)` / `string(byte)` conversion

**Files:**
- Modify: `src/runtime/runtime.c` + `include/runtime.h` (goo_string_from_rune — header ⇒ make clean), `src/types/expression_checker.c` (conversion arm), `src/codegen/call_codegen.c` (lowering)
- Test: `examples/strconv_rune_probe.goo` + `.expected.txt`

- [ ] **Step 1: Probe**:
```go
package main

import "fmt"

func shift(s string, k int) string {
	out := ""
	for _, c := range s {
		if c >= 'a' && c <= 'z' {
			nc := 'a' + (c-'a'+int32(k))%26
			out = out + string(nc)
		} else {
			out = out + string(c)
		}
	}
	return out
}

func main() {
	fmt.Println(shift("hello, world", 3))
	fmt.Println(string('A'))
	b := byte(66)
	fmt.Println(string(b))
	fmt.Println(string('世'))
}
```
`.expected.txt`: `khoor, zruog` `A` `B` `世`. `go run`-verify (the caesar re-probe program plus direct forms incl. a multibyte rune).
- [ ] **Step 2: Verify today** — "cannot convert to string" (RED).
- [ ] **Step 3: Runtime** — `goo_string_from_rune(int32_t r)`: UTF-8 encode (1-4 bytes; invalid runes → U+FFFD per Go), allocate via the same conventions as goo_string_new, return by the SAME convention (verify: by-value {ptr,len} or out-param? mirror exactly). Header decl + doc comment.
- [ ] **Step 4: Checker + codegen** — conversion arm: `string(x)` where x is integer-kind (rune/int32/byte/uint8/any int — Go allows any integer type) → TYPE_STRING; beside the numeric arms at :2120/:2139 region. Codegen: lower to `goo_string_from_rune(value sext/trunc to i32)` — mirror how existing runtime-call conversions declare/call the extern. KEEP `[]byte(s)`/`string([]byte)` OUT of scope (record).
- [ ] **Step 5: Gate** — probe PASS; verify 197/0; test 76/1; ccomp PASS (runtime boundary — the 16-byte return ABI is the risk; the goo_string_new precedent is the guide).
- [ ] **Step 6: Commit** — "feat(types,codegen,runtime): string(rune) and string(byte) conversions".

---

### Task 3: selector/index postfix ++/--

**Files:**
- Modify: `src/codegen/expression_codegen.c` (:60-72 postfix arm)
- Test: `examples/postfix_probe.goo` + `.expected.txt`

- [ ] **Step 1: Probe**:
```go
package main

import "fmt"

type C struct {
	n int
}

func main() {
	c := C{n: 40}
	c.n++
	c.n++
	fmt.Println(c.n)
	a := []int{1, 2, 3}
	a[1]++
	fmt.Println(a[1])
	a[0]--
	fmt.Println(a[0])
	m := C{n: 0}
	for i := 0; i < 3; i = i + 1 {
		m.n++
	}
	fmt.Println(m.n)
}
```
`.expected.txt`: `42` `3` `0` `3`. `go run`-verify.
- [ ] **Step 2: Verify today** — codegen rejection (RED; the parse succeeds — confirm and record, it defines the fix's layer).
- [ ] **Step 3: Implement** — in the postfix arm: selector/index operands route through their existing LVALUE codegen (the address-producing paths the #91 work built — find how ASSIGNMENT to c.n / a[i] gets its address and reuse THAT, not a new walk); load, ±1 at the loaded type's width, store. Keep the plain-identifier fast path unchanged. `a[i]++` bounds-checks via whatever the index-lvalue path already does (verify: same as `a[i] = x`).
- [ ] **Step 4: Gate** — probe PASS; verify 198/0; test 76/1.
- [ ] **Step 5: Commit** — "feat(codegen): postfix ++/-- on selector and index lvalues".

---

### Task 4: os.Args

**Files:**
- Modify: `src/runtime/runtime.c` + `include/runtime.h` (argv capture — header ⇒ make clean), the os-package registration point (FIND it — Step 2 records where; likely src/types/type_checker.c package setup or goostd), codegen member-access lowering if os members lower specially (READ how os.Getenv lowers and mirror)
- Test: `examples/osargs_probe.goo` + `.expected.txt` + a Makefile probe passing args (goldens run without args — the Makefile probe covers the WITH-args case)

- [ ] **Step 1: Probes** — golden `osargs_probe.goo`: `fmt.Println(len(os.Args) >= 1)` → `true` (argv[0] always present). Makefile `osargs-probe`: runs the same binary WITH two args, asserts `len(os.Args)==3` output and `os.Args[1]` echo (write the program to also print those when len>1; the golden's no-arg run prints only the first line — design the program so BOTH harnesses assert cleanly; document in the header).
- [ ] **Step 2: Verify today** — "Package 'os' has no member 'Args'" (RED). Record where os's members register and how Getenv lowers.
- [ ] **Step 3: Runtime** — capture argc/argv: find where the generated main / runtime init receives them (READ how the executable's entry is emitted; if the generated main is `int main(int argc, char** argv)` calling runtime init, pass them through to `goo_os_args_init(argc, argv)` storing a goo_slice of goo_string; expose `goo_os_args()` returning it). Slice-of-string ABI per the m12 by-pointer rule — mirror how existing slice-returning runtime surfaces work (goo_slice_alloc precedent: return bare pointer, codegen builds the header).
- [ ] **Step 4: Register + lower** — os.Args as a `[]string` package member; lower reads to the runtime getter. Mirror Getenv's whole path.
- [ ] **Step 5: Gate** — both probes PASS; verify 199/0 + 32 probes; test 76/1; ccomp PASS.
- [ ] **Step 6: Commit** — "feat(runtime,types): os.Args".

---

### Task 5: ARROW precedence (send RHS absorbs arithmetic)

**Files:**
- Modify: `src/parser/parser.y` (precedence declaration position ONLY)
- Test: `examples/sendexpr_probe.goo` + `.expected.txt`

- [ ] **Step 1: Probe**:
```go
package main

import "fmt"

func main() {
	ch := make(chan int, 2)
	n := 20
	ch <- n + 22
	ch <- n * 2
	a := <-ch
	b := <-ch
	fmt.Println(a)
	fmt.Println(b)
	c2 := make(chan bool, 1)
	c2 <- n > 10
	fmt.Println(<-c2)
	fmt.Println(10 + <-ch2())
}
```
Where `ch2()` is replaced by a small helper returning a 1-buffered chan holding 32 (write the helper in the probe) — the `10 + <-ch` receive-in-arithmetic shape MUST stay working after the precedence move. `.expected.txt`: `42` `40` `true` `42`. `go run`-verify. Covers +, *, a comparison as send RHS, and receive-inside-arithmetic.
- [ ] **Step 2: Verify today** — `ch <- n + 22` misparses; record the actual failure mode ((ch <- n) + 1 = send-as-expression then arithmetic on its void/monostate result — checker or codegen error text, record).
- [ ] **Step 3: Implement** — move `%left ARROW` from :200 to immediately BEFORE `%left OR` (:195) — send binds loosest of the binary operators, so the RHS absorbs arithmetic/comparison. CHECK the unary receive production for a `%prec` annotation (`<-ch` prefix — if it carries %prec ARROW, verify the move keeps receive binding TIGHT enough for `a := <-ch` and `t = t + <-results` — the batch-2 worker probe exercised `t + <-results`, it must stay working; it's in the suite via p8-style shapes? NOT in goldens — add `fmt.Println(10 + <-ch)`-shaped line to the probe if absent from the suite).
- [ ] **Step 4: Bison guard** — `make clean && make lexer`; counts vs 81/256; precedence moves CAN shift conflict counts legitimately — shape-diff analysis + full-suite differential REQUIRED for any delta; goldens with channel sends (`chan-probe`, select family, soak probes) are the behavioral differential.
- [ ] **Step 5: Gate** — probe PASS; verify 200/0; test 76/1; ccomp PASS.
- [ ] **Step 6: Commit** — "fix(parser): channel send binds loosest — ch <- n + 1 sends n+1".

---

## Final gate

`make verify` → ALL GREEN (200/0 + 32 probes). `make test` → 76/1. `make ccomp-link` → PASS. Re-run the re-probe programs that failed (p2/p7 out-of-tree, p15 caesar, p10 selector-++, p3 os.Args) — all five must now pass; record in the final report.

## Self-review notes

- T1 fixes the resolver chain, not the probe's symptoms — the out-of-tree probe compiles from a genuinely different cwd.
- T2's ABI risk (16-byte string return) leans on the goo_string_new precedent; ccomp gate is the guard.
- T5 is the only grammar change and goes LAST — everything else ships even if T5 blocks on conflict analysis.
- Out of scope (recorded): `[]byte(s)`/`string([]byte)`; struct embedding and func-typed map values (design-first items); os.Exit/os.Open etc. beyond Args.
