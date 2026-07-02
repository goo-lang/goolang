# Select-Statement Type Checking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `type_check_select_stmt` actually type-checks select statements: comm operations (send channel/value compatibility, recv validity) and case bodies (so expressions inside select get their `node_type` annotations — today `int64(x)` inside a select fails at codegen with "conversion: missing resolved target type", and a type-mismatched send silently reaches codegen's slot machinery).

**Architecture:** Fill in the existing no-op stub (`src/types/type_checker.c:1792-1807`) following `type_check_if_stmt`'s pattern: per case — a NULL `comm` is `default` (body only); a send comm (binary arrow) checks the left side is a channel and the value is assignable to its element type; a recv comm routes through the existing expression checker (which already has `type_check_channel_receive_op`); the body is walked with `type_check_statement`. No parser, codegen, or runtime changes.

**Tech Stack:** C23. Checker-only.

**Discovered:** during PR #93 (Task 2 probe had to hoist `int64()` conversions out of the select block). Recorded as gating gap 1g.

## Global Constraints

- Branch: `fix/select-typecheck` (already created off main @1b75a09 — do NOT commit on main).
- Commits: conventional style, imperative mood, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- No header edits expected (the function is already declared); if any, `make clean` first.
- Gate: `make lexer`, probes, then `eval "$(opam env --switch=default)"` STANDALONE, then `make verify` (all PASS; golden baseline 167/0 grows to 168/0) and `make test` (76 pass / 1 pre-existing skip). The select/channel goldens (`select-probe`, `parallel-select-soak-probe`, `chan-*`, `deadlock-*`) MUST stay green — this change makes previously-unchecked code paths checked, and a checker that's too strict will reject valid programs; if any golden regresses, the new checker is wrong (report the exact rejection).
- Bison conflicts: no grammar changes — 79 S/R + 256 R/R untouched.
- Probes: same-width prints only (mixed-width Println bug).

## Reference: verified code landmarks (2026-07-02, main @1b75a09)

