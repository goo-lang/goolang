// ARC leg: per-LOCAL escape summaries — table-driven unit test.
//
// Same shape as param_escape_test.c and block_escape_test.c: a Goo source
// string -> parse_input -> type_check_program (ignore rc; needed to populate
// FuncLitNode.captured_names the way the real pipeline does, per the
// closure-capture row) -> local_escape_analyze -> assert one boolean per
// named local.
//
// The SOUNDNESS rows are the load-bearing ones: an unsound implementation
// shows up as a wrong `false` there, and a consumer will free on a false.
// The PRECISION rows are what stop a lazy mark-everything implementation
// from passing — without them, `return true;` satisfies the whole table and
// ARC reclaims nothing.

#include "parser.h"
#include "ast.h"
#include "types.h"
#include "local_escape.h"
#include "../goo_check.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_EXPECT_LOCALS 4

typedef struct {
    const char*   name;
    bool          expected_escapes;
    // ADR 0005. Checked ONLY when check_reasons is set, so every row written
    // before the reason set existed keeps its exact two-field initialiser and
    // its exact assertion count.
    EscapeReasons expected_reasons;
    bool          check_reasons;
} LocalExpectation;

typedef struct {
    int              row;
    const char*      description;
    const char*      src;
    const char*      fn;                          // function whose locals we assert
    LocalExpectation expect[MAX_EXPECT_LOCALS];
    int              expect_count;
} TestRow;

