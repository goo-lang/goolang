# ARC error-release unblock (steps A+B+C) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Take a function-scoped `error` local from "refused by two independent guards" to "released", so that step D's CFG has a target it can actually move.

**Architecture:** Three small, independent changes to the existing two-layer ARC split. `release_decision.c` (pure AST, no LLVM, table-tested) answers *does this local own its value*; `codegen_arc_note_local` answers *which part of this slot is the heap object*. Task 1 adds a diagnostic that distinguishes the two refusals. Task 2 teaches codegen a fourth slot shape. Task 3 teaches the decision walk to attribute a tuple destructure's single call value to every target.

**Tech Stack:** C23, LLVM 22 C API, GNU Make, valgrind. No new dependencies.

## Global Constraints

- **C23** (`-std=c23`), built with `-Wall -Wextra`. **A new warning is a regression** — the build is clean today apart from three pre-existing ones (`PARAMS_INT64_STRING` unused, `_XOPEN_SOURCE` redefined, `write_file` unused).
- **Commits must use `git -c commit.gpgsign=false commit`** — the 1Password SSH signing agent fails in this environment.
- Commit trailer, verbatim on every commit:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
- **Code goes via PR, never direct to main.** Only `.handoff.md` is exempt.
- Work on branch `feat/t4-error-release-unblock`, cut from `main`.
- **Soundness direction is inverted from the escape passes.** In `release_decision`, `false` (do not release) is the safe answer. A wrong `true` frees live memory. Every construct not precisely understood must return false.
- **`make verify-core` must pass before the PR.** It takes ~4 min locally.
- Do NOT change `param_escape`, `block_escape`, `local_escape`, or the escape engine. Their row matrices must be untouched.

---

### Task 1: The verdict diagnostic

The instrument that makes tasks 2 and 3 diagnosable. Today only *approvals* print under `GOO_ARC_DEBUG`, so "the plan refused" and "codegen refused the shape" look identical — they are defects in different modules.

**Files:**
- Modify: `src/codegen/statement_codegen.c` (in `codegen_arc_note_local`, immediately before the `release_plan_should_release` bail at ~line 2750)

**Interfaces:**
- Consumes: `release_plan_verdict(const ReleasePlan*, const char* fn, const char* local)` and `release_verdict_name(ReleaseVerdict)`, both already declared in `include/release_decision.h`.
- Produces: a stderr line of the exact form `[arc?] <function>: <local> -> <VERDICT_NAME>`. Tasks 2 and 3 assert on this string.

- [ ] **Step 1: Write the failing check as a shell fixture**

Create `/tmp/arc_diag.goo`:

```go
package main

import "errors"
import "fmt"

func f(s string) int {
	e := errors.New("boom")
	if e == nil {
		return 1
	}
	return 0
}

func main() { fmt.Println(f("x")) }
```

- [ ] **Step 2: Run it to verify no verdict line exists yet**

```bash
GOO_ARC_DEBUG=1 ./bin/goo build -o /tmp/arc_diag.out /tmp/arc_diag.goo 2>&1 | grep '\[arc?\]'
```

Expected: no output, exit 1 from grep. This is the gap.

- [ ] **Step 3: Add the diagnostic**

In `codegen_arc_note_local`, insert immediately BEFORE the existing line
`if (!release_plan_should_release(codegen->release_plan, fi->name, info->name)) return;`:

```c
    // WHY THIS PRINTS BEFORE THE BAIL. Two different modules can refuse the
    // same local, and until this line they were indistinguishable from the
    // outside: release_decision answering "not owned" looked exactly like this
    // function answering "I do not recognise that slot shape". They are defects
    // in different files with different fixes.
    //
    // Measured worth: this line is what identified BOTH guards refusing `err`
    // in seconds, after .handoff.md had attributed the refusal to condition 4
    // (loop scope), which `err` never reaches. See
    // docs/superpowers/specs/2026-07-31-arc-loop-scoped-release-design.md.
    if (getenv("GOO_ARC_DEBUG")) {
        fprintf(stderr, "[arc?] %s: %s -> %s\n", fi->name, info->name,
                release_verdict_name(release_plan_verdict(codegen->release_plan,
                                                          fi->name, info->name)));
    }
```

- [ ] **Step 4: Rebuild and verify the line appears**

```bash
make lexer 2>&1 | grep -E " error:| warning:" ; echo "--- none above = clean ---"
GOO_ARC_DEBUG=1 ./bin/goo build -o /tmp/arc_diag.out /tmp/arc_diag.goo 2>&1 | grep '\[arc?\] f:'
```

Expected exactly:
```
[arc?] f: s -> RELEASE_NO_NO_BINDING
[arc?] f: e -> RELEASE_OK
```

