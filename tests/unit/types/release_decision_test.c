// T4: the release decision — table-driven unit test.
//
// This is the FIRST decision in the ARC leg that a consumer acts on by FREEING
// memory, so the soundness invariant runs the OPPOSITE way from its three escape
// siblings: there, `escapes = true` is the safe answer; here, `release = false`
// is. A wrong `true` frees live memory.
//
// EVERY ROW ASSERTS THE VERDICT, NOT JUST THE BOOLEAN. A row that refuses a
// local for the wrong reason would still pass a boolean check, and this arc has
// now been bitten twice by a check that passed for an unrelated cause (a
// function extractor that matched forward declarations, and a local_escape run
// whose imports had not resolved). Asserting the cause is what stops it.
//
// IMPORT-FREE ON PURPOSE. `.handoff.md` records a local_escape table that looked
// confident and was conservative for an unrelated reason: the imports had not
// resolved, and the tell was `i`, a plain int loop counter, reading as escaping.
// The shim half of condition 2 therefore cannot be covered here — it needs a
// resolved `strings` import — and is covered by an integration probe instead.

#include "parser.h"
#include "ast.h"
#include "types.h"
#include "release_decision.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_EXPECT_LOCALS 4

typedef struct {
    const char*    local;
    ReleaseVerdict verdict;
} LocalExpectation;

typedef struct {
    int              row;
    const char*      description;
    const char*      src;
    const char*      fn;
    LocalExpectation expect[MAX_EXPECT_LOCALS];
    int              expect_count;
} TestRow;

// A callee that returns a VIEW of its argument, so its ParamEscapeSummary
// carries return_escapes = true. This is the TrimPrefix shape that
// include/local_escape.h names as the hole, written without an import.
#define BORROW_HELPER \
    "func borrowView(s string) string {\n" \
    "    return s[1:]\n" \
    "}\n"

// A callee that returns a FRESH allocation: return_escapes = false.
#define OWNED_HELPER \
    "func makeOwned() *int {\n" \
    "    return new(int)\n" \
    "}\n"

