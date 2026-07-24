# Lanes M2-B1: NNG Far Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Distributed halo exchange and cross-rank collectives for `goostd/lanes` over vendored NNG, multi-process on one machine, probe-gated in `make verify-core`.

**Architecture:** Approach A from the approved spec (`docs/superpowers/specs/2026-07-24-p6-lanes-m2-b1-design.md`): lane channel fields stay ordinary cap-1 channels; Goo-level pump goroutines bridge process-edge channels to a tiny `far` shim package; the C side (`src/runtime/far_transport.c`) dispatches through a static transport-ops vtable with NNG pair sockets as implementation #1. Zero modification to existing C runtime paths — all code is additive.

**Tech Stack:** C23, NNG v1.12.0 (vendored tarball, static lib via cmake), LLVM-C codegen, vendored-Goo (`goostd/lanes`), bash probe scripts.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-24-p6-lanes-m2-b1-design.md` is normative; deviations get recorded there.
- **NNG version:** exactly v1.12.0, sha256-pinned tarball in `third_party/`. Never link the system/`/usr/local` nng (that machine has an unrelated 2.0.0-dev).
- **No parser/lexer/grammar changes anywhere.** If a task seems to need one, STOP — that's a design bug. `./scripts/grammar-tripwire.sh` must stay at its recorded baseline (31 S/R / 0 R/R).
- **Frozen surfaces:** `Partition`/`Run`/`Lane` public shapes and all existing `Lane` field shapes are frozen. Adding fields/functions is allowed; renaming/reshaping is not.
- **Vendored-Goo dialect** (lanes.go header documents these; they are real rejects): every binding is its own `:=`/`var` (no combined `var a, b`); goroutine loop-variable capture needs the `i := i` rebind inside a classic 3-clause `for` header; fixed-size arrays can't be sliced; no comma-ok receive; tagged switch on rune is bugged.
- **Commits:** conventional commits, imperative mood, `--no-gpg-sign` (1Password signing fails in this environment).
- **After every task:** `make test` green and the task's own probe green before commit. `make test-golden` (462/0) must be green after Task 4 (the only task touching `Run`'s internals) and at the end.
- **Never trust a piped exit code** — check command status directly.
- Error strings are load-bearing API: the exact spellings `"far: closed"` and `"far: recv failed: "` are compared by lanes.go pump code. Do not re-word them.

---

### Task 0: Spike — vendored goostd source calling a shim package

The design's one flagged assumption. Nothing in goostd today imports a shim
package; if this fails, everything after Task 3 is blocked. **Throwaway
code — do NOT commit the spike files.** (The permanent regression for this
behavior arrives in Task 4, when `lanes.go` itself does `import "far"` and
every far probe pins it.)

**Files:**
- Create (temporary, deleted at end of task): `goostd/spikeshim/spikeshim.go`
- Create (temporary, deleted at end of task): `examples/spike_shim_probe.goo`

**Interfaces:**
- Consumes: existing `fmt` shim (`fmt.Sprintf`), GOOROOT vendored-package resolution.
- Produces: a written finding (works / fails + failure mode) that gates Task 3's approach.

- [ ] **Step 1: Write the spike vendored package**

`goostd/spikeshim/spikeshim.go`:

```go
// TEMPORARY M2-B1 Task-0 spike: does a vendored goostd package's call into a
// SHIM package (fmt) type-check and lower? Deleted at the end of this task.
package spikeshim

import "fmt"

func Greet(n int) string {
	return fmt.Sprintf("spike %d", n)
}
```

`examples/spike_shim_probe.goo`:

```go
package main

import "fmt"
import "spikeshim"

func main() {
	fmt.Println(spikeshim.Greet(7))
}
```

- [ ] **Step 2: Build the compiler and run the spike**

Run: `make lexer && ./bin/goo -o build/spike_shim examples/spike_shim_probe.goo && ./build/spike_shim`
Expected: `spike 7`

- [ ] **Step 3: Record the outcome and clean up**

If it printed `spike 7`: the assumption holds. Delete both spike files
(`rm goostd/spikeshim/spikeshim.go && rmdir goostd/spikeshim && rm examples/spike_shim_probe.goo`),
and note "Task-0 spike: vendored→shim import works" for Task 7's handoff
update. Nothing to commit.

If it FAILED: **STOP the plan.** Record the exact error. The spec's fallback
applies: fixing the vendored-source→shim checker/codegen path becomes a new
task inserted before Task 3, designed against the actual failure mode (the
plan cannot pre-write that fix). Report to the user before proceeding.

---

### Task 1: Vendor NNG 1.12.0 and merge it into the runtime archive

**Files:**
- Create: `third_party/nng-1.12.0.tar.gz` (pinned tarball)
- Create: `third_party/README.md`
- Modify: `Makefile` (NNG build vars + rule; `$(RUNTIME_LIB)` rule becomes an `ar -M` merge)
- Create (throwaway, not committed): `build/nng_smoke.c`

**Interfaces:**
- Produces: `$(NNG_LIB)` = `build/nng/lib/libnng.a`; nng headers at `build/nng/include`; `lib/libgoo_runtime.a` now contains all nng objects, so every `bin/goo`-linked executable resolves nng symbols with **no codegen/linker changes** (codegen.c's link argv is untouched — the archive path it already uses just got richer).

- [ ] **Step 1: Download and pin the tarball**

```bash
mkdir -p third_party
curl -L -o third_party/nng-1.12.0.tar.gz https://github.com/nanomsg/nng/archive/refs/tags/v1.12.0.tar.gz
sha256sum third_party/nng-1.12.0.tar.gz
```

Record the printed hash — it goes into the Makefile as `NNG_SHA256` in Step 3
and into `third_party/README.md`.

- [ ] **Step 2: Write third_party/README.md**

```markdown
# third_party

Vendored dependencies. Policy: pinned release tarballs (sha256 recorded in
the Makefile and here), extracted and built into `build/` at compile time —
source trees are never committed, only tarballs.

| Dependency | Version | File | sha256 |
|---|---|---|---|
| NNG (nanomsg-next-gen) | v1.12.0 | nng-1.12.0.tar.gz | `<hash from Step 1>` |

NNG backs the `far` shim package (lanes M2-B1 far transport). MIT license.
The build deliberately ignores any system/`/usr/local` nng install.
```

- [ ] **Step 3: Add the NNG build rule to the Makefile**

Near the existing `RUNTIME_LIB` variable block (Makefile ~line 117), add:

```makefile
# M2-B1: vendored NNG (far transport). Pinned tarball, static lib, merged
# into libgoo_runtime.a below so bin/goo-linked executables need no new
# link flags. cmake is a build dependency of this rule only.
NNG_VERSION := 1.12.0
NNG_TARBALL := third_party/nng-$(NNG_VERSION).tar.gz
NNG_SHA256  := <hash from Step 1>
NNG_BUILD   := build/nng
NNG_LIB     := $(NNG_BUILD)/lib/libnng.a

$(NNG_LIB): $(NNG_TARBALL)
	@echo "$(NNG_SHA256)  $(NNG_TARBALL)" | sha256sum -c - >/dev/null
	rm -rf build/nng-src $(NNG_BUILD)
	mkdir -p build/nng-src
	tar -xzf $(NNG_TARBALL) -C build/nng-src --strip-components=1
	cmake -S build/nng-src -B $(NNG_BUILD)/cm -DCMAKE_BUILD_TYPE=Release \
	  -DBUILD_SHARED_LIBS=OFF -DNNG_TESTS=OFF -DNNG_TOOLS=OFF -DNNG_ENABLE_NNGCAT=OFF \
	  -DCMAKE_INSTALL_PREFIX=$(abspath $(NNG_BUILD)) -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
	cmake --build $(NNG_BUILD)/cm -j$(shell nproc) >/dev/null
	cmake --install $(NNG_BUILD)/cm >/dev/null
```

- [ ] **Step 4: Merge nng into the runtime archive**

Find the current rule (Makefile ~line 214):

```makefile
$(RUNTIME_LIB): $(RUNTIME_OBJS) | $(LIBDIR)
```

Replace its recipe with an MRI merge (GNU `ar -M`) and add `$(NNG_LIB)` to
the prerequisites:

```makefile
$(RUNTIME_LIB): $(RUNTIME_OBJS) $(NNG_LIB) | $(LIBDIR)
	rm -f $@
	{ echo "create $@"; \
	  for o in $(RUNTIME_OBJS); do echo "addmod $$o"; done; \
	  echo "addlib $(NNG_LIB)"; \
	  echo "save"; echo "end"; } | ar -M
	ranlib $@
