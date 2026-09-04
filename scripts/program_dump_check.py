#!/usr/bin/env python3
"""Structural check for a goo program dump. Exit 1 with one line naming the
first problem. Kept independent of the kind list on purpose: it checks the
SHAPE every node and type must have, and that every id resolves, so a new
kind needs no change here and a malformed one is still refused."""
import json, sys

EXPR_KINDS = {
    "IDENTIFIER", "LITERAL", "BINARY_EXPR", "UNARY_EXPR", "POSTFIX_EXPR",
    "CALL_EXPR", "INDEX_EXPR", "SELECTOR_EXPR", "SLICE_LIT", "MAP_LIT",
    "SLICE_INDEX_EXPR", "STRUCT_LITERAL", "ARRAY_LITERAL", "FUNC_LIT",
    "SLICE_CONVERSION", "TYPE_ASSERT", "TRY_EXPR", "CATCH_EXPR", "ADDR_OF",
    "PTR_DEREF", "MATCH_EXPR",
}

def fail(msg):
    print(msg); sys.exit(1)

def walk(node, typed, ntypes, path):
    if isinstance(node, list):
        for i, n in enumerate(node): walk(n, typed, ntypes, f"{path}[{i}]")
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
        elif typed and k in EXPR_KINDS:
            fail(f"{path} ({k}): typed stage but no type id")
    for key, v in node.items():
        if key in ("kind", "pos", "type"): continue
        walk(v, typed, ntypes, f"{path}.{key}")

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
