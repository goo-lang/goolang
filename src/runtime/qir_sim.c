// Phase 7 M1 — Deterministic 16-qubit QIR 1.0 Adaptive Profile simulator shim.
// Implements the `@__quantum__qis__*` and `@__quantum__rt__*` symbols so
// hybrid classical-quantum Goo programs link and run deterministically in
// `make test-golden` (-O0 and -O2) and `goo test`.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define QIR_SIM_MAX_QUBITS 16
#define QIR_SIM_DIM        (1u << QIR_SIM_MAX_QUBITS)
#define QIR_SIM_MAX_RES    64

typedef struct {
    double re;
    double im;
} QirComplex;

typedef struct {
    int        initialized;
    uint16_t   active_mask;
    uint64_t   rng_state;
    bool       results[QIR_SIM_MAX_RES];
    QirComplex sv[QIR_SIM_DIM];
} QirSimState;

static _Thread_local QirSimState g_qir_sim;

static void qir_sim_ensure_init(void) {
    if (g_qir_sim.initialized) return;
    memset(&g_qir_sim, 0, sizeof(g_qir_sim));
    g_qir_sim.sv[0].re = 1.0;
    g_qir_sim.sv[0].im = 0.0;
    const char* seed_env = getenv("GOO_QIR_SEED");
    if (seed_env && *seed_env) {
        g_qir_sim.rng_state = (uint64_t)strtoull(seed_env, NULL, 0);
    }
    if (g_qir_sim.rng_state == 0) {
        g_qir_sim.rng_state = 0x5149523153454544ULL;
    }
    g_qir_sim.initialized = 1;
}

static double qir_sim_rand_unit(void) {
    uint64_t x = g_qir_sim.rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_qir_sim.rng_state = x;
    uint64_t r = x * 2685821657736338717ULL;
    return (double)(r >> 11) * (1.0 / 9007199254740992.0);
}

static unsigned qir_sim_qubit_index(void* q) {
    qir_sim_ensure_init();
    unsigned idx = (unsigned)((uintptr_t)q) & (QIR_SIM_MAX_QUBITS - 1u);
    g_qir_sim.active_mask |= (uint16_t)(1u << idx);
    return idx;
}

static unsigned qir_sim_result_index(void* r) {
    qir_sim_ensure_init();
    return (unsigned)((uintptr_t)r) & (QIR_SIM_MAX_RES - 1u);
}

// Apply a 2x2 unitary [[u00, u01], [u10, u11]] to single qubit `q_idx`.
static void qir_sim_apply_1q(unsigned q_idx,
                             QirComplex u00, QirComplex u01,
                             QirComplex u10, QirComplex u11) {
    uint32_t bit = 1u << q_idx;
    for (uint32_t i = 0; i < QIR_SIM_DIM; i++) {
        if (i & bit) continue;
        uint32_t j = i | bit;
        QirComplex a = g_qir_sim.sv[i];
        QirComplex b = g_qir_sim.sv[j];

        g_qir_sim.sv[i].re = (u00.re * a.re - u00.im * a.im) + (u01.re * b.re - u01.im * b.im);
        g_qir_sim.sv[i].im = (u00.re * a.im + u00.im * a.re) + (u01.re * b.im + u01.im * b.re);

        g_qir_sim.sv[j].re = (u10.re * a.re - u10.im * a.im) + (u11.re * b.re - u11.im * b.im);
        g_qir_sim.sv[j].im = (u10.re * a.im + u10.im * a.re) + (u11.re * b.im + u11.im * b.re);
    }
}

void __quantum__qis__x__body(void* q);
void __quantum__qis__reset__body(void* q);

void* __quantum__rt__qubit_allocate(void) {
    qir_sim_ensure_init();
    for (unsigned idx = 0; idx < QIR_SIM_MAX_QUBITS; idx++) {
        if (!(g_qir_sim.active_mask & (uint16_t)(1u << idx))) {
            g_qir_sim.active_mask |= (uint16_t)(1u << idx);
            __quantum__qis__reset__body((void*)(uintptr_t)idx);
            return (void*)(uintptr_t)idx;
        }
    }
    return (void*)(uintptr_t)0;
}

void __quantum__rt__qubit_release(void* q) {
    unsigned idx = qir_sim_qubit_index(q);
    __quantum__qis__reset__body(q);
    g_qir_sim.active_mask &= (uint16_t)~(1u << idx);
}

