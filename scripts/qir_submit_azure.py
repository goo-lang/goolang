#!/usr/bin/env python3
"""Azure Quantum (Quantinuum / Rigetti / IonQ) QIR submission script with credit/HQC guards.

Safeguards for Azure Quantum credits & Quantinuum HQC quotas:
  1. Offline QIR static analysis & HQC formula estimate first (works even without
     AZURE_QUANTUM_RESOURCE_ID): counts 1Q gates, 2Q gates, and measurements in the
     emitted QIR and computes Quantinuum's HQC cost:
       HQC = 5 + ((N_1q + 10 * N_2q + 5 * N_m) * shots) / 5000
     (Note: every non-syntax-checker Quantinuum job carries a 5 HQC base fee!)
  2. Dry-run by default (0 HQC / $0 spent unless AZURE_LIVE_QPU=1): queries
     target.estimate_cost() when credentials are set and exits before submitting.
  3. Zero-cost cloud verifier support: defaults to checking whether you want
     quantinuum.sim.h1-1sc (0 HQC syntax/QIR adaptive verifier) before spending
     HQC on quantinuum.sim.h1-1e or quantinuum.qpu.h2-1.
  4. Hard credit circuit-breaker (AZURE_MAX_HQC=10.0 default): refuses to submit
     if either the analytical or Azure-reported cost exceeds AZURE_MAX_HQC.
"""
import json
import os
import re
import sys

ir_paths = [p for p in sys.argv[1:] if not p.startswith("-")]
if not ir_paths:
    sys.exit("Usage: [AZURE_LIVE_QPU=1] python3 scripts/qir_submit_azure.py <file.ll>")

ir_path = ir_paths[0]
target_id = os.environ.get("AZURE_QPU_TARGET", "quantinuum.sim.h1-1sc")
shots = int(os.environ.get("QPU_SHOTS", "50"))
max_hqc = float(os.environ.get("AZURE_MAX_HQC", "10.0"))
live_qpu = os.environ.get("AZURE_LIVE_QPU", "0") == "1"

with open(ir_path) as f:
    qir_ir = f.read()

# Count QIR intrinsic call sites (excluding `declare` lines)
call_ops = re.findall(r"call void @__quantum__qis__([a-z0-9_]+)__body", qir_ir)
two_q_names = {"cnot", "cx", "cz", "swap", "ccx"}
meas_names = {"mz", "m", "mresetz", "reset"}

n_1q = sum(1 for op in call_ops if op not in two_q_names and op not in meas_names)
n_2q = sum(2 if op == "ccx" else 1 for op in call_ops if op in two_q_names)
n_m = sum(1 for op in call_ops if op in meas_names)

is_syntax_checker = target_id.endswith("sc")
est_hqc = 0.0 if is_syntax_checker else (5.0 + ((n_1q + 10 * n_2q + 5 * n_m) * shots) / 5000.0)

print(
    f"[QIR pre-flight] {os.path.basename(ir_path)} -> {target_id}: "
    f"1Q_calls={n_1q}, 2Q_calls={n_2q}, meas/reset_calls={n_m}, "
    f"shots={shots}, est_HQC={est_hqc:.3f} (cap={max_hqc:.2f})"
)

if est_hqc > max_hqc:
    sys.exit(
        f"ABORT: estimated {est_hqc:.2f} HQC exceeds AZURE_MAX_HQC={max_hqc:.2f}. "
        f"Lower QPU_SHOTS or use AZURE_QPU_TARGET=quantinuum.sim.h1-1sc (0 HQC)."
    )

resource_id = os.environ.get("AZURE_QUANTUM_RESOURCE_ID")
if not resource_id:
    print("AZURE_QUANTUM_RESOURCE_ID not set; offline pre-flight complete (0 credits used).")
    sys.exit(0)

from qdk.azure import Workspace

ws = Workspace(resource_id=resource_id)
target = ws.get_targets(target_id)

try:
    cost = target.estimate_cost(
        qir_ir,
        input_data_format="qir.v1",
        shots=shots,
        input_params={"entryPoint": "main", "arguments": []},
    )
    est_total = getattr(cost, "estimated_total", None)
    currency = getattr(cost, "currency_code", "HQC")
    print(f"[Azure estimate_cost] {est_total} {currency}")
    if est_total is not None and float(est_total) > max_hqc:
        sys.exit(f"ABORT: Azure estimate_cost ({est_total} {currency}) exceeds AZURE_MAX_HQC={max_hqc:.2f}")
except Exception as exc:
    print(f"[Azure estimate_cost] skipped/unsupported on {target_id}: {exc}")

if not live_qpu:
    print("Dry-run complete (0 credits used). Re-run with AZURE_LIVE_QPU=1 to submit.")
    sys.exit(0)

job = target.submit(
    qir_ir,
    name=f"goo-{os.path.basename(ir_path)}",
    input_data_format="qir.v1",
    output_data_format="microsoft.quantum-results.v1",
    shots=shots,
    input_params={"entryPoint": "main", "arguments": []},
)
print(f"Submitted Azure job (cap={max_hqc:.2f} HQC): {job.id} -> {target_id}")
job.wait_until_completed()
print(json.dumps({
    "backend": target_id,
    "job_id": job.id,
    "shots": shots,
    "estimated_hqc": round(est_hqc, 3),
    "results": job.get_results(),
}, indent=2))
