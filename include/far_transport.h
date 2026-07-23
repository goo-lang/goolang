#ifndef GOO_FAR_TRANSPORT_H
#define GOO_FAR_TRANSPORT_H

#include <stdint.h>
#include "runtime.h" // goo_string_t

// M2-B1 far transport. The ops struct is the roadmap's transport-agnostic
// runtime interface: all five Goo-visible far.* shims dispatch through it;
// NNG pair sockets are implementation #1, and an AIO/RDMA transport later
// replaces the ops pointer with no surface change. The Goo-visible
// envelope is deliberately narrow (lanes-internal): blocking send/recv
// only, no select integration, no close-propagation semantics promised.
typedef struct {
    int  (*listen)(const char* url, int64_t* out_handle, goo_string_t* out_err);
    int  (*dial)(const char* url, int64_t* out_handle, goo_string_t* out_err);
    void (*send_f64)(int64_t handle, double v); // goo_panic on failure
    int  (*recv_f64)(int64_t handle, double* out_v, goo_string_t* out_err);
    void (*close)(int64_t handle);
} goo_far_transport_ops;

// Shim entry points (declared to codegen in runtime_integration.c; rows in
// shim_signatures.c). ok-flag + out-param shape mirrors goo_os_read_file:
// return 1 with *out_handle / *out_v filled, or 0 with *out_err filled.
// Error spellings are API — lanes.go's recv pumps string-compare
// "far: closed" to tell clean teardown from mid-run transport death.
int  goo_far_listen(const char* url, int64_t* out_handle, goo_string_t* out_err);
int  goo_far_dial(const char* url, int64_t* out_handle, goo_string_t* out_err);
void goo_far_send_f64(int64_t handle, double v);
int  goo_far_recv_f64(int64_t handle, double* out_v, goo_string_t* out_err);
void goo_far_close(int64_t handle);

#endif