`e` reads `RELEASE_OK` while **no** `[arc]` approval line is printed for it. That divergence is Task 2's entire subject, and this step is what makes it visible.

- [ ] **Step 5: Verify no gate regressed**

```bash
make release-decision-test 2>&1 | grep "summary:"
```
Expected: `142 assertions passed, 0 failed`.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/statement_codegen.c
git -c commit.gpgsign=false commit -m "feat(arc): print the verdict for every release candidate, not just approvals

Two different modules can refuse the same local, and until now they were
indistinguishable from outside: release_decision answering NOT_OWNED looked
exactly like codegen_arc_note_local refusing the slot shape.

This line identified BOTH guards refusing \`err\` in seconds, after
.handoff.md had attributed the refusal to condition 4 -- which \`err\` never
reaches, because condition 2 refuses it first.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: The nullable-pointer slot shape

An `error` local is `TYPE_NULLABLE` of `*int8` (`type_checker_error_type`, `type_checker.c:342`, which overwrites the name to `"error"`). It lowers to `{ i1, ptr }` — field 0 is the nil flag, **the heap object is field 1**. The existing shape test accepts a bare pointer, a 3-field slice, or a 2-field *string*, so it refuses this and emits nothing even on `RELEASE_OK`.

**Files:**
- Modify: `src/codegen/statement_codegen.c` (the shape test in `codegen_arc_note_local`, after the `TYPE_STRING` arm at ~line 2782)
- Modify: `tests/unit/types/release_decision_test.c` (add rows to `rows[]`)
- Create: `examples/arc_release_error_probe.goo`
- Create: `examples/arc_release_unwrap_probe.goo`
- Modify: `Makefile` (extend `arc-release-probe`)

**Interfaces:**
- Consumes: Task 1's `[arc?]` line.
- Produces: `field = 1` release sites. No emission change is needed — the exit already does `LLVMBuildStructGEP2(..., (unsigned)site->field, ...)`, so an arbitrary field index already works; `-1` and `0` were the only values *used*, not the only ones supported.

- [ ] **Step 1: Write the failing probe**

Create `examples/arc_release_error_probe.goo`:

```go
// T4 gate: does an owned `error` local release?
//
// An error is TYPE_NULLABLE of *int8 (type_checker_error_type), lowering to
// { i1, ptr } -- field 0 is the nil flag and THE OBJECT IS FIELD 1. Before
// this arm the shape test accepted a bare pointer, a 3-field slice and a
// 2-field string, so an error read RELEASE_OK and emitted NOTHING.
//
// errors.New is non_retaining = 1, audited against goo_error_from_string ->
// goo_error_wrap, which goo_allocs and memcpys the message. So the returned
// error owns a COPY and no other name holds it.
//
// The bytes are read through Error() rather than compared to nil: a nil
// comparison loads field 0, the FLAG, and never dereferences the object --
// it would pass against a compiler that released the error too early.
package main

import "errors"

var sink int

func build(i int) int {
	e := errors.New("a failure happened here")
	msg := e.Error()
	return len(msg)
}

