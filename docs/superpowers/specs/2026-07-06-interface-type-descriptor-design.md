# Interface Type Descriptor — `fmt.Println(any)`, `%v` of interface args, dynamic panic type-names

Date: 2026-07-06
Status: Design approved (Approach 1)
Related: PR #135 (concrete-type RTTI on `any`), PR #131 (interface-map-keys / vtable slot-0 eq), `goolang-rtti-followups` memory.

## Problem

An `interface{}`-typed value cannot be printed. `fmt.Println(x)` where `x` is
statically `interface{}` fails at codegen with *"unsupported argument type
(only string, integer, bool, and float are supported in v1)"*, because the
Println/Printf lowering dispatches on the **static** `TypeKind` of each argument
(`call_codegen.c` ~line 1984) and an interface value's static kind is
`TYPE_INTERFACE`, which falls into the error branch.

The value's **dynamic** type is available only at runtime, encoded in the
interface's vtable pointer. Recovering it to format the value requires a small
amount of per-type metadata reachable from that pointer.

Two adjacent gaps share the same missing metadata:
- `%v` / `%s` of an interface argument in `Printf`/`Sprintf` (same static-kind
  dispatch, same failure).
- Assertion-failure panic messages are static: `interface conversion: <static>
  is not <T>` (`expression_codegen.c` ~line 276). Go names the **dynamic** type.

