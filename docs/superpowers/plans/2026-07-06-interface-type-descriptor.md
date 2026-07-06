# Interface Type Descriptor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `interface{}` values printable — `fmt.Println(x)`, `%v`/`%s` of interface args in `Printf`/`Sprintf` — and give type-assertion panics the dynamic type name, via a per-concrete-type descriptor reached behind the interface vtable's slot 0.

**Architecture:** Reinterpret interface vtable **slot 0** (today the per-type equality fn) as a pointer to a per-concrete-type descriptor `{ eq_fn, type_name, fmt_fn }` (eq_fn first, so the runtime hop is one extra deref). `fmt_fn(ptr data) -> goo_string` returns a value's `%v` representation; `Println`/`Sprintf` route interface args through it. The 2-word `{vtable,data}` box is unchanged; method slots 1..n are unchanged (no GEP renumbering).

**Tech Stack:** C23, LLVM-C 22 IR builder, the repo's golden-probe harness (`examples/<name>.goo` + `.expected.txt`), `make verify`/`make test`.

## Global Constraints

- TDD: write the failing golden probe / gate first, watch it fail, then implement. (from CLAUDE.md)
- Every task ends green on: `bash scripts/run_golden.sh` (0 failures), `make test` (76 pass / 1 skip), `./scripts/grammar-tripwire.sh` (**82 S/R + 256 R/R** — no grammar change in this plan), and `make verify` ALL GREEN (ccomp-link included; run `eval "$(opam env --switch=default)"` first so `ccomp` is on PATH).
- No change to the interface box width (stays `{ptr vtable, ptr data}`) and no change to method vtable slot indices (methods stay 1..n).
- Golden probe outputs must be byte-identical to upstream Go (`go run`) for the same program.
- No naked returns; explicit error handling; comments explain *why*. (from CLAUDE.md Go/house style — applies to the C here in spirit: no silent failures.)
- Build the compiler with `make lexer` (incremental; header edits rebuild dependents). Binary is `bin/goo`.
- Descriptor + `fmt_fn` are emitted **on demand and name-deduped by concrete type** exactly like `goo.vtable.<T>.<I>` / `codegen_get_or_emit_type_eq`.

**Reference implementations to mirror (read these before starting):**
- `src/codegen/interface_codegen.c` — `codegen_interface_vtable` (~line 205, slot array build), `build_thunk` (~line 55, per-type thunk template), `codegen_get_or_emit_type_eq` reference at `src/codegen/codegen.c:880`.
- `src/codegen/call_codegen.c` — Println arg loop (~line 1966), `fmt_emit_segments` (~line 2186), `%v` verb handling, the Sprintf to-string helpers block (~line 2222).
- `src/runtime/runtime.c` — `goo_iface_key_eq` (~line 603), `goo_int_to_string` (~line 314), `goo_bool_to_string` (~line 352).
- `src/codegen/runtime_integration.c` — `add_runtime_function` registrations (~line 350 for `goo_iface_key_eq`).

---

### Task 1: Descriptor global + reinterpret vtable slot-0 (refactor, regression-gated)

Introduce the descriptor and repoint slot 0 at it, preserving map-key equality behavior exactly. No new user-visible behavior — this is the load-bearing refactor; its gate is the interface regression suite staying green. `fmt_fn` is emitted as a null pointer for now (Task 2 fills it).

**Files:**
- Modify: `src/codegen/interface_codegen.c` — add `codegen_get_or_emit_type_desc`; change `codegen_interface_vtable` slot[0].
- Modify: `include/codegen.h` — declare `codegen_get_or_emit_type_desc`.
- Modify: `src/runtime/runtime.c` — `goo_iface_key_eq` hops through the descriptor.

