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

# Incremental, header-aware builds ("only rebuild what changed"). -MMD writes a
# .d file next to each .o listing every header that object includes
# (transitively); -MP adds phony targets for those headers so deleting one
# doesn't break the build. The .d files are -included further down, so editing a
# header (ast.h, types.h, codegen.h, runtime.h, ...) rebuilds EXACTLY the objects
# that include it — no more whole-tree `make clean` after a header edit, and no
# stale-object miscompiles. The .d files live in build/ and are removed by
# `make clean` along with the .o files.
DEPFLAGS = -MMD -MP

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
PARSER_SRCS = $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/lexer_bridge.c $(SRCDIR)/parser/parser_errors.c $(SRCDIR)/parser/parser_actions.c
AST_SRCS = $(SRCDIR)/ast/ast.c $(SRCDIR)/ast/ast_constructors.c
TYPES_SRCS = $(SRCDIR)/types/types.c $(SRCDIR)/types/type_checker.c $(SRCDIR)/types/expression_checker.c $(SRCDIR)/types/tc_fctx.c $(SRCDIR)/types/embedding.c $(SRCDIR)/types/expression_helpers.c $(SRCDIR)/types/ownership_checker.c $(SRCDIR)/types/channel_checker.c $(SRCDIR)/types/constraint_inference.c $(SRCDIR)/types/advanced_constraint_inference.c $(SRCDIR)/types/concept_generics.c $(SRCDIR)/types/higher_kinded_types.c $(SRCDIR)/types/type_level_programming.c $(SRCDIR)/types/type_level_dependent.c $(SRCDIR)/types/type_level_eval.c $(SRCDIR)/types/interface_integration.c $(SRCDIR)/types/flow_sensitive_analysis.c $(SRCDIR)/types/flow_analysis_core.c $(SRCDIR)/types/reference_manager.c $(SRCDIR)/types/hkt_auto_impl.c $(SRCDIR)/types/protocol_oriented_programming.c $(SRCDIR)/types/escape_analysis.c $(SRCDIR)/types/resource_manager.c $(SRCDIR)/types/memory_safety_integration.c $(SRCDIR)/types/bounds_verifier.c $(SRCDIR)/types/symbolic_expression.c $(SRCDIR)/types/dependent_types.c $(SRCDIR)/types/contracts.c $(SRCDIR)/types/proof_generation.c $(SRCDIR)/types/proof_smt.c $(SRCDIR)/types/proof_obligations.c $(SRCDIR)/types/proof_reporting.c $(SRCDIR)/types/runtime_optimization.c $(SRCDIR)/types/param_escape.c $(SRCDIR)/types/nonretaining.c $(SRCDIR)/types/block_escape.c $(SRCDIR)/types/terminating_stmt.c $(SRCDIR)/types/shim_signatures.c $(SRCDIR)/types/lane_ownership.c
CODEGEN_SRCS = $(SRCDIR)/codegen/codegen.c $(SRCDIR)/codegen/cfctx.c $(SRCDIR)/codegen/value_scope.c $(SRCDIR)/codegen/type_mapping.c $(SRCDIR)/codegen/function_codegen.c $(SRCDIR)/codegen/statement_codegen.c $(SRCDIR)/codegen/expression_codegen.c $(SRCDIR)/codegen/call_codegen.c $(SRCDIR)/codegen/composite_codegen.c $(SRCDIR)/codegen/lowlevel_codegen.c $(SRCDIR)/codegen/error_union_codegen.c $(SRCDIR)/codegen/nullable_codegen.c $(SRCDIR)/codegen/interface_codegen.c $(SRCDIR)/codegen/runtime_integration.c $(SRCDIR)/codegen/wasm_codegen.c $(SRCDIR)/codegen/monomorphize.c
RUNTIME_SRCS = $(SRCDIR)/runtime/runtime.c $(SRCDIR)/runtime/platform.c $(SRCDIR)/runtime/concurrency.c $(SRCDIR)/runtime/channels.c $(SRCDIR)/runtime/sync.c $(SRCDIR)/runtime/sync_shim.c $(SRCDIR)/runtime/time_shim.c $(SRCDIR)/runtime/deadlock.c $(SRCDIR)/runtime/arena.c $(SRCDIR)/runtime/defer.c
ERROR_SRCS = $(SRCDIR)/errors/error.c $(SRCDIR)/errors/ergonomic_errors.c
IDE_SRCS = $(SRCDIR)/ide/hot_reload.c $(SRCDIR)/ide/repl.c $(SRCDIR)/ide/performance_monitor.c $(SRCDIR)/ide/repl_errors.c $(SRCDIR)/ide/time_travel_debug.c $(SRCDIR)/ide/time_travel_debug_repl.c $(SRCDIR)/ide/repl_syntax.c
# Only import_resolver.c from package/ is part of the compiler. The rest of
# the directory (IPFS/registry/p2p modules) is compiled by nothing and has
# pre-existing build breakage (e.g. gateway_intelligence.c's stale
# ipfs_gateway_create call); the broken gmod CLI itself was quarantined in
# P5.5. Wiring a real package manager is post-v1
# (docs/2026-07-08-v1-roadmap.md).
PACKAGE_SRCS = $(SRCDIR)/package/import_resolver.c
TEST_FRAMEWORK_SRCS = $(TEST_FRAMEWORK_DIR)/test_framework.c