This directly limits the just-shipped type-switch feature (#135): dispatch into
`case int, string:` works, but the bound `v` (still statically `interface{}` in
a multi-type case) cannot be printed.

## Scope (this cycle)

In:
- `fmt.Println` / `fmt.Print` of an `interface{}`-typed argument.
- `%v` / `%s` of an interface argument in `fmt.Printf` / `fmt.Sprintf`.
- Dynamic type-name in the type-assertion failure panic message.
- Descriptor covers **scalar** dynamic kinds for formatting: int/uint (all
  widths), bool, float32/64, string. Nil interface prints `<nil>`.

Out (explicit follow-ups):
- `%v` formatting of **struct / pointer / slice / map** dynamic values — the
  descriptor and `fmt_fn` thunk are designed to grow into these, but v1 emits a
  `type_name`-based fallback string rather than the full Go representation.
- `%T` verb (the descriptor carries `type_name`, so this is a later small add).
- Interface-target assertions/switches (`x.(Stringer)`, `case Stringer:`) — the
  deferred RTTI phase-2; this descriptor is its foundation but the
  interface-implements table is separate work.
- Interface-to-interface `==` (`i1 == i2`) — orthogonal, currently a clean
  codegen rejection.

## Chosen approach — descriptor behind vtable slot-0 (Approach 1)

Reinterpret the interface vtable's **slot 0**. Today slot 0 holds the concrete
type's per-type value-equality function (`codegen_get_or_emit_type_eq`, added by
PR #131); slots 1..n are the method thunks. Under this design slot 0 instead
holds a pointer to a per-concrete-type **descriptor** global, and the eq
function moves *into* the descriptor. Method slots 1..n are **unchanged** — no
GEP renumbering — so the regression surface is far smaller than adding a slot.

This mirrors Go's own representation (`itab → _type`): the interface value stays
the existing 2-word `{vtable, data}` box (no ABI width change — the change #132
declined), and all type metadata lives behind the vtable pointer.

### Rejected alternatives

- **Add a new vtable slot for the descriptor** (keep eq at its slot): forces a
  second "+1" shift of every method dispatch GEP (the PR #131 shift), with its
  full regression net, for no benefit over reinterpreting slot 0.
- **Kind-tag descriptor + generic runtime formatter** (`{kind, width,
  type_name}`, no per-type `fmt_fn`): simpler for scalars but cannot format a
  struct (`%v` of a struct is `{1 2}`, needing per-type field walking the
  runtime cannot do without generated code). A per-type `fmt_fn` thunk handles
  structs when we extend it, and reuses the existing on-demand per-type-thunk
  machinery.

## Components

### 1. The descriptor global

Per concrete type `T`, a private constant global:

```
goo.typedesc.<T> = { ptr eq_fn, ptr type_name, ptr fmt_fn }
```

- `eq_fn` — placed **first** so the runtime hop is a single extra deref (see
  §3). This is the existing `codegen_get_or_emit_type_eq(T)` for value-boxed
  types, or the pointer-identity eq for the pointer-boxed form (as today).
- `type_name` — a C string constant: `"int"`, `"string"`, `"main.Point"`,
  `"*main.Point"`. Derived from `T`'s Go type name.
- `fmt_fn : goo_string (*)(ptr data)` — see §2.

Name-deduped by `<T>` exactly like the vtable globals, so one descriptor per
concrete type regardless of how many interfaces it satisfies. Emitted on demand
by a new `codegen_get_or_emit_type_desc(codegen, checker, concrete,
pointer_form)`, colocated with `codegen_get_or_emit_type_eq` /
`codegen_interface_vtable`.

### 2. The `fmt_fn` thunk

`goo_string goo.fmt.<T>(ptr data)` — loads the concrete value from `data` and
returns its `%v` / `Println` string representation. Generated on demand, per
type, like the method thunks and eq fn.

- Value-boxed scalar `T`: load the `T` value from `data`, format it to a
  `goo_string` using the **same runtime scalar→string path that `Sprintf`'s
  `%d`/`%v` already uses** (see Open Questions — confirm/extract a
  `goo_fmt_int`/`goo_fmt_uint`/`goo_fmt_bool`/`goo_fmt_float` helper; string
  returns a copy of the value).
- Pointer-boxed `T`: v1 fallback (see below).
- Non-scalar (struct/slice/map/pointer) in v1: return the `type_name`-based
  fallback string (a clearly-bounded placeholder, not a crash). Extending these
  to true `%v` is the named follow-up.

Returning a `goo_string` (rather than printing directly) unifies `Println`
(print the returned string) and `Sprintf`/`Printf` `%v` (append the returned
string) through one thunk.

### 3. Vtable + runtime wiring

- `codegen_interface_vtable` (`interface_codegen.c` ~line 234): `slots[0] =
  codegen_get_or_emit_type_desc(...)` instead of the bare `eq_fn`. Slots 1..n
  unchanged.
- `goo_iface_key_eq` (`runtime.c` ~line 603): change `eq = ((void**)vta)[0]` to
  `desc = ((void**)vta)[0]; eq = ((void**)desc)[0]` — one extra load, because
  `eq_fn` is the descriptor's first field. No other runtime change (the map-key
  eq is the only slot-0 consumer).
- Type-assertion vtable-identity checks compare whole vtable pointers and never
  index slot 0, so they are unaffected (confirmed by grep during design).

### 4. `fmt.Println` / `%v` lowering for interface arguments

In the Println arg loop (`call_codegen.c` ~line 1984) add a `TYPE_INTERFACE`
case before the "unsupported" fallback; the `%v` path in `fmt_emit_segments`
(Sprintf/Printf) gets the parallel case:

1. Ensure the interface value is loaded (the pair `{vtable, data}`).
2. Extract `vtable` (field 0) and `data` (field 1).
3. Branch on `vtable == null`:
   - null → the constant string `"<nil>"`.
   - non-null → `desc = vtable[0]`; `s = desc->fmt_fn(data)`.
4. `Println`: `goo_print_string(s)`. `Sprintf`/`Printf` `%v`: append `s`.

A small runtime helper `goo_string goo_iface_format(void* vtable, void* data)`
may encapsulate steps 2–3 to keep the codegen site and the Sprintf site sharing
one implementation; decided at implementation time.

### 5. Dynamic panic type-name

The assertion-failure panic (`expression_codegen.c` ~line 276) currently formats
a compile-time static string. Where the interface value is in hand, read
`desc->type_name` for the `<dynamic>` half so the message matches Go
(`interface conversion: int is not string`). If the value is a nil interface,
use `"<nil>"`. This is an isolated change guarded to not regress the existing
reject-probe (which will be updated to the dynamic wording).

## Data flow summary

```
fmt.Println(x)   x: interface{}
  └─ load {vtable, data}
     ├─ vtable == null → print "<nil>"
     └─ else desc = vtable[0]; s = desc.fmt_fn(data); goo_print_string(s)

interface map key eq (unchanged behavior, +1 deref)
  └─ desc = vtable[0]; eq = desc.eq_fn; eq(dataA, dataB)

x.(T) fails
  └─ desc = vtable[0]; panic "interface conversion: {desc.type_name} is not T"
```

## Error handling

- Nil interface (`{null,null}`) prints/formats as `<nil>` everywhere.
- `fmt_fn` never crashes on an in-scope value; a v1-unsupported dynamic kind
  yields the `type_name` fallback string.
- Existing static-argument fmt paths are untouched — only the `TYPE_INTERFACE`
  argument case is new.

## Testing

Golden probes (each go-run-verified against upstream Go):
- `iface_print_scalars` — `Println` of an `interface{}` holding int/uint/bool/
  float/string, plus a nil interface → `<nil>`.
- `iface_print_typeswitch` — printing the bound `v` inside a `switch x.(type)`
  multi-type case (`case int, string:`).
- `iface_sprintf_v` — `%v` and `%s` of an interface argument in `Sprintf`.
- Reject/panic probe — assertion failure shows the **dynamic** type name
  (updates the existing assertion-panic reject wording).

Regression net (the slot-0 reinterpretation touches the shared vtable):
- The full interface-method-dispatch, interface-map-key (incl.
  `iface-map-key-uncomparable-probe`), embedding, and type-assertion/switch
  suites.
- `make verify` ALL GREEN (ccomp-link included), golden 0 failures, `make test`
  76/1, bison unchanged (no grammar change expected).

## Open questions (resolve during planning/implementation)

1. **Scalar→string runtime helper.** Does a reusable `goo_fmt_<kind>(value) →
   goo_string` already exist behind `Sprintf`'s `%d`/`%v`, or must the `fmt_fn`
   thunk call a to-be-extracted helper? Confirm before writing `fmt_fn`.
2. **`goo_iface_format` helper vs inline codegen.** Encapsulate the
   vtable/nil/dispatch dance in one runtime function shared by the Println and
   Sprintf sites, or inline at both. Prefer the shared helper if it does not
   complicate passing the already-loaded interface value.
3. **Pointer-boxed `fmt_fn`.** v1 fallback string vs a minimal `0x…`/`&{…}`
   form. Default to the `type_name` fallback unless trivial.
4. **`type_name` source.** Reuse an existing type→name function
   (`type_to_string`/`type_receiver_name`) or a dedicated Go-faithful namer
   (package-qualified `main.Point`, `*main.Point`). Pick one during planning.

## Risks

- **Slot-0 reinterpretation** is the load-bearing change: any missed slot-0
  consumer would miscompile map-key eq. Grep during design found exactly two
  (builder + `goo_iface_key_eq`); the final whole-branch review must re-confirm.
- **`fmt_fn` scope creep** toward full `%v`: kept bounded by the scalar-only v1
  with an explicit fallback for aggregates.