func main() {
	total := 0
	for i := 0; i < 10000; i++ {
		total = total + build(i)
	}
	sink = total
	println(total)
}
```

- [ ] **Step 2: Run it to verify the release does NOT happen yet**

```bash
make lexer >/dev/null 2>&1
GOO_ARC_DEBUG=1 ./bin/goo build -o build/arc_err /tmp/../examples/arc_release_error_probe.goo 2>&1 | grep -E '\[arc\??\] build:'
```

Expected: `[arc?] build: e -> RELEASE_OK` appears, but **no** `[arc] build: will release e` line. That is the defect.

Then confirm it leaks:
```bash
valgrind --leak-check=full ./build/arc_err 2>&1 | grep -E "definitely lost|indirectly lost"
```
Expected: a non-zero leak.

- [ ] **Step 3: Add the shape arm**

In `codegen_arc_note_local`, immediately AFTER the `TYPE_STRING` arm's closing brace and BEFORE the final `else { return; }`, add:

```c
    } else if (LLVMGetTypeKind(slot_ty) == LLVMStructTypeKind &&
               info->goo_type && info->goo_type->kind == TYPE_NULLABLE &&
               info->goo_type->data.nullable.base_type &&
               info->goo_type->data.nullable.base_type->kind == TYPE_POINTER &&
               LLVMCountStructElementTypes(slot_ty) == 2 &&
               LLVMGetTypeKind(LLVMStructGetTypeAtIndex(slot_ty, 0)) == LLVMIntegerTypeKind &&
               LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(slot_ty, 0)) == 1 &&
               LLVMGetTypeKind(LLVMStructGetTypeAtIndex(slot_ty, 1)) == LLVMPointerTypeKind) {
        // A NULLABLE POINTER: `{ i1, ptr }` — field 0 is the nil FLAG and the
        // object is FIELD 1. `error` is the shape that matters today
        // (type_checker_error_type builds ?*int8 and renames it "error"), but
        // the rule is stated over the TYPE, not over that name, so `?*T`
        // behaves the same and no string comparison decides a free().
        //
        // THE FLAG IS WHY THIS IS FIELD 1 AND NOT FIELD 0. A string and a
        // slice both LEAD with their buffer; this leads with an i1. Testing the
        // leading field's kind and width is what keeps the three arms mutually
        // exclusive — a 2-field string is `{ ptr, i64 }` and cannot match here.
        //
        // NIL SAFETY IS ALREADY PAID FOR. The error-union construction builds
        // `{ i1 false, ptr undef }` and fills field 1 in, so an unwritten slot
        // would hold UNDEF — the exact use-of-undef class PR #265 fixed.
        // codegen_arc_zero_slot below stores LLVMConstNull(slot_ty), which for
        // this shape is zeroinitializer and therefore `{ false, null }`, and
        // goo_release is a no-op on NULL. The guarantee is FAIL-CLOSED: no
        // release site is recorded unless that store was emitted.
        field = 1;
```

- [ ] **Step 4: Rebuild and verify the release now happens, clean**

```bash
make lexer 2>&1 | grep -E " error:| warning:.*statement_codegen" ; echo "--- none above = clean ---"
GOO_ARC_DEBUG=1 ./bin/goo build -o build/arc_err examples/arc_release_error_probe.goo 2>&1 | grep -E '\[arc\] build:'
valgrind --leak-check=full --error-exitcode=99 ./build/arc_err > /dev/null 2> /tmp/arc_err.vg; echo "rc=$?"
grep -E "definitely lost|indirectly lost|All heap blocks" /tmp/arc_err.vg
grep -cE "Invalid read|Invalid write|Invalid free" /tmp/arc_err.vg
```

Expected: a `will release e at exit (field=1, ...)` line; `rc=0`; zero bytes lost; zero invalid accesses.

- [ ] **Step 5: Write the `errors.Unwrap` soundness probe**

This arm is what makes a wrong ownership answer on an error *dangerous* rather than inert. `goo_error_unwrap` returns `e->cause` — a pointer INTO its argument — which is why its table row carries `non_retaining = 0`.

Create `examples/arc_release_unwrap_probe.goo`:

```go
// T4 SOUNDNESS gate: an error obtained by UNWRAPPING another must not release.
//
// errors.Unwrap returns e->cause (src/runtime/runtime.c), a pointer INTO its
// argument's own structure. Its shim row carries non_retaining = 0 for exactly
// that reason, so condition 2 refuses it.
//
// Before the nullable-pointer arm that refusal was INERT -- codegen would not
// have emitted a release for an error whatever the verdict said. After it, a
// wrong answer here frees memory the outer error still owns. This probe is the
// difference between those two states.
//
// LEAKS ARE EXPECTED and must NOT be asserted on: the outer error is
// loop-scoped, so condition 4 refuses it and nothing reclaims it. What this
// asserts is that nothing releases the INNER one wrongly, which shows up as an
// invalid access rather than as a leak count.
package main

import "errors"

var sink int

func main() {
	total := 0
	for i := 0; i < 1000; i++ {
		outer := errors.New("outer failure")
		inner := errors.Unwrap(outer)
		if inner == nil {
			total = total + 1
		}
		// Read the OUTER error's bytes after `inner` went out of scope. If
		// `inner` had been released, this walks freed memory.
		total = total + len(outer.Error())
	}
	sink = total
	println(total)
}
```

- [ ] **Step 6: Verify the unwrap probe is clean**

```bash
./bin/goo build -o build/arc_unwrap examples/arc_release_unwrap_probe.goo
valgrind --leak-check=full ./build/arc_unwrap > /dev/null 2> /tmp/arc_unwrap.vg
grep -cE "Invalid read|Invalid write|Invalid free|double free" /tmp/arc_unwrap.vg
```
Expected: `0`.

- [ ] **Step 7: Add the unit rows**

Append to `rows[]` in `tests/unit/types/release_decision_test.c`, before the closing `};`. Use the next free row numbers (the array is currently numbered through 34):

```c
    {
        // THE ROW THE NULLABLE-POINTER ARM EXISTS FOR. errors.New is
        // non_retaining = 1, so condition 2 approves. Before the codegen arm
        // this verdict was correct and emitted nothing.
        35, "an owned error binding -> RELEASE_OK",
        "package main\n"
        "import \"errors\"\n"
        "func f() int {\n"
        "    e := errors.New(\"boom\")\n"
        "    return len(e.Error())\n"
        "}\n",
        "f", {{"e", RELEASE_OK}}, 1
    },
    {
        // SOUNDNESS. errors.Unwrap returns a pointer INTO its argument, so its
        // row is non_retaining = 0 and condition 2 must refuse. Inert before
        // the nullable-pointer arm; load-bearing after it.
        36, "an unwrapped error is BORROWED -> refused",
        "package main\n"
        "import \"errors\"\n"
        "func f() int {\n"
        "    outer := errors.New(\"boom\")\n"
        "    inner := errors.Unwrap(outer)\n"
        "    if inner == nil {\n"
        "        return 1\n"
        "    }\n"
        "    return 0\n"
        "}\n",
        "f", {{"inner", RELEASE_NO_NOT_OWNED}}, 1
    },