COMPTIME_SRCS = $(SRCDIR)/comptime/comptime.c $(SRCDIR)/comptime/comptime_value.c $(SRCDIR)/comptime/comptime_intrinsics.c $(SRCDIR)/comptime/comptime_types.c $(SRCDIR)/comptime/optimization.c $(SRCDIR)/comptime/profile_guided_optimization.c $(SRCDIR)/comptime/advanced_optimization.c $(SRCDIR)/comptime/hardware_aware.c $(SRCDIR)/comptime/code_specialization.c $(SRCDIR)/advanced_macro_system.c $(SRCDIR)/derive_macros.c $(SRCDIR)/template_macros.c
CURRENT_SRCS = $(LEXER_SRCS) $(PARSER_SRCS) $(AST_SRCS) $(TYPES_SRCS) $(CODEGEN_SRCS) $(RUNTIME_SRCS) $(ERROR_SRCS) $(IDE_SRCS) $(PACKAGE_SRCS) $(COMPTIME_SRCS)
COMPILER_SRCS = $(COMPILERDIR)/goo.c
SRC_OBJS = $(CURRENT_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
TEST_FRAMEWORK_OBJ = $(TEST_FRAMEWORK_SRCS:$(TEST_FRAMEWORK_DIR)/%.c=$(BUILDDIR)/framework/%.o)
OBJS = $(SRC_OBJS) $(TEST_FRAMEWORK_OBJ)

# ---------------------------------------------------------------------------
# P5.6: bin/goo links ONLY the reachable set below. The full TYPES_SRCS /
# COMPTIME_SRCS / IDE_SRCS lists above still feed OBJS for the standalone
# test targets that exercise the unlinked frameworks (constraint inference,
# concept generics, HKT, flow analysis, reference manager, ...) — dropping a
# module HERE quarantines it from the shipped compiler without deleting its
# tests. The membership test is symbols, not headers: a module joins this
# list only if the link otherwise fails with an undefined reference.
# ---------------------------------------------------------------------------
GOO_TYPES_SRCS = $(SRCDIR)/types/types.c $(SRCDIR)/types/type_checker.c $(SRCDIR)/types/expression_checker.c $(SRCDIR)/types/tc_fctx.c $(SRCDIR)/types/embedding.c $(SRCDIR)/types/expression_helpers.c $(SRCDIR)/types/channel_checker.c $(SRCDIR)/types/param_escape.c $(SRCDIR)/types/nonretaining.c $(SRCDIR)/types/block_escape.c $(SRCDIR)/types/terminating_stmt.c $(SRCDIR)/types/shim_signatures.c $(SRCDIR)/types/ownership_checker.c $(SRCDIR)/types/lane_ownership.c
GOO_COMPTIME_SRCS = $(SRCDIR)/comptime/comptime.c $(SRCDIR)/comptime/comptime_value.c $(SRCDIR)/comptime/comptime_intrinsics.c $(SRCDIR)/comptime/comptime_types.c
GOO_SRCS = $(LEXER_SRCS) $(PARSER_SRCS) $(AST_SRCS) $(GOO_TYPES_SRCS) $(CODEGEN_SRCS) $(RUNTIME_SRCS) $(ERROR_SRCS) $(PACKAGE_SRCS) $(GOO_COMPTIME_SRCS)
GOO_OBJS = $(GOO_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

# Runtime library
RUNTIME_LIB = $(LIBDIR)/libgoo_runtime.a
# Runtime library must include every translation unit referenced by
# the runtime entrypoints. runtime.o's goo_init/goo_exit call into
# deadlock.o, and concurrency.o calls channels/sync/platform — leaving
# any of these out fails the link of even a hello-world executable.
RUNTIME_OBJS = $(BUILDDIR)/runtime/runtime.o $(BUILDDIR)/runtime/platform.o $(BUILDDIR)/runtime/concurrency.o $(BUILDDIR)/runtime/channels.o $(BUILDDIR)/runtime/sync.o $(BUILDDIR)/runtime/sync_shim.o $(BUILDDIR)/runtime/time_shim.o $(BUILDDIR)/runtime/deadlock.o $(BUILDDIR)/runtime/io.o $(BUILDDIR)/runtime/arena.o $(BUILDDIR)/runtime/defer.o

# Main targets
COMPILER = $(BINDIR)/goo
ANALYZER = $(BINDIR)/goo-analyzer
TEST_RUNNER = $(BINDIR)/test_runner
# P5.5: goo-repl, goo-repl-enhanced, goo-lsp, goo-lsp-standalone, gmod,
# goo-debug-adapter, and goo-dashboard were quarantined out of the tree —
# each fabricated its results (hardcoded eval, demo menus, simulated PID,
# canned metrics) or did not compile (gmod). Recover from git history if a
# real implementation is ever built (see docs/2026-07-08-v1-roadmap.md
# post-v1 list). lsp-enhanced stays pending the P5.11 open decision.
LSP_ENHANCED_SERVER = $(BINDIR)/goo-lsp-enhanced
TEST_PERFORMANCE = $(BINDIR)/test_performance
TEST_ERROR_REPORTING = $(BINDIR)/test_error_reporting

.PHONY: all clean test install lexer analyzer coverage coverage-report coverage-clean debug format check runtime-lib test-lexer test-codegen test-units goostd-resolver-probe param-escape-test block-escape-test arena-routing-test arena-free-probe arena-valgrind-probe arena-rss-probe

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

# Bison rules. GROUPED TARGET (`&:`, GNU Make >= 4.3): one bison invocation
# produces BOTH parser.tab.c AND parser.tab.h. Without `&:` Make treats this as
# two independent targets sharing a recipe, and under `-j` can invoke bison
# twice concurrently for the two targets (or race it against an old duplicate
# rule for parser.tab.c alone — see the removed rule that used to live under
# "Parser generation" below). `&:` tells Make the single recipe produces every
# listed output, so it schedules bison once and blocks every consumer of
# either output until it finishes.
$(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.tab.h &: $(SRCDIR)/parser/parser.y
	bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y

# parser_actions.c, lexer_bridge.c and parser.tab.c itself all #include the
# generated parser.tab.h. On a clean build the per-object .d files (DEPFLAGS,
# below) don't exist yet, so Make can't know that from the .c sources alone —
# under `-j` it would race their compilation against bison (fails with
# "parser.tab.h: No such file or directory" or, if bison is mid-write,
# "YYSTYPE has no member / <TOKEN> undeclared"). An ORDER-ONLY prereq (`|`)
# forces parser.tab.h to exist first WITHOUT forcing a rebuild when the header
# legitimately changes — the .d files drive real header->object rebuilds once
# they exist. This is what makes `make -j` correct from a clean tree.
$(SRC_OBJS) $(TEST_FRAMEWORK_OBJ) $(RUNTIME_OBJS): | $(SRCDIR)/parser/parser.tab.h

# Object file compilation
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILDDIR)/compiler/%.o: $(COMPILERDIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(DEPFLAGS) -c $< -o $@

# Test framework object compilation
$(BUILDDIR)/framework/%.o: $(TEST_FRAMEWORK_DIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(DEPFLAGS) -c $< -o $@

# Pull in the compiler-generated header dependencies (see DEPFLAGS). These files
# don't exist on a fresh checkout; Make treats absent `-include` files as
# non-fatal and generates them on the first build via the rules above, so the
# first build is a normal full build and every build after it is header-aware.
-include $(SRC_OBJS:.o=.d) $(TEST_FRAMEWORK_OBJ:.o=.d) $(RUNTIME_OBJS:.o=.d)

# Main compiler executable
goo: $(COMPILER)

# Compiler binary does not link the test framework — it's a runtime concern
# for test runners. The test framework's header (test/test_framework.h) is
# missing from include/, so building TEST_FRAMEWORK_OBJ fails; that breakage
# belongs to task #33 and shouldn't gate compiler builds.
$(COMPILER): $(GOO_OBJS) $(COMPILER_SRCS) | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(COMPILER_SRCS) $(GOO_OBJS) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS)

# Runtime library
runtime-lib: $(RUNTIME_LIB)

$(RUNTIME_LIB): $(RUNTIME_OBJS) | $(LIBDIR)
	ar rcs $@ $^

# (P5.7: test-pipeline retired — tests/test_runner.c's assertions were
# near-vacuous (`tokens_found || exit==0` style escape hatches). The golden
# suites + tests/cli/cli_test.sh assert the same pipeline end-to-end with
# real expected-output and exit-code checks.)

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

# Select comms are now type-checked — sending the wrong element type must be
# a clean compile error, not silent slot-machinery corruption at runtime.
selectsend-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== selectsend-reject-probe: type-mismatched select send must reject ==="
	@printf 'package main\nfunc main(){ ch := make_chan(int, 1); select { case ch <- "oops":\n_ = 1\ndefault:\n_ = 2\n } }\n' > build/selectsend_reject.goo
	@rm -f build/selectsend_reject
	@$(COMPILER) -o build/selectsend_reject build/selectsend_reject.goo > build/selectsend_reject.out 2> build/selectsend_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "selectsend-reject-probe: FAIL (compiled rc=0 — mismatched send silently accepted)"; exit 1; fi; \
	if [ -x build/selectsend_reject ]; then echo "selectsend-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "select send" build/selectsend_reject.err; then echo "selectsend-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/selectsend_reject.err; exit 1; fi; \
	echo "selectsend-reject-probe: PASS (rejected rc=$$rc)"

# A call (including a builtin like append) in a package-level var initializer
# used to SIGSEGV: codegen_generate_var_decl's global path had no positioned
# LLVM builder (codegen->current_function is NULL), but builtin codegen
# issues LLVMBuildXxx calls unconditionally, dereferencing a null insert
# block before the "requires constant initializer" backstop was ever reached.
# An early AST_CALL_EXPR check used to reject this cleanly instead of
# crashing. Task 2 (var-init cluster) replaced that guard: a call — including
# a builtin — in a global initializer is now DEFERRED to a synthesized
# goo.global_init(), evaluated (with a real positioned builder) before user
# main, so it compiles AND runs correctly instead of being rejected. This
# probe now asserts the new behavior; the rc!=139 assertion stays as the
# regression guard for the original crash.
globalcall-init-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== globalcall-init-probe: builtin call in global initializer now runs correctly (Task 2), not crash ==="
	@printf 'package main\nimport "fmt"\nvar g = append([]int64{1}, 2)\nfunc main(){ fmt.Println(len(g)); fmt.Println(g[0]); fmt.Println(g[1]) }\n' > build/globalcall_init.goo
	@rm -f build/globalcall_init
	@$(COMPILER) -o build/globalcall_init build/globalcall_init.goo > build/globalcall_init.out 2> build/globalcall_init.err; rc=$$?; \
	if [ $$rc -eq 139 ]; then echo "globalcall-init-probe: FAIL (SIGSEGV, rc=139 — crashed instead of compiling)"; exit 1; fi; \
	if [ $$rc -ne 0 ]; then echo "globalcall-init-probe: FAIL (compile rc=$$rc, expected success)"; cat build/globalcall_init.err; exit 1; fi; \
	if [ ! -x build/globalcall_init ]; then echo "globalcall-init-probe: FAIL (no binary emitted despite rc=0)"; exit 1; fi; \
	actual=$$(./build/globalcall_init); \
	expected=$$(printf '2\n1\n2'); \
	if [ "$$actual" != "$$expected" ]; then echo "globalcall-init-probe: FAIL (wrong output)"; echo "expected: $$expected"; echo "actual: $$actual"; exit 1; fi; \
	echo "globalcall-init-probe: PASS (builtin call in global initializer compiled and ran correctly)"

# Implicit float->int is a silent bit-store, not a conversion: codegen has no
# narrowing path for this direction, so it reinterprets the float's raw bits
# as an int (`var i int64 = float32(2.5)` produced 1075838976, not 2). Guards
# the type_compatible() float->int asymmetry across the four shapes that used
# to silently accept it (var-decl init, plain assignment, append elem,
# chan-send — the last added in T4 since type_check_channel_send_op routes
# through the same type_compatible()), plus a positive control confirming
# int->float stays permitted (PR #99 probes depend on it: `var y float64 = x`,
# `[]float64{1, 2.5}`).
floatint-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== floatint-reject-probe: implicit float->int must reject, int->float stays permitted ==="
	@printf 'package main\nfunc main(){ g := float32(2.5); var i int64 = g; _ = i }\n' > build/fi_vardecl.goo
	@printf 'package main\nfunc main(){ g := float32(2.5); var i int64; i = g; _ = i }\n' > build/fi_assign.goo
	@printf 'package main\nfunc main(){ g := float32(2.5); s := append([]int64{1}, g); _ = s }\n' > build/fi_append.goo
	@printf 'package main\nfunc main(){ g := float32(2.5); ch := make_chan(int64, 1); ch <- g; _ = ch }\n' > build/fi_chansend.goo
	@printf 'package main\nimport "fmt"\nfunc main(){ x := int64(3); var y float64 = x; s := []float64{1, 2.5}; fmt.Println(y, s[0], s[1]) }\n' > build/fi_int2float.goo
	@rm -f build/fi_vardecl build/fi_assign build/fi_append build/fi_chansend build/fi_int2float
	@$(COMPILER) -o build/fi_vardecl build/fi_vardecl.goo > build/fi_vardecl.out 2> build/fi_vardecl.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "floatint-reject-probe: FAIL (var-decl init: float32->int64 silently accepted)"; exit 1; fi; \
	if [ -x build/fi_vardecl ]; then echo "floatint-reject-probe: FAIL (var-decl init: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "Cannot assign float32 to int64" build/fi_vardecl.err; then echo "floatint-reject-probe: FAIL (var-decl init: wrong/missing diagnostic)"; cat build/fi_vardecl.err; exit 1; fi
	@$(COMPILER) -o build/fi_assign build/fi_assign.goo > build/fi_assign.out 2> build/fi_assign.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "floatint-reject-probe: FAIL (assignment: float32->int64 silently accepted)"; exit 1; fi; \
	if [ -x build/fi_assign ]; then echo "floatint-reject-probe: FAIL (assignment: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "Cannot assign float32 to int64" build/fi_assign.err; then echo "floatint-reject-probe: FAIL (assignment: wrong/missing diagnostic)"; cat build/fi_assign.err; exit 1; fi
	@$(COMPILER) -o build/fi_append build/fi_append.goo > build/fi_append.out 2> build/fi_append.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "floatint-reject-probe: FAIL (append elem: float32->int64 silently accepted)"; exit 1; fi; \
	if [ -x build/fi_append ]; then echo "floatint-reject-probe: FAIL (append elem: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "append: cannot use float32 as element of \[\]int64" build/fi_append.err; then echo "floatint-reject-probe: FAIL (append elem: wrong/missing diagnostic)"; cat build/fi_append.err; exit 1; fi
	@$(COMPILER) -o build/fi_chansend build/fi_chansend.goo > build/fi_chansend.out 2> build/fi_chansend.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "floatint-reject-probe: FAIL (chan-send: float32->int64 silently accepted)"; exit 1; fi; \
	if [ -x build/fi_chansend ]; then echo "floatint-reject-probe: FAIL (chan-send: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "Cannot send float32 to channel of int64" build/fi_chansend.err; then echo "floatint-reject-probe: FAIL (chan-send: wrong/missing diagnostic)"; cat build/fi_chansend.err; exit 1; fi
	@$(COMPILER) -o build/fi_int2float build/fi_int2float.goo > build/fi_int2float.out 2> build/fi_int2float.err; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "floatint-reject-probe: FAIL (int->float positive control wrongly rejected)"; cat build/fi_int2float.err; exit 1; fi; \
	out="$$(./build/fi_int2float)"; if [ "$$out" != "3 1 2.5" ]; then echo "floatint-reject-probe: FAIL (int->float positive control output '$$out' != '3 1 2.5')"; exit 1; fi
	@echo "floatint-reject-probe: PASS (all four float->int shapes rejected; int->float still permitted)"

# Same float-context exclusion, modulo shape: `(1 % 2) * g`. Go legally
# computes this as 2.5 (constant-folds `1%2` to the int 1 before promoting);
# Goo rejects for the same reason the migrated constdiv reject fixture does
# (tests/golden/reject/constdiv-reject-probe.{goo,err.txt}). See
# examples/constmod_reject.goo.
constmod-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== constmod-reject-probe: (1%2)*g float-context modulo must reject ==="
	@rm -f build/constmod_reject
	@$(COMPILER) -o build/constmod_reject examples/constmod_reject.goo > build/constmod_reject.out 2> build/constmod_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "constmod-reject-probe: FAIL (compiled rc=0 — (1%2)*g silently accepted)"; exit 1; fi; \
	if [ -x build/constmod_reject ]; then echo "constmod-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR" build/constmod_reject.err; then echo "constmod-reject-probe: FAIL (invalid IR reached the LLVM verifier instead of a clean rejection)"; cat build/constmod_reject.err; exit 1; fi; \
	if ! grep -q "no implicit int/float conversion" build/constmod_reject.err; then echo "constmod-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/constmod_reject.err; exit 1; fi; \
	echo "constmod-reject-probe: PASS (rejected rc=$$rc)"

# Final-sweep negative gate: a bare int literal meeting a float operand
# across `%` (`1 % g`, g float32) must be REJECTED, not silently adapted.
# Unlike constdiv/constmod above (stricter-than-Go subtree exclusions), Go
# itself also rejects `1 % g` (mismatched types) — this rejection is
# Go-conformant. See examples/baremod_reject.goo and the TOKEN_MODULO gate
# on expression_checker.c's cross-kind adaptation block.
baremod-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== baremod-reject-probe: 1 % g bare-literal cross-kind modulo must reject ==="
	@rm -f build/baremod_reject
	@$(COMPILER) -o build/baremod_reject examples/baremod_reject.goo > build/baremod_reject.out 2> build/baremod_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "baremod-reject-probe: FAIL (compiled rc=0 — 1 % g silently accepted)"; exit 1; fi; \
	if [ -x build/baremod_reject ]; then echo "baremod-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR" build/baremod_reject.err; then echo "baremod-reject-probe: FAIL (invalid IR reached the LLVM verifier instead of a clean rejection)"; cat build/baremod_reject.err; exit 1; fi; \
	if ! grep -q "no implicit int/float conversion" build/baremod_reject.err; then echo "baremod-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/baremod_reject.err; exit 1; fi; \
	echo "baremod-reject-probe: PASS (rejected rc=$$rc)"

# Task 3 negative gate: a constant that overflows its declared int8 width
# must be REJECTED, not silently truncated (`var b int8 = 300` printed 44
# before this fix). Unlike constdiv/constmod/baremod above (deliberately
# STRICTER than Go), this rejection is Go-CONFORMANT — go run rejects the
# same program with "... overflows int8" too. See examples/constint8_reject.goo
# and literal_fits_type's doc comment in expression_checker.c.
constint8-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== constint8-reject-probe: var b int8 = 300 must reject (Go-conformant) ==="
	@rm -f build/constint8_reject
	@$(COMPILER) -o build/constint8_reject examples/constint8_reject.goo > build/constint8_reject.out 2> build/constint8_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "constint8-reject-probe: FAIL (compiled rc=0 — 300 silently accepted for int8)"; exit 1; fi; \
	if [ -x build/constint8_reject ]; then echo "constint8-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR" build/constint8_reject.err; then echo "constint8-reject-probe: FAIL (invalid IR reached the LLVM verifier instead of a clean rejection)"; cat build/constint8_reject.err; exit 1; fi; \
	if ! grep -q "overflows int8" build/constint8_reject.err; then echo "constint8-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/constint8_reject.err; exit 1; fi; \
	echo "constint8-reject-probe: PASS (rejected rc=$$rc)"

# Task 3 negative gate: a negative constant assigned to an unsigned declared
# type must be REJECTED, not silently wrapped (`var u uint8 = -1` printed
# 255 before this fix). Go-conformant (go run rejects with "... overflows
# uint8" too). See examples/constuint8_reject.goo — the key edge this probe
# guards is the NEGATION-THREADING itself: literal_fits_type must see
# negated=true here to reject correctly (a naive unsigned check that ignored
# the sign would let -1 alias raw magnitude 1, which trivially fits).
constuint8-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== constuint8-reject-probe: var u uint8 = -1 must reject (Go-conformant) ==="
	@rm -f build/constuint8_reject
	@$(COMPILER) -o build/constuint8_reject examples/constuint8_reject.goo > build/constuint8_reject.out 2> build/constuint8_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "constuint8-reject-probe: FAIL (compiled rc=0 — -1 silently accepted for uint8)"; exit 1; fi; \
	if [ -x build/constuint8_reject ]; then echo "constuint8-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR" build/constuint8_reject.err; then echo "constuint8-reject-probe: FAIL (invalid IR reached the LLVM verifier instead of a clean rejection)"; cat build/constuint8_reject.err; exit 1; fi; \
	if ! grep -q "overflows uint8" build/constuint8_reject.err; then echo "constuint8-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/constuint8_reject.err; exit 1; fi; \
	echo "constuint8-reject-probe: PASS (rejected rc=$$rc)"

# Task 3 negative gate: a float constant that overflows float32's finite
# range must be REJECTED, not silently rounded to +/-inf (`var f float32 =
# 1e40` printed +Inf before this fix). Go-conformant (go run rejects with
# "... overflows float32" too). See examples/constf32_reject.goo and
# literal_fits_type's float32 arm (float32_is_finite((float)v) after strtod
# — a raw IEEE-754 bit test, not the <math.h> isfinite macro, which pulls in
# a `long double` declaration CompCert's ccomp build cannot compile).
constf32-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== constf32-reject-probe: var f float32 = 1e40 must reject (Go-conformant) ==="
	@rm -f build/constf32_reject
	@$(COMPILER) -o build/constf32_reject examples/constf32_reject.goo > build/constf32_reject.out 2> build/constf32_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "constf32-reject-probe: FAIL (compiled rc=0 — 1e40 silently accepted for float32)"; exit 1; fi; \
	if [ -x build/constf32_reject ]; then echo "constf32-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR" build/constf32_reject.err; then echo "constf32-reject-probe: FAIL (invalid IR reached the LLVM verifier instead of a clean rejection)"; cat build/constf32_reject.err; exit 1; fi; \
	if ! grep -q "overflows float32" build/constf32_reject.err; then echo "constf32-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/constf32_reject.err; exit 1; fi; \
	echo "constf32-reject-probe: PASS (rejected rc=$$rc)"

# Task 3 review-fix negative gate: a float constant overflowing float64's
# finite range must be REJECTED, not silently saturated to +/-inf (`var f
# float64 = 1e309` compiled and printed inf — the first task-3 commit's
# errno-based float64 check was dead because the lexer/parser pipeline
# hands the checker the TEXT "inf", which strtod parses without ERANGE; see
# examples/constf64_reject.goo and float64_is_finite's doc comment in
# expression_checker.c). Go-conformant (go run rejects with "... overflows
# float64" too).
constf64-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== constf64-reject-probe: var f float64 = 1e309 must reject (Go-conformant) ==="
	@rm -f build/constf64_reject
	@$(COMPILER) -o build/constf64_reject examples/constf64_reject.goo > build/constf64_reject.out 2> build/constf64_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "constf64-reject-probe: FAIL (compiled rc=0 — 1e309 silently accepted for float64)"; exit 1; fi; \
	if [ -x build/constf64_reject ]; then echo "constf64-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR" build/constf64_reject.err; then echo "constf64-reject-probe: FAIL (invalid IR reached the LLVM verifier instead of a clean rejection)"; cat build/constf64_reject.err; exit 1; fi; \
	if ! grep -q "overflows float64" build/constf64_reject.err; then echo "constf64-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/constf64_reject.err; exit 1; fi; \
	echo "constf64-reject-probe: PASS (rejected rc=$$rc)"

# Task 3b negative gate: a CONSTANT conversion whose operand overflows the
# target must be REJECTED (`b := int8(300)` compiled and printed 44 before
# this fix) while a runtime-value conversion (`int8(x)`) stays legal
# truncation — that asymmetry is Go's, and conv_probe.goo's runtime-variable
# SExt/Trunc lines lock the legal side. Go-conformant (go run rejects with
# "constant 300 overflows int8"). See examples/constconv_reject.goo and
# check_conversion_operand_range in expression_checker.c.
constconv-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== constconv-reject-probe: b := int8(300) constant conversion must reject (Go-conformant) ==="
	@rm -f build/constconv_reject
	@$(COMPILER) -o build/constconv_reject examples/constconv_reject.goo > build/constconv_reject.out 2> build/constconv_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "constconv-reject-probe: FAIL (compiled rc=0 — int8(300) silently accepted)"; exit 1; fi; \
	if [ -x build/constconv_reject ]; then echo "constconv-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR" build/constconv_reject.err; then echo "constconv-reject-probe: FAIL (invalid IR reached the LLVM verifier instead of a clean rejection)"; cat build/constconv_reject.err; exit 1; fi; \
	if ! grep -q "overflows int8" build/constconv_reject.err; then echo "constconv-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/constconv_reject.err; exit 1; fi; \
	echo "constconv-reject-probe: PASS (rejected rc=$$rc)"

# Task 2 (float-literal-fidelity) negative gate: a CONSTANT integer
# conversion whose FLOAT operand is not integral must be REJECTED (`a :=
# int(3.5)` compiled and printed 3 before this fix) while an INTEGRAL float
# constant (`int(2.0)`, `int32(-4.0)` — see const_range_probe.goo's Task 2
# lines) and a runtime-value conversion (`int(x)`, x float) both stay
# legal — that asymmetry is Go's, same shape as constconv-reject-probe's
# overflow case above. Go-conformant: go run rejects (current toolchain
# wording differs — "cannot convert 3.5 (untyped float constant) to type
# int" — Goo's own diagnostic vocabulary continues check_literal_range's
# "constant %s overflows %s" sibling wording instead; see task-2 report).
# See examples/consttrunc_reject.goo and check_literal_integral /
# check_conversion_operand_range in expression_checker.c.
consttrunc-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== consttrunc-reject-probe: a := int(3.5) constant conversion must reject (Go-conformant) ==="
	@rm -f build/consttrunc_reject
	@$(COMPILER) -o build/consttrunc_reject examples/consttrunc_reject.goo > build/consttrunc_reject.out 2> build/consttrunc_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "consttrunc-reject-probe: FAIL (compiled rc=0 — int(3.5) silently accepted)"; exit 1; fi; \
	if [ -x build/consttrunc_reject ]; then echo "consttrunc-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR" build/consttrunc_reject.err; then echo "consttrunc-reject-probe: FAIL (invalid IR reached the LLVM verifier instead of a clean rejection)"; cat build/consttrunc_reject.err; exit 1; fi; \
	if ! grep -q "truncated to integer" build/consttrunc_reject.err; then echo "consttrunc-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/consttrunc_reject.err; exit 1; fi; \
	echo "consttrunc-reject-probe: PASS (rejected rc=$$rc)"

# Task 3b negative gate: a slice-literal element constant overflowing the
# declared element type must be REJECTED (`t := []int8{300}` compiled and
# printed 44 before this fix — check_slice_elements never adapted elements,
# so the task-3 range check never saw them; array and map-value sinks are
# hooked by the same fix). Go-conformant (go run rejects with "cannot use
# 300 ... as int8 value in array or slice literal (overflows)"). See
# examples/constelem_reject.goo. The boundary ACCEPT side ([]int8{127,-128})
# lives in const_range_probe.goo.
constelem-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== constelem-reject-probe: t := []int8{300} element constant must reject (Go-conformant) ==="
	@rm -f build/constelem_reject
	@$(COMPILER) -o build/constelem_reject examples/constelem_reject.goo > build/constelem_reject.out 2> build/constelem_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "constelem-reject-probe: FAIL (compiled rc=0 — []int8{300} silently accepted)"; exit 1; fi; \
	if [ -x build/constelem_reject ]; then echo "constelem-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR" build/constelem_reject.err; then echo "constelem-reject-probe: FAIL (invalid IR reached the LLVM verifier instead of a clean rejection)"; cat build/constelem_reject.err; exit 1; fi; \
	if ! grep -q "overflows int8" build/constelem_reject.err; then echo "constelem-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/constelem_reject.err; exit 1; fi; \
	echo "constelem-reject-probe: PASS (rejected rc=$$rc)"

# var-init-cluster review-fix negative gate: a constant that overflows the
# BASE type behind a NULLABLE declared type must be REJECTED (`var gz ?int8
# = 300` compiled and printed 44 before this fix — adapt_var_decl_initializer
# bailed on any TYPE_NULLABLE declared type since it isn't itself numeric, so
# the range check never saw the literal). Go-conformant (go run rejects the
# unwrapped equivalent with "constant 300 overflows int8" too). See
# examples/constnul_reject.goo and adapt_var_decl_initializer's doc comment
# in expression_checker.c. The accept side (boundary + negation through a
# nullable declared type) lives in nullable_adapt_probe.goo.
constnul-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== constnul-reject-probe: var gz ?int8 = 300 must reject (Go-conformant) ==="
	@rm -f build/constnul_reject
	@$(COMPILER) -o build/constnul_reject examples/constnul_reject.goo > build/constnul_reject.out 2> build/constnul_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "constnul-reject-probe: FAIL (compiled rc=0 — 300 silently accepted for ?int8)"; exit 1; fi; \
	if [ -x build/constnul_reject ]; then echo "constnul-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR" build/constnul_reject.err; then echo "constnul-reject-probe: FAIL (invalid IR reached the LLVM verifier instead of a clean rejection)"; cat build/constnul_reject.err; exit 1; fi; \
	if ! grep -q "overflows int8" build/constnul_reject.err; then echo "constnul-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/constnul_reject.err; exit 1; fi; \
	echo "constnul-reject-probe: PASS (rejected rc=$$rc)"

# Task 1 (checker hygiene) negative gate: `%` on FLOAT operands must be
# REJECTED at the checker (`g := 2.5; h := g % 2.0` crashed codegen with two
# opaque errors — "Failed to generate binary operation" then "Failed to
# generate initializer" — before this fix; type_check_arithmetic_op's float-
# promotion branch happily returned FLOAT64 for `%`, and codegen's
# TOKEN_MODULO arm is integer-only). Go-conformant: go run rejects the same
# program too ("invalid operation: operator % not defined on g (variable of
# type float64)"). See examples/floatmod_reject.goo and
# type_check_arithmetic_op's `%` gate in expression_helpers.c.
floatmod-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== floatmod-reject-probe: g % 2.0 (g float64) must reject (Go-conformant) ==="
	@rm -f build/floatmod_reject
	@$(COMPILER) -o build/floatmod_reject examples/floatmod_reject.goo > build/floatmod_reject.out 2> build/floatmod_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "floatmod-reject-probe: FAIL (compiled rc=0 — g % 2.0 silently accepted)"; exit 1; fi; \
	if [ -x build/floatmod_reject ]; then echo "floatmod-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR" build/floatmod_reject.err; then echo "floatmod-reject-probe: FAIL (invalid IR reached the LLVM verifier instead of a clean rejection)"; cat build/floatmod_reject.err; exit 1; fi; \
	if ! grep -q "not defined on float" build/floatmod_reject.err; then echo "floatmod-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/floatmod_reject.err; exit 1; fi; \
	echo "floatmod-reject-probe: PASS (rejected rc=$$rc)"

# Task 3 (checker hygiene): a rejected declaration must not cascade into a
# pile of spurious "Undefined variable" errors for every later use of the
# name. Before this fix, `var b int8 = 300; fmt.Println(int(b)); c := b;
# fmt.Println(int(c))` produced FIVE errors (the real overflow, plus
# "Undefined variable 'b'" x2, "Invalid initializer expression", and
# "Undefined variable 'c'") — go run reports exactly ONE. Root cause:
# type_check_var_decl skipped scope registration on every initializer-
# failure path, so `b` was never bound and every downstream reference
# looked undefined. Fix: on a failed initializer where the declaration
# carries an EXPLICIT type, register the name(s) with that declared type
# before returning failure (see register_declared_names_after_failure in
# type_checker.c) — compilation still fails, only the checker's scope
# state recovers so it stops manufacturing new errors. Today's count with
# the fix applied is 1 (down from 5); the assertion below allows up to 2
# to leave headroom without re-tightening the probe. See
# examples/cascade_reject.goo and the task-3 report for the `:=`-chain
# residual this fix does NOT cover (a failed `:=` RHS has no type to fall
# back on, so `c := <bad-rhs>` still cascades).
cascade-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== cascade-reject-probe: one rejected decl must not cascade into spurious errors ==="
	@rm -f build/cascade_reject
	@$(COMPILER) -o build/cascade_reject examples/cascade_reject.goo > build/cascade_reject.out 2> build/cascade_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "cascade-reject-probe: FAIL (compiled rc=0 — int8 overflow silently accepted)"; exit 1; fi; \
	if [ -x build/cascade_reject ]; then echo "cascade-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR|Segmentation|SIGSEGV" build/cascade_reject.err; then echo "cascade-reject-probe: FAIL (invalid IR/crash reached instead of a clean rejection)"; cat build/cascade_reject.err; exit 1; fi; \
	if ! grep -q "overflows int8" build/cascade_reject.err; then echo "cascade-reject-probe: FAIL (wrong/missing diagnostic for the real error)"; cat build/cascade_reject.err; exit 1; fi; \
	errcount=$$(grep -c 'error' build/cascade_reject.err); \
	if [ $$errcount -gt 2 ]; then echo "cascade-reject-probe: FAIL (cascade regressed — $$errcount error lines, expected <=2)"; cat build/cascade_reject.err; exit 1; fi; \
	echo "cascade-reject-probe: PASS (rejected rc=$$rc, $$errcount error line(s) <= 2)"

# P2.8 T4.2: the `:=`-chain residual cascade-reject-probe's own comment
# flagged as uncovered ("a failed `:=` RHS has no type to fall back on, so
# `c := <bad-rhs>` still cascades"). `x := undefinedFn(); y := x + 1;
# println(y)` produced FIVE errors before this fix (the real undefined-
# function error, plus "Undefined variable 'x'", "Invalid initializer
# expression" x2, and "Undefined variable 'y'") — see
# examples/cascade_binop_reject.goo for the full before/after account. Fix:
# register a TYPE_POISON marker in scope instead of nothing on a `:=`
# initializer failure, propagate it silently through
# type_check_binary_expr, and skip the generic wrapper diagnostic once the
# specific cause already reported. Unlike cascade-reject-probe's <=2
# headroom, this asserts the count EXACTLY — the recon's acceptance bar for
# this probe shape is precisely one diagnostic, not "fewer than before".
cascade-binop-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== cascade-binop-reject-probe: a failed := decl must not cascade through later uses ==="
	@rm -f build/cascade_binop_reject
	@$(COMPILER) -o build/cascade_binop_reject examples/cascade_binop_reject.goo > build/cascade_binop_reject.out 2> build/cascade_binop_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "cascade-binop-reject-probe: FAIL (compiled rc=0 — undefined function silently accepted)"; exit 1; fi; \
	if [ -x build/cascade_binop_reject ]; then echo "cascade-binop-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR|Segmentation|SIGSEGV" build/cascade_binop_reject.err; then echo "cascade-binop-reject-probe: FAIL (invalid IR/crash reached instead of a clean rejection)"; cat build/cascade_binop_reject.err; exit 1; fi; \
	if ! grep -q "Undefined variable 'undefinedFn'" build/cascade_binop_reject.err; then echo "cascade-binop-reject-probe: FAIL (wrong/missing diagnostic for the real error)"; cat build/cascade_binop_reject.err; exit 1; fi; \
	errcount=$$(grep -c 'error' build/cascade_binop_reject.err); \
	if [ $$errcount -ne 1 ]; then echo "cascade-binop-reject-probe: FAIL (cascade regressed — $$errcount error lines, expected exactly 1)"; cat build/cascade_binop_reject.err; exit 1; fi; \
	echo "cascade-binop-reject-probe: PASS (rejected rc=$$rc, exactly 1 error line)"

# decl-surface breadth task 1: `var a, b int = 1` — an arity-mismatched
# initializer (2 names, 1 value) on the multi-name var-decl form. The
# no-initializer form (`var a, b int`) shipped this task; the initializer
# form (`var a, b int = 1, 2`) did not (needs codegen changes to walk a
# values chain in lockstep with names — out of this task's file set, see
# the task-1 report). Consequently this shape is GRAMMAR-rejected (a parse
# error, mirroring the parser.y explicit-production arity: the 2-name
# production has no `= expr` continuation at all), not checker-rejected —
# unlike cascade-reject-probe above, which asserts a checker diagnostic.
# Go itself rejects this with "assignment mismatch: 2 variables but 1
# value" (see the task-1 report's go build comparison); either rejection
# reason satisfies the must-not-compile bar the brief sets.
multivar-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== multivar-reject-probe: var a, b int = 1 (arity mismatch) must reject ==="
	@rm -f build/multivar_reject
	@$(COMPILER) -o build/multivar_reject examples/multivar_reject.goo > build/multivar_reject.out 2> build/multivar_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "multivar-reject-probe: FAIL (compiled rc=0 — arity mismatch silently accepted)"; exit 1; fi; \
	if [ -x build/multivar_reject ]; then echo "multivar-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR|Segmentation|SIGSEGV" build/multivar_reject.err; then echo "multivar-reject-probe: FAIL (invalid IR/crash reached instead of a clean rejection)"; cat build/multivar_reject.err; exit 1; fi; \
	if ! grep -qi "syntax error" build/multivar_reject.err; then echo "multivar-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/multivar_reject.err; exit 1; fi; \
	echo "multivar-reject-probe: PASS (rejected rc=$$rc)"

# Task 2 (variadic ...T params): a variadic parameter must be the FINAL
# parameter (Go: "can only use ... with final parameter in list"). Guards
# declare_function_signature's must-be-last check, which fires at signature-
# build time (before the body is even checked) so `func f(a ...int, b int)`
# gets a clean, specific rejection instead of a confusing downstream error
# from treating `b` as part of the (already-consumed) variadic slot.
variadic-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== variadic-reject-probe: non-final variadic parameter must reject ==="
	@rm -f build/variadic_reject
	@$(COMPILER) -o build/variadic_reject examples/variadic_reject.goo > build/variadic_reject.out 2> build/variadic_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "variadic-reject-probe: FAIL (compiled rc=0 — non-final variadic silently accepted)"; exit 1; fi; \
	if [ -x build/variadic_reject ]; then echo "variadic-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR|Segmentation|SIGSEGV" build/variadic_reject.err; then echo "variadic-reject-probe: FAIL (invalid IR/crash reached instead of a clean rejection)"; cat build/variadic_reject.err; exit 1; fi; \
	if ! grep -q "variadic parameter must be the final parameter" build/variadic_reject.err; then echo "variadic-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/variadic_reject.err; exit 1; fi; \
	echo "variadic-reject-probe: PASS (rejected rc=$$rc)"

# Task 2 (variadic ...T params): the #102 narrow-integer range-check net must
# fire for a variadic ELEMENT type at the call-site PACK, not just for an
# ordinary fixed parameter. `small(300)` into `func small(bs ...int8)` routes
# through the same is_untyped_int_rooted/adapt_untyped_int_operand path a
# non-variadic int8 param already uses (expression_checker.c's call-arg
# loop, keyed off the variadic param's element type once the fixed prefix is
# exhausted) — this is what proves that reuse actually happened instead of
# the pack site silently truncating 300 to whatever int8 that bit pattern
# aliases.
variadic-range-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== variadic-range-reject-probe: narrow variadic element overflow must reject ==="
	@rm -f build/variadic_range_reject
	@$(COMPILER) -o build/variadic_range_reject examples/variadic_range_reject.goo > build/variadic_range_reject.out 2> build/variadic_range_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "variadic-range-reject-probe: FAIL (compiled rc=0 — 300 silently accepted for a variadic int8 element)"; exit 1; fi; \
	if [ -x build/variadic_range_reject ]; then echo "variadic-range-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR|Segmentation|SIGSEGV" build/variadic_range_reject.err; then echo "variadic-range-reject-probe: FAIL (invalid IR/crash reached instead of a clean rejection)"; cat build/variadic_range_reject.err; exit 1; fi; \
	if ! grep -q "overflows int8" build/variadic_range_reject.err; then echo "variadic-range-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/variadic_range_reject.err; exit 1; fi; \
	echo "variadic-range-reject-probe: PASS (rejected rc=$$rc)"

# math/bits Div panics on divide-by-zero (y==0) and overflow (y<=hi). Guards
# that both taken panics exit 2 (Go-conformant per Task 6; GOO_PANIC_ABORT=1
# restores the old abort()/134 for debugging) with the runtime-error message
# (the non-panic paths are in bits_div_probe).
bits-div-abort-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== bits-div-abort-probe: Div y==0 / y<=hi abort ==="
	@printf 'package main\nimport "bits"\nfunc main(){ bits.Div64(0, 10, 0) }\n' > build/div_dz.goo
	@printf 'package main\nimport "bits"\nfunc main(){ bits.Div64(5, 0, 3) }\n' > build/div_of.goo
	@$(COMPILER) -o build/div_dz build/div_dz.goo >/dev/null 2>build/div_dz.cerr || (echo "bits-div-abort-probe: FAIL (dz did not compile)"; cat build/div_dz.cerr; exit 1)
	@$(COMPILER) -o build/div_of build/div_of.goo >/dev/null 2>build/div_of.cerr || (echo "bits-div-abort-probe: FAIL (of did not compile)"; cat build/div_of.cerr; exit 1)
	@./build/div_dz >/dev/null 2>build/div_dz.err; rc=$$?; \
	if [ $$rc -ne 2 ]; then echo "bits-div-abort-probe: FAIL (divide-by-zero exit $$rc, want 2)"; exit 1; fi; \
	if ! grep -qiE "integer divide by zero" build/div_dz.err; then echo "bits-div-abort-probe: FAIL (no divide-by-zero message)"; cat build/div_dz.err; exit 1; fi
	@./build/div_of >/dev/null 2>build/div_of.err; rc=$$?; \
	if [ $$rc -ne 2 ]; then echo "bits-div-abort-probe: FAIL (overflow exit $$rc, want 2)"; exit 1; fi; \
	if ! grep -qiE "integer overflow" build/div_of.err; then echo "bits-div-abort-probe: FAIL (no overflow message)"; cat build/div_of.err; exit 1; fi
	@echo "bits-div-abort-probe: PASS"

# panic(v) builtin: a taken panic must exit 2 (Go-conformant per Task 6;
# GOO_PANIC_ABORT=1 restores the old abort()/134 for debugging) and print
# "panic: <msg>" to stderr. Guards the runtime behavior that panic_probe
# (untaken branch) cannot.
panic-abort-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== panic-abort-probe: a taken panic aborts with a message ==="
	@printf 'package main\nfunc main(){ panic("boom") }\n' > build/panic_abort.goo
	@$(COMPILER) -o build/panic_abort build/panic_abort.goo >/dev/null 2>build/panic_abort.cerr || (echo "panic-abort-probe: FAIL (did not compile)"; cat build/panic_abort.cerr; exit 1)
	@./build/panic_abort > build/panic_abort.out 2> build/panic_abort.err; rc=$$?; \
	if [ $$rc -ne 2 ]; then echo "panic-abort-probe: FAIL (exit $$rc, want 2)"; exit 1; fi; \
	if ! grep -qiE "panic: boom" build/panic_abort.err; then echo "panic-abort-probe: FAIL (no 'panic: boom' on stderr)"; cat build/panic_abort.err; exit 1; fi; \
	echo "panic-abort-probe: PASS (exit rc=$$rc)"

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

# Task 5 (raw string literals): CR (0x0D) bytes must be STRIPPED from a raw
# string's content per the Go spec (`a\r\nb` -> "a\nb"), so a raw string read
# from a CRLF source file is line-ending-independent. A literal CR byte inside
# a committed .goo golden fixture is risky — git autocrlf, an editor's line-
# ending normalization, or a future `gofmt`-alike could silently rewrite CRLF
# to LF before the compiler ever sees it, quietly defeating the coverage. This
# probe sidesteps that by generating the source with `printf` at test time
# (same technique as hexesc-reject-probe above), which writes the exact byte
# every run instead of depending on a text file surviving untouched.
rawstring-cr-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@printf 'package main\nimport "fmt"\nfunc main() {\n\ts := `a\r\nb`\n\tfmt.Println(len(s))\n\tfmt.Println(int(s[0]))\n\tfmt.Println(int(s[1]))\n\tfmt.Println(int(s[2]))\n}\n' > build/rawstring_cr_probe.goo
	$(COMPILER) -o build/rawstring_cr_probe build/rawstring_cr_probe.goo
	@./build/rawstring_cr_probe > build/rawstring_cr_probe.actual.txt
	@printf '3\n97\n10\n98\n' > build/rawstring_cr_probe.expected.txt
	@if diff -u build/rawstring_cr_probe.expected.txt build/rawstring_cr_probe.actual.txt; then \
	  echo "rawstring-cr-probe: PASS (CR stripped: len 3, bytes 97/10/98 = 'a','\\n','b')"; \
	else \
	  echo "rawstring-cr-probe: FAIL (see diff above)"; \
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
#  2. `string(byte(66))` NOW CONVERTS (Task 2, port unblocker): Go-conformant
#     rune/byte->string conversion shipped, superseding the old "string/bool
#     both cleanly rejected" assumption this sub-check used to encode. `bool`
#     stays unsupported and is still exercised as the clean-rejection
#     exemplar below (single conversion-specific diagnostic, "cannot convert
#     ... only numeric conversions are supported in v1", NOT the misleading
#     "Undefined variable 'bool'" cascade, never invalid IR reaching the LLVM
#     verifier).
#  3. No over-rejection: a plain numeric conversion still compiles + runs.
conv-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== conv-reject-probe: T(x) shadowing soundness + clean rejection ==="
	@printf 'package main\nimport "fmt"\nfunc int(n int) int { return n + 100 }\nfunc main(){ fmt.Println(int(5)) }\n' > build/cr_shadow.goo
	@printf 'package main\nimport "fmt"\nfunc main(){ fmt.Println(string(byte(66))) }\n' > build/cr_string.goo
	@printf 'package main\nfunc main(){ _ = bool(1) }\n' > build/cr_bool.goo
	@printf 'package main\nimport "fmt"\nfunc main(){ fmt.Println(int(byte(200))) }\n' > build/cr_numok.goo
	@"$(COMPILER)" build/cr_shadow.goo -o build/cr_shadow.out 2>build/cr_shadow.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "conv-reject-probe: FAIL (user 'func int' + int(5) wrongly rejected)"; cat build/cr_shadow.err; exit 1; fi; \
	  out="$$(./build/cr_shadow.out)"; if [ "$$out" != "105" ]; then echo "conv-reject-probe: FAIL (shadowing soundness: int(5) printed '$$out' != 105 — gate regressed to name-only, converting instead of calling the user func)"; exit 1; fi
	@"$(COMPILER)" build/cr_string.goo -o build/cr_string.out 2>build/cr_string.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "conv-reject-probe: FAIL (string(byte(66)) wrongly rejected — Task 2 shipped rune/byte->string conversion)"; cat build/cr_string.err; exit 1; fi; \
	  out="$$(./build/cr_string.out)"; if [ "$$out" != "B" ]; then echo "conv-reject-probe: FAIL (string(byte(66)) printed '$$out' != B)"; exit 1; fi
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

# Unbuffered fan-in lost-wakeup regression (2026-07-10 review finding): the
# not_full condvar serves two sender wait-predicates; a single signal could
# strand a slot-waiter forever. Fixed by broadcasting not_full; this stress
# reproduces the pre-fix failure within the first batches.
fanin-stress: $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== fanin-stress: multi-sender unbuffered fan-in has no lost wakeups ==="
	$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/fanin_stress.c $(RUNTIME_LIB) -lpthread -lm -o build/fanin_stress
	@timeout 120 ./build/fanin_stress; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "fanin-stress: PASS"; else echo "fanin-stress: FAIL (exit $$rc)"; exit 1; fi

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
	@echo "=== deadlock-goroutine-probe: blocked goroutine abandoned at main exit (exit 0, Go parity — P3.3) ==="
	$(COMPILER) -o build/deadlock_goroutine_probe examples/deadlock_goroutine_probe.goo
	@timeout 10 ./build/deadlock_goroutine_probe >build/deadlock_goroutine_probe.out 2>build/deadlock_goroutine_probe.err; rc=$$?; \
	if [ $$rc -eq 124 ]; then echo "deadlock-goroutine-probe: FAIL (hang — main-exit abandonment broken)"; cat build/deadlock_goroutine_probe.err; exit 1; fi; \
	if [ $$rc -ne 0 ]; then echo "deadlock-goroutine-probe: FAIL (exit $$rc, expected 0)"; cat build/deadlock_goroutine_probe.err; exit 1; fi; \
	if [ -s build/deadlock_goroutine_probe.out ]; then echo "deadlock-goroutine-probe: FAIL (abandoned goroutine produced output)"; cat build/deadlock_goroutine_probe.out; exit 1; fi; \
	echo "deadlock-goroutine-probe: PASS"

# P0-4: a failed link must not leave a stray object file behind.
link-cleanup-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== link-cleanup-probe: failed link leaves no .o ==="
	@printf 'package main\nfunc main() {}\n' > build/cleanup_probe.goo
	@rm -f build/cleanup_probe.out.o
	@GOO_RUNTIME=/nonexistent/libgoo_runtime.a "$(COMPILER)" build/cleanup_probe.goo -o build/cleanup_probe.out 2>/dev/null; true
	@if [ -e build/cleanup_probe.out.o ]; then echo "link-cleanup-probe: FAIL (.o left behind)"; exit 1; else echo "link-cleanup-probe: PASS"; fi

# P3.10: -O2 must actually run optimization passes (IR differs from -O0)
# and must not change program behavior (O0 and O2 binaries produce
# identical output) — the differential correctness gate for `make
# verify-core`'s optimizer coverage.
.PHONY: opt-differs-probe
opt-differs-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build build/opt_o0 build/opt_o2
	@echo "=== opt-differs-probe: -O2 IR differs from -O0, same runtime output (P3.10) ==="
	@printf 'package main\n\nimport "fmt"\n\nfunc sum(n int) int {\n\ttotal := 0\n\tfor i := 0; i < n; i++ {\n\t\ttotal = total + i*2 - 1\n\t}\n\treturn total\n}\n\nfunc fib(n int) int {\n\tif n < 2 {\n\t\treturn n\n\t}\n\ta := 0\n\tb := 1\n\tfor i := 2; i <= n; i++ {\n\t\tc := a + b\n\t\ta = b\n\t\tb = c\n\t}\n\treturn b\n}\n\nfunc main() {\n\tx := sum(100)\n\ty := fib(20)\n\ttotal := 0\n\tfor i := 0; i < 50; i++ {\n\t\ttotal = total + x*i - y\n\t}\n\tfmt.Println(x, y, total)\n}\n' > build/opt_probe.goo
	@# P5.2 made --emit-llvm IR-only, so IR and binary are separate compiles.
	@"$(COMPILER)" --emit-llvm -O0 build/opt_probe.goo -o build/opt_o0/opt_probe.ll >build/opt_probe_o0.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "opt-differs-probe: FAIL (O0 IR compile failed)"; cat build/opt_probe_o0.log; exit 1; fi
	@"$(COMPILER)" --emit-llvm -O2 build/opt_probe.goo -o build/opt_o2/opt_probe.ll >build/opt_probe_o2.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "opt-differs-probe: FAIL (O2 IR compile failed)"; cat build/opt_probe_o2.log; exit 1; fi
	@"$(COMPILER)" -O0 build/opt_probe.goo -o build/opt_o0/opt_probe >>build/opt_probe_o0.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "opt-differs-probe: FAIL (O0 binary compile failed)"; cat build/opt_probe_o0.log; exit 1; fi
	@"$(COMPILER)" -O2 build/opt_probe.goo -o build/opt_o2/opt_probe >>build/opt_probe_o2.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "opt-differs-probe: FAIL (O2 binary compile failed)"; cat build/opt_probe_o2.log; exit 1; fi
	@if cmp -s build/opt_o0/opt_probe.ll build/opt_o2/opt_probe.ll; then \
	  echo "opt-differs-probe: FAIL (O2 IR identical to O0 — optimizer not running)"; exit 1; \
	fi
	@./build/opt_o0/opt_probe > build/opt_probe_o0.out; \
	  ./build/opt_o2/opt_probe > build/opt_probe_o2.out; \
	  if ! diff -u build/opt_probe_o0.out build/opt_probe_o2.out >/dev/null; then \
	    echo "opt-differs-probe: FAIL (O0/O2 output mismatch — miscompile under optimization)"; \
	    diff -u build/opt_probe_o0.out build/opt_probe_o2.out; exit 1; \
	  fi
	@echo "opt-differs-probe: PASS"

# P3.11: an output path containing a space must link and run correctly.
# Pre-fork/execvp, system(link_command) shelled the whole command through
# /bin/sh, which word-splits unquoted paths — this probe FAILS against that
# code (confirm failing-first before implementing the fork/execvp rewrite).
.PHONY: link-spaces-probe
link-spaces-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p 'build/dir with space'
	@echo "=== link-spaces-probe: output path containing a space links and runs (P3.11) ==="
	@printf 'package main\nimport "fmt"\nfunc main() { fmt.Println("hello") }\n' > build/link_spaces_probe.goo
	@"$(COMPILER)" build/link_spaces_probe.goo -o 'build/dir with space/hello_probe' >build/link_spaces_probe.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "link-spaces-probe: FAIL (compile/link failed)"; cat build/link_spaces_probe.log; exit 1; fi
	@out="$$('build/dir with space/hello_probe')"; \
	  if [ "$$out" != "hello" ]; then echo "link-spaces-probe: FAIL (got '$$out', want 'hello')"; exit 1; fi; \
	  echo "link-spaces-probe: PASS"

# P3.11: the -l/--link CLI flag (goo.c's `-l` getopt case, options->link_libs)
# must reach the link line without breaking it — proves the fork/execvp argv
# construction correctly appends user libs after the runtime archive and
# before -lm/-lpthread.
.PHONY: link-libs-probe
link-libs-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== link-libs-probe: -l flag reaches the link line without breaking it (P3.11) ==="
	@printf 'package main\nimport "fmt"\nfunc main() { fmt.Println("linked") }\n' > build/link_libs_probe.goo
	@"$(COMPILER)" build/link_libs_probe.goo -l m -o build/link_libs_probe.out >build/link_libs_probe.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "link-libs-probe: FAIL (compile/link failed with -l m)"; cat build/link_libs_probe.log; exit 1; fi
	@out="$$(./build/link_libs_probe.out)"; \
	  if [ "$$out" != "linked" ]; then echo "link-libs-probe: FAIL (got '$$out', want 'linked')"; exit 1; fi
	@# Negative case is the discriminating half: -lm is unconditionally
	@# appended by the linker argv construction, so the positive case above
	@# would pass even if -l threading were silently dropped. A bogus
	@# library name MUST fail the link, and the echoed failing link command
	@# (codegen_error's "Linking failed with command: ..." on stderr) must
	@# still show -ltotallybogus_xyz — proving the flag actually reached
	@# argv rather than being swallowed. A dropped flag would let the link
	@# succeed and fail this half of the probe instead.
	@printf 'package main\nimport "fmt"\nfunc main() { fmt.Println("nope") }\n' > build/link_libs_bogus.goo
	@rm -f build/link_libs_bogus.out build/link_libs_bogus.out.o
	@"$(COMPILER)" build/link_libs_bogus.goo -l totallybogus_xyz -o build/link_libs_bogus.out >build/link_libs_bogus.log 2>build/link_libs_bogus.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "link-libs-probe: FAIL (bogus -l linked fine — flag not reaching linker argv)"; exit 1; fi; \
	  if ! grep -q -- "-ltotallybogus_xyz" build/link_libs_bogus.err; then echo "link-libs-probe: FAIL (echoed link command missing -ltotallybogus_xyz)"; cat build/link_libs_bogus.err; exit 1; fi; \
	  if [ -e build/link_libs_bogus.out.o ]; then echo "link-libs-probe: FAIL (failed link left stray .o behind)"; exit 1; fi; \
	  echo "link-libs-probe: PASS (positive + negative)"

# P5.1: `goo -r` must propagate the child program's exit code as goo's own
# exit code. Pre-fix, compile_file ran the program via system(), discarded
# the status, and returned success — `goo -r` exited 0 no matter what the
# program did (and the unconditional "./" prefix + shell parsing made -o
# paths fragile). This probe FAILS against that driver — failing-first.
.PHONY: run-exit-probe
run-exit-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build/run_exit_subdir
	@echo "=== run-exit-probe: goo -r propagates the program's exit code (P5.1) ==="
	@printf 'package main\nimport "os"\nfunc main() { os.Exit(7) }\n' > build/run_exit_probe.goo
	@"$(COMPILER)" -r build/run_exit_probe.goo -o build/run_exit_subdir/run_exit_probe >build/run_exit_probe.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 7 ]; then echo "run-exit-probe: FAIL (goo -r exited $$rc, want 7 — child exit code not propagated)"; cat build/run_exit_probe.log; exit 1; fi
	@printf 'package main\nimport "fmt"\nfunc main() { fmt.Println("ran ok") }\n' > build/run_exit_zero.goo
	@"$(COMPILER)" -r build/run_exit_zero.goo -o build/run_exit_zero_probe >build/run_exit_zero.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "run-exit-probe: FAIL (successful run exited $$rc, want 0)"; cat build/run_exit_zero.log; exit 1; fi
	@# absolute -o path must compile AND run (no unconditional "./" prefix)
	@printf 'package main\nimport "os"\nfunc main() { os.Exit(3) }\n' > build/run_exit_abs.goo
	@"$(COMPILER)" -r build/run_exit_abs.goo -o "$(CURDIR)/build/run_exit_abs_probe" >build/run_exit_abs.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 3 ]; then echo "run-exit-probe: FAIL (absolute -o: goo -r exited $$rc, want 3)"; cat build/run_exit_abs.log; exit 1; fi
	@# a compile error under -r must still exit exactly 1 (never a run code)
	@printf 'package main\nfunc main() { this is not goo }\n' > build/run_exit_bad.goo
	@"$(COMPILER)" -r build/run_exit_bad.goo -o build/run_exit_bad_probe >build/run_exit_bad.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 1 ]; then echo "run-exit-probe: FAIL (compile error under -r exited $$rc, want 1)"; exit 1; fi; \
	  echo "run-exit-probe: PASS (nonzero + zero propagation, absolute -o, compile-error=1)"