static TestRow rows[] = {
    // ---------------- PRECISION: must be false ----------------
    {
        1, "local never used past its own scope -> false",
        "package main\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    _ = x\n"
        "}\n",
        "f", { { "x", false } }, 1
    },
    {
        2, "local passed to a NON-retaining callee -> false",
        "package main\n"
        "func sink(p *int) {\n"
        "}\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    sink(x)\n"
        "}\n",
        "f", { { "x", false } }, 1
    },
    {
        3, "two locals, neither leaves the function -> false, false",
        "package main\n"
        "func f() {\n"
        "    a := new(int)\n"
        "    b := a\n"
        "    _ = b\n"
        "}\n",
        "f", { { "a", false }, { "b", false } }, 2
    },
    {
        4, "local declared inside a loop, dies each iteration -> false",
        "package main\n"
        "func f() {\n"
        "    for i := 0; i < 3; i++ {\n"
        "        t := new(int)\n"
        "        _ = t\n"
        "    }\n"
        "}\n",
        "f", { { "t", false } }, 1
    },

    // ---------------- SOUNDNESS: must be true ----------------
    {
        5, "local returned -> true",
        "package main\n"
        "func f() *int {\n"
        "    x := new(int)\n"
        "    return x\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        6, "local stored into a package global -> true",
        "package main\n"
        "var g *int\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    g = x\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        7, "local passed to a RETAINING callee -> true",
        "package main\n"
        "var g *int\n"
        "func stash(p *int) {\n"
        "    g = p\n"
        "}\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    stash(x)\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        8, "local captured by a closure -> true",
        "package main\n"
        "var h func()\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    h = func() { _ = x }\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        9, "local passed as a goroutine argument -> true",
        "package main\n"
        "func work(p *int) {\n"
        "}\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    go work(x)\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        10, "local used as a METHOD-CALL RECEIVER -> true (unresolved callee)",
        // The receiver rule fixed in PR #248: a selector callee resolves to
        // no summary, and a receiver is not a member of call->args, so the
        // retain-all rule for an unresolved callee never covered it. This
        // module must not reintroduce that hole.
        "package main\n"
        "type T struct { x int }\n"
        "var g *T\n"
        "func (t *T) stash() {\n"
        "    g = t\n"
        "}\n"
        "func f() {\n"
        "    p := &T{x: 1}\n"
        "    p.stash()\n"
        "}\n",
        "f", { { "p", true } }, 1
    },
    {
        11, "transitive: local copied into another that is returned -> true, true",
        "package main\n"
        "func f() *int {\n"
        "    a := new(int)\n"
        "    b := a\n"
        "    return b\n"
        "}\n",
        "f", { { "a", true }, { "b", true } }, 2
    },
    {
        12, "local passed to an EXTERNAL/unregistered callee -> true",
        "package main\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    unknownExternal(x)\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        13, "local stored through a pointer deref -> true",
        "package main\n"
        "func f(out **int) {\n"
        "    x := new(int)\n"
        "    *out = x\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        14, "local sent on a channel -> true",
        "package main\n"
        "func f(ch chan *int) {\n"
        "    x := new(int)\n"
        "    ch <- x\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // SOUNDNESS. A map KEY is a stored reference, and nothing used to say
        // so: assign_to_lvalue marks only the RIGHT-hand taint, so the INDEX
        // expression of a non-identifier lvalue was never tainted.
        //
        // Measured before the fix: `m` ESCAPES and `k` did NOT. But
        // goo_map_set_sv (src/runtime/runtime.c) stores the key pointer
        // VERBATIM and never frees it, so the returned map genuinely holds
        // `k`. An ARC release consumer acting on that verdict frees a buffer
        // a live, returned map still points at.
        //
        // Found by docs/superpowers/specs/
        // 2026-07-28-daemon-alloc-attribution-findings.md. The slice
        // equivalent (row 16) was already sound, because append is an
        // ordinary call and the call sink covers it.
        15, "local used as a MAP KEY, map escapes -> true (key is a stored ref)",
        "package main\n"
        "func f(s string) map[string]int {\n"
        "    m := map[string]int{}\n"
        "    k := s + \"x\"\n"
        "    m[k] = 1\n"
        "    return m\n"
        "}\n",
        "f", { { "m", true }, { "k", true } }, 2
    },
    {
        // The slice counterpart of row 15, kept as the CONTRAST: this one was
        // already true before the map-key fix, and it must stay true.
        16, "local appended to a slice that escapes -> true",
        "package main\n"
        "func f(s string) []string {\n"
        "    parts := []string{}\n"
        "    k := s + \"x\"\n"
        "    parts = append(parts, k)\n"
        "    return parts\n"
        "}\n",
        "f", { { "parts", true }, { "k", true } }, 2
    },

    // ---------------- SOUNDNESS: one row per unguarded engine arm ----------
    //
    // docs/adr/0002-measurements/escape_arm_coverage.md mutated all 17 arms of
    // escape_expr_taint in BOTH directions and measured which suite notices.
    // Eleven of the 16 testable arms had NO soundness coverage in ANY of the
    // three suites: nothing detected the arm claiming its expression aliases
    // nothing, and under-marking is the one bug class that can dangle a
    // pointer once T4 emits a release.
    //
    // Every row below routes the taint THROUGH one arm with NO OTHER PATH to a
    // sink. That is deliberate and it is why row 15 does not already cover the
    // binary arm: row 15 contains `k := s + "x"`, but `k`'s verdict comes from
    // sink #2b (the map key), so under-marking the binary arm leaves row 15
    // green. A row that passes for a second reason measures nothing.
    //
    // Each row was verified to FAIL under
    // `scripts/escape_arm_coverage.sh <ARM> under` and to pass with the arm
    // restored.
    {
        // AST_BINARY_EXPR. `s` is bound to a LITERAL, so it carries only its
        // own bit and no call sink touches it. The concatenation is the only
        // route to the global.
        17, "local reaches a global THROUGH a binary expression -> true",
        "package main\n"
        "var g string\n"
        "func f() {\n"
        "    s := \"abc\"\n"
        "    g = s + \"def\"\n"
        "}\n",
        "f", { { "s", true } }, 1
    },
    {
        // AST_STRUCT_LITERAL. The arm unions over field_values, which is also
        // why AST_KEYED_ELEMENT never runs: the parser hands the values list
        // directly and no keyed-element node is walked.
        18, "local carried into a global inside a STRUCT LITERAL -> true",
        "package main\n"
        "type T struct { p *int }\n"
        "var g T\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    g = T{p: x}\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // AST_SLICE_EXPR (a SliceLitNode, not a slice-index). This is the
        // analysis-side guard for the "slices of pointers" ledger item:
        // goo_slice_append copies raw bytes, so nothing counts an appended
        // pointer at RUNTIME, but the analysis must at least mark it.
        19, "local carried into a global inside a SLICE LITERAL -> true",
        "package main\n"
        "var g []*int\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    g = []*int{x}\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // AST_PAREN_EXPR is the MAP LITERAL (MapLitNode). The arm walks the
        // keys list AND the values list, so this row taints one of each: `k`
        // proves the key loop, `x` proves the value loop. One row, two loops.
        // The map is 23.2% of the daemon's retained bytes.
        20, "locals carried into a global inside a MAP LITERAL -> true, true",
        "package main\n"
        "var g map[string]*int\n"
        "func f() {\n"
        "    k := \"a\"\n"
        "    x := new(int)\n"
        "    g = map[string]*int{k: x}\n"
        "}\n",
        "f", { { "k", true }, { "x", true } }, 2
    },
    {
        // AST_INDEX_EXPR, and this is its FIRST fixture in any of the three
        // suites. `--reach` measured 0 hits across all 70 rows before this one.
        //
        // That zero looked wrong against PR #255, whose headline was the
        // map-key sink, so it was checked: assign_to_lvalue handles the WRITE
        // `m[k] = v` through mark_lvalue_subscripts (sink #2b) and never routes
        // through this arm. This arm is the READ, `v := xs[i]`, and nothing
        // exercised it.
        //
        // What it must assert: an element read OUT of a container aliases the
        // container. `xs[0]` can be a pointer the slice's buffer holds, so if
        // the element reaches a global the slice has to be kept alive too.
        // Under-marking here would let T4 release a buffer that a live global
        // still points into.
        21, "local read by INDEX into a global -> true (the element aliases the base)",
        "package main\n"
        "var g *int\n"
        "func f() {\n"
        "    xs := []*int{}\n"
        "    g = xs[0]\n"
        "}\n",
        "f", { { "xs", true } }, 1
    },
    {
        // FLIPPED FROM `true` TO `false` on 2026-08-01, and the previous
        // version of this comment asked for an argument rather than an
        // accident. Here it is.
        //
        // WHAT CHANGED. escape_expr_taint's AST_INDEX_EXPR arm no longer unions
        // the INDEX's taint into a READ's result. On a WRITE the key is still
        // marked, from the lvalue side, by mark_lvalue_subscripts -- so row 15
        // is untouched and goo_map_set_sv's verbatim key is still covered.
        //
        // WHY IT IS SOUND. `m[k]` yields the STORED VALUE. No expression makes
        // that value alias the key: goo_map_get stores nothing, and a slice
        // index is an integer. The one shape that looks dangerous, `m[k] = k`,
        // is a WRITE and marks k twice over -- CONTAINER_STORE from the
        // right-hand side and SUBSCRIPT_STORE from the subscript.
        //
        // WHY THE OLD "~0%" WAS RIGHT AND IS NOW WRONG. Under one boolean the
        // WRITE marked the daemon's key anyway, so tightening the READ moved no
        // verdict and the measurement said ~0%. Under ADR 0005's reason set the
        // same edit is what makes the map-key consumer reachable: the daemon's
        // `f` goes from SUBSCRIPT_STORE|CONTAINER_STORE to SUBSCRIPT_STORE
        // alone, and "escapes ONLY as a subscript" is the safety question a map
        // must answer before it may own a key. Measured, and row 40 pins it.
        //
        // WHAT THIS ROW GUARDS NOW. Restoring the union makes it FAIL, so the
        // precision is pinned in the same way the conservatism was.
        22, "local used only as a map READ key -> false (a read retains nothing)",
        "package main\n"
        "var g *int\n"
        "func f(m map[string]*int) {\n"
        "    k := \"a\"\n"
        "    g = m[k]\n"
        "}\n",
        "f", { { "k", false } }, 1
    },

    // ---------------- SOUNDNESS: the STATEMENT arms, which are the sinks ----
    //
    // The arm matrix was extended to escape_walk_stmt, and the statement arms
    // measured worse than the expression ones. An expression arm only
    // PROPAGATES taint; a statement arm DECIDES what escapes. Skipping one
    // deletes a sink outright.
    //
    // Measured across all 78 rows of the three suites: AST_MULTI_ASSIGN,
    // AST_SWITCH_STMT, AST_TYPE_SWITCH and AST_SELECT_STMT were NEVER REACHED,
    // and AST_IF_STMT had zero hits in param and local. The three
    // control-flow arms are the dangerous ones, because each RECURSES into a
    // nested statement body -- skip the arm and the whole body goes unwalked,
    // so `switch n { case 1: return x }` never marks x at all.
    //
    // Each row verified to FAIL under
    // `scripts/escape_arm_coverage.sh <ARM> stmt-under` and to pass with the
    // arm restored.
    {
        // AST_MULTI_ASSIGN. A sink, and `v, err := f()` is among the commonest
        // statements in Go, yet nothing in 78 rows exercised it.
        23, "local stored to a global by MULTI-ASSIGN -> true",
        "package main\n"
        "var g *int\n"
        "var h *int\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    g, h = x, x\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // AST_SWITCH_STMT. The escape is inside a case body, so the arm's
        // recursion into that body is the only thing that finds it.
        24, "local returned from inside a SWITCH case -> true",
        "package main\n"
        "func f(n int) *int {\n"
        "    x := new(int)\n"
        "    switch n {\n"
        "    case 1:\n"
        "        return x\n"
        "    }\n"
        "    return nil\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // AST_TYPE_SWITCH. Same shape, different arm -- a type switch carries
        // its own node kind and its own recursion.
        25, "local stored to a global inside a TYPE SWITCH case -> true",
        "package main\n"
        "var g *int\n"
        "func f(v interface{}) {\n"
        "    x := new(int)\n"
        "    switch v.(type) {\n"
        "    case int:\n"
        "        g = x\n"
        "    }\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // AST_SELECT_STMT. The channel send is the sink, and the select arm is
        // what routes the comm clause to reach it.
        //
        // THIS COMMENT WAS ONLY TRUE FROM THE COMM FIX ONWARDS. Before it, the
        // arm handed comm -- an EXPRESSION -- to escape_walk_stmt, which fell to
        // `default:` and called escape_mark_all. `x` was marked because
        // EVERYTHING was, so this row was green whether or not a send sink
        // existed. Measured during the fix: routing comm through plain
        // escape_expr_taint with no sink makes this row report `x` NOT escaping.
        // That is the under-mark this row now genuinely guards.
        26, "local sent on a channel inside a SELECT clause -> true",
        "package main\n"
        "func f(ch chan *int) {\n"
        "    x := new(int)\n"
        "    select {\n"
        "    case ch <- x:\n"
        "    }\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // AST_IF_STMT had 0 hits in param and local. `if` is everywhere in real
        // code, so an unwalked then-branch is a broad hole rather than a narrow
        // one.
        27, "local returned from inside an IF branch -> true",
        "package main\n"
        "func f(n int) *int {\n"
        "    x := new(int)\n"
        "    if n > 0 {\n"
        "        return x\n"
        "    }\n"
        "    return nil\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // THE PRECISION ROW, and it is the one this change exists for. `y` is
        // never named by the select, so nothing should make it escape. Before the
        // select arm was fixed it DID: escape_walk_stmt got `sc->comm`, which is
        // an EXPRESSION, fell to `default:` and called escape_mark_all, so every
        // local in any function containing a select read as escaping.
        //
        // `x` in the same function still escapes, and that contrast is what keeps
        // the row honest: the send sink survived the change. A version that simply
        // stopped walking comm would pass on `y` and fail on `x`.
        28, "a local NOT named by a select does not escape; the sent one still does",
        "package main\n"
        "func f(ch chan *int) {\n"
        "    x := new(int)\n"
        "    y := new(int)\n"
        "    select {\n"
        "    case ch <- x:\n"
        "    }\n"
        "    _ = y\n"
        "}\n",
        "f", { { "x", true }, { "y", false } }, 2
    },

    // ---------------- THE SELF-STORE RULE (PR B) ----------------
    //
    // `m[k] = m[k] + 1` marked `m` escaping ON ACCOUNT OF ITS OWN CONTENTS.
    // assign_to_lvalue marks rhs_taint for a non-identifier lvalue, and the
    // right side carries m's OWN bit out of the AST_INDEX_EXPR arm. So the
    // compound update was the ONE map shape that refused to release: a plain
    // write, a parameter key, a write in a loop and a write with an import all
    // released already.
    //
    // Measured cost of the refusal, at bench/daemon/daemon.goo:31 — 902,000 of
    // 2,209,982 bytes per 2,000 requests (40.8%), of which 822,000 are the
    // entry-chain nodes that goo_map_dtor (#269) already knows how to free.
    {
        // THE PRECISION ROW, and the only one here that failed before the rule
        // existed. `m` never leaves f, so nothing may mark it. Its key is a
        // PARAMETER, deliberately: that keeps the map-key sink out of the way
        // so this row measures the self-store and nothing else.
        29, "map updated from its OWN contents, never escapes -> false",
        "package main\n"
        "func f(s string) int {\n"
        "    m := map[string]int{}\n"
        "    m[s] = m[s] + 1\n"
        "    return len(m)\n"
        "}\n",
        "f", { { "m", false } }, 1
    },
    {
        // SOUNDNESS: the rule must not reach past the base's own taint. Here
        // the stored value is a DIFFERENT local, so subtracting outer's taint
        // leaves inner's bit in place and inner still escapes.
        //
        // This row passes BOTH before and after the change, by design. It is
        // the guard that fails if the subtraction is written too wide — for
        // example by skipping escape_mark for every index lvalue.
        30, "a DIFFERENT local stored into a map that escapes -> true",
        "package main\n"
        "func f(s string) map[string]*int {\n"
        "    inner := new(int)\n"
        "    outer := map[string]*int{}\n"
        "    outer[s] = inner\n"
        "    return outer\n"
        "}\n",
        "f", { { "outer", true }, { "inner", true } }, 2
    },
    {
        // SOUNDNESS: the KEY path must survive the subtraction. Row 15 records
        // why this is the dangerous direction — goo_map_set_sv stores the key
        // pointer verbatim and never frees it, so a released key dangles inside
        // a live map.
        //
        // mark_lvalue_subscripts runs AFTER the subtraction and on its own
        // path, so `k` is marked by the key sink and not by rhs_taint. A
        // subtraction written over the whole assignment instead of over
        // rhs_taint alone turns this row red.
        31, "self-store with a LOCAL key: the key still escapes -> true",
        "package main\n"
        "func f(s string) map[string]int {\n"
        "    m := map[string]int{}\n"
        "    k := s + \"x\"\n"
        "    m[k] = m[k] + 1\n"
        "    return m\n"
        "}\n",
        "f", { { "m", true }, { "k", true } }, 2
    },
    {
        // SOUNDNESS: the subtraction is local to ONE assignment and must not
        // follow the map to another sink. `m` is self-stored and then given to
        // a global, which is an identifier lvalue and a different code path.
        32, "self-stored map later stored to a global -> true",
        "package main\n"
        "var g map[string]int\n"
        "func f(s string) {\n"
        "    m := map[string]int{}\n"
        "    m[s] = m[s] + 1\n"
        "    g = m\n"
        "}\n",
        "f", { { "m", true } }, 1
    },

    // ---------------- SOUNDNESS: the pass's OWN hooks, not the engine -------
    //
    // The three rows below exist because scripts/escape_teeth.sh mutated this
    // pass's own hooks (not the shared engine, which escape_arm_coverage.sh
    // already covers) and found them unguarded: every mutation left all 32 rows
    // above green. See that script's header for the two mutation directions.
    {
        // Written to isolate local_callee_retention's
        // `*out_return_escapes = callee->return_escapes`, and MEASUREMENT SHOWED
        // IT CANNOT. The negative result is kept because it is what justifies
        // scripts/escape_teeth.sh carrying no such entry for this pass.
        //
        // Sink #1 (AST_RETURN_STMT) marks the returned value before on_return
        // records the signal, so `id` comes out with escapes[0] AND
        // return_escapes both true -- param_escape_test row 26 asserts that. At
        // the call site `retains` reads escapes[0], already true, so `x` is
        // marked by sink #5 whatever return_escapes says.
        //
        // The row still earns its place: it covers a local reaching a global
        // through a CALL RESULT rather than a direct store.
        33, "local escapes THROUGH a callee that returns its own parameter -> true",
        "package main\n"
        "var g *int\n"
        "func id(p *int) *int {\n"
        "    return p\n"
        "}\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    g = id(x)\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // LOCAL_HOOKS.defer_is_like_go. The source comment on that field asks
        // for exactly this row -- "Tighten it only together with that ordering,
        // and with a row" -- and no row existed until the teeth said so.
        //
        // `sink` deliberately does NOT retain its parameter, so sink #5 is
        // silent and the defer treatment is the only thing that can mark `x`.
        // With the field flipped to false a defer becomes an ordinary call,
        // `x` reads non-escaping, and a release emitted before the deferred
        // call runs would dangle.
        34, "local passed to a DEFER whose callee does not retain it -> true",
        "package main\n"
        "func sink(p *int) {\n"
        "}\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    defer sink(x)\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // unit_add_local's `_` skip, which no fixture above reaches: `_` only
        // arrives there from a DECLARATION, and every `_` in the rows above is
        // an assignment (`_ = p`), which never calls unit_add_local.
        //
        // THE MEASURED ANSWER, and the reason scripts/escape_teeth.sh has no
        // blank-skip entry: removing the skip changes NOTHING. `_` takes slot 0
        // and pushes `x` to slot 1, but the own-bit seeding and the name-to-slot
        // lookup move together, so the shift cancels. `_` is never read, so its
        // bit reaches no sink. The skip saves a slot; it decides no verdict.
        //
        // The row stays as the fixture that fact rests on -- it is the only one
        // here that puts a blank name in a DECLARATION.
        35, "a local declared beside a BLANK name still escapes -> true",
        "package main\n"
        "var g *int\n"
        "func f() {\n"
        "    _, x := 1, new(int)\n"
        "    g = x\n"
        "}\n",
        "f", { { "x", true } }, 1
    },

    // ================== ADR 0005: WHICH reason, not just whether ==========
    //
    // One row per NAMED reason. A name nothing pins can be wrong forever: the
    // boolean stays true either way, so every row above passes whichever cause
    // the mark records. These are the only rows that can tell the names apart.
    //
    // EACH ASSERTS THE WHOLE SET, not a bit test. `expected_reasons` is
    // compared with `==`, so an EXTRA reason fails the row. That is deliberate
    // and it is what the map-key consumer needs: its safety question is
    // "escapes ONLY via the subscript", and a row that passed on a superset
    // would not be testing that question.
    //
    // NO ROW PINS UNCLASSIFIED, and this is the honest gap. Its four sites are
    // the two default arms plus two defensive arms, and reaching any of them
    // needs an AST node kind the engine does not know or a shape Go's grammar
    // does not produce. A fixture would have to be a deliberately broken tree.
    // scripts/escape_arm_coverage.sh already mutates arms INTO the default, so
    // the catch-all is exercised there rather than here.
    {
        36, "ADR 0005: returned local -> RETURN alone",
        "package main\n"
        "func f() *int {\n"
        "    x := new(int)\n"
        "    return x\n"
        "}\n",
        "f", { { .name = "x", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_RETURN, .check_reasons = true } }, 1
    },
    {
        37, "ADR 0005: stored to a package global -> GLOBAL_STORE alone",
        "package main\n"
        "var g *int\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    g = x\n"
        "}\n",
        "f", { { .name = "x", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_GLOBAL_STORE, .check_reasons = true } }, 1
    },
    {
        // The VALUE side. The key is a LITERAL, so nothing marks a subscript
        // and this row isolates CONTAINER_STORE from row 39's SUBSCRIPT_STORE.
        38, "ADR 0005: stored as a map VALUE -> CONTAINER_STORE alone",
        "package main\n"
        "func f(m map[string]*int) {\n"
        "    x := new(int)\n"
        "    m[\"k\"] = x\n"
        "}\n",
        "f", { { .name = "x", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_CONTAINER_STORE, .check_reasons = true } }, 1
    },
    {
        // THE ROW THE WHOLE ADR EXISTS FOR. The value stored is a literal, so
        // the store sink marks nothing and the subscript is the only cause.
        // A map may take ownership of a key in exactly this state.
        39, "ADR 0005: used as a map KEY only -> SUBSCRIPT_STORE alone",
        "package main\n"
        "import \"strings\"\n"
        "func f(m map[string]int, s string) {\n"
        "    k := strings.TrimSpace(s)\n"
        "    m[k] = 1\n"
        "}\n",
        "f", { { .name = "k", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_SUBSCRIPT_STORE, .check_reasons = true } }, 1
    },
    {
        // THE DAEMON'S ACTUAL SHAPE. It read SUBSCRIPT_STORE|CONTAINER_STORE
        // when the reasons were first named, and the extra bit came from the
        // READ half: `m[k]` on the right carried k's taint into the store sink,
        // naming a flow that does not exist -- the value stored is an int.
        //
        // The AST_INDEX_EXPR arm no longer propagates a read's index, so this
        // is now SUBSCRIPT_STORE alone and the shape is one a map may own. Row
        // 22 carries the soundness argument for that change.
        //
        // KEEP THIS ROW SEPARATE FROM ROW 39. Row 39's key appears once, in a
        // plain write. This one appears three times -- twice reading, once
        // writing -- and reaching the same verdict by a longer path is exactly
        // what a consumer needs proved.
        40, "ADR 0005: compound map update -> SUBSCRIPT_STORE alone",
        "package main\n"
        "import \"strings\"\n"
        "func f(m map[string]int, s string) {\n"
        "    k := strings.TrimSpace(s)\n"
        "    m[k] = m[k] + 1\n"
        "}\n",
        "f", { { .name = "k", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_SUBSCRIPT_STORE,
                 .check_reasons = true } }, 1
    },
    {
        41, "ADR 0005: argument a callee retains -> CALL_RETAIN alone",
        "package main\n"
        "var g *int\n"
        "func sink(p *int) {\n"
        "    g = p\n"
        "}\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    sink(x)\n"
        "}\n",
        "f", { { .name = "x", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_CALL_RETAIN, .check_reasons = true } }, 1
    },
    {
        // THE CEILING ON ARC, pinned. Calling a METHOD on a local marks that
        // local, whatever the method does, because the receiver is not a member
        // of call->args. Every local with a method set is unreleasable for this
        // reason -- sync.Mutex, bytes.Buffer, os.File, any user struct with
        // methods. The row exists so the ceiling is measurable rather than
        // folded into an unnamed `true`.
        42, "ADR 0005: method call on a local -> CALLEE_VALUE alone",
        "package main\n"
        "type T struct { v int }\n"
        "func (t *T) m() int {\n"
        "    return t.v\n"
        "}\n"
        "func f() {\n"
        "    p := &T{}\n"
        "    _ = p.m()\n"
        "}\n",
        "f", { { .name = "p", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_CALLEE_VALUE, .check_reasons = true } }, 1
    },
    {
        43, "ADR 0005: goroutine argument -> GO_ARG alone",
        "package main\n"
        "func sink(p *int) {\n"
        "}\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    go sink(x)\n"
        "}\n",
        "f", { { .name = "x", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_GO_ARG, .check_reasons = true } }, 1
    },
    {
        // DEFER_ARG rather than GO_ARG, which is the whole point of passing the
        // reason through handle_go_call. local_escape sets defer_is_like_go,
        // so the two share the helper and must still be distinguishable.
        // param_escape sets it false and never raises DEFER_ARG at all.
        44, "ADR 0005: deferred argument -> DEFER_ARG, not GO_ARG",
        "package main\n"
        "func sink(p *int) {\n"
        "}\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    defer sink(x)\n"
        "}\n",
        "f", { { .name = "x", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_DEFER_ARG, .check_reasons = true } }, 1
    },
    {
        45, "ADR 0005: sent on a channel -> CHAN_SEND alone",
        "package main\n"
        "func f(ch chan *int) {\n"
        "    x := new(int)\n"
        "    ch <- x\n"
        "}\n",
        "f", { { .name = "x", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_CHAN_SEND, .check_reasons = true } }, 1
    },
    {
        46, "ADR 0005: captured by a closure -> CLOSURE_CAPTURE alone",
        "package main\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    c := func() { _ = x }\n"
        "    _ = c\n"
        "}\n",
        "f", { { .name = "x", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_CLOSURE_CAPTURE, .check_reasons = true } }, 1
    },
    {
        // PRECISION, and the counterweight to all of the above: a local that
        // escapes for NO reason must read as the empty set, not as ALL. If the
        // lookup ever confused "found, zero" with "not found", every row above
        // would still pass and this one would fail.
        47, "ADR 0005: a local that does not escape -> the EMPTY set",
        "package main\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    _ = x\n"
        "}\n",
        "f", { { .name = "x", .expected_escapes = false,
                 .expected_reasons = ESCAPE_REASON_NONE, .check_reasons = true } }, 1
    },
    {
        // FAIL-CLOSED, asserted rather than assumed. An unknown local must
        // report the FULL set: the consumer's question is "only via X", and
        // ESCAPE_REASON_NONE on a miss would answer it wrongly for every X.
        48, "ADR 0005: an unknown local -> ESCAPE_REASON_ALL, not zero",
        "package main\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    _ = x\n"
        "}\n",
        "f", { { .name = "nosuchlocal", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_ALL, .check_reasons = true } }, 1
    },
    {
        // THE SPLIT OF CALL_RETAIN, and this row is the half that carries no
        // evidence. `unregistered` has no body in this unit, so nothing tells
        // the engine what it does with `x`, and the call arm marks the argument
        // pure-conservatively. That is SOUND and it is NOT the same fact as
        // row 41's: there, param_escape MEASURED that `sink` stores its
        // parameter in a global.
        //
        // ONE BIT USED TO CARRY BOTH, and the two want opposite actions -- the
        // ADR 0005 pattern exactly. "It genuinely outlives the call" is a fact
        // about the program; "we never looked" is a fact about the ANALYSIS,
        // and only the second can be fixed by making the analysis better.
        //
        // MEASURED across 599 corpus programs before this split landed: 578
        // locals carry CALL_OPAQUE against 86 carrying CALL_RETAIN, so 87% of
        // what the old single bit reported was the analysis declining to
        // answer. See docs/adr/0005-measurements/reason-census.md.
        49, "an unregistered callee -> CALL_OPAQUE, never CALL_RETAIN",
        "package main\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    unregistered(x)\n"
        "}\n",
        "f", { { .name = "x", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_CALL_OPAQUE, .check_reasons = true } }, 1
    },
    {
        // The THIRD cause, and it is neither of the other two. `g` HAS a
        // summary, so this is not the no-summary case, and the summary does not
        // reach the spread argument, so it is not evidence either. A distinct
        // fix -- model variadic parameters -- which is why it is not folded
        // into CALL_OPAQUE.
        //
        // SMALL, AND SAID SO: 12 locals across the corpus against CALL_OPAQUE's
        // 578. It gets its own bit because the alternative is filing a
        // no-evidence mark under an evidenced name, which is the conflation
        // this split exists to remove.
        50, "a spread argument -> CALL_VARIADIC, not CALL_RETAIN",
        "package main\n"
        "func g(xs ...*int) {\n"
        "}\n"
        "func f() {\n"
        "    xs := []*int{}\n"
        "    g(xs...)\n"
        "}\n",
        "f", { { .name = "xs", .expected_escapes = true,
                 .expected_reasons = ESCAPE_REASON_CALL_VARIADIC, .check_reasons = true } }, 1
    },
};