```

- [ ] **Step 8: Run the unit rows**

```bash
make release-decision-test 2>&1 | grep -E "Row 3[56]:|summary:"
```
Expected: both rows PASS, `146 assertions passed, 0 failed`.

- [ ] **Step 9: Wire both probes into the gate**

In `Makefile`, inside the `arc-release-probe` recipe, immediately before the final
`if [ $$fail -ne 0 ]; then echo "arc-release-probe: FAIL"; exit 1; fi; \` line, add:

```make
	GOO_ARC_RELEASE=0 $(COMPILER) -o build/arc_err_off examples/arc_release_error_probe.goo > build/arc_err_off.cerr 2>&1 \
	  || { echo "  FAIL (compile, error probe, release off)"; cat build/arc_err_off.cerr; exit 1; }; \
	$(COMPILER) -o build/arc_err_on examples/arc_release_error_probe.goo > build/arc_err_on.cerr 2>&1 \
	  || { echo "  FAIL (compile, error probe, release on)"; cat build/arc_err_on.cerr; exit 1; }; \
	valgrind --leak-check=full ./build/arc_err_off > build/arc_err_off.out 2> build/arc_err_off.vg; \
	ed=$$(grep -oP "definitely lost: \K[0-9,]+" build/arc_err_off.vg | tr -d , ); \
	ei=$$(grep -oP "indirectly lost: \K[0-9,]+" build/arc_err_off.vg | tr -d , ); \
	e_off=$$(( $${ed:-0} + $${ei:-0} )); \
	if [ "$$e_off" -eq 0 ]; then \
	  echo "  FAIL: error probe leaked nothing with the release OFF — it measures nothing"; fail=1; \
	else \
	  echo "  error OFF: $$e_off bytes leaked ($${ed:-0} direct + $${ei:-0} indirect)"; \
	fi; \
	valgrind --leak-check=full --error-exitcode=99 ./build/arc_err_on > build/arc_err_on.out 2> build/arc_err_on.vg; \
	rc=$$?; \
	ed2=$$(grep -oP "definitely lost: \K[0-9,]+" build/arc_err_on.vg | tr -d , ); \
	ei2=$$(grep -oP "indirectly lost: \K[0-9,]+" build/arc_err_on.vg | tr -d , ); \
	e_on=$$(( $${ed2:-0} + $${ei2:-0} )); \
	: "Access first, then leak, then rc -- see the owned-key block above for why." ; \
	if grep -qE "Invalid read|Invalid write|Invalid free|double free" build/arc_err_on.vg; then \
	  echo "  FAIL: an error local was released while still live"; tail -30 build/arc_err_on.vg; fail=1; \
	elif [ "$$e_on" -ne 0 ]; then \
	  echo "  FAIL: error probe still leaked $$e_on bytes"; fail=1; \
	elif [ $$rc -ne 0 ]; then \
	  echo "  FAIL: valgrind exited $$rc with no leak and no invalid access — read the log"; \
	  tail -30 build/arc_err_on.vg; fail=1; \
	elif ! diff -q build/arc_err_off.out build/arc_err_on.out > /dev/null; then \
	  echo "  FAIL: error probe output differs from the GOO_ARC_RELEASE=0 control"; fail=1; \
	else \
	  echo "  error ON:   clean, 0 bytes, output matches the control (was $$e_off)"; \
	fi; \
	$(COMPILER) -o build/arc_unw examples/arc_release_unwrap_probe.goo > build/arc_unw.cerr 2>&1 \
	  || { echo "  FAIL (compile, unwrap probe)"; cat build/arc_unw.cerr; exit 1; }; \
	valgrind --leak-check=full ./build/arc_unw > /dev/null 2> build/arc_unw.vg; \
	: "LEAKS ARE EXPECTED HERE and must NOT be asserted on -- the outer error is" ; \
	: "loop-scoped, so condition 4 refuses it. NO BACKTICKS in these strings." ; \
	if grep -qE "Invalid read|Invalid write|Invalid free|double free" build/arc_unw.vg; then \
	  echo "  FAIL: an UNWRAPPED error was released — it points into its parent"; \
	  grep -E "Invalid read|Invalid write|Invalid free|double free" build/arc_unw.vg | head -3; \
	  fail=1; \
	else \
	  echo "  unwrap:     clean — a borrowed inner error survived 1000 releases"; \
	fi; \