# P5.2: --emit-llvm must emit IR ONLY, at exactly the -o path. Pre-fix, the
# always-true `if (!emit_llvm_ir || emit_llvm_ir)` in compile_file wrote the
# ELF executable to the -o path and the IR to <path>.ll — so `-o out.ll`
# produced an ELF named out.ll plus an out.ll.ll. Without -o the default
# is <input-stem>.ll (executable default <input-stem>.out is unchanged).
# This probe FAILS against the pre-fix driver — failing-first.
.PHONY: emit-llvm-probe
emit-llvm-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== emit-llvm-probe: --emit-llvm emits IR only, correct naming (P5.2) ==="
	@printf 'package main\nimport "fmt"\nfunc main() { fmt.Println("ir") }\n' > build/emit_llvm_probe.goo
	@rm -f build/emit_llvm_probe.ll build/emit_llvm_probe.ll.ll
	@"$(COMPILER)" --emit-llvm build/emit_llvm_probe.goo -o build/emit_llvm_probe.ll >build/emit_llvm_probe.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "emit-llvm-probe: FAIL (compile failed)"; cat build/emit_llvm_probe.log; exit 1; fi
	@if ! head -1 build/emit_llvm_probe.ll | grep -q '^; ModuleID'; then \
	  echo "emit-llvm-probe: FAIL (-o path is not textual IR — got: $$(head -c 32 build/emit_llvm_probe.ll | LC_ALL=C tr -c '[:print:]' '.'))"; exit 1; fi
	@if [ -e build/emit_llvm_probe.ll.ll ]; then echo "emit-llvm-probe: FAIL (stray .ll.ll written next to -o path)"; exit 1; fi
	@# default naming without -o: IR at <stem>.ll, and no <stem>.out ELF
	@cp build/emit_llvm_probe.goo build/emit_llvm_default.goo
	@rm -f build/emit_llvm_default.ll build/emit_llvm_default.out build/emit_llvm_default.out.ll
	@"$(COMPILER)" --emit-llvm build/emit_llvm_default.goo >build/emit_llvm_default.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "emit-llvm-probe: FAIL (default-name compile failed)"; cat build/emit_llvm_default.log; exit 1; fi
	@if ! head -1 build/emit_llvm_default.ll 2>/dev/null | grep -q '^; ModuleID'; then \
	  echo "emit-llvm-probe: FAIL (default <stem>.ll missing or not IR)"; exit 1; fi
	@if [ -e build/emit_llvm_default.out ] || [ -e build/emit_llvm_default.out.ll ]; then \
	  echo "emit-llvm-probe: FAIL (--emit-llvm still produced executable-path artifacts)"; exit 1; fi
	@echo "emit-llvm-probe: PASS (IR only, exact -o naming, <stem>.ll default)"

# Go spec conformance suite (tests/spec/ + manifest.tsv): one fixture per
# spec construct, manifest records mode (run|reject) + honest status
# (works|divergent|rejected|absent). The runner is a DRIFT GATE — behavior
# changing in either direction fails until the matrix/doc are updated.
# Human-readable report: docs/GO_SPEC_CONFORMANCE.md.
.PHONY: spec-conformance
spec-conformance: $(COMPILER) $(RUNTIME_LIB)
	@echo "=== spec-conformance: Go spec construct matrix (tests/spec) ==="
	@COMPILER="$(COMPILER)" bash scripts/run_spec_conformance.sh

# P5 rider (2026-07-11): gpu_kernel must be a HARD COMPILE REJECT in v1.
# The GPU grammar arms (kernel_decl/kernel_launch) are dead — TOKEN_KERNEL is
# not mapped by lexer_bridge.c, so `gpu_kernel` reaches the parser as an
# identifier and fails to parse. That reject is the honest v1 surface (no
# fabricated GPU output); this probe PINS it so a future grammar change
# cannot half-revive GPU syntax without tripping a gate. Real GPU support is
# post-v1 (lanes-then-GPU phasing, docs/2026-07-08-v1-roadmap.md).
.PHONY: gpu-kernel-reject-probe
gpu-kernel-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== gpu-kernel-reject-probe: gpu_kernel is a clean compile reject (P5 rider) ==="
	@printf 'package main\n\ngpu_kernel add(n int) {\n\tn = n + 1\n}\n\nfunc main() {\n}\n' > build/gpu_kernel_reject.goo
	@rm -f build/gpu_kernel_reject_bin
	@"$(COMPILER)" build/gpu_kernel_reject.goo -o build/gpu_kernel_reject_bin >build/gpu_kernel_reject.stdout 2>build/gpu_kernel_reject.stderr; rc=$$?; \
	  if [ $$rc -ne 1 ]; then echo "gpu-kernel-reject-probe: FAIL (exit $$rc, want 1)"; exit 1; fi; \
	  if [ -e build/gpu_kernel_reject_bin ]; then echo "gpu-kernel-reject-probe: FAIL (binary produced for gpu_kernel source)"; exit 1; fi; \
	  if [ -s build/gpu_kernel_reject.stdout ]; then echo "gpu-kernel-reject-probe: FAIL (error text on stdout)"; exit 1; fi; \
	  if ! grep -q "error" build/gpu_kernel_reject.stderr; then echo "gpu-kernel-reject-probe: FAIL (no error text on stderr)"; cat build/gpu_kernel_reject.stderr; exit 1; fi; \
	  echo "gpu-kernel-reject-probe: PASS (exit 1, no binary, error on stderr)"

# P5.3: `goo build` / `goo run` / `goo help` subcommands. build = Go parity
# (executable named <stem> in the cwd); run = compile to a temp binary, exec
# it forwarding args after `--`, propagate its exit code, clean up the temp;
# legacy flag-form invocations stay byte-compatible. Replaces the deleted
# tools/goo facade (its builtin_build returned 0 without compiling anything).
# This probe FAILS against the pre-subcommand driver — failing-first.
.PHONY: subcommand-probe
subcommand-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build/subcmd
	@echo "=== subcommand-probe: goo build/run/help subcommands (P5.3) ==="
	@# help: exits 0 and prints usage
	@"$(COMPILER)" help >build/subcmd/help.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ] || ! grep -q "Usage:" build/subcmd/help.log; then \
	    echo "subcommand-probe: FAIL (goo help: rc=$$rc or no Usage text)"; cat build/subcmd/help.log; exit 1; fi
	@# build: Go parity — executable named <stem> in the cwd
	@printf 'package main\nimport "fmt"\nfunc main() { fmt.Println("built") }\n' > build/subcmd/sub_build.goo
	@rm -f build/subcmd/sub_build build/subcmd/sub_build.out
	@cd build/subcmd && "$(CURDIR)/$(COMPILER)" build sub_build.goo >build.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "subcommand-probe: FAIL (goo build failed)"; cat build/subcmd/build.log; exit 1; fi
	@if [ ! -x build/subcmd/sub_build ]; then echo "subcommand-probe: FAIL (goo build did not produce ./sub_build)"; exit 1; fi
	@if [ -e build/subcmd/sub_build.out ]; then echo "subcommand-probe: FAIL (goo build produced legacy .out name too)"; exit 1; fi
	@out="$$(./build/subcmd/sub_build)"; \
	  if [ "$$out" != "built" ]; then echo "subcommand-probe: FAIL (built binary printed '$$out')"; exit 1; fi
	@# run: forwards args after -- and prints via os.Args
	@printf 'package main\nimport "fmt"\nimport "os"\nfunc main() {\n\tfmt.Println(len(os.Args) >= 1)\n\tif len(os.Args) > 1 {\n\t\tfmt.Println(len(os.Args))\n\t\tfmt.Println(os.Args[1])\n\t}\n}\n' > build/subcmd/sub_args.goo
	@out="$$("$(COMPILER)" run build/subcmd/sub_args.goo -- alpha beta 2>build/subcmd/run_args.err)"; rc=$$?; \
	  if [ $$rc -ne 0 ] || [ "$$out" != "$$(printf 'true\n3\nalpha')" ]; then \
	    echo "subcommand-probe: FAIL (goo run arg forwarding: rc=$$rc, out='$$out')"; cat build/subcmd/run_args.err; exit 1; fi
	@# run: exit-code propagation
	@printf 'package main\nimport "os"\nfunc main() { os.Exit(5) }\n' > build/subcmd/sub_exit.goo
	@"$(COMPILER)" run build/subcmd/sub_exit.goo >build/subcmd/run_exit.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 5 ]; then echo "subcommand-probe: FAIL (goo run exited $$rc, want 5)"; cat build/subcmd/run_exit.log; exit 1; fi
	@# run: no binary left behind in cwd or next to the source
	@if ls build/subcmd/sub_exit 2>/dev/null || ls build/subcmd/sub_exit.out 2>/dev/null; then \
	  echo "subcommand-probe: FAIL (goo run left a binary behind)"; exit 1; fi
	@# run: compile error still exits exactly 1
	@printf 'package main\nfunc main() { not goo at all }\n' > build/subcmd/sub_bad.goo
	@"$(COMPILER)" run build/subcmd/sub_bad.goo >build/subcmd/run_bad.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 1 ]; then echo "subcommand-probe: FAIL (run compile error exited $$rc, want 1)"; exit 1; fi
	@# legacy flag form stays byte-compatible
	@printf 'package main\nimport "fmt"\nfunc main() { fmt.Println("legacy") }\n' > build/subcmd/sub_legacy.goo
	@"$(COMPILER)" build/subcmd/sub_legacy.goo -o build/subcmd/sub_legacy_probe >build/subcmd/legacy.log 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "subcommand-probe: FAIL (legacy flag form broke)"; cat build/subcmd/legacy.log; exit 1; fi; \
	  out="$$(./build/subcmd/sub_legacy_probe)"; \
	  if [ "$$out" != "legacy" ]; then echo "subcommand-probe: FAIL (legacy binary printed '$$out')"; exit 1; fi; \
	  echo "subcommand-probe: PASS (help, build Go-parity naming, run args+exit+cleanup, legacy intact)"

# P0-3: a run of blank lines must NOT overflow the stack. The newline ASI
# handler now iterates instead of tail-recursing, so 1,000,000 consecutive
# blank lines lex without a SIGSEGV. The fixture is generated at test time
# (1MB+), never committed. Guards against a regression back to recursion.
.PHONY: blank-lines-probe comment-lines-probe
blank-lines-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== blank-lines-probe: 1e6 blank lines must not crash the lexer ==="
	@{ yes '' | head -n 1000000; printf 'package main\nfunc main() {}\n'; } > build/blank_lines_probe.goo
	@"$(COMPILER)" build/blank_lines_probe.goo -o build/blank_lines_probe.out 2>build/blank_lines_probe.err; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "blank-lines-probe: FAIL (compile rc=$$rc — stack overflow regression?)"; cat build/blank_lines_probe.err; exit 1; fi; \
	./build/blank_lines_probe.out; rrc=$$?; \
	if [ $$rrc -ne 0 ]; then echo "blank-lines-probe: FAIL (run rc=$$rrc)"; exit 1; fi; \
	echo "blank-lines-probe: PASS"

# P0-5: a run of consecutive line comments must NOT overflow the stack. The
# `//` and `/* */` comment-skip paths now iterate (continue the scan loop)
# instead of tail-recursing, so 400,000 consecutive comment lines lex without
# a SIGSEGV. The fixture is generated at test time, never committed. Guards
# against a regression back to recursion (sibling of blank-lines-probe).
comment-lines-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== comment-lines-probe: 4e5 comment lines must not crash the lexer ==="
	@{ printf 'package main\nfunc main() {\n'; yes '// c' | head -n 400000; printf '}\n'; } > build/comment_lines_probe.goo
	@"$(COMPILER)" build/comment_lines_probe.goo -o build/comment_lines_probe.out 2>build/comment_lines_probe.err; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "comment-lines-probe: FAIL (compile rc=$$rc — stack overflow regression?)"; cat build/comment_lines_probe.err; exit 1; fi; \
	./build/comment_lines_probe.out; rrc=$$?; \
	if [ $$rrc -ne 0 ]; then echo "comment-lines-probe: FAIL (run rc=$$rrc)"; exit 1; fi; \
	echo "comment-lines-probe: PASS"

# Blank identifier `_` is a discard, never a binding: it may repeat across `:=`
# in one scope, but it can NEVER be read back as a value (Go: "cannot use _ as
# value"). This probe pins the reject side — reading `_` must be a compile error.
blank-read-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== blank-read-reject-probe: reading _ as a value must reject ==="
	@printf 'package main\nimport "fmt"\nfunc main(){\n\t_, a := 1, 2\n\tfmt.Println(_, a)\n}\n' > build/blank_read.goo
	@if $(COMPILER) -o build/blank_read build/blank_read.goo 2>build/blank_read.err; then \
	  echo "blank-read-reject-probe: FAIL (reading _ compiled)"; exit 1; \
	else echo "blank-read-reject-probe: PASS"; fi

# Index-bounds-checking Task 1: a slice-index WRITE (s[i]=x) out of range must
# abort (non-zero exit + "bounds check failed"), not write past the backing
# buffer. Covers both the too-large index and the negative-index case.
slice-write-bounds-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== slice-write-bounds-probe: s[5]=x and s[-1]=x on len-3 slice must abort ==="
	@printf 'package main\nfunc main(){ s:=[]int{1,2,3}; s[5]=9; _=s }\n' > build/swb_oob.goo
	@"$(COMPILER)" build/swb_oob.goo -o build/swb_oob.out 2>build/swb_oob.cerr || \
	  { echo "slice-write-bounds-probe: FAIL (compile)"; cat build/swb_oob.cerr; exit 1; }
	@./build/swb_oob.out 2>build/swb_oob.err; rc=$$?; \
	  if [ $$rc -ne 2 ]; then echo "slice-write-bounds-probe: FAIL (OOB write expected panic exit 2, got rc=$$rc)"; exit 1; fi; \
	  if ! grep -qi "bounds check failed" build/swb_oob.err; then echo "slice-write-bounds-probe: FAIL (no bounds message)"; cat build/swb_oob.err; exit 1; fi
	@printf 'package main\nfunc main(){ s:=[]int{1,2,3}; i:=-1; s[i]=9; _=s }\n' > build/swb_neg.goo
	@"$(COMPILER)" build/swb_neg.goo -o build/swb_neg.out 2>build/swb_neg.cerr || \
	  { echo "slice-write-bounds-probe: FAIL (compile neg)"; cat build/swb_neg.cerr; exit 1; }
	@./build/swb_neg.out 2>build/swb_neg.err; rc=$$?; \
	  if [ $$rc -ne 2 ]; then echo "slice-write-bounds-probe: FAIL (negative-index write expected panic exit 2, got rc=$$rc)"; exit 1; fi; \
	  if ! grep -qi "bounds check failed" build/swb_neg.err; then echo "slice-write-bounds-probe: FAIL (no bounds message on neg)"; cat build/swb_neg.err; exit 1; fi
	@echo "slice-write-bounds-probe: PASS"

# Index-bounds-checking Task 2: array-index READ and WRITE (arr[i], arr[i]=x)
# out of range must abort (non-zero exit + "bounds check failed"), not access
# past the fixed array. Uses a VARIABLE index — a constant OOB array index is
# a Go compile error and would not exercise the runtime check.
array-bounds-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== array-bounds-probe: arr[i]=x and _=arr[i] with i out of range must abort (variable index) ==="
	@printf 'package main\nfunc main(){ var arr [3]int; i:=5; arr[i]=9; _=arr }\n' > build/awb_oob.goo
	@"$(COMPILER)" build/awb_oob.goo -o build/awb_oob.out 2>build/awb_oob.cerr || \
	  { echo "array-bounds-probe: FAIL (compile write)"; cat build/awb_oob.cerr; exit 1; }
	@./build/awb_oob.out 2>build/awb_oob.err; rc=$$?; \
	  if [ $$rc -ne 2 ]; then echo "array-bounds-probe: FAIL (OOB array write expected panic exit 2, got rc=$$rc)"; exit 1; fi; \
	  if ! grep -qi "bounds check failed" build/awb_oob.err; then echo "array-bounds-probe: FAIL (no bounds message on write)"; cat build/awb_oob.err; exit 1; fi
	@printf 'package main\nimport "fmt"\nfunc main(){ var arr [3]int; i:=5; fmt.Println(arr[i]) }\n' > build/arb_oob.goo
	@"$(COMPILER)" build/arb_oob.goo -o build/arb_oob.out 2>build/arb_oob.cerr || \
	  { echo "array-bounds-probe: FAIL (compile read)"; cat build/arb_oob.cerr; exit 1; }
	@./build/arb_oob.out 2>build/arb_oob.err; rc=$$?; \
	  if [ $$rc -ne 2 ]; then echo "array-bounds-probe: FAIL (OOB array read expected panic exit 2, got rc=$$rc)"; exit 1; fi; \
	  if ! grep -qi "bounds check failed" build/arb_oob.err; then echo "array-bounds-probe: FAIL (no bounds message on read)"; cat build/arb_oob.err; exit 1; fi
	@echo "array-bounds-probe: PASS"

# F5 follow-up: slice/substring EXPRESSIONS `base[low:high]` out of range must
# abort (non-zero exit + "slice bounds out of range"), not silently alias
# memory past the backing buffer/capacity. Sibling of bounds-probe (which
# covers single-element index reads) and slice-write-bounds-probe (single-
# element writes) — this one covers the two-bound slice-expression form.
# Covers: high>cap, low>high, negative low, and a string (no cap, so max=len).
slice-expr-bounds-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== slice-expr-bounds-probe: out-of-range s[low:high] must abort ==="
	@printf 'package main\nimport "fmt"\nfunc main(){ s:=[]int{1,2,3}; t:=s[1:10]; fmt.Println(len(t)) }\n' > build/sebp_highcap.goo
	@"$(COMPILER)" build/sebp_highcap.goo -o build/sebp_highcap.out 2>build/sebp_highcap.cerr || \
	  { echo "slice-expr-bounds-probe: FAIL (compile high>cap)"; cat build/sebp_highcap.cerr; exit 1; }
	@./build/sebp_highcap.out 2>build/sebp_highcap.err; rc=$$?; \
	  if [ $$rc -ne 2 ]; then echo "slice-expr-bounds-probe: FAIL (high>cap expected panic exit 2, got rc=$$rc)"; exit 1; fi; \
	  if ! grep -qi "slice bounds out of range" build/sebp_highcap.err; then echo "slice-expr-bounds-probe: FAIL (no slice-bounds message on high>cap)"; cat build/sebp_highcap.err; exit 1; fi
	@printf 'package main\nimport "fmt"\nfunc main(){ s:=[]int{1,2,3}; t:=s[3:1]; fmt.Println(len(t)) }\n' > build/sebp_lowhigh.goo
	@"$(COMPILER)" build/sebp_lowhigh.goo -o build/sebp_lowhigh.out 2>build/sebp_lowhigh.cerr || \
	  { echo "slice-expr-bounds-probe: FAIL (compile low>high)"; cat build/sebp_lowhigh.cerr; exit 1; }
	@./build/sebp_lowhigh.out 2>build/sebp_lowhigh.err; rc=$$?; \
	  if [ $$rc -ne 2 ]; then echo "slice-expr-bounds-probe: FAIL (low>high expected panic exit 2, got rc=$$rc)"; exit 1; fi; \
	  if ! grep -qi "slice bounds out of range" build/sebp_lowhigh.err; then echo "slice-expr-bounds-probe: FAIL (no slice-bounds message on low>high)"; cat build/sebp_lowhigh.err; exit 1; fi
	@printf 'package main\nimport "fmt"\nfunc main(){ s:=[]int{1,2,3}; i:=-1; t:=s[i:]; fmt.Println(len(t)) }\n' > build/sebp_neg.goo
	@"$(COMPILER)" build/sebp_neg.goo -o build/sebp_neg.out 2>build/sebp_neg.cerr || \
	  { echo "slice-expr-bounds-probe: FAIL (compile negative low)"; cat build/sebp_neg.cerr; exit 1; }
	@./build/sebp_neg.out 2>build/sebp_neg.err; rc=$$?; \
	  if [ $$rc -ne 2 ]; then echo "slice-expr-bounds-probe: FAIL (negative low expected panic exit 2, got rc=$$rc)"; exit 1; fi; \
	  if ! grep -qi "slice bounds out of range" build/sebp_neg.err; then echo "slice-expr-bounds-probe: FAIL (no slice-bounds message on negative low)"; cat build/sebp_neg.err; exit 1; fi
	@printf 'package main\nimport "fmt"\nfunc main(){ str:="hello"; sub:=str[0:99]; fmt.Println(sub) }\n' > build/sebp_str.goo
	@"$(COMPILER)" build/sebp_str.goo -o build/sebp_str.out 2>build/sebp_str.cerr || \
	  { echo "slice-expr-bounds-probe: FAIL (compile string OOB)"; cat build/sebp_str.cerr; exit 1; }
	@./build/sebp_str.out 2>build/sebp_str.err; rc=$$?; \
	  if [ $$rc -ne 2 ]; then echo "slice-expr-bounds-probe: FAIL (string OOB expected panic exit 2, got rc=$$rc)"; exit 1; fi; \
	  if ! grep -qi "slice bounds out of range" build/sebp_str.err; then echo "slice-expr-bounds-probe: FAIL (no slice-bounds message on string OOB)"; cat build/sebp_str.err; exit 1; fi
	@echo "slice-expr-bounds-probe: PASS"

# fix/const-array-length: an array-type length written as a const identifier
# (`[N]int`) or a const expression (`[N+1]int`) used to silently fall back to
# a fixed placeholder length of 10, regardless of what the const actually
# resolved to — which also made bounds checks meaningless (an OOB index
# against the real N could sail under the placeholder-10 and never trip).
# type_from_ast now folds the length via goo_fold_const_int_ctx (checker-
# aware: resolves const identifiers, recursing through const expressions).
# Sibling of array-bounds-probe (which covers a literal-length array with a
# variable index) — this one pins the LENGTH resolution itself: arr[i]=x
# with i out of range against the real const-resolved length (3, not 10)
# must abort.
const-array-bounds-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== const-array-bounds-probe: const-sized array must bounds-check against its REAL length, not a placeholder ==="
	@printf 'package main\nfunc main(){ const N=3; var arr [N]int; i:=5; arr[i]=9; _=arr }\n' > build/cabp_oob.goo
	@"$(COMPILER)" build/cabp_oob.goo -o build/cabp_oob.out 2>build/cabp_oob.cerr || \
	  { echo "const-array-bounds-probe: FAIL (compile)"; cat build/cabp_oob.cerr; exit 1; }
	@./build/cabp_oob.out 2>build/cabp_oob.err; rc=$$?; \
	  if [ $$rc -ne 2 ]; then echo "const-array-bounds-probe: FAIL (OOB write against const-sized array expected panic exit 2, got rc=$$rc)"; exit 1; fi; \
	  if ! grep -qi "bounds check failed" build/cabp_oob.err; then echo "const-array-bounds-probe: FAIL (no bounds message)"; cat build/cabp_oob.err; exit 1; fi; \
	  if ! grep -q "length 3" build/cabp_oob.err; then echo "const-array-bounds-probe: FAIL (checked against wrong length — placeholder-10 regression?)"; cat build/cabp_oob.err; exit 1; fi
	@printf 'package main\nfunc main(){ var arr [2+3]int; i:=5; arr[i]=9; _=arr }\n' > build/cabp_expr.goo
	@"$(COMPILER)" build/cabp_expr.goo -o build/cabp_expr.out 2>build/cabp_expr.cerr || \
	  { echo "const-array-bounds-probe: FAIL (compile expr-length)"; cat build/cabp_expr.cerr; exit 1; }
	@./build/cabp_expr.out 2>build/cabp_expr.err; rc=$$?; \
	  if [ $$rc -ne 2 ]; then echo "const-array-bounds-probe: FAIL (OOB write against expr-sized [2+3]int expected panic exit 2, got rc=$$rc)"; exit 1; fi; \
	  if ! grep -q "length 5" build/cabp_expr.err; then echo "const-array-bounds-probe: FAIL (expr length not resolved to 5)"; cat build/cabp_expr.err; exit 1; fi
	@echo "const-array-bounds-probe: PASS"