```

(Whatever the old recipe's `ar rcs` line was, this replaces it; keep any
other lines, e.g. an `@echo`.)

- [ ] **Step 5: Smoke-test the merged archive**

```bash
make runtime-lib
cat > build/nng_smoke.c <<'EOF'
#include <nng/nng.h>
#include <nng/protocol/pair1/pair.h>
#include <stdio.h>
int main(void) {
    nng_socket s;
    if (nng_pair1_open(&s) != 0) { printf("open FAIL\n"); return 1; }
    nng_close(s);
    printf("nng %s ok\n", nng_version());
    return 0;
}
EOF
cc -Ibuild/nng/include build/nng_smoke.c lib/libgoo_runtime.a -lm -lpthread -o build/nng_smoke && ./build/nng_smoke
```

Expected: `nng 1.12.0 ok`

- [ ] **Step 6: Verify nothing regressed**

Run: `make test`
Expected: unit suite + CLI suite green (the archive got bigger; unused nng
objects are never pulled into existing programs).

- [ ] **Step 7: Commit**

```bash
git add third_party/nng-1.12.0.tar.gz third_party/README.md Makefile
git commit --no-gpg-sign -m "build(third_party): vendor NNG 1.12.0, merge static lib into runtime archive (M2-B1 T1)"
```

---

### Task 2: `far_transport.c` — transport vtable + NNG impl + C unit test

**Files:**
- Create: `include/far_transport.h`
- Create: `src/runtime/far_transport.c`
- Create: `tests/runtime/far_transport_test.c`
- Modify: `Makefile` (add `far_transport.o` to `RUNTIME_OBJS`, its include-path/target deps, `far-transport-test` + `far-transport-asan` targets)

**Interfaces:**
- Consumes: `$(NNG_LIB)` + headers (Task 1); `goo_string_new`, `goo_panic` from `include/runtime.h` (check `goo_panic`'s exact prototype in runtime.h before writing the extern — match it).
- Produces (the shim ABI Tasks 3+ compile against — exact):
  - `int  goo_far_listen(const char* url, int64_t* out_handle, goo_string_t* out_err);`
  - `int  goo_far_dial(const char* url, int64_t* out_handle, goo_string_t* out_err);`
  - `void goo_far_send_f64(int64_t handle, double v);`
  - `int  goo_far_recv_f64(int64_t handle, double* out_v, goo_string_t* out_err);`
  - `void goo_far_close(int64_t handle);`
  - Error spellings: `"far: closed"` (recv after local close), `"far: recv failed: <nng error>"`, `"far: listen failed: <url>: <nng error>"`, `"far: dial failed: <url>: <nng error>"`.

- [ ] **Step 1: Write the failing unit test**

`tests/runtime/far_transport_test.c`:

```c
// far_transport unit test (M2-B1 T2). Pins the shim ABI, FIFO ordering,
// the buffering envelope (sends never block on remote progress for the
// protocol's in-flight counts), and the "far: closed" vs hard-failure
// error split the lanes.go pumps compare against.
#include "far_transport.h"
#include "runtime.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char url[256];

static int str_eq(goo_string_t s, const char* want) {
    return s.length == strlen(want) && memcmp(s.data, want, s.length) == 0;
}
static int str_starts(goo_string_t s, const char* pre) {
    return s.length >= strlen(pre) && memcmp(s.data, pre, strlen(pre)) == 0;
}

static void* closer_thread(void* arg) {
    int64_t h = *(int64_t*)arg;
    usleep(200 * 1000);
    goo_far_close(h);
    return NULL;
}