```

- [ ] **Step 10: Run the gate**

```bash
make arc-release-probe 2>&1 | tail -8
```
Expected: `error OFF: <n> bytes`, `error ON: clean, 0 bytes`, `unwrap: clean`, `arc-release-probe: PASS`.

- [ ] **Step 11: Prove the gate can report a failure**

Two mutants. Apply each with an asserting Python replace and **diff the file before believing any result** — a `sed` whose delimiter collides with the pattern silently no-ops and the suite reports PASS.

Mutant 1 — claim field 0 instead of field 1:
```bash
cp src/codegen/statement_codegen.c /tmp/sc.bak
python3 - <<'EOF'
p="src/codegen/statement_codegen.c"; s=open(p).read()
old="""               LLVMGetTypeKind(LLVMStructGetTypeAtIndex(slot_ty, 1)) == LLVMPointerTypeKind) {"""
assert s.count(old)==1, "TARGET NOT FOUND — abort"
i=s.index(old); j=s.index("field = 1;", i)
open(p,"w").write(s[:j]+"field = 0;"+s[j+len("field = 1;"):])
print("mutant 1 applied")
EOF
diff /tmp/sc.bak src/codegen/statement_codegen.c | head -5   # MUST show the change
make lexer >/dev/null 2>&1 && make arc-release-probe 2>&1 | grep -E "FAIL|PASS" | tail -3
cp /tmp/sc.bak src/codegen/statement_codegen.c && make lexer >/dev/null 2>&1
```
Expected: the gate goes **RED** (releasing an `i1` as a pointer).

Mutant 2 — make `errors.Unwrap` claim ownership:
```bash
cp src/types/shim_signatures.c /tmp/ss.bak
python3 - <<'EOF'
p="src/types/shim_signatures.c"; s=open(p).read()
old='{ "errors", "Unwrap", SHIM_RET_ERROR, PARAMS_ERROR,  NPARAMS(PARAMS_ERROR), 0, 0, 0 },'
assert s.count(old)==1, "TARGET NOT FOUND — abort"
open(p,"w").write(s.replace(old, old.replace(", 0, 0, 0 },", ", 0, 1, 0 },")))
print("mutant 2 applied")
EOF
diff /tmp/ss.bak src/types/shim_signatures.c | head -5   # MUST show the change
make lexer >/dev/null 2>&1
make release-decision-test 2>&1 | grep -E "Row 36:|summary:"
make arc-release-probe 2>&1 | grep -E "unwrap|FAIL" | head -4
cp /tmp/ss.bak src/types/shim_signatures.c && make lexer >/dev/null 2>&1
```
Expected: row 36 FAILs **and** the unwrap probe reports an invalid access.

Record both results in the PR body. If either mutant stays green, the gate is not measuring what it claims and must be strengthened before proceeding.

- [ ] **Step 12: Verify the tree is restored, then commit**

```bash
git diff --stat   # must show ONLY the intended files
make release-decision-test 2>&1 | grep "summary:"
git add src/codegen/statement_codegen.c tests/unit/types/release_decision_test.c \
        examples/arc_release_error_probe.goo examples/arc_release_unwrap_probe.goo Makefile
git -c commit.gpgsign=false commit -m "feat(arc): a nullable pointer releases field 1, so an error can be reclaimed

An error is TYPE_NULLABLE of *int8 (type_checker_error_type), lowering to
{ i1, ptr } -- field 0 is the nil FLAG and the object is FIELD 1. The shape
test accepted a bare pointer, a 3-field slice and a 2-field string, so an
error read RELEASE_OK and emitted NOTHING.

No emission change: the exit already GEPs site->field, so an arbitrary index
worked. -1 and 0 were the only values USED, not the only ones supported.

Stated over the TYPE, not the name -- ?*T behaves the same, and no string
comparison decides a free(). The leading field's kind AND width keep the
three struct arms mutually exclusive: a string is { ptr, i64 }.