**Interfaces:**
- Consumes: `codegen_get_or_emit_type_eq(codegen, checker, concrete)` (existing, `codegen.c:880`), `iface_ptr_eq_fn(codegen)` (existing, `interface_codegen.c:186`), `iface_ptr_type(codegen)` (existing).
- Produces:
  - `LLVMValueRef codegen_get_or_emit_type_desc(CodeGenerator* codegen, TypeChecker* checker, Type* concrete, int pointer_form)` — returns the private-constant global `goo.typedesc.<T>` (or `goo.typedesc.$ptr$<T>` for pointer_form), a `{ ptr eq_fn, ptr type_name, ptr fmt_fn }`. Name-deduped; returns the existing global if already emitted. `fmt_fn` field initialized to a null `ptr` in this task.
  - Descriptor field order is FIXED here: index 0 = eq_fn, 1 = type_name (C string, e.g. `"int"`, `"Point"`, `"*Point"`), 2 = fmt_fn.

- [ ] **Step 1: Add the descriptor emitter (fmt_fn = null for now)**

In `src/codegen/interface_codegen.c`, add before `codegen_interface_vtable`. Mirror the name-dedup + private-constant-global pattern already used for vtables:

```c
// Per-concrete-type descriptor reached behind interface vtable slot 0
// (Go's itab->_type shape). Layout: { ptr eq_fn, ptr type_name, ptr fmt_fn }.
// eq_fn is FIRST so goo_iface_key_eq's slot-0 hop is a single extra deref.
// fmt_fn is null here; codegen_get_or_emit_type_fmt (Task 2) fills it.
// Name-deduped by concrete type, like the vtable globals.
LLVMValueRef codegen_get_or_emit_type_desc(CodeGenerator* codegen, TypeChecker* checker,
                                           Type* concrete, int pointer_form) {
    char gname[256];
    const char* cname = type_receiver_name(concrete);
    if (!cname) cname = type_to_string(concrete);
    if (pointer_form) snprintf(gname, sizeof(gname), "goo.typedesc.$ptr$%s", cname);
    else              snprintf(gname, sizeof(gname), "goo.typedesc.%s", cname);
    LLVMValueRef existing = LLVMGetNamedGlobal(codegen->module, gname);
    if (existing) return existing;

    LLVMTypeRef ptrty = iface_ptr_type(codegen);

    // eq_fn: pointer-boxed form uses pointer identity; value form uses the
    // per-type value comparator — same choice codegen_interface_vtable made.
    LLVMValueRef eq_fn = pointer_form
        ? iface_ptr_eq_fn(codegen)
        : codegen_get_or_emit_type_eq(codegen, checker, concrete);
    if (!eq_fn) return NULL;

    // type_name: a private constant C string. For the pointer form, prefix '*'.
    char tname[256];
    if (pointer_form) snprintf(tname, sizeof(tname), "*%s", cname);
    else              snprintf(tname, sizeof(tname), "%s", cname);
    LLVMValueRef name_str = LLVMBuildGlobalStringPtr(codegen->builder, tname, "typename");
    // NOTE: LLVMBuildGlobalStringPtr needs an insertion point. The descriptor
    // is always emitted while boxing (inside a function), so builder has one.
    // If a builder-free path ever calls this, switch to a module-level constant
    // string global (see codegen_const_string_value in composite_codegen.c).

    LLVMValueRef null_fmt = LLVMConstNull(ptrty);

    LLVMValueRef fields[3] = { eq_fn, name_str, null_fmt };
    LLVMTypeRef descty = LLVMStructType((LLVMTypeRef[]){ptrty, ptrty, ptrty}, 3, 0);
    LLVMValueRef init = LLVMConstNamedStruct(descty, fields, 3);
    LLVMValueRef g = LLVMAddGlobal(codegen->module, descty, gname);
    LLVMSetInitializer(g, init);
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetGlobalConstant(g, 1);
    return g;
}
```

Declare it in `include/codegen.h` next to the interface codegen decls:

```c
LLVMValueRef codegen_get_or_emit_type_desc(CodeGenerator* codegen, TypeChecker* checker,
                                           Type* concrete, int pointer_form);
```

