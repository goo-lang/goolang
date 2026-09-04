#!/usr/bin/env python3
"""Structural check for a goo program dump. Exit 1 with one line naming the
first problem. Kept independent of the kind list on purpose: it checks the
SHAPE every node and type must have, and that every id resolves, so a new
kind needs no change here and a malformed one is still refused."""
import json, re, sys

EXPR_KINDS = {
    "IDENTIFIER", "LITERAL", "BINARY_EXPR", "UNARY_EXPR", "POSTFIX_EXPR",
    "CALL_EXPR", "INDEX_EXPR", "SELECTOR_EXPR", "SLICE_LIT", "MAP_LIT",
    "SLICE_INDEX_EXPR", "STRUCT_LITERAL", "ARRAY_LITERAL", "FUNC_LIT",
    "SLICE_CONVERSION", "TYPE_ASSERT", "TRY_EXPR", "CATCH_EXPR", "ADDR_OF",
    "PTR_DEREF", "MATCH_EXPR",
}

def fail(msg):
    print(msg); sys.exit(1)

# Phase 0 Task 5 finding: these AST shapes are recognized structurally, or
# by literal name, inside the checker itself -- never routed through the
# ordinary type_check_identifier/type_check_literal/type_check_expression
# pass that stamps node_type -- so a node in one of them legitimately
# carries no "type" in the typed-stage dump, and any EXPRESSION NESTED
# inside it (a `2+3` array length, say) never gets stamped either, since
# the whole subtree is consumed by constant-folding or a type-name lookup
# rather than the ordinary checker walk. Closing the gap (giving each of
# these a real type, or dropping them from EXPR_KINDS) is Phase 1 work, not
# this task's; each comment below names one fixture that reproduces it
# (there are more).
#
# _UNCHECKED_SUBTREES(node) names the child KEYS of `node` whose entire
# subtree the walk below must stop requiring a type for. A single node
# lacking a type is handled separately by known_unstamped, below.
def _unchecked_subtrees(node):
    k = node.get("kind")
    if k == "ARRAY_TYPE":
        # examples/array_literal_probe.goo, examples/const_array_probe.goo:
        # ArrayTypeNode.length (`10`, or `2+3`) is a compile-time constant
        # folded directly (goo_fold_const_int_ctx) -- the whole expression,
        # not just its top node.
        return {"length"}
    if k == "KEYED_ELEMENT":
        # examples/composite_widen_probe.goo: a keyed array literal's index
        # (`{2: v}`) is resolved via goo_fold_const_int, never
        # type_check_expression.
        return {"key"}
    if k == "CALL_EXPR":
        # type_check_call_expr (src/types/expression_checker.c) recognizes
        # a builtin name (append/len/cap/make/...), make_chan, a named-type
        # conversion (IntSlice(x)), or an explicit generic instantiation
        # (Id[T](x), callee kind INDEX_EXPR) by NAME/lookup/shape and
        # handles the call itself, never calling type_check_expression on
        # call->function. Confirmed by contrast: a plain user-function
        # callee (`work()` in examples/arc_release_probe.goo) DOES get a
        # type id through the ordinary path.
        # examples/append_probe.goo (append), examples/chan_probe.goo
        # (make_chan), examples/named_type_conv_probe.goo (IntSlice),
        # examples/generic_explicit_inst_probe.goo (Id[T]).
        return {"function"}
    return set()

# make_chan/new/make's FIRST argument is a TYPE, resolved via type_from_ast
# rather than type_check_expression -- examples/chan_probe.goo (make_chan),
# examples/new_probe.goo (new). A later argument (make_chan's capacity,
# make's len/cap/size-hint) IS an ordinary checked value expression, so only
# args[0] is exempt, not the whole `args` list.
_TYPE_ARG0_BUILTINS = {"make_chan", "new", "make"}