// A failing reason row must say WHICH reason differs. `reasons=48, expected 16`
// sends the reader to the header to decode two bit patterns by hand, and the
// interesting failure -- one extra bit -- is the one hardest to read that way.
//
// THE NAME TABLE MOVED INTO escape_core (escape_reason_names). It used to live
// here, and codegen's GOO_ARC_DEBUG line needed the same thing -- two copies of
// a table keyed on an enum drift the moment a reason is added, which is exactly
// the #274 class. One table, two readers.
// The name table is now shared with codegen, so it needs its own assertions
// rather than being exercised only as failure decoration. These run BEFORE the
// row loop: a broken decoder makes every row's failure output a lie, so it must
// be the first thing proved.
//
// WHY THE FULL-SET ROW MATTERS. ESCAPE_REASON_ALL is `(1 << 11) - 1`. If a
// reason is added to the header and NOT to the table, the header's ALL grows a
// bit the table cannot name and this row fails -- which is the only automatic
// notice that the two went out of step.
static int check_reason_names(void) {
    static const struct { EscapeReasons why; const char* want; } CASES[] = {
        { ESCAPE_REASON_NONE,            "NONE"            },
        { ESCAPE_REASON_SUBSCRIPT_STORE, "SUBSCRIPT_STORE" },
        { ESCAPE_REASON_CALLEE_VALUE,    "CALLEE_VALUE"    },
        { ESCAPE_REASON_RETURN | ESCAPE_REASON_CALLEE_VALUE, "RETURN|CALLEE_VALUE" },
        { ESCAPE_REASON_SUBSCRIPT_STORE | ESCAPE_REASON_CONTAINER_STORE,
          "CONTAINER_STORE|SUBSCRIPT_STORE" },
        { ESCAPE_REASON_CALL_OPAQUE,     "CALL_OPAQUE"     },
        { ESCAPE_REASON_CALL_RETAIN | ESCAPE_REASON_CALL_OPAQUE,
          "CALL_RETAIN|CALL_OPAQUE" },
        { ESCAPE_REASON_ALL,
          "UNCLASSIFIED|RETURN|GLOBAL_STORE|CONTAINER_STORE|SUBSCRIPT_STORE|"
          "CALL_RETAIN|CALLEE_VALUE|GO_ARG|DEFER_ARG|CHAN_SEND|CLOSURE_CAPTURE|"
          "CALL_OPAQUE|CALL_VARIADIC" },
    };
    int bad = 0;
    printf("=== escape_reason_names ===\n");
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char buf[ESCAPE_REASON_NAMES_MAX];
        const char* got = escape_reason_names(CASES[i].why, buf, sizeof buf);
        if (strcmp(got, CASES[i].want) != 0) {
            printf("  FAIL: 0x%x -> \"%s\", expected \"%s\"\n",
                   (unsigned)CASES[i].why, got, CASES[i].want);
            bad++;
        }
    }
    // A short buffer must TRUNCATE rather than overrun, because codegen passes
    // a stack buffer and a diagnostic must never be the thing that corrupts a
    // frame. Asked of the widest input, which is the one that would overrun.
    {
        char tiny[8];
        const char* got = escape_reason_names(ESCAPE_REASON_ALL, tiny, sizeof tiny);
        if (got != tiny || strlen(got) >= sizeof tiny) {
            printf("  FAIL: short buffer not truncated (len=%zu, cap=%zu)\n",
                   strlen(got), sizeof tiny);
            bad++;
        }
    }
    printf("  escape_reason_names: %s\n", bad ? "FAIL" : "PASS");
    return bad;
}