> If `name_str` (a `GlobalStringPtr`) cannot be an initializer of a private
> global in this LLVM build (it is itself a global pointer — valid as a
> constant initializer), keep as-is. If the verifier objects, make `name_str`
> a module-level private constant array global and store its `i8*` — the
> `codegen_const_string_value` helper in `composite_codegen.c` does exactly this
> builder-free.

- [ ] **Step 2: Point vtable slot-0 at the descriptor**

In `src/codegen/interface_codegen.c`, `codegen_interface_vtable`, replace the `slots[0] = eq_fn;` assignment (~line 253) with the descriptor pointer. The `eq_fn` computation right above it moves into the descriptor emitter, so delete the now-redundant `eq_fn` local here and use:

```c
    // slot-0 is now the type descriptor pointer (eq fn lives inside it).
    LLVMValueRef desc = codegen_get_or_emit_type_desc(codegen, checker, concrete, pointer_form);
    if (!desc) { free(slots); return NULL; }
    slots[0] = desc;
```

Leave the method-thunk loop (`slots[i + 1] = thunk`) and everything else unchanged.

- [ ] **Step 3: Update `goo_iface_key_eq` to hop through the descriptor**

In `src/runtime/runtime.c` (~line 603), the current body loads `((void**)vta)[0]` as the eq fn. Change it to load the descriptor then its first field:

```c
int goo_iface_key_eq(int64_t a, int64_t b) {
    // a, b are the interface data words; the caller-side compares vtable
    // identity first. vta below is the vtable of one operand.
    // slot 0 is now the type descriptor; the descriptor's field 0 is eq_fn.
    void* vta = /* existing: however vta is obtained today */;
    void* desc = ((void**)vta)[0];          // vtable slot 0 -> descriptor
    GooKeyEqFn eq = (GooKeyEqFn)((void**)desc)[0];  // descriptor field 0 -> eq_fn
    return eq(a, b);
}
```

Keep the surrounding logic (how `vta` is derived from the operands) exactly as it is — only the two lines that fetch `eq` change (one extra deref).

- [ ] **Step 4: Build and run the interface regression gate (the RED→GREEN for a refactor is "stays green")**

```bash
make lexer 2>&1 | grep -iE "error:" ; echo "build ok if no error lines"
bash scripts/run_golden.sh 2>&1 | tail -3
```
Expected: golden **270/0** (unchanged). If any interface-map-key/dispatch/assertion probe regresses, slot-0 wiring is wrong — fix before proceeding.

- [ ] **Step 5: Full gate + commit**