# fix/const-array-length: a genuinely non-constant array length (a plain
# runtime variable, not a const) must be a clean type error — NOT a silent
# fallback to the placeholder length, and not a crash. See
# examples/nonconst_arraylen_reject.goo.
nonconst-arraylen-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== nonconst-arraylen-reject-probe: [n]int with a runtime variable n must be rejected ==="
	@rm -f build/nonconst_arraylen_reject
	@$(COMPILER) -o build/nonconst_arraylen_reject examples/nonconst_arraylen_reject.goo > build/nonconst_arraylen_reject.out 2> build/nonconst_arraylen_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "nonconst-arraylen-reject-probe: FAIL (compiled rc=0 — non-const array length silently accepted)"; exit 1; fi; \
	if [ -x build/nonconst_arraylen_reject ]; then echo "nonconst-arraylen-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "array length must be a constant expression" build/nonconst_arraylen_reject.err; then echo "nonconst-arraylen-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/nonconst_arraylen_reject.err; exit 1; fi; \
	echo "nonconst-arraylen-reject-probe: PASS (rejected rc=$$rc)"

# Comptime-value params: a runtime value to a comptime parameter is a clean
# type error (not invalid IR).
comptime-value-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== comptime-value-reject-probe: runtime arg to comptime param fails cleanly ==="
	@printf 'package main\nfunc fill(comptime n int, s int) int { return s }\nfunc main() { x := 5; _ = fill(x, 1) }\n' > build/cvr.goo
	@"$(COMPILER)" build/cvr.goo -o build/cvr.out 2>build/cvr.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "comptime-value-reject-probe: FAIL (compiled a runtime comptime arg)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/cvr.err; then echo "comptime-value-reject-probe: FAIL (invalid IR reached verifier)"; cat build/cvr.err; exit 1; fi; \
	  if grep -qiE "compile-time constant|comptime parameter" build/cvr.err; then echo "comptime-value-reject-probe: PASS"; else echo "comptime-value-reject-probe: FAIL (no clean diagnostic)"; cat build/cvr.err; exit 1; fi

# Comptime-value params reject matrix (fix round 2, I3): one target sweeping
# EVERY safety wall around comptime-param functions. Each case must FAIL to
# compile, emit its specific diagnostic, and never leak LLVM-verifier noise.
# The single-case comptime-value-reject-probe above stays as-is (it predates
# this matrix and other docs reference it); this is the breadth net. The
# package-runtime-arg case resolves `cpkg` from a throwaway GOOROOT tree under
# build/ (same env contract as import_resolver's goo_gooroot_dir): since the
# P6 M1 wall lift a comptime-CONST arg into a package function COMPILES, so this
# case pins the surviving wall — a RUNTIME arg across the package boundary still
# rejects with "must be a compile-time constant". The package-generic-comptime
# case is a DIFFERENT wall: `cpkg.GenFill[T any](comptime n int, x T) T` called
# cross-package as `cpkg.GenFill(3, 5)` never reaches the comptime machinery at
# all — generic type-parameter inference for a package-qualified call doesn't
# unify `x T` against the second argument, so it fails noisily-but-cleanly with
# a generic-inference diagnostic ("argument 2: cannot use int64 as T") rather
# than a comptime-specific one. Pinned as-observed (captured fresh for this
# case; matches expression_checker.c's "argument %zu: cannot use %s as %s"), not
# as a designed error message — composed generic+comptime across a package
# boundary is simply unimplemented, and this case is the tripwire for it.
comptime-value-reject-matrix: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build/cvm_gooroot/goostd/cpkg
	@printf 'package cpkg\nfunc Fill(comptime n int, s int) int { return s }\nfunc GenFill[T any](comptime n int, x T) T { return x }\n' > build/cvm_gooroot/goostd/cpkg/cpkg.go
	@echo "=== comptime-value-reject-matrix: every comptime-param safety wall rejects cleanly ==="
	@set -e; \
	run_case() { \
	  name="$$1"; pat="$$2"; \
	  rc=0; GOOROOT=build/cvm_gooroot "$(COMPILER)" build/cvm.goo -o build/cvm_bin >build/cvm.err 2>&1 || rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "comptime-value-reject-matrix: FAIL ($$name compiled — wall is down)"; cat build/cvm.err; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/cvm.err; then echo "comptime-value-reject-matrix: FAIL ($$name: invalid IR reached the verifier)"; cat build/cvm.err; exit 1; fi; \
	  if ! grep -qE "$$pat" build/cvm.err; then echo "comptime-value-reject-matrix: FAIL ($$name: missing diagnostic /$$pat/)"; cat build/cvm.err; exit 1; fi; \
	  echo "  PASS $$name"; \
	}; \
	printf 'package main\nfunc fill(comptime n int, s int) int { return s }\nfunc main() { x := 5; _ = fill(x, 1) }\n' > build/cvm.goo; \
	run_case "runtime-arg" "must be a compile-time constant"; \
	printf 'package main\nfunc fill(comptime n int, s int) int { return s }\nfunc main() { f := fill; _ = f }\n' > build/cvm.goo; \
	run_case "assign-as-value" "cannot be used as a value"; \
	printf 'package main\nfunc fill(comptime n int, s int) int { return s }\nfunc takes(f func(int, int) int) int { return f(1, 2) }\nfunc main() { _ = takes(fill) }\n' > build/cvm.goo; \
	run_case "call-arg" "cannot be passed as an argument"; \
	printf 'package main\nfunc fill(comptime n int, s int) int { return s }\nfunc Apply[T any](f func(int, int) int, x T) T { return x }\nfunc main() { _ = Apply(fill, 5) }\n' > build/cvm.goo; \
	run_case "generic-call-arg" "cannot be passed as an argument"; \
	printf 'package main\ntype Holder struct { f func(int, int) int }\nfunc fill(comptime n int, s int) int { return s }\nfunc main() { h := Holder{f: fill}; _ = h }\n' > build/cvm.goo; \
	run_case "composite-literal" "cannot be stored in a composite literal"; \
	printf 'package main\nfunc fill(comptime n int, s int) int { return s }\nfunc main() {\n    ch := make(chan int, 1)\n    ch <- fill\n}\n' > build/cvm.goo; \
	run_case "channel-send" "cannot be sent on a channel"; \
	printf 'package main\ntype S struct { v int }\nfunc (s S) Fill(comptime n int, x int) int { return x }\nfunc main() { }\n' > build/cvm.goo; \
	run_case "method-declaration" "not yet supported on methods"; \
	printf 'package main\nimport "cpkg"\nfunc main() { x := 5; _ = cpkg.Fill(x, 1) }\n' > build/cvm.goo; \
	run_case "package-runtime-arg" "must be a compile-time constant"; \
	printf 'package main\nfunc bad[T any](comptime n T) T { return n }\nfunc main() { }\n' > build/cvm.goo; \
	run_case "composed-tparam-comptime-type" "comptime parameter type cannot be a type parameter"; \
	printf 'package main\nfunc kernel[T any](comptime n int, seed T) T { return seed }\nfunc main() { x := 5; _ = kernel(x, 10) }\n' > build/cvm.goo; \
	run_case "composed-runtime-comptime-arg" "must be a compile-time constant"; \
	printf 'package main\nfunc kernel[T any](comptime n int, seed T) T { return seed }\nfunc main() { f := kernel; _ = f }\n' > build/cvm.goo; \
	run_case "composed-fn-as-value" "cannot be used as a value"; \
	printf 'package main\nfunc main() {\n    f := func(comptime n int, s int) int { return s }\n    _ = f\n}\n' > build/cvm.goo; \
	run_case "closure-declaration" "only supported on named functions"; \
	printf 'package main\nfunc fill(comptime n int, s int) int {\n    var buf [n]int\n    _ = buf\n    return s\n}\nfunc main() { _ = fill(-1, 1) }\n' > build/cvm.goo; \
	run_case "negative-length" "array length must be non-negative"; \
	printf 'package main\nfunc pick(comptime n int, s int) int {\n    var buf [n]int\n    buf[3] = s\n    return buf[3]\n}\nfunc main() { _ = pick(2, 1) }\n' > build/cvm.goo; \
	run_case "const-index-oob-instance" "out of bounds .0:2. in comptime instance"; \
	printf 'package main\nfunc asgn(comptime n int, s int) int {\n    var a [n]int\n    var b [4]int\n    b = a\n    return b[0] + s\n}\nfunc main() { _ = asgn(2, 1) }\n' > build/cvm.goo; \
	run_case "array-assign-mismatch-instance" "length array in comptime instance"; \
	printf 'package main\nfunc sum4(arr [4]int) int { return arr[0] }\nfunc f(comptime n int, s int) int {\n    var a [n]int\n    a[0] = s\n    return sum4(a)\n}\nfunc main() { _ = f(2, 5) }\n' > build/cvm.goo; \
	run_case "call-arg-mismatch-instance" "length array parameter in comptime instance"; \
	printf 'package main\nfunc f(comptime n int, s int) int {\n    var a [n]int\n    a[0] = s\n    var b [4]int = a\n    return b[0]\n}\nfunc main() { _ = f(2, 5) }\n' > build/cvm.goo; \
	run_case "var-init-mismatch-instance" "length array in comptime instance"; \
	printf 'package main\nfunc Id[T any](x T) T { return x }\nfunc f(comptime n int, s int) int {\n    var a [n]int\n    a[0] = s\n    b := Id(a)\n    return b[0]\n}\nfunc main() { _ = f(4, 5) }\n' > build/cvm.goo; \
	run_case "generic-typeparam-comptime-array" "cannot bind a generic type parameter"; \
	printf 'package main\nimport "cpkg"\nfunc main() { _ = cpkg.GenFill(3, 5) }\n' > build/cvm.goo; \
	run_case "package-generic-comptime" "argument 2: cannot use int64 as T"; \
	echo "comptime-value-reject-matrix: PASS (19/19 walls hold)"

# Composed generic+comptime IR pin (sub-project 2, Task 4 step 4): the golden
# probe's stdout diff alone can't tell "one specialized instance reused
# correctly" from "three instances that happen to compute the same numbers",
# so this greps the emitted LLVM IR directly for the three distinct combined
# mangled symbols the design doc's mangling scheme predicts
# (`base__<typetok>...__n<value>...`, monomorphize.c) — kernel__int64__n4,
# kernel__int64__n2, kernel__float64__n4 — each defined EXACTLY ONCE despite
# multiple call sites per tuple (dedup: kernel__int64__n4 alone is called
# from three call sites in the probe — two direct, one inside the `go`
# wrapper — and must still have only one `define`). Distinct alloca array
# sizes/types per instance are also pinned as the codegen-level proof that
# `[n]T` actually re-derived a real per-instance length and element type,
# not a shared template placeholder.
#
# The per-symbol exactly-once greps catch collapse (count 0) and misnaming,
# but NOT duplication: if the monomorphizer's dedup guard
# (LLVMGetNamedFunction / mono_seen_has) were bypassed, LLVM auto-uniquifies
# the second insertion to `@"kernel__int64__n4.1"` — the base symbol still
# counts exactly 1, so the per-symbol check passes. The TOTAL-count assertion
# (exactly 3 `kernel__`-prefixed defines) closes that hole: a `.1`-suffixed
# duplicate bumps the total to 4 and FAILs.
#
# Call-EDGE greps pin the call-site -> instance WIRING, not just instance
# generation: without them, a cross-wired dispatch (main's `kernel(2, ...)`
# call linking to `__n4`) would pass every other gate — the defines all
# exist, and the probe's stdout happens to be dispatch-insensitive for some
# shapes. Three edges are pinned: main's two direct literal-argument calls
# (the comptime value is baked into the FIRST argument, so `(i64 4, i64 10)`
# vs `(i64 2, i64 100)` ties each call site to its exact instance), and at
# least one register-argument edge to `__n2` — only the defer trampoline and
# the go-thunk call with loaded (register) first arguments, so `\(i64 %` after
# the symbol matches exactly those paths without pinning the volatile
# register names themselves.
comptime-generic-compose-ir-pin: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== comptime-generic-compose-ir-pin: composed instances are real and deduped ==="
	@"$(COMPILER)" --emit-llvm examples/comptime_generic_compose_probe.goo -o build/cgc_ir.ll >build/cgc_ir.err 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "comptime-generic-compose-ir-pin: FAIL (compile failed)"; cat build/cgc_ir.err; exit 1; fi
	@for sym in kernel__int64__n4 kernel__int64__n2 kernel__float64__n4; do \
	  n=$$(grep -cE "^define[^{]*@\"?$${sym}\"?\(" build/cgc_ir.ll); \
	  if [ "$$n" != "1" ]; then echo "comptime-generic-compose-ir-pin: FAIL ($$sym: expected exactly 1 define, found $$n)"; exit 1; fi; \
	  echo "  PASS $$sym defined exactly once"; \
	done
	@total=$$(grep -cE "^define[^{]*@\"?kernel__" build/cgc_ir.ll); \
	  if [ "$$total" != "3" ]; then \
	    echo "comptime-generic-compose-ir-pin: FAIL (expected exactly 3 kernel__ instance defines total, found $$total — a uniquified duplicate escaped dedup)"; \
	    grep -nE "^define[^{]*@\"?kernel__" build/cgc_ir.ll; exit 1; \
	  fi; \
	  echo "  PASS exactly 3 kernel__ instance defines total (no uniquified duplicates)"
	@if ! grep -qE "call i64 @\"?kernel__int64__n4\"?\(i64 4, i64 10\)" build/cgc_ir.ll; then \
	    echo "comptime-generic-compose-ir-pin: FAIL (missing call edge: kernel(4, 10) -> kernel__int64__n4)"; \
	    grep -nE "call [a-z0-9]+ @\"?kernel__" build/cgc_ir.ll; exit 1; \
	  fi; \
	  echo "  PASS call edge kernel(4, 10) -> kernel__int64__n4"
	@if ! grep -qE "call i64 @\"?kernel__int64__n2\"?\(i64 2, i64 100\)" build/cgc_ir.ll; then \
	    echo "comptime-generic-compose-ir-pin: FAIL (missing call edge: kernel(2, 100) -> kernel__int64__n2)"; \
	    grep -nE "call [a-z0-9]+ @\"?kernel__" build/cgc_ir.ll; exit 1; \
	  fi; \
	  echo "  PASS call edge kernel(2, 100) -> kernel__int64__n2"
	@if ! grep -qE "call i64 @\"?kernel__int64__n2\"?\(i64 %" build/cgc_ir.ll; then \
	    echo "comptime-generic-compose-ir-pin: FAIL (missing defer/go-thunk register-arg call edge to kernel__int64__n2)"; \
	    grep -nE "call [a-z0-9]+ @\"?kernel__" build/cgc_ir.ll; exit 1; \
	  fi; \
	  echo "  PASS defer/go-thunk register-arg call edge -> kernel__int64__n2"
	@n4_alloca=$$(awk '/^define i64 @"?kernel__int64__n4"?\(/,/^}/' build/cgc_ir.ll | grep -c "alloca \[4 x i64\]"); \
	  n2_alloca=$$(awk '/^define i64 @"?kernel__int64__n2"?\(/,/^}/' build/cgc_ir.ll | grep -c "alloca \[2 x i64\]"); \
	  f4_alloca=$$(awk '/^define double @"?kernel__float64__n4"?\(/,/^}/' build/cgc_ir.ll | grep -c "alloca \[4 x double\]"); \
	  if [ "$$n4_alloca" -lt 1 ] || [ "$$n2_alloca" -lt 1 ] || [ "$$f4_alloca" -lt 1 ]; then \
	    echo "comptime-generic-compose-ir-pin: FAIL (distinct per-instance alloca sizes/types not found: n4=$$n4_alloca n2=$$n2_alloca f4=$$f4_alloca)"; exit 1; \
	  fi; \
	  echo "  PASS distinct alloca sizes/types ([4 x i64], [2 x i64], [4 x double])"
	@echo "comptime-generic-compose-ir-pin: PASS"

# lanes-monomorphize-ir-pin (P6 M1 Task 7): cross-package comptime
# monomorphization proof for goostd/lanes.Partition — same keystone as
# comptime-generic-compose-ir-pin, one level up the package boundary.
# examples/lanes_monomorphize_probe.goo calls lanes.Partition with two
# distinct comptime counts (2 and 4) on two different backing arrays; the
# emitted symbols are package-mangled
# (`goo_pkg__lanes__Partition__n<value>` — codegen.c's package-mangling
# composed with monomorphize.c's `__n<value>` comptime-instance suffix; see
# goostd/cpkg's `goo_pkg__cpkg__Fill__n4` precedent, monomorphize.c:793).
# Symbol spelling verified empirically against build/lm_ir.ll before this
# target was written (see docs/superpowers/sdd/task-7-report.md) — it is
# NOT guessed from the design doc's bare `lanes__Partition__n2` shorthand.
#
# Same three-part structure as comptime-generic-compose-ir-pin:
#   (i)   per-symbol exactly-once grep catches collapse (0 defines) or
#         misnaming for each of __n2 and __n4;
#   (ii)  a TOTAL-count check (exactly 2 `goo_pkg__lanes__Partition__`
#         -prefixed defines) closes the same LLVM auto-uniquify `.1` hole:
#         a bypassed dedup guard would still leave the base symbol's own
#         count at 1 (LLVM renames the SECOND insertion, not the first), so
#         only the total check catches it;
#   (iii) one call-edge grep per count, pinning call-site -> instance
#         WIRING (not just instance existence) — a cross-wired dispatch
#         would pass (i) and (ii) but land here. Partition's signature is
#         (arr []float64, comptime count int): the comptime value is the
#         SECOND argument (unlike kernel's first-argument comptime value in
#         the sibling probe), and the first argument is always a register
#         (the caller's own local slice variable, never a literal), so each
#         edge pattern anchors on the literal trailing `i64 2)`/`i64 4)` and
#         allows any register name for the slice-struct argument — the same
#         "don't pin volatile register names" principle the sibling pin's
#         doc comment states for its own register-argument edge.
lanes-monomorphize-ir-pin: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== lanes-monomorphize-ir-pin: goostd/lanes.Partition instances are real and deduped ==="
	@"$(COMPILER)" --emit-llvm examples/lanes_monomorphize_probe.goo -o build/lm_ir.ll >build/lm_ir.err 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "lanes-monomorphize-ir-pin: FAIL (compile failed)"; cat build/lm_ir.err; exit 1; fi
	@for sym in goo_pkg__lanes__Partition__n2 goo_pkg__lanes__Partition__n4; do \
	  n=$$(grep -cE "^define[^{]*@\"?$${sym}\"?\(" build/lm_ir.ll); \
	  if [ "$$n" != "1" ]; then echo "lanes-monomorphize-ir-pin: FAIL ($$sym: expected exactly 1 define, found $$n)"; exit 1; fi; \
	  echo "  PASS $$sym defined exactly once"; \
	done
	@total=$$(grep -cE "^define[^{]*@\"?goo_pkg__lanes__Partition__" build/lm_ir.ll); \
	  if [ "$$total" != "2" ]; then \
	    echo "lanes-monomorphize-ir-pin: FAIL (expected exactly 2 goo_pkg__lanes__Partition__ instance defines total, found $$total — a uniquified duplicate escaped dedup)"; \
	    grep -nE "^define[^{]*@\"?goo_pkg__lanes__Partition__" build/lm_ir.ll; exit 1; \
	  fi; \
	  echo "  PASS exactly 2 goo_pkg__lanes__Partition__ instance defines total (no uniquified duplicates)"
	@if ! grep -qE 'call %Partitioned @"?goo_pkg__lanes__Partition__n2"?\(\{ ptr, i64, i64 \} %[A-Za-z0-9_]+, i64 2\)' build/lm_ir.ll; then \
	    echo "lanes-monomorphize-ir-pin: FAIL (missing call edge: Partition(arrayA, 2) -> goo_pkg__lanes__Partition__n2)"; \
	    grep -nE 'call [^@]+@"?goo_pkg__lanes__Partition__' build/lm_ir.ll; exit 1; \
	  fi; \
	  echo "  PASS call edge Partition(arrayA, 2) -> goo_pkg__lanes__Partition__n2"
	@if ! grep -qE 'call %Partitioned @"?goo_pkg__lanes__Partition__n4"?\(\{ ptr, i64, i64 \} %[A-Za-z0-9_]+, i64 4\)' build/lm_ir.ll; then \
	    echo "lanes-monomorphize-ir-pin: FAIL (missing call edge: Partition(arrayB, 4) -> goo_pkg__lanes__Partition__n4)"; \
	    grep -nE 'call [^@]+@"?goo_pkg__lanes__Partition__' build/lm_ir.ll; exit 1; \
	  fi; \
	  echo "  PASS call edge Partition(arrayB, 4) -> goo_pkg__lanes__Partition__n4"
	@echo "lanes-monomorphize-ir-pin: PASS"

# spmd-bench-probe: SPMD harness sub-project, Task 3 — "the proof". Builds a
# CPU-bound comptime-specialized kernel (`burn`: a tight LCG loop over a
# comptime-fixed iteration count, deterministic and side-effect-free per
# lane) TWICE from source generated inline into build/ (per the sub-project
# plan: bench programs are not goldens, so they don't belong in examples/) —
# once fanned out across N=8 goroutines via `go burn(...)`, once as a serial
# baseline via N direct calls — and diffs their stdout. Buffered-channel
# fan-in (cap N) makes the aggregate checksum order-independent (summation
# is commutative), so both variants MUST print the identical total
# regardless of goroutine interleaving or how many OS threads the M8
# scheduler actually schedules onto. Correctness (compile + run + bit-
# identical output) is the ONLY thing ASSERTED — wall-clock and CPU
# utilization are REPORTED (informational echo lines), never asserted,
# because timing is inherently noisy and this probe must pass on machines
# with fewer cores than were available when the pattern was scouted (see
# docs/spmd-harness.md's measured-numbers section: 787% CPU / ~6.2x wall
# speedup, 8-lane vs serial, on a 32-core machine, method: external
# `/usr/bin/time`).
#
# Timing detection is portable and best-effort, three tiers: `/usr/bin/time
# -v` (GNU, reports Percent-of-CPU) is preferred; `/usr/bin/time -l`
# (BSD/macOS, reports real/user/sys on one line) is the fallback; if neither
# is available the shell's builtin `time` is used (real/user/sys only, no
# CPU%, reported as "n/a"). Wall-clock for the report AND for the optional
# gate below is measured independently via `date +%s.%N` deltas so it does
# not depend on which timing tier is available — this trades a soft
# dependency on GNU `date` (this repo's dev/CI environment) for not having
# to parse three different wall-clock text formats; on a machine where
# `date +%s.%N` is unsupported the wall-clock report degrades to a
# nonsensical number but nothing here ever turns that into a FAIL.
#
# SPMD_BENCH_ASSERT_SPEEDUP=<factor>: OFF by default. When set to a number
# > 1, additionally FAILS the target if serial-wall / 8lane-wall is below
# that factor. Manual local runs only, e.g.:
#   make spmd-bench-probe SPMD_BENCH_ASSERT_SPEEDUP=2
# — wall-clock ratios are too noisy on shared/CI machines to be a
# correctness gate, which is why this is opt-in and undocumented in `verify`.
spmd-bench-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== spmd-bench-probe: CPU-bound parallelism proof (8-lane vs serial) ==="
	@printf '%s\n' \
		'package main' \
		'' \
		'import "fmt"' \
		'' \
		'func burn(comptime iters int, seed int64, ch chan int64) {' \
		'x := seed' \
		'i := 0' \
		'for i < iters {' \
		'x = x*1103515245 + 12345' \
		'i = i + 1' \
		'}' \
		'ch <- x' \
		'}' \
		'' \
		'func main() {' \
		'const N = 8' \
		'const ITERS = 200000000' \
		'ch := make(chan int64, N)' \
		'i := 0' \
		'for i < N {' \
		'go burn(ITERS, int64(i), ch)' \
		'i = i + 1' \
		'}' \
		'total := int64(0)' \
		'i = 0' \
		'for i < N {' \
		'total = total + <-ch' \
		'i = i + 1' \
		'}' \
		'fmt.Println(total)' \
		'}' \
		> build/spmd_bench_8lane.goo
	@printf '%s\n' \
		'package main' \
		'' \
		'import "fmt"' \
		'' \
		'func burn(comptime iters int, seed int64, ch chan int64) {' \
		'x := seed' \
		'i := 0' \
		'for i < iters {' \
		'x = x*1103515245 + 12345' \
		'i = i + 1' \
		'}' \
		'ch <- x' \
		'}' \
		'' \
		'func main() {' \
		'const N = 8' \
		'const ITERS = 200000000' \
		'ch := make(chan int64, N)' \
		'i := 0' \
		'for i < N {' \
		'burn(ITERS, int64(i), ch)' \
		'i = i + 1' \
		'}' \
		'total := int64(0)' \
		'i = 0' \
		'for i < N {' \
		'total = total + <-ch' \
		'i = i + 1' \
		'}' \
		'fmt.Println(total)' \
		'}' \
		> build/spmd_bench_serial.goo
	@$(COMPILER) -o build/spmd_bench_8lane build/spmd_bench_8lane.goo > build/spmd_bench_8lane.cerr 2>&1; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "spmd-bench-probe: FAIL (8-lane compile rc=$$rc)"; cat build/spmd_bench_8lane.cerr; exit 1; fi
	@$(COMPILER) -o build/spmd_bench_serial build/spmd_bench_serial.goo > build/spmd_bench_serial.cerr 2>&1; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "spmd-bench-probe: FAIL (serial compile rc=$$rc)"; cat build/spmd_bench_serial.cerr; exit 1; fi
	@TIME_MODE=none; \
	if [ -x /usr/bin/time ] && /usr/bin/time -v true >/dev/null 2>&1; then TIME_MODE=gnu; \
	elif [ -x /usr/bin/time ] && /usr/bin/time -l true >/dev/null 2>&1; then TIME_MODE=bsd; \
	fi; \
	echo "spmd-bench-probe: timing method = $$TIME_MODE (report-only, never a pass/fail threshold)"; \
	t0=$$(date +%s.%N); \
	if [ "$$TIME_MODE" = "gnu" ]; then \
	  /usr/bin/time -v ./build/spmd_bench_8lane > build/spmd_bench_8lane.out 2> build/spmd_bench_8lane.time; rc=$$?; \
	elif [ "$$TIME_MODE" = "bsd" ]; then \
	  /usr/bin/time -l ./build/spmd_bench_8lane > build/spmd_bench_8lane.out 2> build/spmd_bench_8lane.time; rc=$$?; \
	else \
	  { time ./build/spmd_bench_8lane > build/spmd_bench_8lane.out; } 2> build/spmd_bench_8lane.time; rc=$$?; \
	fi; \
	t1=$$(date +%s.%N); \
	if [ $$rc -ne 0 ]; then echo "spmd-bench-probe: FAIL (8-lane run rc=$$rc)"; cat build/spmd_bench_8lane.time; exit 1; fi; \
	wall_8lane=$$(awk -v a="$$t0" -v b="$$t1" 'BEGIN{printf "%.3f", b-a}'); \
	t0=$$(date +%s.%N); \
	if [ "$$TIME_MODE" = "gnu" ]; then \
	  /usr/bin/time -v ./build/spmd_bench_serial > build/spmd_bench_serial.out 2> build/spmd_bench_serial.time; rc=$$?; \
	elif [ "$$TIME_MODE" = "bsd" ]; then \
	  /usr/bin/time -l ./build/spmd_bench_serial > build/spmd_bench_serial.out 2> build/spmd_bench_serial.time; rc=$$?; \
	else \
	  { time ./build/spmd_bench_serial > build/spmd_bench_serial.out; } 2> build/spmd_bench_serial.time; rc=$$?; \
	fi; \
	t1=$$(date +%s.%N); \
	if [ $$rc -ne 0 ]; then echo "spmd-bench-probe: FAIL (serial run rc=$$rc)"; cat build/spmd_bench_serial.time; exit 1; fi; \
	wall_serial=$$(awk -v a="$$t0" -v b="$$t1" 'BEGIN{printf "%.3f", b-a}'); \
	if ! diff -u build/spmd_bench_8lane.out build/spmd_bench_serial.out; then \
	  echo "spmd-bench-probe: FAIL (8-lane and serial outputs differ — not deterministic)"; exit 1; \
	fi; \
	cpu_8lane=""; cpu_serial=""; \
	if [ "$$TIME_MODE" = "gnu" ]; then \
	  cpu_8lane=$$(grep -F "Percent of CPU this job got" build/spmd_bench_8lane.time | grep -oE '[0-9]+%'); \
	  cpu_serial=$$(grep -F "Percent of CPU this job got" build/spmd_bench_serial.time | grep -oE '[0-9]+%'); \
	elif [ "$$TIME_MODE" = "bsd" ]; then \
	  cpu_8lane=$$(awk '{for(i=1;i<=NF;i++){if($$i=="real")r=$$(i-1); if($$i=="user")u=$$(i-1); if($$i=="sys")s=$$(i-1)}} END{if(r>0) printf "%.0f%%", (u+s)/r*100}' build/spmd_bench_8lane.time); \
	  cpu_serial=$$(awk '{for(i=1;i<=NF;i++){if($$i=="real")r=$$(i-1); if($$i=="user")u=$$(i-1); if($$i=="sys")s=$$(i-1)}} END{if(r>0) printf "%.0f%%", (u+s)/r*100}' build/spmd_bench_serial.time); \
	fi; \
	[ -n "$$cpu_8lane" ] || cpu_8lane="n/a"; \
	[ -n "$$cpu_serial" ] || cpu_serial="n/a"; \
	echo "spmd-bench-probe: REPORT 8-lane  wall=$${wall_8lane}s cpu=$$cpu_8lane"; \
	echo "spmd-bench-probe: REPORT serial  wall=$${wall_serial}s cpu=$$cpu_serial"; \
	speedup=$$(awk -v s="$$wall_serial" -v p="$$wall_8lane" 'BEGIN{if (p>0) printf "%.2f", s/p; else print "n/a"}'); \
	echo "spmd-bench-probe: REPORT speedup (serial-wall / 8lane-wall) = $${speedup}x (informational only, never a pass/fail threshold)"; \
	if [ -n "$$SPMD_BENCH_ASSERT_SPEEDUP" ]; then \
	  ok=$$(awk -v got="$$speedup" -v want="$$SPMD_BENCH_ASSERT_SPEEDUP" 'BEGIN{print (got+0 >= want+0) ? 1 : 0}'); \
	  if [ "$$ok" != "1" ]; then \
	    echo "spmd-bench-probe: FAIL (SPMD_BENCH_ASSERT_SPEEDUP=$$SPMD_BENCH_ASSERT_SPEEDUP not met: got $${speedup}x)"; \
	    exit 1; \
	  fi; \
	  echo "spmd-bench-probe: speedup gate PASS ($${speedup}x >= $${SPMD_BENCH_ASSERT_SPEEDUP}x)"; \
	fi; \
	echo "spmd-bench-probe: PASS (8-lane and serial compiled, ran, and produced bit-identical output)"