Nil safety was already paid for. The error-union construction leaves field 1
UNDEF until it is filled, and codegen_arc_zero_slot's LLVMConstNull is
zeroinitializer for this shape, so an unwritten slot reads { false, null }
and goo_release is a no-op on NULL. Fail-closed, as since #267.

errors.Unwrap returns a pointer INTO its argument. Its refusal was INERT
before this arm and is load-bearing after it -- row 36 and a probe that reads
the parent's bytes now pin it.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Tuple-destructure ownership

`n, err := strconv.Atoi(f)` has two targets and one value. `seed_names` and the `AST_MULTI_ASSIGN` arm both record `NULL` when the counts differ, so condition 2 refuses every target.

Soundness comes from an existing definition, not a new one. Both ownership sources are already defined over the **whole** result list: `shim_signature_is_non_retaining` is *"does not retain a pointer argument past the call, AND does not return a value that aliases one"*, and `ParamEscapeSummary.return_escapes` is *"does F return a value derived from one of its own params?"*. If no result aliases an argument, every result is owned.

**Files:**
- Modify: `src/types/release_decision.c` (`seed_names` at ~line 269, and the `AST_MULTI_ASSIGN` arm at ~line 315)
- Modify: `tests/unit/types/release_decision_test.c`
- Create: `examples/arc_release_tuple_probe.goo`
- Modify: `Makefile`

**Interfaces:**
- Consumes: Task 2's `field = 1` arm (without it, an `err` target would still emit nothing).
- Produces: no new API. `note_declaration(ctx, name, value)` is simply called with the call node rather than `NULL`.

- [ ] **Step 1: Write the failing unit rows**

Append to `rows[]` in `tests/unit/types/release_decision_test.c`:

```c
    {
        // THE ROW THIS CHANGE EXISTS FOR. Two targets, ONE value. Before this,
        // both recorded NULL and condition 2 refused. strconv.Atoi is
        // non_retaining = 1, which is defined over the WHOLE result list, so
        // no result aliases the argument and every result is owned.
        37, "a tuple destructure from a non-retaining shim -> owned",
        "package main\n"
        "import \"strconv\"\n"
        "func f(s string) int {\n"
        "    n, err := strconv.Atoi(s)\n"
        "    if err == nil {\n"
        "        return n\n"
        "    }\n"
        "    return 0\n"
        "}\n",
        "f", {{"err", RELEASE_OK}}, 1
    },
    {
        // SOUNDNESS. A Goo callee whose return_escapes is TRUE returns a value
        // derived from a parameter, so NO target of its destructure is owned.
        38, "a tuple destructure from a borrowing callee -> refused",
        "package main\n"
        "func two(s string) (string, int) {\n"
        "    return s[1:], 1\n"
        "}\n"
        "func f(s string) int {\n"
        "    a, b := two(s)\n"
        "    return len(a) + b\n"
        "}\n",
        "f", {{"a", RELEASE_NO_NOT_OWNED}}, 1
    },
    {
        // UNCHANGED BEHAVIOUR, pinned. Counts MATCH here, so each target keeps
        // its own value and this change must not touch it.
        39, "two targets, two values -> each keeps its own binding",
        "package main\n"
        "import \"strings\"\n"
        "func f(s string) int {\n"
        "    a, b := strings.TrimSpace(s), s\n"
        "    return len(a) + len(b)\n"
        "}\n",
        "f", {{"a", RELEASE_OK}, {"b", RELEASE_NO_NOT_OWNED}}, 2
    },
```

- [ ] **Step 2: Run them to verify rows 37 and 38 behave as expected pre-change**

```bash
make release-decision-test 2>&1 | grep -E "Row 3[789]:|summary:"
```
Expected: **row 37 FAILS** (reads `RELEASE_NO_NOT_OWNED`, expected `RELEASE_OK`); rows 38 and 39 PASS. Row 38 passing pre-change is expected — it must *keep* passing, which is the point.

- [ ] **Step 3: Make the change**

In `seed_names`, replace the final line
`    for (size_t i = 0; i < name_count; i++) note_declaration(ctx, names[i], NULL);`
with:

```c
    // COUNTS DIFFER: `a, b := f()`, one call feeding several names. Attribute
    // the SINGLE value to every target rather than recording NULL.
    //
    // SOUNDNESS COMES FROM AN EXISTING DEFINITION, not a new one. Both
    // ownership sources are already stated over the WHOLE result list:
    // shim_signature_is_non_retaining is "does not retain a pointer argument
    // AND does not return a value that aliases one", and
    // ParamEscapeSummary.return_escapes is "does F return a value derived from
    // one of its own params?". Neither is per-result and neither needs to be:
    // if NO result aliases an argument, then EVERY result is owned.
    //
    // Only the one-value case qualifies. Any other mismatch is a shape this
    // module does not understand, and the safe answer there is still NULL.
    ASTNode* single = (value_count == 1) ? values : NULL;
    for (size_t i = 0; i < name_count; i++) note_declaration(ctx, names[i], single);
```