```bash
eval "$(opam env --switch=default)"
make test 2>&1 | tail -3          # 76 pass / 1 skip
./scripts/grammar-tripwire.sh      # PASS 82/256
make verify 2>&1 | tail -3         # ALL GREEN
git add src/codegen/interface_codegen.c include/codegen.h src/runtime/runtime.c
git commit -m "refactor(codegen): interface vtable slot-0 -> per-type descriptor {eq,name,fmt}

Reinterpret vtable slot 0 as a pointer to goo.typedesc.<T> (eq_fn first,
so goo_iface_key_eq gains one deref). Method slots 1..n unchanged. Behavior
preserved: full interface map-key/dispatch/assertion suite green. fmt_fn is
null until the next commit fills it. Foundation for fmt.Println(any) + %v +
dynamic panic type-names.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: `fmt_fn` scalar thunk + `goo_uint_to_string` + `fmt.Println(interface{})`

Fill the descriptor's `fmt_fn` with a per-type thunk that returns a value's `%v` string, add the missing unsigned formatter, and lower `fmt.Println`/`fmt.Print` of an interface argument through it.

**Files:**
- Create: `examples/iface_print_scalars.goo` + `.expected.txt`
- Create: `examples/iface_print_typeswitch.goo` + `.expected.txt`
- Modify: `src/runtime/runtime.c` — add `goo_uint_to_string`.
- Modify: `include/runtime.h` — declare `goo_uint_to_string`.
- Modify: `src/codegen/runtime_integration.c` — register `goo_uint_to_string`.
- Modify: `src/codegen/interface_codegen.c` — add `codegen_get_or_emit_type_fmt`; wire it into `codegen_get_or_emit_type_desc` (replace the null fmt field).
- Modify: `include/codegen.h` — declare `codegen_get_or_emit_type_fmt`.
- Modify: `src/codegen/call_codegen.c` — Println arg loop `TYPE_INTERFACE` case.

**Interfaces:**
- Consumes: `codegen_get_or_emit_type_desc` (Task 1); `goo_int_to_string(int64_t)`, `goo_float_to_string(double)`, `goo_bool_to_string(int)` (existing, `runtime.h:86-88`); `goo_string_new_with_length` / string layout.
- Produces:
  - `goo_string_t goo_uint_to_string(uint64_t value)` (runtime).
  - `LLVMValueRef codegen_get_or_emit_type_fmt(CodeGenerator* codegen, TypeChecker* checker, Type* concrete, int pointer_form)` — returns the function `goo.fmt.<T>` of LLVM type `goo_string (ptr data)`. Loads the concrete from `data` and returns its `%v` string. v1 scalar kinds: int/uint widths, bool, float32/64, string. Non-scalar / pointer_form: returns a `goo_string` copy of the descriptor `type_name` (bounded fallback).

- [ ] **Step 1: Write the failing golden probe**

`examples/iface_print_scalars.goo`:

```go
package main

import "fmt"

func main() {
	var i interface{} = 42
	var s interface{} = "hi"
	var b interface{} = true
	var f interface{} = 3.5
	var n interface{}
	fmt.Println(i)
	fmt.Println(s)
	fmt.Println(b)
	fmt.Println(f)
	fmt.Println(n)
}
```

`examples/iface_print_scalars.expected.txt` (verify with `go run` on the same source):

```
42
hi
true
3.5
<nil>
```

`examples/iface_print_typeswitch.goo`:

```go
package main

import "fmt"

func describe(x interface{}) {
	switch v := x.(type) {
	case int, string:
		fmt.Println(v)
	default:
		fmt.Println("other")
	}
}