# P6 M1 Task 8, Step 1 (spike verdict (c) -- helgrind is available but
# STRUCTURALLY BLIND to goroutine races under the M:N ucontext scheduler;
# see docs/superpowers/specs/2026-07-11-p6-lanes-m1-spike-findings.md
# Section 3: a proven real data race -- lost updates, 3000000 != 4000000
# -- produced "0 errors" from helgrind at every GOMAXPROCS). Wiring a
# helgrind gate here would certify race-freedom it structurally cannot
# see, which is worse than no gate at all, so the design's own
# pre-authorized fallback applies: a documented manual runbook instead of
# an automated race-detector gate (docs/lanes-race-runbook.md). This probe
# does not and cannot detect races itself -- it only asserts the runbook
# file exists and still contains its load-bearing section headings, so
# the doc can't silently rot out from under the gate that references it.
stencil-race-runbook-probe:
	@echo "=== stencil-race-runbook-probe: docs/lanes-race-runbook.md exists + load-bearing sections present ==="
	@if [ ! -f docs/lanes-race-runbook.md ]; then \
	  echo "stencil-race-runbook-probe: FAIL (docs/lanes-race-runbook.md missing)"; exit 1; \
	fi
	@fail=0; \
	for heading in "## Why there is no automated race gate" "## What the compile-time proofs guarantee (and do not)" "## Manual runbook: re-checking with future tooling" "## When to revisit"; do \
	  if ! grep -qF "$$heading" docs/lanes-race-runbook.md; then \
	    echo "stencil-race-runbook-probe: FAIL (missing section heading: $$heading)"; fail=1; \
	  fi; \
	done; \
	if [ $$fail -ne 0 ]; then exit 1; fi
	@echo "stencil-race-runbook-probe: PASS"

# stencil-parallel-probe: P6 M1 Task 8, Step 2 -- the parallel-soak sibling
# of spmd-bench-probe above, but exercised through the ACTUAL goostd/lanes
# package (comptime count=8) rather than raw `go` fan-out -- this is the
# proof that lanes itself, not just the underlying goroutine primitive,
# delivers real wall-clock parallelism. Builds a CPU-bound per-cell
# workload (`burn`: a tight float64 multiply-add loop, 200,000,000
# iterations per cell, deterministic and side-effect-free per cell -- no
# cross-cell dependency, so cell order/interleaving cannot change the
# answer) TWICE from source generated inline into build/ (same rationale
# as spmd-bench-probe: these are benchmark programs, not goldens, so they
# don't belong in examples/) -- once via lanes.Partition(data, 8) +
# lanes.Run driving one goroutine per lane, once as a serial reference
# that calls the identical `burn` function directly in a loop with no
# lanes/goroutines involved at all -- and diffs their stdout. Because each
# cell's result depends only on its own seed, both variants MUST print the
# identical total regardless of how many OS threads the M8 scheduler
# actually schedules the 8 lane goroutines onto.
#
# Correctness (compile + run + bit-identical output) is the ONLY thing
# ASSERTED here -- wall-clock and CPU utilization are REPORTED
# (informational echo lines), never asserted by default. This is a
# DELIBERATE deviation from the design spec's wording ("asserting
# wall-time speedup"): wall-clock ratios are inherently noisy on
# shared/CI machines and this probe must stay green on machines with
# fewer cores than were available when it was authored -- following this
# repo's spmd-bench-probe precedent above (see that target's comment for
# the full rationale). The opt-in LANES_BENCH_ASSERT_SPEEDUP env var below
# IS the speedup assertion, for the cases (a developer's own many-core
# box) where hardware makes it meaningful; verify-core never sets it.
#
# Measured on a 32-thread/16-core AMD Ryzen 9 5950X during authoring:
# 8-lane wall ~0.67s (795% CPU) vs serial wall ~5.1s (99% CPU), ~7.6x
# speedup, both printing 2.12508e+10. Numbers will vary by machine; only
# the bit-identical-output assertion is load-bearing.
#
# Timing detection mirrors spmd-bench-probe verbatim: /usr/bin/time -v
# (GNU) preferred, /usr/bin/time -l (BSD/macOS) fallback, shell builtin
# `time` (no CPU%) as the last resort; wall-clock for the report AND the
# optional gate is measured independently via `date +%s.%N` deltas so it
# does not depend on which timing tier is available.
#
# LANES_BENCH_ASSERT_SPEEDUP=<factor>: OFF by default. When set to a
# number > 1, additionally FAILS the target if serial-wall / 8lane-wall is
# below that factor. Manual local runs only, e.g.:
#   make stencil-parallel-probe LANES_BENCH_ASSERT_SPEEDUP=2
stencil-parallel-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== stencil-parallel-probe: goostd/lanes CPU-bound parallelism proof (8-lane vs serial) ==="
	@printf '%s\n' \
		'package main' \
		'' \
		'import "fmt"' \
		'import "lanes"' \
		'' \
		'func burn(iters int, seed float64) float64 {' \
		'x := seed' \
		'i := 0' \
		'for i < iters {' \
		'x = x*1.0000001013 + 0.0000000731' \
		'i = i + 1' \
		'}' \
		'return x' \
		'}' \
		'' \
		'func main() {' \
		'data := []float64{0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0}' \
		'p := lanes.Partition(data, 8)' \
		'out := lanes.Run(p, 1, func(ctx *lanes.Lane) {' \
		'own := ctx.Own()' \
		'own[0] = burn(200000000, own[0])' \
		'})' \
		'total := 0.0' \
		'i := 0' \
		'for i < 8 {' \
		'total = total + out[i]' \
		'i = i + 1' \
		'}' \
		'fmt.Println(total)' \
		'}' \
		> build/stencil_parallel_8lane.goo
	@printf '%s\n' \
		'package main' \
		'' \
		'import "fmt"' \
		'' \
		'func burn(iters int, seed float64) float64 {' \
		'x := seed' \
		'i := 0' \
		'for i < iters {' \
		'x = x*1.0000001013 + 0.0000000731' \
		'i = i + 1' \
		'}' \
		'return x' \
		'}' \
		'' \
		'func main() {' \
		'data := []float64{0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0}' \
		'i := 0' \
		'for i < 8 {' \
		'data[i] = burn(200000000, data[i])' \
		'i = i + 1' \
		'}' \
		'total := 0.0' \
		'i = 0' \
		'for i < 8 {' \
		'total = total + data[i]' \
		'i = i + 1' \
		'}' \
		'fmt.Println(total)' \
		'}' \
		> build/stencil_parallel_serial.goo
	@$(COMPILER) -o build/stencil_parallel_8lane build/stencil_parallel_8lane.goo > build/stencil_parallel_8lane.cerr 2>&1; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "stencil-parallel-probe: FAIL (8-lane compile rc=$$rc)"; cat build/stencil_parallel_8lane.cerr; exit 1; fi
	@$(COMPILER) -o build/stencil_parallel_serial build/stencil_parallel_serial.goo > build/stencil_parallel_serial.cerr 2>&1; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "stencil-parallel-probe: FAIL (serial compile rc=$$rc)"; cat build/stencil_parallel_serial.cerr; exit 1; fi
	@TIME_MODE=none; \
	if [ -x /usr/bin/time ] && /usr/bin/time -v true >/dev/null 2>&1; then TIME_MODE=gnu; \
	elif [ -x /usr/bin/time ] && /usr/bin/time -l true >/dev/null 2>&1; then TIME_MODE=bsd; \
	fi; \
	echo "stencil-parallel-probe: timing method = $$TIME_MODE (report-only, never a pass/fail threshold)"; \
	t0=$$(date +%s.%N); \
	if [ "$$TIME_MODE" = "gnu" ]; then \
	  /usr/bin/time -v ./build/stencil_parallel_8lane > build/stencil_parallel_8lane.out 2> build/stencil_parallel_8lane.time; rc=$$?; \
	elif [ "$$TIME_MODE" = "bsd" ]; then \
	  /usr/bin/time -l ./build/stencil_parallel_8lane > build/stencil_parallel_8lane.out 2> build/stencil_parallel_8lane.time; rc=$$?; \
	else \
	  { time ./build/stencil_parallel_8lane > build/stencil_parallel_8lane.out; } 2> build/stencil_parallel_8lane.time; rc=$$?; \
	fi; \
	t1=$$(date +%s.%N); \
	if [ $$rc -ne 0 ]; then echo "stencil-parallel-probe: FAIL (8-lane run rc=$$rc)"; cat build/stencil_parallel_8lane.time; exit 1; fi; \
	wall_8lane=$$(awk -v a="$$t0" -v b="$$t1" 'BEGIN{printf "%.3f", b-a}'); \
	t0=$$(date +%s.%N); \
	if [ "$$TIME_MODE" = "gnu" ]; then \
	  /usr/bin/time -v ./build/stencil_parallel_serial > build/stencil_parallel_serial.out 2> build/stencil_parallel_serial.time; rc=$$?; \
	elif [ "$$TIME_MODE" = "bsd" ]; then \
	  /usr/bin/time -l ./build/stencil_parallel_serial > build/stencil_parallel_serial.out 2> build/stencil_parallel_serial.time; rc=$$?; \
	else \
	  { time ./build/stencil_parallel_serial > build/stencil_parallel_serial.out; } 2> build/stencil_parallel_serial.time; rc=$$?; \
	fi; \
	t1=$$(date +%s.%N); \
	if [ $$rc -ne 0 ]; then echo "stencil-parallel-probe: FAIL (serial run rc=$$rc)"; cat build/stencil_parallel_serial.time; exit 1; fi; \
	wall_serial=$$(awk -v a="$$t0" -v b="$$t1" 'BEGIN{printf "%.3f", b-a}'); \
	if ! diff -u build/stencil_parallel_8lane.out build/stencil_parallel_serial.out; then \
	  echo "stencil-parallel-probe: FAIL (8-lane and serial outputs differ -- not deterministic)"; exit 1; \
	fi; \
	cpu_8lane=""; cpu_serial=""; \
	if [ "$$TIME_MODE" = "gnu" ]; then \
	  cpu_8lane=$$(grep -F "Percent of CPU this job got" build/stencil_parallel_8lane.time | grep -oE '[0-9]+%'); \
	  cpu_serial=$$(grep -F "Percent of CPU this job got" build/stencil_parallel_serial.time | grep -oE '[0-9]+%'); \
	elif [ "$$TIME_MODE" = "bsd" ]; then \
	  cpu_8lane=$$(awk '{for(i=1;i<=NF;i++){if($$i=="real")r=$$(i-1); if($$i=="user")u=$$(i-1); if($$i=="sys")s=$$(i-1)}} END{if(r>0) printf "%.0f%%", (u+s)/r*100}' build/stencil_parallel_8lane.time); \
	  cpu_serial=$$(awk '{for(i=1;i<=NF;i++){if($$i=="real")r=$$(i-1); if($$i=="user")u=$$(i-1); if($$i=="sys")s=$$(i-1)}} END{if(r>0) printf "%.0f%%", (u+s)/r*100}' build/stencil_parallel_serial.time); \
	fi; \
	[ -n "$$cpu_8lane" ] || cpu_8lane="n/a"; \
	[ -n "$$cpu_serial" ] || cpu_serial="n/a"; \
	echo "stencil-parallel-probe: REPORT 8-lane  wall=$${wall_8lane}s cpu=$$cpu_8lane"; \
	echo "stencil-parallel-probe: REPORT serial  wall=$${wall_serial}s cpu=$$cpu_serial"; \
	speedup=$$(awk -v s="$$wall_serial" -v p="$$wall_8lane" 'BEGIN{if (p>0) printf "%.2f", s/p; else print "n/a"}'); \
	echo "stencil-parallel-probe: REPORT speedup (serial-wall / 8lane-wall) = $${speedup}x (informational only, never a pass/fail threshold)"; \
	if [ -n "$$LANES_BENCH_ASSERT_SPEEDUP" ]; then \
	  ok=$$(awk -v got="$$speedup" -v want="$$LANES_BENCH_ASSERT_SPEEDUP" 'BEGIN{print (got+0 >= want+0) ? 1 : 0}'); \
	  if [ "$$ok" != "1" ]; then \
	    echo "stencil-parallel-probe: FAIL (LANES_BENCH_ASSERT_SPEEDUP=$$LANES_BENCH_ASSERT_SPEEDUP not met: got $${speedup}x)"; \
	    exit 1; \
	  fi; \
	  echo "stencil-parallel-probe: speedup gate PASS ($${speedup}x >= $${LANES_BENCH_ASSERT_SPEEDUP}x)"; \
	fi; \
	echo "stencil-parallel-probe: PASS (8-lane and serial compiled, ran, and produced bit-identical output)"

# Task 3 (func-values): calling a nil function value must abort cleanly
# (Go: "invalid memory address or nil pointer dereference"-class panic),
# not jump to a NULL instruction pointer. `var f func(int) int` zero-values
# to the fat pointer {NULL, NULL}. Mirrors bits-div-abort-probe/divzero-
# probe's runtime-abort pattern: compiles cleanly (rc=0), the RUN exits 2
# (Go-conformant per Task 6; GOO_PANIC_ABORT=1 restores the old abort()/134
# for debugging), and the panic message is grepped.
funcnil-abort-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== funcnil-abort-probe: nil func value call must abort with 'nil function' ==="
	@"$(COMPILER)" examples/funcnil_abort.goo -o build/funcnil_abort.out 2>build/funcnil_abort.cerr || \
	  { echo "funcnil-abort-probe: FAIL (compile)"; cat build/funcnil_abort.cerr; exit 1; }
	@./build/funcnil_abort.out 2>build/funcnil_abort.err; rc=$$?; \
	if [ $$rc -ne 2 ]; then echo "funcnil-abort-probe: FAIL (nil call exit $$rc, want 2)"; exit 1; fi; \
	if ! grep -q "nil function" build/funcnil_abort.err; then echo "funcnil-abort-probe: FAIL (no nil-function panic message)"; cat build/funcnil_abort.err; exit 1; fi
	@echo "funcnil-abort-probe: PASS"

# queue #3 (funcval == nil): a function value compares to nil. A zero-valued
# `var f func(int) int` is the {NULL,NULL} fat pointer, so `f == nil` reads
# its fn-ptr word and is true; after `f = dbl` it is false and `f != nil` is
# true, and the assigned value still calls. Exercises the typecheck exception
# (expression_helpers.c) and the codegen fn-ptr compare (expression_codegen.c).
# The probe panics on any wrong answer, so a clean rc=0 is the pass signal.
funcval-nilcmp-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== funcval-nilcmp-probe: funcval == nil / != nil must be correct ==="
	@"$(COMPILER)" examples/funcval_nilcmp.goo -o build/funcval_nilcmp.out 2>build/funcval_nilcmp.cerr || \
	  { echo "funcval-nilcmp-probe: FAIL (compile)"; cat build/funcval_nilcmp.cerr; exit 1; }
	@./build/funcval_nilcmp.out 2>build/funcval_nilcmp.err; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "funcval-nilcmp-probe: FAIL (rc=$$rc)"; cat build/funcval_nilcmp.err; exit 1; fi
	@echo "funcval-nilcmp-probe: PASS"

# Task 2 (generic map values): calling a MISSING dispatch-table entry must
# hit the existing nil-func panic (zero-guard unbox yields the {NULL,NULL}
# fat pointer), not segfault. Mirrors funcnil-abort-probe's structure — same
# panic mechanism (call_codegen.c's indirect-call guard), same assertions
# (exit 2, Go-conformant per Task 6, GOO_PANIC_ABORT=1 restores the old
# abort()/134 for debugging + "nil function" on stderr) — with the func
# value read out of a map lookup instead of a bare zero-valued var.
map-nilfunc-abort-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== map-nilfunc-abort-probe: ops[missing]() must panic cleanly ==="
	@printf 'package main\nfunc main(){\n\tops := map[string]func() int{"a": func() int { return 1 }}\n\t_ = ops["missing"]()\n}\n' > build/map_nilfunc_abort.goo
	@"$(COMPILER)" build/map_nilfunc_abort.goo -o build/map_nilfunc_abort.out 2>build/map_nilfunc_abort.cerr || \
	  { echo "map-nilfunc-abort-probe: FAIL (compile)"; cat build/map_nilfunc_abort.cerr; exit 1; }
	@./build/map_nilfunc_abort.out 2>build/map_nilfunc_abort.err; rc=$$?; \
	if [ $$rc -ne 2 ]; then echo "map-nilfunc-abort-probe: FAIL (nil call exit $$rc, want 2)"; exit 1; fi; \
	if ! grep -q "nil function" build/map_nilfunc_abort.err; then echo "map-nilfunc-abort-probe: FAIL (no nil-function panic message)"; cat build/map_nilfunc_abort.err; exit 1; fi
	@echo "map-nilfunc-abort-probe: PASS"

# NOTE: the empty-interface type-switch guard that used to live here
# (type_check_type_switch_stmt, src/types/type_checker.c) was lifted by the
# RTTI concrete-type-switch plan Task 2 — per-type vtable identity
# (post-#132) makes the empty-interface case safe for concrete switch cases
# now, same as the assert-side lift in Task 1 (see the NOTE that used to
# sit above typeassert-emptyiface-reject-probe, now removed). That behavior
# is covered by the rtti_type_switch_any golden, not by a reject-probe here.
# Method-bearing interfaces were always unaffected (distinct thunks ->
# distinct vtables — see type_assert_probe / type_switch_probe /
# type_switch_fmt_probe goldens). The interface-TARGET case (`case S:` where
# S is itself an interface, the per-case loop in type_check_type_switch_stmt
# ~line 2308) used to be rejected pending its own codegen primitive; the
# interface-target RTTI plan's Task 3 lifted that guard and wired
# codegen_interface_target_match into the type switch — see
# examples/iface_target_switch.goo. No reject-probe remains for this shape
# (rtti-iface-target-reject-probe, formerly here, was retired).

# Task 4 (type assertions/switches — reject-probe sweep): two static
# `x.(T)` rejections bundled into one probe (mirrors floatint-reject-probe's
# multi-sub-check-per-target shape): (a) operand isn't an interface at all;
# (b) target concrete type doesn't implement the operand's interface
# (type_interface_satisfied miss). Both diagnostics live in
# expression_checker.c / type_checker.c.
#
# A third sub-check used to live here: COMMA-OK assert-to-an-interface-
# target, narrowed by the interface-target RTTI plan's Task 1 (which lifted
# the rejection for the SINGLE-return form only — `_ = a.(Named)`, see
# examples/iface_target_assert.goo). Task 2 wired the comma-ok codegen
# primitive too (`n, ok := a.(Named)` now compiles and runs — see
# examples/iface_target_commaok.goo), so that sub-check no longer describes
# a rejection and was removed; the positive case is covered by the golden
# above instead.
typeassert-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== typeassert-reject-probe: x.(T) static rejections ==="
	@printf 'package main\nfunc main() {\n\tvar x int = 5\n\t_ = x.(int)\n}\n' > build/ta_noniface.goo
	@printf 'package main\ntype Animal interface {\n\tSound() string\n}\ntype Dog struct{ N string }\nfunc (d Dog) Sound() string { return "woof" }\ntype Rock struct{}\nfunc main() {\n\tvar a Animal = Dog{N: "Rex"}\n\t_ = a.(Rock)\n}\n' > build/ta_impossible.goo
	@rm -f build/ta_noniface build/ta_impossible
	@$(COMPILER) -o build/ta_noniface build/ta_noniface.goo > build/ta_noniface.out 2> build/ta_noniface.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "typeassert-reject-probe: FAIL (non-interface operand: compiled rc=0)"; exit 1; fi; \
	if [ -x build/ta_noniface ]; then echo "typeassert-reject-probe: FAIL (non-interface operand: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "not an interface type" build/ta_noniface.err; then echo "typeassert-reject-probe: FAIL (non-interface operand: wrong/missing diagnostic)"; cat build/ta_noniface.err; exit 1; fi
	@$(COMPILER) -o build/ta_impossible build/ta_impossible.goo > build/ta_impossible.out 2> build/ta_impossible.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "typeassert-reject-probe: FAIL (impossible assertion: compiled rc=0)"; exit 1; fi; \
	if [ -x build/ta_impossible ]; then echo "typeassert-reject-probe: FAIL (impossible assertion: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "impossible type assertion" build/ta_impossible.err; then echo "typeassert-reject-probe: FAIL (impossible assertion: wrong/missing diagnostic)"; cat build/ta_impossible.err; exit 1; fi
	@echo "typeassert-reject-probe: PASS"

# Task 4 (type assertions/switches — reject-probe sweep): two static
# `switch x.(type)` rejections bundled into one probe: (a) the same case
# type appearing twice across the whole switch (not just one clause); (b)
# more than one `default` clause. Both diagnostics live in
# type_check_type_switch_stmt (type_checker.c).
typeswitch-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== typeswitch-reject-probe: switch x.(type) static rejections ==="
	@printf 'package main\nimport "fmt"\ntype Animal interface {\n\tSound() string\n}\ntype Dog struct{ N string }\nfunc (d Dog) Sound() string { return "woof" }\ntype Cat struct{ Lives int }\nfunc (c Cat) Sound() string { return "meow" }\nfunc main() {\n\tvar a Animal = Dog{N: "Rex"}\n\tswitch a.(type) {\n\tcase Dog:\n\t\tfmt.Println("dog")\n\tcase Dog:\n\t\tfmt.Println("dog2")\n\tcase Cat:\n\t\tfmt.Println("cat")\n\t}\n}\n' > build/ts_dupcase.goo
	@printf 'package main\nimport "fmt"\ntype Animal interface {\n\tSound() string\n}\ntype Dog struct{ N string }\nfunc (d Dog) Sound() string { return "woof" }\nfunc main() {\n\tvar a Animal = Dog{N: "Rex"}\n\tswitch a.(type) {\n\tcase Dog:\n\t\tfmt.Println("dog")\n\tdefault:\n\t\tfmt.Println("other")\n\tdefault:\n\t\tfmt.Println("other2")\n\t}\n}\n' > build/ts_multidefault.goo
	@rm -f build/ts_dupcase build/ts_multidefault
	@$(COMPILER) -o build/ts_dupcase build/ts_dupcase.goo > build/ts_dupcase.out 2> build/ts_dupcase.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "typeswitch-reject-probe: FAIL (duplicate case type: compiled rc=0)"; exit 1; fi; \
	if [ -x build/ts_dupcase ]; then echo "typeswitch-reject-probe: FAIL (duplicate case type: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "duplicate case type" build/ts_dupcase.err; then echo "typeswitch-reject-probe: FAIL (duplicate case type: wrong/missing diagnostic)"; cat build/ts_dupcase.err; exit 1; fi
	@$(COMPILER) -o build/ts_multidefault build/ts_multidefault.goo > build/ts_multidefault.out 2> build/ts_multidefault.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "typeswitch-reject-probe: FAIL (multiple defaults: compiled rc=0)"; exit 1; fi; \
	if [ -x build/ts_multidefault ]; then echo "typeswitch-reject-probe: FAIL (multiple defaults: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "multiple defaults" build/ts_multidefault.err; then echo "typeswitch-reject-probe: FAIL (multiple defaults: wrong/missing diagnostic)"; cat build/ts_multidefault.err; exit 1; fi
	@echo "typeswitch-reject-probe: PASS"

# Papercut batch Task 3 (if-init guard): `if init; cond { }` desugars to a
# wrapping block `{ init; if cond {...} }` (parser.y if_stmt) so the init
# var's scope is naturally bounded to that wrapper — it must NOT be visible
# after the if, matching Go's scoping rule. Compile-must-fail.
if-init-scope-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== if-init-scope-reject-probe: init var out of scope after if ==="
	@printf 'package main\nimport "fmt"\nfunc main(){\n\tif x := 1; x > 0 {\n\t\t_ = x\n\t}\n\tfmt.Println(x)\n}\n' > build/ifscope.goo
	@if $(COMPILER) -o build/ifscope build/ifscope.goo 2>build/ifscope.err; then \
	  echo "if-init-scope-reject-probe: FAIL (x leaked past if)"; exit 1; \
	else echo "if-init-scope-reject-probe: PASS"; fi

# Closures Task 2 fix: capturing a LOOP VARIABLE in a closure must be a clean
# checker error. Modern Go (1.22+) ACCEPTS examples/loopcapture_reject.goo and
# prints 3 (per-iteration loopvar semantics); Goo's one-slot-per-declaration
# promotion model would silently compute the pre-1.22 shared-slot answer (9),
# so it rejects instead (the #101 reject-rather-than-silently-deviate
# precedent; per-iteration slots are the recorded follow-up). The `i := i`
# body-local-copy workaround is the positive control in closure_probe.goo
# (golden suite), expected 3.
loopcapture-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== loopcapture-reject-probe: closure capturing a loop variable must reject ==="
	@rm -f build/loopcapture_reject
	@$(COMPILER) -o build/loopcapture_reject examples/loopcapture_reject.goo > build/loopcapture_reject.out 2> build/loopcapture_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "loopcapture-reject-probe: FAIL (compiled rc=0 — would print 9 where go1.22+ prints 3)"; exit 1; fi; \
	if [ -x build/loopcapture_reject ]; then echo "loopcapture-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if grep -qiE "Module verification failed|LLVM ERROR" build/loopcapture_reject.err; then echo "loopcapture-reject-probe: FAIL (invalid IR reached the LLVM verifier instead of a clean rejection)"; cat build/loopcapture_reject.err; exit 1; fi; \
	if ! grep -q "cannot capture loop variable" build/loopcapture_reject.err; then echo "loopcapture-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/loopcapture_reject.err; exit 1; fi; \
	echo "loopcapture-reject-probe: PASS (rejected rc=$$rc)"

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

