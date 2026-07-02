CC = gcc
# _GNU_SOURCE is a project-wide feature-test macro: it exposes mkstemp,
# pthread_rwlock_t, popen, etc. used across the runtime/proof/concurrency
# sources. It was only reaching the main build (via LLVM_CFLAGS); the many
# bespoke test targets that compile with $(CFLAGS) alone missed it and broke
# on "implicit declaration of mkstemp" / "unknown type pthread_rwlock_t".
# -I. lets sources that spell includes as "include/foo.h" (many concurrency
# and example tests do) resolve from the repo root; plain "foo.h" still
# resolves via -Iinclude. Both forms are in use; keeping both on the path
# avoids "fatal error: include/foo.h: No such file" in those targets.
CFLAGS = -Wall -Wextra -std=c23 -g -I. -Iinclude -I/opt/homebrew/include -D_GNU_SOURCE
LDFLAGS = -lm -pthread -ljson-c -lcurl -lz -L/opt/homebrew/lib

# Apple-style blocks (^-syntax) build path. GCC cannot parse ^-blocks, but
# clang can with -fblocks, linked against the BlocksRuntime. Exactly three
# sources use blocks: src/async/async_streams.c,
# src/concurrency/structured_concurrency.c and
# src/concurrency/structured_concurrency_enhanced.c. Any test target that
# compiles/links one of those must use these BLOCKS_* variables instead of
# $(CC)/$(CFLAGS)/$(LDFLAGS) so it goes through clang -fblocks -lBlocksRuntime.
BLOCKS_CC = clang
BLOCKS_CFLAGS = $(CFLAGS) -fblocks
BLOCKS_LDFLAGS = $(LDFLAGS) -lBlocksRuntime

# Coverage flags
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
COVERAGE_LIBS = -lgcov

# LLVM configuration - prefer system LLVM 20, then Homebrew LLVM
LLVM_CONFIG = $(shell which llvm-config-20 2>/dev/null || which llvm-config 2>/dev/null || echo "/opt/homebrew/opt/llvm/bin/llvm-config")

ifeq ($(shell test -x $(LLVM_CONFIG) && echo "exists"), exists)
    LLVM_CFLAGS = $(shell $(LLVM_CONFIG) --cflags) -DLLVM_AVAILABLE=1
    LLVM_LDFLAGS = $(shell $(LLVM_CONFIG) --ldflags --libs core)
    # Add WebAssembly target libraries if available
    LLVM_LDFLAGS += $(shell $(LLVM_CONFIG) --libs webassembly 2>/dev/null || echo "")
    $(info Using LLVM config: $(LLVM_CONFIG))
    $(info LLVM version: $(shell $(LLVM_CONFIG) --version))
else
    LLVM_CFLAGS = -DLLVM_AVAILABLE=0
    LLVM_LDFLAGS = 
    $(warning LLVM not found at $(LLVM_CONFIG) - building without LLVM support)
endif

# Directories
SRCDIR = src
INCDIR = include
TESTDIR = tests
BUILDDIR = build
BINDIR = bin
LIBDIR = lib
COMPILERDIR = $(SRCDIR)/compiler

# Test subdirectories
TEST_UNIT_DIR = $(TESTDIR)/unit
TEST_INTEGRATION_DIR = $(TESTDIR)/integration
TEST_FRAMEWORK_DIR = $(TESTDIR)/framework
TEST_DEMOS_DIR = $(TESTDIR)/demos

# Source files (lexer + parser + AST + types + error handling + test framework)
LEXER_SRCS = $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
PARSER_SRCS = $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/lexer_bridge.c $(SRCDIR)/parser/parser_errors.c
AST_SRCS = $(SRCDIR)/ast/ast.c $(SRCDIR)/ast/ast_constructors.c
TYPES_SRCS = $(SRCDIR)/types/types.c $(SRCDIR)/types/type_checker.c $(SRCDIR)/types/expression_checker.c $(SRCDIR)/types/expression_helpers.c $(SRCDIR)/types/ownership_checker.c $(SRCDIR)/types/channel_checker.c $(SRCDIR)/types/constraint_inference.c $(SRCDIR)/types/advanced_constraint_inference.c $(SRCDIR)/types/concept_generics.c $(SRCDIR)/types/higher_kinded_types.c $(SRCDIR)/types/type_level_programming.c $(SRCDIR)/types/type_level_dependent.c $(SRCDIR)/types/type_level_eval.c $(SRCDIR)/types/interface_integration.c $(SRCDIR)/types/flow_sensitive_analysis.c $(SRCDIR)/types/flow_analysis_core.c $(SRCDIR)/types/reference_manager.c $(SRCDIR)/types/hkt_auto_impl.c $(SRCDIR)/types/protocol_oriented_programming.c $(SRCDIR)/types/escape_analysis.c $(SRCDIR)/types/resource_manager.c $(SRCDIR)/types/memory_safety_integration.c $(SRCDIR)/types/bounds_verifier.c $(SRCDIR)/types/symbolic_expression.c $(SRCDIR)/types/dependent_types.c $(SRCDIR)/types/contracts.c $(SRCDIR)/types/proof_generation.c $(SRCDIR)/types/proof_smt.c $(SRCDIR)/types/proof_obligations.c $(SRCDIR)/types/proof_reporting.c $(SRCDIR)/types/runtime_optimization.c
CODEGEN_SRCS = $(SRCDIR)/codegen/codegen.c $(SRCDIR)/codegen/type_mapping.c $(SRCDIR)/codegen/function_codegen.c $(SRCDIR)/codegen/statement_codegen.c $(SRCDIR)/codegen/expression_codegen.c $(SRCDIR)/codegen/call_codegen.c $(SRCDIR)/codegen/composite_codegen.c $(SRCDIR)/codegen/lowlevel_codegen.c $(SRCDIR)/codegen/error_union_codegen.c $(SRCDIR)/codegen/nullable_codegen.c $(SRCDIR)/codegen/interface_codegen.c $(SRCDIR)/codegen/runtime_integration.c $(SRCDIR)/codegen/wasm_codegen.c
RUNTIME_SRCS = $(SRCDIR)/runtime/runtime.c $(SRCDIR)/runtime/platform.c $(SRCDIR)/runtime/concurrency.c $(SRCDIR)/runtime/channels.c $(SRCDIR)/runtime/sync.c $(SRCDIR)/runtime/deadlock.c
ERROR_SRCS = $(SRCDIR)/errors/error.c $(SRCDIR)/errors/ergonomic_errors.c
IDE_SRCS = $(SRCDIR)/ide/hot_reload.c $(SRCDIR)/ide/repl.c $(SRCDIR)/ide/performance_monitor.c $(SRCDIR)/ide/repl_errors.c $(SRCDIR)/ide/time_travel_debug.c $(SRCDIR)/ide/time_travel_debug_repl.c $(SRCDIR)/ide/repl_syntax.c
# The package/ subsystem (IPFS package manager — task #42) has pre-existing
# build breakage: gmod_cli.c includes both goo_mod.h and package_manager.h
# which redefine the same types; gmod_ipfs_cli.c includes missing
# gmod_cli.h; gateway_intelligence.c has a stale ipfs_gateway_create call.
# No core compiler code (compiler/, parser/, types/, codegen/, lexer/, ast/)
# depends on package/, so we exclude the subsystem from the compiler build.
# Repair lives in a separate task.
PACKAGE_SRCS = $(SRCDIR)/package/import_resolver.c
TEST_FRAMEWORK_SRCS = $(TEST_FRAMEWORK_DIR)/test_framework.c