func main() {
	describe(7)
	describe("go")
	describe(true)
}
```

`examples/iface_print_typeswitch.expected.txt`:

```
7
go
other
```

- [ ] **Step 2: Run to confirm they fail for the right reason**

```bash
./bin/goo examples/iface_print_scalars.goo -o /tmp/ips 2>&1 | head -3
```
Expected: `fmt.Println: unsupported argument type (only string, integer, bool, and float are supported in v1)`.

- [ ] **Step 3: Add `goo_uint_to_string`**

In `src/runtime/runtime.c` after `goo_int_to_string` (~line 323), mirroring it with `%llu`:

```c
goo_string_t goo_uint_to_string(uint64_t value) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        goo_panic("goo_uint_to_string: snprintf overflow");
    }
    return goo_string_new_with_length(buf, (size_t)len);
}
```

Declare in `include/runtime.h` next to line 86:

```c
goo_string_t goo_uint_to_string(uint64_t value);
```

Register in `src/codegen/runtime_integration.c` alongside the other to-string helpers (find where `goo_int_to_string` is registered and add a sibling: return type = the `goo_string_t` LLVM struct, one `i64` param).

- [ ] **Step 4: Add the `fmt_fn` thunk emitter**

In `src/codegen/interface_codegen.c`, add `codegen_get_or_emit_type_fmt`. It creates a function `goo.fmt.<T>` (name-deduped) of type `goo_string(ptr)`, and in its body loads the concrete value from the `data` param and calls the matching `goo_*_to_string` runtime fn, returning its result. Structure (mirror `build_thunk`'s function creation + `LLVMPositionBuilderAtEnd` pattern; use `codegen->builder` saved/restored):

```c
// Per-type %v formatter reached via the descriptor's fmt_fn field.
// goo_string goo.fmt.<T>(ptr data): load T from data, return its %v string.
// v1: scalar kinds. Non-scalar / pointer_form -> copy of type_name.
LLVMValueRef codegen_get_or_emit_type_fmt(CodeGenerator* codegen, TypeChecker* checker,
                                          Type* concrete, int pointer_form) {
    // 1. Compute name goo.fmt.<T> / goo.fmt.$ptr$<T>; return existing if present.
    // 2. Create function: goo_string (ptr). Save codegen->builder + current block.
    // 3. entry block; data = param 0.
    // 4. Dispatch on concrete->kind (value form):
    //    - TYPE_INT8..INT64:  load iN, SExt to i64, call goo_int_to_string.
    //    - TYPE_UINT8..UINT64/BYTE: load, ZExt to i64, call goo_uint_to_string.
    //    - TYPE_BOOL:  load i1/i8, ZExt to i32, call goo_bool_to_string.
    //    - TYPE_FLOAT32: load, FPExt to double, call goo_float_to_string.
    //    - TYPE_FLOAT64: load, call goo_float_to_string.
    //    - TYPE_STRING:  load the goo_string value from data, return a copy
    //                    (goo_string_new_with_length(data,len)) — data is a
    //                    heap goo_string; return it (or a copy) directly.
    //    - pointer_form or any other kind: build a goo_string from the
    //      descriptor's type_name (fallback) — use goo_string_new(<tname cstr>).
    // 5. LLVMBuildRet the goo_string. Restore builder to the saved block.
}
```

Then in `codegen_get_or_emit_type_desc` (Task 1), replace `LLVMValueRef null_fmt = LLVMConstNull(ptrty);` with:

```c
    LLVMValueRef fmt_fn = codegen_get_or_emit_type_fmt(codegen, checker, concrete, pointer_form);
    if (!fmt_fn) return NULL;
```
and use `fmt_fn` in the `fields[3]` initializer instead of `null_fmt`.

Declare `codegen_get_or_emit_type_fmt` in `include/codegen.h`.

> Load width note: `data` points at a heap copy of the concrete (value-boxed).
> Use `codegen_type_to_llvm(codegen, concrete)` as the load type. For `TYPE_STRING`
> the boxed value IS a `goo_string` struct; load it and return it.

- [ ] **Step 5: Lower `fmt.Println(interface{})`**

In `src/codegen/call_codegen.c` Println arg loop, add a `TYPE_INTERFACE` branch BEFORE the final `else` "unsupported" error (~line 2052). Extract vtable+data, branch on null vtable, dispatch through fmt_fn, print the resulting string:

```c
        } else if (kind == TYPE_INTERFACE) {
            // Print an interface value by its dynamic type: load {vtable,data};
            // nil vtable -> "<nil>"; else desc = vtable[0]; s = desc.fmt_fn(data);
            // goo_print_string(s). fmt_fn is descriptor field index 2.
            LLVMValueRef ival = arg_val->llvm_value;   // {ptr vtable, ptr data}
            LLVMValueRef vtab = LLVMBuildExtractValue(codegen->builder, ival, 0, "ifvt");
            LLVMValueRef data = LLVMBuildExtractValue(codegen->builder, ival, 1, "ifdata");
            LLVMTypeRef  i8p  = iface_ptr_type(codegen);

            // s = goo_iface_format(vtab, data) — a runtime helper that does the
            // null-check + descriptor hop + fmt_fn call, returning goo_string.
            LLVMValueRef fmtcall = LLVMGetNamedFunction(codegen->module, "goo_iface_format");
            if (!fmtcall) { codegen_error(codegen, a->pos, "goo_iface_format not found"); value_info_free(arg_val); return NULL; }
            LLVMValueRef fargs[] = { vtab, data };
            LLVMValueRef s = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fmtcall),
                                            fmtcall, fargs, 2, "ifstr");

            LLVMValueRef str_fn = LLVMGetNamedFunction(codegen->module, "goo_print_string");
            LLVMValueRef pargs[] = { s };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(str_fn), str_fn, pargs, 1, "");
        }
