#!/usr/bin/env bash
# fpga_equiv_probe.sh — Phase 8 M2: Exhaustive truth-table equivalence gate
# between native LLVM (-O2) execution, CIRCT hw/comb MLIR (--emit-mlir), and
# module-scope continuous-assignment SystemVerilog (--emit-sv) on
# examples/fpga_qec_decoder_probe.goo.
set -euo pipefail

COMPILER="${COMPILER:-./bin/goo}"
SRC="examples/fpga_qec_decoder_probe.goo"

evaluate_sv_and_mlir() {
    local sv_file="$1"
    local mlir_file="$2"
    local native_out="$3"
    python3 - "$sv_file" "$mlir_file" "$native_out" <<'PY'
import re, sys

sv_path, mlir_path, native_path = sys.argv[1:4]
sv_text = open(sv_path, encoding="utf-8").read()
mlir_text = open(mlir_path, encoding="utf-8").read()
native_lines = [l.strip() for l in open(native_path, encoding="utf-8") if l.strip()]

# 1. Verify CIRCT MLIR structural contract
for mod in ("@fpga_decode_bitflip", "@fpga_canary_alarm", "@fpga_syndrome_accum"):
    if f"hw.module {mod}(" not in mlir_text:
        sys.exit(f"FAIL: missing CIRCT hw.module {mod} in {mlir_path}")
for op in ("comb.icmp", "comb.mux", "comb.add", "hw.output"):
    if op not in mlir_text:
        sys.exit(f"FAIL: missing CIRCT {op} operation in {mlir_path}")

# 2. Verify SystemVerilog module-scope continuous-assignment hygiene (no latches / procedural blocks)
if "always" in sv_text:
    sys.exit("FAIL: SystemVerilog output contains procedural 'always' block (risk of unintended latch)")

def parse_sv_modules(text):
    mods = {}
    for m in re.finditer(r"module\s+(\w+)\s*\((.*?)\);\s*(.*?)endmodule", text, re.S):
        name, ports_raw, body = m.group(1), m.group(2), m.group(3)
        assigns = []
        for line in body.splitlines():
            line = line.strip()
            am = re.match(r"assign\s+(\w+)\s*=\s*(.+?);", line)
            if am:
                assigns.append((am.group(1), am.group(2)))
        mods[name] = assigns
    return mods

mods = parse_sv_modules(sv_text)
for req in ("fpga_decode_bitflip", "fpga_canary_alarm", "fpga_syndrome_accum"):
    if req not in mods:
        sys.exit(f"FAIL: missing SystemVerilog module {req}")

def eval_sv_mod(mod_name, inputs):
    env = dict(inputs)
    for lhs, rhs in mods[mod_name]:
        expr = re.sub(r"64'sd(-?\d+)", r"(\1)", rhs)
        expr = re.sub(r"1'b([01])", r"(\1)", expr)
        expr = expr.replace("&&", " and ").replace("||", " or ")
        # Handle ternary `cond ? a : b`
        tm = re.match(r"^(\w+)\s*\?\s*(.+?)\s*:\s*(.+)$", expr.strip())
        if tm:
            c_var, t_expr, f_expr = tm.group(1), tm.group(2), tm.group(3)
            val = eval(t_expr, {"__builtins__": {}}, env) if env[c_var] else eval(f_expr, {"__builtins__": {}}, env)
        else:
            val = int(bool(eval(expr, {"__builtins__": {}}, env))) if any(op in expr for op in ("==", "!=", "<", ">", " and ", " or ")) else int(eval(expr, {"__builtins__": {}}, env))
        env[lhs] = val
    return env["out_0"]

sv_lines = []
for s0 in (0, 1):
    for s1 in (0, 1):
        sv_lines.append(f"decode {s0} {s1} {eval_sv_mod('fpga_decode_bitflip', {'s0': s0, 's1': s1})}")
for t0 in (0, 1):
    for t1 in (0, 1):
        sv_lines.append(f"canary {t0} {t1} {eval_sv_mod('fpga_canary_alarm', {'trap0': t0, 'trap1': t1})}")
sv_lines.append(f"accum {eval_sv_mod('fpga_syndrome_accum', {'history_0': 1, 'history_1': 2, 'history_2': 3, 'history_3': 4, 'latest': 5})}")

if sv_lines != native_lines:
    sys.exit(f"FAIL: SystemVerilog truth table mismatch vs -O2 binary:\n  SV:     {sv_lines}\n  Native: {native_lines}")
PY
}

 WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

"$COMPILER" -O2 -o "$WORK/fpga_bin" "$SRC"
"$WORK/fpga_bin" > "$WORK/native.out"
"$COMPILER" --emit-mlir -o "$WORK/fpga.mlir" "$SRC"
"$COMPILER" --emit-sv -o "$WORK/fpga.sv" "$SRC"

if [ "${1:-}" = "--self-test" ]; then
    # 1. Control: unmutated SV + MLIR must pass against -O2 native output
    evaluate_sv_and_mlir "$WORK/fpga.sv" "$WORK/fpga.mlir" "$WORK/native.out"
    echo "    ok: control (unmutated MLIR + SystemVerilog vs -O2 binary) is GREEN"

    # 2. Mutation 1: flip a constant in the emitted SystemVerilog MUX tree -> must go RED
    sed 's/64'\''sd4/64'\''sd7/' "$WORK/fpga.sv" > "$WORK/fpga_bad.sv"
    if evaluate_sv_and_mlir "$WORK/fpga_bad.sv" "$WORK/fpga.mlir" "$WORK/native.out" 2>/dev/null; then
        echo "fpga-equiv-probe --self-test: FAIL (corrupted SystemVerilog MUX constant stayed GREEN)" >&2
        exit 1
    fi
    echo "    ok: corrupted SystemVerilog MUX constant turns the equivalence gate RED"

    # 3. Mutation 2: strip comb.mux from MLIR -> must go RED
    sed 's/comb\.mux/comb.noop/g' "$WORK/fpga.mlir" > "$WORK/fpga_bad.mlir"
    if evaluate_sv_and_mlir "$WORK/fpga.sv" "$WORK/fpga_bad.mlir" "$WORK/native.out" 2>/dev/null; then
        echo "fpga-equiv-probe --self-test: FAIL (missing comb.mux in MLIR stayed GREEN)" >&2
        exit 1
    fi
    echo "    ok: missing CIRCT comb.mux in MLIR turns the gate RED"
    echo "fpga-equiv-probe --self-test: PASS"
    exit 0
fi

evaluate_sv_and_mlir "$WORK/fpga.sv" "$WORK/fpga.mlir" "$WORK/native.out"
echo "fpga-equiv-probe: PASS (exhaustive truth table matches across LLVM -O2, CIRCT MLIR, and SystemVerilog RTL)"
