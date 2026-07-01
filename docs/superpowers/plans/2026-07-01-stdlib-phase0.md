# Stdlib Phase 0 — Import Resolution + Multi-Package Compilation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A hand-written `goostd/mypkg/mypkg.go` with one exported pure function, imported and called from a `.goo` file, compiles → links → runs with correct output; all existing golden gates stay green.

**Architecture:** Add package import + multi-file compilation on top of the existing single-module pipeline. A package = the `*.go` files in a `goostd/<path>/` directory. The driver resolves imports, parses each package (snapshotting the global `ast_root`), type-checks in topological order into per-package export scopes, and codegens all packages into the ONE existing LLVM module with mangled cross-package symbol names. Selector resolution consults real package exports first, falling back to the existing hardcoded C shim so every current gate stays green.

**Tech Stack:** C23, LLVM-C 22, Bison/Flex parser (global-state, re-entrant via `parse_input`), golden probes.

## Global Constraints

- Build env: `eval "$(opam env --switch=default)"` STANDALONE (never piped) before `make verify`.
- Build: `make lexer` produces `bin/goo` + `lib/libgoo_runtime.a`. Gate: `make verify` → `ALL GREEN GATES PASSED` (golden count unchanged except where a task adds a probe) AND `make test` → 76/1. Commits `--no-gpg-sign`.
- **Backward compatibility is sacred:** a `.goo` file with NO imports must compile/run exactly as today. `main`-package symbols keep BARE names. The hardcoded shim (`stdlib_package_lookup`, the `call_codegen.c` `goo_*` if-chain) stays as a per-symbol FALLBACK — do not delete it.
- **Keystone (verified):** AST nodes `str_dup` their strings and `parse_input` (lexer_bridge.c:378) is self-contained (own lexer, state reset on entry, sets global `ast_root`). So: call `parse_input(src, file)`, immediately snapshot `ast_root`, and the tree is independent of `src`. Keep each package's source buffer alive until after codegen anyway (belt-and-suspenders), freeing all at the end.
- Package source discovery: `goostd/<import/path>/*.go`, excluding `*_test.go`. Do NOT use the stale `stdlib/*.goo` tree.
- Syntax freeze: add NO new surface syntax this milestone (Go source-compat is a hard constraint).

---

### Task 1: Import resolver (GOOROOT + package source discovery)

**Files:**
- Create: `src/package/import_resolver.c`, `include/import_resolver.h`
- Modify: `Makefile` (add `src/package/import_resolver.c` to the compiler source list; add a `goostd-resolver-probe` target)
- Create: `goostd/mypkg/mypkg.go` (the Phase-0 exit fixture — one exported pure fn)
- Create: `tests/fixtures/goostd/greet/greet.go` + a `_test.go` sibling (to prove `_test.go` exclusion)

**Interfaces:**
- Consumes: `goo_runtime_archive_path()` precedence pattern (src/codegen/codegen.c) — mirror it.
- Produces:
  - `typedef struct { char** files; size_t file_count; char* name; char* import_path; } PackageSource;`
  - `int resolve_import(const char* import_path, PackageSource* out);` — returns 0 on success, non-0 if the package dir doesn't exist. Fills `out` with the sorted list of non-`_test.go` `*.go` file paths, the package short name (last path segment), and a strdup'd `import_path`.
  - `void package_source_free(PackageSource* p);`
  - GOOROOT resolution `const char* goo_gooroot_dir(void)`: `$GOOROOT` → `<exe>/../lib/goostd` (via `/proc/self/exe`, mirroring the runtime-archive path logic) → `./goostd`.

- [ ] **Step 1: Write the failing test (a tiny C probe)**

Create `tests/package/resolver_probe.c` that calls `resolve_import("mypkg", &ps)` against a `GOOROOT` pointing at a fixture dir, asserts `ps.name == "mypkg"`, `ps.file_count == 1`, and that a `greet` package with a `greet_test.go` yields `file_count == 1` (the `_test.go` excluded). Print `RESOLVER OK` on success, `RESOLVER FAIL: <reason>` + exit 1 otherwise.

```c
#include "import_resolver.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void) {
    setenv("GOOROOT", "tests/fixtures/goostd", 1);
    PackageSource ps;
    if (resolve_import("greet", &ps) != 0) { printf("RESOLVER FAIL: resolve\n"); return 1; }
    if (strcmp(ps.name, "greet") != 0) { printf("RESOLVER FAIL: name=%s\n", ps.name); return 1; }
    if (ps.file_count != 1) { printf("RESOLVER FAIL: count=%zu (expected 1, _test.go must be excluded)\n", ps.file_count); return 1; }
    package_source_free(&ps);
    printf("RESOLVER OK\n");
    return 0;
}
```

- [ ] **Step 2: Create the fixtures**

`tests/fixtures/goostd/greet/greet.go`:
```go
package greet
func Hello() string { return "hi" }
```
`tests/fixtures/goostd/greet/greet_test.go`:
```go
package greet
func TestHello() {}
```
And the exit fixture `goostd/mypkg/mypkg.go`:
```go
package mypkg
func Double(n int) int { return n + n }
```