void __quantum__qis__h__body(void* q) {
    unsigned idx = qir_sim_qubit_index(q);
    const double s = 0.70710678118654752440; // 1/sqrt(2)
    qir_sim_apply_1q(idx,
                     (QirComplex){s, 0.0}, (QirComplex){s, 0.0},
                     (QirComplex){s, 0.0}, (QirComplex){-s, 0.0});
}

void __quantum__qis__x__body(void* q) {
    unsigned idx = qir_sim_qubit_index(q);
    qir_sim_apply_1q(idx,
                     (QirComplex){0.0, 0.0}, (QirComplex){1.0, 0.0},
                     (QirComplex){1.0, 0.0}, (QirComplex){0.0, 0.0});
}

void __quantum__qis__y__body(void* q) {
    unsigned idx = qir_sim_qubit_index(q);
    qir_sim_apply_1q(idx,
                     (QirComplex){0.0, 0.0}, (QirComplex){0.0, -1.0},
                     (QirComplex){0.0, 1.0}, (QirComplex){0.0, 0.0});
}

void __quantum__qis__z__body(void* q) {
    unsigned idx = qir_sim_qubit_index(q);
    qir_sim_apply_1q(idx,
                     (QirComplex){1.0, 0.0}, (QirComplex){0.0, 0.0},
                     (QirComplex){0.0, 0.0}, (QirComplex){-1.0, 0.0});
}

void __quantum__qis__s__body(void* q) {
    unsigned idx = qir_sim_qubit_index(q);
    qir_sim_apply_1q(idx,
                     (QirComplex){1.0, 0.0}, (QirComplex){0.0, 0.0},
                     (QirComplex){0.0, 0.0}, (QirComplex){0.0, 1.0});
}

void __quantum__qis__s__adj(void* q) {
    unsigned idx = qir_sim_qubit_index(q);
    qir_sim_apply_1q(idx,
                     (QirComplex){1.0, 0.0}, (QirComplex){0.0, 0.0},
                     (QirComplex){0.0, 0.0}, (QirComplex){0.0, -1.0});
}

void __quantum__qis__t__body(void* q) {
    unsigned idx = qir_sim_qubit_index(q);
    const double s = 0.70710678118654752440;
    qir_sim_apply_1q(idx,
                     (QirComplex){1.0, 0.0}, (QirComplex){0.0, 0.0},
                     (QirComplex){0.0, 0.0}, (QirComplex){s, s});
}

void __quantum__qis__t__adj(void* q) {
    unsigned idx = qir_sim_qubit_index(q);
    const double s = 0.70710678118654752440;
    qir_sim_apply_1q(idx,
                     (QirComplex){1.0, 0.0}, (QirComplex){0.0, 0.0},
                     (QirComplex){0.0, 0.0}, (QirComplex){s, -s});
}

void __quantum__qis__rx__body(double theta, void* q) {
    unsigned idx = qir_sim_qubit_index(q);
    double c = cos(theta * 0.5);
    double s = sin(theta * 0.5);
    qir_sim_apply_1q(idx,
                     (QirComplex){c, 0.0}, (QirComplex){0.0, -s},
                     (QirComplex){0.0, -s}, (QirComplex){c, 0.0});
}

void __quantum__qis__ry__body(double theta, void* q) {
    unsigned idx = qir_sim_qubit_index(q);
    double c = cos(theta * 0.5);
    double s = sin(theta * 0.5);
    qir_sim_apply_1q(idx,
                     (QirComplex){c, 0.0}, (QirComplex){-s, 0.0},
                     (QirComplex){s, 0.0}, (QirComplex){c, 0.0});
}

void __quantum__qis__rz__body(double theta, void* q) {
    unsigned idx = qir_sim_qubit_index(q);
    double c = cos(theta * 0.5);
    double s = sin(theta * 0.5);
    qir_sim_apply_1q(idx,
                     (QirComplex){c, -s}, (QirComplex){0.0, 0.0},
                     (QirComplex){0.0, 0.0}, (QirComplex){c, s});
}

void __quantum__qis__cnot__body(void* c, void* t) {
    unsigned c_idx = qir_sim_qubit_index(c);
    unsigned t_idx = qir_sim_qubit_index(t);
    if (c_idx == t_idx) return;
    uint32_t c_bit = 1u << c_idx;
    uint32_t t_bit = 1u << t_idx;
    for (uint32_t i = 0; i < QIR_SIM_DIM; i++) {
        if ((i & c_bit) && !(i & t_bit)) {
            uint32_t j = i | t_bit;
            QirComplex tmp = g_qir_sim.sv[i];
            g_qir_sim.sv[i] = g_qir_sim.sv[j];
            g_qir_sim.sv[j] = tmp;
        }
    }
}