int main(void) {
    char tmpl[] = "/tmp/goo-far-test-XXXXXX";
    char* dir = mkdtemp(tmpl);
    assert(dir);
    snprintf(url, sizeof(url), "ipc://%s/sock", dir);

    goo_string_t err;
    int64_t a = -1, b = -1;

    // listen/dial roundtrip
    assert(goo_far_listen(url, &a, &err) == 1);
    assert(goo_far_dial(url, &b, &err) == 1);

    // Buffering envelope pin: 8 sends complete with NO receiver progress.
    // (Protocol in-flight max is per-sub-exchange 1 for halos and `count`
    // partials + 1 broadcast for collectives; 8 covers the probe shapes
    // with margin. far_transport.c sets SENDBUF/RECVBUF=128 to make this
    // a configured property, not a default-dependent accident.)
    for (int i = 0; i < 8; i++) goo_far_send_f64(a, 1.5 * i);

    // FIFO pin: values arrive in send order.
    for (int i = 0; i < 8; i++) {
        double v = -1.0;
        assert(goo_far_recv_f64(b, &v, &err) == 1);
        assert(v == 1.5 * i);
    }

    // Bidirectional (pair socket): b -> a works too.
    goo_far_send_f64(b, 42.0);
    double v = 0.0;
    assert(goo_far_recv_f64(a, &v, &err) == 1);
    assert(v == 42.0);

    // "far: closed": recv blocked on b unblocks when b is closed locally.
    pthread_t t;
    pthread_create(&t, NULL, closer_thread, &b);
    assert(goo_far_recv_f64(b, &v, &err) == 0);
    assert(str_eq(err, "far: closed"));
    pthread_join(t, NULL);

    // Hard failure branch: malformed URL.
    int64_t c = -1;
    assert(goo_far_listen("bogus://nope", &c, &err) == 0);
    assert(str_starts(err, "far: listen failed: "));

    goo_far_close(a);
    printf("far_transport_test: PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run it to see it fail to build**

Run: `cc -Iinclude -Ibuild/nng/include tests/runtime/far_transport_test.c lib/libgoo_runtime.a -lm -lpthread -o build/far_test`
Expected: FAIL — `far_transport.h: No such file or directory`.

- [ ] **Step 3: Write the header**

`include/far_transport.h`:

```c
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
```

- [ ] **Step 4: Write the implementation**

`src/runtime/far_transport.c`:

```c
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
```

Before compiling: check `goo_panic`'s prototype in `include/runtime.h`. If it
is variadic (`goo_panic(const char* fmt, ...)`) the calls above still work
as written; if it takes a single `const char*`, they also work. Match
whatever the header says — do not add your own extern.

Note the NNG 1.12 option-setter name: if `nng_socket_set_int` is not
declared in the vendored headers, the 1.x spelling is `nng_setopt_int(sock,
NNG_OPT_SENDBUF, n)` — use whichever the vendored `nng.h` declares (grep it),
and keep the explicit buffer configuration either way.

- [ ] **Step 5: Wire into the Makefile**

In `Makefile`:

1. Append to `RUNTIME_OBJS` (line ~122): ` $(BUILDDIR)/runtime/far_transport.o`
2. Below the NNG rule from Task 1, add the object's include path + dep:

```makefile
$(BUILDDIR)/runtime/far_transport.o: CFLAGS += -I$(NNG_BUILD)/include
$(BUILDDIR)/runtime/far_transport.o: $(NNG_LIB)
```

3. Add the test targets (near the other test targets, ~line 4400):

```makefile
# M2-B1: far transport C unit test — shim ABI, FIFO, buffering envelope,
# "far: closed" split. The ASan variant compiles the runtime objects it
# needs directly (an archive built without ASan can't be reused).
far-transport-test: $(RUNTIME_LIB)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -Iinclude -I$(NNG_BUILD)/include tests/runtime/far_transport_test.c $(RUNTIME_LIB) -o $(BINDIR)/far_transport_test -lm -lpthread
	@$(BINDIR)/far_transport_test && echo "far-transport-test: PASS"

far-transport-asan: $(NNG_LIB)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -g -fsanitize=address -Iinclude -I$(NNG_BUILD)/include \
	  tests/runtime/far_transport_test.c src/runtime/far_transport.c src/runtime/runtime.c src/runtime/platform.c \
	  $(NNG_LIB) -o $(BINDIR)/far_transport_asan -lm -lpthread
	@$(BINDIR)/far_transport_asan && echo "far-transport-asan: PASS"
.PHONY: far-transport-test far-transport-asan
```

(If `runtime.c`/`platform.c` drag in more runtime deps than they resolve,
add the minimal extra `src/runtime/*.c` files the linker names — keep the
ASan compile list minimal, not the whole archive.)

- [ ] **Step 6: Run the tests**

Run: `make far-transport-test && make far-transport-asan`
Expected: both print `... PASS`. If the buffering assert (8 sends) hangs,
the buffer options did not take — fix the option-setter spelling per Step 4's
note; do NOT weaken the test.

- [ ] **Step 7: Full suite, then commit**

Run: `make test`
Expected: green.

```bash
git add include/far_transport.h src/runtime/far_transport.c tests/runtime/far_transport_test.c Makefile
git commit --no-gpg-sign -m "feat(runtime): far transport vtable + NNG pair impl + unit test (M2-B1 T2)"
```

---

### Task 3: `far` shim package — checker + codegen integration

**Files:**
- Modify: `src/compiler/goo.c` (shim package list, ~line 605)
- Modify: `src/types/type_checker.c` + `src/types/expression_checker.c` (the two marker lists goo.c's comment says to keep in sync)
- Modify: `include/shim_signatures.h` (new `SHIM_RET_F64_RESULT`; widen the ATOI comment)
- Modify: `src/types/shim_signatures.c` (five `far` rows + param arrays + `shim_ret_type` case)
- Modify: `src/codegen/runtime_integration.c` (five declarations)
- Modify: `src/codegen/call_codegen.c` (dispatch arms + `codegen_generate_far_result_call`)
- Create: `examples/far_shim_probe.goo` + `examples/far_shim_probe.expected.txt`
- Modify: `Makefile` (`far-shim-probe` target)

**Interfaces:**
- Consumes: Task 2's five `goo_far_*` symbols (exact signatures in Task 2's Produces block).
- Produces (what Task 4's lanes.go compiles against):
  - `far.Listen(url string) !int`
  - `far.Dial(url string) !int`
  - `far.SendF64(sock int, v float64)`
  - `far.RecvF64(sock int) !float64`
  - `far.Close(sock int)`

- [ ] **Step 1: Write the failing fixture**

`examples/far_shim_probe.goo`:

```go
// far_shim_probe (M2-B1 T3): the far shim surface end-to-end in ONE
// process — listen+dial to self over ipc, send/recv floats both ways,
// and both error-union branches (bogus listen URL; catch prints).
package main

import "far"
import "fmt"

func main() {
	url := "ipc:///tmp/goo-far-shim-probe.sock"
	a := far.Listen(url) catch e {
		panic(e.Error())
	}
	b := far.Dial(url) catch e {
		panic(e.Error())
	}
	far.SendF64(a, 1.5)
	far.SendF64(a, 2.5)
	v1 := far.RecvF64(b) catch e {
		panic(e.Error())
	}
	v2 := far.RecvF64(b) catch e {
		panic(e.Error())
	}
	fmt.Println(v1 == 1.5)
	fmt.Println(v2 == 2.5)
	far.SendF64(b, 9.0)
	v3 := far.RecvF64(a) catch e {
		panic(e.Error())
	}
	fmt.Println(v3 == 9.0)
	far.Close(a)
	far.Close(b)
	bad := far.Listen("bogus://x") catch e {
		fmt.Println(e.Error())
		fmt.Println(true)
		return
	}
	fmt.Println(bad)
}
```

`examples/far_shim_probe.expected.txt`:

```
true
true
true
far: listen failed: bogus://x: Address invalid
true
```

(The last-but-one line is nng_strerror(NNG_EADDRINVAL)'s text. If the real
run prints a different nng message for a bogus scheme, update the expected
file to the OBSERVED text once — it is pinned from then on.)

- [ ] **Step 2: Run to verify it fails**

Run: `make lexer && ./bin/goo -o build/far_shim examples/far_shim_probe.goo`
Expected: FAIL — unresolved import/package `far` (exact message depends on
which layer rejects first; record it).

- [ ] **Step 3: Register the package in the three marker lists**

In `src/compiler/goo.c` (~line 605), extend:

```c
    static const char* const shim[] = {"fmt", "os", "math", "errors", "sync", "time", "far"};
```

Then find the two sibling lists the comment above that function names
("Keep in sync with the marker list in type_checker.c and
stdlib_package_lookup in expression_checker.c"):

Run: `grep -n '"time"' src/types/type_checker.c src/types/expression_checker.c`

Add `"far"` to each list found, following the exact local pattern (these are
string arrays or if-chains naming the shim packages; mirror the `"time"`
entries).

- [ ] **Step 4: Add the signature rows**

`include/shim_signatures.h` — add to the `ShimRetKind` enum after
`SHIM_RET_STRING_RESULT`:

```c
    SHIM_RET_F64_RESULT,     // far.RecvF64: !float64 (float64 success / string error)
```

and widen the `SHIM_RET_ATOI_RESULT` comment from "strconv.Atoi only" to
"strconv.Atoi + far.Listen/Dial (all !int: int64 success / string error)".

`src/types/shim_signatures.c` — add a param array next to the existing ones:

```c
static const ShimParamKind PARAMS_INT64_FLOAT64[]        = { SHIM_PARAM_INT64, SHIM_PARAM_FLOAT64 };
```

and the table rows (after the last existing package block):

```c
    // far (M2-B1): far-transport shims, runtime side src/runtime/far_transport.c.
    // Listen/Dial reuse the !int construction ATOI_RESULT already builds;
    // RecvF64 needs the new !float64 tag. Error spellings are API (see
    // far_transport.h).
    { "far", "Listen",  SHIM_RET_ATOI_RESULT, PARAMS_STRING,        NPARAMS(PARAMS_STRING), 0 },
    { "far", "Dial",    SHIM_RET_ATOI_RESULT, PARAMS_STRING,        NPARAMS(PARAMS_STRING), 0 },
    { "far", "SendF64", SHIM_RET_VOID,        PARAMS_INT64_FLOAT64, NPARAMS(PARAMS_INT64_FLOAT64), 0 },
    { "far", "RecvF64", SHIM_RET_F64_RESULT,  PARAMS_INT64,         NPARAMS(PARAMS_INT64), 0 },
    { "far", "Close",   SHIM_RET_VOID,        PARAMS_INT64,         NPARAMS(PARAMS_INT64), 0 },
```

In the same file's `shim_ret_type()` switch, add the new case by mirroring
the `SHIM_RET_ATOI_RESULT` case exactly, substituting the float64 builtin
for the int64 one (same error-union constructor, same one-line shape).

- [ ] **Step 5: Declare the runtime functions to codegen**

In `src/codegen/runtime_integration.c`, next to the `goo_os_read_file`
declaration (~line 357), using the same local type variables that block
already has in scope (i32/i64/double/pointer types — mirror the neighboring
declarations' style exactly):

```c
    // M2-B1 far transport (far_transport.h): ok-flag + out-param shims.
    // int goo_far_listen(const char* url, int64_t* out, goo_string_t* err)
    // int goo_far_dial(const char* url, int64_t* out, goo_string_t* err)
    // void goo_far_send_f64(int64_t, double)
    // int goo_far_recv_f64(int64_t, double* out, goo_string_t* err)
    // void goo_far_close(int64_t)
```

with five `add_runtime_function` calls whose param arrays match those C
prototypes (pointer params use the same pointer-type variable the ReadFile
declaration uses; `double*` is a plain pointer type in opaque-pointer LLVM —
same variable).

- [ ] **Step 6: Codegen — result helper + dispatch arms**

In `src/codegen/call_codegen.c`, directly below
`codegen_generate_string_result_call` (~line 2699), add:

```c
// far.Listen/Dial (!int) and far.RecvF64 (!float64), M2-B1. Same ok-flag
// shape as codegen_generate_string_result_call above, but with TWO
// out-params (typed value slot + goo_string_t error slot) because the
// success type is not a string: ok=1 -> load *out_val, wrap success;
// ok=0 -> load *out_err, wrap error.
typedef enum { FAR_RESULT_I64, FAR_RESULT_F64 } FarResultKind;

static ValueInfo* codegen_generate_far_result_call(CodeGenerator* codegen, TypeChecker* checker,
                                                    ASTNode* expr, const char* runtime_symbol,
                                                    FarResultKind kind, ASTNode* arg,
                                                    int arg_is_string) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available for %s", runtime_symbol);
    return NULL;
#else
    if (!codegen || !checker || !expr) return NULL;

    Type* result_type = expr->node_type; // !int64 or !float64 via shim table
    if (!result_type || !type_is_error_union(result_type)) {
        codegen_error(codegen, expr->pos, "%s: no error-union type context", runtime_symbol);
        return NULL;
    }
    LLVMTypeRef union_llvm = codegen_type_to_llvm(codegen, result_type);
    if (!union_llvm) return NULL;

    LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, runtime_symbol);
    if (!fn) {
        codegen_error(codegen, expr->pos, "%s not found in module", runtime_symbol);
        return NULL;
    }

    LLVMTypeRef val_llvm = (kind == FAR_RESULT_I64)
        ? LLVMInt64TypeInContext(codegen->context)
        : LLVMDoubleTypeInContext(codegen->context);
    LLVMTypeRef string_llvm = codegen_get_basic_type(codegen, TYPE_STRING);
    LLVMValueRef out_val_ptr = codegen_create_entry_alloca(codegen, val_llvm, "far_result_val_out");
    LLVMValueRef out_err_ptr = codegen_create_entry_alloca(codegen, string_llvm, "far_result_err_out");

    LLVMValueRef first;
    if (arg_is_string) {
        first = codegen_arg_as_cstr(codegen, checker, arg);
        if (!first) return NULL;
    } else {
        ValueInfo* vi = codegen_generate_expression(codegen, checker, arg);
        if (!vi) return NULL;
        first = vi->value;
    }
    LLVMValueRef call_args[] = { first, out_val_ptr, out_err_ptr };
    LLVMValueRef ok = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn,
                                     call_args, 3, "far_result_ok");

    LLVMValueRef zero_i32 = LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0);
    LLVMValueRef cond = LLVMBuildICmp(codegen->builder, LLVMIntNE, ok, zero_i32, "far_result_cond");

    LLVMBasicBlockRef success_block = codegen_create_block(codegen, "far_result.success");
    LLVMBasicBlockRef error_block   = codegen_create_block(codegen, "far_result.error");
    LLVMBasicBlockRef merge_block   = codegen_create_block(codegen, "far_result.merge");
    LLVMBuildCondBr(codegen->builder, cond, success_block, error_block);

    codegen_set_insert_point(codegen, success_block);
    LLVMValueRef success_val = LLVMBuildLoad2(codegen->builder, val_llvm, out_val_ptr, "far_result_v");
    Type* value_type = result_type->data.error_union.value_type;
    LLVMValueRef succ = codegen_create_error_union_success(codegen, union_llvm, success_val, value_type);
    LLVMBuildBr(codegen->builder, merge_block);
    LLVMBasicBlockRef success_exit = LLVMGetInsertBlock(codegen->builder);

    codegen_set_insert_point(codegen, error_block);
    LLVMValueRef error_str = LLVMBuildLoad2(codegen->builder, string_llvm, out_err_ptr, "far_result_e");
    LLVMValueRef errv = codegen_create_error_union_error(codegen, union_llvm, error_str);
    LLVMBuildBr(codegen->builder, merge_block);
    LLVMBasicBlockRef error_exit = LLVMGetInsertBlock(codegen->builder);

    codegen_set_insert_point(codegen, merge_block);
    LLVMValueRef phi = LLVMBuildPhi(codegen->builder, union_llvm, "far_result");
    LLVMAddIncoming(phi, &succ, &success_exit, 1);
    LLVMAddIncoming(phi, &errv, &error_exit, 1);

    return value_info_new(NULL, phi, result_type);
#endif
}
```

If `codegen_generate_expression`'s actual name/signature at that site
differs (grep how the nearest int-taking stdlib arm generates its argument),
mirror THAT idiom for the non-string argument instead — the helper's shape
stays the same. If the value slot's expected width differs (Goo `int` is
i64 per shim_signatures.h's doc), keep i64.

Then, next to the `os.ReadFile` dispatch arm (~line 1332), add the `far`
arms in the same dispatch chain:

```c
            if (strcmp(dispatch_pkg, "far") == 0) {
                if (strcmp(sel->selector, "Listen") == 0 || strcmp(sel->selector, "Dial") == 0) {
                    if (call->arg_count != 1) {
                        codegen_error(codegen, expr->pos, "far.%s: expected one string argument", sel->selector);
                        return NULL;
                    }
                    const char* sym = (strcmp(sel->selector, "Listen") == 0) ? "goo_far_listen" : "goo_far_dial";
                    return codegen_generate_far_result_call(codegen, checker, expr, sym,
                                                            FAR_RESULT_I64, call->args[0], 1);
                }
                if (strcmp(sel->selector, "RecvF64") == 0) {
                    if (call->arg_count != 1) {
                        codegen_error(codegen, expr->pos, "far.RecvF64: expected one int argument");
                        return NULL;
                    }
                    return codegen_generate_far_result_call(codegen, checker, expr, "goo_far_recv_f64",
                                                            FAR_RESULT_F64, call->args[0], 0);
                }
                if (strcmp(sel->selector, "SendF64") == 0) {
                    return codegen_generate_stdlib_call(codegen, checker, expr, "goo_far_send_f64", TYPE_VOID, 0);
                }
                if (strcmp(sel->selector, "Close") == 0) {
                    return codegen_generate_stdlib_call(codegen, checker, expr, "goo_far_close", TYPE_VOID, 0);
                }
            }
```

(Mirror the surrounding arms' exact access patterns for `sel`, `call`,
`dispatch_pkg` — they are already in scope at that site.)

- [ ] **Step 7: Build, run the fixture, add the probe target**

Run: `make lexer && ./bin/goo -o build/far_shim examples/far_shim_probe.goo && ./build/far_shim`
Expected: the five lines of the expected file (adjust the nng error text
once, per Step 1's note).

Makefile (next to the other probe targets):

```makefile
far-shim-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/far_shim_probe examples/far_shim_probe.goo
	@rm -f /tmp/goo-far-shim-probe.sock
	@./build/far_shim_probe > build/far_shim_probe.actual.txt
	@if diff -u examples/far_shim_probe.expected.txt build/far_shim_probe.actual.txt; then \
	  echo "far-shim-probe: PASS"; \
	else \
	  echo "far-shim-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
.PHONY: far-shim-probe
```

Also add `far-transport-test`, `far-transport-asan`, and `far-shim-probe` to
the verify list: find it with `grep -n "VERIFY_ALL_DEPS" Makefile`, append
the three names to the definition (NOT to the `filter-out`, so they run in
both `verify` and `verify-core`).

- [ ] **Step 8: Gates, then commit**

Run: `make test && make far-shim-probe && ./scripts/grammar-tripwire.sh`
Expected: all green; tripwire at baseline (no grammar files touched).

```bash
git add src/compiler/goo.c src/types/type_checker.c src/types/expression_checker.c \
  include/shim_signatures.h src/types/shim_signatures.c \
  src/codegen/runtime_integration.c src/codegen/call_codegen.c \
  examples/far_shim_probe.goo examples/far_shim_probe.expected.txt Makefile
git commit --no-gpg-sign -m "feat(types,codegen): far shim package — Listen/Dial !int, SendF64, RecvF64 !float64, Close (M2-B1 T3)"
```

---

### Task 4: `lanes.RunFar` + pump goroutines + far-halo-probe

**Files:**
- Modify: `goostd/lanes/lanes.go` (runCore extraction, `farCfg`, `farItoa`, `RunFar`, pumps)
- Create: `examples/far_halo_probe.goo`
- Create: `scripts/far-probe.sh`
- Modify: `Makefile` (`far-halo-probe` target + VERIFY_ALL_DEPS)

**Interfaces:**
- Consumes: Task 3's `far.*` surface (exact signatures in Task 3's Produces block).
- Produces:
  - `func RunFar(p Partitioned, steps int, rank int, world int, urlBase string, body func(ctx *Lane)) []float64`
  - private `type farCfg struct` with fields `hasL, hasR bool; sendL, recvL, sendR, recvR chan float64` (Task 6 adds collective fields to this same struct)
  - private `func runCore(p Partitioned, steps int, body func(ctx *Lane), fc farCfg) []float64`
  - private `func farItoa(n int) string`
  - Socket naming: `<urlBase>.halo.<r>` between ranks r and r+1; rank r LISTENS on `.halo.<r>` (its right edge), rank r+1 DIALS it.
  - `scripts/far-probe.sh <fixture-binary> <world> [extra-args...]` — spawns ranks with args `far <rank> <url>`, 30s timeout each, asserts every rank's stdout is non-empty and all-`true` lines.

- [ ] **Step 1: Extract runCore (pure refactor, no far edges yet)**

In `goostd/lanes/lanes.go`, add above `Run`:

```go
// farCfg carries RunFar's process-boundary wiring into runCore. The zero
// value (all false/nil) is exactly Run's M1/M2-B2 behavior: process edges
// are global Dirichlet edges. M2-B1 design record:
// docs/superpowers/specs/2026-07-24-p6-lanes-m2-b1-design.md.
type farCfg struct {
	hasL  bool
	hasR  bool
	sendL chan float64
	recvL chan float64
	sendR chan float64
	recvR chan float64
}
```

Then rename `Run`'s body into `runCore(p Partitioned, steps int, body func(ctx *Lane), fc farCfg) []float64` with ONLY these wiring changes (everything else — done/rightward/leftward/partials/results construction, the join loop, the return — is moved verbatim):

```go
			l := Lane{
				id:       i,
				steps:    steps,
				own:      p.backing[i*p.width : (i+1)*p.width],
				edgeL:    i == 0 && !fc.hasL,
				edgeR:    i == p.count-1 && !fc.hasR,
				count:    p.count,
				partials: partials,
				results:  results,
				scratch:  make([]float64, p.width),
			}
			if i > 0 {
				l.recvL = rightward[i-1]
				l.sendL = leftward[i-1]
			}
			if i == 0 && fc.hasL {
				l.sendL = fc.sendL
				l.recvL = fc.recvL
			}
			if i < p.count-1 {
				l.sendR = rightward[i]
				l.recvR = leftward[i]
			}
			if i == p.count-1 && fc.hasR {
				l.sendR = fc.sendR
				l.recvR = fc.recvR
			}
```

(Equivalence argument to keep in a comment: with the zero farCfg,
`edgeL == (i==0)` and `edgeR == (i==count-1)`, so `i > 0 ⇔ !edgeL` and
`i < count-1 ⇔ !edgeR` — the exact conditions the old wiring used.)

And `Run` becomes:

```go
func Run(p Partitioned, steps int, body func(ctx *Lane)) []float64 {
	return runCore(p, steps, body, farCfg{})
}
```

Keep `Run`'s full doc comment on `Run` (its contract is unchanged).

- [ ] **Step 2: Prove the refactor is behavior-neutral**

Run: `make test && make test-golden && make verify-core`
Expected: ALL green — 462/0 goldens, every lanes probe passing. This is the
gate that lets a frozen surface's internals move. If ANYTHING fails, fix or
revert before continuing.

- [ ] **Step 3: Commit the refactor separately**

```bash
git add goostd/lanes/lanes.go
git commit --no-gpg-sign -m "refactor(lanes): extract runCore with farCfg seam; Run delegates with zero cfg (M2-B1 T4)"
```

- [ ] **Step 4: Add farItoa + RunFar + pumps**

Append to `goostd/lanes/lanes.go`:

```go
// farItoa: minimal non-negative int -> decimal string, local so lanes.go
// assumes nothing about vendored-to-vendored imports (only the far/fmt
// SHIM import path is proven). rank/world are small; negatives cannot
// reach it (RunFar validates first).
func farItoa(n int) string {
	if n == 0 {
		return "0"
	}
	digits := "0123456789"
	s := ""
	for n > 0 {
		d := n % 10
		s = digits[d:d+1] + s
		n = n / 10
	}
	return s
}

// RunFar is Run across process boundaries: `world` cooperating OS
// processes (ranks 0..world-1), each partitioning its own rank-local span,
// exchange halos over the far transport at rank boundaries. Interior lanes
// are wired exactly as Run wires them; only a rank's outermost lanes
// differ — their outward channels are bridged to NNG pair sockets by two
// pump goroutines per far edge (send-pump drains the cap-1 channel into
// far.SendF64; recv-pump feeds far.RecvF64 into the cap-1 channel), so
// Publish/HaloLeft/HaloRight/StencilStep bodies run unchanged and the M1
// cap-1 deadlock-freedom argument carries over (a send-pump's only job is
// draining the slot; far.SendF64 buffers without waiting on the remote).
// Global Dirichlet edges exist only at rank 0's left and rank world-1's
// right. Every rank must call RunFar with the SAME steps, world, urlBase,
// and per-rank lane count (equal-count is a documented contract; the
// bit-identity probes enforce it empirically).
//
// Teardown order (spec): quit send-pumps, far.Close sockets (unblocks
// recv-pumps with the "far: closed" error they string-match), then join
// all pumps. The BSP protocol is symmetric lockstep, so no message is in
// flight at join time; both pumps are idle-blocked when teardown starts.
//
// Failure model: setup errors and mid-run transport errors panic with
// explicit messages (a torn transport is unrecoverable for lockstep BSP;
// same process-fatal story as a panicking lane body).
func RunFar(p Partitioned, steps int, rank int, world int, urlBase string, body func(ctx *Lane)) []float64 {
	if world < 1 {
		panic("lanes.RunFar: world must be >= 1")
	}
	if rank < 0 {
		panic("lanes.RunFar: rank out of range")
	}
	if rank >= world {
		panic("lanes.RunFar: rank out of range")
	}
	if len(urlBase) == 0 {
		panic("lanes.RunFar: urlBase must be non-empty")
	}
	hasL := rank > 0
	hasR := rank < world-1

	// Every rank LISTENS on its right boundary and DIALS its left, so
	// each boundary URL has exactly one owner. far.Dial is async-retry
	// (NNG_FLAG_NONBLOCK), so process start order cannot deadlock setup.
	sockL := 0
	sockR := 0
	if hasR {
		ur := urlBase + ".halo." + farItoa(rank)
		sr := far.Listen(ur) catch e {
			panic("lanes.RunFar: listen " + ur + ": " + e.Error())
		}
		sockR = sr
	}
	if hasL {
		ul := urlBase + ".halo." + farItoa(rank-1)
		sl := far.Dial(ul) catch e {
			panic("lanes.RunFar: dial " + ul + ": " + e.Error())
		}
		sockL = sl
	}

	fc := farCfg{}
	fc.hasL = hasL
	fc.hasR = hasR
	quitL := make(chan int, 1)
	quitR := make(chan int, 1)
	pumpDone := make(chan int, 4)
	pumps := 0
	if hasL {
		sendL := make(chan float64, 1)
		recvL := make(chan float64, 1)
		fc.sendL = sendL
		fc.recvL = recvL
		go func() {
			for {
				select {
				case v := <-sendL:
					far.SendF64(sockL, v)
				case <-quitL:
					pumpDone <- 1
					return
				}
			}
		}()
		go func() {
			for {
				v := far.RecvF64(sockL) catch e {
					if e.Error() == "far: closed" {
						pumpDone <- 1
						return
					}
					panic("lanes.RunFar: far recv failed: " + e.Error())
				}
				recvL <- v
			}
		}()
		pumps = pumps + 2
	}
	if hasR {
		sendR := make(chan float64, 1)
		recvR := make(chan float64, 1)
		fc.sendR = sendR
		fc.recvR = recvR
		go func() {
			for {
				select {
				case v := <-sendR:
					far.SendF64(sockR, v)
				case <-quitR:
					pumpDone <- 1
					return
				}
			}
		}()
		go func() {
			for {
				v := far.RecvF64(sockR) catch e {
					if e.Error() == "far: closed" {
						pumpDone <- 1
						return
					}
					panic("lanes.RunFar: far recv failed: " + e.Error())
				}
				recvR <- v
			}
		}()
		pumps = pumps + 2
	}

	out := runCore(p, steps, body, fc)

	if hasL {
		quitL <- 1
	}
	if hasR {
		quitR <- 1
	}
	if hasL {
		far.Close(sockL)
	}
	if hasR {
		far.Close(sockR)
	}
	k := 0
	for k < pumps {
		<-pumpDone
		k = k + 1
	}
	return out
}
```

And add `import "far"` at the top of the file (its first import).

Dialect watchpoints while writing this: every binding on its own line; the
pump closures capture ordinary locals (sockL, sendL, quitL…), NOT loop
variables, so no rebind idiom is needed; if `digits[d:d+1]` (string slice of
a local) is rejected by the checker, replace farItoa's body with a
switch-free digit chain built from an if/else ladder over 0..9 returning
literal strings for the final digit — but try the slice form first and note
the outcome.

- [ ] **Step 5: Write the far-halo fixture**

`examples/far_halo_probe.goo`:

```go
// far_halo_probe (M2-B1 T4): 2 ranks x 2 lanes, radius-1 3-point kernel,
// 3 BSP rounds over NNG — every rank computes the FULL serial reference
// locally and compares its own span bit-for-bit, printing only booleans
// (no float printing in the contract). near mode is the same global
// problem on single-process Run with the same tiling (4 lanes of width
// 4), so near-vs-ref and far-vs-ref together pin far == near.
package main

import "fmt"
import "lanes"
import "os"
import "strconv"

func serialRef(n int, steps int) []float64 {
	cur := make([]float64, n)
	next := make([]float64, n)
	for i := 0; i < n; i++ {
		cur[i] = float64(i) * 0.5
	}
	for s := 0; s < steps; s++ {
		for i := 0; i < n; i++ {
			left := 0.0
			if i > 0 {
				left = cur[i-1]
			}
			right := 0.0
			if i < n-1 {
				right = cur[i+1]
			}
			acc := 0.25 * left
			acc = acc + 0.5*cur[i]
			acc = acc + 0.25*right
			next[i] = acc
		}
		for i := 0; i < n; i++ {
			cur[i] = next[i]
		}
	}
	return cur
}

func kernel(ctx *lanes.Lane) {
	own := ctx.Own()
	buf := make([]float64, len(own))
	for {
		ctx.Publish()
		hl := ctx.HaloLeft()
		hr := ctx.HaloRight()
		for i := 0; i < len(own); i++ {
			left := hl
			if i > 0 {
				left = own[i-1]
			}
			right := hr
			if i < len(own)-1 {
				right = own[i+1]
			}
			acc := 0.25 * left
			acc = acc + 0.5*own[i]
			acc = acc + 0.25*right
			buf[i] = acc
		}
		for i := 0; i < len(own); i++ {
			own[i] = buf[i]
		}
		if !ctx.Step() {
			break
		}
	}
}

func main() {
	args := os.Args
	mode := args[1]
	world := 2
	spanN := 8
	steps := 3
	n := world * spanN
	ref := serialRef(n, steps)

	if mode == "near" {
		arr := make([]float64, n)
		for i := 0; i < n; i++ {
			arr[i] = float64(i) * 0.5
		}
		p := lanes.Partition(arr, 4)
		out := lanes.Run(p, steps, kernel)
		ok := true
		for i := 0; i < n; i++ {
			if out[i] != ref[i] {
				ok = false
			}
		}
		fmt.Println(ok)
		return
	}

	rank := strconv.Atoi(args[2]) catch e {
		panic(e.Error())
	}
	urlBase := args[3]
	arr := make([]float64, spanN)
	base := rank * spanN
	for i := 0; i < spanN; i++ {
		arr[i] = float64(base+i) * 0.5
	}
	p := lanes.Partition(arr, 2)
	out := lanes.RunFar(p, steps, rank, world, urlBase, kernel)
	ok := true
	for i := 0; i < spanN; i++ {
		if out[i] != ref[base+i] {
			ok = false
		}
	}
	fmt.Println(ok)
}
```

- [ ] **Step 6: Write the launcher script**

`scripts/far-probe.sh`:

```bash
#!/usr/bin/env bash
# far-probe.sh (M2-B1): spawn a distributed lanes fixture as <world>
# processes over ipc:// in a temp dir, wait (30s timeout each — a teardown
# hang is a FAIL, not a stuck gate), and assert every rank's stdout is
# non-empty and all-"true" lines. Fixture arg contract:
#   <binary> far <rank> <urlBase> [extra-args...]
set -u
BIN="$1"
WORLD="$2"
shift 2
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
URL="ipc://$TMP/lane"
pids=()
for r in $(seq 0 $((WORLD - 1))); do
  timeout 30 "$BIN" far "$r" "$URL" "$@" > "$TMP/out.$r" 2> "$TMP/err.$r" &
  pids+=($!)
done
fail=0
for i in "${!pids[@]}"; do
  if ! wait "${pids[$i]}"; then
    echo "far-probe: rank $i FAILED (nonzero exit or 30s timeout)"
    sed "s/^/  rank$i stderr: /" "$TMP/err.$i"
    fail=1
  fi
done
if [ "$fail" -ne 0 ]; then exit 1; fi
for r in $(seq 0 $((WORLD - 1))); do
  if [ ! -s "$TMP/out.$r" ]; then
    echo "far-probe: rank $r produced no output"
    fail=1
    continue
  fi
  bad=$(grep -vx "true" "$TMP/out.$r")
  if [ -n "$bad" ]; then
    echo "far-probe: rank $r output not all-true:"
    sed "s/^/  rank$r: /" "$TMP/out.$r"
    fail=1
  fi
done
exit "$fail"
```

Run: `chmod +x scripts/far-probe.sh`

- [ ] **Step 7: Makefile probe target + verify wiring**

```makefile
# M2-B1: 2-rank NNG halo exchange, bit-identical to the in-fixture serial
# reference (near mode pins Run against the same reference first).
far-halo-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/far_halo_probe examples/far_halo_probe.goo
	@./build/far_halo_probe near > build/far_halo_near.txt
	@if ! grep -qx "true" build/far_halo_near.txt; then \
	  echo "far-halo-probe: FAIL (near mode diverged from serial reference)"; exit 1; fi
	@bash scripts/far-probe.sh build/far_halo_probe 2
	@echo "far-halo-probe: PASS (2-rank NNG halo exchange bit-identical)"
.PHONY: far-halo-probe
```

Append `far-halo-probe` to VERIFY_ALL_DEPS.

- [ ] **Step 8: Run everything**

Run: `make far-halo-probe`
Expected: `far-halo-probe: PASS ...`. Debug notes if it fails:
- A hang (timeout) with both ranks stuck → suspect pump wiring direction (sockL pumps must serve lane 0's sendL/recvL, sockR the LAST lane's sendR/recvR) or listen/dial URL mismatch (`halo.<rank>` listen vs `halo.<rank-1>` dial).
- `false` output with clean exit → arithmetic/tiling mismatch between kernel and serialRef; check the boundary `hl`/`hr` handling first.

Then: `make test && make test-golden && make verify-core`
Expected: all green.

- [ ] **Step 9: Commit**

```bash
git add goostd/lanes/lanes.go examples/far_halo_probe.goo scripts/far-probe.sh Makefile
git commit --no-gpg-sign -m "feat(lanes): RunFar — distributed halo exchange via far-transport pump goroutines (M2-B1 T4)"
```

---

### Task 5: Radius-2 stencil across ranks (sub-exchange over the wire)

**Files:**
- Create: `examples/far_stencil_r2_probe.goo`
- Modify: `Makefile` (`far-stencil-r2-probe` target + VERIFY_ALL_DEPS)

**Interfaces:**
- Consumes: `lanes.RunFar` (Task 4), `lanes.StencilStep(ctx, comptime radius, coeffs)` (existing, frozen), `scripts/far-probe.sh` (Task 4's arg contract).
- Produces: nothing new — pure evidence that the radius-r sequential sub-exchange protocol survives FIFO transport.

- [ ] **Step 1: Write the fixture**

`examples/far_stencil_r2_probe.goo`:

```go
// far_stencil_r2_probe (M2-B1 T5): radius-2 StencilStep across 2 ranks —
// each BSP round ships TWO sequential sub-exchanges per far edge, so this
// pins the k+1-gated-on-k sub-exchange ordering over the wire (FIFO
// end-to-end). Same boolean-only comparison discipline as far_halo_probe.
// Reference accumulates k-ascending, exactly StencilStep's pinned order.
package main

import "fmt"
import "lanes"
import "os"
import "strconv"

func serialRef(n int, steps int) []float64 {
	coeffs := []float64{0.05, 0.2, 0.5, 0.2, 0.05}
	cur := make([]float64, n)
	next := make([]float64, n)
	for i := 0; i < n; i++ {
		cur[i] = float64(i%5) * 1.25
	}
	for s := 0; s < steps; s++ {
		for i := 0; i < n; i++ {
			acc := 0.0
			for k := 0; k <= 4; k++ {
				idx := i + k - 2
				v := 0.0
				if idx >= 0 {
					if idx < n {
						v = cur[idx]
					}
				}
				acc = acc + coeffs[k]*v
			}
			next[i] = acc
		}
		for i := 0; i < n; i++ {
			cur[i] = next[i]
		}
	}
	return cur
}

func kernel(ctx *lanes.Lane) {
	coeffs := []float64{0.05, 0.2, 0.5, 0.2, 0.05}
	for {
		lanes.StencilStep(ctx, 2, coeffs)
		if !ctx.Step() {
			break
		}
	}
}

func main() {
	args := os.Args
	mode := args[1]
	world := 2
	spanN := 8
	steps := 3
	n := world * spanN
	ref := serialRef(n, steps)

	if mode == "near" {
		arr := make([]float64, n)
		for i := 0; i < n; i++ {
			arr[i] = float64(i%5) * 1.25
		}
		p := lanes.Partition(arr, 4)
		out := lanes.Run(p, steps, kernel)
		ok := true
		for i := 0; i < n; i++ {
			if out[i] != ref[i] {
				ok = false
			}
		}
		fmt.Println(ok)
		return
	}

	rank := strconv.Atoi(args[2]) catch e {
		panic(e.Error())
	}
	urlBase := args[3]
	arr := make([]float64, spanN)
	base := rank * spanN
	for i := 0; i < spanN; i++ {
		arr[i] = float64((base+i)%5) * 1.25
	}
	p := lanes.Partition(arr, 2)
	out := lanes.RunFar(p, steps, rank, world, urlBase, kernel)
	ok := true
	for i := 0; i < spanN; i++ {
		if out[i] != ref[base+i] {
			ok = false
		}
	}
	fmt.Println(ok)
}
```

- [ ] **Step 2: Probe target**

```makefile
# M2-B1: radius-2 sub-exchange protocol over the wire (2 floats per edge
# per round, order-gated) — bit-identical to the serial reference.
far-stencil-r2-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/far_stencil_r2 examples/far_stencil_r2_probe.goo
	@./build/far_stencil_r2 near > build/far_stencil_r2_near.txt
	@if ! grep -qx "true" build/far_stencil_r2_near.txt; then \
	  echo "far-stencil-r2-probe: FAIL (near mode diverged)"; exit 1; fi
	@bash scripts/far-probe.sh build/far_stencil_r2 2
	@echo "far-stencil-r2-probe: PASS (radius-2 sub-exchange survives the wire)"
.PHONY: far-stencil-r2-probe
```

Append `far-stencil-r2-probe` to VERIFY_ALL_DEPS.

- [ ] **Step 3: Run, then commit**

Run: `make far-stencil-r2-probe && make test`
Expected: PASS + green.

```bash
git add examples/far_stencil_r2_probe.goo Makefile
git commit --no-gpg-sign -m "test(lanes): pin radius-2 halo sub-exchange across ranks (M2-B1 T5)"
```

---

### Task 6: Cross-rank collectives (bit-identical AllReduce)

**Files:**
- Modify: `goostd/lanes/lanes.go` (Lane fields, farCfg fields, runCore stamping, coll socket wiring + teardown in RunFar, `farRecvMust`, allReduce far branch)
- Create: `examples/far_collective_probe.goo`
- Modify: `Makefile` (`far-collective-probe` target + VERIFY_ALL_DEPS)

**Interfaces:**
- Consumes: Tasks 3–4 surfaces.
- Produces:
  - New additive `Lane` fields: `isFar bool; rank int; world int; collSock int; collSocks []int`
  - New `farCfg` fields with the same names; `runCore` copies them onto every Lane.
  - Coll socket naming: `<urlBase>.coll.<r>` — rank 0 LISTENS (one socket per r=1..world-1, held in `collSocks[r]`), rank r DIALS (held in `collSock`).
  - `AllReduceSum`/`AllReduceMax` become world-global under RunFar with NO signature change.

- [ ] **Step 1: Extend farCfg, Lane, runCore, RunFar**

In `goostd/lanes/lanes.go`:

Add to `farCfg` (below its existing fields):

```go
	isFar     bool
	rank      int
	world     int
	collSock  int
	collSocks []int
```

Add to `Lane` (below the M2-B2 field block, additive — existing shapes untouched):

```go
	// M2-B1: cross-rank collective wiring (zero-valued for in-process
	// Run). rank>0's lane 0 forwards RAW per-lane partials in local-ID
	// order over collSock; rank 0's lane 0 flat-combines in GLOBAL lane-ID
	// order over collSocks (never pre-combined per rank — float addition
	// is not associative, and bit-identity with the in-process scan
	// requires the identical accumulation sequence).
	isFar     bool
	rank      int
	world     int
	collSock  int
	collSocks []int
```

In `runCore`'s Lane construction, add after `scratch: ...`:

```go
				isFar:     fc.isFar,
				rank:      fc.rank,
				world:     fc.world,
				collSock:  fc.collSock,
				collSocks: fc.collSocks,
```

In `RunFar`, after the halo socket setup (before the pump block), add:

```go
	fc.isFar = world > 1
	fc.rank = rank
	fc.world = world
	if world > 1 {
		if rank == 0 {
			collSocks := make([]int, world)
			for r := 1; r < world; r++ {
				uc := urlBase + ".coll." + farItoa(r)
				sc := far.Listen(uc) catch e {
					panic("lanes.RunFar: listen " + uc + ": " + e.Error())
				}
				collSocks[r] = sc
			}
			fc.collSocks = collSocks
		}
		if rank > 0 {
			uc := urlBase + ".coll." + farItoa(rank)
			sc := far.Dial(uc) catch e {
				panic("lanes.RunFar: dial " + uc + ": " + e.Error())
			}
			fc.collSock = sc
		}
	}
```

And in RunFar's teardown, after the pump join loop (collectives make direct
blocking calls from lane goroutines — no pumps to quit; by join time no
collective is in flight):

```go
	if world > 1 {
		if rank == 0 {
			for r := 1; r < world; r++ {
				far.Close(fc.collSocks[r])
			}
		}
		if rank > 0 {
			far.Close(fc.collSock)
		}
	}
```

- [ ] **Step 2: The far allReduce branch**

Add the helper above `allReduce`:

```go
// farRecvMust: collective receives happen mid-protocol, where any
// transport error is unrecoverable — panic loudly, never hang silently.
func farRecvMust(sock int) float64 {
	v := far.RecvF64(sock) catch e {
		panic("lanes.allReduce: far recv failed: " + e.Error())
	}
	return v
}
```

Rewrite `allReduce`'s `if l.id == 0 { ... }` block to branch on `l.isFar`
(the existing in-process path stays byte-identical in its branch; combine
loops shown in full):

```go
func (l *Lane) allReduce(local float64, useMax bool) float64 {
	l.partials[l.id] <- local
	if l.id == 0 {
		if !l.isFar {
			acc := <-l.partials[0]
			for i := 1; i < l.count; i++ {
				v := <-l.partials[i]
				if useMax {
					if v > acc {
						acc = v
					}
				} else {
					acc = acc + v
				}
			}
			for i := 1; i < l.count; i++ {
				l.results[i] <- acc
			}
			return acc
		}
		if l.rank == 0 {
			// GLOBAL flat combine, lane-ID order: rank 0's own lanes
			// first, then rank 1's raw partials, then rank 2's, ... —
			// instruction-for-instruction the in-process scan's sequence
			// for the same total lane count (bit-identity contract).
			acc := <-l.partials[0]
			for i := 1; i < l.count; i++ {
				v := <-l.partials[i]
				if useMax {
					if v > acc {
						acc = v
					}
				} else {
					acc = acc + v
				}
			}
			for r := 1; r < l.world; r++ {
				for i := 0; i < l.count; i++ {
					v := farRecvMust(l.collSocks[r])
					if useMax {
						if v > acc {
							acc = v
						}
					} else {
						acc = acc + v
					}
				}
			}
			for r := 1; r < l.world; r++ {
				far.SendF64(l.collSocks[r], acc)
			}
			for i := 1; i < l.count; i++ {
				l.results[i] <- acc
			}
			return acc
		}
		// rank > 0's lane 0: forward RAW partials in local-ID order
		// (never pre-combined — see the Lane field block's comment),
		// then wait for the global result and distribute it locally.
		for i := 0; i < l.count; i++ {
			v := <-l.partials[i]
			far.SendF64(l.collSock, v)
		}
		acc := farRecvMust(l.collSock)
		for i := 1; i < l.count; i++ {
			l.results[i] <- acc
		}
		return acc
	}
	r := <-l.results[l.id]
	return r
}
```

(Equal-count-per-rank is why rank 0 can loop `l.count` per remote rank —
already a documented RunFar contract from Task 4.)

- [ ] **Step 3: Write the collective fixture**

`examples/far_collective_probe.goo`:

```go
// far_collective_probe (M2-B1 T6): distributed AllReduceSum/Max vs the
// in-fixture ID-order serial scan, on NON-ASSOCIATIVE data (1e16 absorbs
// 1.0): any combine-order deviation — arrival-order, per-rank
// pre-combining — lands on different bits and prints false. 2 ranks x 2
// lanes; global lane g = rank*2 + local id contributes contrib[g].
package main

import "fmt"
import "lanes"
import "os"
import "strconv"

func main() {
	args := os.Args
	mode := args[1]
	contrib := []float64{1e16, 1.0, -1e16, 1.0}
	sumRef := contrib[0]
	sumRef = sumRef + contrib[1]
	sumRef = sumRef + contrib[2]
	sumRef = sumRef + contrib[3]
	maxRef := contrib[0]
	for i := 1; i < 4; i++ {
		if contrib[i] > maxRef {
			maxRef = contrib[i]
		}
	}

	if mode == "near" {
		arr := []float64{0.0, 0.0, 0.0, 0.0}
		got := []float64{0.0, 0.0, 0.0, 0.0}
		gotMax := []float64{0.0, 0.0, 0.0, 0.0}
		p := lanes.Partition(arr, 4)
		lanes.Run(p, 1, func(ctx *lanes.Lane) {
			s := ctx.AllReduceSum(contrib[ctx.ID()])
			m := ctx.AllReduceMax(contrib[ctx.ID()])
			got[ctx.ID()] = s
			gotMax[ctx.ID()] = m
		})
		ok := true
		for i := 0; i < 4; i++ {
			if got[i] != sumRef {
				ok = false
			}
			if gotMax[i] != maxRef {
				ok = false
			}
		}
		fmt.Println(ok)
		return
	}

	rank := strconv.Atoi(args[2]) catch e {
		panic(e.Error())
	}
	urlBase := args[3]
	arr := []float64{0.0, 0.0}
	got := []float64{0.0, 0.0}
	gotMax := []float64{0.0, 0.0}
	base := rank * 2
	p := lanes.Partition(arr, 2)
	lanes.RunFar(p, 1, rank, 2, urlBase, func(ctx *lanes.Lane) {
		s := ctx.AllReduceSum(contrib[base+ctx.ID()])
		m := ctx.AllReduceMax(contrib[base+ctx.ID()])
		got[ctx.ID()] = s
		gotMax[ctx.ID()] = m
	})
	ok := true
	for i := 0; i < 2; i++ {
		if got[i] != sumRef {
			ok = false
		}
		if gotMax[i] != maxRef {
			ok = false
		}
	}
	fmt.Println(ok)
}
```

- [ ] **Step 4: Probe target**

```makefile
# M2-B1: cross-rank AllReduce bit-identity on non-associative data —
# any combine-order deviation (arrival order, per-rank pre-combine)
# flips the booleans.
far-collective-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/far_collective examples/far_collective_probe.goo
	@./build/far_collective near > build/far_collective_near.txt
	@if ! grep -qx "true" build/far_collective_near.txt; then \
	  echo "far-collective-probe: FAIL (near mode diverged)"; exit 1; fi
	@bash scripts/far-probe.sh build/far_collective 2
	@echo "far-collective-probe: PASS (global ID-order combine, bit-identical)"
.PHONY: far-collective-probe
```

Append `far-collective-probe` to VERIFY_ALL_DEPS.

- [ ] **Step 5: Run all lanes gates, then commit**

Run: `make far-collective-probe && make far-halo-probe && make far-stencil-r2-probe && make test && make test-golden`
Expected: all green (the allReduce rewrite must not disturb the in-process
collective probes — `lanes_allreduce_probe` and the Jacobi golden pin the
`!l.isFar` branch).

```bash
git add goostd/lanes/lanes.go examples/far_collective_probe.goo Makefile
git commit --no-gpg-sign -m "feat(lanes): cross-rank AllReduce — raw-partial forwarding, global ID-order combine (M2-B1 T6)"
```

---

### Task 7: Distributed Jacobi capstone + docs + full gate

**Files:**
- Create: `examples/far_jacobi_probe.goo`
- Modify: `Makefile` (`far-jacobi-probe` target, runs the world TWICE + VERIFY_ALL_DEPS)
- Modify: `docs/lanes.md` (new `## M2-B1: far transport` section)
- Modify: `docs/2026-07-08-v1-roadmap.md` (Phase 6 far-transport bullet: landed)
- Modify: `.handoff.md` (B1 done; #144 MERGED / #142 CLOSED disposition; Task-0 spike finding)

**Interfaces:**
- Consumes: everything above.
- Produces: the milestone's flagship evidence — the M2-B2 Jacobi convergence loop (kernel + collective) distributed 2×2, same final field AND same iteration count as the serial tiled reference, twice.

- [ ] **Step 1: Write the capstone fixture**

`examples/far_jacobi_probe.goo` — the M2-B2 capstone
(`examples/lanes_jacobi_probe.goo`) adapted to ranks. Global problem
n=8, a=[10,0,0,0,0,0,0,10], 4 tiles of width 2, tol=0.05, maxRounds=64 —
identical numbers, so the reference logic is copied VERBATIM from that file
(same accumulation order everywhere):

```go
// far_jacobi_probe (M2-B1 T7, capstone): the M2-B2 Jacobi convergence
// loop — StencilStep + AllReduceMax until the max-norm delta falls under
// tol — distributed 2 ranks x 2 lanes. Same global problem and tiling as
// lanes_jacobi_probe.goo (n=8, 4 tiles of width 2), so the serial tiled
// reference is copied VERBATIM from there; every rank recomputes it
// locally and pins: (1) its span of the final field bit-for-bit, (2) its
// lanes' round counts agree with each other and with refRounds — the
// convergence DECISION itself crossed the wire correctly (one ULP of
// drift anywhere flips the iteration count).
package main

import "fmt"
import "lanes"
import "os"
import "strconv"

func main() {
	args := os.Args
	n := 8
	coeffs := []float64{0.25, 0.5, 0.25}
	tol := 0.05
	maxRounds := 64

	// Serial tiled reference — verbatim from lanes_jacobi_probe.goo.
	r := []float64{10.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 10.0}
	prev := []float64{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}
	refRounds := 0
	for round := 0; round < maxRounds; round++ {
		for i := 0; i < n; i++ {
			prev[i] = r[i]
		}
		for i := 0; i < n; i++ {
			left := 0.0
			if i > 0 {
				left = prev[i-1]
			}
			right := 0.0
			if i < n-1 {
				right = prev[i+1]
			}
			acc := 0.25 * left
			acc = acc + 0.5*prev[i]
			acc = acc + 0.25*right
			r[i] = acc
		}
		delta := 0.0
		for t := 0; t < 4; t++ {
			local := 0.0
			for i := t * 2; i < (t+1)*2; i++ {
				d := r[i] - prev[i]
				if d < 0.0 {
					d = 0.0 - d
				}
				if d > local {
					local = d
				}
			}
			if local > delta {
				delta = local
			}
		}
		refRounds = refRounds + 1
		if delta < tol {
			break
		}
	}

	rank := strconv.Atoi(args[2]) catch e {
		panic(e.Error())
	}
	urlBase := args[3]

	// Rank-local span of the global initial field [10,0,...,0,10].
	a := []float64{0.0, 0.0, 0.0, 0.0}
	if rank == 0 {
		a[0] = 10.0
	}
	if rank == 1 {
		a[3] = 10.0
	}
	rounds := []float64{0.0, 0.0}
	p := lanes.Partition(a, 2)
	res := lanes.RunFar(p, maxRounds, rank, 2, urlBase, func(ctx *lanes.Lane) {
		own := ctx.Own()
		mine := 0.0
		before := []float64{0.0, 0.0}
		for {
			for i := 0; i < len(own); i++ {
				before[i] = own[i]
			}
			lanes.StencilStep(ctx, 1, coeffs)
			local := 0.0
			for i := 0; i < len(own); i++ {
				d := own[i] - before[i]
				if d < 0.0 {
					d = 0.0 - d
				}
				if d > local {
					local = d
				}
			}
			delta := ctx.AllReduceMax(local)
			mine = mine + 1.0
			if delta < tol {
				break
			}
			if !ctx.Step() {
				break
			}
		}
		rounds[ctx.ID()] = mine
	})

	base := rank * 4
	valsOK := true
	for i := 0; i < 4; i++ {
		if res[i] != r[base+i] {
			valsOK = false
		}
	}
	fmt.Println(valsOK)
	fmt.Println(rounds[0] == rounds[1])
	fmt.Println(rounds[0] == float64(refRounds))
	fmt.Println(refRounds > 1)
}
```

(No near mode needed: `lanes_jacobi_probe` already gates the in-process
run against this exact reference; this fixture gates the distributed run
against the same reference — transitively far == near.)

- [ ] **Step 2: Probe target — run the world twice (determinism repeat)**

```makefile
# M2-B1 capstone: distributed Jacobi convergence — final field AND
# iteration count bit-identical to the serial tiled reference, run TWICE
# (schedule-independence repeat).
far-jacobi-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/far_jacobi examples/far_jacobi_probe.goo
	@bash scripts/far-probe.sh build/far_jacobi 2
	@bash scripts/far-probe.sh build/far_jacobi 2
	@echo "far-jacobi-probe: PASS (distributed convergence, twice, bit-identical)"
.PHONY: far-jacobi-probe
```

Append `far-jacobi-probe` to VERIFY_ALL_DEPS.

Run: `make far-jacobi-probe`
Expected: PASS.

- [ ] **Step 3: Documentation**

1. `docs/lanes.md` — append a `## M2-B1: far transport` section AFTER the
   M2 sections, covering (write it from the spec + what shipped, in the
   file's existing voice): the RunFar contract (signature, equal
   per-rank lane count, same steps/world/urlBase everywhere, global
   Dirichlet only at world edges), the socket topology and pump
   architecture (invisible to lane bodies; FIFO end-to-end; cap-1
   deadlock argument extension), the collective bit-identity protocol
   (raw-partial forwarding, global ID-order flat combine), the teardown
   order and failure model ("far: closed" vs panic), the envelope
   (single-machine multi-process proven; per-rank lane count <= 128 per
   the FAR_BUF_DEPTH comment; multi-machine is a documented future
   runbook, wire format 8-byte LE IEEE-754), and the gates table (the
   five far probes + C unit test).
2. `docs/2026-07-08-v1-roadmap.md` — in Phase 6, update the
   "Distributed boundary ('halo') exchange…" bullet with a landed note:
   NNG 1.12.0 vendored, transport-agnostic ops vtable in
   `src/runtime/far_transport.c`, `lanes.RunFar` + cross-rank
   collectives probe-gated in verify-core (M2-B1, this date). Keep the
   original text (it records the decision trail); append, don't rewrite.
3. `.handoff.md` — under the burndown/backlog: mark the Phase 6 M2 item
   updated (B2 done PR #209, B1 done this arc); record "#144 MERGED /
   #142 CLOSED (superseded by arc-18) — disposition item retired"; add
   one line for the Task-0 spike finding (vendored→shim import works —
   pinned by every far probe via lanes.go's `import "far"`).

- [ ] **Step 4: The full gate**

Run: `make test && make test-golden && make test-golden-o2 && make verify-core && ./scripts/grammar-tripwire.sh`
Expected: ALL green, including all five far probes inside verify-core, and
tripwire at baseline. Read the actual output — no piped status.

- [ ] **Step 5: Commit**

```bash
git add examples/far_jacobi_probe.goo Makefile docs/lanes.md docs/2026-07-08-v1-roadmap.md .handoff.md
git commit --no-gpg-sign -m "test,docs(lanes): distributed Jacobi capstone + M2-B1 docs and handoff (M2-B1 T7)"
```

---

## Plan Self-Review (done at write time)

- **Spec coverage:** envelope (multi-process localhost — far-probe.sh, ipc://), remote-backed channels (runCore farCfg seam), rank-local spans (fixtures), collectives (Task 6), vendored 1.x (Task 1, v1.12.0), vtable (Task 2), pumps + select/quit + "far: closed" split (Tasks 2/4), Task-0 spike + fallback stop-rule (Task 0), all eight gate rows (C unit test + ASan Task 2; halo T4; r2 T5; collective T6; jacobi + repeat T7; timeout wrapper in far-probe.sh; spike permanence via lanes.go import), teardown discipline (T4 Step 4), docs + ledger note (T7). Valgrind: covered by ASan target instead — deviation recorded here deliberately (ASan catches the same class for the C unit's scope; a valgrind far-run rides the existing arena-valgrind pattern post-merge if wanted).
- **Placeholder scan:** clean — every code step carries the code; the two "mirror the local idiom" notes (runtime_integration declarations, expression-arg generation) name the exact neighboring symbol to copy and the exact prototype to match, which is a lookup, not a design gap.
- **Type consistency:** `farCfg` fields (T4 def / T6 extension), `far.*` signatures (T3 Produces = T4/T6 call sites), `goo_far_*` C signatures (T2 Produces = T3 declarations), error spellings (`"far: closed"` in T2 test, T2 impl, T4 pumps, T6 farRecvMust panic prefix distinct), probe arg contract (`far <rank> <urlBase>` in far-probe.sh = all four fixtures) — checked.