int main(void) {
    size_t nrows = sizeof(rows) / sizeof(rows[0]);
    goo_check_expect((int)nrows + 1);   // the table, plus row 0 below

    // Before any row: prove the decoder. Every FAIL line below is printed
    // through it, so a broken decoder turns each failure into a wrong claim.
    goo_check_row(0, "the escape-reason name decoder, before anything reads it");
    goo_check(check_reason_names() == 0, "escape_reason_names decodes every case");

    for (size_t r = 0; r < nrows; r++) {
        TestRow* row = &rows[r];
        goo_check_row(row->row, row->description);

        char ctxbuf[320];
        int parse_rc = parse_input(row->src, "test.goo");
        snprintf(ctxbuf, sizeof(ctxbuf), "row %d: the fixture parses (rc=%d)", row->row, parse_rc);
        goo_check(parse_rc == 0 && ast_root != NULL, ctxbuf);
        if (parse_rc != 0 || !ast_root) {
            if (ast_root) { ast_node_free(ast_root); ast_root = NULL; }
            continue;
        }

        // Run the real type checker so FuncLitNode.captured_names is
        // populated the way the compiler pipeline populates it (row 8's
        // closure-capture case needs it). The return code is ignored on
        // purpose: row 12 calls an unregistered external and is EXPECTED to
        // fail type-checking, which must not stop the analysis.
        TypeChecker* checker = type_checker_new();
        if (checker) {
            type_check_program(checker, ast_root);
        }

        ParamEscapeResult* summaries = param_escape_analyze(ast_root);
        LocalEscapeResult* result = local_escape_analyze(ast_root, summaries);
        goo_check(result != NULL, "local_escape_analyze returned a result");
        if (!result) continue;

        for (int i = 0; i < row->expect_count; i++) {
            bool got = local_escape_local_escapes(result, row->fn, row->expect[i].name);
            snprintf(ctxbuf, sizeof(ctxbuf), "row %d: local '%s' escapes=%d, expected %d",
                     row->row, row->expect[i].name, (int)got,
                     (int)row->expect[i].expected_escapes);
            goo_check(got == row->expect[i].expected_escapes, ctxbuf);

            if (!row->expect[i].check_reasons) continue;
            EscapeReasons why =
                local_escape_local_reasons(result, row->fn, row->expect[i].name);
            // Both reason sets go through the decoder proved in row 0, so the
            // label states what was observed and not merely what was wanted.
            char gotbuf[ESCAPE_REASON_NAMES_MAX], wantbuf[ESCAPE_REASON_NAMES_MAX];
            snprintf(ctxbuf, sizeof(ctxbuf), "row %d: local '%s' reasons=%s, expected %s",
                     row->row, row->expect[i].name,
                     escape_reason_names(why, gotbuf, sizeof gotbuf),
                     escape_reason_names(row->expect[i].expected_reasons, wantbuf, sizeof wantbuf));
            goo_check(why == row->expect[i].expected_reasons, ctxbuf);
        }

        local_escape_result_free(result);
        param_escape_result_free(summaries);
        if (checker) type_checker_free(checker);
        if (ast_root) { ast_node_free(ast_root); ast_root = NULL; }
    }

    return goo_check_done("local-escape-test");
}