In the `AST_MULTI_ASSIGN` arm, replace
`                                             (tcount == vcount) ? v : NULL);`
with:

```c
                                             (tcount == vcount) ? v
                                                 : ((vcount == 1) ? n->values : NULL));
```

and update that arm's comment: the sentence *"so the value is recorded NULL and condition 2 refuses. That is the daemon's `n, err := strconv.Atoi(f)` shape"* is now false. Replace it with:

```c
                // `targets` is a NODE LIST, not a name array. `a, b := f()`
                // declares, `a, b = x, y` assigns; either way condition 4 only
                // needs the count. When ONE value feeds several names -- the
                // daemon's `n, err := strconv.Atoi(f)` shape -- that value is
                // attributed to every target, because non_retaining and
                // return_escapes are both defined over the whole result list.
                // Any other count mismatch stays NULL, which refuses.
```

- [ ] **Step 4: Run the rows again**

```bash
make release-decision-test 2>&1 | grep -E "Row 3[789]:|summary:"
```
Expected: all three PASS, `152 assertions passed, 0 failed`.

- [ ] **Step 5: Write the end-to-end probe**

Create `examples/arc_release_tuple_probe.goo`:

```go
// T4 gate: does a tuple destructure's error target release?
//
// `n, err := strconv.Atoi(s)` has TWO targets and ONE value. Before this,
// release_decision recorded NULL for both and condition 2 refused -- which is
// why .handoff.md's attribution of the daemon's err leak to condition 4 (loop
// scope) was never reached.
//
// strconv.Atoi is non_retaining = 1, and that bit is defined over the whole
// result list, so neither result aliases the argument. `n` is an i64 slot and
// the codegen shape test refuses it; `err` is { i1, ptr } and releases field 1.
// That split is the two-layer design working, not a special case.
//
// The bytes are read through Error(), not by comparing to nil: a nil test
// loads field 0, the FLAG, and never dereferences the object.
package main

import "strconv"

var sink int

func parse(s string) int {
	n, err := strconv.Atoi(s)
	if err == nil {
		return n
	}
	return len(err.Error())
}

func main() {
	total := 0
	for i := 0; i < 10000; i++ {
		total = total + parse("not a number")
	}
	sink = total
	println(total)
}
```

- [ ] **Step 6: Verify the probe releases cleanly**

```bash
make lexer >/dev/null 2>&1
GOO_ARC_DEBUG=1 ./bin/goo build -o build/arc_tup examples/arc_release_tuple_probe.goo 2>&1 | grep -E '\[arc\] parse:'
valgrind --leak-check=full --error-exitcode=99 ./build/arc_tup > /dev/null 2> /tmp/arc_tup.vg; echo "rc=$?"
grep -E "definitely lost|indirectly lost|All heap blocks" /tmp/arc_tup.vg
```
Expected: `will release err at exit (field=1, ...)`; `rc=0`; zero bytes lost.

- [ ] **Step 7: Wire the probe into the gate**

In `Makefile`, in `arc-release-probe`, before the final `if [ $$fail -ne 0 ]` line, add the same differential block as Task 2 step 9 with `arc_err`→`arc_tup`, `error probe`→`tuple probe`, `examples/arc_release_error_probe.goo`→`examples/arc_release_tuple_probe.goo`, and the label `error ON:`→`tuple ON:  `.

- [ ] **Step 8: Prove the gate can report a failure**

```bash
cp src/types/release_decision.c /tmp/rd.bak
python3 - <<'EOF'
p="src/types/release_decision.c"; s=open(p).read()
old="    ASTNode* single = (value_count == 1) ? values : NULL;"
assert s.count(old)==1, "TARGET NOT FOUND — abort"
open(p,"w").write(s.replace(old, "    ASTNode* single = values;   /* MUTANT: any count mismatch */"))
print("mutant applied")
EOF
diff /tmp/rd.bak src/types/release_decision.c | head -5   # MUST show the change
make lexer >/dev/null 2>&1
make release-decision-test 2>&1 | grep -E "summary:"
cp /tmp/rd.bak src/types/release_decision.c && make lexer >/dev/null 2>&1
```

Expected: the suite goes RED. If it stays green, the row set does not cover a
multi-value mismatch — add a row for `a, b, c := f(), g()` before proceeding.

- [ ] **Step 9: Full gate, then commit**