- [ ] **Step 3: Run the probe to confirm it fails**

Run: `make goostd-resolver-probe` (add the target: compile `resolver_probe.c` + `import_resolver.c` into `build/resolver_probe`, run it).
Expected: FAIL — `resolve_import` undefined / probe can't build.

- [ ] **Step 4: Implement `import_resolver.c`**

Implement `goo_gooroot_dir` (env → exe-relative `/proc/self/exe` dirname + `/../lib/goostd` → `./goostd`), and `resolve_import`: build `<gooroot>/<import_path>`, `opendir`, collect entries ending in `.go` but NOT `_test.go`, sort them (`qsort` + `strcmp` for determinism), strdup each full path, set `name` = last segment of `import_path`. Return non-0 if the dir is missing or has zero `.go` files. `package_source_free` frees the array + strings.

- [ ] **Step 5: Run the probe to confirm it passes**

Run: `make goostd-resolver-probe`
Expected: `RESOLVER OK`

- [ ] **Step 6: Regression gate + commit**

Run: `eval "$(opam env --switch=default)"; make verify 2>&1 | tail -2 && make test 2>&1 | grep -E "Passed|Skipped"`
Expected: unchanged golden count, ALL GREEN; 76/1.
```bash
git add src/package/import_resolver.c include/import_resolver.h Makefile goostd/mypkg/mypkg.go tests/fixtures/goostd tests/package/resolver_probe.c
git commit --no-gpg-sign -m "feat(package): import resolver — GOOROOT + package .go discovery (excludes _test.go)"
```

---

### Task 2: Package data structures + export filter

**Files:**
- Modify: `include/types.h` (add `Package` struct; `TypeChecker.packages`, `TypeChecker.current_package`)
- Modify: `src/types/type_checker.c` (init/free the new fields; add `package_export_filter` helper)

**Interfaces:**
- Consumes: existing `Scope`, `Variable`, `scope_new`, `scope_add_variable`, `scope_lookup_variable`.
- Produces:
  - `typedef struct Package { char* import_path; char* name; Scope* exports; int state; /* 0=unvisited 1=in-progress 2=done */ struct Package* next; } Package;`
  - `TypeChecker.packages` (linked list), `TypeChecker.current_package` (Package* or NULL for main).
  - `Package* type_checker_find_package(TypeChecker*, const char* import_path);`
  - `Package* type_checker_add_package(TypeChecker*, const char* import_path, const char* name);`
  - `void package_export_filter(Scope* pkg_scope, Scope* exports);` — copy top-level symbols whose name starts `A`–`Z` into `exports`.

- [ ] **Step 1: Add the struct + fields (behavior-preserving)**

Add `Package` to `include/types.h`; add the two fields to `TypeChecker`; init to NULL in `type_checker_new`; free the list in `type_checker_free`. `package_export_filter` walks `pkg_scope`'s variable list and `scope_add_variable`s each `A`–`Z`-leading symbol into `exports`.

- [ ] **Step 2: Build + gate (no behavior change yet)**

