# Rename dead-feature keywords off common Go identifiers

**Date:** 2026-06-30
**Branch:** `feat/v1-phase3-keyword-rename`
**Phase:** v1 Phase 3 (Go-source compatibility)
**Trigger:** the TinyGo-style `sort` port — `sort.Sort(data Interface)` failed to
parse because `data` was a reserved keyword.

## Problem

The lexer keyword table (`src/lexer/token.c`) reserves ~30 common Go identifiers
for dead/standalone WASM/GPU/parallel/ownership/messaging subsystems. Because the
lexer turns these spellings into feature tokens the grammar then rejects,
idiomatic Go code using them as identifiers fails to **parse** (bare "syntax
error"). Examples that collide: `data start memory table local global host shared
from atomic reduce device constant elem export push pull parallel owned borrowed
kernel`.

## Why it is safe to change (architecture)

Three decoupled layers:
1. lexer keyword table: source spelling → `TOKEN_*` (`token.c`)
2. `lexer_bridge.c`: `TOKEN_*` → bison token code (e.g. `TOKEN_DATA` → `DATA`)
3. `parser.y`: bison token codes → grammar rules

Changing a **spelling** in layer 1 is invisible to layers 2–3: the `TOKEN_*`
enums, the bridge, and every grammar rule are untouched. The (mostly "useless in
grammar") feature rules still exist — they are simply now reached under the new
spelling. So `data` becomes an ordinary `TOKEN_IDENT`, and `wasm_data` triggers
`TOKEN_DATA`.

## Decision

Scheme: **subsystem-prefix (underscore)**. Chosen over a sigil (`@data`, needs new
lexer logic) and outright removal (discards the feature's revival path). Pure
keyword-table edit, zero structural risk.

Keep reserved (live Goo differentiators, not dead-feature): `try catch match
comptime concept let enum`. Also keep the low-level / block-introducer keywords
`wasm unsafe asm extern no_std` (rare as Go identifiers).

### Old → new map

| Family | Renames |
|--------|---------|
| WASM | data→wasm_data, memory→wasm_memory, table→wasm_table, start→wasm_start, elem→wasm_elem, export→wasm_export |
| GPU | kernel→gpu_kernel, device→gpu_device, host→gpu_host, global→gpu_global, local→gpu_local, constant→gpu_constant, sharedMem→gpu_shared_mem |
| Parallel | parallel→par_parallel, reduce→par_reduce, barrier→par_barrier, atomic→par_atomic, threadLocal→par_thread_local |
| Ownership | owned→own_owned, borrowed→own_borrowed, shared→own_shared |
| Messaging | pub→msg_pub, sub→msg_sub, req→msg_req, rep→msg_rep, push→msg_push, pull→msg_pull, from→msg_from |
| Low-level misc | volatile→ll_volatile, inline→ll_inline |

## Testing

`examples/go_identifiers_probe.goo` (+`.expected.txt`): Go-style code using the
freed words as function names, parameters, struct fields, and variables. Written
failing first (red), green after the rename. Gate: `make verify` ALL GREEN (incl.
ccomp), golden 80/0, `make test` 76/1.

## Out of scope / follow-ups

- 4 standalone, non-gated LSP-server files (`lsp_enhanced`, `lsp_standalone`,
  `debug_adapter`, `performance_dashboard`) still list old spellings in
  autocomplete; not built by `goo`/`verify`/`test`. Cosmetic.
- Ungated illustrative demos (`advanced_features_demo`, etc.) do not compile on
  `main` today (independent of this change) and were not updated.
