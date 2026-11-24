#!/usr/bin/env python3
"""IBM Quantum OpenQASM 3.0 runner: Local Testing Mode (Aer / FakeBackend) + Live QPU.

Follows https://quantum.cloud.ibm.com/docs/en/guides/local-simulators:
  - IBM_MODE=aer  (default): runs SamplerV2(mode=AerSimulator()) locally (ideal
    dynamic-circuit execution, 0 QPU seconds, no IBM cloud credentials required).
  - IBM_MODE=fake: runs SamplerV2(mode=AerSimulator.from_backend(fake_backend))
    using qiskit_ibm_runtime.fake_provider (e.g. FakeTorino / FakeSherbrooke /
    FakeKyiv / FakeManilaV2), transpiling to the real device's ISA coupling map
    and simulating with its calibrated hardware noise model (0 QPU seconds).
  - IBM_LIVE_QPU=1: connects to QiskitRuntimeService(), transpiles to the target
    QPU's ISA, enforces sampler.options.max_execution_time=10s (protecting the
    10-minute/month Open Plan quota), and submits a single batched PUB job.
"""
import json
import os
import sys
from qiskit import qasm3
from qiskit.transpiler import generate_preset_pass_manager
from qiskit_ibm_runtime import SamplerV2 as Sampler

qasm_paths = [p for p in sys.argv[1:] if not p.startswith("-")]
if not qasm_paths:
    sys.exit(
        "Usage: [IBM_MODE=aer|fake] [IBM_LIVE_QPU=1] "
        "python3 scripts/qasm3_submit_ibm.py <file1.qasm> [file2.qasm ...]"
    )

shots = int(os.environ.get("QPU_SHOTS", "100"))
max_exec_s = int(os.environ.get("IBM_MAX_EXEC_SECONDS", "10"))
live_qpu = os.environ.get("IBM_LIVE_QPU", "0") == "1"
mode = os.environ.get("IBM_MODE", "live" if live_qpu else "aer").lower()

circuits = []
for path in qasm_paths:
    with open(path) as f:
        circuits.append((path, qasm3.loads(f.read())))

max_qubits = max(qc.num_qubits for _, qc in circuits)

if mode == "live" or live_qpu:
    from qiskit_ibm_runtime import QiskitRuntimeService

    service = QiskitRuntimeService()  # uses saved ~/.qiskit/qiskit-ibm.json
    try:
        usage = service.usage()
        print(f"IBM Quantum monthly quota usage: {json.dumps(usage)}", file=sys.stderr)
    except Exception:
        pass
    backend_name = os.environ.get("IBM_BACKEND")
    if backend_name:
        backend = service.backend(backend_name)
    else:
        backend = service.least_busy(
            operational=True, simulator=False, min_num_qubits=max_qubits
        )
elif mode == "fake":
    from qiskit_aer import AerSimulator
    from qiskit_ibm_runtime import fake_provider

    raw_name = os.environ.get("IBM_FAKE_BACKEND", "FakeMarrakesh")
    alias_map = {
        "ibm_fez": "FakeFez",
        "fake_fez": "FakeFez",
        "ibm_marrakesh": "FakeMarrakesh",
        "fake_marrakesh": "FakeMarrakesh",
        "ibm_kingston": "FakeKingston",
        "fake_kingston": "FakeKingston",
        "ibm_torino": "FakeTorino",
        "fake_torino": "FakeTorino",
        "ibm_sherbrooke": "FakeSherbrooke",
        "fake_sherbrooke": "FakeSherbrooke",
        "ibm_kyiv": "FakeKyiv",
        "fake_kyiv": "FakeKyiv",
    }
    fake_cls_name = alias_map.get(raw_name.lower(), raw_name)
    fake_cls = (
        getattr(fake_provider, fake_cls_name, None)
        or getattr(fake_provider, "FakeMarrakesh", None)
        or getattr(fake_provider, "FakeFez", None)
        or getattr(fake_provider, "FakeTorino", None)
        or getattr(fake_provider, "FakeManilaV2")
    )
    hw_snapshot = fake_cls()
    backend = AerSimulator.from_backend(hw_snapshot)
else:
    from qiskit_aer import AerSimulator

    backend = AerSimulator()

pm = generate_preset_pass_manager(optimization_level=1, backend=backend)
isa_circuits = []
for path, qc in circuits:
    isa_qc = pm.run(qc)
    ops = {str(k): int(v) for k, v in isa_qc.count_ops().items()}
    print(
        f"[ISA pre-flight] {os.path.basename(path)} -> {backend.name}: "
        f"depth={isa_qc.depth()}, ops={ops}, shots={shots}",
        file=sys.stderr,
    )
    isa_circuits.append(isa_qc)

if mode == "live" and not live_qpu:
    print(
        "Dry-run complete (0 QPU seconds used). Re-run with IBM_LIVE_QPU=1 to submit.",
        file=sys.stderr,
    )
    sys.exit(0)

sampler = Sampler(mode=backend)
if live_qpu:
    sampler.options.max_execution_time = max_exec_s

job = sampler.run(isa_circuits, shots=shots)
result = job.result()

out = {
    "mode": "live_qpu" if live_qpu else f"local_{mode}",
    "backend": backend.name,
    "job_id": job.job_id(),
    "shots": shots,
    "pubs": {
        os.path.basename(path): result[i].data.c.get_counts()
        for i, (path, _) in enumerate(circuits)
    },
}
print(json.dumps(out, indent=2))