```

Add the runtime helper `goo_iface_format` to `src/runtime/runtime.c` (encapsulates the null-check + descriptor hop, so the codegen site and Task 3's Sprintf site share it):

```c
// Format an interface value {vtable,data} as its %v string. nil vtable -> "<nil>".
goo_string_t goo_iface_format(void* vtable, void* data) {
    if (!vtable) return goo_string_new("<nil>");
    void* desc = ((void**)vtable)[0];              // vtable slot 0 -> descriptor
    typedef goo_string_t (*GooFmtFn)(void*);
    GooFmtFn fmt = (GooFmtFn)((void**)desc)[2];    // descriptor field 2 -> fmt_fn
    return fmt(data);
}
```
Declare it in `include/runtime.h` and register it in `runtime_integration.c` (return = goo_string struct; params = two `ptr`).

- [ ] **Step 6: Build, run probes GREEN**

```bash
make lexer 2>&1 | grep -iE "error:"; echo done
./bin/goo examples/iface_print_scalars.goo -o /tmp/ips && /tmp/ips | diff - examples/iface_print_scalars.expected.txt && echo "scalars OK"
./bin/goo examples/iface_print_typeswitch.goo -o /tmp/ipt && /tmp/ipt | diff - examples/iface_print_typeswitch.expected.txt && echo "typeswitch OK"
```
Expected: both `OK`.

- [ ] **Step 7: Full gate + commit**

```bash
eval "$(opam env --switch=default)"
bash scripts/run_golden.sh 2>&1 | tail -2   # +2 probes, 0 failures
make test 2>&1 | tail -2
make verify 2>&1 | tail -2                    # ALL GREEN
git add src/runtime/runtime.c include/runtime.h src/codegen/runtime_integration.c \
        src/codegen/interface_codegen.c include/codegen.h src/codegen/call_codegen.c \
        examples/iface_print_scalars.goo examples/iface_print_scalars.expected.txt \
        examples/iface_print_typeswitch.goo examples/iface_print_typeswitch.expected.txt
git commit -m "feat(fmt): print interface{} values via per-type descriptor fmt_fn

fmt.Println(x)/fmt.Print of an interface arg now dispatches on the dynamic
type: {vtable,data} -> goo_iface_format -> descriptor.fmt_fn(data). nil
interface prints <nil>. v1 scalar kinds (int/uint widths, bool, float,
string); adds goo_uint_to_string. Unblocks printing the bound v in a
multi-type type-switch case.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: `%v` / `%s` of an interface argument in `Printf` / `Sprintf`

Route an interface argument matched to `%v` (and `%s`) through the same `goo_iface_format` helper.

**Files:**
- Create: `examples/iface_sprintf_v.goo` + `.expected.txt`
- Modify: `src/codegen/call_codegen.c` — `fmt_emit_segments`, the `%v` (and `%s`) verb argument handling.

**Interfaces:**
- Consumes: `goo_iface_format(void* vtable, void* data) -> goo_string` (Task 2); the Sprintf accumulator (`acc`, `concat_fn`) and Printf `print_str_fn` already in `fmt_emit_segments`.
- Produces: no new symbols.

- [ ] **Step 1: Write the failing probe**

`examples/iface_sprintf_v.goo`:

```go
package main

import "fmt"

func main() {
	var i interface{} = 9
	var s interface{} = "x"
	out := fmt.Sprintf("i=%v s=%v", i, s)
	fmt.Println(out)
	fmt.Printf("p=%v\n", i)
}
```