void __quantum__qis__cz__body(void* c, void* t) {
    unsigned c_idx = qir_sim_qubit_index(c);
    unsigned t_idx = qir_sim_qubit_index(t);
    if (c_idx == t_idx) return;
    uint32_t mask = (1u << c_idx) | (1u << t_idx);
    for (uint32_t i = 0; i < QIR_SIM_DIM; i++) {
        if ((i & mask) == mask) {
            g_qir_sim.sv[i].re = -g_qir_sim.sv[i].re;
            g_qir_sim.sv[i].im = -g_qir_sim.sv[i].im;
        }
    }
}

void __quantum__qis__swap__body(void* a, void* b) {
    unsigned a_idx = qir_sim_qubit_index(a);
    unsigned b_idx = qir_sim_qubit_index(b);
    if (a_idx == b_idx) return;
    uint32_t a_bit = 1u << a_idx;
    uint32_t b_bit = 1u << b_idx;
    for (uint32_t i = 0; i < QIR_SIM_DIM; i++) {
        if ((i & a_bit) && !(i & b_bit)) {
            uint32_t j = (i & ~a_bit) | b_bit;
            QirComplex tmp = g_qir_sim.sv[i];
            g_qir_sim.sv[i] = g_qir_sim.sv[j];
            g_qir_sim.sv[j] = tmp;
        }
    }
}

void __quantum__qis__ccx__body(void* c1, void* c2, void* t) {
    unsigned c1_idx = qir_sim_qubit_index(c1);
    unsigned c2_idx = qir_sim_qubit_index(c2);
    unsigned t_idx  = qir_sim_qubit_index(t);
    if (c1_idx == t_idx || c2_idx == t_idx) return;
    uint32_t c_mask = (1u << c1_idx) | (1u << c2_idx);
    uint32_t t_bit  = 1u << t_idx;
    for (uint32_t i = 0; i < QIR_SIM_DIM; i++) {
        if (((i & c_mask) == c_mask) && !(i & t_bit)) {
            uint32_t j = i | t_bit;
            QirComplex tmp = g_qir_sim.sv[i];
            g_qir_sim.sv[i] = g_qir_sim.sv[j];
            g_qir_sim.sv[j] = tmp;
        }
    }
}

static bool qir_sim_project_z(unsigned q_idx) {
    uint32_t bit = 1u << q_idx;
    double p1 = 0.0;
    for (uint32_t i = 0; i < QIR_SIM_DIM; i++) {
        if (i & bit) {
            p1 += g_qir_sim.sv[i].re * g_qir_sim.sv[i].re +
                  g_qir_sim.sv[i].im * g_qir_sim.sv[i].im;
        }
    }
    bool outcome;
    if (p1 <= 1e-12) {
        outcome = false;
    } else if (p1 >= 1.0 - 1e-12) {
        outcome = true;
    } else {
        outcome = (qir_sim_rand_unit() < p1);
    }

    double norm_sq = outcome ? p1 : (1.0 - p1);
    double inv_norm = (norm_sq > 1e-30) ? (1.0 / sqrt(norm_sq)) : 1.0;
    for (uint32_t i = 0; i < QIR_SIM_DIM; i++) {
        bool is_one = (i & bit) != 0;
        if (is_one == outcome) {
            g_qir_sim.sv[i].re *= inv_norm;
            g_qir_sim.sv[i].im *= inv_norm;
        } else {
            g_qir_sim.sv[i].re = 0.0;
            g_qir_sim.sv[i].im = 0.0;
        }
    }
    return outcome;
}

void __quantum__qis__mz__body(void* q, void* r) {
    unsigned q_idx = qir_sim_qubit_index(q);
    unsigned r_idx = qir_sim_result_index(r);
    g_qir_sim.results[r_idx] = qir_sim_project_z(q_idx);
}

void __quantum__qis__reset__body(void* q) {
    unsigned q_idx = qir_sim_qubit_index(q);
    if (qir_sim_project_z(q_idx)) {
        __quantum__qis__x__body(q);
    }
}

bool __quantum__rt__read_result(void* r) {
    unsigned r_idx = qir_sim_result_index(r);
    return g_qir_sim.results[r_idx];
}