# port-unblockers #1: goostd resolution (GOOROOT) must be cwd-independent
# too, not just the runtime archive (cwd-link-probe covers that half).
# Compiled+run from build/oot (cwd != repo root) with an `import "strings"`
# program: pass 1 unsets GOOROOT so resolution must fall through to the
# dev-tree exe-relative branch (<exe-dir>/../goostd, mirroring how
# cwd-link-probe's binary resolves lib/libgoo_runtime.a); pass 2 pins
# GOOROOT explicitly to the repo root (the directory containing goostd/)
# to prove the env-var override in goo_gooroot_dir()'s precedence works
# standalone. See src/package/import_resolver.c: goo_gooroot_dir().
outoftree-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build/oot
	@echo "=== outoftree-probe: goostd (import \"strings\") resolves from a non-repo-root cwd ==="
	@printf 'package main\n\nimport "fmt"\nimport "strings"\n\nfunc main() {\n\tparts := strings.Split("a,b,c", ",")\n\tfmt.Println(len(parts))\n}\n' > build/oot/prog.goo
	@echo "--- pass 1: dev-tree exe-relative resolution (no GOOROOT) ---"
	@cd build/oot && GOOROOT="" "$(abspath $(COMPILER))" prog.goo -o prog.out 2>"$(CURDIR)/build/oot/prog.err"; \
	  rc=$$?; if [ $$rc -ne 0 ]; then echo "outoftree-probe: FAIL (exe-relative compile rc=$$rc)"; cat "$(CURDIR)/build/oot/prog.err"; exit 1; fi
	@out=$$(build/oot/prog.out); if [ "$$out" = "3" ]; then echo "outoftree-probe: PASS (exe-relative)"; else echo "outoftree-probe: FAIL (exe-relative got '$$out' want 3)"; exit 1; fi
	@echo "--- pass 2: GOOROOT env override (set to repo root) ---"
	@cd build/oot && GOOROOT="$(CURDIR)" "$(abspath $(COMPILER))" prog.goo -o prog_env.out 2>"$(CURDIR)/build/oot/prog_env.err"; \
	  rc=$$?; if [ $$rc -ne 0 ]; then echo "outoftree-probe: FAIL (GOOROOT-env compile rc=$$rc)"; cat "$(CURDIR)/build/oot/prog_env.err"; exit 1; fi
	@out=$$(build/oot/prog_env.out); if [ "$$out" = "3" ]; then echo "outoftree-probe: PASS (GOOROOT env)"; else echo "outoftree-probe: FAIL (GOOROOT env got '$$out' want 3)"; exit 1; fi

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

# Task 4: os.Args probe — dual-harness (see examples/osargs_probe.goo header).
# The golden suite (test-golden) already runs this same binary with NO
# args and asserts "true" (argv[0] always present). This target covers
# what a golden diff can't: scripts/run_golden.sh never passes args to
# the binary it runs, so the len(os.Args) > 1 branch (len==3 + Args[1]
# echo) needs a direct run with args, asserted here.
osargs-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/osargs_probe examples/osargs_probe.goo
	@actual=$$(./build/osargs_probe alpha beta); \
	expected=$$(printf 'true\n3\nalpha'); \
	if [ "$$actual" != "$$expected" ]; then \
	  echo "osargs-probe: FAIL (with-args run)"; \
	  echo "  expected: $$expected"; \
	  echo "  actual:   $$actual"; \
	  exit 1; \
	fi; \
	echo "osargs-probe: PASS (os.Args end-to-end, with args)"

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

# Full aggregate probe net — the single source of truth for `verify`'s
# dependency list (per `verification_gates.md`). Extend THIS list when a
# new gate is promoted in; do not add a second literal list anywhere else
# (verify-core below derives from it, so a second list would drift).
#
# ccomp-gated: v2-bootstrap-pilot depends on ccomp-build, which requires
# an opam CompCert switch. It is the only target in VERIFY_ALL_DEPS that
# does — verified via `make -n verify 2>&1 | grep -i ccomp`.
#
# comptime-probe joined the net once M11 closed (commits 605acaf,
# 47b5ca2, d7bc61c); m10-probe joined as M10-probe-gate-v2 once
# struct literals shipped (commit 1adab3c) — same promotion pattern.
VERIFY_ALL_DEPS := \
    baseline-probe \
    lvalue-probe \
    file-io-probe \
    pointer-probe \
    smoke-stdlib \
    v2-bootstrap-pilot \
    comptime-block-probe \
    comptime-probe \
    m10-probe \
    exit-code-probe \
    switch-probe \
    methods-probe \
    pointer-write-probe \
    new-probe \
    enum-probe \
    match-probe \
    append-probe \
    cap-probe \
    conv-probe \
    conv-reject-probe \
    charlit-probe \
    charlit-reject-probe \
    strindex-probe \
    strindex-reject-probe \
    hexesc-probe \
    hexesc-reject-probe \
    panic-abort-probe \
    bits-div-abort-probe \
    conststr-nul-probe \
    conststr-probe \
    rawstring-cr-probe \
    map-probe \
    int64-probe \
    commaok-probe \
    guard-probe \
    nullable-iflet-probe \
    nullable-nilcmp-probe \
    nullable-abi-probe \
    nullable-intret-probe \
    nullable-assign-probe \
    nullable-width-probe \
    erru-catch-probe \
    erru-error-probe \
    erru-abi-probe \
    chan-probe \
    chan-elem-probe \
    chan-padded-probe \
    chan-uint-probe \
    go-probe \
    unbuffered-probe \
    select-probe \
    block-scope-probe \
    escape-probe \
    escape-range-probe \
    mt-scheduler-stress \
    yield-stress \
    chan-mt-stress \
    fanin-stress \
    deadlock-probe \
    deadlock-goroutine-probe \
    default-thread-count-test \
    parallel-soak-probe \
    parallel-select-soak-probe \
    cwd-link-probe \
    outoftree-probe \
    break-probe \
    continue-probe \
    break-nested-probe \
    println-badtype-probe \
    error-arity-probe \
    return-type-erru-probe \
    erru-catch-type-reject-probe \
    iface-parse-probe \
    iface-satisfaction-probe \
    iface-recv-kind-probe \
    try-nonerru-probe \
    return-mismatch-probe \
    named-return-reject-probe \
    composite-literal-reject-probe \
    call-arity-probe \
    call-argtype-probe \
    pkg-argcheck-probe \
    forward-ref-probe \
    print-aggregate-probe \
    ptr-recv-nonaddr-probe \
    link-cleanup-probe \
    opt-differs-probe \
    link-spaces-probe \
    link-libs-probe \
    run-exit-probe \
    emit-llvm-probe \
    subcommand-probe \
    gpu-kernel-reject-probe \
    spec-conformance \
    blank-lines-probe \
    comment-lines-probe \
    slice-write-bounds-probe \
    array-bounds-probe \
    slice-expr-bounds-probe \
    const-array-bounds-probe \
    nonconst-arraylen-reject-probe \
    comptime-value-reject-probe \
    comptime-value-reject-matrix \
    comptime-generic-compose-ir-pin \
    lanes-monomorphize-ir-pin \
    selectsend-reject-probe \
    globalcall-init-probe \
    floatint-reject-probe \
    constmod-reject-probe \
    baremod-reject-probe \
    constint8-reject-probe \
    constuint8-reject-probe \
    constf32-reject-probe \
    constf64-reject-probe \
    constconv-reject-probe \
    consttrunc-reject-probe \
    constelem-reject-probe \
    constnul-reject-probe \
    floatmod-reject-probe \
    cascade-reject-probe \
    cascade-binop-reject-probe \
    multivar-reject-probe \
    variadic-reject-probe \
    variadic-range-reject-probe \
    funcnil-abort-probe \
    funcval-nilcmp-probe \
    map-nilfunc-abort-probe \
    loopcapture-reject-probe \
    osargs-probe \
    embed-iface-reject-probe \
    embed-dup-reject-probe \
    embed-badtype-reject-probe \
    embed-enum-reject-probe \
    embed-ambiguous-reject-probe \
    embed-literal-reject-probe \
    map-addr-reject-probe \
    mapkey-reject-probe \
    struct-map-key-reject-probe \
    iface-map-key-uncomparable-probe \
    bytesconv-reject-probe \
    spread-reject-probe \
    copy-reject-probe \
    typeassert-reject-probe \
    typeswitch-reject-probe \
    if-init-scope-reject-probe \
    blank-read-reject-probe \
    const-index-reject-probe \
    rtti-assert-panic-probe \
    iface-assert-dynname-probe \
    iface-target-assert-abort-probe \
    generics-reject-probe \
    generics-bound-reject-probe \
    asi-hardening-probe \
    asi-gocompat-probe \
    param-escape-test \
    block-escape-test \
    arena-routing-test \
    arena-free-probe \
    arena-valgrind-probe \
    arena-rss-probe \
    test-golden \
    test-golden-o2 \
    test-golden-reject \
    spmd-bench-probe \
    stencil-race-runbook-probe \
    stencil-parallel-probe \
    goostd-resolver-probe \
    reldir-import-probe \
    readline-probe \
    stdlib-smoke-coverage

# verify-core = VERIFY_ALL_DEPS minus the ccomp-gated set. This is the
# authoritative ccomp-free gate: green on any machine, no CompCert / opam
# switch required. Use it for pre-push everywhere; use `verify` (below)
# only where the CompCert bootstrap pilot toolchain is set up.
VERIFY_CORE_DEPS := $(filter-out v2-bootstrap-pilot,$(VERIFY_ALL_DEPS))

.PHONY: verify verify-core

# Exits non-zero on any failure. Use this on cross-cutting changes;
# use individual targets when iterating on a specific area.
verify-core: $(VERIFY_CORE_DEPS)
	@echo ""
	@echo "verify-core: ALL GREEN GATES PASSED (ccomp-free)"

verify: $(VERIFY_ALL_DEPS)
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
# Was []int (slice) until P3.8 (2026-07-10) added TYPE_SLICE/TYPE_ARRAY arms
# to codegen_emit_fmt_value, making that a green compile — map stays
# genuinely unsupported (no TYPE_MAP arm), so it keeps this probe honest.
println-badtype-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== println-badtype-probe: printing an unsupported type (map) fails cleanly ==="
	@printf 'package main\nimport "fmt"\nfunc main() { s := map[string]int{"a": 1}; fmt.Println(s) }\n' > build/println_bad.goo
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