`examples/iface_sprintf_v.expected.txt` (verify with `go run`):

```
i=9 s=x
p=9
```

- [ ] **Step 2: Run to confirm failure**

```bash
./bin/goo examples/iface_sprintf_v.goo -o /tmp/isv 2>&1 | head -3
```
Expected: a `%v: unsupported argument type` error from `fmt_emit_segments`.

- [ ] **Step 3: Handle `TYPE_INTERFACE` in the `%v`/`%s` arm**

In `fmt_emit_segments`, where the `%v` verb dispatches on the argument's `TypeKind`, add a `TYPE_INTERFACE` case that calls `goo_iface_format(vtable, data)` and then, in `sprintf_mode`, concats the result into `acc` via `concat_fn`; in Printf mode, calls `goo_print_string`. Extract vtable/data from the loaded interface value exactly as in Task 2 Step 5. Apply the same case to the `%s` verb arm.

- [ ] **Step 4: Build, run probe GREEN**

```bash
make lexer 2>&1 | grep -iE "error:"; echo done
./bin/goo examples/iface_sprintf_v.goo -o /tmp/isv && /tmp/isv | diff - examples/iface_sprintf_v.expected.txt && echo "sprintf OK"
```

- [ ] **Step 5: Full gate + commit**

```bash
eval "$(opam env --switch=default)"
bash scripts/run_golden.sh 2>&1 | tail -2
make verify 2>&1 | tail -2
git add src/codegen/call_codegen.c examples/iface_sprintf_v.goo examples/iface_sprintf_v.expected.txt
git commit -m "feat(fmt): %v and %s of an interface argument in Printf/Sprintf

Route an interface-typed %v/%s argument through goo_iface_format, sharing
the Println dispatch path. Reuses the Sprintf accumulator / Printf string
printer.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Dynamic type-name in type-assertion panic

Make the assertion-failure panic name the **dynamic** type via `descriptor.type_name` instead of a static string.

**Files:**
- Create: `examples/iface_assert_dynname_probe.goo` (a Makefile abort-probe style, or a golden probe capturing the panic message if the harness captures stderr — see note).
- Modify: `src/codegen/expression_codegen.c` — the assertion panic message (~line 276).
- Modify: the existing assertion-panic reject/abort probe wording if it asserts the old static message (search `make verify` list for `rtti-assert-panic-probe` / `typeassert-abort-probe`).

**Interfaces:**
- Consumes: the interface value in hand at the assertion site; `descriptor.type_name` (descriptor field index 1); `goo_iface_format` NOT used here — a dedicated tiny read, or a `goo_iface_typename(void* vtable) -> const char*` runtime helper for the null case.
- Produces (optional): `const char* goo_iface_typename(void* vtable)` returning `descriptor.type_name` or `"<nil>"`.

- [ ] **Step 1: Establish the current (static) wording**

```bash
grep -n "interface conversion" src/codegen/expression_codegen.c
```
Confirm line ~276 formats `"interface conversion: %s is not %s"` with the static source type for the first `%s`.

- [ ] **Step 2: Write the failing probe**

Add an assertion that fails and check the message names the dynamic type. Example program (`examples/iface_assert_dynname_probe.goo`):

```go
package main