```bash
git diff --stat   # ONLY intended files
make arc-release-probe 2>&1 | tail -6
make verify-core 2>&1 | tail -3
```
Expected: `arc-release-probe: PASS` and `verify-core: ALL GREEN GATES PASSED`.

```bash
git add src/types/release_decision.c tests/unit/types/release_decision_test.c \
        examples/arc_release_tuple_probe.goo Makefile
git -c commit.gpgsign=false commit -m "feat(arc): one value feeding several names is owned by all of them

\`n, err := strconv.Atoi(f)\` has two targets and one value, so seed_names and
the MULTI_ASSIGN arm both recorded NULL and condition 2 refused every target.
That -- not condition 4 -- is what refused the daemon's err, and .handoff.md
attributed it to loop scope for two sessions.

Soundness reuses an existing definition. non_retaining is \"does not retain a
pointer argument AND does not return a value that aliases one\", and
return_escapes is \"does F return a value derived from one of its own
params?\". Neither is per-result and neither needs to be: if NO result
aliases an argument, EVERY result is owned.

ONLY the one-value case qualifies. Any other count mismatch is a shape this
module does not understand and stays NULL, which refuses.

The non-pointer target needs no special case: \`n\` is an i64 slot that the
codegen shape test refuses, which is the same two-layer split that lets the
integer \`+\` arm stay deliberately approximate.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Measure, and open the PR

**Files:** none modified. This task produces the PR body.

- [ ] **Step 1: Measure what A+B+C reclaim on the daemon**

```bash
./bin/goo build -o build/daemon bench/daemon/daemon.goo
valgrind --leak-check=full ./build/daemon 2000 >/dev/null 2> /tmp/d.vg
grep -E "definitely lost|indirectly lost" /tmp/d.vg
```

**Expected: 952,000 bytes, UNCHANGED.** `err` is loop-scoped as well as
tuple-bound, so condition 4 still refuses it. This is the predicted result, and
recording it is the point — do NOT report A+B+C as progress against the
340,000. If the number *does* move, stop: something is releasing that the
design did not predict, and it must be explained before merging.

- [ ] **Step 2: Confirm the three new probes ran rather than skipped**

```bash
make arc-release-probe 2>&1 | grep -E "error |unwrap|tuple |SKIPPED"
```
A gate that prints SKIPPED and returns success is not a gate.

- [ ] **Step 3: Push and open the PR**

```bash
git push -u origin feat/t4-error-release-unblock
gh pr create --base main --head feat/t4-error-release-unblock \
  --title "feat(arc): unblock error release — the two guards that refuse err before the kill rule" \
  --body-file /tmp/pr-body.md
```

The PR body must state, in this order: the two measured refusals with their
verdicts; that the daemon is **unchanged at 952,000** and why; the mutant table
from tasks 2 and 3 with each result; and that step D (the CFG) is the only
thing that moves the 340,000.

- [ ] **Step 4: Confirm CI green by reading the conclusion, not a pipe exit**

```bash
gh pr checks <N>
gh run view <run-id> --json conclusion --jq .conclusion
```
Expected: `success` for both `demos` and `verify-core`.

---

## Self-Review

**Spec coverage.** Spec step A → Task 1. Step B → Task 2 (arm, rows 35–36, error probe, unwrap probe, mutants). Step C → Task 3 (seed_names, MULTI_ASSIGN arm, rows 37–39, tuple probe, mutant). Spec's "Gates" section: every listed gate appears. Spec's "nothing before D moves the daemon" → Task 4 Step 1, which asserts the number is unchanged. Spec's step D is explicitly out of scope and gets no task, per the spec's own decomposition note.

**Placeholder scan.** No TBD/TODO. Every code step carries real code. The one templated instruction is Task 3 Step 7, which names each substitution explicitly rather than saying "similar to Task 2".

**Type consistency.** `field` is the existing `int` local in `codegen_arc_note_local`; `ArcReleaseSite.field` is already `int` and already GEP'd. `release_plan_verdict` / `release_verdict_name` signatures match `include/release_decision.h`.

Three facts verified against the source while writing this plan, rather than left for the implementer:

| claim | verified |
|---|---|
| the nullable member is `data.nullable.base_type` | `type_mapping.c:414` — an earlier draft of this plan said `inner_type` and was **wrong** |
| a nullable lowers to `{ i1 is_null, base_type }`, flag first | `codegen_get_nullable_type`, `type_mapping.c:410-422` |
| `rows[]` currently ends at row 34, so 35–39 are free | `release_decision_test.c` |
| the `errors.Unwrap` row text in mutant 2 matches byte-for-byte | `shim_signatures.c:197` |