def known_unstamped(path, kind, node, parent):
    if kind == "IDENTIFIER" and node.get("name") == "_":
        # examples/type_assert_valptr_probe.goo: `_ = vAsValue` -- the
        # blank identifier is a discard target, never a resolved Variable.
        return True
    if kind == "IDENTIFIER" and path.endswith(".bind_name"):
        # examples/type_switch_probe.goo: TypeSwitchNode.bind_name is the
        # switch's own declaration-site name holder (the `v` in
        # `switch v := x.(type)`), never independently type-checked as an
        # expression -- unlike a later reference to `v` inside a case body,
        # which resolves normally and does carry a type.
        return True
    if kind == "IDENTIFIER" and re.search(r"\.methods\[\d+\]$", path):
        # examples/interface_embedding.goo: an embedded interface named in
        # another interface's method list (`interface { Reader; ... }`) is
        # a type reference, not a checked expression.
        return True
    if kind == "IDENTIFIER" and re.search(r"\.fields\[\d+\]$", path):
        # examples/match_probe.goo: a PATTERN_DESTRUCTURE field is a
        # binding name (`case Num(v):` binds v), never a resolved value --
        # unlike a later reference to v inside the case body.
        return True
    if kind == "LITERAL" and node.get("literal_type") == "NIL" and re.search(r"\.types\[\d+\]$", path):
        # examples/iface_target_switch.goo: `case nil:` in a type switch
        # (type_check_type_switch, type_checker.c) explicitly `continue`s
        # past the type_from_ast/node_type stamp every other case type gets.
        return True
    if kind == "BINARY_EXPR" and path.endswith(".comm"):
        # examples/select_send_closed_abort_probe.goo: a select case's send
        # comm (`ch <- v`) is checked by evaluating left/right individually
        # (mirroring codegen_setup_select_case); the whole ARROW BINARY_EXPR
        # is deliberately never routed through type_check_expression.
        return True
    return False

def walk(node, typed, ntypes, path, parent=None):
    if isinstance(node, list):
        for i, n in enumerate(node): walk(n, typed, ntypes, f"{path}[{i}]", parent)
        return
    if not isinstance(node, dict): return
    if "kind" in node:
        k = node["kind"]
        if not isinstance(k, str) or not k: fail(f"{path}: kind is not a string")
        p = node.get("pos")
        if not (isinstance(p, list) and len(p) == 3 and all(isinstance(x, int) for x in p)):
            fail(f"{path} ({k}): pos must be [line, column, offset]")
        if "type" in node:
            t = node["type"]
            if not isinstance(t, int) or t < 0 or t >= ntypes: fail(f"{path} ({k}): type id {t} does not resolve")
        elif typed and k in EXPR_KINDS and not known_unstamped(path, k, node, parent):
            fail(f"{path} ({k}): typed stage but no type id")
    unchecked = _unchecked_subtrees(node) if isinstance(node, dict) else set()
    type_arg0 = (node.get("kind") == "CALL_EXPR"
                 and (node.get("function") or {}).get("name") in _TYPE_ARG0_BUILTINS)
    for key, v in node.items():
        if key in ("kind", "pos", "type"): continue
        if key == "args" and type_arg0 and isinstance(v, list):
            for i, item in enumerate(v):
                walk(item, typed and i != 0, ntypes, f"{path}.{key}[{i}]", node)
            continue
        walk(v, typed and key not in unchecked, ntypes, f"{path}.{key}", node)

def check_types(types):
    for i, t in enumerate(types):
        if t.get("id") != i: fail(f"types[{i}]: id {t.get('id')} out of order")
        if not isinstance(t.get("kind"), str): fail(f"types[{i}]: kind missing")
        for key, v in t.items():
            refs = v if isinstance(v, list) else [v]
            for r in refs:
                if key.endswith("_type") or key in ("element", "key", "value", "pointee", "referenced", "base", "return", "constraint", "payload", "error", "params"):
                    if isinstance(r, int) and not (0 <= r < len(types)): fail(f"types[{i}].{key}: id {r} does not resolve")
                if isinstance(r, dict) and "type" in r and isinstance(r["type"], int) and not (0 <= r["type"] < len(types)):
                    fail(f"types[{i}].{key}: member type id {r['type']} does not resolve")

def main():
    with open(sys.argv[1]) as fh: d = json.load(fh)
    if d.get("goo_program_dump") != 1: fail("missing goo_program_dump: 1")
    stage = d.get("stage")
    if stage not in ("parse", "typed"): fail(f"bad stage {stage!r}")
    types = d.get("types")
    if not isinstance(types, list): fail("types must be a list")
    if stage == "parse" and types: fail("parse stage must have an empty type table")
    check_types(types)
    files = d.get("files")
    if not isinstance(files, list) or not files: fail("files must be a non-empty list")
    for fi, f in enumerate(files):
        for k in ("file", "package", "imports", "decls", "plan"):
            if k not in f: fail(f"files[{fi}]: missing {k}")
        walk(f["imports"], stage == "typed", len(types), f"files[{fi}].imports")
        walk(f["decls"], stage == "typed", len(types), f"files[{fi}].decls")
        if stage == "parse" and f["plan"] is not None: fail(f"files[{fi}]: parse stage must have plan: null")
    print("ok")

if __name__ == "__main__": main()