# Function generics Task 4: generic-declaration invariants. An un-inferable
# type param (never appears in a parameter type) is rejected; a non-interface
# type constraint is rejected (Tier B: interface bounds incl. named
# interfaces are legal — see generics-bound-reject-probe for the
# interface-vs-non-interface boundary); arithmetic on an opaque type param is
# rejected. No invalid IR may escape in any case.
generics-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== generics-reject-probe: generic function declaration invariants ==="
	@printf 'package main\nfunc Zero[T any]() T { var z T\n return z }\nfunc main() {}\n' > build/gen_uninferable.goo
	@printf 'package main\nfunc F[T int](x T) T { return x }\nfunc main() {}\n' > build/gen_badconstraint.goo
	@printf 'package main\nfunc Add[T any](x T) T { return x + 1 }\nfunc main() {}\n' > build/gen_opaque_op.goo
	@printf 'package main\nfunc Pair[T any](a T, b T) T { return a }\nfunc main() { _ = Pair(1, "x") }\n' > build/gen_conflict.goo
	@"$(COMPILER)" build/gen_uninferable.goo -o build/gen_uninferable.out 2>build/gen_uninferable.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-reject-probe: FAIL (un-inferable type param compiled)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/gen_uninferable.err; then echo "generics-reject-probe: FAIL (invalid IR)"; cat build/gen_uninferable.err; exit 1; fi; \
	  if ! grep -qiE "never used in a parameter|cannot be inferred" build/gen_uninferable.err; then echo "generics-reject-probe: FAIL (no un-inferable diagnostic)"; cat build/gen_uninferable.err; exit 1; fi
	@"$(COMPILER)" build/gen_badconstraint.goo -o build/gen_badconstraint.out 2>build/gen_badconstraint.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-reject-probe: FAIL (non-interface constraint compiled)"; exit 1; fi; \
	  if ! grep -qiE "constraint must be an interface|constraint" build/gen_badconstraint.err; then echo "generics-reject-probe: FAIL (no constraint diagnostic)"; cat build/gen_badconstraint.err; exit 1; fi
	@"$(COMPILER)" build/gen_opaque_op.goo -o build/gen_opaque_op.out 2>build/gen_opaque_op.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-reject-probe: FAIL (arithmetic on opaque T compiled)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/gen_opaque_op.err; then echo "generics-reject-probe: FAIL (invalid IR)"; cat build/gen_opaque_op.err; exit 1; fi
	@"$(COMPILER)" build/gen_conflict.goo -o build/gen_conflict.out 2>build/gen_conflict.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-reject-probe: FAIL (Pair(1, \"x\") — conflicting T binding — compiled)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/gen_conflict.err; then echo "generics-reject-probe: FAIL (invalid IR)"; cat build/gen_conflict.err; exit 1; fi; \
	  if ! grep -qi "conflicting types" build/gen_conflict.err; then echo "generics-reject-probe: FAIL (no conflicting-types diagnostic)"; cat build/gen_conflict.err; exit 1; fi
	@echo "generics-reject-probe: PASS"

# Function generics Tier B Task 1: a type constraint must be an interface
# (named interface, or `any` the 0-method interface). A non-interface bound
# (e.g. `[T int]`) is rejected here with a dedicated diagnostic.
generics-bound-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== generics-bound-reject-probe: interface-constraint bounds ==="
	@printf 'package main\nfunc F[T int](x T) T { return x }\nfunc main() {}\n' > build/genb_noniface.goo
	@"$(COMPILER)" build/genb_noniface.goo -o build/genb_noniface.out 2>build/genb_noniface.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-bound-reject-probe: FAIL (non-interface bound compiled)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/genb_noniface.err; then echo "generics-bound-reject-probe: FAIL (invalid IR)"; cat build/genb_noniface.err; exit 1; fi; \
	  if ! grep -qiE "constraint must be an interface" build/genb_noniface.err; then echo "generics-bound-reject-probe: FAIL (no constraint diagnostic)"; cat build/genb_noniface.err; exit 1; fi
	@printf 'package main\ntype Stringer interface { String() string }\ntype Pt struct { x int }\nfunc Show[T Stringer](v T) string { return v.String() }\nfunc main() { _ = Show(Pt{x: 1}) }\n' > build/genb_notsat.goo
	@"$(COMPILER)" build/genb_notsat.goo -o build/genb_notsat.out 2>build/genb_notsat.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-bound-reject-probe: FAIL (non-satisfying arg compiled)"; exit 1; fi; \
	  if ! grep -qiE "does not implement" build/genb_notsat.err; then echo "generics-bound-reject-probe: FAIL (no satisfaction diagnostic)"; cat build/genb_notsat.err; exit 1; fi
	@printf 'package main\ntype Stringer interface { String() string }\ntype Pt struct { x int }\nfunc (p Pt) String() string { return "p" }\nfunc Bad[T Stringer](v T) string { return v.Nope() }\nfunc main() { _ = Bad(Pt{x: 1}) }\n' > build/genb_nomethod.goo
	@"$(COMPILER)" build/genb_nomethod.goo -o build/genb_nomethod.out 2>build/genb_nomethod.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-bound-reject-probe: FAIL (unknown bound method compiled)"; exit 1; fi; \
	  if ! grep -qiE "has no method" build/genb_nomethod.err; then echo "generics-bound-reject-probe: FAIL (no unknown-method diagnostic)"; cat build/genb_nomethod.err; exit 1; fi
	@printf 'package main\ntype Stringer interface { String() string }\nfunc Op[T Stringer](a T, b T) T { return a + b }\nfunc main() {}\n' > build/genb_op.goo
	@"$(COMPILER)" build/genb_op.goo -o build/genb_op.out 2>build/genb_op.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-bound-reject-probe: FAIL (operator on bounded T compiled)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/genb_op.err; then echo "generics-bound-reject-probe: FAIL (invalid IR)"; cat build/genb_op.err; exit 1; fi
	@# Transitive bound with a same-named but DIFFERENT-signature method: Outer's
	@# bound A requires M() int, Inner's bound B requires M(int) int. The abstract
	@# transitive check (interface_covers) must compare signatures, not just names,
	@# and reject at type-check — NOT let a wrong-arity call reach the LLVM verifier.
	@printf 'package main\ntype A interface { M() int }\ntype B interface { M(x int) int }\ntype C struct { n int }\nfunc (c C) M() int { return c.n }\nfunc Inner[U B](u U) int { return u.M(5) }\nfunc Outer[T A](t T) int { return Inner(t) + 1 }\nfunc main() { _ = Outer(C{n: 3}) }\n' > build/genb_sigmismatch.goo
	@"$(COMPILER)" build/genb_sigmismatch.goo -o build/genb_sigmismatch.out 2>build/genb_sigmismatch.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-bound-reject-probe: FAIL (transitive signature-mismatch bound compiled)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/genb_sigmismatch.err; then echo "generics-bound-reject-probe: FAIL (invalid IR — mismatch reached codegen instead of type-check)"; cat build/genb_sigmismatch.err; exit 1; fi; \
	  if ! grep -qiE "does not satisfy" build/genb_sigmismatch.err; then echo "generics-bound-reject-probe: FAIL (no satisfaction diagnostic)"; cat build/genb_sigmismatch.err; exit 1; fi
	@echo "generics-bound-reject-probe: PASS"

# ASI greedy-join hardening: a value-ending token must NOT silently absorb a
# following `(`, `[`, or `.` across a newline. These three cases cannot be
# run-and-diff goldens (the un-joined line 2 — `(5)`, `[0]`, `.x` — is not a
# valid standalone statement), so each asserts the implementation-agnostic
# invariant: the program must NOT compile-and-print the joined-WRONG value, and
# must never reach the LLVM verifier with a crash. A clean front-end rejection
# or a correct reparse both pass.
asi-hardening-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== asi-hardening-probe: value-ending token must not join ( [ . across a newline ==="
	@# Case 1: `g := id` <nl> `(5)` — pre-fix joins to `g := id(5)` -> prints 5.
	@printf 'package main\nimport "fmt"\nfunc id(n int) int { return n }\nfunc main() {\n\tg := id\n\t(5)\n\tfmt.Println(g)\n}\n' > build/asi_call.goo
	@"$(COMPILER)" build/asi_call.goo -o build/asi_call.out 2>build/asi_call.err; rc=$$?; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/asi_call.err; then echo "asi-hardening-probe: FAIL (call case reached verifier)"; cat build/asi_call.err; exit 1; fi; \
	  if [ $$rc -eq 0 ] && [ "$$(./build/asi_call.out 2>/dev/null)" = "5" ]; then echo "asi-hardening-probe: FAIL (call case silently joined -> printed 5)"; exit 1; fi
	@# Case 2: `b := a` <nl> `[0]` — pre-fix joins to `b := a[0]` -> prints 10.
	@printf 'package main\nimport "fmt"\nfunc main() {\n\ta := []int{10, 20, 30}\n\tb := a\n\t[0]\n\tfmt.Println(b)\n}\n' > build/asi_index.goo
	@"$(COMPILER)" build/asi_index.goo -o build/asi_index.out 2>build/asi_index.err; rc=$$?; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/asi_index.err; then echo "asi-hardening-probe: FAIL (index case reached verifier)"; cat build/asi_index.err; exit 1; fi; \
	  if [ $$rc -eq 0 ] && [ "$$(./build/asi_index.out 2>/dev/null)" = "10" ]; then echo "asi-hardening-probe: FAIL (index case silently joined -> printed 10)"; exit 1; fi
	@# Case 3: `q := p` <nl> `.x` — pre-fix joins to `q := p.x` -> prints 7.
	@printf 'package main\nimport "fmt"\ntype P struct { x int; y int }\nfunc main() {\n\tp := P{x: 7, y: 9}\n\tq := p\n\t.x\n\tfmt.Println(q)\n}\n' > build/asi_sel.goo
	@"$(COMPILER)" build/asi_sel.goo -o build/asi_sel.out 2>build/asi_sel.err; rc=$$?; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/asi_sel.err; then echo "asi-hardening-probe: FAIL (selector case reached verifier)"; cat build/asi_sel.err; exit 1; fi; \
	  if [ $$rc -eq 0 ] && [ "$$(./build/asi_sel.out 2>/dev/null)" = "7" ]; then echo "asi-hardening-probe: FAIL (selector case silently joined -> printed 7)"; exit 1; fi
	@echo "asi-hardening-probe: PASS"

# Positive Go-compat ASI matrix: the flip side of asi-hardening-probe. That
# probe pins hazards (ASI must NOT silently join across a newline);
# this one pins the matrix of ordinary, ASI-dependent Go forms that MUST
# keep parsing AND running correctly — one case per gofmt-syntax task's
# positive behavior, each compiled, run, and diffed against asserted
# stdout. Every case here was individually verified against today's
# compiler before being added (see task-7 report for the verification
# log); nothing in this matrix is aspirational.
.PHONY: asi-gocompat-probe
asi-gocompat-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== asi-gocompat-probe: positive Go-compat ASI matrix ==="
	@# Case 1: no-semicolon statements, multi-statement body, newline-separated.
	@printf 'package main\nimport "fmt"\nfunc main() {\n\ta := 1\n\tb := 2\n\tc := a + b\n\tfmt.Println(c)\n}\n' > build/gc_nosemi.goo
	@"$(COMPILER)" build/gc_nosemi.goo -o build/gc_nosemi.out 2>build/gc_nosemi.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "asi-gocompat-probe: FAIL (no-semicolon statements: compile failed rc=$$rc)"; cat build/gc_nosemi.err; exit 1; fi
	@./build/gc_nosemi.out > build/gc_nosemi.actual.txt
	@printf '3\n' > build/gc_nosemi.expected.txt
	@if ! diff -u build/gc_nosemi.expected.txt build/gc_nosemi.actual.txt; then echo "asi-gocompat-probe: FAIL (no-semicolon statements: stdout mismatch)"; exit 1; fi
	@# Case 2: bare `return` (void func, mid-function); bare `break`/`continue` in a loop.
	@printf 'package main\nimport "fmt"\nfunc early(n int) {\n\tif n < 0 {\n\t\tfmt.Println("neg")\n\t\treturn\n\t}\n\tfmt.Println("nonneg")\n}\nfunc main() {\n\tearly(-1)\n\tearly(1)\n\tsum := 0\n\tfor i := 0; i < 10; i++ {\n\t\tif i == 5 {\n\t\t\tbreak\n\t\t}\n\t\tif i%%2 == 0 {\n\t\t\tcontinue\n\t\t}\n\t\tsum = sum + i\n\t}\n\tfmt.Println(sum)\n}\n' > build/gc_barejump.goo
	@"$(COMPILER)" build/gc_barejump.goo -o build/gc_barejump.out 2>build/gc_barejump.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "asi-gocompat-probe: FAIL (bare return/break/continue: compile failed rc=$$rc)"; cat build/gc_barejump.err; exit 1; fi
	@./build/gc_barejump.out > build/gc_barejump.actual.txt
	@printf 'neg\nnonneg\n4\n' > build/gc_barejump.expected.txt
	@if ! diff -u build/gc_barejump.expected.txt build/gc_barejump.actual.txt; then echo "asi-gocompat-probe: FAIL (bare return/break/continue: stdout mismatch)"; exit 1; fi
	@# Case 3: `p := &x` <nl> `*p = v` — the continuation-op hazard's positive side.
	@printf 'package main\nimport "fmt"\nfunc main() {\n\tx := 1\n\tp := &x\n\t*p = 42\n\tfmt.Println(x)\n}\n' > build/gc_ptrderef.goo
	@"$(COMPILER)" build/gc_ptrderef.goo -o build/gc_ptrderef.out 2>build/gc_ptrderef.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "asi-gocompat-probe: FAIL (p := &x / *p = v: compile failed rc=$$rc)"; cat build/gc_ptrderef.err; exit 1; fi
	@./build/gc_ptrderef.out > build/gc_ptrderef.actual.txt
	@printf '42\n' > build/gc_ptrderef.expected.txt
	@if ! diff -u build/gc_ptrderef.expected.txt build/gc_ptrderef.actual.txt; then echo "asi-gocompat-probe: FAIL (p := &x / *p = v: stdout mismatch)"; exit 1; fi
	@# Case 4: trailing-op continuation — `a := 1 +` <nl> `2` must NOT be ASI-split.
	@printf 'package main\nimport "fmt"\nfunc main() {\n\ta := 1 +\n\t\t2\n\tfmt.Println(a)\n}\n' > build/gc_trailop.goo
	@"$(COMPILER)" build/gc_trailop.goo -o build/gc_trailop.out 2>build/gc_trailop.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "asi-gocompat-probe: FAIL (trailing-op continuation: compile failed rc=$$rc)"; cat build/gc_trailop.err; exit 1; fi
	@./build/gc_trailop.out > build/gc_trailop.actual.txt
	@printf '3\n' > build/gc_trailop.expected.txt
	@if ! diff -u build/gc_trailop.expected.txt build/gc_trailop.actual.txt; then echo "asi-gocompat-probe: FAIL (trailing-op continuation: stdout mismatch)"; exit 1; fi
	@# Case 5: dot continuation — `fmt.` <nl> `Println(...)` must NOT be ASI-split.
	@printf 'package main\nimport "fmt"\nfunc main() {\n\tfmt.\n\t\tPrintln("dotcontinue")\n}\n' > build/gc_dotcont.goo
	@"$(COMPILER)" build/gc_dotcont.goo -o build/gc_dotcont.out 2>build/gc_dotcont.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "asi-gocompat-probe: FAIL (dot continuation: compile failed rc=$$rc)"; cat build/gc_dotcont.err; exit 1; fi
	@./build/gc_dotcont.out > build/gc_dotcont.actual.txt
	@printf 'dotcontinue\n' > build/gc_dotcont.expected.txt
	@if ! diff -u build/gc_dotcont.expected.txt build/gc_dotcont.actual.txt; then echo "asi-gocompat-probe: FAIL (dot continuation: stdout mismatch)"; exit 1; fi
	@# Case 6: struct embedding on its own line (struct-body ASI, gofmt-syntax A §4).
	@printf 'package main\nimport "fmt"\ntype Base struct {\n\tid int\n}\ntype Derived struct {\n\tBase\n\tname string\n}\nfunc main() {\n\td := Derived{Base: Base{id: 7}, name: "d"}\n\tfmt.Println(d.id, d.name)\n}\n' > build/gc_embed.goo
	@"$(COMPILER)" build/gc_embed.goo -o build/gc_embed.out 2>build/gc_embed.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "asi-gocompat-probe: FAIL (struct embedding own-line: compile failed rc=$$rc)"; cat build/gc_embed.err; exit 1; fi
	@./build/gc_embed.out > build/gc_embed.actual.txt
	@printf '7 d\n' > build/gc_embed.expected.txt
	@if ! diff -u build/gc_embed.expected.txt build/gc_embed.actual.txt; then echo "asi-gocompat-probe: FAIL (struct embedding own-line: stdout mismatch)"; exit 1; fi
	@# Case 7: interface method specs on their own lines — multi-method interface,
	@# void method first (Task 1's behavior).
	@printf 'package main\nimport "fmt"\ntype Doer interface {\n\tDo()\n\tValue() int\n}\ntype Impl struct {\n\tn int\n}\nfunc (i Impl) Do() {\n\tfmt.Println("doing")\n}\nfunc (i Impl) Value() int {\n\treturn i.n\n}\nfunc main() {\n\tvar d Doer = Impl{n: 9}\n\td.Do()\n\tfmt.Println(d.Value())\n}\n' > build/gc_ifacespec.goo
	@"$(COMPILER)" build/gc_ifacespec.goo -o build/gc_ifacespec.out 2>build/gc_ifacespec.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "asi-gocompat-probe: FAIL (interface method specs own-line: compile failed rc=$$rc)"; cat build/gc_ifacespec.err; exit 1; fi
	@./build/gc_ifacespec.out > build/gc_ifacespec.actual.txt
	@printf 'doing\n9\n' > build/gc_ifacespec.expected.txt
	@if ! diff -u build/gc_ifacespec.expected.txt build/gc_ifacespec.actual.txt; then echo "asi-gocompat-probe: FAIL (interface method specs own-line: stdout mismatch)"; exit 1; fi
	@# Case 8: receive-at-line-start (Task 6's behavior) — `x := 2` <nl> `<-ch` is
	@# a standalone receive statement, not a continuation into a send expression.
	@printf 'package main\nimport "fmt"\nfunc main() {\n\tch := make(chan int, 1)\n\tgo func() { ch <- 1 }()\n\tx := 2\n\t<-ch\n\tfmt.Println(x)\n}\n' > build/gc_recv.goo
	@"$(COMPILER)" build/gc_recv.goo -o build/gc_recv.out 2>build/gc_recv.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "asi-gocompat-probe: FAIL (receive-at-line-start: compile failed rc=$$rc)"; cat build/gc_recv.err; exit 1; fi
	@./build/gc_recv.out > build/gc_recv.actual.txt
	@printf '2\n' > build/gc_recv.expected.txt
	@if ! diff -u build/gc_recv.expected.txt build/gc_recv.actual.txt; then echo "asi-gocompat-probe: FAIL (receive-at-line-start: stdout mismatch)"; exit 1; fi
	@# Case 9: comment+ASI interplay — `x = 10 // c` <nl> `*p = 7`; the trailing
	@# line comment must not suppress ASI before the next statement.
	@printf 'package main\nimport "fmt"\nfunc main() {\n\tx := 1\n\tp := &x\n\tx = 10 // c\n\t*p = 7\n\tfmt.Println(x)\n}\n' > build/gc_commentasi.goo
	@"$(COMPILER)" build/gc_commentasi.goo -o build/gc_commentasi.out 2>build/gc_commentasi.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "asi-gocompat-probe: FAIL (comment+ASI interplay: compile failed rc=$$rc)"; cat build/gc_commentasi.err; exit 1; fi
	@./build/gc_commentasi.out > build/gc_commentasi.actual.txt
	@printf '7\n' > build/gc_commentasi.expected.txt
	@if ! diff -u build/gc_commentasi.expected.txt build/gc_commentasi.actual.txt; then echo "asi-gocompat-probe: FAIL (comment+ASI interplay: stdout mismatch)"; exit 1; fi
	@# Case 10: raw-string line followed by a new statement (Task 5 interaction).
	@printf 'package main\nimport "fmt"\nfunc main() {\n\ts := `raw string`\n\tn := len(s)\n\tfmt.Println(s, n)\n}\n' > build/gc_rawstr.goo
	@"$(COMPILER)" build/gc_rawstr.goo -o build/gc_rawstr.out 2>build/gc_rawstr.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "asi-gocompat-probe: FAIL (raw-string then new statement: compile failed rc=$$rc)"; cat build/gc_rawstr.err; exit 1; fi
	@./build/gc_rawstr.out > build/gc_rawstr.actual.txt
	@printf 'raw string 10\n' > build/gc_rawstr.expected.txt
	@if ! diff -u build/gc_rawstr.expected.txt build/gc_rawstr.actual.txt; then echo "asi-gocompat-probe: FAIL (raw-string then new statement: stdout mismatch)"; exit 1; fi
	@# Case 11: receive-after-brace (Task 6 REGRESSION fix) — `for { ... }` <nl>
	@# `<-ch` is the standard Go "loop, then join on channel" pattern. Part 2.5's
	@# ASI guard inserts a `;` after the loop's closing `}` (it is a value-ending
	@# token); `statement:` must tolerate that trailing SEMICOLON on for_stmt (and
	@# if/switch/select/block) or the join is a loud parse error. See
	@# examples/asi_recv_after_for_probe.goo / asi_recv_after_if_probe.goo for the
	@# single-fixture-per-shape goldens; this case pins the same hazard inline.
	@printf 'package main\nimport "fmt"\nfunc main() {\n\tch := make(chan int, 1)\n\tgo func() { ch <- 1 }()\n\tfor i := 0; i < 2; i++ {\n\t\tfmt.Println(i)\n\t}\n\t<-ch\n\tfmt.Println("joined")\n}\n' > build/gc_recvafterbrace.goo
	@"$(COMPILER)" build/gc_recvafterbrace.goo -o build/gc_recvafterbrace.out 2>build/gc_recvafterbrace.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "asi-gocompat-probe: FAIL (receive-after-brace: compile failed rc=$$rc)"; cat build/gc_recvafterbrace.err; exit 1; fi
	@./build/gc_recvafterbrace.out > build/gc_recvafterbrace.actual.txt
	@printf '0\n1\njoined\n' > build/gc_recvafterbrace.expected.txt
	@if ! diff -u build/gc_recvafterbrace.expected.txt build/gc_recvafterbrace.actual.txt; then echo "asi-gocompat-probe: FAIL (receive-after-brace: stdout mismatch)"; exit 1; fi
	@echo "asi-gocompat-probe: PASS"

# Receiver-kind soundness (Go method-set rule): a pointer-receiver method is in
# the method set of *T only, not value T. So a VALUE concrete must NOT satisfy an
# interface whose method has a pointer receiver — reject it here (not an
# LLVM-verifier crash). Embedding composes: a value outer embedding a VALUE field
# whose method has a pointer receiver is likewise rejected (the promoted method
# needs an addressable owner). Over-rejection guards: a POINTER concrete, and a
# value outer embedding a value field with a VALUE-receiver method, must compile.
iface-recv-kind-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== iface-recv-kind-probe: value concrete must not satisfy a pointer-receiver interface ==="
	@printf 'package main\ntype I interface { M() int }\ntype T struct { x int }\nfunc (t *T) M() int { return t.x }\nfunc main() { var i I = T{x: 1}\n _ = i }\n' > build/recvkind_direct.goo
	@printf 'package main\ntype I interface { M() int }\ntype E struct { x int }\nfunc (e *E) M() int { return e.x }\ntype S struct {\n\tE\n}\nfunc main() { var i I = S{E: E{x: 1}}\n _ = i }\n' > build/recvkind_embed.goo
	@printf 'package main\ntype I interface { M() int }\ntype T struct { x int }\nfunc (t *T) M() int { return t.x }\nfunc main() { var i I = &T{x: 1}\n _ = i }\n' > build/recvkind_ptr_ok.goo
	@"$(COMPILER)" build/recvkind_direct.goo -o build/recvkind_direct.out 2>build/recvkind_direct.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "iface-recv-kind-probe: FAIL (value satisfied pointer-receiver interface)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/recvkind_direct.err; then echo "iface-recv-kind-probe: FAIL (invalid IR)"; cat build/recvkind_direct.err; exit 1; fi; \
	  if ! grep -qiE "does not implement" build/recvkind_direct.err; then echo "iface-recv-kind-probe: FAIL (no satisfaction diagnostic)"; cat build/recvkind_direct.err; exit 1; fi
	@"$(COMPILER)" build/recvkind_embed.goo -o build/recvkind_embed.out 2>build/recvkind_embed.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "iface-recv-kind-probe: FAIL (value outer w/ value-embedded pointer-receiver method satisfied interface)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/recvkind_embed.err; then echo "iface-recv-kind-probe: FAIL (invalid IR)"; cat build/recvkind_embed.err; exit 1; fi; \
	  if ! grep -qiE "does not implement" build/recvkind_embed.err; then echo "iface-recv-kind-probe: FAIL (no satisfaction diagnostic)"; cat build/recvkind_embed.err; exit 1; fi
	@"$(COMPILER)" build/recvkind_ptr_ok.goo -o build/recvkind_ptr_ok.out 2>build/recvkind_ptr_ok.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "iface-recv-kind-probe: FAIL (pointer concrete over-rejected)"; cat build/recvkind_ptr_ok.err; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/recvkind_ptr_ok.err; then echo "iface-recv-kind-probe: FAIL (pointer concrete reached the verifier)"; cat build/recvkind_ptr_ok.err; exit 1; fi
	@echo "iface-recv-kind-probe: PASS"

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
	  if ! grep -qiE "try requires the enclosing function to return an error union" build/try_nonerru_int.err; then echo "try-nonerru-probe: FAIL (no clean try-context diagnostic)"; cat build/try_nonerru_int.err; exit 1; fi
	@"$(COMPILER)" build/try_nonerru_void.goo -o build/try_nonerru_void.out 2>build/try_nonerru_void.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "try-nonerru-probe: FAIL (try in void func compiled — expected a type error)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/try_nonerru_void.err; then echo "try-nonerru-probe: FAIL (void-func try reached verifier)"; cat build/try_nonerru_void.err; exit 1; fi; \
	  if ! grep -qiE "try requires the enclosing function to return an error union" build/try_nonerru_void.err; then echo "try-nonerru-probe: FAIL (no clean diagnostic for void-func try)"; cat build/try_nonerru_void.err; exit 1; fi
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

# Embedding: interface embedding is deferred — must reject cleanly, not crash.
embed-iface-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== embed-iface-reject-probe: embedded interface must reject ==="
	@printf 'package main\ntype I interface { M() int }\ntype S struct {\n\tI\n}\nfunc main(){ _ = S{} }\n' > build/embed_iface_reject.goo
	@rm -f build/embed_iface_reject
	@$(COMPILER) -o build/embed_iface_reject build/embed_iface_reject.goo > build/embed_iface_reject.out 2> build/embed_iface_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "embed-iface-reject-probe: FAIL (compiled rc=0)"; exit 1; fi; \
	if ! grep -q "embedded interface types are not yet supported" build/embed_iface_reject.err; then echo "embed-iface-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/embed_iface_reject.err; exit 1; fi; \
	echo "embed-iface-reject-probe: PASS (rejected rc=$$rc)"

# Embedding: duplicate member names (Base twice, or Base + field Base) reject.
embed-dup-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== embed-dup-reject-probe: duplicate embedded name must reject ==="
	@printf 'package main\ntype Base struct { N int }\ntype S struct {\n\tBase\n\t*Base\n}\nfunc main(){ _ = S{} }\n' > build/embed_dup_reject.goo
	@rm -f build/embed_dup_reject
	@$(COMPILER) -o build/embed_dup_reject build/embed_dup_reject.goo > build/embed_dup_reject.out 2> build/embed_dup_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "embed-dup-reject-probe: FAIL (compiled rc=0)"; exit 1; fi; \
	if ! grep -q "duplicate field name 'Base'" build/embed_dup_reject.err; then echo "embed-dup-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/embed_dup_reject.err; exit 1; fi; \
	echo "embed-dup-reject-probe: PASS (rejected rc=$$rc)"

# Embedding: only named types / pointers to named types can be embedded.
embed-badtype-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== embed-badtype-reject-probe: value-recursive embedding must reject, not hang ==="
	@printf 'package main\ntype A struct {\n\tB\n}\ntype B struct {\n\tA\n}\nfunc main(){ _ = A{} }\n' > build/embed_badtype_reject.goo
	@rm -f build/embed_badtype_reject
	@timeout 10 $(COMPILER) -o build/embed_badtype_reject build/embed_badtype_reject.goo > build/embed_badtype_reject.out 2> build/embed_badtype_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "embed-badtype-reject-probe: FAIL (compiled rc=0)"; exit 1; fi; \
	if [ $$rc -eq 124 ]; then echo "embed-badtype-reject-probe: FAIL (compiler hung on recursive embedding)"; exit 1; fi; \
	echo "embed-badtype-reject-probe: PASS (rejected rc=$$rc)"

# Embedding: enum variant bodies share struct_field_list; embedding there is out of scope.
embed-enum-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== embed-enum-reject-probe: embedded field in enum variant must reject ==="
	@printf 'package main\ntype Base struct { N int }\ntype E enum { V{Base;} }\nfunc main(){ }\n' > build/embed_enum_reject.goo
	@rm -f build/embed_enum_reject
	@$(COMPILER) -o build/embed_enum_reject build/embed_enum_reject.goo > build/embed_enum_reject.out 2> build/embed_enum_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "embed-enum-reject-probe: FAIL (compiled rc=0)"; exit 1; fi; \
	if ! grep -q "embedded fields are not supported in enum variants" build/embed_enum_reject.err; then echo "embed-enum-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/embed_enum_reject.err; exit 1; fi; \
	echo "embed-enum-reject-probe: PASS (rejected rc=$$rc)"

# Embedding: same-depth collision is an error only when the BARE name is used.
embed-ambiguous-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== embed-ambiguous-reject-probe: ambiguous promoted name must reject at use ==="
	@printf 'package main\nimport "fmt"\ntype A struct { X int }\ntype B struct { X int }\ntype S struct {\n\tA\n\tB\n}\nfunc main(){ s := S{}; s.A.X = 1; fmt.Println(s.X) }\n' > build/embed_ambig_reject.goo
	@rm -f build/embed_ambig_reject
	@$(COMPILER) -o build/embed_ambig_reject build/embed_ambig_reject.goo > build/embed_ambig_reject.out 2> build/embed_ambig_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "embed-ambiguous-reject-probe: FAIL (compiled rc=0 — ambiguous s.X accepted)"; exit 1; fi; \
	if ! grep -q "ambiguous selector 'X'" build/embed_ambig_reject.err; then echo "embed-ambiguous-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/embed_ambig_reject.err; exit 1; fi; \
	echo "embed-ambiguous-reject-probe: PASS (rejected rc=$$rc)"
	@echo "=== declaration alone must stay LEGAL (Go rule) ==="
	@printf 'package main\nimport "fmt"\ntype A struct { X int }\ntype B struct { X int }\ntype S struct {\n\tA\n\tB\n}\nfunc main(){ s := S{}; s.A.X = 4; fmt.Println(s.A.X) }\n' > build/embed_ambig_ok.goo
	@$(COMPILER) -o build/embed_ambig_ok build/embed_ambig_ok.goo 2> build/embed_ambig_ok.err || (echo "embed-ambiguous-reject-probe: FAIL (declaring ambiguous struct rejected — should be use-site only)"; cat build/embed_ambig_ok.err; exit 1)
	@out=$$(build/embed_ambig_ok); [ "$$out" = "4" ] || (echo "embed-ambiguous-reject-probe: FAIL (explicit path broken)"; exit 1)
	@echo "embed-ambiguous-reject-probe: PASS (explicit path fine)"

# Embedding: promoted names are NOT valid composite-literal keys (Go rule) —
# only the embedded type's own name is. Pins the literal checker's flat scan.
embed-literal-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== embed-literal-reject-probe: promoted name as literal key must reject ==="
	@printf 'package main\ntype Base struct { N int }\ntype S struct {\n\tBase\n}\nfunc main(){ _ = S{N: 1} }\n' > build/embed_lit_reject.goo
	@rm -f build/embed_lit_reject
	@$(COMPILER) -o build/embed_lit_reject build/embed_lit_reject.goo > build/embed_lit_reject.out 2> build/embed_lit_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "embed-literal-reject-probe: FAIL (compiled rc=0 — S{N:1} accepted)"; exit 1; fi; \
	echo "embed-literal-reject-probe: PASS (rejected rc=$$rc)"
	@echo "=== keyed embedded-type name must stay LEGAL ==="
	@printf 'package main\nimport "fmt"\ntype Base struct { N int }\ntype S struct {\n\tBase\n}\nfunc main(){ s := S{Base: Base{N: 7}}; fmt.Println(s.N) }\n' > build/embed_lit_ok.goo
	@$(COMPILER) -o build/embed_lit_ok build/embed_lit_ok.goo 2> build/embed_lit_ok.err || (echo "embed-literal-reject-probe: FAIL (keyed Base literal rejected)"; cat build/embed_lit_ok.err; exit 1)
	@out=$$(build/embed_lit_ok); [ "$$out" = "7" ] || (echo "embed-literal-reject-probe: FAIL (keyed literal wrong value: $$out)"; exit 1)
	@echo "embed-literal-reject-probe: PASS (keyed Base literal fine)"

# Generic map values: Go's map values are NOT addressable. &m[k] and partial
# writes through a STRUCT or ARRAY map value (m[k].F = v, m[k][i] = v) must
# reject at compile time — without the guard the lvalue path would silently
# mutate a private box nobody reads back. Sibling of map_value_write_probe
# (examples/), which pins the two partial-write forms that ARE legal Go and
# must NOT reject: m[k][i] = v on a SLICE-valued map (the slice header aliases
# its backing array) and m[k].F = v on a POINTER-valued map (auto-deref).
map-addr-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== map-addr-reject-probe: &m[k] must reject ==="
	@printf 'package main\nfunc main(){\n\tm := map[string]int{"a": 1}\n\tp := &m["a"]\n\t_ = p\n}\n' > build/map_addr_reject.goo
	@rm -f build/map_addr_reject
	@$(COMPILER) -o build/map_addr_reject build/map_addr_reject.goo > /dev/null 2> build/map_addr_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "map-addr-reject-probe: FAIL (&m[k] compiled)"; exit 1; fi; \
	if ! grep -q "cannot take the address of a map value" build/map_addr_reject.err; then echo "map-addr-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/map_addr_reject.err; exit 1; fi; \
	echo "map-addr-reject-probe: PASS"
	@echo "=== m[k].F = v must reject ==="
	@printf 'package main\ntype P struct { X int }\nfunc main(){\n\tm := map[string]P{"a": P{X: 1}}\n\tm["a"].X = 5\n}\n' > build/map_field_reject.goo
	@rm -f build/map_field_reject
	@$(COMPILER) -o build/map_field_reject build/map_field_reject.goo > /dev/null 2> build/map_field_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "map-addr-reject-probe: FAIL (m[k].F = v compiled)"; exit 1; fi; \
	if ! grep -q "cannot assign through a map value" build/map_field_reject.err; then echo "map-addr-reject-probe: FAIL (wrong/missing partial-write diagnostic)"; cat build/map_field_reject.err; exit 1; fi; \
	echo "map-addr-reject-probe: PASS (partial write rejected)"
	@echo "=== m[k][i] = v on an ARRAY-valued map must reject (sibling of the []T-valued map, which IS legal Go) ==="
	@printf 'package main\nfunc main(){\n\tm := map[string][3]int{}\n\tm["a"] = [3]int{1, 2, 3}\n\tm["a"][0] = 5\n}\n' > build/map_array_reject.goo
	@rm -f build/map_array_reject
	@$(COMPILER) -o build/map_array_reject build/map_array_reject.goo > /dev/null 2> build/map_array_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "map-addr-reject-probe: FAIL (m[k][i] = v on array-valued map compiled)"; exit 1; fi; \
	if ! grep -q "cannot assign through a map value" build/map_array_reject.err; then echo "map-addr-reject-probe: FAIL (wrong/missing array-value partial-write diagnostic)"; cat build/map_array_reject.err; exit 1; fi; \
	echo "map-addr-reject-probe: PASS (array-value index write rejected)"

# Non-string map keys (Task 3, AST_MAP_TYPE comparability gate in
# type_checker.c): a key type is admitted if it's string, an integer/uint
# width, bool, rune/byte (both integer kinds), or a pointer — anything else
# rejects with one of two distinct messages. (a) a key type that's
# comparable in Go but not yet wired into the v1 slot runtime (float, struct,
# interface, array) gets the "not yet supported in v1" deferred message.
# (b) a key type that's never comparable in Go (slice, map, func) gets the
# "not comparable" message. (c) once a map's key type is fixed, indexing it
# with a mismatched key type (a string literal against map[int]int) must
# still reject — the type_check_index_expr TYPE_MAP case, exercised
# separately from the type-declaration gate above.
mapkey-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== mapkey-reject-probe: map[float64]int{} must reject (deferred-comparable) ==="
	@printf 'package main\nfunc main(){\n\tm := map[float64]int{}\n\t_ = m\n}\n' > build/mapkey_float.goo
	@rm -f build/mapkey_float
	@$(COMPILER) -o build/mapkey_float build/mapkey_float.goo > /dev/null 2> build/mapkey_float.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "mapkey-reject-probe: FAIL (map[float64]int{} compiled)"; exit 1; fi; \
	if [ -x build/mapkey_float ]; then echo "mapkey-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "not yet supported in v1" build/mapkey_float.err; then echo "mapkey-reject-probe: FAIL (wrong/missing deferred-comparable diagnostic)"; cat build/mapkey_float.err; exit 1; fi; \
	echo "mapkey-reject-probe: PASS (deferred-comparable rejected)"
	@echo "=== mapkey-reject-probe: map[[]int]int{} must reject (non-comparable) ==="
	@printf 'package main\nfunc main(){\n\tm := map[[]int]int{}\n\t_ = m\n}\n' > build/mapkey_slice.goo
	@rm -f build/mapkey_slice
	@$(COMPILER) -o build/mapkey_slice build/mapkey_slice.goo > /dev/null 2> build/mapkey_slice.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "mapkey-reject-probe: FAIL (map[[]int]int{} compiled)"; exit 1; fi; \
	if [ -x build/mapkey_slice ]; then echo "mapkey-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "not comparable" build/mapkey_slice.err; then echo "mapkey-reject-probe: FAIL (wrong/missing non-comparable diagnostic)"; cat build/mapkey_slice.err; exit 1; fi; \
	echo "mapkey-reject-probe: PASS (non-comparable rejected)"
	@echo "=== mapkey-reject-probe: m[\"x\"] on map[int]int must reject (key-type mismatch) ==="
	@printf 'package main\nfunc main(){\n\tm := map[int]int{}\n\t_ = m["x"]\n}\n' > build/mapkey_wrongindex.goo
	@rm -f build/mapkey_wrongindex
	@$(COMPILER) -o build/mapkey_wrongindex build/mapkey_wrongindex.goo > /dev/null 2> build/mapkey_wrongindex.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "mapkey-reject-probe: FAIL (m[\"x\"] on map[int]int compiled)"; exit 1; fi; \
	if [ -x build/mapkey_wrongindex ]; then echo "mapkey-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "Map key type mismatch" build/mapkey_wrongindex.err; then echo "mapkey-reject-probe: FAIL (wrong/missing key-type-mismatch diagnostic)"; cat build/mapkey_wrongindex.err; exit 1; fi; \
	echo "mapkey-reject-probe: PASS (wrong-typed index rejected)"

# Struct-typed map keys (Task 3, struct_is_comparable_key in type_checker.c):
# a struct key is admitted only if every field is recursively comparable.
# Mirrors mapkey-reject-probe's two-reason split, but for fields NESTED
# inside a struct key rather than the key type itself. (a) a slice field is
# never comparable in Go (permanently rejected — "invalid map key type: ...
# non-comparable field"). (b) an array field IS comparable in Go but the v1
# synthesized comparator has no per-element loop yet (deferred — "not yet
# supported in v1"), same diagnostic family as a bare map[[N]int]int key.
struct-map-key-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== struct-map-key-reject-probe: struct key with a SLICE field must reject (non-comparable) ==="
	@printf 'package main\ntype SliceKey struct {\n\tX []int\n}\nfunc main(){\n\tm := map[SliceKey]int{}\n\t_ = m\n}\n' > build/struct_mapkey_slice.goo
	@rm -f build/struct_mapkey_slice
	@$(COMPILER) -o build/struct_mapkey_slice build/struct_mapkey_slice.goo > /dev/null 2> build/struct_mapkey_slice.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "struct-map-key-reject-probe: FAIL (struct key with a slice field compiled)"; exit 1; fi; \
	if [ -x build/struct_mapkey_slice ]; then echo "struct-map-key-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -qE "not comparable|invalid map key type" build/struct_mapkey_slice.err; then echo "struct-map-key-reject-probe: FAIL (wrong/missing non-comparable diagnostic)"; cat build/struct_mapkey_slice.err; exit 1; fi; \
	echo "struct-map-key-reject-probe: PASS (slice-field struct key rejected)"
	@echo "=== struct-map-key-reject-probe: struct key with an ARRAY field must reject (deferred-comparable) ==="
	@printf 'package main\ntype ArrKey struct {\n\tX [3]int\n}\nfunc main(){\n\tm := map[ArrKey]int{}\n\t_ = m\n}\n' > build/struct_mapkey_array.goo
	@rm -f build/struct_mapkey_array
	@$(COMPILER) -o build/struct_mapkey_array build/struct_mapkey_array.goo > /dev/null 2> build/struct_mapkey_array.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "struct-map-key-reject-probe: FAIL (struct key with an array field compiled)"; exit 1; fi; \
	if [ -x build/struct_mapkey_array ]; then echo "struct-map-key-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "not yet supported in v1" build/struct_mapkey_array.err; then echo "struct-map-key-reject-probe: FAIL (wrong/missing deferred-comparable diagnostic)"; cat build/struct_mapkey_array.err; exit 1; fi; \
	echo "struct-map-key-reject-probe: PASS (array-field struct key rejected)"

# Interface-typed map keys (Task 3, uncomparable dynamic key): the map's
# STATIC key type (`any`) is always comparable (TYPE_INTERFACE admits at the
# AST_MAP_TYPE gate), so `map[any]int{}` and `m[k] = v` must COMPILE even
# when k's dynamic value is a slice — the runtime doesn't know a key is
# uncomparable until it actually tries to compare two boxed values of that
# dynamic type. The first `m[k] = 1` just inserts (no existing entry to
# compare against); the SECOND `m[k] = 2` must runtime-panic while probing
# for an existing entry to overwrite, via the shared goo.uncmpeq stub
# (codegen_get_or_emit_type_eq's non-scalar arm, codegen.c) — Go-faithful:
# Go panics comparing/hashing an uncomparable dynamic value, not a compile
# error.
iface-map-key-uncomparable-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== iface-map-key-uncomparable-probe: uncomparable dynamic key must runtime-panic ==="
	@printf 'package main\nfunc main(){\n\tm := map[any]int{}\n\tvar k any = []int{1,2,3}\n\tm[k] = 1\n\tm[k] = 2\n}\n' > build/imk_unc.goo
	@$(COMPILER) -o build/imk_unc build/imk_unc.goo 2>build/imk_unc.cerr || { echo "iface-map-key-uncomparable-probe: FAIL (should COMPILE, panic at runtime)"; cat build/imk_unc.cerr; exit 1; }
	@./build/imk_unc 2>build/imk_unc.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "iface-map-key-uncomparable-probe: FAIL (no panic)"; exit 1; fi; \
	  if ! grep -qi "comparing uncomparable" build/imk_unc.err; then echo "iface-map-key-uncomparable-probe: FAIL (wrong message)"; cat build/imk_unc.err; exit 1; fi
	@echo "iface-map-key-uncomparable-probe: PASS"

# Task 2 (stdlib unblocker): `[]T(expr)` is only supported for []byte(string)
# in v1 (expression_checker.c's AST_SLICE_CONVERSION case) — any other
# element type (`[]int("x")`) must reject with the v1-scoped diagnostic,
# not reach codegen with no lowering.
bytesconv-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== bytesconv-reject-probe: []int(x) conversion must reject ==="
	@printf 'package main\nfunc main(){\n\tb := []int("x")\n\t_ = b\n}\n' > build/bytesconv_reject.goo
	@rm -f build/bytesconv_reject
	@$(COMPILER) -o build/bytesconv_reject build/bytesconv_reject.goo > /dev/null 2> build/bytesconv_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "bytesconv-reject-probe: FAIL (compiled rc=0 — []int(\"x\") silently accepted)"; exit 1; fi; \
	if [ -x build/bytesconv_reject ]; then echo "bytesconv-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "only supported for" build/bytesconv_reject.err; then echo "bytesconv-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/bytesconv_reject.err; exit 1; fi; \
	echo "bytesconv-reject-probe: PASS (rejected rc=$$rc)"

# Task 3 (spread `f(s...)`): four distinct reject shapes, each with its own
# diagnostic (expression_checker.c's has_spread block) — spread on a
# non-variadic callee, a spread element-type mismatch (no coercion, even
# though int32->int64 is otherwise a permitted numeric widening), a spread
# call missing a required fixed argument, and (reviewer finding, closed by
# making the spread checks independent of check_signature) the same
# element-type mismatch reached through a function-VALUED STRUCT FIELD
# callee (`o.Sum(s...)`), whose name-based resolution never sets
# check_signature — before the fix this compiled silently.
spread-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== spread-reject-probe: spread f(s...) must reject four distinct ways ==="
	@printf 'package main\nfunc f(a int) int { return a }\nfunc main(){ s := []int{1,2,3}; _ = f(s...) }\n' > build/spread_nonvariadic.goo
	@printf 'package main\nfunc sum64(xs ...int64) int64 { return 0 }\nfunc main(){ s := []int32{1,2,3}; _ = sum64(s...) }\n' > build/spread_elemmismatch.goo
	@printf 'package main\nfunc tagged(tag string, xs ...int) int { return 0 }\nfunc main(){ s := []int{1,2,3}; _ = tagged(s...) }\n' > build/spread_missingfixed.goo
	@printf 'package main\ntype Ops struct { Sum func(xs ...int64) int64 }\nfunc sum64(xs ...int64) int64 { return 0 }\nfunc main(){ o := Ops{Sum: sum64}; s := []int32{1,2,3}; _ = o.Sum(s...) }\n' > build/spread_fieldelemmismatch.goo
	@rm -f build/spread_nonvariadic build/spread_elemmismatch build/spread_missingfixed build/spread_fieldelemmismatch
	@$(COMPILER) -o build/spread_nonvariadic build/spread_nonvariadic.goo > build/spread_nonvariadic.out 2> build/spread_nonvariadic.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "spread-reject-probe: FAIL (non-variadic: f(s...) silently accepted)"; exit 1; fi; \
	if [ -x build/spread_nonvariadic ]; then echo "spread-reject-probe: FAIL (non-variadic: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "spread argument requires a variadic function" build/spread_nonvariadic.err; then echo "spread-reject-probe: FAIL (non-variadic: wrong/missing diagnostic)"; cat build/spread_nonvariadic.err; exit 1; fi
	@$(COMPILER) -o build/spread_elemmismatch build/spread_elemmismatch.goo > build/spread_elemmismatch.out 2> build/spread_elemmismatch.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "spread-reject-probe: FAIL (elem mismatch: []int32 into ...int64 silently accepted)"; exit 1; fi; \
	if [ -x build/spread_elemmismatch ]; then echo "spread-reject-probe: FAIL (elem mismatch: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "cannot spread \[\]int32 into variadic parameter ...int64" build/spread_elemmismatch.err; then echo "spread-reject-probe: FAIL (elem mismatch: wrong/missing diagnostic)"; cat build/spread_elemmismatch.err; exit 1; fi
	@$(COMPILER) -o build/spread_missingfixed build/spread_missingfixed.goo > build/spread_missingfixed.out 2> build/spread_missingfixed.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "spread-reject-probe: FAIL (missing fixed arg: tagged(s...) silently accepted)"; exit 1; fi; \
	if [ -x build/spread_missingfixed ]; then echo "spread-reject-probe: FAIL (missing fixed arg: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "spread call must supply exactly the fixed arguments then one slice" build/spread_missingfixed.err; then echo "spread-reject-probe: FAIL (missing fixed arg: wrong/missing diagnostic)"; cat build/spread_missingfixed.err; exit 1; fi
	@$(COMPILER) -o build/spread_fieldelemmismatch build/spread_fieldelemmismatch.goo > build/spread_fieldelemmismatch.out 2> build/spread_fieldelemmismatch.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "spread-reject-probe: FAIL (struct-field elem mismatch: o.Sum(s...) with []int32 silently accepted)"; exit 1; fi; \
	if [ -x build/spread_fieldelemmismatch ]; then echo "spread-reject-probe: FAIL (struct-field elem mismatch: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "cannot spread \[\]int32 into variadic parameter ...int64" build/spread_fieldelemmismatch.err; then echo "spread-reject-probe: FAIL (struct-field elem mismatch: wrong/missing diagnostic)"; cat build/spread_fieldelemmismatch.err; exit 1; fi
	@echo "spread-reject-probe: PASS (all four spread-reject shapes correctly rejected)"

# Task 4 (copy builtin): two distinct reject shapes, each with its own
# diagnostic (expression_checker.c's copy() arm) — an element-type mismatch
# ([]string source into a []int destination, neither identical-elem nor the
# byte/string special case) and a non-slice destination (a string passed as
# dst, which Go also rejects: only src may be a string).
copy-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== copy-reject-probe: copy(dst, src) must reject two distinct ways ==="
	@printf 'package main\nfunc main(){ a := []int{1,2,3}; b := []string{"x"}; _ = copy(a, b) }\n' > build/copy_elemmismatch.goo
	@printf 'package main\nfunc main(){ s := "x"; b := []byte{1,2,3}; _ = copy(s, b) }\n' > build/copy_stringdst.goo
	@rm -f build/copy_elemmismatch build/copy_stringdst
	@$(COMPILER) -o build/copy_elemmismatch build/copy_elemmismatch.goo > build/copy_elemmismatch.out 2> build/copy_elemmismatch.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "copy-reject-probe: FAIL (elem mismatch: []string into []int64 silently accepted)"; exit 1; fi; \
	if [ -x build/copy_elemmismatch ]; then echo "copy-reject-probe: FAIL (elem mismatch: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "copy: cannot copy \[\]string into \[\]int64" build/copy_elemmismatch.err; then echo "copy-reject-probe: FAIL (elem mismatch: wrong/missing diagnostic)"; cat build/copy_elemmismatch.err; exit 1; fi
	@$(COMPILER) -o build/copy_stringdst build/copy_stringdst.goo > build/copy_stringdst.out 2> build/copy_stringdst.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "copy-reject-probe: FAIL (string dst: copy(s, b) silently accepted)"; exit 1; fi; \
	if [ -x build/copy_stringdst ]; then echo "copy-reject-probe: FAIL (string dst: emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "copy: destination must be a slice" build/copy_stringdst.err; then echo "copy-reject-probe: FAIL (string dst: wrong/missing diagnostic)"; cat build/copy_stringdst.err; exit 1; fi
	@echo "copy-reject-probe: PASS (both copy-reject shapes correctly rejected)"

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

# P4.5: source-dir-relative imports. Three throwaway package trees under
# build/reldir_probe/, each proving one resolution rule from the design doc
# (docs/superpowers/specs/2026-07-10-p4-packages-a-design.md):
#   - rel: "./mathx" resolves against the MAIN FILE'S OWN DIRECTORY, with no
#     matching entry anywhere under GOOROOT (proves the source-dir tier is
#     real, not an accidental GOOROOT hit).
#   - local: a bare "onlylocal" import — absent from GOOROOT — falls back to
#     the main file's directory as the LAST tier.
#   - shadow: a bare "mypkg" import exists in BOTH goostd/mypkg (Double(n) =
#     n+n) and a same-named local directory (Double(n) = n*3, deliberately
#     different). GOOROOT must win — asserted by behavior (21 -> 42, not 63)
#     since the resolver returns no other signal distinguishing the tiers.
#     This is the deliberate roadmap deviation: source-dir is a FALLBACK,
#     never a shadow, to avoid a local dir silently hijacking a stdlib name.
reldir-import-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build/reldir_probe/mathx build/reldir_probe/onlylocal build/reldir_probe/mypkg
	@echo "=== reldir-import-probe: source-dir-relative + bare-fallback + shadow-prevention imports ==="
	@printf 'package mathx\nfunc Double(x int) int { return x*2 }\n' > build/reldir_probe/mathx/mathx.go
	@printf 'package main\nimport ("fmt"\n"./mathx")\nfunc main() { fmt.Println(mathx.Double(21)) }\n' > build/reldir_probe/rel_main.goo
	@printf 'package onlylocal\nfunc Value() int { return 77 }\n' > build/reldir_probe/onlylocal/onlylocal.go
	@printf 'package main\nimport ("fmt"\n"onlylocal")\nfunc main() { fmt.Println(onlylocal.Value()) }\n' > build/reldir_probe/local_main.goo
	@printf 'package mypkg\nfunc Double(n int) int { return n*3 }\n' > build/reldir_probe/mypkg/mypkg.go
	@printf 'package main\nimport ("fmt"\n"mypkg")\nfunc main() { fmt.Println(mypkg.Double(21)) }\n' > build/reldir_probe/shadow_main.goo
	@"$(COMPILER)" build/reldir_probe/rel_main.goo -o build/reldir_probe/rel_main.out 2>build/reldir_probe/rel_main.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "reldir-import-probe: FAIL (./mathx did not compile)"; cat build/reldir_probe/rel_main.err; exit 1; fi; \
	  out=$$(./build/reldir_probe/rel_main.out); if [ "$$out" != "42" ]; then echo "reldir-import-probe: FAIL (./mathx: printed '$$out', want 42)"; exit 1; fi
	@"$(COMPILER)" build/reldir_probe/local_main.goo -o build/reldir_probe/local_main.out 2>build/reldir_probe/local_main.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "reldir-import-probe: FAIL (bare onlylocal fallback did not compile)"; cat build/reldir_probe/local_main.err; exit 1; fi; \
	  out=$$(./build/reldir_probe/local_main.out); if [ "$$out" != "77" ]; then echo "reldir-import-probe: FAIL (bare onlylocal: printed '$$out', want 77)"; exit 1; fi
	@"$(COMPILER)" build/reldir_probe/shadow_main.goo -o build/reldir_probe/shadow_main.out 2>build/reldir_probe/shadow_main.err; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "reldir-import-probe: FAIL (bare mypkg shadow case did not compile)"; cat build/reldir_probe/shadow_main.err; exit 1; fi; \
	  out=$$(./build/reldir_probe/shadow_main.out); if [ "$$out" != "42" ]; then echo "reldir-import-probe: FAIL (bare mypkg printed '$$out', want 42 (GOOROOT) — got the local shadow instead)"; exit 1; fi
	@echo "reldir-import-probe: PASS"

# P4.8 os.ReadLine stdin gate: run_golden.sh has no mechanism to pipe stdin
# into a fixture (see its doc comment — env sidecars only), so ReadLine gets
# a dedicated probe target instead of an examples/*.expected.txt golden
# fixture (examples/os_readline_probe.goo deliberately has no sibling
# .expected.txt, so run_golden.sh's glob skips it entirely).
#
# Stdin is fed via `<` file redirection, NOT a `|` pipe: a pipe's `rc=$?`
# would capture only the LAST stage's exit status (the CLAUDE.md piped-
# exit-codes gotcha — `cmd | othercmd` masks `cmd`'s own failure), which
# here would hide a ReadLine/exit-code regression in os_readline_probe
# itself. Redirection has no such stage to mask: `rc=$?` is the probe
# binary's own exit code, captured directly off the invocation.
readline-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== readline-probe: os.ReadLine reads stdin lines until EOF ==="
	@"$(COMPILER)" examples/os_readline_probe.goo -o build/os_readline_probe.out
	@printf 'alpha\nbeta\ngamma\n' > build/readline_probe.stdin
	@./build/os_readline_probe.out < build/readline_probe.stdin > build/readline_probe.actual.txt; rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "readline-probe: FAIL (exit $$rc)"; exit 1; fi
	@if diff -u examples/os_readline_probe.probe_expected.txt build/readline_probe.actual.txt; then \
	  echo "readline-probe: PASS"; \
	else \
	  echo "readline-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi

# P4.11: stdlib e2e smoke suite + shim-table drift catch. Runs
# scripts/check_stdlib_coverage.sh, which mechanically extracts every
# SHIM_TABLE row (shim_signatures.c), seeded sync/time export, package value
# member (os.Args, math.Pi, time.* Duration constants), and goostd exported
# func (strings/strconv/utf8/bits) and requires each to appear in at least
# one golden-wired examples/*.goo fixture. A stdlib symbol added without
# smoke coverage fails THIS target — the drift catch
# docs/2026-07-08-v1-roadmap.md:159 asks for. Pure source/text scan, no
# compiler build needed (unlike most probes above). Supersedes the narrower
# `smoke-stdlib` (4 symbols, M7-era) as the authoritative stdlib coverage
# gate; `smoke-stdlib` itself is left running as-is since a coord milestone
# still references it by name.
stdlib-smoke-coverage:
	@bash scripts/check_stdlib_coverage.sh

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
#
# P2.8 T4.3: the !int case used to go through an intermediate `x := f()`
# binding, which this task now rejects EARLIER, at the binding itself
# ("error union must be handled: use try, catch, or v, err := destructuring")
# — a strictly better diagnostic (source of the mistake, not just its first
# symptom), but no longer this probe's target print-time message. Call
# fmt.Println(f()) directly (no intermediate binding) so this probe keeps
# exercising the print-time "unsupported argument type" path it was written
# for; the var-decl path has its own dedicated golden
# (erru_unhandled_bind_reject).
print-aggregate-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== print-aggregate-probe: printing ?T/!T aggregates fails cleanly ==="
	@printf 'package main\nimport "fmt"\nfunc main() { var x ?int = 5; fmt.Println(x) }\n' > build/print_agg_null.goo
	@printf 'package main\nimport "fmt"\nfunc f() !int { return 5 }\nfunc main() { fmt.Println(f()) }\n' > build/print_agg_erru.goo
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
.PHONY: blank-read-reject-probe const-index-reject-probe comptime-value-reject-probe comptime-value-reject-matrix comptime-generic-compose-ir-pin lanes-monomorphize-ir-pin spmd-bench-probe stencil-race-runbook-probe stencil-parallel-probe test-golden test-golden-o2 test-golden-reject
test-golden: $(COMPILER) $(RUNTIME_LIB)
	@echo "=== test-golden: data-driven end-to-end golden suite ==="
	@COMPILER="$(COMPILER)" bash scripts/run_golden.sh

# Phase 3 exit gate (P3.10): the ENTIRE golden suite must also be green
# with real optimization passes on — a fixture that passes at -O0 but
# fails here is a miscompile-under-optimization (this exact gate caught
# the shift-width poison bug and the pre-datalayout pass-ordering bug
# when -O was first wired). GOOFLAGS is run_golden.sh's compile-flags
# passthrough.
test-golden-o2: $(COMPILER) $(RUNTIME_LIB)
	@echo "=== test-golden-o2: golden suite at -O2 (miscompile-under-optimization gate) ==="
	@COMPILER="$(COMPILER)" GOOFLAGS="-O2" bash scripts/run_golden.sh

# P0.9: data-driven compile-REJECT golden suite — the negative-space sibling
# of test-golden. Every tests/golden/reject/<name>.goo must fail to compile;
# see scripts/run_golden_reject.sh's header for the exact per-fixture
# assertions (exit nonzero, no binary emitted, stderr contains the sidecar's
# .err.txt substring). Five Makefile reject-probes (addrlit, boolnot,
# constdiv, funcsig, trailingcomma) migrated here as the pilot fixtures;
# later tasks add more by dropping a .goo + .err.txt pair in, no Makefile
# changes required.
test-golden-reject: $(COMPILER) $(RUNTIME_LIB)
	@echo "=== test-golden-reject: data-driven compile-reject golden suite ==="
	@COMPILER="$(COMPILER)" bash scripts/run_golden_reject.sh

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

TEST_FLOW_ANALYSIS = $(BINDIR)/test_flow_analysis
TEST_REFERENCE_MANAGER = $(BINDIR)/test_reference_manager
TEST_HARDWARE_AWARE = $(BINDIR)/test_hardware_aware

# Tests
test: $(TEST_RUNNER) test-cli
	./$(TEST_RUNNER)

# P5.4: table-driven CLI exit-code and stderr discipline audit. Success=0,
# parse/type error=1, link failure nonzero, run-failure propagation, all
# error text on stderr (usage included), stdout only for requested output.
.PHONY: test-cli
test-cli: $(COMPILER) $(RUNTIME_LIB)
	@bash tests/cli/cli_test.sh "$(COMPILER)"

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

# (P5.7: test-interface retired — test_interface_system.c no longer compiled
# against the current framework headers, and the interface/protocol framework
# it exercised is unlinked from bin/goo since P5.6. Recover from git history
# if the framework is ever revived.)

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

# Development Workflow Tools
PROJECT_WIZARD = $(BINDIR)/goo-wizard
PROFILER_TOOL = $(BINDIR)/goo-profiler
DOC_GENERATOR = $(BINDIR)/goo-docs
HEALTH_DASHBOARD = $(BINDIR)/goo-health

# Complete development workflow toolchain
# (test-tool removed: its source tools/test_runner/main.c was never created;
# the pipeline is asserted by the golden suites and tests/cli/cli_test.sh.)
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

# Enhanced LSP Server with AST integration. Known broken link (undefined
# parser_cleanup) — repairing it is the P5.11 open decision; kept because it
# is real AST-integrated code, unlike the quarantined toy LSPs (P5.5).
lsp-enhanced: $(LSP_ENHANCED_SERVER)

$(LSP_ENHANCED_SERVER): $(SRCDIR)/ide/lsp_enhanced.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

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

# Arena leg Task 7a: interprocedural param-escape summaries (table-driven,
# 15-row test matrix — see docs/superpowers/specs/2026-07-07-arena-7a-param-
# escape-summaries-design.md). Modeled on runtime_optimization_test above.
param_escape_test: $(TEST_UNIT_DIR)/types/param_escape_test.c $(SRC_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LDFLAGS) $(LLVM_LDFLAGS)

param-escape-test: param_escape_test
	@echo "Running param-escape summary tests..."
	./param_escape_test

# Arena leg Task 7b: per-alloc-site block-escape decisions (table-driven,
# 15-row test matrix — see docs/superpowers/specs/2026-07-07-arena-7b-
# block-escape-decision-design.md). Modeled on param_escape_test above.
block_escape_test: $(TEST_UNIT_DIR)/types/block_escape_test.c $(SRC_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LDFLAGS) $(LLVM_LDFLAGS)

block-escape-test: block_escape_test
	@echo "Running block-escape decision tests..."
	./block_escape_test

# Arena leg Task 7c: codegen_arena_eligible predicate — the gate that
# consumes 7a/7b's decisions at the codegen_emit_alloc choke point (see
# docs/superpowers/specs/2026-07-07-arena-7c-emit-alloc-routing-design.md).
# Modeled on block_escape_test above; links the full SRC_OBJS like the other
# codegen-adjacent unit tests since it pulls in codegen.o for
# codegen_arena_eligible.
arena_routing_test: $(TEST_UNIT_DIR)/codegen/arena_routing_test.c $(SRC_OBJS)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $^ $(LDFLAGS) $(LLVM_LDFLAGS)

arena-routing-test: arena_routing_test
	@echo "Running arena-routing predicate tests..."
	./arena_routing_test

# Arena leg Task 6: golden + valgrind probe matrix for `arena {}` actually
# freeing memory at block exit (see docs/superpowers/specs/2026-07-07-
# arena-6-arena-free-at-block-exit-design.md). Five examples/*.goo probes,
# each a distinct escape shape: reclaim (non-escaping, freed on
# fall-through), escape-via-return, escape-via-store-to-an-outer-local,
# escape-via-embedding-in-a-returned-composite (7b's field-taint union),
# and a 100000-iteration loop capstone (per-iteration arena reclaimed, no
# unbounded growth). Every one of these already has a sibling
# examples/*.expected.txt, so `make test-golden` also covers them — this
# target exists as the named, scoped-to-Task-6 entry point the design doc
# asks for.
#
# arena-goto fix (2026-07-09): arena_goto_probe added to this list purely
# for arena-valgrind-probe's UAF/double-free gate below — a regression
# fence for "goto backward into an arena{} block double-frees" (a fixed
# arena depth off-by-one in codegen_emit_arena_frees_to_depth,
# statement_codegen.c, would show up here as an invalid free/UAF even
# though the probe's own output would still happen to look right). NOT a
# meaningful addition to arena-rss-probe's PROBES list (scripts/
# arena_rss_probe.sh) — see the probe file's own header comment: any
# function containing a goto currently defeats block_escape.c's arena-
# eligibility classification (no AST_GOTO_STMT/AST_LABEL_STMT case there),
# so an RSS-delta assertion on a goto-containing arena probe would measure
# that unrelated, pre-existing gap instead of this fix.
ARENA_FREE_PROBE_NAMES = arena_reclaim_probe arena_escape_return_probe arena_escape_store_probe arena_embedded_escape_probe arena_loop_reclaim_probe arena_defer_escape_probe arena_chan_send_probe arena_return_probe arena_loopexit_probe arena_fmt_println_probe arena_goto_probe

arena-free-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== arena-free-probe: Task 6 golden matrix (compile+run+diff) ==="
	@fail=0; total=0; \
	for name in $(ARENA_FREE_PROBE_NAMES); do \
	  total=$$((total+1)); \
	  if ! $(COMPILER) -o build/$$name examples/$$name.goo > build/$$name.cerr 2>&1; then \
	    echo "$$name: FAIL (compile/link)"; cat build/$$name.cerr; fail=1; continue; \
	  fi; \
	  ./build/$$name > build/$$name.actual.txt 2>/dev/null; \
	  if diff -u examples/$$name.expected.txt build/$$name.actual.txt > build/$$name.diff; then \
	    echo "$$name: PASS"; \
	  else \
	    echo "$$name: FAIL (output mismatch)"; cat build/$$name.diff; fail=1; \
	  fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "arena-free-probe: FAIL"; exit 1; fi; \
	echo "arena-free-probe: PASS ($$total/$$total)"

# Same 5 binaries, run under the UAF/double-free gate:
#   valgrind --leak-check=no --error-exitcode=99 ./probe
# Leaks are IGNORED (--leak-check=no; the prototype's goo_alloc never
# frees, so every heap allocation is a "leak" — expected, not a bug). Only
# a genuine memory-access error trips this: exit 99 (from --error-exitcode)
# or the literal "Invalid read"/"Invalid write"/"Invalid free"/"double
# free" text in valgrind's own diagnostic is what would indicate the arena
# free-at-block-exit design has a use-after-free or double-free. If
# valgrind isn't installed, SKIP loudly rather than silently passing.
arena-valgrind-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@if ! which valgrind > /dev/null 2>&1; then \
	  echo "valgrind not found — SKIPPED"; \
	  exit 0; \
	fi
	@echo "=== arena-valgrind-probe: Task 6 UAF/double-free gate (valgrind) ==="
	@fail=0; total=0; \
	for name in $(ARENA_FREE_PROBE_NAMES); do \
	  total=$$((total+1)); \
	  if ! $(COMPILER) -o build/$$name examples/$$name.goo > build/$$name.cerr 2>&1; then \
	    echo "$$name: FAIL (compile/link)"; cat build/$$name.cerr; fail=1; continue; \
	  fi; \
	  valgrind --leak-check=no --error-exitcode=99 ./build/$$name \
	    > build/$$name.vg.out 2> build/$$name.vg.err; \
	  rc=$$?; \
	  if [ $$rc -ne 0 ] || grep -qE "Invalid read|Invalid write|Invalid free|double free" build/$$name.vg.err; then \
	    echo "$$name: FAIL (valgrind rc=$$rc — see build/$$name.vg.err)"; \
	    tail -40 build/$$name.vg.err; \
	    fail=1; \
	  else \
	    echo "$$name: PASS (valgrind clean, rc=$$rc)"; \
	  fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "arena-valgrind-probe: FAIL"; exit 1; fi; \
	echo "arena-valgrind-probe: PASS ($$total/$$total clean)"

# Arena RSS capstone (Task 9): the concrete reclamation proof. Compiles the
# 100k-iteration temporary-building loop with `arena { }` (freed each iteration)
# vs. a plain-block variant (leaks), and asserts the arena build's peak resident
# memory is well below the leaking build's. Skips loudly if /usr/bin/time -v is
# unavailable.
arena-rss-probe: $(COMPILER) $(RUNTIME_LIB)
	@COMPILER="$(COMPILER)" bash scripts/arena_rss_probe.sh

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
	rm -f runtime_optimization_test runtime_optimization_demo contracts_test contract_proof_integration_test proof_generation_test param_escape_test block_escape_test arena_routing_test
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

# Go rejects a CONSTANT out-of-bounds (or negative) array index at COMPILE time.
# This probe pins that: arr[5] on a [3]int, a const-identifier OOB, and arr[-1]
# must all be compile errors (the runtime bounds check covers variable indices).
const-index-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== const-index-reject-probe: constant OOB / negative array index rejects at compile ==="
	@printf 'package main\nfunc main(){ var a [3]int; a[5]=9; _=a }\n' > build/cir_oob.goo
	@if $(COMPILER) -o build/cir_oob build/cir_oob.goo 2>build/cir_oob.err; then echo "const-index-reject-probe: FAIL (arr[5] compiled)"; exit 1; fi
	@grep -q "out of bounds" build/cir_oob.err || { echo "const-index-reject-probe: FAIL (wrong msg)"; cat build/cir_oob.err; exit 1; }
	@printf 'package main\nfunc main(){ var a [3]int; _=a[-1] }\n' > build/cir_neg.goo
	@if $(COMPILER) -o build/cir_neg build/cir_neg.goo 2>build/cir_neg.err; then echo "const-index-reject-probe: FAIL (arr[-1] compiled)"; exit 1; fi
	@grep -q "must not be negative" build/cir_neg.err || { echo "const-index-reject-probe: FAIL (wrong neg msg)"; cat build/cir_neg.err; exit 1; }
	@printf 'package main\nfunc main(){ a:=[3]int{1,2,3}; _=a[2] }\n' > build/cir_ok.goo
	@if ! $(COMPILER) -o build/cir_ok build/cir_ok.goo 2>build/cir_ok.err; then echo "const-index-reject-probe: FAIL (in-bounds arr[2] rejected)"; cat build/cir_ok.err; exit 1; fi
	@echo "const-index-reject-probe: PASS"

# RTTI concrete-type-switch plan, Task 1: a failed `x.(T)` assert on the
# empty interface (no comma-ok to absorb the miss) must still panic cleanly
# now that the empty-interface guard is lifted — mirrors typeassert-abort-probe
# but with an `interface{}` operand instead of a method-bearing interface.
# exit 2 is Go-conformant per Task 6 (GOO_PANIC_ABORT=1 restores the old
# abort()/134 for debugging).
rtti-assert-panic-probe: lexer
	@printf 'package main\nfunc main(){ var x interface{} = "s"; _ = x.(int) }\n' > build/rtti_panic.goo
	@$(COMPILER) build/rtti_panic.goo -o build/rtti_panic 2>/dev/null || (echo "FAIL: should compile"; exit 1)
	@build/rtti_panic; rc=$$?; \
	if [ $$rc -ne 2 ]; then echo "FAIL: expected exit 2 on assert-miss, got $$rc"; exit 1; fi
	@echo "PASS rtti-assert-panic-probe (assert-miss panics, exit 2)"

# RTTI concrete-type-switch plan, Task 3 — retired by the interface-target
# RTTI plan's Task 3: this probe used to lock in that `case Interface:` in a
# type switch stayed compile-rejected pending its own codegen primitive.
# That primitive (codegen_interface_target_match, interface_codegen.c) now
# backs all three interface-target shapes — single-return `x.(Interface)`
# (Task 1), comma-ok `v, ok := x.(Interface)` (Task 2), and `case Interface:`
# (Task 3, see examples/iface_target_switch.goo) — so there is no longer a
# still-rejected sub-case for this probe to exercise; it was removed rather
# than narrowed further (mirrors the ta_ifacetarget/rtti_rej_assert removals
# in Tasks 1–2's commits).

# Interface-type-descriptor plan, Task 4: a failed `x.(T)` panic must name
# the DYNAMIC (actually held) type — read from the interface value's vtable
# slot 0 -> descriptor field 1 at RUNTIME (goo_panic_iface_conversion,
# runtime.c) — not just the static interface/target names the old message
# was limited to. examples/iface_assert_dynname_probe.goo holds a bool and
# asserts to string; mirrors typeassert-abort-probe's compile/run/grep shape.
# exit 2 is Go-conformant per Task 6 (GOO_PANIC_ABORT=1 restores the old
# abort()/134 for debugging).
iface-assert-dynname-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== iface-assert-dynname-probe: failed x.(T) names the dynamic type ==="
	@"$(COMPILER)" -o build/iface_assert_dynname examples/iface_assert_dynname_probe.goo 2>build/iface_assert_dynname.cerr || \
	  { echo "iface-assert-dynname-probe: FAIL (compile)"; cat build/iface_assert_dynname.cerr; exit 1; }
	@./build/iface_assert_dynname 2>build/iface_assert_dynname.err; rc=$$?; \
	if [ $$rc -ne 2 ]; then echo "iface-assert-dynname-probe: FAIL (bad assert exit $$rc, want 2)"; exit 1; fi; \
	if ! grep -q "is bool, not string" build/iface_assert_dynname.err; then echo "iface-assert-dynname-probe: FAIL (dynamic type not named)"; cat build/iface_assert_dynname.err; exit 1; fi
	@echo "iface-assert-dynname-probe: PASS"

# Interface-target RTTI plan, Task 1: a failed single-return `x.(I)` where I
# is an INTERFACE target (not a concrete type) must panic cleanly, naming the
# dynamic type via Go's own "is not" wording for this shape
# (goo_panic_iface_notimpl, runtime.c — distinct from goo_panic_iface_conversion's
# concrete-target "X is Y, not Z"). examples/iface_target_assert_abort_probe.goo
# boxes an int (Goo names it "int64") that does not implement Speaker; mirrors
# typeassert-abort-probe's compile/run/grep shape. exit 2 is Go-conformant
# per Task 6 (GOO_PANIC_ABORT=1 restores the old abort()/134 for debugging).
iface-target-assert-abort-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== iface-target-assert-abort-probe: failed x.(I) on an interface target must panic ==="
	@"$(COMPILER)" -o build/iface_target_assert_abort examples/iface_target_assert_abort_probe.goo 2>build/iface_target_assert_abort.cerr || \
	  { echo "iface-target-assert-abort-probe: FAIL (compile)"; cat build/iface_target_assert_abort.cerr; exit 1; }
	@./build/iface_target_assert_abort 2>build/iface_target_assert_abort.err; rc=$$?; \
	if [ $$rc -ne 2 ]; then echo "iface-target-assert-abort-probe: FAIL (bad assert exit $$rc, want 2)"; exit 1; fi; \
	if ! grep -q "is not Speaker" build/iface_target_assert_abort.err; then echo "iface-target-assert-abort-probe: FAIL (dynamic type not named)"; cat build/iface_target_assert_abort.err; exit 1; fi
	@echo "iface-target-assert-abort-probe: PASS"

# Regenerate the Go-stdlib coverage report (docs/stdlib-coverage.json).
# Scores supported symbols against $GOROOT/api/go1*.txt. Needs `go` on PATH.
.PHONY: stdlib-coverage
stdlib-coverage:
	python3 scripts/stdlib-coverage.py