func main() {
	var x interface{} = 42
	_ = x.(string) // panics: interface conversion: int is not string
}
```

If the golden harness captures only stdout, add this as a Makefile abort-probe (mirror `typeassert-abort-probe`) that greps the panic text for `int is not string`. Wire the new probe target into the `verify:` list. Verify the current binary prints the OLD (static) wording first, to confirm the probe fails RED.

- [ ] **Step 3: Emit the dynamic name**

At the panic-message construction, the assertion codegen has the interface value. Build the message at runtime: read `descriptor.type_name` from the value's vtable (null vtable -> `"<nil>"`), and format `interface conversion: <dyn> is not <T>` where `<T>` stays the static target name. Introduce `goo_iface_typename` (runtime) if it keeps the codegen site simple:

```c
const char* goo_iface_typename(void* vtable) {
    if (!vtable) return "<nil>";
    void* desc = ((void**)vtable)[0];
    return (const char*)((void**)desc)[1];   // descriptor field 1 -> type_name
}
```
Declare + register it. At the assertion site, build the panic string via the existing panic/format path using this dynamic name for the first argument.

- [ ] **Step 4: Run probe GREEN + update any stale reject wording**

```bash
make lexer 2>&1 | grep -iE "error:"; echo done
# run the new abort probe target
make iface-assert-dynname-probe 2>&1 | tail -3
```
Update the previously-static assertion reject/abort probe's expected text to the dynamic wording if it hard-coded the old string.

- [ ] **Step 5: Full gate + commit**

```bash
eval "$(opam env --switch=default)"
make verify 2>&1 | tail -3   # ALL GREEN incl. the new probe
git add src/codegen/expression_codegen.c src/runtime/runtime.c include/runtime.h \
        src/codegen/runtime_integration.c examples/iface_assert_dynname_probe.goo Makefile
git commit -m "feat(rtti): type-assertion panic names the dynamic type

interface conversion: <dynamic> is not <T>, reading descriptor.type_name
instead of the static source type. nil interface -> <nil>.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage** (against `2026-07-06-interface-type-descriptor-design.md`):
- Descriptor global `{eq_fn, type_name, fmt_fn}` → Task 1 (layout) + Task 2 (fmt_fn).
- Vtable slot-0 reinterpretation + `goo_iface_key_eq` hop → Task 1.
- `fmt_fn` scalar thunk → Task 2.
- `fmt.Println`/`Print` interface arg → Task 2.
- `%v`/`%s` interface arg in Printf/Sprintf → Task 3.
- Dynamic panic type-name → Task 4.
- `<nil>` handling → Task 2 (`goo_iface_format`) + Task 4 (`goo_iface_typename`).
- Scalar-only v1 + fallback → Task 2 Step 4 (non-scalar → type_name copy).
- Testing/regression net → each task's gate + Task 1's regression-only gate.
- Open question 1 (scalar→string helper) → RESOLVED: `goo_int_to_string`/`goo_float_to_string`/`goo_bool_to_string` exist; `goo_uint_to_string` added in Task 2.
- Open question 2 (`goo_iface_format` helper vs inline) → RESOLVED: shared runtime helper (Task 2 Step 5).
- Open question 3 (pointer-boxed fmt) → RESOLVED: type_name fallback (Task 2 Step 4).
- Open question 4 (type_name source) → RESOLVED: `type_receiver_name`/`type_to_string` + `*` prefix (Task 1 Step 1), bare names matching the existing static panic wording.

**Placeholder scan:** The `codegen_get_or_emit_type_fmt` body (Task 2 Step 4) is given as a numbered structure rather than literal LLVM-C, because it must mirror `build_thunk`'s function-creation/builder-save-restore idiom exactly and inlining ~80 lines of LLVM-C API calls invites transcription errors; the reference (`build_thunk`, `interface_codegen.c:55`) is cited and the per-kind dispatch is fully enumerated. Same for the `%v` arm in Task 3 Step 3 (mirror the existing verb-dispatch switch). All other steps carry literal code.

**Type consistency:** Descriptor field indices are fixed and used consistently — 0=eq_fn (Task 1 Step 3 `desc[0]`), 1=type_name (Task 4 `desc[1]`), 2=fmt_fn (Task 2 `desc[2]`). `goo_iface_format(void*,void*)->goo_string` and `goo_iface_typename(void*)->const char*` signatures match across producer (runtime) and consumer (codegen) tasks. `codegen_get_or_emit_type_desc(..., int pointer_form)` / `codegen_get_or_emit_type_fmt(..., int pointer_form)` signatures consistent.