Run: `eval "$(opam env --switch=default)"; make lexer 2>&1 | tail -3 && make verify 2>&1 | tail -2 && make test 2>&1 | grep -E "Passed|Skipped"`
Expected: builds; golden unchanged; 76/1. (Nothing calls the new code yet — this is scaffolding folded into Task 3's deliverable, committed separately for reviewability.)

- [ ] **Step 3: Commit**

```bash
git add include/types.h src/types/type_checker.c
git commit --no-gpg-sign -m "feat(types): Package namespace struct + export filter (scaffolding)"
```

---

### Task 3: Driver import-graph walk (parse + topological order)

**Files:**
- Modify: `src/compiler/goo.c` (`compile_file`: resolve+parse imported packages, topo-order, keep sources alive)
- Modify: `include/import_resolver.h` if a small parse helper is needed

**Interfaces:**
- Consumes: `resolve_import` (Task 1), `parse_input`/`ast_root` (lexer_bridge.c), `type_checker_add_package` (Task 2).
- Produces: after this task, `compile_file` has, before type-checking main, a `Package` per transitively-imported package, each carrying its parsed `ProgramNode*` AST, ordered leaves-first. No checking/codegen wired yet (Task 4/5) — the deliverable is: the walk builds the correct ordered package list and detects cycles, verified by a `--dump-packages` debug flag.

- [ ] **Step 1: Failing test** — add a hidden `--dump-packages` flag that prints resolved packages in processing order, one per line. Probe: a `.goo` importing `mypkg` prints `mypkg` before `main`. Confirm it FAILS first (flag/walk absent).
- [ ] **Step 2: Implement the walk** — parse main, read its `imports`; for each unresolved import: `resolve_import`, concatenate the package's `*.go` sources, `parse_input` each and snapshot `ast_root`, merge into one `ProgramNode` per package (or keep a list), record its own `imports`; recurse; tri-color `state` cycle detection (error on a back-edge to an in-progress package); emit in finish order (topo, leaves first). Keep every source buffer alive in an array freed at `compile_file` end.
- [ ] **Step 3: Pass** — `--dump-packages` on the mypkg-importing probe prints `mypkg` then `main`; a deliberately cyclic fixture errors cleanly.
- [ ] **Step 4: Gate + commit** (`git add src/compiler/goo.c include/import_resolver.h`; message `feat(driver): import-graph walk — parse packages in topological order`).

---

### Task 4: Package-scoped type-checking + cross-package mangling

**Files:**
- Modify: `src/types/type_checker.c` (`type_check_program` → add `type_check_package`; conditional package-marker seeding ~205)
- Modify: `src/codegen/function_codegen.c` (~293: mangle non-main package function names)
- Modify: `src/codegen/codegen.c` (drive per-package codegen into the one module)

**Interfaces:**
- Consumes: the ordered package list (Task 3), `package_export_filter` (Task 2).
- Produces: `type_check_package(TypeChecker*, Package*, ASTNode* program)` checks a package with `current_package` set, then fills `pkg->exports`. Non-main package top-level functions codegen under `goo_pkg__<pkg>__<name>`; `main` package unchanged (bare names). Exit: a package's exported function exists in the module under its mangled name and in `pkg->exports` with a real `TYPE_FUNCTION` signature (verify via `--dump-packages` extended to list exports, or an `--emit-llvm` grep).

- [ ] Steps: failing check (mangled symbol absent) → implement `type_check_package` + export filter call + mangling at `LLVMAddFunction` + per-package codegen loop → confirm the mangled symbol appears in `--emit-llvm` and main still uses bare names → gate → commit (`feat(types,codegen): package-scoped checking + cross-package name mangling`).

---

### Task 5: Selector resolution (both sides) + Phase-0 exit probe

**Files:**
- Modify: `src/types/expression_checker.c` (`type_check_selector_expr` ~1273: consult `pkg->exports` before `stdlib_package_lookup`)
- Modify: `src/codegen/call_codegen.c` (~310: try mangled `goo_pkg__<pkg>__<name>` before the hardcoded `goo_*` arms)
- Create: `examples/import_mypkg_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: `pkg->exports` (Task 4), mangled symbols (Task 4).
- Produces: `mypkg.Double(21)` type-checks against the real exported signature and codegens a call to `goo_pkg__mypkg__Double`. The shim fallback is untouched for packages with no real exports.

- [ ] **Step 1: Failing golden probe**

`examples/import_mypkg_probe.goo`:
```go
package main
import (
	"fmt"
	"mypkg"
)
func main() {
	fmt.Println(mypkg.Double(21))
}
```
`examples/import_mypkg_probe.expected.txt`:
```
42
```
Run it → FAIL (mypkg.Double unresolved).

- [ ] **Step 2: Implement selector resolution** — typecheck: for `TYPE_PACKAGE` base carrying a real `Package*`, `scope_lookup_variable(pkg->exports, selector)` first (real signature → real arg checking), else `stdlib_package_lookup`. Codegen: for a real package selector call, `LLVMGetNamedFunction(module, "goo_pkg__<pkg>__<name>")` and call it; else the existing `goo_*` if-chain.
- [ ] **Step 3: Pass** — the probe prints `42`.
- [ ] **Step 4: Backward-compat + full gate** — a no-import probe (`examples/baseline_probe.goo`) still passes; `make verify` ALL GREEN with `import_mypkg_probe` added (+1 golden); `make test` 76/1. The `smoke-stdlib`/`m12-probe` gates (shim path) stay green.
- [ ] **Step 5: Commit** (`git add src/types/expression_checker.c src/codegen/call_codegen.c examples/import_mypkg_probe.*`; message `feat(stdlib): resolve package selectors to real exports, shim fallback intact`).

---

## Verification (end-to-end)

1. `make goostd-resolver-probe` → `RESOLVER OK`.
2. `bin/goo -o build/import_mypkg_probe examples/import_mypkg_probe.goo && build/import_mypkg_probe` → `42`.
3. `eval "$(opam env --switch=default)"; make verify` → `ALL GREEN GATES PASSED` (all prior probes green — backward compat + shim fallback intact — plus the new import probe); `make test` → 76/1.
4. A no-import `.goo` compiles/runs byte-identically to before (regression guard).
5. Whole-branch review → push + PR.

## Self-Review notes

- **Coupling honesty:** Phase 0's exit behavior needs the whole vertical slice, so Tasks 2–4 commit scaffolding that only becomes observable in Task 5's probe. Each task still ends with a build+gate (and Tasks 1/3 with their own probes) so a reviewer can reject one independently.
- **Backward compat is the dominant risk** — every task re-runs `make verify`/`make test`, and Task 5 explicitly re-checks a no-import probe + the shim gates.
- **Discovery-heavy tasks (3, 4)** specify precise interfaces, files, and the observable test (`--dump-packages`, `--emit-llvm` grep) rather than fabricated exact IR, because the integration code genuinely emerges against the global-state parser; the contracts and tests are exact.
- Scope: Phase 0 only (import machinery + one hand-written package). Phase 1 (real upstream `errors`) and Phase 2 (overrides) are separate plans.
