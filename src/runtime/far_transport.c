// M2-B1 far transport, NNG implementation. See far_transport.h for the
// interface contract and docs/superpowers/specs/2026-07-24-p6-lanes-m2-b1-design.md
// for the milestone design. Wire format: one message per value — 8-byte
// little-endian IEEE-754 float64 (native on every supported target today;
// documented for future cross-machine transports).
#include "far_transport.h"
#include <nng/nng.h>
#include <nng/protocol/pair1/pair.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

// Handle table: small, mutex-guarded, index-stable. FAR_MAX_SOCKETS bounds
// sockets per PROCESS: a rank needs at most 2 halo + (world-1 or 1) coll
// sockets, so 64 covers any plausible single-machine world.
#define FAR_MAX_SOCKETS 64

static nng_socket     far_socks[FAR_MAX_SOCKETS];
static int            far_used[FAR_MAX_SOCKETS];
static pthread_mutex_t far_mu = PTHREAD_MUTEX_INITIALIZER;

// Send/recv buffer depth (messages). Configured explicitly so the
// "sends never block on remote progress" envelope is a property we set,
// not an NNG default we inherited: protocol in-flight max is 1 per halo
// sub-exchange and `count`+1 per collective per rank; 128 bounds any
// probe shape with margin (documented envelope: per-rank lane count for
// far runs stays <= 128).
#define FAR_BUF_DEPTH 128

static goo_string_t far_errf(const char* op, const char* url, int rv) {
    char buf[512];
    if (url) {
        snprintf(buf, sizeof(buf), "far: %s failed: %s: %s", op, url, nng_strerror(rv));
    } else {
        snprintf(buf, sizeof(buf), "far: %s failed: %s", op, nng_strerror(rv));
    }
    return goo_string_new(buf);
}

static int far_slot_alloc(void) {
    pthread_mutex_lock(&far_mu);
    for (int i = 0; i < FAR_MAX_SOCKETS; i++) {
        if (!far_used[i]) {
            far_used[i] = 1;
            pthread_mutex_unlock(&far_mu);
            return i;
        }
    }
    pthread_mutex_unlock(&far_mu);
    return -1;
}

static void far_slot_free(int i) {
    pthread_mutex_lock(&far_mu);
    far_used[i] = 0;
    pthread_mutex_unlock(&far_mu);
}

static nng_socket far_sock(int64_t h) {
    if (h < 0 || h >= FAR_MAX_SOCKETS || !far_used[h]) {
        goo_panic("far: invalid socket handle");
    }
    return far_socks[h];
}

static int far_open_common(const char* op, const char* url,
                           int64_t* out_handle, goo_string_t* out_err,
                           int is_listen) {
    int slot = far_slot_alloc();
    if (slot < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "far: %s failed: %s: socket table full", op, url);
        *out_err = goo_string_new(buf);
        return 0;
    }
    int rv = nng_pair1_open(&far_socks[slot]);
    if (rv == 0) {
        nng_socket_set_int(far_socks[slot], NNG_OPT_SENDBUF, FAR_BUF_DEPTH);
        nng_socket_set_int(far_socks[slot], NNG_OPT_RECVBUF, FAR_BUF_DEPTH);
        if (is_listen) {
            rv = nng_listen(far_socks[slot], url, NULL, 0);
        } else {
            // NONBLOCK: the dialer starts async and retries until the
            // listener appears — process start order cannot deadlock setup.
            rv = nng_dial(far_socks[slot], url, NULL, NNG_FLAG_NONBLOCK);
        }
    }
    if (rv != 0) {
        nng_close(far_socks[slot]);
        far_slot_free(slot);
        *out_err = far_errf(op, url, rv);
        return 0;
    }
    *out_handle = slot;
    return 1;
}

static int nng_far_listen(const char* url, int64_t* out_handle, goo_string_t* out_err) {
    return far_open_common("listen", url, out_handle, out_err, 1);
}

static int nng_far_dial(const char* url, int64_t* out_handle, goo_string_t* out_err) {
    return far_open_common("dial", url, out_handle, out_err, 0);
}

static void nng_far_send_f64(int64_t h, double v) {
    int rv = nng_send(far_sock(h), &v, sizeof(v), 0);
    if (rv != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "far: send failed: %s", nng_strerror(rv));
        goo_panic(buf);
    }
}

static int nng_far_recv_f64(int64_t h, double* out_v, goo_string_t* out_err) {
    double tmp = 0.0;
    size_t sz = sizeof(tmp);
    int rv = nng_recv(far_sock(h), &tmp, &sz, 0);
    if (rv == NNG_ECLOSED) {
        *out_err = goo_string_new("far: closed");
        return 0;
    }
    if (rv != 0) {
        *out_err = far_errf("recv", NULL, rv);
        return 0;
    }
    if (sz != sizeof(tmp)) {
        goo_panic("far: recv message size != 8 (protocol violation)");
    }
    *out_v = tmp;
    return 1;
}

static void nng_far_close(int64_t h) {
    nng_close(far_sock(h));
    far_slot_free((int)h);
}

static const goo_far_transport_ops nng_ops = {
    .listen   = nng_far_listen,
    .dial     = nng_far_dial,
    .send_f64 = nng_far_send_f64,
    .recv_f64 = nng_far_recv_f64,
    .close    = nng_far_close,
};

// The active transport. Static for M2-B1 (NNG is the only impl); a future
// transport milestone swaps this pointer, nothing else.
static const goo_far_transport_ops* far_ops = &nng_ops;

int goo_far_listen(const char* url, int64_t* out_handle, goo_string_t* out_err) {
    return far_ops->listen(url, out_handle, out_err);
}
int goo_far_dial(const char* url, int64_t* out_handle, goo_string_t* out_err) {
    return far_ops->dial(url, out_handle, out_err);
}
void goo_far_send_f64(int64_t handle, double v) {
    far_ops->send_f64(handle, v);
}
int goo_far_recv_f64(int64_t handle, double* out_v, goo_string_t* out_err) {
    return far_ops->recv_f64(handle, out_v, out_err);
}
void goo_far_close(int64_t handle) {
    far_ops->close(handle);
}