- The stub: `src/types/type_checker.c:1792-1807` — loops cases with a TODO, returns 1.
- Pattern to follow: `type_check_if_stmt` (same file, ~L60 above the stub) — check expr, type_error + return 0 on mismatch, `type_check_statement` for bodies.
- `SelectCaseNode` (`include/ast.h:376-380`): `comm` (NULL = default) + `body`. VERIFY how codegen walks the body (the select codegen in `src/codegen/statement_codegen.c` around the `codegen_setup_select_case` region and its body-emission counterpart) — mirror the same walk shape (single statement vs `->next` chain) in the checker; the block case in `type_check_statement` (~L1360-1370) shows the chain-walk idiom.
- Comm node shapes (from the select codegen, statement_codegen.c ~L1795-1830): send = `AST_BINARY_EXPR` with the arrow operator (`binary->left` channel, `binary->right` value); recv = `AST_UNARY_EXPR` with `TOKEN_ARROW`. VERIFY against the parser's select rules before coding; if the grammar also allows `v := <-ch` comm forms (assignment/short-decl wrapping a recv), handle them by delegating the whole comm node to `type_check_statement`/`type_check_expression` as appropriate — check what node the parser actually builds.
- Channel type shape: `TYPE_CHANNEL` with `data.channel.element_type` (used by the send coercion in lowlevel_codegen.c:44+).
- Assignability: `type_compatible(value_t, elem_t)` — the same predicate `delete`'s key check and `append`'s elem check use (`src/types/expression_checker.c`).
- Existing recv checking: `type_check_channel_receive_op` (referenced from `type_check_unary_expr`'s TOKEN_ARROW arm) — recv comm exprs can just go through `type_check_expression`.
- Reject-probe Makefile pattern: `boolnot-reject-probe` (search Makefile); registration in `verify:` before `test-golden`.

---

### Task 1: Implement `type_check_select_stmt` + probes

**Files:**
- Modify: `src/types/type_checker.c:1792-1807` (the stub)
- Modify: `Makefile` (new `selectsend-reject-probe` target + registration)
- Test: `examples/select_typecheck_probe.goo` + `examples/select_typecheck_probe.expected.txt`

**Interfaces:**
- Consumes: `type_check_expression`, `type_check_statement`, `type_compatible`, `type_error` — all existing.
- Produces: select comm exprs and bodies carry checker annotations (`node_type` stamped), so conversions and all expression forms work inside select; type-mismatched sends and non-channel sends are clean compile errors.

- [ ] **Step 1: Write the failing probe**

`examples/select_typecheck_probe.goo`:
```go
package main

import "fmt"

type T struct {
	N int32
}

func main() {
	ch := make_chan(int64, 1)
	t := T{N: 5}
	sent := 0
	select {
	case ch <- t.N:
		sent = int(int64(1))
	default:
		sent = int(int64(0))
	}
	fmt.Println(sent)
	v := <-ch
	fmt.Println(int(v))
	c2 := make_chan(int, 1)
	c2 <- 3
	select {
	case got := <-c2:
		fmt.Println(int(got) + 10)
	default:
		fmt.Println(0)
	}
}
```

`examples/select_typecheck_probe.expected.txt`:
```
1
5
13
```

Coverage: `int64()`/`int()` conversions INSIDE select bodies (the motivating failure — PR #93's probe had to hoist these out), a send comm with a narrow lvalue (annotations feed the #93 coercion), a recv-binding case (`got := <-c2`) with arithmetic on the bound value inside the body. NOTE: if `case got := <-c2:` turns out not to parse (check first with a tiny throwaway — the existing `select_probe.goo` only uses bare `<-a` comms), replace that case with a bare `<-c2` recv comm and `fmt.Println(13)` in the body, keep the expected file consistent, and report the grammar limitation.

- [ ] **Step 2: Verify it fails today**

Run: `bin/goo examples/select_typecheck_probe.goo 2>&1 | head -3`
Expected: `conversion: missing resolved target type` (a codegen error — the checker never annotated the select body). Record the exact output.

- [ ] **Step 3: Implement the checker**

Replace the stub body in `src/types/type_checker.c`:

```c
int type_check_select_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_SELECT_STMT) return 0;

    SelectStmtNode* select_stmt = (SelectStmtNode*)stmt;

    int ok = 1;
    for (ASTNode* case_node = select_stmt->cases; case_node; case_node = case_node->next) {
        if (case_node->type != AST_SELECT_CASE) continue;  // verify the actual tag name in ast.h
        SelectCaseNode* sc = (SelectCaseNode*)case_node;

        // comm == NULL is the default case — body only.
        if (sc->comm) {
            if (sc->comm->type == AST_BINARY_EXPR /* send: ch <- v */) {
                BinaryExprNode* send = (BinaryExprNode*)sc->comm;
                Type* chan_t = type_check_expression(checker, send->left);
                if (!chan_t) { ok = 0; }
                else if (chan_t->kind != TYPE_CHANNEL) {
                    type_error(checker, send->left->pos,
                               "select send requires a channel, got %s", type_to_string(chan_t));
                    ok = 0;
                } else {
                    Type* val_t = type_check_expression(checker, send->right);
                    if (!val_t) { ok = 0; }
                    else if (!type_compatible(val_t, chan_t->data.channel.element_type)) {
                        type_error(checker, send->right->pos,
                                   "select send: cannot use %s as %s channel element",
                                   type_to_string(val_t),
                                   type_to_string(chan_t->data.channel.element_type));
                        ok = 0;
                    }
                }
            } else {
                // Receive comm (<-ch, or a binding form the parser may wrap it
                // in). Route through the general checkers so the existing
                // channel-receive checking and any binding declaration apply.
                // VERIFY the actual node kinds the parser builds and dispatch
                // accordingly (expression vs statement).
                if (!type_check_expression(checker, sc->comm)) ok = 0;
            }
        }

        // Body: walk statements like the block checker does.
        for (ASTNode* s = sc->body; s; s = s->next) {   // adjust to the real body shape
            if (!type_check_statement(checker, s)) ok = 0;
        }
    }
    return ok;
}
```

This block is the SHAPE — before coding, verify against reality: (a) the select-case AST tag constant, (b) the send comm's operator/token check used by codegen (match it), (c) whether the body is a `->next` chain or a single block node (mirror codegen's walk), (d) whether recv comms can be binding forms and what node they build (dispatch to `type_check_statement` if they're statements). Match the file's error-message style. Accumulating `ok` (rather than early return) reports multiple errors like the block checker; keep whichever style the neighboring checkers use — read them first.

- [ ] **Step 4: Rebuild and verify the probe passes**

Run: `make lexer`, then `bin/goo -o build/select_typecheck_probe examples/select_typecheck_probe.goo && ./build/select_typecheck_probe`
Expected: `1 5 13`, one per line.

- [ ] **Step 5: Add the reject probe**

Makefile target following `boolnot-reject-probe`'s exact shape:

```make
# Select comms are now type-checked — sending the wrong element type must be
# a clean compile error, not silent slot-machinery corruption at runtime.
selectsend-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== selectsend-reject-probe: type-mismatched select send must reject ==="
	@printf 'package main\nfunc main(){ ch := make_chan(int, 1); select { case ch <- "oops":\n_ = 1\ndefault:\n_ = 2\n } }\n' > build/selectsend_reject.goo
	@rm -f build/selectsend_reject
	@$(COMPILER) -o build/selectsend_reject build/selectsend_reject.goo > build/selectsend_reject.out 2> build/selectsend_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "selectsend-reject-probe: FAIL (compiled rc=0 — mismatched send silently accepted)"; exit 1; fi; \
	if [ -x build/selectsend_reject ]; then echo "selectsend-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "select send" build/selectsend_reject.err; then echo "selectsend-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/selectsend_reject.err; exit 1; fi; \
	echo "selectsend-reject-probe: PASS (rejected rc=$$rc)"
```

(If the printf'd one-liner trips the known `}; stmt` same-line parse quirk, reshape it with more `\n`s — the probe source just needs a mismatched send inside a select with a default case; adjust and note it.) Register in `verify:` before `test-golden`. Run `make selectsend-reject-probe` → PASS.

- [ ] **Step 6: Run the gate**

`eval "$(opam env --switch=default)"` (standalone), `make verify` → all PASS, golden 168/0, select/channel goldens green. `make test` → 76/1.

- [ ] **Step 7: Commit**

```bash
git add src/types/type_checker.c Makefile examples/select_typecheck_probe.goo examples/select_typecheck_probe.expected.txt
git commit --no-gpg-sign -m "fix(types): type-check select statements

type_check_select_stmt was a no-op stub: select comm operations and
case bodies were never type-annotated, so conversions inside select
failed at codegen (missing resolved target type) and a type-mismatched
send reached the runtime slot machinery unchecked. Check send comms
(channel + element assignability), route recv comms through the
expression checker, and walk case bodies like the block checker.
selectsend-reject-probe guards the mismatch diagnostic."
```

---

## Final gate

`make verify` → ALL GREEN (168/0). `make test` → 76/1. ccomp (checker-only change — run anyway):
```bash
eval "$(opam env --switch=default)"
make ccomp-link
```

## Self-review notes

- Single task: the checker fill-in and its two probes are one reviewable unit.
- The Step 3 code is explicitly labeled a SHAPE with four verify-against-reality points (AST tag, send operator check, body walk shape, recv binding forms) — the implementer confirms each against the parser/codegen before coding.
- Risk called out in constraints: over-strict checking would reject valid programs; the existing select goldens are the guard.
- Out of scope: making codegen use the new annotations beyond what it already reads; recv-binding grammar additions if `got := <-ch` doesn't parse (report instead).
