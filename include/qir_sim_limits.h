#ifndef GOO_QIR_SIM_LIMITS_H
#define GOO_QIR_SIM_LIMITS_H

// Capacity of the embedded QIR simulator (src/runtime/qir_sim.c). The compiler
// reads these limits to refuse a constant id that a native build cannot hold,
// and the simulator reads them to refuse an id that reaches it at run time.
// This header holds macros only, so the simulator can include it without the
// whole of runtime.h.
#define GOO_QIR_SIM_MAX_QUBITS 16u
#define GOO_QIR_SIM_MAX_RESULTS 64u

#endif