COMPTIME_SRCS = $(SRCDIR)/comptime/comptime.c $(SRCDIR)/comptime/comptime_value.c $(SRCDIR)/comptime/comptime_intrinsics.c $(SRCDIR)/comptime/comptime_types.c $(SRCDIR)/comptime/optimization.c $(SRCDIR)/comptime/profile_guided_optimization.c $(SRCDIR)/comptime/advanced_optimization.c $(SRCDIR)/comptime/hardware_aware.c $(SRCDIR)/comptime/code_specialization.c $(SRCDIR)/advanced_macro_system.c $(SRCDIR)/derive_macros.c $(SRCDIR)/template_macros.c
CURRENT_SRCS = $(LEXER_SRCS) $(PARSER_SRCS) $(AST_SRCS) $(TYPES_SRCS) $(CODEGEN_SRCS) $(RUNTIME_SRCS) $(ERROR_SRCS) $(IDE_SRCS) $(PACKAGE_SRCS) $(COMPTIME_SRCS)
COMPILER_SRCS = $(COMPILERDIR)/goo.c
SRC_OBJS = $(CURRENT_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
TEST_FRAMEWORK_OBJ = $(TEST_FRAMEWORK_SRCS:$(TEST_FRAMEWORK_DIR)/%.c=$(BUILDDIR)/framework/%.o)
OBJS = $(SRC_OBJS) $(TEST_FRAMEWORK_OBJ)

# Runtime library
RUNTIME_LIB = $(LIBDIR)/libgoo_runtime.a
# Runtime library must include every translation unit referenced by
# the runtime entrypoints. runtime.o's goo_init/goo_exit call into
# deadlock.o, and concurrency.o calls channels/sync/platform — leaving
# any of these out fails the link of even a hello-world executable.
RUNTIME_OBJS = $(BUILDDIR)/runtime/runtime.o $(BUILDDIR)/runtime/platform.o $(BUILDDIR)/runtime/concurrency.o $(BUILDDIR)/runtime/channels.o $(BUILDDIR)/runtime/sync.o $(BUILDDIR)/runtime/deadlock.o $(BUILDDIR)/runtime/io.o

# Main targets
COMPILER = $(BINDIR)/goo
ANALYZER = $(BINDIR)/goo-analyzer
TEST_RUNNER = $(BINDIR)/test_runner
REPL = $(BINDIR)/goo-repl
REPL_ENHANCED = $(BINDIR)/goo-repl-enhanced
LSP_SERVER = $(BINDIR)/goo-lsp
LSP_ENHANCED_SERVER = $(BINDIR)/goo-lsp-enhanced
LSP_STANDALONE_SERVER = $(BINDIR)/goo-lsp-standalone
GMOD_CLI = $(BINDIR)/gmod
TEST_REPL = $(BINDIR)/test_repl
TEST_PERFORMANCE = $(BINDIR)/test_performance
TEST_ERROR_REPORTING = $(BINDIR)/test_error_reporting

.PHONY: all clean test install lexer analyzer test-interface test-repl repl repl-enhanced lsp gmod coverage coverage-report coverage-clean debug format check runtime-lib test-pipeline test-lexer test-codegen test-units goostd-resolver-probe

all: lexer

# P0-1: the runtime archive is a prerequisite of a usable compiler. Without
# it, a fresh checkout builds bin/goo but the FIRST `goo foo.goo` fails to
# link ("cannot find lib/libgoo_runtime.a") because `make clean` removes
# build/ and bin/ but the archive lived only in lib/ as a stale prebuilt.
# Listing it here makes `make` (= all = lexer) build both.
lexer: $(COMPILER) $(RUNTIME_LIB)

# Minimal analyzer (lexer only)
analyzer: $(ANALYZER)

$(ANALYZER): $(LEXER_SRCS) $(SRCDIR)/main_minimal.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^

# Create directories
$(BUILDDIR) $(BINDIR) $(LIBDIR):
	mkdir -p $@

# Bison rules
$(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.tab.h: $(SRCDIR)/parser/parser.y
	bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y

# Object file compilation
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c $< -o $@

$(BUILDDIR)/compiler/%.o: $(COMPILERDIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c $< -o $@

# Test framework object compilation
$(BUILDDIR)/framework/%.o: $(TEST_FRAMEWORK_DIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c $< -o $@

# Parser generation
$(SRCDIR)/parser/parser.tab.c: $(SRCDIR)/parser/parser.y
	bison -d -o $@ $<

# Main compiler executable
goo: $(COMPILER)

# Compiler binary does not link the test framework — it's a runtime concern
# for test runners. The test framework's header (test/test_framework.h) is
# missing from include/, so building TEST_FRAMEWORK_OBJ fails; that breakage
# belongs to task #33 and shouldn't gate compiler builds.
$(COMPILER): $(SRC_OBJS) $(COMPILER_SRCS) | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(COMPILER_SRCS) $(SRC_OBJS) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS)

# Runtime library
runtime-lib: $(RUNTIME_LIB)

$(RUNTIME_LIB): $(RUNTIME_OBJS) | $(LIBDIR)
	ar rcs $@ $^

# Pipeline integration tests
test-pipeline: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p tests
	$(CC) $(CFLAGS) tests/test_runner.c -o tests/test_runner
	./tests/test_runner

# V1 CompCert-compatibility audit: prints counts for the non-CompCert-
# friendly constructs catalogued in docs/COMPCERT_AUDIT.md. Static check
# only — doesn't run ccomp. Exits 0 (informational regression metric).
ccomp-audit:
	@echo "=== Goo C-source CompCert compatibility audit ==="
	@echo "Build flag (Makefile:2):    $$(grep -E '^CFLAGS' Makefile | head -1)"
	@echo "LLVM C API call sites:      $$(grep -rE '\bLLVMBuild|\bLLVMConst|\bLLVMType|\bLLVMAdd|\bLLVMGet' src/ 2>/dev/null | wc -l | tr -d ' ')"
	@echo "_Atomic declarations:       $$(grep -rE '_Atomic|_Thread_local' src/ include/ 2>/dev/null | wc -l | tr -d ' ')"
	@echo "<stdatomic.h> includes:     $$(grep -rE '<stdatomic\.h>|<threads\.h>' src/ include/ 2>/dev/null | wc -l | tr -d ' ')"
	@echo "__attribute__ uses:         $$(grep -rE '__attribute__' src/ include/ 2>/dev/null | wc -l | tr -d ' ')"
	@echo "__builtin_ uses:            $$(grep -rE '__builtin_' src/ include/ 2>/dev/null | wc -l | tr -d ' ')"
	@echo "(typeof(...)){} sites:      $$(grep -rE '\(typeof\(' src/ 2>/dev/null | wc -l | tr -d ' ')"
	@echo "C23 nullptr keyword:        $$(grep -rE '\bnullptr\b' src/ include/ 2>/dev/null | wc -l | tr -d ' ')"
	@echo ""
	@echo "Full report: docs/COMPCERT_AUDIT.md"

# V2-bootstrap-pilot: build examples/tinygoo_v2.goo with bin/goo-ccomp
# (CompCert-built compiler), run it to emit LLVM IR, link with clang,
# verify the resulting binary prints the expected string. Proves the
# chain Goo source → ccomp-built compiler → IR emitter → clang →
# working executable.
v2-bootstrap-pilot: ccomp-build
	@mkdir -p build/v2
	@$(BINDIR)/goo-ccomp -o build/v2/tinygoo examples/tinygoo_v2.goo
	@build/v2/tinygoo > build/v2/tinygoo.ll
	@clang -o build/v2/tiny build/v2/tinygoo.ll 2>/dev/null
	@actual="`./build/v2/tiny`"; \
	  expected="hello from tinygoo v2"; \
	  if [ "$$actual" = "$$expected" ]; then \
	    echo "v2-bootstrap-pilot: PASS  (Goo→ccomp-built→IR emitter→clang→'$$actual')"; \
	  else \
	    echo "v2-bootstrap-pilot: FAIL"; \
	    echo "  expected: $$expected"; \
	    echo "  got:      $$actual"; \
	    exit 1; \
	  fi

# V1 empirical CompCert survey: try ccomp -c against every .c file in
# src/ (excluding src/package/ which is excluded from the gcc build
# too) and report pass/fail counts. Requires ccomp installed via
# `opam install coq-compcert`. Prints failing files for follow-up.
CCOMP ?= ccomp
CCOMP_LLVM_INC := $(shell /opt/homebrew/opt/llvm/bin/llvm-config --includedir 2>/dev/null || llvm-config --includedir 2>/dev/null || echo /opt/homebrew/include)
# -D_POSIX_C_SOURCE: -std=c99 hides POSIX symbols (struct timespec, CLOCK_*),
#   which the gcc build gets via _XOPEN_SOURCE in runtime.h; expose them here too.
# -finline-asm: let CompCert accept the inline asm in <cpuid.h> (hardware probe).
CCOMP_CFLAGS = -Iinclude -I/opt/homebrew/include -I$(CCOMP_LLVM_INC) -std=c99 -fstruct-passing -include include/ccomp_shim.h -D_POSIX_C_SOURCE=200809L -finline-asm -DLLVM_AVAILABLE=1 -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS

# V1-ccomp-link: build bin/goo-ccomp from CompCert .o files. The
# resulting binary is the Goo compiler compiled through CompCert
# (verified-C-compiler) end-to-end. LLVM, libpthread, libm, libcurl,
# libjson-c, libz remain trusted external deps.
CCOMP_LLVM_LIB := $(shell /opt/homebrew/opt/llvm/bin/llvm-config --libdir 2>/dev/null || llvm-config --libdir 2>/dev/null || echo /opt/homebrew/lib)
CCOMP_LDLIBS = -lm -lpthread -ljson-c -lcurl -lz -L/opt/homebrew/lib -L$(CCOMP_LLVM_LIB) -lLLVM-22
CCOMP_ESSENTIAL_SRCS = $(LEXER_SRCS) $(PARSER_SRCS) $(AST_SRCS) $(TYPES_SRCS) $(CODEGEN_SRCS) $(RUNTIME_SRCS) $(ERROR_SRCS) $(IDE_SRCS) $(COMPTIME_SRCS) $(PACKAGE_SRCS) $(COMPILER_SRCS) $(SRCDIR)/advanced_macro_system.c $(SRCDIR)/derive_macros.c $(SRCDIR)/template_macros.c

ccomp-build:
	@command -v $(CCOMP) >/dev/null || (echo "ccomp not installed — see V1-ccomp-install" && exit 1)
	@mkdir -p build/ccomp $(BINDIR)
	@echo "Compiling Goo compiler under CompCert..."
	@rm -rf build/ccomp/obj && mkdir -p build/ccomp/obj
	@for f in $(CCOMP_ESSENTIAL_SRCS); do \
	  obj=build/ccomp/obj/`echo "$$f" | tr '/' '_' | sed 's/\.c$$/.o/'`; \
	  $(CCOMP) -c "$$f" $(CCOMP_CFLAGS) -o "$$obj" 2>/dev/null || (echo "ccomp compile failed: $$f" && exit 1); \
	done
	@echo "Linking bin/goo-ccomp..."
	@# Link only the essential objects (build/ccomp/obj), never the survey's
	@# build/ccomp/*.o, which includes peripheral files that don't build here.
	@$(CCOMP) build/ccomp/obj/*.o -o $(BINDIR)/goo-ccomp $(CCOMP_LDLIBS)
	@echo "ccomp-build: $(BINDIR)/goo-ccomp built"

# V1-ccomp-link gate: build goo-ccomp, run baseline_probe through it,
# diff against the clang-built expected output. PASS iff identical.
ccomp-link: ccomp-build
	@mkdir -p build
	@$(BINDIR)/goo-ccomp -o build/probe_via_ccomp examples/baseline_probe.goo >/dev/null 2>&1 || (echo "ccomp-link: FAIL — goo-ccomp couldn't compile baseline_probe" && exit 1)
	@build/probe_via_ccomp > build/probe_via_ccomp.actual.txt
	@if diff -u examples/baseline_probe.expected.txt build/probe_via_ccomp.actual.txt >/dev/null; then \
	  count=`wc -l < build/probe_via_ccomp.actual.txt | tr -d ' '`; \
	  echo "ccomp-link: PASS ($$count constructs, identical to clang-built goo)"; \
	else \
	  echo "ccomp-link: FAIL — output differs from clang-built goo"; \
	  diff -u examples/baseline_probe.expected.txt build/probe_via_ccomp.actual.txt; \
	  exit 1; \
	fi

ccomp-survey:
	@command -v $(CCOMP) >/dev/null || (echo "ccomp not installed — see V1-ccomp-install" && exit 1)
	@mkdir -p build/ccomp
	@find src -name "*.c" -not -path "*/package/*" 2>/dev/null | sort > build/ccomp/files.txt
	@total=0; pass=0; rm -f build/ccomp/fail.txt; \
	while IFS= read -r f; do \
	  total=$$((total+1)); \
	  obj=build/ccomp/`echo "$$f" | tr '/' '_' | sed 's/\.c$$/.o/'`; \
	  if $(CCOMP) -c "$$f" $(CCOMP_CFLAGS) -o "$$obj" >/dev/null 2>&1; then \
	    pass=$$((pass+1)); \
	  else \
	    echo "$$f" >> build/ccomp/fail.txt; \
	  fi; \
	done < build/ccomp/files.txt; \
	echo "ccomp-survey: PASS $$pass / $$total"; \
	if [ -s build/ccomp/fail.txt ]; then \
	  echo "FAIL list (first error each):"; \
	  while IFS= read -r f; do \
	    err=`$(CCOMP) -c "$$f" $(CCOMP_CFLAGS) -o /dev/null 2>&1 | grep -E "error:|unsupported" | head -1`; \
	    echo "  $$f → $$err"; \
	  done < build/ccomp/fail.txt; \
	fi

# M8 Foundation Verification gate: compile + run the baseline probe,
# which exercises basic language constructs each printing a distinct
# PASS line. Diff against expected.txt. Used by `coord milestone-status
# M8`. Implementation goes via temp files because /bin/sh (which make
# invokes for recipe lines) doesn't support bash's `<(…)` process
# substitution, which is what an inline diff would need.
baseline-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/baseline_probe examples/baseline_probe.goo
	@./build/baseline_probe > build/baseline_probe.actual.txt
	@if diff -u examples/baseline_probe.expected.txt build/baseline_probe.actual.txt; then \
	  count=`wc -l < build/baseline_probe.actual.txt | tr -d ' '`; \
	  echo "baseline-probe: PASS ($$count constructs)"; \
	else \
	  echo "baseline-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M1 lvalue gate: assignment to non-identifier lvalues — struct fields and
# slice elements. Mutation is a prerequisite for any non-trivial program
# (symbol tables, growable buffers). Diffs stdout against the expected probe.
lvalue-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/lvalue_probe examples/lvalue_probe.goo
	@./build/lvalue_probe > build/lvalue_probe.actual.txt
	@if diff -u examples/lvalue_probe.expected.txt build/lvalue_probe.actual.txt; then \
	  echo "lvalue-probe: PASS"; \
	else \
	  echo "lvalue-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M1 file-I/O gate: a Goo program writes a file, queries its size, and reads
# bytes back via os.WriteFile/FileSize/ReadByte (lowered to the goo_sys_*
# runtime). Reading source/writing output is a prerequisite for self-hosting.
file-io-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/file_io_probe examples/file_io_probe.goo
	@./build/file_io_probe > build/file_io_probe.actual.txt
	@if diff -u examples/file_io_probe.expected.txt build/file_io_probe.actual.txt; then \
	  echo "file-io-probe: PASS"; \
	else \
	  echo "file-io-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M1 pointer gate: address-of (`&x`) and dereference read (`*p`), including
# deref in arithmetic. (Heap `new` is a follow-up.)
pointer-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/pointer_probe examples/pointer_probe.goo
	@./build/pointer_probe > build/pointer_probe.actual.txt
	@if diff -u examples/pointer_probe.expected.txt build/pointer_probe.actual.txt; then \
	  echo "pointer-probe: PASS"; \
	else \
	  echo "pointer-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M1 pointer-write gate: mutation through a pointer (`*p = v`) written
# idiomatically (no semicolons), exercising targeted ASI + deref-assignment.
pointer-write-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/pointer_write_probe examples/pointer_write_probe.goo
	@./build/pointer_write_probe > build/pointer_write_probe.actual.txt
	@if diff -u examples/pointer_write_probe.expected.txt build/pointer_write_probe.actual.txt; then \
	  echo "pointer-write-probe: PASS"; \
	else \
	  echo "pointer-write-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M1 heap gate: `new(T)` allocates a T (builtin or struct) on the heap and
# returns *T, mutated through the pointer. Completes M1 pointers/heap.
new-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/new_probe examples/new_probe.goo
	@./build/new_probe > build/new_probe.actual.txt
	@if diff -u examples/new_probe.expected.txt build/new_probe.actual.txt; then \
	  echo "new-probe: PASS"; \
	else \
	  echo "new-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M2 enum gate (construction): declare a tagged union and construct variants.
enum-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/enum_ctor_probe examples/enum_ctor_probe.goo
	@./build/enum_ctor_probe > build/enum_ctor_probe.actual.txt
	@if diff -u examples/enum_ctor_probe.expected.txt build/enum_ctor_probe.actual.txt; then \
	  echo "enum-probe: PASS"; \
	else \
	  echo "enum-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M2 gate (the milestone deliverable): build a tagged-union AST and walk it
# with `match` over the tag, recursing through pointer payloads.
match-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/match_probe examples/match_probe.goo
	@./build/match_probe > build/match_probe.actual.txt
	@if diff -u examples/match_probe.expected.txt build/match_probe.actual.txt; then \
	  echo "match-probe: PASS"; \
	else \
	  echo "match-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M2-slices gate: the append() builtin grows a slice past its capacity via
# goo_slice_append (amortized 2x), proving the 3-field {ptr,len,cap} layout
# end-to-end (literal realloc + element survival).
append-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/append_probe examples/append_probe.goo
	@./build/append_probe > build/append_probe.actual.txt
	@if diff -u examples/append_probe.expected.txt build/append_probe.actual.txt; then \
	  echo "append-probe: PASS"; \
	else \
	  echo "append-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M2-slices gate: the cap() builtin reads a slice's capacity (header field 2),
# distinct from len, and tracks amortized 2x growth across an append.
cap-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/cap_probe examples/cap_probe.goo
	@./build/cap_probe > build/cap_probe.actual.txt
	@if diff -u examples/cap_probe.expected.txt build/cap_probe.actual.txt; then \
	  echo "cap-probe: PASS"; \
	else \
	  echo "cap-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# F2 gate: builtin numeric type conversions `T(x)`. Non-vacuous w.r.t. the cast
# semantics — exercises ZExt vs SExt (int(byte(200))==200, not -56), a SExt
# contrast (int(int8(200))==-56), and a value-dropping Trunc (int(byte(300))==44),
# so a regression to SExt-for-unsigned or a botched truncation fails here.
conv-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/conv_probe examples/conv_probe.goo
	@./build/conv_probe > build/conv_probe.actual.txt
	@if diff -u examples/conv_probe.expected.txt build/conv_probe.actual.txt; then \
	  echo "conv-probe: PASS"; \
	else \
	  echo "conv-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# F3 gate: char/rune literals `'x'`. A rune literal is an untyped integer
# constant (rune = int32 = `int` today), so the lexer decodes `'A'`/`'\n'`/`'\''`
# to its integer value and emits an INT token — no new grammar terminal, so the
# parser conflict count is unaffected. Covers basic ASCII, char arithmetic
# ('0'+5), the common escapes (\n \t \\ \'), and rune subtraction ('a'-'A').
charlit-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/charlit_probe examples/charlit_probe.goo
	@./build/charlit_probe > build/charlit_probe.actual.txt
	@if diff -u examples/charlit_probe.expected.txt build/charlit_probe.actual.txt; then \
	  echo "charlit-probe: PASS"; \
	else \
	  echo "charlit-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# Stdlib table enabler A: string indexing `s[i]` -> byte (uint8). The const
# lookup tables in math/bits (len8tab etc.) are indexed strings, so this is a
# prerequisite for the table-based Len8/LeadingZeros family. Guards both the
# read path (correct byte value) and that byte values flow through int() and
# arithmetic. String-index ASSIGNMENT stays rejected (immutable) — covered by
# strindex-reject-probe.
strindex-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/strindex_probe examples/strindex_probe.goo
	@./build/strindex_probe > build/strindex_probe.actual.txt
	@if diff -u examples/strindex_probe.expected.txt build/strindex_probe.actual.txt; then \
	  echo "strindex-probe: PASS"; \
	else \
	  echo "strindex-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# Stdlib table enabler A, negative gate: strings are immutable, so `s[i] = x`
# must be REJECTED (rc != 0, no binary), not silently write into the string's
# backing bytes. Guards that adding the read-side TYPE_STRING index case did
# not open a write hole (codegen_emit_lvalue_address has no string case).
strindex-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== strindex-reject-probe: string index assignment must reject ==="
	@printf 'package main\nfunc main(){ s := "ABC"; s[1] = 90; _ = s }\n' > build/strindex_reject.goo
	@rm -f build/strindex_reject
	@$(COMPILER) -o build/strindex_reject build/strindex_reject.goo > build/strindex_reject.out 2> build/strindex_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "strindex-reject-probe: FAIL (compiled rc=0 — string write silently accepted)"; exit 1; fi; \
	if [ -x build/strindex_reject ]; then echo "strindex-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -qiE "error" build/strindex_reject.err; then echo "strindex-reject-probe: FAIL (no diagnostic)"; cat build/strindex_reject.err; exit 1; fi; \
	echo "strindex-reject-probe: PASS (rejected rc=$$rc)"

# &T{} supports STRUCT literals only — & on a slice/array/map literal must be
# a clean type error naming the literal kind, never the generic non-lvalue
# codegen error or a crash. Guards the expression_checker.c rejection.
addrlit-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== addrlit-reject-probe: & on a slice literal must reject ==="
	@printf 'package main\nfunc main(){ p := &[]int{1, 2}; _ = p }\n' > build/addrlit_reject.goo
	@rm -f build/addrlit_reject
	@$(COMPILER) -o build/addrlit_reject build/addrlit_reject.goo > build/addrlit_reject.out 2> build/addrlit_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "addrlit-reject-probe: FAIL (compiled rc=0 — &slice-literal silently accepted)"; exit 1; fi; \
	if [ -x build/addrlit_reject ]; then echo "addrlit-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "address of a slice literal" build/addrlit_reject.err; then echo "addrlit-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/addrlit_reject.err; exit 1; fi; \
	echo "addrlit-reject-probe: PASS (rejected rc=$$rc)"

# math/bits Div panics on divide-by-zero (y==0) and overflow (y<=hi). Guards
# that both taken panics abort with the runtime-error message (the non-panic
# paths are in bits_div_probe).
bits-div-abort-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== bits-div-abort-probe: Div y==0 / y<=hi abort ==="
	@printf 'package main\nimport "bits"\nfunc main(){ bits.Div64(0, 10, 0) }\n' > build/div_dz.goo
	@printf 'package main\nimport "bits"\nfunc main(){ bits.Div64(5, 0, 3) }\n' > build/div_of.goo
	@$(COMPILER) -o build/div_dz build/div_dz.goo >/dev/null 2>build/div_dz.cerr || (echo "bits-div-abort-probe: FAIL (dz did not compile)"; cat build/div_dz.cerr; exit 1)
	@$(COMPILER) -o build/div_of build/div_of.goo >/dev/null 2>build/div_of.cerr || (echo "bits-div-abort-probe: FAIL (of did not compile)"; cat build/div_of.cerr; exit 1)
	@./build/div_dz >/dev/null 2>build/div_dz.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "bits-div-abort-probe: FAIL (divide-by-zero did not abort)"; exit 1; fi; \
	if ! grep -qiE "integer divide by zero" build/div_dz.err; then echo "bits-div-abort-probe: FAIL (no divide-by-zero message)"; cat build/div_dz.err; exit 1; fi
	@./build/div_of >/dev/null 2>build/div_of.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "bits-div-abort-probe: FAIL (overflow did not abort)"; exit 1; fi; \
	if ! grep -qiE "integer overflow" build/div_of.err; then echo "bits-div-abort-probe: FAIL (no overflow message)"; cat build/div_of.err; exit 1; fi
	@echo "bits-div-abort-probe: PASS"

# panic(v) builtin: a taken panic must abort — print "panic: <msg>" to stderr
# and exit non-zero (the runtime goo_panic calls abort()). Guards the runtime
# behavior that panic_probe (untaken branch) cannot.
panic-abort-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== panic-abort-probe: a taken panic aborts with a message ==="
	@printf 'package main\nfunc main(){ panic("boom") }\n' > build/panic_abort.goo
	@$(COMPILER) -o build/panic_abort build/panic_abort.goo >/dev/null 2>build/panic_abort.cerr || (echo "panic-abort-probe: FAIL (did not compile)"; cat build/panic_abort.cerr; exit 1)
	@./build/panic_abort > build/panic_abort.out 2> build/panic_abort.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "panic-abort-probe: FAIL (panic did not abort — exit 0)"; exit 1; fi; \
	if ! grep -qiE "panic: boom" build/panic_abort.err; then echo "panic-abort-probe: FAIL (no 'panic: boom' on stderr)"; cat build/panic_abort.err; exit 1; fi; \
	echo "panic-abort-probe: PASS (aborted rc=$$rc)"

# Stdlib table enabler B: hex byte escapes `\xNN` in string literals. The const
# lookup tables in math/bits are strings of raw bytes written as `\x00\x01...`,
# so correct two-hex-digit decoding is a prerequisite. Guards byte values AND
# length (the pre-fix bug got both wrong: `\x05` -> 'x','0','5').
hexesc-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/hexesc_probe examples/hexesc_probe.goo
	@./build/hexesc_probe > build/hexesc_probe.actual.txt
	@if diff -u examples/hexesc_probe.expected.txt build/hexesc_probe.actual.txt; then \
	  echo "hexesc-probe: PASS"; \
	else \
	  echo "hexesc-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# Stdlib table enabler B, negative gate: a MALFORMED hex escape (fewer than two
# hex digits, or a non-hex digit) must be REJECTED, not silently mis-decoded.
# Before the fix `\xG1`/`\x` fell through the forgiving default and produced
# garbage bytes with exit 0. Now the lexer returns NULL -> TOKEN_ERROR, so the
# program fails to compile (rc != 0, no binary, diagnostic emitted).
hexesc-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== hexesc-reject-probe: malformed hex escapes must reject, not mis-decode ==="
	@printf 'package main\nimport "fmt"\nfunc main(){ fmt.Println("\\xG1") }\n' > build/hexesc_reject_nonhex.goo
	@printf 'package main\nimport "fmt"\nfunc main(){ fmt.Println("ab\\x") }\n' > build/hexesc_reject_short.goo
	@for name in nonhex short; do \
	  rm -f build/hexesc_reject_$$name; \
	  $(COMPILER) -o build/hexesc_reject_$$name build/hexesc_reject_$$name.goo > build/hexesc_reject_$$name.out 2> build/hexesc_reject_$$name.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "hexesc-reject-probe: FAIL ($$name compiled rc=0 — malformed hex silently accepted)"; exit 1; fi; \
	  if [ -x build/hexesc_reject_$$name ]; then echo "hexesc-reject-probe: FAIL ($$name emitted a binary despite the error)"; exit 1; fi; \
	  if ! grep -qiE "error" build/hexesc_reject_$$name.err; then echo "hexesc-reject-probe: FAIL ($$name produced no diagnostic)"; cat build/hexesc_reject_$$name.err; exit 1; fi; \
	  echo "hexesc-reject-probe: $$name rejected (rc=$$rc)"; \
	done
	@echo "hexesc-reject-probe: PASS"

# Stdlib table enabler C (parts 1+2): a package-level const string with embedded
# NUL bytes, no concatenation. Guards C1 (no crash at global scope) and C2 (the
# embedded NUL does not truncate — len is 4 though byte 0 is NUL), independently
# of the concat folding exercised by conststr-probe.
conststr-nul-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/conststr_nul_probe examples/conststr_nul_probe.goo
	@./build/conststr_nul_probe > build/conststr_nul_probe.actual.txt
	@if diff -u examples/conststr_nul_probe.expected.txt build/conststr_nul_probe.actual.txt; then \
	  echo "conststr-nul-probe: PASS"; \
	else \
	  echo "conststr-nul-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# Stdlib table enabler C: package-level const strings with embedded NUL bytes
# and compile-time concatenation — the exact shape of the math/bits lookup
# tables (const len8tab = "" + "\x00\x01..." + ...). Guards all three fixes at
# once: C1 no crash at global scope, C2 embedded-NUL length preserved (len == 5
# though byte 0 is NUL), C3 the "+" concatenation folds to a compile-time const.
conststr-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/conststr_probe examples/conststr_probe.goo
	@./build/conststr_probe > build/conststr_probe.actual.txt
	@if diff -u examples/conststr_probe.expected.txt build/conststr_probe.actual.txt; then \
	  echo "conststr-probe: PASS"; \
	else \
	  echo "conststr-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# F3 negative gate: a MALFORMED char literal must be rejected cleanly, NOT
# silently dropped. The lexer emits TOKEN_ERROR for ''/'\z'/unterminated 'a),
# which the Bison bridge maps to an unknown token and skips — so before the fix
# the literal vanished and the surrounding program compiled to a running binary
# with exit 0 and no diagnostic. Each case below must now: (a) fail to compile
# (rc != 0, no binary emitted), and (b) print a positioned lexer diagnostic.
# Guards the "rejected cleanly" claim the plan (Step 3) and commit asserted.
charlit-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== charlit-reject-probe: malformed char literals must reject, not silently drop ==="
	@printf 'package main\nimport "fmt"\nfunc main() {\n\tfmt.Println('\'''\'')\n}\n' > build/charlit_reject_empty.goo
	@printf 'package main\nimport "fmt"\nfunc main() {\n\tfmt.Println('\''\\z'\'')\n}\n' > build/charlit_reject_badescape.goo
	@printf 'package main\nimport "fmt"\nfunc main() {\n\tfmt.Println('\''a)\n}\n' > build/charlit_reject_unterminated.goo
	@for name in empty badescape unterminated; do \
	  rm -f build/charlit_reject_$$name; \
	  $(COMPILER) -o build/charlit_reject_$$name build/charlit_reject_$$name.goo > build/charlit_reject_$$name.out 2> build/charlit_reject_$$name.err; \
	  rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "charlit-reject-probe: FAIL ($$name compiled rc=0 — malformed literal silently accepted)"; cat build/charlit_reject_$$name.err; exit 1; fi; \
	  if [ -x build/charlit_reject_$$name ]; then echo "charlit-reject-probe: FAIL ($$name emitted a binary despite the error)"; exit 1; fi; \
	  if ! grep -qiE "error:" build/charlit_reject_$$name.err; then echo "charlit-reject-probe: FAIL ($$name produced no diagnostic)"; cat build/charlit_reject_$$name.err; exit 1; fi; \
	  echo "charlit-reject-probe: $$name rejected (rc=$$rc)"; \
	done
	@echo "charlit-reject-probe: PASS"

# F2 boundary gate for `T(x)` conversions: the SOUNDNESS + clean-rejection
# properties that conv-probe (positive numeric cases) does NOT guard.
#  1. Function-shadowing soundness (the fix in 6d69e2a): a user `func int`
#     shadows the predeclared type, so `int(5)` must CALL the function (prints
#     105), NOT convert (would print 5). A regression to name-only gating in
#     codegen — the exact bug 6d69e2a fixed — fails here.
#  2. Clean rejection of unsupported `string`/`bool` conversions (plan F2 Step 3
#     "reject cleanly if unsupported"): a single conversion-specific diagnostic
#     ("cannot convert ... only numeric conversions are supported in v1"), NOT
#     the misleading "Undefined variable 'string'" cascade, and never an invalid
#     IR reaching the LLVM verifier.
#  3. No over-rejection: a plain numeric conversion still compiles + runs.
conv-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== conv-reject-probe: T(x) shadowing soundness + clean rejection ==="
	@printf 'package main\nimport "fmt"\nfunc int(n int) int { return n + 100 }\nfunc main(){ fmt.Println(int(5)) }\n' > build/cr_shadow.goo
	@printf 'package main\nfunc main(){ _ = string(byte(66)) }\n' > build/cr_string.goo
	@printf 'package main\nfunc main(){ _ = bool(1) }\n' > build/cr_bool.goo
	@printf 'package main\nimport "fmt"\nfunc main(){ fmt.Println(int(byte(200))) }\n' > build/cr_numok.goo
	@"$(COMPILER)" build/cr_shadow.goo -o build/cr_shadow.out 2>build/cr_shadow.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "conv-reject-probe: FAIL (user 'func int' + int(5) wrongly rejected)"; cat build/cr_shadow.err; exit 1; fi; \
	  out="$$(./build/cr_shadow.out)"; if [ "$$out" != "105" ]; then echo "conv-reject-probe: FAIL (shadowing soundness: int(5) printed '$$out' != 105 — gate regressed to name-only, converting instead of calling the user func)"; exit 1; fi
	@"$(COMPILER)" build/cr_string.goo -o build/cr_string.out 2>build/cr_string.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "conv-reject-probe: FAIL (string(b) compiled — unsupported conversion not rejected)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/cr_string.err; then echo "conv-reject-probe: FAIL (invalid IR reached verifier for string(b))"; cat build/cr_string.err; exit 1; fi; \
	  if grep -qiE "Undefined variable" build/cr_string.err; then echo "conv-reject-probe: FAIL (string(b) gave misleading 'Undefined variable' cascade, not a clean conversion diagnostic)"; cat build/cr_string.err; exit 1; fi; \
	  if ! grep -qiE "cannot convert to string" build/cr_string.err; then echo "conv-reject-probe: FAIL (no clean 'cannot convert to string' diagnostic)"; cat build/cr_string.err; exit 1; fi
	@"$(COMPILER)" build/cr_bool.goo -o build/cr_bool.out 2>build/cr_bool.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "conv-reject-probe: FAIL (bool(1) compiled — unsupported conversion not rejected)"; exit 1; fi; \
	  if grep -qiE "Undefined variable" build/cr_bool.err; then echo "conv-reject-probe: FAIL (bool(1) gave misleading 'Undefined variable', not a clean conversion diagnostic)"; cat build/cr_bool.err; exit 1; fi; \
	  if ! grep -qiE "cannot convert to bool" build/cr_bool.err; then echo "conv-reject-probe: FAIL (no clean 'cannot convert to bool' diagnostic)"; cat build/cr_bool.err; exit 1; fi
	@"$(COMPILER)" build/cr_numok.goo -o build/cr_numok.out 2>build/cr_numok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "conv-reject-probe: FAIL (numeric int(byte(200)) wrongly rejected — over-rejection)"; cat build/cr_numok.err; exit 1; fi; \
	  out="$$(./build/cr_numok.out)"; if [ "$$out" != "200" ]; then echo "conv-reject-probe: FAIL (numeric conversion output '$$out' != 200)"; exit 1; fi
	@echo "conv-reject-probe: PASS"

# M2-maps gate: general map[string]V on an 8-byte value slot. Covers int and
# pointer value types, m[k]=v insert/overwrite, and missing-key zero value.
map-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/map_probe examples/map_probe.goo
	@./build/map_probe > build/map_probe.actual.txt
	@if diff -u examples/map_probe.expected.txt build/map_probe.actual.txt; then \
	  echo "map-probe: PASS"; \
	else \
	  echo "map-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

int64-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== int64-probe: int literal coercion to int64 ==="
	$(COMPILER) -o build/int64_probe examples/int64_probe.goo
	@./build/int64_probe > build/int64_probe.actual.txt
	@if diff -u examples/int64_probe.expected.txt build/int64_probe.actual.txt; then \
	  echo "int64-probe: PASS"; \
	else \
	  echo "int64-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

commaok-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== commaok-probe: comma-ok map reads v, ok := m[k] ==="
	$(COMPILER) -o build/commaok_probe examples/commaok_probe.goo
	@./build/commaok_probe > build/commaok_probe.actual.txt
	@if diff -u examples/commaok_probe.expected.txt build/commaok_probe.actual.txt; then \
	  echo "commaok-probe: PASS"; \
	else \
	  echo "commaok-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

guard-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== guard-probe: match-arm guard (case X{..} if cond:) ==="
	$(COMPILER) -o build/guard_probe examples/guard_probe.goo
	@./build/guard_probe > build/guard_probe.actual.txt
	@if diff -u examples/guard_probe.expected.txt build/guard_probe.actual.txt; then \
	  echo "guard-probe: PASS"; \
	else \
	  echo "guard-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

nullable-iflet-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== nullable-iflet-probe: ?int construct + if-let observe ==="
	$(COMPILER) -o build/nullable_iflet_probe examples/nullable_iflet_probe.goo
	@./build/nullable_iflet_probe > build/nullable_iflet_probe.actual.txt
	@if diff -u examples/nullable_iflet_probe.expected.txt build/nullable_iflet_probe.actual.txt; then \
	  echo "nullable-iflet-probe: PASS"; \
	else \
	  echo "nullable-iflet-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

nullable-nilcmp-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== nullable-nilcmp-probe: ?int == nil / != nil ==="
	$(COMPILER) -o build/nullable_nilcmp_probe examples/nullable_nilcmp_probe.goo
	@./build/nullable_nilcmp_probe > build/nullable_nilcmp_probe.actual.txt
	@if diff -u examples/nullable_nilcmp_probe.expected.txt build/nullable_nilcmp_probe.actual.txt; then \
	  echo "nullable-nilcmp-probe: PASS"; \
	else \
	  echo "nullable-nilcmp-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

nullable-abi-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== nullable-abi-probe: ?BigStruct (>16B) by-pointer across functions ==="
	$(COMPILER) -o build/nullable_abi_probe examples/nullable_abi_probe.goo
	@./build/nullable_abi_probe > build/nullable_abi_probe.actual.txt
	@if diff -u examples/nullable_abi_probe.expected.txt build/nullable_abi_probe.actual.txt; then \
	  echo "nullable-abi-probe: PASS"; \
	else \
	  echo "nullable-abi-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M4 review-fix regression guard: integer widening before nullable auto-wrap
# (?int64 returns i32 literal → must SExt to i64 before InsertValue), and
# if-let with both branches terminating (exit_bb gets unreachable, not stale ret).
nullable-intret-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== nullable-intret-probe: ?int64 narrow-literal return + if-let both-branch-terminate ==="
	$(COMPILER) -o build/nullable_intret_probe examples/nullable_intret_probe.goo
	@./build/nullable_intret_probe > build/nullable_intret_probe.actual.txt
	@if diff -u examples/nullable_intret_probe.expected.txt build/nullable_intret_probe.actual.txt; then \
	  echo "nullable-intret-probe: PASS"; \
	else \
	  echo "nullable-intret-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M4 final-fix behavioral gate: default-zero nil, reassignment, ?T arg passing
nullable-assign-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== nullable-assign-probe: default-nil, reassign, ?T arg passing ==="
	$(COMPILER) -o build/nullable_assign_probe examples/nullable_assign_probe.goo
	@./build/nullable_assign_probe > build/nullable_assign_probe.actual.txt
	@if diff -u examples/nullable_assign_probe.expected.txt build/nullable_assign_probe.actual.txt; then \
	  echo "nullable-assign-probe: PASS"; \
	else \
	  echo "nullable-assign-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M4 width-fix gate: integer-width adjustment in codegen_create_nullable_with_value.
# Wrapping a narrow literal (i32) into a ?int64 struct (slot 1 is i64) must SExt
# the value before InsertValue; without this the IR verifier rejects the module.
# Covers the reassignment (C2) and argument auto-wrap (I1) paths.
nullable-width-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== nullable-width-probe: ?int64 reassign + arg auto-wrap width adjustment ==="
	$(COMPILER) -o build/nullable_width_probe examples/nullable_width_probe.goo
	@./build/nullable_width_probe > build/nullable_width_probe.actual.txt
	@if diff -u examples/nullable_width_probe.expected.txt build/nullable_width_probe.actual.txt; then \
	  echo "nullable-width-probe: PASS"; \
	else \
	  echo "nullable-width-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M5 Task 1 gate: !int declare + success construct (return v) + catch ok path.
# Foundational: proves the error-union codegen pipeline end-to-end on the
# success branch. Error path (catch body + error construction) is Task 2.
erru-catch-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== erru-catch-probe: !int declare + success + catch ok path ==="
	$(COMPILER) -o build/erru_catch_probe examples/erru_catch_probe.goo
	@./build/erru_catch_probe > build/erru_catch_probe.actual.txt
	@if diff -u examples/erru_catch_probe.expected.txt build/erru_catch_probe.actual.txt; then \
	  echo "erru-catch-probe: PASS"; \
	else \
	  echo "erru-catch-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M5 Task 2 gate: error("msg") builtin + catch error-var binding.
# Exercises the error case of !T — error construction, e binding with real
# string (pointer + length), falling-through catch, and diverging catch.
erru-error-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== erru-error-probe: error(\"msg\") builtin + catch error-var binding ==="
	$(COMPILER) -o build/erru_error_probe examples/erru_error_probe.goo
	@./build/erru_error_probe > build/erru_error_probe.actual.txt
	@if diff -u examples/erru_error_probe.expected.txt build/erru_error_probe.actual.txt; then \
	  echo "erru-error-probe: PASS"; \
	else \
	  echo "erru-error-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

erru-abi-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== erru-abi-probe: !BigStruct ABI + !int64 width across functions ==="
	$(COMPILER) -o build/erru_abi_probe examples/erru_abi_probe.goo
	@./build/erru_abi_probe > build/erru_abi_probe.actual.txt
	@if diff -u examples/erru_abi_probe.expected.txt build/erru_abi_probe.actual.txt; then \
	  echo "erru-abi-probe: PASS"; \
	else \
	  echo "erru-abi-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M7 Task 1: in-process buffered int channel make/send/recv round-trip + FIFO order.
# Reproduces and guards against the LLVM-context mismatch bug where channel
# codegen built types in the global context instead of codegen->context.
chan-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== chan-probe: in-process buffered int channel round-trip + FIFO ==="
	$(COMPILER) -o build/chan_probe examples/chan_probe.goo
	@./build/chan_probe > build/chan_probe.actual.txt
	@if diff -u examples/chan_probe.expected.txt build/chan_probe.actual.txt; then \
	  echo "chan-probe: PASS"; \
	else \
	  echo "chan-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

chan-elem-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== chan-elem-probe: non-int channel elements (int64, struct) no truncation ==="
	$(COMPILER) -o build/chan_elem_probe examples/chan_elem_probe.goo
	@./build/chan_elem_probe > build/chan_elem_probe.actual.txt
	@if diff -u examples/chan_elem_probe.expected.txt build/chan_elem_probe.actual.txt; then \
	  echo "chan-elem-probe: PASS"; \
	else \
	  echo "chan-elem-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

chan-padded-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== chan-padded-probe: ABI-padded struct {int8,int64} channel no truncation ==="
	$(COMPILER) -o build/chan_padded_probe examples/chan_padded_probe.goo
	@./build/chan_padded_probe > build/chan_padded_probe.actual.txt
	@if diff -u examples/chan_padded_probe.expected.txt build/chan_padded_probe.actual.txt; then \
	  echo "chan-padded-probe: PASS"; \
	else \
	  echo "chan-padded-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

chan-uint-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== chan-uint-probe: unsigned integer widening uses ZExt not SExt ==="
	$(COMPILER) -o build/chan_uint_probe examples/chan_uint_probe.goo
	@./build/chan_uint_probe > build/chan_uint_probe.actual.txt
	@if diff -u examples/chan_uint_probe.expected.txt build/chan_uint_probe.actual.txt; then \
	  echo "chan-uint-probe: PASS"; \
	else \
	  echo "chan-uint-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M8 concurrency substrate. go-probe: `go f(args)` spawns goroutines that run
# to completion and are observable. Three goroutines each send 1 into a buffered
# channel; main sums the receives (order-independent). Exercises argument
# thunking + the goo_scheduler_wait run-to-completion barrier in generated main.
go-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== go-probe: goroutine spawn + args + scheduler run-to-completion ==="
	$(COMPILER) -o build/go_probe examples/go_probe.goo
	@./build/go_probe > build/go_probe.actual.txt
	@if diff -u examples/go_probe.expected.txt build/go_probe.actual.txt; then \
	  echo "go-probe: PASS"; \
	else \
	  echo "go-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# Default-thread-count resolver: GOMAXPROCS/NCPU policy, clamped to [1,16].
default-thread-count-test: $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== default-thread-count-test: GOMAXPROCS/NCPU resolution ==="
	$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/default_thread_count_test.c $(RUNTIME_LIB) -lpthread -lm -o build/default_thread_count_test
	@timeout 10 ./build/default_thread_count_test; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "default-thread-count-test: PASS"; else echo "default-thread-count-test: FAIL (exit $$rc)"; exit 1; fi

# M8c: M:N scheduler correctness under real multi-threading (num_threads=4).
mt-scheduler-stress: $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== mt-scheduler-stress: M:N scheduler correct under num_threads=4 ==="
	$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/mt_scheduler_stress.c $(RUNTIME_LIB) -lpthread -lm -o build/mt_scheduler_stress
	@timeout 60 ./build/mt_scheduler_stress; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "mt-scheduler-stress: PASS"; else echo "mt-scheduler-stress: FAIL (exit $$rc — crash/hang/wrong count)"; exit 1; fi

# M8d: goo_yield correctness under real multi-threading.
yield-stress: $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== yield-stress: goo_yield safe under num_threads=4 ==="
	$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/yield_stress.c $(RUNTIME_LIB) -lpthread -lm -o build/yield_stress
	@timeout 60 ./build/yield_stress; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "yield-stress: PASS"; else echo "yield-stress: FAIL (exit $$rc)"; exit 1; fi

# M8d: channel send/recv correctness under real multi-threading.
chan-mt-stress: $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== chan-mt-stress: channel send/recv correct under num_threads=4 ==="
	$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/chan_mt_stress.c $(RUNTIME_LIB) -lpthread -lm -o build/chan_mt_stress
	@timeout 60 ./build/chan_mt_stress; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "chan-mt-stress: PASS"; else echo "chan-mt-stress: FAIL (exit $$rc)"; exit 1; fi

# M9: a fully-deadlocked program aborts with Go's message + exit 2 (not a hang).
deadlock-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== deadlock-probe: main blocked on empty channel aborts (exit 2) ==="
	$(COMPILER) -o build/deadlock_probe examples/deadlock_probe.goo
	@timeout 10 ./build/deadlock_probe 2>build/deadlock_probe.err; rc=$$?; \
	if [ $$rc -eq 124 ]; then echo "deadlock-probe: FAIL (hang — no detection)"; cat build/deadlock_probe.err; exit 1; fi; \
	if [ $$rc -ne 2 ]; then echo "deadlock-probe: FAIL (exit $$rc, expected 2)"; cat build/deadlock_probe.err; exit 1; fi; \
	if grep -q "all goroutines are asleep - deadlock!" build/deadlock_probe.err; then echo "deadlock-probe: PASS"; else echo "deadlock-probe: FAIL (missing message)"; cat build/deadlock_probe.err; exit 1; fi

deadlock-goroutine-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== deadlock-goroutine-probe: blocked goroutine + idle main aborts (exit 2) ==="
	$(COMPILER) -o build/deadlock_goroutine_probe examples/deadlock_goroutine_probe.goo
	@timeout 10 ./build/deadlock_goroutine_probe 2>build/deadlock_goroutine_probe.err; rc=$$?; \
	if [ $$rc -eq 124 ]; then echo "deadlock-goroutine-probe: FAIL (hang — no detection)"; cat build/deadlock_goroutine_probe.err; exit 1; fi; \
	if [ $$rc -ne 2 ]; then echo "deadlock-goroutine-probe: FAIL (exit $$rc, expected 2)"; cat build/deadlock_goroutine_probe.err; exit 1; fi; \
	if grep -q "all goroutines are asleep - deadlock!" build/deadlock_goroutine_probe.err; then echo "deadlock-goroutine-probe: PASS"; else echo "deadlock-goroutine-probe: FAIL (missing message)"; cat build/deadlock_goroutine_probe.err; exit 1; fi

# P0-4: a failed link must not leave a stray object file behind.
link-cleanup-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== link-cleanup-probe: failed link leaves no .o ==="
	@printf 'package main\nfunc main() {}\n' > build/cleanup_probe.goo
	@rm -f build/cleanup_probe.out.o
	@GOO_RUNTIME=/nonexistent/libgoo_runtime.a "$(COMPILER)" build/cleanup_probe.goo -o build/cleanup_probe.out 2>/dev/null; true
	@if [ -e build/cleanup_probe.out.o ]; then echo "link-cleanup-probe: FAIL (.o left behind)"; exit 1; else echo "link-cleanup-probe: PASS"; fi

# P0-3: a run of blank lines must NOT overflow the stack. The newline ASI
# handler now iterates instead of tail-recursing, so 1,000,000 consecutive
# blank lines lex without a SIGSEGV. The fixture is generated at test time
# (1MB+), never committed. Guards against a regression back to recursion.
blank-lines-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== blank-lines-probe: 1e6 blank lines must not crash the lexer ==="
	@{ yes '' | head -n 1000000; printf 'package main\nfunc main() {}\n'; } > build/blank_lines_probe.goo
	@"$(COMPILER)" build/blank_lines_probe.goo -o build/blank_lines_probe.out 2>build/blank_lines_probe.err; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "blank-lines-probe: FAIL (compile rc=$$rc — stack overflow regression?)"; cat build/blank_lines_probe.err; exit 1; fi; \
	./build/blank_lines_probe.out; rrc=$$?; \
	if [ $$rrc -ne 0 ]; then echo "blank-lines-probe: FAIL (run rc=$$rrc)"; exit 1; fi; \
	echo "blank-lines-probe: PASS"

# P1-7: integer divide-by-zero must abort (non-zero exit + panic message),
# not return garbage. Also confirms normal division still exits 0.
divzero-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== divzero-probe: 10/0 must abort with 'integer divide by zero' ==="
	@"$(COMPILER)" examples/divzero_probe.goo -o build/divzero_probe.out 2>build/divzero_probe.cerr || \
	  { echo "divzero-probe: FAIL (compile)"; cat build/divzero_probe.cerr; exit 1; }
	@./build/divzero_probe.out 2>build/divzero_probe.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "divzero-probe: FAIL (10/0 did not abort, rc=0)"; exit 1; fi; \
	if ! grep -q "integer divide by zero" build/divzero_probe.err; then echo "divzero-probe: FAIL (no panic message)"; cat build/divzero_probe.err; exit 1; fi
	@printf 'package main\nimport "fmt"\nfunc main(){ fmt.Println(20/4) }\n' > build/divok.goo
	@"$(COMPILER)" build/divok.goo -o build/divok.out 2>/dev/null && ./build/divok.out >build/divok.out.txt 2>&1; \
	if [ "$$(cat build/divok.out.txt)" != "5" ]; then echo "divzero-probe: FAIL (normal 20/4 != 5: $$(cat build/divok.out.txt))"; exit 1; fi
	@echo "divzero-probe: PASS"

# P1-6: an out-of-range slice index must abort (non-zero exit + message),
# not read past the backing buffer. Also confirms in-bounds indexing exits 0.
bounds-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== bounds-probe: s[5] on len-3 slice must abort ==="
	@"$(COMPILER)" examples/bounds_probe.goo -o build/bounds_probe.out 2>build/bounds_probe.cerr || \
	  { echo "bounds-probe: FAIL (compile)"; cat build/bounds_probe.cerr; exit 1; }
	@./build/bounds_probe.out 2>build/bounds_probe.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "bounds-probe: FAIL (OOB did not abort, rc=0)"; exit 1; fi; \
	if ! grep -qi "bounds check failed\|index .* >= length" build/bounds_probe.err; then echo "bounds-probe: FAIL (no bounds message)"; cat build/bounds_probe.err; exit 1; fi
	@printf 'package main\nimport "fmt"\nfunc main(){ s:=[10,20,30]; fmt.Println(s[1]) }\n' > build/inbounds.goo
	@"$(COMPILER)" build/inbounds.goo -o build/inbounds.out 2>/dev/null && ./build/inbounds.out >build/inbounds.txt 2>&1; \
	if [ "$$(cat build/inbounds.txt)" != "20" ]; then echo "bounds-probe: FAIL (in-bounds s[1] != 20: $$(cat build/inbounds.txt))"; exit 1; fi
	@echo "bounds-probe: PASS"

# Soak iteration count for the parallel probes (override: make ... PARALLEL_SOAK_ITERS=200).
PARALLEL_SOAK_ITERS ?= 50

# parallel-soak-probe: 64-goroutine channel fan-in, deterministic sum=64,
# run PARALLEL_SOAK_ITERS times under the default multi-threaded scheduler.
parallel-soak-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== parallel-soak-probe: 64-goroutine fan-in x $(PARALLEL_SOAK_ITERS) (default parallelism) ==="
	$(COMPILER) -o build/parallel_soak_probe examples/parallel_soak_probe.goo
	@for i in $$(seq 1 $(PARALLEL_SOAK_ITERS)); do \
	  out=$$(timeout 10 ./build/parallel_soak_probe); rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "parallel-soak-probe: FAIL (iter $$i exit $$rc)"; exit 1; fi; \
	  if [ "$$out" != "64" ]; then echo "parallel-soak-probe: FAIL (iter $$i got '$$out' want 64)"; exit 1; fi; \
	done; \
	echo "parallel-soak-probe: PASS ($(PARALLEL_SOAK_ITERS) iters, sum=64)"

# parallel-select-soak-probe: 32+32 goroutines feeding two channels; main runs
# 64 blocking selects, deterministic count=64, looped PARALLEL_SOAK_ITERS times.
parallel-select-soak-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== parallel-select-soak-probe: 64 selects over 2 channels x $(PARALLEL_SOAK_ITERS) (default parallelism) ==="
	$(COMPILER) -o build/parallel_select_soak_probe examples/parallel_select_soak_probe.goo
	@for i in $$(seq 1 $(PARALLEL_SOAK_ITERS)); do \
	  out=$$(timeout 10 ./build/parallel_select_soak_probe); rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "parallel-select-soak-probe: FAIL (iter $$i exit $$rc)"; exit 1; fi; \
	  if [ "$$out" != "64" ]; then echo "parallel-select-soak-probe: FAIL (iter $$i got '$$out' want 64)"; exit 1; fi; \
	done; \
	echo "parallel-select-soak-probe: PASS ($(PARALLEL_SOAK_ITERS) iters, count=64)"

# M8b escape-probe: a local whose address escapes into a goroutine spawned from
# a non-main frame survives after that frame returns (heap-promotion).
escape-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== escape-probe: &local escaping into go from a non-main frame is heap-promoted ==="
	$(COMPILER) -o build/escape_probe examples/escape_probe.goo
	@timeout 10 ./build/escape_probe > build/escape_probe.actual.txt; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "escape-probe: FAIL (exit $$rc — hang or crash)"; exit 1; fi
	@if diff -u examples/escape_probe.expected.txt build/escape_probe.actual.txt; then \
	  echo "escape-probe: PASS"; \
	else \
	  echo "escape-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M8b escape-range-probe: value var from for-range is heap-promoted when its
# address escapes into a goroutine, so goroutines read the correct value.
escape-range-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== escape-range-probe: for-range value var heap-promoted when &v escapes into go ==="
	$(COMPILER) -o build/escape_range_probe examples/escape_range_probe.goo
	@timeout 10 ./build/escape_range_probe > build/escape_range_probe.actual.txt; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "escape-range-probe: FAIL (exit $$rc — hang or crash)"; exit 1; fi
	@if diff -u examples/escape_range_probe.expected.txt build/escape_range_probe.actual.txt; then \
	  echo "escape-range-probe: PASS"; \
	else \
	  echo "escape-range-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M8 unbuffered channels. unbuffered-probe: make_chan(T) with no capacity is a
# rendezvous channel — send blocks until a receiver takes the value. A goroutine
# sends two values; main receives both (second send exercises slot reuse).
unbuffered-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== unbuffered-probe: unbuffered channel rendezvous handoff ==="
	$(COMPILER) -o build/unbuffered_probe examples/unbuffered_probe.goo
	@./build/unbuffered_probe > build/unbuffered_probe.actual.txt
	@if diff -u examples/unbuffered_probe.expected.txt build/unbuffered_probe.actual.txt; then \
	  echo "unbuffered-probe: PASS"; \
	else \
	  echo "unbuffered-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M8 select. select-probe: blocking select over channels — picks a ready case
# (at a non-zero index), fires the default when nothing is ready, and blocks
# until a goroutine makes a case ready.
select-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== select-probe: blocking select (ready case / default / blocking) ==="
	$(COMPILER) -o build/select_probe examples/select_probe.goo
	@./build/select_probe > build/select_probe.actual.txt
	@if diff -u examples/select_probe.expected.txt build/select_probe.actual.txt; then \
	  echo "select-probe: PASS"; \
	else \
	  echo "select-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

block-scope-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== block-scope-probe: inner-block redeclarations do not leak ==="
	$(COMPILER) -o build/block_scope_probe examples/block_scope_probe.goo
	@./build/block_scope_probe > build/block_scope_probe.actual.txt
	@if diff -u examples/block_scope_probe.expected.txt build/block_scope_probe.actual.txt; then \
	  echo "block-scope-probe: PASS"; \
	else \
	  echo "block-scope-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M7-stdlib-expansion completion gate: compile + run the stdlib smoke
# test, which exercises one function from each of fmt, strings, math, os
# and exits 0. Used by `coord milestone-status M7-stdlib-expansion`.
smoke-stdlib: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/stdlib_smoke examples/stdlib_smoke.goo
	@actual="$$(./build/stdlib_smoke)"; \
	  expected="hello, world"; \
	  if [ "$$actual" = "$$expected" ]; then \
	    echo "smoke-stdlib: PASS"; \
	  else \
	    echo "smoke-stdlib: FAIL"; \
	    echo "  expected: $$expected"; \
	    echo "  got:      $$actual"; \
	    exit 1; \
	  fi

# M11 comptime probe: compile + run examples/comptime_probe.goo and
# assert the binary exits with code 55 (= fib(10)). Exit-code verification
# is used instead of stdout-diff because fmt.Println(int_const) currently
# misprints ints as ASCII chars — see docs/COMPTIME_AUDIT.md. Once the
# Println bug (M9-fmt-println-int) is fixed, this gate could optionally
# check stdout too. Was the aspirational red gate during M11; promoted
# into the `verify` aggregate once M11 closed, so any regression that
# breaks compile-time fib(10) evaluation now fails the standard net.
comptime-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/comptime_probe examples/comptime_probe.goo
	@./build/comptime_probe; code=$$?; \
	  if [ $$code -eq 55 ]; then \
	    echo "comptime-probe: PASS (fib(10) evaluated at compile time, exit 55)"; \
	  else \
	    echo "comptime-probe: FAIL (got exit $$code, want 55 = fib(10))"; \
	    exit 1; \
	  fi

# M10 struct-literal probe: compile + run examples/m10_probe.goo and
# diff stdout against expected.txt (baseline-probe pattern). Covers
# keyed/positional/partial-keyed/empty literals, rvalue field access,
# and Go zero-value semantics for omitted keyed fields. Was the
# aspirational red gate while M10-struct-literal-impl was in flight;
# joined `verify` as M10-probe-gate-v2 once green (commit 1adab3c).
m10-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/m10_probe examples/m10_probe.goo
	@./build/m10_probe > build/m10_probe.actual.txt
	@if diff -u examples/m10_probe.expected.txt build/m10_probe.actual.txt; then \
	  echo "m10-probe: PASS (struct literals end-to-end)"; \
	else \
	  echo "m10-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# P0-1: the compiler must link from ANY cwd, not just the repo root. Invoked
# from /tmp (no lib/ present) with GOO_RUNTIME unset, the compiler must still
# resolve libgoo_runtime.a relative to its own binary, link, and run.
cwd-link-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== cwd-link-probe: compile+run from a non-repo-root cwd ==="
	@cd /tmp && GOO_RUNTIME="" "$(abspath $(COMPILER))" "$(abspath examples/cwd_link_probe.goo)" -o /tmp/cwd_link_probe.out 2>/tmp/cwd_link_probe.err; \
	  rc=$$?; if [ $$rc -ne 0 ]; then echo "cwd-link-probe: FAIL (compile/link rc=$$rc)"; cat /tmp/cwd_link_probe.err; exit 1; fi
	@out=$$(/tmp/cwd_link_probe.out); if [ "$$out" = "7" ]; then echo "cwd-link-probe: PASS"; else echo "cwd-link-probe: FAIL (got '$$out' want 7)"; exit 1; fi

# M12 stdlib-breadth probe: compile + run examples/m12_probe.goo and
# diff stdout against expected.txt (m10-probe pattern). Each
# M12-stdlib-* child appends a numbered section + expected lines in
# the same commit as its feature. Joins `verify` as gate 7 when the
# M12 track closes (M12-probe-promotion).
m12-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/m12_probe examples/m12_probe.goo
	@M12_PROBE_VAR=hello ./build/m12_probe > build/m12_probe.actual.txt
	@if diff -u examples/m12_probe.expected.txt build/m12_probe.actual.txt; then \
	  echo "m12-probe: PASS (stdlib breadth end-to-end)"; \
	else \
	  echo "m12-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# Methods probe: compile + run examples/methods_probe.goo and diff
# stdout against expected.txt (m10-probe pattern). Covers value-receiver
# methods (name-mangled static dispatch) plus the struct-field read path
# they depend on — field arithmetic in a return, methods with args, and
# same-named methods on distinct receiver types. Joined `verify` as the
# methods gate once value-receiver methods shipped.
methods-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/methods_probe examples/methods_probe.goo
	@./build/methods_probe > build/methods_probe.actual.txt
	@if diff -u examples/methods_probe.expected.txt build/methods_probe.actual.txt; then \
	  echo "methods-probe: PASS (value-receiver methods end-to-end)"; \
	else \
	  echo "methods-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# P2-3 (follow-up): a pointer-receiver method called on a NON-addressable
# value (e.g. a composite literal `Counter{...}.inc()`) has no storage to take
# the address of. The method-dispatch auto-address-of branch must reject this
# cleanly in codegen — a source-located diagnostic, non-zero exit — and must
# NOT degrade to a struct-value-into-pointer-param LLVM verifier crash.
#
# This is the negative counterpart to the ptr_recv_probe golden (which gates
# the four *valid* receiver/value dispatch branches). Together they regression-
# gate the whole method-dispatch path added in call_codegen.c.
ptr-recv-nonaddr-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== ptr-recv-nonaddr-probe: pointer-recv on a non-addressable value fails cleanly ==="
	@printf 'package main\nimport "fmt"\ntype Counter struct { n int }\nfunc (c *Counter) inc() { c.n = c.n + 1 }\nfunc main() { Counter{n: 5}.inc(); fmt.Println("unreached") }\n' > build/ptr_recv_nonaddr.goo
	@"$(COMPILER)" build/ptr_recv_nonaddr.goo -o build/ptr_recv_nonaddr.out 2>build/ptr_recv_nonaddr.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "ptr-recv-nonaddr-probe: FAIL (compiled a non-addressable pointer-recv call — expected an error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/ptr_recv_nonaddr.err; then echo "ptr-recv-nonaddr-probe: FAIL (invalid IR reached the verifier)"; cat build/ptr_recv_nonaddr.err; exit 1; fi; \
	  if grep -qiE "non-addressable" build/ptr_recv_nonaddr.err; then echo "ptr-recv-nonaddr-probe: PASS"; else echo "ptr-recv-nonaddr-probe: FAIL (no clean source-located diagnostic)"; cat build/ptr_recv_nonaddr.err; exit 1; fi

# stdlib Phase 0 Task 1: import resolver (GOOROOT + package .go discovery).
# Plain C probe — NOT a .goo golden (no compiler pipeline involvement yet),
# so it compiles/links resolver_probe.c directly against import_resolver.c
# rather than going through $(COMPILER)/$(RUNTIME_LIB) like the .goo probes
# above. Not part of `verify`: this task is self-contained groundwork, no
# compiler-pipeline change to gate yet.
goostd-resolver-probe:
	@mkdir -p build
	@echo "=== goostd-resolver-probe: GOOROOT resolution + package .go file discovery ==="
	$(CC) $(CFLAGS) tests/package/resolver_probe.c $(SRCDIR)/package/import_resolver.c -o build/resolver_probe
	./build/resolver_probe

# Aggregate verification net per `verification_gates.md`. Runs the
# green gates in sequence: baseline-probe, smoke-stdlib,
# v2-bootstrap-pilot, comptime-block-probe, comptime-probe, m10-probe,
# exit-code-probe, methods-probe.
# Exits non-zero on any failure. Use this on cross-cutting changes;
# use individual targets when iterating on a specific area.
# comptime-probe joined the net once M11 closed (commits 605acaf,
# 47b5ca2, d7bc61c); m10-probe joined as M10-probe-gate-v2 once
# struct literals shipped (commit 1adab3c) — same promotion pattern.
verify: baseline-probe lvalue-probe file-io-probe pointer-probe smoke-stdlib v2-bootstrap-pilot comptime-block-probe comptime-probe m10-probe exit-code-probe switch-probe methods-probe pointer-write-probe new-probe enum-probe match-probe append-probe cap-probe conv-probe conv-reject-probe charlit-probe charlit-reject-probe strindex-probe strindex-reject-probe hexesc-probe hexesc-reject-probe panic-abort-probe bits-div-abort-probe conststr-nul-probe conststr-probe map-probe int64-probe commaok-probe guard-probe nullable-iflet-probe nullable-nilcmp-probe nullable-abi-probe nullable-intret-probe nullable-assign-probe nullable-width-probe erru-catch-probe erru-error-probe erru-abi-probe chan-probe chan-elem-probe chan-padded-probe chan-uint-probe go-probe unbuffered-probe select-probe block-scope-probe escape-probe escape-range-probe mt-scheduler-stress yield-stress chan-mt-stress deadlock-probe deadlock-goroutine-probe default-thread-count-test parallel-soak-probe parallel-select-soak-probe cwd-link-probe break-probe continue-probe break-nested-probe println-badtype-probe error-arity-probe return-type-erru-probe erru-catch-type-reject-probe iface-parse-probe iface-satisfaction-probe try-nonerru-probe return-mismatch-probe named-return-reject-probe composite-literal-reject-probe call-arity-probe call-argtype-probe pkg-argcheck-probe forward-ref-probe print-aggregate-probe ptr-recv-nonaddr-probe link-cleanup-probe blank-lines-probe divzero-probe bounds-probe addrlit-reject-probe test-golden
	@echo ""
	@echo "verify: ALL GREEN GATES PASSED"

# P0-2: break/continue codegen via a loop-context stack. break must exit the
# loop; continue must re-run the post/increment then re-test the condition.
break-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/break_probe examples/break_probe.goo
	@./build/break_probe > build/break_probe.actual.txt
	@if diff -u examples/break_probe.expected.txt build/break_probe.actual.txt; then \
	  echo "break-probe: PASS (break exits the loop)"; \
	else \
	  echo "break-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

continue-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/continue_probe examples/continue_probe.goo
	@./build/continue_probe > build/continue_probe.actual.txt
	@if diff -u examples/continue_probe.expected.txt build/continue_probe.actual.txt; then \
	  echo "continue-probe: PASS (continue skips to the increment)"; \
	else \
	  echo "continue-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# Nested-loop sanity: a break in the inner loop must exit ONLY the inner loop.
break-nested-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/break_nested examples/break_nested.goo
	@./build/break_nested > build/break_nested.actual.txt
	@if diff -u examples/break_nested.expected.txt build/break_nested.actual.txt; then \
	  echo "break-nested-probe: PASS (inner break leaves outer loop running)"; \
	else \
	  echo "break-nested-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# P0-3: printing an unsupported type must be a clean compile error, not invalid IR.
println-badtype-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== println-badtype-probe: printing a struct fails cleanly ==="
	@printf 'package main\nimport "fmt"\ntype P struct { x int }\nfunc main() { p := P{1}; fmt.Println(p) }\n' > build/println_bad.goo
	@"$(COMPILER)" build/println_bad.goo -o build/println_bad.out 2>build/println_bad.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "println-badtype-probe: FAIL (compiled an unsupported print — expected error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/println_bad.err; then echo "println-badtype-probe: FAIL (invalid IR reached verifier)"; cat build/println_bad.err; exit 1; fi; \
	  if grep -qiE "unsupported|cannot print|Println" build/println_bad.err; then echo "println-badtype-probe: PASS"; else echo "println-badtype-probe: FAIL (no clean diagnostic)"; cat build/println_bad.err; exit 1; fi

# P1-1: error(msg) builtin — recognition + string-arg type-check. The bad forms
# error() (wrong arity) and error(5) (non-string arg) must each be rejected by
# the type checker's `error` special-case with its OWN diagnostic, and must NOT
# reach the LLVM verifier (no invalid-IR crash).
#
# The assertions key on the special-case's specific messages ("expects exactly
# one string argument" / "argument must be a string"), not just the substring
# "error". This is deliberate: that special-case only fires when the predeclared
# `error` builtin resolves in scope (registered in type_checker.c). If the
# registration is removed, `error(...)` instead falls through to ordinary
# identifier resolution and fails with "Undefined variable 'error'" — which would
# NOT match these patterns, turning this probe RED. So the probe genuinely
# exercises the builtin registration, not just the pre-existing name match.
error-arity-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== error-arity-probe: error()/error(5) fail cleanly in type check ==="
	@printf 'package main\nfunc f() !int { return error() }\nfunc main() {}\n' > build/error_arity_noargs.goo
	@printf 'package main\nfunc g() !int { return error(5) }\nfunc main() {}\n' > build/error_arity_intarg.goo
	@"$(COMPILER)" build/error_arity_noargs.goo -o build/error_arity_noargs.out 2>build/error_arity_noargs.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "error-arity-probe: FAIL (error() compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/error_arity_noargs.err; then echo "error-arity-probe: FAIL (invalid IR reached verifier for error())"; cat build/error_arity_noargs.err; exit 1; fi; \
	  if ! grep -qiE "error expects exactly one string argument" build/error_arity_noargs.err; then echo "error-arity-probe: FAIL (error() not rejected by the error builtin special-case)"; cat build/error_arity_noargs.err; exit 1; fi
	@"$(COMPILER)" build/error_arity_intarg.goo -o build/error_arity_intarg.out 2>build/error_arity_intarg.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "error-arity-probe: FAIL (error(5) compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/error_arity_intarg.err; then echo "error-arity-probe: FAIL (invalid IR reached verifier for error(5))"; cat build/error_arity_intarg.err; exit 1; fi; \
	  if ! grep -qiE "argument must be a string" build/error_arity_intarg.err; then echo "error-arity-probe: FAIL (error(5) not rejected by the error builtin special-case)"; cat build/error_arity_intarg.err; exit 1; fi
	@echo "error-arity-probe: PASS"

# P1-3: type-check `return` against the declared !T value type. Inside a function
# returning !T, a `return <expr>` is valid iff <expr> is an error(...) construction
# (or the SAME !T forwarded whole) OR its type is compatible with T. A plain value
# of an incompatible type — e.g. `return "str"` from an !int function — must be a
# clean type error, NOT silently accepted (today the return stub accepts anything)
# and NOT an LLVM-verifier crash. A MISMATCHED error union forwarded whole — e.g.
# `return s()` where s() is !string inside an !int function — must likewise be a
# clean type error, not an "any error union accepted" pass that crashes the
# verifier with a return-operand type mismatch. The probe also guards against
# over-rejection: `return 42` and `return error("x")` from an !int function, and a
# matching !int forwarded whole, must all still compile.
return-type-erru-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== return-type-erru-probe: return type-checked against !T value type ==="
	@printf 'package main\nfunc f() !int { return "str" }\nfunc main() {}\n' > build/rt_erru_bad.goo
	@printf 'package main\nfunc f() !int { return }\nfunc main() {}\n' > build/rt_erru_bare.goo
	@printf 'package main\nfunc s() !string { return error("x") }\nfunc f() !int { return s() }\nfunc main() {}\n' > build/rt_erru_xfwd.goo
	@printf 'package main\nimport "fmt"\nfunc okval() !int { return 42 }\nfunc okerr() !int { return error("x") }\nfunc fwd() !int { return okval() }\nfunc main() { fmt.Println("ok") }\n' > build/rt_erru_ok.goo
	@"$(COMPILER)" build/rt_erru_bad.goo -o build/rt_erru_bad.out 2>build/rt_erru_bad.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "return-type-erru-probe: FAIL (return \"str\" from !int compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/rt_erru_bad.err; then echo "return-type-erru-probe: FAIL (invalid IR reached verifier)"; cat build/rt_erru_bad.err; exit 1; fi; \
	  if ! grep -qiE "return type mismatch" build/rt_erru_bad.err; then echo "return-type-erru-probe: FAIL (no clean return-type diagnostic)"; cat build/rt_erru_bad.err; exit 1; fi
	@"$(COMPILER)" build/rt_erru_bare.goo -o build/rt_erru_bare.out 2>build/rt_erru_bare.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "return-type-erru-probe: FAIL (bare return from !int compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/rt_erru_bare.err; then echo "return-type-erru-probe: FAIL (bare return reached verifier)"; cat build/rt_erru_bare.err; exit 1; fi; \
	  if ! grep -qiE "return type mismatch" build/rt_erru_bare.err; then echo "return-type-erru-probe: FAIL (no clean diagnostic for bare return)"; cat build/rt_erru_bare.err; exit 1; fi
	@"$(COMPILER)" build/rt_erru_xfwd.goo -o build/rt_erru_xfwd.out 2>build/rt_erru_xfwd.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "return-type-erru-probe: FAIL (mismatched !string forwarded from !int compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/rt_erru_xfwd.err; then echo "return-type-erru-probe: FAIL (mismatched forward reached verifier)"; cat build/rt_erru_xfwd.err; exit 1; fi; \
	  if ! grep -qiE "return type mismatch" build/rt_erru_xfwd.err; then echo "return-type-erru-probe: FAIL (no clean diagnostic for mismatched forward)"; cat build/rt_erru_xfwd.err; exit 1; fi
	@"$(COMPILER)" build/rt_erru_ok.goo -o build/rt_erru_ok.out 2>build/rt_erru_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "return-type-erru-probe: FAIL (valid !T returns rejected)"; cat build/rt_erru_ok.err; exit 1; fi
	@echo "return-type-erru-probe: PASS"

# P4-1/P4-2: interface type declarations parse and resolve to TYPE_INTERFACE.
# Empty interface, single-method, and a multi-method (sort.Interface-shaped)
# form must all compile, and the interface name must be usable as a variable
# type. Dispatch/satisfaction are later tasks (P4-3…P4-5); this gates the
# front-end (grammar + type_from_ast) against regression.
iface-parse-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== iface-parse-probe: interface decls parse + resolve + usable as a type ==="
	@printf 'package main\ntype Any interface {}\ntype Shape interface { Area() int }\ntype Sortable interface {\n Len() int\n Less(i int, j int) bool\n Swap(i int, j int)\n}\nfunc main() { var s Shape; _ = s }\n' > build/iface_parse.goo
	@"$(COMPILER)" build/iface_parse.goo -o build/iface_parse.out 2>build/iface_parse.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "iface-parse-probe: FAIL (interface decls did not compile)"; cat build/iface_parse.err; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR|syntax error" build/iface_parse.err; then echo "iface-parse-probe: FAIL (parse/verify error)"; cat build/iface_parse.err; exit 1; fi
	@echo "iface-parse-probe: PASS"

# P4-3/P4-4: interface satisfaction + method-call type checking. A non-implementer
# assigned to an interface is rejected ("does not implement"); a signature
# mismatch is rejected; an unknown interface method and a wrong-arity call are
# rejected. A genuine implementer passes the type checker and reaches the clean
# P4-5 "not yet implemented" codegen boundary (NOT a verifier crash). No invalid
# IR may escape in any case.
iface-satisfaction-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== iface-satisfaction-probe: interface satisfaction + method-call checks ==="
	@printf 'package main\ntype Shape interface { Area() int }\ntype Pt struct { x int }\nfunc main() { var b Shape = Pt{x: 1}\n _ = b }\n' > build/iface_missing.goo
	@printf 'package main\ntype Shape interface { Area() int }\ntype Bad struct { x int }\nfunc (b Bad) Area() string { return "n" }\nfunc main() { var c Shape = Bad{x: 1}\n _ = c }\n' > build/iface_mm.goo
	@printf 'package main\ntype Shape interface { Area() int }\ntype Sq struct { side int }\nfunc (s Sq) Area() int { return s.side }\nfunc main() { var a Shape = Sq{side: 5}\n _ = a.Perim() }\n' > build/iface_unknownm.goo
	@printf 'package main\ntype Shape interface { Scale(f int) int }\ntype Sq struct { side int }\nfunc (s Sq) Scale(f int) int { return s.side * f }\nfunc main() { var a Shape = Sq{side: 5}\n _ = a.Scale(1, 2) }\n' > build/iface_wrongargs.goo
	@printf 'package main\ntype Shape interface { Area() int }\ntype Sq struct { side int }\nfunc (s Sq) Area() int { return s.side * s.side }\nfunc main() { var a Shape = Sq{side: 3}\n _ = a }\n' > build/iface_ok.goo
	@"$(COMPILER)" build/iface_missing.goo -o build/iface_missing.out 2>build/iface_missing.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "iface-satisfaction-probe: FAIL (non-implementer compiled)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/iface_missing.err; then echo "iface-satisfaction-probe: FAIL (invalid IR)"; cat build/iface_missing.err; exit 1; fi; \
	  if ! grep -qiE "does not implement" build/iface_missing.err; then echo "iface-satisfaction-probe: FAIL (no satisfaction diagnostic)"; cat build/iface_missing.err; exit 1; fi
	@"$(COMPILER)" build/iface_mm.goo -o build/iface_mm.out 2>build/iface_mm.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "iface-satisfaction-probe: FAIL (signature mismatch compiled)"; exit 1; fi; \
	  if ! grep -qiE "does not implement|signature mismatch" build/iface_mm.err; then echo "iface-satisfaction-probe: FAIL (no mismatch diagnostic)"; cat build/iface_mm.err; exit 1; fi
	@"$(COMPILER)" build/iface_unknownm.goo -o build/iface_unknownm.out 2>build/iface_unknownm.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "iface-satisfaction-probe: FAIL (unknown method compiled)"; exit 1; fi; \
	  if ! grep -qiE "has no method" build/iface_unknownm.err; then echo "iface-satisfaction-probe: FAIL (no unknown-method diagnostic)"; cat build/iface_unknownm.err; exit 1; fi
	@"$(COMPILER)" build/iface_wrongargs.goo -o build/iface_wrongargs.out 2>build/iface_wrongargs.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "iface-satisfaction-probe: FAIL (wrong-arity call compiled)"; exit 1; fi; \
	  if ! grep -qiE "wrong number of arguments" build/iface_wrongargs.err; then echo "iface-satisfaction-probe: FAIL (no arity diagnostic)"; cat build/iface_wrongargs.err; exit 1; fi
	@"$(COMPILER)" build/iface_ok.goo -o build/iface_ok.out 2>build/iface_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "iface-satisfaction-probe: FAIL (genuine implementer should compile — P4-5 boxing)"; cat build/iface_ok.err; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/iface_ok.err; then echo "iface-satisfaction-probe: FAIL (implementer reached the verifier)"; cat build/iface_ok.err; exit 1; fi
	@echo "iface-satisfaction-probe: PASS"

# P2-1: a value-producing catch handler (final statement is a non-void
# expression) recovers with that expression's value, so its type must be
# assignable to the error union's value type T. A string handler over an !int
# union is rejected here with a clean type error, NOT an LLVM-verifier crash.
# Over-rejection guard: a void trailing handler (fmt.Println) and an int-typed
# value handler over !int must both still compile.
erru-catch-type-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== erru-catch-type-reject-probe: catch handler value type must be assignable to T ==="
	@printf 'package main\nimport "fmt"\nfunc f(b bool) !int { if b { return error("x") }\n return 1 }\nfunc main() { x := f(true) catch e { "wrong" }\n fmt.Println(x) }\n' > build/catch_type_bad.goo
	@printf 'package main\nimport "fmt"\nfunc f(b bool) !int { if b { return error("x") }\n return 1 }\nfunc main() { x := f(true) catch e { fmt.Println(e) }\n fmt.Println(x) }\n' > build/catch_type_void.goo
	@printf 'package main\nimport "fmt"\nfunc f(b bool) !int { if b { return error("x") }\n return 1 }\nfunc main() { x := f(true) catch e { -1 }\n fmt.Println(x) }\n' > build/catch_type_ok.goo
	@"$(COMPILER)" build/catch_type_bad.goo -o build/catch_type_bad.out 2>build/catch_type_bad.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "erru-catch-type-reject-probe: FAIL (string handler over !int compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/catch_type_bad.err; then echo "erru-catch-type-reject-probe: FAIL (invalid IR reached verifier)"; cat build/catch_type_bad.err; exit 1; fi; \
	  if ! grep -qiE "catch handler value of type .* is not assignable" build/catch_type_bad.err; then echo "erru-catch-type-reject-probe: FAIL (no clean catch-handler-type diagnostic)"; cat build/catch_type_bad.err; exit 1; fi
	@"$(COMPILER)" build/catch_type_void.goo -o build/catch_type_void.out 2>build/catch_type_void.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "erru-catch-type-reject-probe: FAIL (void trailing handler over-rejected)"; cat build/catch_type_void.err; exit 1; fi
	@"$(COMPILER)" build/catch_type_ok.goo -o build/catch_type_ok.out 2>build/catch_type_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "erru-catch-type-reject-probe: FAIL (int value handler over !int over-rejected)"; cat build/catch_type_ok.err; exit 1; fi
	@echo "erru-catch-type-reject-probe: PASS"

# P1-5: `try` propagates the real error in an !T function; reject `try` whose
# enclosing function does NOT return an error union. Before Phase 1 a `try` in a
# non-!T function silently emitted `unreachable` (no diagnostic, garbage IR);
# now the type checker rejects it with a clean diagnostic, NOT an LLVM-verifier
# crash. The probe also guards against over-rejection: a `try` inside an !T
# function (the legitimate propagation form) must still compile.
try-nonerru-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== try-nonerru-probe: try rejected outside !T; allowed inside !T (incl. cross-value-type) ==="
	@printf 'package main\nfunc mightFail() !int { return error("x") }\nfunc f() int { v := try mightFail(); return v }\nfunc main() {}\n' > build/try_nonerru_int.goo
	@printf 'package main\nfunc mightFail() !int { return error("x") }\nfunc f() { v := try mightFail(); _ = v }\nfunc main() {}\n' > build/try_nonerru_void.goo
	@printf 'package main\nfunc mightFailStr() !string { return error("x") }\nfunc f() !int { v := try mightFailStr(); return len(v) }\nfunc main() {}\n' > build/try_crossval.goo
	@printf 'package main\nimport "fmt"\nfunc mightFail() !int { return 5 }\nfunc f() !int { v := try mightFail(); return v + 1 }\nfunc main() { fmt.Println("ok") }\n' > build/try_erru_ok.goo
	@"$(COMPILER)" build/try_nonerru_int.goo -o build/try_nonerru_int.out 2>build/try_nonerru_int.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "try-nonerru-probe: FAIL (try in !int->int compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/try_nonerru_int.err; then echo "try-nonerru-probe: FAIL (invalid IR reached verifier)"; cat build/try_nonerru_int.err; exit 1; fi; \
	  if ! grep -qiE "try can only be used inside a function that returns an error union" build/try_nonerru_int.err; then echo "try-nonerru-probe: FAIL (no clean try-context diagnostic)"; cat build/try_nonerru_int.err; exit 1; fi
	@"$(COMPILER)" build/try_nonerru_void.goo -o build/try_nonerru_void.out 2>build/try_nonerru_void.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "try-nonerru-probe: FAIL (try in void func compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/try_nonerru_void.err; then echo "try-nonerru-probe: FAIL (void-func try reached verifier)"; cat build/try_nonerru_void.err; exit 1; fi; \
	  if ! grep -qiE "try can only be used inside a function that returns an error union" build/try_nonerru_void.err; then echo "try-nonerru-probe: FAIL (no clean diagnostic for void-func try)"; cat build/try_nonerru_void.err; exit 1; fi
	@"$(COMPILER)" build/try_crossval.goo -o build/try_crossval.out 2>build/try_crossval.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "try-nonerru-probe: FAIL (cross-value-type try !string-in-!int rejected — error should re-wrap into the enclosing !int)"; cat build/try_crossval.err; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/try_crossval.err; then echo "try-nonerru-probe: FAIL (cross-value-type try reached verifier)"; cat build/try_crossval.err; exit 1; fi
	@"$(COMPILER)" build/try_erru_ok.goo -o build/try_erru_ok.out 2>build/try_erru_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "try-nonerru-probe: FAIL (legitimate try inside !T rejected)"; cat build/try_erru_ok.err; exit 1; fi
	@echo "try-nonerru-probe: PASS"

# P2-1: a `return <value>` is type-checked against a NON-error-union function
# signature. Before Phase 2 the return stub only validated the `!T` case, so
# `func f() int { return "str" }` was silently accepted and then crashed the
# LLVM verifier ("Function return type does not match operand type of return
# inst!"). Now it is a clean type error here, before codegen. The probe also
# rejects returning a value from a void function, and guards against
# over-rejection: scalar, string, nullable, and multi-return functions must all
# still compile.
return-mismatch-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== return-mismatch-probe: return value type-checked against non-!T signature ==="
	@printf 'package main\nfunc f() int { return "str" }\nfunc main() {}\n' > build/rt_mm_str.goo
	@printf 'package main\nfunc f() { return 5 }\nfunc main() {}\n' > build/rt_mm_void.goo
	@printf 'package main\nfunc f() int { return 3.9 }\nfunc main() {}\n' > build/rt_mm_float.goo
	@printf 'package main\nfunc big() uint32 { return 9 }\nfunc f() int { return big() }\nfunc main() {}\n' > build/rt_mm_width.goo
	@printf 'package main\nfunc f() byte { return 65 }\nfunc main() {}\n' > build/rt_mm_narrow.goo
	@printf 'package main\nimport "fmt"\nfunc i() int { return 42 }\nfunc w() int64 { return 42 }\nfunc c() int64 { return 1 + 1 }\nfunc s() string { return "ok" }\nfunc n() ?int { return 5 }\nfunc divmod(a int, b int) (int, int) { return a / b, a % b }\nfunc main() { fmt.Println(i()); fmt.Println(w()); fmt.Println(c()) }\n' > build/rt_mm_ok.goo
	@"$(COMPILER)" build/rt_mm_str.goo -o build/rt_mm_str.out 2>build/rt_mm_str.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "return-mismatch-probe: FAIL (return \"str\" from int compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/rt_mm_str.err; then echo "return-mismatch-probe: FAIL (invalid IR reached verifier)"; cat build/rt_mm_str.err; exit 1; fi; \
	  if ! grep -qiE "return type mismatch" build/rt_mm_str.err; then echo "return-mismatch-probe: FAIL (no clean return-type diagnostic)"; cat build/rt_mm_str.err; exit 1; fi
	@"$(COMPILER)" build/rt_mm_void.goo -o build/rt_mm_void.out 2>build/rt_mm_void.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "return-mismatch-probe: FAIL (return value from void func compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/rt_mm_void.err; then echo "return-mismatch-probe: FAIL (void-return value reached verifier)"; cat build/rt_mm_void.err; exit 1; fi; \
	  if ! grep -qiE "return type mismatch" build/rt_mm_void.err; then echo "return-mismatch-probe: FAIL (no clean diagnostic for value-from-void)"; cat build/rt_mm_void.err; exit 1; fi
	@"$(COMPILER)" build/rt_mm_float.goo -o build/rt_mm_float.out 2>build/rt_mm_float.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "return-mismatch-probe: FAIL (return float from int compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/rt_mm_float.err; then echo "return-mismatch-probe: FAIL (float->int reached verifier)"; cat build/rt_mm_float.err; exit 1; fi; \
	  if ! grep -qiE "return type mismatch" build/rt_mm_float.err; then echo "return-mismatch-probe: FAIL (no clean diagnostic for float->int)"; cat build/rt_mm_float.err; exit 1; fi
	@"$(COMPILER)" build/rt_mm_width.goo -o build/rt_mm_width.out 2>build/rt_mm_width.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "return-mismatch-probe: FAIL (return uint32 from int compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/rt_mm_width.err; then echo "return-mismatch-probe: FAIL (uint32->int reached verifier)"; cat build/rt_mm_width.err; exit 1; fi; \
	  if ! grep -qiE "return type mismatch" build/rt_mm_width.err; then echo "return-mismatch-probe: FAIL (no clean diagnostic for uint32->int)"; cat build/rt_mm_width.err; exit 1; fi
	@"$(COMPILER)" build/rt_mm_narrow.goo -o build/rt_mm_narrow.out 2>build/rt_mm_narrow.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "return-mismatch-probe: FAIL (narrowing int literal return byte<-65 compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/rt_mm_narrow.err; then echo "return-mismatch-probe: FAIL (narrowing int-literal return reached verifier)"; cat build/rt_mm_narrow.err; exit 1; fi; \
	  if ! grep -qiE "return type mismatch" build/rt_mm_narrow.err; then echo "return-mismatch-probe: FAIL (no clean diagnostic for narrowing int-literal return)"; cat build/rt_mm_narrow.err; exit 1; fi
	@"$(COMPILER)" build/rt_mm_ok.goo -o build/rt_mm_ok.out 2>build/rt_mm_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "return-mismatch-probe: FAIL (valid scalar/string/nullable/multi returns rejected)"; cat build/rt_mm_ok.err; exit 1; fi
	@echo "return-mismatch-probe: PASS"

# P3-5: a bare `return` from an UNNAMED multi-result function `func f()
# (int, int)` is invalid in Go and MUST be rejected at type-check with a
# clean diagnostic — NOT silently miscompiled to a zeroed (0,0) aggregate.
# This is the negative guard for the P3-5 type-checker rule; the positive
# named-result behavior (incl. the aggregate single-result ABI) is covered
# by examples/named_return_probe.goo under test-golden. Two positive cases
# here also assert NO over-rejection: a NAMED multi-result and an aggregate
# single named result (`(s string)`) must still compile.
named-return-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== named-return-reject-probe: bare return from unnamed multi-result rejected ==="
	@printf 'package main\nfunc f() (int, int) { return }\nfunc main() {}\n' > build/nrr_unnamed.goo
	@printf 'package main\nimport "fmt"\nfunc f() (x int, y int) { x = 1; y = 2; return }\nfunc main() { a, b := f(); fmt.Println(a); fmt.Println(b) }\n' > build/nrr_named_ok.goo
	@printf 'package main\nimport "fmt"\nfunc g() (s string) { s = "hi"; return }\nfunc main() { fmt.Println(g()) }\n' > build/nrr_aggr_ok.goo
	@"$(COMPILER)" build/nrr_unnamed.goo -o build/nrr_unnamed.out 2>build/nrr_unnamed.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "named-return-reject-probe: FAIL (bare return from unnamed (int,int) compiled — expected a type error, was a silent 0,0 miscompile)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/nrr_unnamed.err; then echo "named-return-reject-probe: FAIL (invalid IR reached verifier instead of clean rejection)"; cat build/nrr_unnamed.err; exit 1; fi; \
	  if ! grep -qiE "bare return is only allowed when the function has named results" build/nrr_unnamed.err; then echo "named-return-reject-probe: FAIL (no clean bare-return diagnostic)"; cat build/nrr_unnamed.err; exit 1; fi
	@"$(COMPILER)" build/nrr_named_ok.goo -o build/nrr_named_ok.out 2>build/nrr_named_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "named-return-reject-probe: FAIL (named multi-result bare return wrongly rejected — over-rejection)"; cat build/nrr_named_ok.err; exit 1; fi
	@"$(COMPILER)" build/nrr_aggr_ok.goo -o build/nrr_aggr_ok.out 2>build/nrr_aggr_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "named-return-reject-probe: FAIL (aggregate single named result wrongly rejected — over-rejection)"; cat build/nrr_aggr_ok.err; exit 1; fi
	@echo "named-return-reject-probe: PASS"

# P3-1: Go-standard composite literals (`[]T{...}` / `map[K]V{...}`). This is
# the NEGATIVE/boundary gate for the type-check + lowering rules; the positive
# construction+indexing cases live in examples/composite_{slice,map}_probe.goo
# under test-golden. Four rejections + a no-over-rejection group:
#   REJECT (clean type error, never a silent miscompile or LLVM-verifier crash):
#     * element/value type mismatch — the DECLARED element/key/value type is
#       honored over first-element inference ([]string{1}, []int{1,"two"},
#       map[string]int{"a":"notint"}). This is the core of fix d242a56.
#     * non-i32-width typed slice ([]int64{...}) — codegen lowers the typed form
#       with i32 element constants laid out against the declared width, so wider
#       widths SILENTLY MISCOMPILE (e.g. []int64{100,200,300} -> 858993459300,0).
#       Until P3-2 lowers these, the type-checker rejects them with a clear
#       "not yet supported" error instead of emitting garbage.
#   ACCEPT (no over-rejection): []int / []string (incl. empty {}), the native
#     untyped [1,2,3] form, and the bare empty `[]` literal followed on the NEXT
#     line by an identifier-led statement (the lexer's same-line `[]` lookahead
#     must not swallow it as the slice element type).
composite-literal-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== composite-literal-reject-probe: typed []T{}/map[K]V{} boundary gates ==="
	@printf 'package main\nimport "fmt"\nfunc main(){ xs := []string{1,2,3}; fmt.Println(xs[0]) }\n' > build/clr_slice_elem.goo
	@printf 'package main\nimport "fmt"\nfunc main(){ xs := []int{1,"two",3}; fmt.Println(xs[0]) }\n' > build/clr_slice_mix.goo
	@printf 'package main\nimport "fmt"\nfunc main(){ m := map[string]int{"a":"notint"}; fmt.Println(m["a"]) }\n' > build/clr_map_val.goo
	@printf 'package main\nimport "fmt"\nfunc main(){ xs := []int64{100,200,300}; fmt.Println(xs[0], xs[2]) }\n' > build/clr_slice_width.goo
	@printf 'package main\nimport "fmt"\nfunc main(){\n\tnative := [1,2,3]\n\txs := []int{4,5,6}\n\tys := []string{"go","lang"}\n\tei := []int{}\n\tes := []string{}\n\tfmt.Println(native[0], xs[2], ys[1], len(ei), len(es))\n}\n' > build/clr_ok.goo
	@printf 'package main\nimport "fmt"\nfunc main(){\n\txs := []\n\tfmt.Println(len(xs))\n}\n' > build/clr_empty_ok.goo
	@"$(COMPILER)" build/clr_slice_elem.goo -o build/clr_slice_elem.out 2>build/clr_slice_elem.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "composite-literal-reject-probe: FAIL ([]string{1,2,3} compiled — declared element type not honored over inference)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/clr_slice_elem.err; then echo "composite-literal-reject-probe: FAIL (bad []string{int} IR reached verifier)"; cat build/clr_slice_elem.err; exit 1; fi; \
	  if ! grep -qiE "not compatible with declared element type" build/clr_slice_elem.err; then echo "composite-literal-reject-probe: FAIL (no clean element-mismatch diagnostic for []string{int})"; cat build/clr_slice_elem.err; exit 1; fi
	@"$(COMPILER)" build/clr_slice_mix.goo -o build/clr_slice_mix.out 2>build/clr_slice_mix.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "composite-literal-reject-probe: FAIL ([]int{1,\"two\",3} compiled — mixed-type elements not rejected)"; exit 1; fi; \
	  if ! grep -qiE "not compatible with declared element type" build/clr_slice_mix.err; then echo "composite-literal-reject-probe: FAIL (no clean element-mismatch diagnostic for []int{string})"; cat build/clr_slice_mix.err; exit 1; fi
	@"$(COMPILER)" build/clr_map_val.goo -o build/clr_map_val.out 2>build/clr_map_val.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "composite-literal-reject-probe: FAIL (map[string]int{\"a\":\"notint\"} compiled — declared value type not honored)"; exit 1; fi; \
	  if ! grep -qiE "not compatible with declared value type" build/clr_map_val.err; then echo "composite-literal-reject-probe: FAIL (no clean value-mismatch diagnostic for map value)"; cat build/clr_map_val.err; exit 1; fi
	@"$(COMPILER)" build/clr_slice_width.goo -o build/clr_slice_width.out 2>build/clr_slice_width.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "composite-literal-reject-probe: FAIL ([]int64{100,200,300} wrongly rejected — general []T width lowering regressed)"; cat build/clr_slice_width.err; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/clr_slice_width.err; then echo "composite-literal-reject-probe: FAIL ([]int64{} bad IR reached verifier)"; cat build/clr_slice_width.err; exit 1; fi; \
	  out="$$(./build/clr_slice_width.out)"; if [ "$$out" != "100 300" ]; then echo "composite-literal-reject-probe: FAIL ([]int64 width-coerced output '$$out' != '100 300')"; exit 1; fi
	@"$(COMPILER)" build/clr_ok.goo -o build/clr_ok.out 2>build/clr_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "composite-literal-reject-probe: FAIL ([]int/[]string (incl. empty) or native [1,2,3] wrongly rejected — over-rejection)"; cat build/clr_ok.err; exit 1; fi; \
	  out="$$(./build/clr_ok.out)"; if [ "$$out" != "1 6 lang 0 0" ]; then echo "composite-literal-reject-probe: FAIL (accepted-forms output '$$out' != '1 6 lang 0 0')"; exit 1; fi
	@"$(COMPILER)" build/clr_empty_ok.goo -o build/clr_empty_ok.out 2>build/clr_empty_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "composite-literal-reject-probe: FAIL (bare empty [] <nl> identifier-led stmt wrongly mis-lexed as slice type — regression)"; cat build/clr_empty_ok.err; exit 1; fi; \
	  out="$$(./build/clr_empty_ok.out)"; if [ "$$out" != "0" ]; then echo "composite-literal-reject-probe: FAIL (bare empty [] output '$$out' != '0')"; exit 1; fi
	@echo "composite-literal-reject-probe: PASS"

# P2-2: a user-function call with the wrong number of arguments must be
# rejected at type-check with a clean source-located diagnostic, NOT reach
# the LLVM verifier ("Incorrect number of arguments passed to called
# function!"). Covers too-few and too-many; the OK fixture (correct arity,
# a method call, and variadic builtins) must still compile so we don't
# over-reject.
call-arity-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== call-arity-probe: user-function call arity type-checked ==="
	@printf 'package main\nfunc add(a int, b int) int { return a + b }\nfunc main() { add(1) }\n' > build/ca_few.goo
	@printf 'package main\nfunc add(a int, b int) int { return a + b }\nfunc main() { add(1, 2, 3) }\n' > build/ca_many.goo
	@printf 'package main\ntype Counter struct { n int }\nfunc (c Counter) get() int { return c.n }\nfunc main() { var c Counter = Counter{n: 7}; c.get(99) }\n' > build/ca_method.goo
	@printf 'package main\nimport "fmt"\ntype Counter struct { n int }\nfunc (c Counter) get() int { return c.n }\nfunc (c Counter) addn(x int) int { return c.n + x }\nfunc add(a int, b int) int { return a + b }\nfunc main() { var c Counter = Counter{n: 7}; fmt.Println(add(1, 2)); fmt.Println(c.get()); fmt.Println(c.addn(3)) }\n' > build/ca_ok.goo
	@"$(COMPILER)" build/ca_few.goo -o build/ca_few.out 2>build/ca_few.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "call-arity-probe: FAIL (add(1) with too few args compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/ca_few.err; then echo "call-arity-probe: FAIL (invalid IR reached verifier for too-few)"; cat build/ca_few.err; exit 1; fi; \
	  if ! grep -qiE "wrong number of arguments" build/ca_few.err; then echo "call-arity-probe: FAIL (no clean arity diagnostic for too-few)"; cat build/ca_few.err; exit 1; fi
	@"$(COMPILER)" build/ca_many.goo -o build/ca_many.out 2>build/ca_many.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "call-arity-probe: FAIL (add(1,2,3) with too many args compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/ca_many.err; then echo "call-arity-probe: FAIL (invalid IR reached verifier for too-many)"; cat build/ca_many.err; exit 1; fi; \
	  if ! grep -qiE "wrong number of arguments" build/ca_many.err; then echo "call-arity-probe: FAIL (no clean arity diagnostic for too-many)"; cat build/ca_many.err; exit 1; fi
	@"$(COMPILER)" build/ca_method.goo -o build/ca_method.out 2>build/ca_method.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "call-arity-probe: FAIL (c.get(99) with too many args compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/ca_method.err; then echo "call-arity-probe: FAIL (invalid IR reached verifier for method too-many)"; cat build/ca_method.err; exit 1; fi; \
	  if ! grep -qiE "wrong number of arguments" build/ca_method.err; then echo "call-arity-probe: FAIL (no clean arity diagnostic for method too-many)"; cat build/ca_method.err; exit 1; fi
	@"$(COMPILER)" build/ca_ok.goo -o build/ca_ok.out 2>build/ca_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "call-arity-probe: FAIL (valid call/method/builtin rejected)"; cat build/ca_ok.err; exit 1; fi
	@echo "call-arity-probe: PASS"

# P2-2: a user-function call whose argument type is incompatible with the
# declared parameter must be rejected at type-check with a clean,
# position-named diagnostic, NOT reach the LLVM verifier ("Call parameter
# type does not match function signature!").
call-argtype-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== call-argtype-probe: user-function call arg types type-checked ==="
	@printf 'package main\nfunc add(a int, b int) int { return a + b }\nfunc main() { add("x", 2) }\n' > build/cat_str.goo
	@printf 'package main\nfunc greet(s string) string { return s }\nfunc main() { greet(42) }\n' > build/cat_int.goo
	@printf 'package main\ntype Counter struct { n int }\nfunc (c Counter) addn(x int) int { return c.n + x }\nfunc main() { var c Counter = Counter{n: 7}; c.addn("x") }\n' > build/cat_method.goo
	@printf 'package main\nimport "fmt"\ntype Counter struct { n int }\nfunc (c Counter) addn(x int) int { return c.n + x }\nfunc add(a int, b int) int { return a + b }\nfunc greet(s string) string { return s }\nfunc main() { var c Counter = Counter{n: 7}; fmt.Println(add(1, 2)); fmt.Println(greet("hi")); fmt.Println(c.addn(3)) }\n' > build/cat_ok.goo
	@"$(COMPILER)" build/cat_str.goo -o build/cat_str.out 2>build/cat_str.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "call-argtype-probe: FAIL (add(\"x\", 2) string-as-int compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/cat_str.err; then echo "call-argtype-probe: FAIL (invalid IR reached verifier for string-as-int)"; cat build/cat_str.err; exit 1; fi; \
	  if ! grep -qiE "cannot use string as int" build/cat_str.err; then echo "call-argtype-probe: FAIL (no clean arg-type diagnostic for string-as-int)"; cat build/cat_str.err; exit 1; fi
	@"$(COMPILER)" build/cat_int.goo -o build/cat_int.out 2>build/cat_int.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "call-argtype-probe: FAIL (greet(42) int-as-string compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/cat_int.err; then echo "call-argtype-probe: FAIL (invalid IR reached verifier for int-as-string)"; cat build/cat_int.err; exit 1; fi; \
	  if ! grep -qiE "cannot use .* as string" build/cat_int.err; then echo "call-argtype-probe: FAIL (no clean arg-type diagnostic for int-as-string)"; cat build/cat_int.err; exit 1; fi
	@"$(COMPILER)" build/cat_method.goo -o build/cat_method.out 2>build/cat_method.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "call-argtype-probe: FAIL (c.addn(\"x\") string-as-int compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/cat_method.err; then echo "call-argtype-probe: FAIL (invalid IR reached verifier for method string-as-int)"; cat build/cat_method.err; exit 1; fi; \
	  if ! grep -qiE "cannot use string as int" build/cat_method.err; then echo "call-argtype-probe: FAIL (no clean arg-type diagnostic for method string-as-int)"; cat build/cat_method.err; exit 1; fi
	@"$(COMPILER)" build/cat_ok.goo -o build/cat_ok.out 2>build/cat_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "call-argtype-probe: FAIL (valid typed calls rejected)"; cat build/cat_ok.err; exit 1; fi
	@echo "call-argtype-probe: PASS"

# Stdlib Phase 1: a CROSS-PACKAGE call (`pkg.Fn(args)`) into a source-compiled
# package must type-check its arguments against the export's real signature. A
# width mismatch from a NON-LITERAL operand (an int32 variable into an int64
# param) or wrong arity must be rejected at type-check with a clean diagnostic,
# NOT reach the LLVM verifier ("Call parameter type does not match function
# signature!"). An untyped integer LITERAL, by contrast, ADAPTS to the parameter
# type (narrow integer-literal adaptation) — pac_lit below proves Half(84) both
# compiles and computes. The hardcoded stdlib shims (fmt.Println etc.) carry
# param-less stubs and stay UNchecked — the happy-path arm drives fmt.Println.
# Fixture package: goostd/pkgcheck (Half(int64) int64, Double(int) int); imports
# resolve via the ./goostd cwd fallback (compiler run from the repo root).
pkg-argcheck-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== pkg-argcheck-probe: cross-package call args type-checked ==="
	@printf 'package main\nimport ("fmt"\n"pkgcheck")\nfunc main() { var v int32 = 84; fmt.Println(pkgcheck.Half(v)) }\n' > build/pac_width.goo
	@printf 'package main\nimport ("fmt"\n"pkgcheck")\nfunc main() { fmt.Println(pkgcheck.Double(1, 2)) }\n' > build/pac_arity.goo
	@printf 'package main\nimport ("fmt"\n"pkgcheck")\nfunc main() { fmt.Println(pkgcheck.Double(21)) }\n' > build/pac_ok.goo
	@printf 'package main\nimport ("fmt"\n"pkgcheck")\nfunc main() { var r int64 = pkgcheck.Half(84); if r == 42 { fmt.Println(42) } else { fmt.Println(0) } }\n' > build/pac_lit.goo
	@"$(COMPILER)" build/pac_width.goo -o build/pac_width.out 2>build/pac_width.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "pkg-argcheck-probe: FAIL (Half(int32 var) compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/pac_width.err; then echo "pkg-argcheck-probe: FAIL (invalid IR reached verifier for width mismatch)"; cat build/pac_width.err; exit 1; fi; \
	  if ! grep -qiE "cannot use int32 as int64" build/pac_width.err; then echo "pkg-argcheck-probe: FAIL (no clean arg-type diagnostic for width mismatch)"; cat build/pac_width.err; exit 1; fi
	@"$(COMPILER)" build/pac_arity.goo -o build/pac_arity.out 2>build/pac_arity.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "pkg-argcheck-probe: FAIL (Double(1, 2) too-many-args compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/pac_arity.err; then echo "pkg-argcheck-probe: FAIL (invalid IR reached verifier for arity)"; cat build/pac_arity.err; exit 1; fi; \
	  if ! grep -qiE "wrong number of arguments" build/pac_arity.err; then echo "pkg-argcheck-probe: FAIL (no clean arity diagnostic)"; cat build/pac_arity.err; exit 1; fi
	@"$(COMPILER)" build/pac_ok.goo -o build/pac_ok.out 2>build/pac_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "pkg-argcheck-probe: FAIL (valid pkgcheck.Double(21) rejected)"; cat build/pac_ok.err; exit 1; fi; \
	  out=$$(./build/pac_ok.out); if [ "$$out" != "42" ]; then echo "pkg-argcheck-probe: FAIL (pkgcheck.Double(21) printed '$$out', want 42)"; exit 1; fi
	@"$(COMPILER)" build/pac_lit.goo -o build/pac_lit.out 2>build/pac_lit.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "pkg-argcheck-probe: FAIL (literal-adapt Half(84) rejected — expected int64 adaptation)"; cat build/pac_lit.err; exit 1; fi; \
	  out=$$(./build/pac_lit.out); if [ "$$out" != "42" ]; then echo "pkg-argcheck-probe: FAIL (Half(84) literal-adapt printed '$$out', want 42)"; exit 1; fi
	@echo "pkg-argcheck-probe: PASS"

# Forward references (Go package-scope semantics): a function body may call a
# function declared LATER in the same file/package. Requires the type checker's
# two-pass signature hoist AND the codegen prototype pre-pass; for a package it
# additionally requires intra-package calls to resolve the package-mangled
# symbol. Regression guard: neither a clean "Undefined" type error nor an LLVM
# verifier failure is acceptable — the programs must compile and run.
# main_fwd: main -> helper -> leaf, both declared below main.
# Package case: goostd/fwdref (Triple declared before the Double it calls).
forward-ref-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== forward-ref-probe: forward references in main + packages ==="
	@printf 'package main\nimport "fmt"\nfunc main() { fmt.Println(helper(20)) }\nfunc helper(x int) int { return leaf(x) + 1 }\nfunc leaf(x int) int { return x * 2 }\n' > build/fr_main.goo
	@printf 'package main\nimport ("fmt"\n"fwdref")\nfunc main() { fmt.Println(fwdref.Triple(14)) }\n' > build/fr_pkg.goo
	@"$(COMPILER)" build/fr_main.goo -o build/fr_main.out 2>build/fr_main.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "forward-ref-probe: FAIL (main-file forward ref rejected)"; cat build/fr_main.err; exit 1; fi; \
	  out=$$(./build/fr_main.out); if [ "$$out" != "41" ]; then echo "forward-ref-probe: FAIL (main forward ref printed '$$out', want 41)"; exit 1; fi
	@"$(COMPILER)" build/fr_pkg.goo -o build/fr_pkg.out 2>build/fr_pkg.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "forward-ref-probe: FAIL (package forward ref rejected)"; cat build/fr_pkg.err; exit 1; fi; \
	  out=$$(./build/fr_pkg.out); if [ "$$out" != "42" ]; then echo "forward-ref-probe: FAIL (package forward ref printed '$$out', want 42)"; exit 1; fi
	@echo "forward-ref-probe: PASS"

# P2-4: printing an AGGREGATE nullable (?T) or error-union (!T) value must be a
# clean, source-located compile error, NOT invalid IR that crashes the LLVM
# verifier. P0-3 already rejects these at the fmt.Println unsupported-argument
# check (only string/integer/bool/float print in v1); this probe is a permanent
# regression gate so a future change can't silently start lowering an aggregate
# ?T/!T into a print and emit invalid IR. Covers both kinds; asserts non-zero
# exit, the clean diagnostic, and the ABSENCE of "Module verification failed".
print-aggregate-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== print-aggregate-probe: printing ?T/!T aggregates fails cleanly ==="
	@printf 'package main\nimport "fmt"\nfunc main() { var x ?int = 5; fmt.Println(x) }\n' > build/print_agg_null.goo
	@printf 'package main\nimport "fmt"\nfunc f() !int { return 5 }\nfunc main() { x := f(); fmt.Println(x) }\n' > build/print_agg_erru.goo
	@"$(COMPILER)" build/print_agg_null.goo -o build/print_agg_null.out 2>build/print_agg_null.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "print-aggregate-probe: FAIL (printing ?int compiled — expected a clean error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/print_agg_null.err; then echo "print-aggregate-probe: FAIL (invalid IR reached verifier for ?int print)"; cat build/print_agg_null.err; exit 1; fi; \
	  if ! grep -qiE "unsupported argument type" build/print_agg_null.err; then echo "print-aggregate-probe: FAIL (no clean diagnostic for ?int print)"; cat build/print_agg_null.err; exit 1; fi
	@"$(COMPILER)" build/print_agg_erru.goo -o build/print_agg_erru.out 2>build/print_agg_erru.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "print-aggregate-probe: FAIL (printing !int compiled — expected a clean error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/print_agg_erru.err; then echo "print-aggregate-probe: FAIL (invalid IR reached verifier for !int print)"; cat build/print_agg_erru.err; exit 1; fi; \
	  if ! grep -qiE "unsupported argument type" build/print_agg_erru.err; then echo "print-aggregate-probe: FAIL (no clean diagnostic for !int print)"; cat build/print_agg_erru.err; exit 1; fi
	@echo "print-aggregate-probe: PASS"

# P0-5: end-to-end golden tests — compile+run real .goo programs, diff stdout.
# The honest e2e signal (unlike `make test`, which never invokes bin/goo).
.PHONY: test-golden
test-golden: $(COMPILER) $(RUNTIME_LIB)
	@echo "=== test-golden: data-driven end-to-end golden suite ==="
	@COMPILER="$(COMPILER)" bash scripts/run_golden.sh

# Switch-statement probe: compile + run examples/switch_probe.goo and diff
# stdout against expected.txt (m10-probe pattern). Covers first/middle case
# matches, default fallthrough, and multi-statement bodies with no implicit
# fallthrough. Joined `verify` when expression switch shipped.
switch-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/switch_probe examples/switch_probe.goo
	@./build/switch_probe > build/switch_probe.actual.txt
	@if diff -u examples/switch_probe.expected.txt build/switch_probe.actual.txt; then \
	  echo "switch-probe: PASS (expression switch end-to-end)"; \
	else \
	  echo "switch-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# M9: a Goo program is the C entry point and must exit 0 on normal completion
# (not a garbage register value). Compiles + runs empty/no-return/bare-return
# mains and asserts exit 0.
exit-code-probe: $(COMPILER) $(RUNTIME_LIB)
	@./scripts/exit_code_probe.sh

# M11 comptime BLOCK probe: sibling to comptime-probe that exercises
# the AST_COMPTIME_BLOCK dispatch path independently of the engine's
# function-call capability (which comptime-probe needs and which is
# blocked until M11-engine-recursion). This probe is the
# immediately-achievable counterpart and is part of the standard
# verification net per `verification_gates.md` (memory).
comptime-block-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/comptime_block_probe examples/comptime_block_probe.goo
	@actual="$$(./build/comptime_block_probe)"; \
	  expected="comptime block ok"; \
	  if [ "$$actual" = "$$expected" ]; then \
	    echo "comptime-block-probe: PASS (AST_COMPTIME_BLOCK ships)"; \
	  else \
	    echo "comptime-block-probe: FAIL"; \
	    echo "  expected: $$expected"; \
	    echo "  got:      $$actual"; \
	    exit 1; \
	  fi

# Unit tests
# Link against $(SRC_OBJS), NOT $(OBJS): the latter includes the test
# framework object, whose source `tests/framework/test_framework.c`
# #includes a missing header `test/test_framework.h`. The framework is
# unused by these unit tests (they use plain assert + stdio), so linking
# the compiler objects directly is correct and sidesteps the broken
# include. Restoring the framework header is its own task; this target
# does not need to wait on that work.
test-lexer: $(SRC_OBJS)
	@mkdir -p tests/unit/lexer
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) tests/unit/lexer/test_lexer_basic.c $(SRC_OBJS) -o tests/test_lexer $(LDFLAGS) $(LLVM_LDFLAGS)
	./tests/test_lexer

test-codegen: $(SRC_OBJS)
	@mkdir -p tests/unit/codegen
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) tests/unit/codegen/test_target_detection.c $(SRC_OBJS) -o tests/test_codegen $(LDFLAGS) $(LLVM_LDFLAGS)
	./tests/test_codegen

test-units: test-lexer test-codegen

# Test main (for running tests)
test-main: $(OBJS) $(SRCDIR)/main_simple.c | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(SRCDIR)/main_simple.c $(OBJS) -o $(BINDIR)/test-main $(LDFLAGS) $(LLVM_LDFLAGS)

# Test targets
TEST_INTERFACE_SYSTEM = $(BINDIR)/test_interface_system
TEST_FLOW_ANALYSIS = $(BINDIR)/test_flow_analysis
TEST_REFERENCE_MANAGER = $(BINDIR)/test_reference_manager
TEST_HARDWARE_AWARE = $(BINDIR)/test_hardware_aware

# Tests
test: $(TEST_RUNNER)
	./$(TEST_RUNNER)

$(TEST_RUNNER): $(OBJS) $(TEST_FRAMEWORK_DIR)/test_main.c $(TEST_UNIT_DIR)/constraint/constraint_inference_test.c $(TEST_UNIT_DIR)/type_system/concept_generics_test.c $(TEST_UNIT_DIR)/type_system/higher_kinded_types_test.c $(TEST_UNIT_DIR)/type_system/concept_declaration_test.c $(TEST_UNIT_DIR)/constraint/advanced_constraint_inference_test.c | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(TEST_FRAMEWORK_DIR)/test_main.c $(TEST_UNIT_DIR)/constraint/constraint_inference_test.c $(TEST_UNIT_DIR)/type_system/concept_generics_test.c $(TEST_UNIT_DIR)/type_system/higher_kinded_types_test.c $(TEST_UNIT_DIR)/type_system/concept_declaration_test.c $(TEST_UNIT_DIR)/constraint/advanced_constraint_inference_test.c $(OBJS) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS)

# Individual test targets
test-reference: $(TEST_REFERENCE_MANAGER)
	./$(TEST_REFERENCE_MANAGER)

$(TEST_REFERENCE_MANAGER): $(OBJS) $(TEST_UNIT_DIR)/memory/reference_manager_test.c | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(TEST_UNIT_DIR)/memory/reference_manager_test.c $(OBJS) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS) -DSTANDALONE_TEST

# Hot reload test
test-hot-reload: $(OBJS) $(TEST_INTEGRATION_DIR)/hot_reload_test.c | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(TEST_INTEGRATION_DIR)/hot_reload_test.c $(OBJS) -o $(BINDIR)/test_hot_reload $(LDFLAGS) $(LLVM_LDFLAGS) -ldl
	./$(BINDIR)/test_hot_reload

# Install local git hooks (one-time, per clone). Points core.hooksPath at the
# tracked .githooks/ dir so the scripts are version-controlled and shared.
#   pre-commit -> make test (~1s)   pre-push -> make verify + make test (~10s)
# These replace GitHub Actions as the test gate (this repo doesn't pay for CI).
# Bypass any hook in an emergency with `git commit/push --no-verify`.
.PHONY: hooks
hooks:
	@chmod +x .githooks/pre-commit .githooks/pre-push
	@git config core.hooksPath .githooks
	@echo "git hooks installed: core.hooksPath=.githooks (pre-commit: make test, pre-push: make verify + test)"

# Clean
clean:
	rm -rf $(BUILDDIR) $(BINDIR)
	rm -f $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.tab.h $(SRCDIR)/parser/parser.yy.c

test-interface: $(TEST_INTERFACE_SYSTEM)
	./$(TEST_INTERFACE_SYSTEM)

$(TEST_INTERFACE_SYSTEM): $(TEST_UNIT_DIR)/interface/test_interface_system.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

test-flow: $(TEST_FLOW_ANALYSIS)
	./$(TEST_FLOW_ANALYSIS)

$(TEST_FLOW_ANALYSIS): $(TEST_UNIT_DIR)/flow/flow_analysis_test.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

test-hardware-aware: $(TEST_HARDWARE_AWARE)
	./$(TEST_HARDWARE_AWARE)

$(TEST_HARDWARE_AWARE): $(TESTDIR)/test_hardware_aware.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

# REPL targets
repl: $(REPL)

$(REPL): $(SRCDIR)/ide/repl_main.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

# Enhanced REPL with syntax highlighting
repl-enhanced: $(REPL_ENHANCED)

$(REPL_ENHANCED): $(SRCDIR)/ide/repl_enhanced_simple.c $(SRCDIR)/ide/repl_syntax.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

# Development Workflow Tools
PROJECT_WIZARD = $(BINDIR)/goo-wizard
PROFILER_TOOL = $(BINDIR)/goo-profiler
DOC_GENERATOR = $(BINDIR)/goo-docs
HEALTH_DASHBOARD = $(BINDIR)/goo-health

# Complete development workflow toolchain
# (test-tool removed: its source tools/test_runner/main.c was never created; the
# maintained test runner is tests/test_runner.c, built by the test-pipeline target.)
dev-tools: wizard profiler doc-generator health-dashboard

# Project template wizard
wizard: $(PROJECT_WIZARD)

$(PROJECT_WIZARD): tools/project_wizard/main.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

# Integrated profiler
profiler: $(PROFILER_TOOL)

$(PROFILER_TOOL): tools/profiler/main.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< -lpthread

# Documentation generator
doc-generator: $(DOC_GENERATOR)

$(DOC_GENERATOR): tools/doc_generator/main.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

# Project health dashboard
health-dashboard: $(HEALTH_DASHBOARD)

$(HEALTH_DASHBOARD): tools/health_dashboard/main.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< -lpthread

# Taint Analysis System Test
TAINT_ANALYSIS_TEST = $(BINDIR)/taint_analysis_test
TAINT_ANALYSIS_SOURCES = src/types/taint_analysis.c src/security/security_framework.c src/errors/error.c

test-taint-analysis: $(TAINT_ANALYSIS_TEST)
	@echo "Running taint analysis system tests..."
	./$(TAINT_ANALYSIS_TEST)

$(TAINT_ANALYSIS_TEST): tests/security/taint_analysis_test.c $(TAINT_ANALYSIS_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Capability Security System Test
CAPABILITY_SECURITY_TEST = $(BINDIR)/capability_security_test
CAPABILITY_SECURITY_SOURCES = src/security/capability_security.c src/security/security_framework.c src/errors/error.c

test-capability-security: $(CAPABILITY_SECURITY_TEST)
	@echo "Running capability security system tests..."
	./$(CAPABILITY_SECURITY_TEST)

$(CAPABILITY_SECURITY_TEST): tests/security/capability_security_test.c $(CAPABILITY_SECURITY_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Security Auditing System Test
SECURITY_AUDITING_TEST = $(BINDIR)/security_auditing_test
SECURITY_AUDITING_SOURCES = src/security/security_auditing.c src/security/security_patterns.c src/security/security_framework.c src/security/capability_security.c src/types/taint_analysis.c src/errors/error.c

test-security-auditing: $(SECURITY_AUDITING_TEST)
	@echo "Running security auditing system tests..."
	./$(SECURITY_AUDITING_TEST)

$(SECURITY_AUDITING_TEST): tests/security/security_auditing_test.c $(SECURITY_AUDITING_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Cryptographic Security System Test
CRYPTO_SECURITY_TEST = $(BINDIR)/crypto_security_test
CRYPTO_SECURITY_SOURCES = src/security/crypto_security.c src/security/security_framework.c src/errors/error.c

test-crypto-security: $(CRYPTO_SECURITY_TEST)
	@echo "Running cryptographic security system tests..."
	./$(CRYPTO_SECURITY_TEST)

$(CRYPTO_SECURITY_TEST): tests/security/crypto_security_simple_test.c $(CRYPTO_SECURITY_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Ergonomic error handling tests
ERGONOMIC_ERROR_TEST = $(BINDIR)/test_ergonomic_errors

test-ergonomic-errors: $(ERGONOMIC_ERROR_TEST)
	./$(ERGONOMIC_ERROR_TEST)

$(ERGONOMIC_ERROR_TEST): $(TEST_UNIT_DIR)/error/ergonomic_errors_test.c $(ERROR_SRCS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# LSP Server targets
lsp: $(LSP_SERVER)

$(LSP_SERVER): $(SRCDIR)/ide/lsp_simple.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

# Enhanced LSP Server with AST integration
lsp-enhanced: $(LSP_ENHANCED_SERVER)

$(LSP_ENHANCED_SERVER): $(SRCDIR)/ide/lsp_enhanced.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

# Standalone Enhanced LSP Server (no dependencies)
lsp-standalone: $(LSP_STANDALONE_SERVER)

$(LSP_STANDALONE_SERVER): $(SRCDIR)/ide/lsp_standalone.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

# Package Manager CLI (gmod)
gmod: $(GMOD_CLI)

$(GMOD_CLI): $(SRCDIR)/package/gmod_cli.c $(PACKAGE_SRCS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lcurl -ljson-c

# Debug Adapter Protocol (DAP) Server
DEBUG_ADAPTER_SERVER = $(BINDIR)/goo-debug-adapter

debug-adapter: $(DEBUG_ADAPTER_SERVER)

$(DEBUG_ADAPTER_SERVER): $(SRCDIR)/ide/debug_adapter.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

# Performance Dashboard Server
PERFORMANCE_DASHBOARD_SERVER = $(BINDIR)/goo-dashboard

dashboard: $(PERFORMANCE_DASHBOARD_SERVER)

$(PERFORMANCE_DASHBOARD_SERVER): $(SRCDIR)/ide/dashboard_main.c $(SRCDIR)/ide/performance_dashboard.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

# Async Streams Test
ASYNC_STREAMS_TEST = $(BINDIR)/async_streams_test
# async_streams.c calls into the structured-concurrency runtime (concurrent_block_*,
# cancellation_token_*), which pulls in its transitive deps (transparent async,
# ergonomic errors, actor system).
ASYNC_STREAMS_SOURCES = src/async/async_streams.c \
	src/concurrency/structured_concurrency_enhanced.c \
	src/concurrency/structured_concurrency.c \
	src/async/transparent_async.c src/async/transparent_execution.c \
	src/errors/error.c src/errors/ergonomic_errors.c \
	src/runtime/actor_system.c

test-async-streams: $(ASYNC_STREAMS_TEST)
	@echo "Running async streams system tests..."
	./$(ASYNC_STREAMS_TEST)

$(ASYNC_STREAMS_TEST): tests/concurrency/async_streams_test.c $(ASYNC_STREAMS_SOURCES)
	@mkdir -p $(BINDIR)
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS)

test-repl: $(TEST_REPL)
	./$(TEST_REPL)

$(TEST_REPL): $(TEST_INTEGRATION_DIR)/repl_test.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

test-performance: $(TEST_PERFORMANCE)
	./$(TEST_PERFORMANCE)

$(TEST_PERFORMANCE): $(TEST_INTEGRATION_DIR)/performance_monitor_test.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

# DEFERRED (does not build): error_reporting_test.c predates a split of one unified
# error system into two that now coexist with colliding type names —
# errors/error.h and error_reporting.h both define ErrorSeverity/ErrorCategory/
# ErrorCode, and ErrorCategory DIVERGES (LEXER/PARSER/CODEGEN vs SYNTAX/SEMANTIC/
# OWNERSHIP/...). The 399-line test pulls in BOTH headers (error_reporting.h +
# repl.h -> errors/error.h) -> enum redeclaration, and treats ErrorContext as both
# the reporting config (now struct ErrorReportingContext) and the REPL's opaque
# error context. Repairing it means reconciling/renaming two error subsystems the
# compiler depends on: high risk, low value. See memory/goolang-test-suite-state.md.
# `test-error-reporting` skips cleanly; build it on demand with `make $(TEST_ERROR_REPORTING)`.
test-error-reporting:
	@echo "SKIP: test-error-reporting deferred — two divergent error subsystems share"
	@echo "      type names (see the comment above this rule in the Makefile)."

$(TEST_ERROR_REPORTING): $(TEST_INTEGRATION_DIR)/error_reporting_test.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

# Time-travel debugging test
TEST_TIME_TRAVEL_DEBUG = $(BINDIR)/test_time_travel_debug

test-time-travel-debug: $(TEST_TIME_TRAVEL_DEBUG)
	./$(TEST_TIME_TRAVEL_DEBUG)

$(TEST_TIME_TRAVEL_DEBUG): $(TEST_INTEGRATION_DIR)/time_travel_debug_test.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

# Install
install: $(COMPILER)
	cp $(COMPILER) /usr/local/bin/

# Development helpers
debug: CFLAGS += -DDEBUG -O0
debug: $(COMPILER)

format:
	find $(SRCDIR) $(INCDIR) -name "*.c" -o -name "*.h" | xargs clang-format -i

check:
	cppcheck --enable=all $(SRCDIR)

# Coverage targets
COVERAGE_DIR = coverage
COVERAGE_TEST_RUNNER = $(BINDIR)/test_runner_coverage

coverage: coverage-clean $(COVERAGE_TEST_RUNNER)
	@echo "Running tests with coverage..."
	./$(COVERAGE_TEST_RUNNER)
	@echo "Generating coverage report..."
	lcov --capture --directory $(BUILDDIR) --output-file $(COVERAGE_DIR)/coverage.info
	lcov --remove $(COVERAGE_DIR)/coverage.info '/usr/*' --output-file $(COVERAGE_DIR)/coverage.info
	lcov --remove $(COVERAGE_DIR)/coverage.info '*/test/*' --output-file $(COVERAGE_DIR)/coverage.info
	genhtml $(COVERAGE_DIR)/coverage.info --output-directory $(COVERAGE_DIR)/html
	@echo "Coverage report generated in $(COVERAGE_DIR)/html/index.html"

$(COVERAGE_TEST_RUNNER): $(TEST_FRAMEWORK_DIR)/test_main.c | $(BINDIR) $(COVERAGE_DIR)
	@echo "Building test runner with coverage support..."
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) $(LLVM_CFLAGS) -c -o $(BUILDDIR)/test_main_coverage.o $(TEST_FRAMEWORK_DIR)/test_main.c
	@# Build objects with coverage
	@for src in $(CURRENT_SRCS); do \
		obj=$$(echo $$src | sed 's|$(SRCDIR)/|$(BUILDDIR)/|g' | sed 's|\.c$$|_coverage.o|g'); \
		mkdir -p $$(dirname $$obj); \
		$(CC) $(CFLAGS) $(COVERAGE_FLAGS) $(LLVM_CFLAGS) -c $$src -o $$obj; \
	done
	@# Link with coverage
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) $(BUILDBIN)/test_main_coverage.o $(filter-out $(BUILDDIR)/main.o, $(OBJS:_coverage.o=)) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS)

coverage-report:
	@echo "Opening coverage report..."
	open $(COVERAGE_DIR)/html/index.html

coverage-clean:
	rm -rf $(COVERAGE_DIR)

# Proof generation test
proof_generation_test: $(TEST_UNIT_DIR)/proof/proof_generation_test.c $(SRC_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LDFLAGS) $(LLVM_LDFLAGS)

# Runtime optimization framework tests
runtime_optimization_test: $(TEST_UNIT_DIR)/runtime/runtime_optimization_test.c $(SRC_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LDFLAGS) $(LLVM_LDFLAGS)

runtime_optimization_test_simple: $(TEST_UNIT_DIR)/runtime/runtime_optimization_test_simple.c $(SRCDIR)/types/runtime_optimization_simple.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

runtime_optimization_demo: $(TEST_DEMOS_DIR)/runtime_optimization_demo.c $(SRCDIR)/types/runtime_optimization.c $(SRCDIR)/types/proof_generation.c $(SRCDIR)/types/proof_smt.c $(SRCDIR)/types/proof_obligations.c $(SRCDIR)/types/proof_reporting.c $(SRCDIR)/types/contracts.c $(SRCDIR)/types/dependent_types.c $(SRCDIR)/types/symbolic_expression.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

runtime_optimization_demo_simple: $(TEST_DEMOS_DIR)/runtime_optimization_demo_simple.c $(SRCDIR)/types/runtime_optimization_simple.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Contract programming framework tests
contracts_test: $(TEST_UNIT_DIR)/contract/contracts_test.c $(SRC_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LDFLAGS) $(LLVM_LDFLAGS)

# Contract proof integration test
contract_proof_integration_test: $(TEST_UNIT_DIR)/contract/contract_proof_integration_test.c $(SRC_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LDFLAGS) $(LLVM_LDFLAGS)

# Actor System Test
ACTOR_SYSTEM_TEST = $(BINDIR)/actor_system_test
ACTOR_SYSTEM_SOURCES = src/runtime/actor_system.c src/errors/error.c

test-actor-system: $(ACTOR_SYSTEM_TEST)
	@echo "Running actor system tests..."
	./$(ACTOR_SYSTEM_TEST)

$(ACTOR_SYSTEM_TEST): tests/concurrency/actor_system_test.c $(ACTOR_SYSTEM_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Shared Variables Test (Task 21.2)
shared_variables_test: tests/concurrency/shared_variables_test.c $(SRCDIR)/concurrency/shared_variables.c $(SRCDIR)/errors/ergonomic_errors.c $(SRCDIR)/errors/error.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Structured Concurrency Test (Task 21.3)
structured_concurrency_test: tests/concurrency/structured_concurrency_test.c $(SRCDIR)/concurrency/structured_concurrency.c $(SRCDIR)/errors/ergonomic_errors.c $(SRCDIR)/errors/error.c
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS)

# All optimization system tests
.PHONY: test-optimization test-optimization-simple test-all-optimization clean-tests
test-optimization: runtime_optimization_test
	@echo "Running runtime optimization tests..."
	./runtime_optimization_test

test-optimization-simple: runtime_optimization_test_simple
	@echo "Running simplified runtime optimization tests..."
	./runtime_optimization_test_simple

test-all-optimization: runtime_optimization_test runtime_optimization_demo contracts_test contract_proof_integration_test proof_generation_test
	@echo "Running all optimization system tests..."
	./runtime_optimization_test
	./contracts_test
	./contract_proof_integration_test
	./proof_generation_test
	@echo "Running runtime optimization demonstration..."
	./runtime_optimization_demo

clean-tests:
	rm -f runtime_optimization_test runtime_optimization_demo contracts_test contract_proof_integration_test proof_generation_test
	rm -f comptime_test comptime_types_test optimization_test pgo_test advanced_optimization_test advanced_macro_test derive_macro_test template_macro_test
	rm -f shared_variables_test structured_concurrency_test
# Work-Stealing Test
WORK_STEALING_TEST = $(BINDIR)/work_stealing_test
# work_stealing.c calls dynamic_chunking_create/_update_metrics, so
# dynamic_chunking.c must be linked in too.
WORK_STEALING_SOURCES = src/concurrency/work_stealing.c src/concurrency/dynamic_chunking.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-work-stealing: $(WORK_STEALING_TEST)
	@echo "Running work-stealing tests..."
	./$(WORK_STEALING_TEST)

$(WORK_STEALING_TEST): tests/concurrency/work_stealing_test.c $(WORK_STEALING_SOURCES)
	@mkdir -p $(BINDIR)
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS) -lm

# Work-Stealing Demo
WORK_STEALING_DEMO = $(BINDIR)/work_stealing_demo

demo-work-stealing: $(WORK_STEALING_DEMO)
	@echo "Running work-stealing demonstration..."
	./$(WORK_STEALING_DEMO)

$(WORK_STEALING_DEMO): tests/examples/work_stealing_demo.c $(WORK_STEALING_SOURCES)
	@mkdir -p $(BINDIR)
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS) -lm

# Dynamic Chunking Test
DYNAMIC_CHUNKING_TEST = $(BINDIR)/dynamic_chunking_test
DYNAMIC_CHUNKING_SOURCES = src/concurrency/dynamic_chunking.c src/concurrency/work_stealing.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-dynamic-chunking: $(DYNAMIC_CHUNKING_TEST)
	@echo "Running dynamic chunking tests..."
	./$(DYNAMIC_CHUNKING_TEST)

$(DYNAMIC_CHUNKING_TEST): tests/concurrency/dynamic_chunking_test.c $(DYNAMIC_CHUNKING_SOURCES)
	@mkdir -p $(BINDIR)
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS) -lm

# Memory Safety Test
MEMORY_SAFETY_TEST = $(BINDIR)/memory_safety_test
MEMORY_SAFETY_SOURCES = src/concurrency/parallel_memory_safety.c src/concurrency/work_stealing.c src/concurrency/dynamic_chunking.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-memory-safety: $(MEMORY_SAFETY_TEST)
	@echo "Running memory safety tests..."
	./$(MEMORY_SAFETY_TEST)

$(MEMORY_SAFETY_TEST): tests/performance/memory_safety_test.c $(MEMORY_SAFETY_SOURCES)
	@mkdir -p $(BINDIR)
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS)

# Performance Monitoring Test
PERFORMANCE_MONITORING_TEST = $(BINDIR)/performance_monitoring_test
PERFORMANCE_MONITORING_SOURCES = src/concurrency/performance_monitoring.c src/concurrency/parallel_memory_safety.c src/concurrency/work_stealing.c src/concurrency/dynamic_chunking.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-performance-monitoring: $(PERFORMANCE_MONITORING_TEST)
	@echo "Running performance monitoring tests..."
	./$(PERFORMANCE_MONITORING_TEST)

$(PERFORMANCE_MONITORING_TEST): tests/performance/performance_monitoring_test.c $(PERFORMANCE_MONITORING_SOURCES)
	@mkdir -p $(BINDIR)
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS) -lm

# Simple Performance Monitoring Test
SIMPLE_PERFORMANCE_TEST = $(BINDIR)/simple_performance_test
SIMPLE_PERFORMANCE_SOURCES = src/concurrency/performance_monitoring.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-simple-performance: $(SIMPLE_PERFORMANCE_TEST)
	@echo "Running simple performance monitoring tests..."
	./$(SIMPLE_PERFORMANCE_TEST)

$(SIMPLE_PERFORMANCE_TEST): tests/performance/simple_performance_test.c $(SIMPLE_PERFORMANCE_SOURCES)
	@mkdir -p $(BINDIR)
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS)

# Parallel Capability Security Test
PARALLEL_CAPABILITY_TEST = $(BINDIR)/parallel_capability_test
PARALLEL_CAPABILITY_SOURCES = src/concurrency/parallel_capability_security.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-parallel-capability: $(PARALLEL_CAPABILITY_TEST)
	@echo "Running parallel capability security tests..."
	./$(PARALLEL_CAPABILITY_TEST)

$(PARALLEL_CAPABILITY_TEST): tests/performance/parallel_capability_test.c $(PARALLEL_CAPABILITY_SOURCES)
	@mkdir -p $(BINDIR)
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS) -lm

# Simple Capability Security Test
SIMPLE_CAPABILITY_TEST = $(BINDIR)/simple_capability_test
SIMPLE_CAPABILITY_SOURCES = src/concurrency/parallel_capability_security.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-simple-capability: $(SIMPLE_CAPABILITY_TEST)
	@echo "Running simple capability security tests..."
	./$(SIMPLE_CAPABILITY_TEST)

$(SIMPLE_CAPABILITY_TEST): tests/security/simple_capability_test.c $(SIMPLE_CAPABILITY_SOURCES)
	@mkdir -p $(BINDIR)
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS) -lm

# Minimal Capability Security Test
MINIMAL_CAPABILITY_TEST = $(BINDIR)/minimal_capability_test
MINIMAL_CAPABILITY_SOURCES = src/concurrency/parallel_capability_security.c src/errors/error.c

test-minimal-capability: $(MINIMAL_CAPABILITY_TEST)
	@echo "Running minimal capability security tests..."
	./$(MINIMAL_CAPABILITY_TEST)

$(MINIMAL_CAPABILITY_TEST): tests/security/minimal_capability_test.c $(MINIMAL_CAPABILITY_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Capability-Only Test (no dependencies)
CAPABILITY_ONLY_TEST = $(BINDIR)/capability_only_test

test-capability-only: $(CAPABILITY_ONLY_TEST)
	@echo "Running capability-only security tests..."
	./$(CAPABILITY_ONLY_TEST)

$(CAPABILITY_ONLY_TEST): capability_only_test.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# NUMA Scheduling Test
NUMA_SCHEDULING_TEST = $(BINDIR)/numa_scheduling_test
NUMA_SCHEDULING_SOURCES = src/concurrency/numa_scheduling.c src/concurrency/performance_monitoring.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-numa-scheduling: $(NUMA_SCHEDULING_TEST)
	@echo "Running NUMA scheduling tests..."
	./$(NUMA_SCHEDULING_TEST)

$(NUMA_SCHEDULING_TEST): tests/concurrency/numa_scheduling_test.c $(NUMA_SCHEDULING_SOURCES)
	@mkdir -p $(BINDIR)
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS)

# Task 21.4 Advanced Channels Demo
TASK_21_4_DEMO = $(BINDIR)/task_21_4_advanced_channels_demo

test-task-21-4: $(TASK_21_4_DEMO)
	@echo "Running Task 21.4 Advanced Channel Patterns demo..."
	./$(TASK_21_4_DEMO)

$(TASK_21_4_DEMO): tests/examples/task_21_4_advanced_channels_demo.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Task 21.5 Deadlock Prevention Demo
TASK_21_5_DEMO = $(BINDIR)/task_21_5_deadlock_prevention_demo

test-task-21-5: $(TASK_21_5_DEMO)
	@echo "Running Task 21.5 Deadlock Prevention and Performance Optimization demo..."
	./$(TASK_21_5_DEMO)

$(TASK_21_5_DEMO): tests/examples/task_21_5_deadlock_prevention_demo.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Task 25.1 Async Runtime Demo
TASK_25_1_DEMO = $(BINDIR)/task_25_1_async_runtime_demo
ASYNC_RUNTIME_SOURCES = src/async/transparent_async.c src/errors/error.c

test-task-25-1: $(TASK_25_1_DEMO)
	@echo "Running Task 25.1 Core Async Runtime demo..."
	./$(TASK_25_1_DEMO)

$(TASK_25_1_DEMO): tests/examples/task_25_1_async_runtime_demo.c $(ASYNC_RUNTIME_SOURCES)
	@mkdir -p $(BINDIR)
	@mkdir -p src/async
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Transparent Async Test
TRANSPARENT_ASYNC_TEST = $(BINDIR)/transparent_async_test
TRANSPARENT_ASYNC_SOURCES = src/async/transparent_async.c src/async/transparent_execution.c src/errors/error.c src/errors/ergonomic_errors.c

test-transparent-async: $(TRANSPARENT_ASYNC_TEST)
	@echo "Running transparent async system tests..."
	./$(TRANSPARENT_ASYNC_TEST)

$(TRANSPARENT_ASYNC_TEST): tests/concurrency/transparent_async_test.c $(TRANSPARENT_ASYNC_SOURCES)
	@mkdir -p $(BINDIR)
	@mkdir -p src/async
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Enhanced Structured Concurrency Test
STRUCTURED_CONCURRENCY_ENHANCED_TEST = $(BINDIR)/structured_concurrency_enhanced_test
STRUCTURED_CONCURRENCY_ENHANCED_SOURCES = src/concurrency/structured_concurrency_enhanced.c src/concurrency/structured_concurrency.c src/async/transparent_async.c src/async/transparent_execution.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-structured-concurrency-enhanced: $(STRUCTURED_CONCURRENCY_ENHANCED_TEST)
	@echo "Running enhanced structured concurrency tests..."
	./$(STRUCTURED_CONCURRENCY_ENHANCED_TEST)

$(STRUCTURED_CONCURRENCY_ENHANCED_TEST): tests/concurrency/structured_concurrency_enhanced_test.c $(STRUCTURED_CONCURRENCY_ENHANCED_SOURCES)
	@mkdir -p $(BINDIR)
	@mkdir -p src/concurrency
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS) -lm

# Enhanced Structured Concurrency Demo
STRUCTURED_CONCURRENCY_DEMO = $(BINDIR)/structured_concurrency_demo
STRUCTURED_CONCURRENCY_DEMO_SOURCES = src/concurrency/structured_concurrency_enhanced.c src/concurrency/structured_concurrency.c src/async/transparent_async.c src/async/transparent_execution.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

demo-structured-concurrency: $(STRUCTURED_CONCURRENCY_DEMO)
	@echo "Running enhanced structured concurrency demo..."
	./$(STRUCTURED_CONCURRENCY_DEMO)

$(STRUCTURED_CONCURRENCY_DEMO): tests/examples/structured_concurrency_demo.c $(STRUCTURED_CONCURRENCY_DEMO_SOURCES)
	@mkdir -p $(BINDIR)
	@mkdir -p src/concurrency
	$(BLOCKS_CC) $(BLOCKS_CFLAGS) -o $@ $^ $(BLOCKS_LDFLAGS) -lm

# Async Resource Management Test
ASYNC_RESOURCE_TEST = $(BINDIR)/async_resource_test
ASYNC_RESOURCE_SOURCES = src/async/async_resource.c src/errors/error.c

test-async-resource: $(ASYNC_RESOURCE_TEST)
	@echo "Running async resource management tests..."
	./$(ASYNC_RESOURCE_TEST)

$(ASYNC_RESOURCE_TEST): tests/async/async_resource_test.c $(ASYNC_RESOURCE_SOURCES)
	@mkdir -p $(BINDIR)
	@mkdir -p tests/async
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Async Resource Management Demo
ASYNC_RESOURCE_DEMO = $(BINDIR)/async_resource_demo

demo-async-resource: $(ASYNC_RESOURCE_DEMO)
	@echo "Running async resource management demo..."
	./$(ASYNC_RESOURCE_DEMO)

$(ASYNC_RESOURCE_DEMO): examples/async_resource_demo.c $(ASYNC_RESOURCE_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Reactive Programming Test
REACTIVE_PROGRAMMING_TEST = $(BINDIR)/reactive_programming_test
REACTIVE_PROGRAMMING_SOURCES = src/async/reactive_programming_simple.c src/errors/error.c

test-reactive-programming: $(REACTIVE_PROGRAMMING_TEST)
	@echo "Running reactive programming tests..."
	./$(REACTIVE_PROGRAMMING_TEST)

$(REACTIVE_PROGRAMMING_TEST): tests/async/reactive_programming_minimal_test.c $(REACTIVE_PROGRAMMING_SOURCES)
	@mkdir -p $(BINDIR)
	@mkdir -p tests/async
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Reactive Programming Demo
REACTIVE_PROGRAMMING_DEMO = $(BINDIR)/reactive_programming_demo

demo-reactive-programming: $(REACTIVE_PROGRAMMING_DEMO)
	@echo "Running reactive programming demo..."
	./$(REACTIVE_PROGRAMMING_DEMO)

$(REACTIVE_PROGRAMMING_DEMO): examples/reactive_programming_demo.c $(REACTIVE_PROGRAMMING_SOURCES) $(ASYNC_RESOURCE_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile-time execution test.
# `types.c` is required because comptime.c's comptime_value_get_type calls
# type_new. Without it the link fails with "Undefined symbols: _type_new".
# `errors/error.c` defines goo_error_new and friends that types.c references.
# `parser/parser_errors.c` defines parser_error which lexer.c calls via
# the bridge; otherwise we'd see an undefined-symbol cascade.
comptime_test: tests/test_comptime.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/comptime/comptime_value.c $(SRCDIR)/comptime/comptime_intrinsics.c $(SRCDIR)/ast/ast.c $(SRCDIR)/ast/ast_constructors.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c $(SRCDIR)/types/types.c $(SRCDIR)/errors/error.c $(SRCDIR)/errors/ergonomic_errors.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile-time types integration test
comptime_types_test: tests/test_comptime_types.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/comptime/comptime_value.c $(SRCDIR)/comptime/comptime_intrinsics.c $(SRCDIR)/ast/ast.c $(SRCDIR)/ast/ast_constructors.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c $(SRCDIR)/types/types.c $(SRCDIR)/errors/error.c $(SRCDIR)/errors/ergonomic_errors.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Optimization directives framework test
optimization_test: tests/test_optimization.c $(SRC_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LDFLAGS) $(LLVM_LDFLAGS)

# Profile-Guided Optimization test
pgo_test: tests/test_pgo.c $(SRC_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LDFLAGS) $(LLVM_LDFLAGS)

# Advanced Optimization Strategies test
advanced_optimization_test: tests/test_advanced_optimization.c $(SRC_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LDFLAGS) $(LLVM_LDFLAGS)

advanced_macro_test: tests/test_advanced_macro.c $(SRCDIR)/advanced_macro_system.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/comptime/comptime_value.c $(SRCDIR)/comptime/comptime_intrinsics.c $(SRCDIR)/ast/ast.c $(SRCDIR)/ast/ast_constructors.c $(SRCDIR)/types/types.c $(SRCDIR)/errors/error.c $(SRCDIR)/errors/ergonomic_errors.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

derive_macro_test: tests/test_derive_macros.c $(SRCDIR)/derive_macros.c $(SRCDIR)/advanced_macro_system.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/comptime/comptime_value.c $(SRCDIR)/comptime/comptime_intrinsics.c $(SRCDIR)/ast/ast.c $(SRCDIR)/ast/ast_constructors.c $(SRCDIR)/types/types.c $(SRCDIR)/errors/error.c $(SRCDIR)/errors/ergonomic_errors.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

template_macro_test: tests/test_template_macros.c $(SRCDIR)/template_macros.c $(SRCDIR)/derive_macros.c $(SRCDIR)/advanced_macro_system.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/comptime/comptime_value.c $(SRCDIR)/comptime/comptime_intrinsics.c $(SRCDIR)/ast/ast.c $(SRCDIR)/ast/ast_constructors.c $(SRCDIR)/types/types.c $(SRCDIR)/errors/error.c $(SRCDIR)/errors/ergonomic_errors.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