static TestRow rows[] = {
    // ---------------- RELEASE: all four conditions hold ----------------
    {
        1, "new(int) at function scope, dies here -> RELEASE",
        "package main\n"
        "func f() {\n"
        "    a := new(int)\n"
        "    _ = a\n"
        "}\n",
        "f", { { "a", RELEASE_OK } }, 1
    },
    {
        2, "call to a Goo function with return_escapes false -> RELEASE",
        "package main\n"
        OWNED_HELPER
        "func f() {\n"
        "    a := makeOwned()\n"
        "    _ = a\n"
        "}\n",
        "f", { { "a", RELEASE_OK } }, 1
    },
    {
        3, "a composite literal is a fresh allocation -> RELEASE",
        "package main\n"
        "type T struct { x int }\n"
        "func f() {\n"
        "    p := &T{x: 1}\n"
        "    _ = p\n"
        "}\n",
        "f", { { "p", RELEASE_OK } }, 1
    },

    // ---------------- CONDITION 1: escapes ----------------
    {
        4, "returned -> refuse, ESCAPES",
        "package main\n"
        "func f() *int {\n"
        "    a := new(int)\n"
        "    return a\n"
        "}\n",
        "f", { { "a", RELEASE_NO_ESCAPES } }, 1
    },
    {
        5, "stored to a global -> refuse, ESCAPES",
        "package main\n"
        "var g *int\n"
        "func f() {\n"
        "    a := new(int)\n"
        "    g = a\n"
        "}\n",
        "f", { { "a", RELEASE_NO_ESCAPES } }, 1
    },

    // ---------------- CONDITION 2: not owned ----------------
    //
    // THE ROW THAT MATTERS MOST. `b` does not outlive f, so condition 1 passes
    // and local_escape alone would say "release". borrowView returns `s[1:]`,
    // a view into the CALLER's buffer, so a release frees the caller's string.
    {
        6, "bound to a callee that returns a VIEW of its arg -> refuse, NOT_OWNED",
        "package main\n"
        BORROW_HELPER
        "func f(s string) {\n"
        "    b := borrowView(s)\n"
        "    _ = b\n"
        "}\n",
        "f", { { "b", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        7, "bound to a slice expression directly -> refuse, NOT_OWNED",
        "package main\n"
        "func f(s string) {\n"
        "    c := s[1:]\n"
        "    _ = c\n"
        "}\n",
        "f", { { "c", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        8, "an ALIAS of another local -> refuse, NOT_OWNED (one owner only)",
        "package main\n"
        "func f() {\n"
        "    a := new(int)\n"
        "    d := a\n"
        "    _ = d\n"
        "}\n",
        // `a` still releases: it is the owner. `d` must not, or the buffer is
        // freed twice.
        "f", { { "a", RELEASE_OK }, { "d", RELEASE_NO_NOT_OWNED } }, 2
    },
    {
        9, "bound to an INDEX read -> refuse, NOT_OWNED (aliases the container)",
        "package main\n"
        "func f(xs []*int) {\n"
        "    e := xs[0]\n"
        "    _ = e\n"
        "}\n",
        "f", { { "e", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        10, "bound to a SELECTOR -> refuse, NOT_OWNED (a field of something else)",
        "package main\n"
        "type T struct { p *int }\n"
        "func f(t *T) {\n"
        "    q := t.p\n"
        "    _ = q\n"
        "}\n",
        "f", { { "q", RELEASE_NO_NOT_OWNED } }, 1
    },

    // ---------------- CONDITION 3: arena-routed ----------------
    //
    // An arena pointer has NO object header, so goo_release would compute
    // `ptr - GOO_OBJ_HEADER_SIZE` on an interior pointer and free() it.
    // local_escape's boundary is the FUNCTION, so `z` reads as non-escaping and
    // nothing else refuses it.
    {
        11, "declared inside `arena { }` -> refuse, ARENA",
        "package main\n"
        "func f() {\n"
        "    arena {\n"
        "        z := new(int)\n"
        "        _ = z\n"
        "    }\n"
        "}\n",
        "f", { { "z", RELEASE_NO_ARENA } }, 1
    },

    // ---------------- CONDITION 4: loop scope ----------------
    //
    // THE ROW THAT RETARGETED T4. `inner` is rebound on every iteration, so a release
    // at function exit frees one of N. `outer` is bound once and does release,
    // which is the contrast that proves the rule is about SCOPE and not about
    // the presence of a loop in the function.
    {
        12, "declared inside a loop -> refuse, LOOP_SCOPE; outer still releases",
        "package main\n"
        "func f(n int) {\n"
        "    outer := new(int)\n"
        "    for i := 0; i < n; i++ {\n"
        "        inner := new(int)\n"
        "        _ = inner\n"
        "    }\n"
        "    _ = outer\n"
        "}\n",
        "f", { { "outer", RELEASE_OK }, { "inner", RELEASE_NO_LOOP_SCOPE } }, 2
    },
    {
        13, "the daemon's shape: err bound inside the loop -> refuse, LOOP_SCOPE",
        "package main\n"
        OWNED_HELPER
        "func f(n int) {\n"
        "    for i := 0; i < n; i++ {\n"
        "        err := makeOwned()\n"
        "        _ = err\n"
        "    }\n"
        "}\n",
        "f", { { "err", RELEASE_NO_LOOP_SCOPE } }, 1
    },

    {
        // CONDITION 4, the re-assignment half, and it is a SOUNDNESS row rather
        // than a precision one. The DECLARATION site is a clean allocation, so
        // condition 2 reads `a` as owned. The later `a = t.p` leaves it holding a
        // field of someone else's struct, and a release at exit would free that.
        // Only counting bindings catches this.
        //
        // Added because scripts/release_decision_teeth.sh reported this condition
        // UNGUARDED: deleting it from decide() left all 15 original rows green.
        16, "declared owned, then RE-ASSIGNED to a borrowed value -> refuse, REBOUND",
        "package main\n"
        "type T struct { p *int }\n"
        "func f(t *T) {\n"
        "    a := new(int)\n"
        "    a = t.p\n"
        "    _ = a\n"
        "}\n",
        "f", { { "a", RELEASE_NO_REBOUND } }, 1
    },

    // ---------------- conservative defaults ----------------
    {
        14, "a scalar literal owns nothing to release -> refuse, NOT_OWNED",
        "package main\n"
        "func f() {\n"
        "    k := 1\n"
        "    _ = k\n"
        "}\n",
        "f", { { "k", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        15, "an UNRESOLVED callee -> refuse (conservative), NOT_OWNED",
        "package main\n"
        "func f() {\n"
        "    u := unknownExternal()\n"
        "    _ = u\n"
        "}\n",
        "f", { { "u", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        // THE CLIFF, LIFTED. This row used to assert RELEASE_NO_UNKNOWN: the walk
        // did not read `switch`, `type switch`, `select` or `if let`, so ANY
        // function containing one was refused entirely and `a` -- an obvious
        // candidate at function scope -- went with it.
        //
        // The four arms now read those statements, so this row asserts the
        // OPPOSITE verdict, and that flip IS the feature. The row is kept rather
        // than deleted because it is the record of what the limit was and of the
        // exact shape that measured it.
        //
        // RELEASE_NO_UNKNOWN IS STILL REACHABLE, and row 34 is what keeps it so.
        // Without a row holding that verdict, release_decision_teeth.sh would
        // report the unreadable condition as unguarded -- which is precisely why
        // this row was added in the first place.
        17, "a function containing a SWITCH is now READ -> RELEASE",
        "package main\n"
        "func f(n int) {\n"
        "    a := new(int)\n"
        "    switch n {\n"
        "    case 1:\n"
        "        _ = a\n"
        "    }\n"
        "}\n",
        "f", { { "a", RELEASE_OK } }, 1
    },

    // ---------------- SELF-APPEND, and the double-free it must not cause -----
    //
    // `L = append(L, x)` does not REBIND L to a different object. It GROWS L's
    // object: append writes into the buffer or reallocs it, and goo_realloc frees
    // the old base itself. One local, one live buffer, ownership never moves.
    //
    // Two changes made this reachable, and NEITHER alone did anything. Measured:
    // before them `xs = append(xs, n)` read RELEASE_NO_ESCAPES; after giving
    // `append` a non-retaining slice argument it read RELEASE_NO_REBOUND; only
    // with the self-append rule as well does it read RELEASE_OK. Building either
    // half alone would have been worth 0%.
    {
        18, "L = append(L, x) is a self-append, not a rebind -> RELEASE",
        "package main\n"
        "var sink int\n"
        "func f(n int) {\n"
        "    xs := []int{}\n"
        "    xs = append(xs, n)\n"
        "    sink = sink + len(xs)\n"
        "}\n",
        "f", { { "xs", RELEASE_OK } }, 1
    },
    {
        // THE DOUBLE-FREE GUARD, and the reason the self-append rule is written
        // as `L = append(L, ...)` and not as "any append".
        //
        // `t := append(s, x)` can leave t.data == s.data, because append reuses
        // the buffer when capacity suffices. Two owners of one buffer is a double
        // free. It is refused because call_result_is_owned finds no summary for
        // `append` and answers false, so this row pins condition 2 carrying the
        // weight rather than the self-append rule being widened.
        19, "t := append(s, x) may SHARE s's buffer -> refuse, NOT_OWNED",
        "package main\n"
        "var sink int\n"
        "func f(s []int, n int) {\n"
        "    t := append(s, n)\n"
        "    sink = sink + len(t)\n"
        "}\n",
        "f", { { "t", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        // A CROSS-append is a real rebind: `a` ends up holding b's buffer, so it
        // has two candidate owners and must stay refused. The self-append rule
        // matches arg 0 against the target BY NAME precisely to exclude this.
        20, "a = append(b, x) is a rebind of a to b's buffer -> refuse, REBOUND",
        "package main\n"
        "var sink int\n"
        "func f(n int) {\n"
        "    a := []int{1}\n"
        "    b := []int{2}\n"
        "    a = append(b, n)\n"
        "    sink = sink + len(a) + len(b)\n"
        "}\n",
        // `b` is still its own owner and still releases; only `a` is refused.
        "f", { { "a", RELEASE_NO_REBOUND }, { "b", RELEASE_OK } }, 2
    },
    {
        // A COMPOUND operator is never an append, so it stays a rebind. Without
        // the `plain_assign` guard, `xs += ...` would take the self-append path
        // by accident.
        21, "a compound assignment is not a self-append -> refuse, REBOUND",
        "package main\n"
        "var sink int\n"
        "func f(n int) {\n"
        "    xs := []int{1}\n"
        "    xs = []int{2}\n"
        "    sink = sink + len(xs) + n\n"
        "}\n",
        "f", { { "xs", RELEASE_NO_REBOUND } }, 1
    },

    // ---------------- STRING CONCATENATION, condition 2's binary arm ----------
    //
    // `s := a + b` on strings lowers to goo_string_concat (src/runtime/runtime.c
    // :523), which ALWAYS returns fresh goo_alloc memory, or {NULL, 0} when both
    // operands are empty. goo_release is a no-op on NULL. So the local is the one
    // owner and the binding site is an allocation, exactly as `new(T)` is.
    //
    // WITHOUT THIS ARM THE STRING RELEASE IS WORTH NOTHING. AST_BINARY_EXPR fell
    // to the conservative default, so the only owned string bindings left were a
    // Goo call and a non-retaining shim. Concatenation is how ordinary Goo code
    // builds a string, and `.handoff.md` records five plans that shipped-looking
    // work would have been worth 0% without a measurement first.
    {
        22, "s := a + b on strings is a fresh allocation -> RELEASE",
        "package main\n"
        "var sink int\n"
        "func f() {\n"
        "    a := \"abc\"\n"
        "    b := \"def\"\n"
        "    s := a + b\n"
        "    sink = sink + len(s)\n"
        "}\n",
        // `a` and `b` stay refused: a bare literal is immortal (GOO_RC_IMMORTAL),
        // owns no heap object, and falls to the conservative default. Asserting
        // them here proves the arm widened the CONCATENATION and nothing else.
        "f", { { "s", RELEASE_OK },
               { "a", RELEASE_NO_NOT_OWNED },
               { "b", RELEASE_NO_NOT_OWNED } }, 3
    },
    {
        // OWNERSHIP DOES NOT DEPEND ON THE OPERANDS, and that is the whole
        // difference between concatenation and `borrowView` in row 6. borrowView
        // returns `s[1:]`, a VIEW into the caller's buffer. Concatenation COPIES
        // both operands into new memory, so a parameter operand is as safe as a
        // local one and the result aliases neither.
        23, "s := p + \"x\" with a PARAMETER operand still copies -> RELEASE",
        "package main\n"
        "var sink int\n"
        "func f(p string) {\n"
        "    s := p + \"x\"\n"
        "    sink = sink + len(s)\n"
        "}\n",
        "f", { { "s", RELEASE_OK } }, 1
    },
    {
        // CONDITION 4 STILL BITES, and this row is why the concat arm does not
        // need a kill rule of its own. `s += "e"` is a compound assignment, so
        // note_assignment counts a second binding and decide() refuses BEFORE it
        // ever reaches condition 2.
        //
        // Releasing the last value would in fact be correct here, because each
        // concatenation returns new memory. The refusal costs reclamation and is
        // never unsafe, and relaxing condition 4 is a separate change with its
        // own rows. Recorded in the .handoff.md ledger.
        24, "s := a + b then s += \"e\" is a rebind -> refuse, REBOUND",
        "package main\n"
        "var sink int\n"
        "func f() {\n"
        "    a := \"abc\"\n"
        "    s := a + \"d\"\n"
        "    s += \"e\"\n"
        "    sink = sink + len(s)\n"
        "}\n",
        "f", { { "s", RELEASE_NO_REBOUND } }, 1
    },
    {
        // AN INTEGER `+` REACHES THE SAME ARM, and this verdict is DELIBERATE.
        //
        // release_decision.c is a pure AST module with no type information, so it
        // cannot tell a string `+` from an integer one. Approving both costs
        // nothing, because the decision is only HALF the guard: codegen_arc_note_
        // local refuses any slot that is not a pointer, a 3-field slice or a
        // 2-field string, and `n`'s slot is a bare i64.
        //
        // Do not "fix" this row by narrowing the arm. The two-layer split is what
        // lets each layer stay simple, and arc-release-probe is what proves the
        // second layer actually refuses.
        // `_ = n` and NOT `sink = sink + n`. Measured: the global store makes `n`
        // ESCAPE, so the row read RELEASE_NO_ESCAPES and would have gone green on
        // condition 1 the moment condition 2 widened -- passing for a cause it was
        // not written to measure. Row 22 avoids this because `len(s)` yields a
        // fresh int and propagates none of s's taint.
        25, "an INTEGER + reaches the same arm -> RELEASE_OK, refused by codegen",
        "package main\n"
        "func f(x int, y int) {\n"
        "    n := x + y\n"
        "    _ = n\n"
        "}\n",
        "f", { { "n", RELEASE_OK } }, 1
    },

    // ---------------- CONDITION 5: declared inside a conditional block -------
    //
    // A SOUNDNESS ARC, not a precision one, and it was a LIVE BUG on main before
    // these rows. Measured on the shape row 26 uses, with `f(false)`:
    //
    //   Use of uninitialised value of size 8
    //      at goo_release (runtime.c:203)      <- the immortal-count read
    //      by f
    //    Uninitialised value was created by a stack allocation at f
    //   ... and again at runtime.c:215, which is the __atomic_fetch_sub -- a
    //   WRITE through the garbage pointer, to an arbitrary address.
    //
    // WHY. codegen_alloc_local_promoted (src/codegen/function_codegen.c) sends an
    // ordinary local to codegen_create_entry_alloca, so the SLOT is hoisted to the
    // entry block. The initialising store stays at the declaration site. A local
    // declared inside a branch therefore has a slot on every path and a VALUE only
    // on the taken one, and an LLVM alloca is undef rather than zero.
    // goo_obj_headerless screens only NULL and goo_zerobase, so nothing downstream
    // catches it.
    //
    // The direct run exited 0 -- the garbage happened to be benign that time. Only
    // valgrind saw it. That is why this is a refusal and not a "known limitation".
    //
    // THE FIX THAT WOULD RECOVER THE PRECISION is an entry-block zero store, the
    // shape defer_entry_store_zero already uses for a defer placed in a branch
    // that is never taken (src/codegen/statement_codegen.c). Then an unexecuted
    // declaration leaves NULL and goo_release no-ops. That is the NEXT increment,
    // deliberately not this one.
    {
        26, "declared inside an IF branch -> refuse, COND_SCOPE",
        "package main\n"
        "var sink int\n"
        "func f(c bool) {\n"
        "    if c {\n"
        "        a := new(int)\n"
        "        sink = sink + 1\n"
        "        _ = a\n"
        "    }\n"
        "}\n",
        "f", { { "a", RELEASE_NO_COND_SCOPE } }, 1
    },
    {
        // The ELSE branch is the same hazard. Written separately because the arm
        // raises the depth around then_stmt and else_stmt independently, so a fix
        // that covered only one would leave this green.
        27, "declared inside an ELSE branch -> refuse, COND_SCOPE",
        "package main\n"
        "var sink int\n"
        "func f(c bool) {\n"
        "    if c {\n"
        "        sink = sink + 1\n"
        "    } else {\n"
        "        b := new(int)\n"
        "        _ = b\n"
        "    }\n"
        "}\n",
        "f", { { "b", RELEASE_NO_COND_SCOPE } }, 1
    },
    {
        // THE CONTRAST, and it is what keeps condition 5 about the DECLARATION
        // SITE rather than about the presence of an `if` in the function. Mirrors
        // row 12's outer/inner pair for loops. Without this row, a fix that
        // refused every local in any function containing a branch would pass.
        28, "function scope local, with an `if` elsewhere -> RELEASE",
        "package main\n"
        "var sink int\n"
        "func f(c bool) {\n"
        "    outer := new(int)\n"
        "    if c {\n"
        "        sink = sink + 1\n"
        "    }\n"
        "    _ = outer\n"
        "}\n",
        "f", { { "outer", RELEASE_OK } }, 1
    },

    // ---------------- THE SWITCH / SELECT PRECISION CLIFF, now read ----------
    //
    // The walk used to meet `switch`, `type switch`, `select` and `if let` at its
    // `default:` arm and mark the WHOLE function unreadable, so every local in it
    // refused with RELEASE_NO_UNKNOWN. Row 17 pinned that. Ordinary Goo code
    // contains a switch frequently, so it was the widest precision limit left.
    //
    // Reading them is not merely recursion. Each one BINDS, and a missed binding
    // is a use-after-free rather than a lost optimisation. Rows 32 and 33 are the
    // two that carry that weight.
    {
        29, "a local declared inside a SWITCH case body -> refuse, COND_SCOPE",
        "package main\n"
        "var sink int\n"
        "func f(n int) {\n"
        "    switch n {\n"
        "    case 1:\n"
        "        c := new(int)\n"
        "        sink = sink + 1\n"
        "        _ = c\n"
        "    }\n"
        "}\n",
        "f", { { "c", RELEASE_NO_COND_SCOPE } }, 1
    },
    {
        // A TYPE SWITCH BIND IS A VIEW of the interface's data pointer, so no
        // local owns it. Recorded with a NULL binding value, which condition 2
        // then refuses. Not a limit to lift -- releasing it would free through
        // the interface the caller still holds.
        30, "v := x.(type) binds a VIEW of the operand -> refuse, NOT_OWNED",
        "package main\n"
        "var sink int\n"
        "type Shape interface { Area() int }\n"
        "func f(x Shape) {\n"
        "    switch v := x.(type) {\n"
        "    case Shape:\n"
        "        sink = sink + 1\n"
        "        _ = v\n"
        "    }\n"
        "}\n",
        "f", { { "v", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        // AN `if let` BIND IS THE UNWRAPPED NULLABLE, which aliases whatever the
        // nullable held. Same treatment, same cause.
        31, "if let v = opt binds the unwrapped value -> refuse, NOT_OWNED",
        "package main\n"
        "var sink int\n"
        "func f(p ?*int) {\n"
        "    if let v = p {\n"
        "        sink = sink + 1\n"
        "        _ = v\n"
        "    }\n"
        "}\n",
        "f", { { "v", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        // A TRIPWIRE ROW, and the verdict here is NOT the one the select arm
        // computes. Read the whole comment before changing it.
        //
        // `case a = <-ch:` is SelectCaseNode.is_declare == 0 -- an assignment into
        // an ALREADY-DECLARED outer local. The declaration site is a clean
        // allocation, so condition 2 reads `a` as owned, and only counting the
        // select's rebind catches that `a` ends up holding what the channel
        // delivered. The AST_SELECT_STMT arm does count it.
        //
        // BUT CONDITION 1 REFUSES FIRST, so REBOUND is unobservable here.
        // MEASURED, three ways:
        //   - a function with a `chan *int` parameter and NO select releases `a`
        //   - adding a select refuses it, even with a non-pointer channel and
        //     even when the select does not mention `a` at all
        //   - bypassing condition 1 in decide() makes this row read REBOUND
        //
        // THE CAUSE IS UPSTREAM, in escape_core.c. Its AST_SELECT_STMT arm calls
        // escape_walk_stmt on `sc->comm`, but the grammar builds every select case
        // from `CASE ... expression COLON case_body`, so comm is an EXPRESSION.
        // It lands on that walk's `default:`, which calls escape_mark_all -- so
        // EVERY local in ANY function containing a select reads as escaping.
        // A precision bug, not a soundness one, which is why it went unseen:
        // escape_core's safe answer is `true`.
        //
        // NOT FIXED HERE ON PURPOSE. That arm is shared by param_escape,
        // block_escape and local_escape, so touching it means re-running all
        // three arm matrices. Recorded in the .handoff.md ledger.
        //
        // WHEN THAT IS FIXED, THIS ROW MUST BECOME RELEASE_NO_REBOUND. It failing
        // is the signal that the select precision arrived -- not a regression.
        32, "case a = <-ch: is masked by condition 1 -> ESCAPES (tripwire)",
        "package main\n"
        "var sink int\n"
        "func f(ch chan *int) {\n"
        "    a := new(int)\n"
        "    select {\n"
        "    case a = <-ch:\n"
        "        sink = sink + 1\n"
        "    }\n"
        "    _ = a\n"
        "}\n",
        "f", { { "a", RELEASE_NO_ESCAPES } }, 1
    },
    {
        // SOUNDNESS ROW, and the one that fails if the case BODIES are not walked
        // at all. Row 16's shape, moved inside a case: the declaration site is a
        // clean allocation and the later `a = t.p` leaves `a` holding a field of
        // someone else's struct. Refusing the whole function used to cover this
        // by accident; now the walk has to actually see it.
        33, "an assignment INSIDE a case body is still a rebind -> refuse, REBOUND",
        "package main\n"
        "var sink int\n"
        "type T struct { p *int }\n"
        "func f(n int, t *T) {\n"
        "    a := new(int)\n"
        "    switch n {\n"
        "    case 1:\n"
        "        a = t.p\n"
        "    }\n"
        "    sink = sink + 1\n"
        "    _ = a\n"
        "}\n",
        "f", { { "a", RELEASE_NO_REBOUND } }, 1
    },
    {
        // RELEASE_NO_UNKNOWN MUST STAY REACHABLE, or release_decision_teeth.sh
        // reports the unreadable condition as unguarded once `switch` becomes
        // readable. AST_LABEL_STMT and AST_GOTO_STMT are still unread, and a
        // label WRAPS a statement -- `L: a = t.p` assigns -- so refusing on one
        // is conservative for a real cause and not an arbitrary placeholder.
        34, "a function containing a LABEL is still unreadable -> UNKNOWN",
        "package main\n"
        "var sink int\n"
        "func f(n int) {\n"
        "    a := new(int)\n"
        "    if n > 0 {\n"
        "        goto done\n"
        "    }\n"
        "    sink = sink + 1\n"
        "done:\n"
        "    _ = a\n"
        "}\n",
        "f", { { "a", RELEASE_NO_UNKNOWN } }, 1
    },
};

static int failures = 0;
static int checks = 0;

int main(void) {
    printf("Running T4 release-decision tests...\n");
    size_t nrows = sizeof(rows) / sizeof(rows[0]);

    for (size_t i = 0; i < nrows; i++) {
        TestRow* row = &rows[i];
        printf("\n=== Row %d: %s ===\n", row->row, row->description);

        if (parse_input(row->src, "row.goo") != 0 || !ast_root) {
            printf("  FAIL: parse failed\n");
            failures++;
            continue;
        }
        TypeChecker* checker = type_checker_new();
        if (checker) type_check_program(checker, ast_root);  // rc ignored, as the siblings do

        ReleasePlan* plan = release_plan_analyze(ast_root);
        if (!plan) {
            printf("  FAIL: release_plan_analyze returned NULL\n");
            failures++;
            if (checker) type_checker_free(checker);
            ast_node_free(ast_root);
            ast_root = NULL;
            continue;
        }

        int row_failed = 0;
        for (int j = 0; j < row->expect_count; j++) {
            ReleaseVerdict got = release_plan_verdict(plan, row->fn, row->expect[j].local);
            ReleaseVerdict want = row->expect[j].verdict;
            checks++;
            if (got != want) {
                printf("  FAIL: local '%s' verdict=%s, expected %s\n",
                       row->expect[j].local,
                       release_verdict_name(got), release_verdict_name(want));
                failures++;
                row_failed = 1;
            }
            // The boolean and the verdict must never disagree, or a caller and a
            // test could read different answers from the same plan.
            bool should = release_plan_should_release(plan, row->fn, row->expect[j].local);
            checks++;
            if (should != (want == RELEASE_OK)) {
                printf("  FAIL: local '%s' should_release=%d disagrees with verdict %s\n",
                       row->expect[j].local, (int)should, release_verdict_name(want));
                failures++;
                row_failed = 1;
            }
        }

        // A miss must be conservative, checked once per row rather than assumed.
        checks++;
        if (release_plan_should_release(plan, "__no_such_function__", "x")) {
            printf("  FAIL: unknown function returned should_release=true\n");
            failures++;
            row_failed = 1;
        }

        printf("  Row %d: %s\n", row->row, row_failed ? "FAIL" : "PASS");

        release_plan_free(plan);
        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        ast_root = NULL;
    }

    printf("\n=================================================\n");
    printf("release_decision_test summary: %d assertions passed, %d failed\n",
           checks - failures, failures);
    return failures ? 1 : 0;
}
