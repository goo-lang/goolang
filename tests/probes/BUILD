load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//tools:goo_probe.bzl", "goo_script_probe")

# The probe census must stay current: it decides which macro each of the 186
# probe gates gets, so a stale copy silently mis-routes probes.
#
# no-sandbox because probe_census.sh shells out to parity.sh, which shells out
# to bazel query -- a nested bazel invocation contends on the output-base lock
# inside a sandboxed test. Same reason parity_selftest.sh is tagged this way.
sh_test(
    name = "census_current",
    size = "small",
    srcs = ["census_current.sh"],
    tags = [
        "external",
        "manual",
        "no-sandbox",
    ],
)


# generated.bzl must stay current. It holds 63 targets DERIVED from the
# Makefile's own probe recipes, and its header says "do not edit" -- nothing
# enforced that, so a hand edit would have survived and the derivation would
# have become a snapshot in silence.
#
# NOT tagged manual, unlike census_current above: the generator reads Makefile
# and census.txt through awk, with no nested bazel to deadlock on the
# output-base lock. tests/probes/targets_current.sh --self-test proves it can
# go red, and make runs both.
sh_test(
    name = "targets_current",
    size = "small",
    srcs = ["targets_current.sh"],
    data = [
        "census.txt",
        "generated.bzl",
        "//:Makefile",
        "//tools:gen_probe_targets.py",
        "//tests/probes/src:all_sources",
    ],
)

load(":generated.bzl", "generated_probes")

# 63 of the 80 fixture-shaped probes, emitted by tools/gen_probe_targets.py.
# The other 17 are omitted with a printed reason -- see the generator's header.
generated_probes()


# Task 14, the script-backed gates. The EXEMPLAR: every other one copies this
# shape. The script is the assertion, so there is nothing to generate -- only
# the COMPILER contract to satisfy.
goo_script_probe(
    name = "exit_code_probe",
    script = "//:scripts/exit_code_probe.sh",
)

# Task 14: the script-backed gates that a sandbox can run.
goo_script_probe(
    name = "lexer_keyword_probe",
    script = "//:scripts/lexer_keyword_probe.sh",
)
goo_script_probe(
    name = "link_flags_probe",
    script = "//:scripts/link_flags_probe.sh",
)
goo_script_probe(
    name = "nil_deref_probe",
    script = "//:scripts/nil_deref_probe.sh",
)

# The two script gates from the no-compiler survey whose reads DO resolve from
# the root package: scripts/ and .github/workflows/ have no BUILD of their own.
goo_script_probe(
    name = "probe_teeth_probe",
    script = "//:scripts/probe_teeth_probe.sh",
    needs_compiler = False,
    data = ["//:probe_scripts"],
)

goo_script_probe(
    name = "workflow_targets_probe",
    script = "//:scripts/workflow_targets_probe.sh",
    needs_compiler = False,
    data = [
        "//:Makefile",
        "//:workflow_files",
    ],
)

# Task 14, the four arc release gates. Each fixture imports "strings", and
# the compiler's last resolver tier is ./goostd relative to the working
# directory -- the runfiles root -- so the stdlib is DATA here, not a
# GOOROOT the macro cannot name. The same tier is what every generated
# fixture probe already relies on.
#
# tags = ["nosan"]: the sanitizer legs (.bazelrc test:san-base) instrument
# the COMPILER and install no valgrind, while these four gates measure the
# COMPILED PROGRAM under valgrind. A sanitizer leg learns nothing from them
# and, with no valgrind on that job, would only turn them red.
goo_script_probe(
    name = "arc_concat_operand_probe",
    script = "//:scripts/arc_concat_operand_probe.sh",
    data = ["//goostd:files"],
    tags = ["nosan"],
)

goo_script_probe(
    name = "arc_map_key_local_probe",
    script = "//:scripts/arc_map_key_local_probe.sh",
    data = ["//goostd:files"],
    tags = ["nosan"],
)

goo_script_probe(
    name = "arc_multi_assign_probe",
    script = "//:scripts/arc_multi_assign_probe.sh",
    data = ["//goostd:files"],
    tags = ["nosan"],
)

goo_script_probe(
    name = "arc_reassign_probe",
    script = "//:scripts/arc_reassign_probe.sh",
    data = ["//goostd:files"],
    tags = ["nosan"],
)

# The generated fixture imports "strings", and the compiler's last resolver
# tier is ./goostd relative to the working directory -- the runfiles root --
# so the stdlib is DATA here, the same shape as the four arc_* gates above.
goo_script_probe(
    name = "dead_package_code_probe",
    script = "//:scripts/dead_package_code_probe.sh",
    data = ["//goostd:files"],
)

# No extra data: the four arena_*_reclaim_probe.goo fixtures this script
# reads by name are top-level examples/*.goo, so the macro's own
# //examples:all_fixtures already covers them.
goo_script_probe(
    name = "arena_rss_probe",
    script = "//:scripts/arena_rss_probe.sh",
)

goo_script_probe(
    name = "goo_test_probe",
    script = "//:scripts/goo_test_probe.sh",
    data = ["//examples:test_packages"],
)

# Its generated source imports only fmt, a C shim, and it never changes
# directory, so the macro's contract is all it needs.
goo_script_probe(
    name = "string_literal_header_probe",
    script = "//:scripts/string_literal_header_probe.sh",
)

# The three C-fixture gates. None invokes the Goo compiler: each compiles
# small C programs against one header with the C compiler the Makefile
# would use, which the macro hands over as CC_PROBE from Bazel's own
# toolchain. tags = ["nosan"]: a sanitizer build of the Goo compiler
# changes nothing these gates test, while the config swaps $(CC) to clang,
# whose coverage notes gcov cannot read. The two _selftest targets are the
# second command each make gate runs, and they are the teeth.
goo_script_probe(
    name = "goo_assert_probe",
    script = "//:scripts/goo_assert_probe.sh",
    data = ["//include:goo_assert.h"],
    needs_compiler = False,
    needs_cc = True,
    tags = ["nosan"],
)
goo_script_probe(
    name = "goo_check_probe",
    script = "//:scripts/goo_check_probe.sh",
    data = ["//tests/unit:goo_check.h"],
    needs_compiler = False,
    needs_cc = True,
    tags = ["nosan"],
)
goo_script_probe(
    name = "goo_check_selftest",
    script = "//:scripts/goo_check_probe.sh",
    args = ["--self-test"],
    data = ["//tests/unit:goo_check.h"],
    needs_compiler = False,
    needs_cc = True,
    tags = ["nosan"],
)
goo_script_probe(
    name = "goo_testcase_probe",
    script = "//:scripts/goo_testcase_probe.sh",
    data = ["//include:goo_assert.h", "//:scripts/testcase_report.sh"],
    needs_compiler = False,
    needs_cc = True,
    tags = ["nosan"],
)
goo_script_probe(
    name = "goo_testcase_selftest",
    script = "//:scripts/goo_testcase_probe.sh",
    args = ["--self-test"],
    data = ["//include:goo_assert.h", "//:scripts/testcase_report.sh"],
    needs_compiler = False,
    needs_cc = True,
    tags = ["nosan"],
)

# Task 14, the five gates that read or compile src/**. Each of the ten
# src/*:probe_sources filegroups exports one package's own .c/.h files, since
# a glob does not cross a package boundary and there is no single //:c_sources
# the way //goostd:files covers the stdlib.
#
# tags = ["nosan"] on ast_free_leak_probe and proof_cache_shell_probe: both
# compile C with $(CC), which the sanitizer configs (.bazelrc test:san-base)
# swap to clang. The other three either never touch a C compiler, or package
# the Goo compiler Bazel already built -- which the sanitizer legs exist to
# exercise, not to re-package.
SRC_PROBE_SOURCES = [
    "//src/ast:probe_sources", "//src/codegen:probe_sources",
    "//src/compiler:probe_sources", "//src/comptime:probe_sources",
    "//src/errors:probe_sources", "//src/lexer:probe_sources",
    "//src/package:probe_sources", "//src/parser:probe_sources",
    "//src/runtime:probe_sources", "//src/types:probe_sources",
    "//:src_orphan_sources",
]

# ARC header-feasibility audit rider: only goo_alloc/goo_realloc/goo_free may
# name a libc allocator, and no bare strdup may appear anywhere in src/. Pure
# source scan, no compiler needed.
goo_script_probe(
    name = "alloc_doors_probe",
    script = "//:scripts/alloc_doors_probe.sh",
    data = SRC_PROBE_SOURCES,
    needs_compiler = False,
)
goo_script_probe(
    name = "alloc_doors_selftest",
    script = "//:scripts/alloc_doors_probe.sh",
    args = ["--self-test"],
    data = SRC_PROBE_SOURCES,
    needs_compiler = False,
)

# The factual claims our documents make about this codebase, re-checked
# against the tree: goostd package classification, the shim set in goo.c, the
# /proc/self/exe site count, and that `make install` still packages rather
# than a bare cp. No self-test target: doc_claims_probe.sh supports
# --self-test, but no Makefile target ever passes that flag, so there is no
# make gate to mirror here.
goo_script_probe(
    name = "doc_claims_probe",
    script = "//:scripts/doc_claims_probe.sh",
    data = SRC_PROBE_SOURCES + [
        "//:Makefile",
        "//:probe_scripts",
        "//goostd:files",
    ],
    needs_compiler = False,
)

# proof_cache_create() must not hand its argument to a shell. No self-test:
# proof_cache_shell_probe.sh has no --self-test mode at all.
goo_script_probe(
    name = "proof_cache_shell_probe",
    script = "//:scripts/proof_cache_shell_probe.sh",
    data = [
        "//src/types:probe_sources",
        "//include:probe_headers",
    ],
    needs_compiler = False,
    needs_cc = True,
    tags = ["nosan"],
)

# ast_node_free() must free the whole tree, not only the spine. Compiles a
# small parse-and-free driver and runs it under valgrind.
_AST_FREE_LEAK_DATA = SRC_PROBE_SOURCES + [
    "//include:probe_headers",
    # parser.tab.c is named twice on purpose: parser_tab_files stages BOTH
    # generated files (the .c the script compiles and the .h it #includes),
    # while the single-file parser.tab.c label is what $(rootpath) below
    # actually expands -- a two-file filegroup cannot be that label's
    # target (measured: "not a declared prerequisite of this rule").
    "//src/parser/gen:parser_tab_files",
    "//src/parser/gen:parser.tab.c",
    "//tests/unit:ast/ast_free_leak_test.c",
]
_AST_FREE_LEAK_ENV = {"PARSER_TAB_C": "$(rootpath //src/parser/gen:parser.tab.c)"}

goo_script_probe(
    name = "ast_free_leak_probe",
    script = "//:scripts/ast_free_leak_probe.sh",
    data = _AST_FREE_LEAK_DATA,
    env = _AST_FREE_LEAK_ENV,
    needs_compiler = False,
    needs_cc = True,
    tags = ["nosan"],
)
goo_script_probe(
    name = "ast_free_leak_selftest",
    script = "//:scripts/ast_free_leak_probe.sh",
    args = ["--self-test"],
    data = _AST_FREE_LEAK_DATA,
    env = _AST_FREE_LEAK_ENV,
    needs_compiler = False,
    needs_cc = True,
    tags = ["nosan"],
)

# The FIRST automated execution of resolver tier 2: an extracted, packaged
# toolchain must compile and run a program that imports a vendored package,
# with no environment variable set and from an unrelated directory. Needs
# the compiler and the runtime archive (needs_compiler defaults True),
# scripts/package_toolchain.sh and scripts/check_stdlib_coverage.sh, and the
# stdlib it packages. No self-test target: release-package-probe has no
# make gate that passes --self-test.
goo_script_probe(
    name = "release_package_probe",
    script = "//:scripts/release_package_probe.sh",
    data = [
        "//:probe_scripts",
        "//goostd:files",
    ],
)

# NOT DECLARED HERE. 7 of the 29 script gates are not one of the targets
# above, each for a measured reason rather than an assumption, held in
# docs/superpowers/plans/2026-08-25-bazel-phase4-probes.md, task 14. All
# seven cannot run in a sandbox at all: podman_image_probe.sh exits 0 with
# no podman, so a sandboxed target would be green while asserting nothing;
# repro_build_probe.sh and archive_determinism_probe.sh each rebuild
# `git archive HEAD`, and a sandbox has no .git; far-collective, far-halo,
# far-jacobi and far-stencil-r2 share a helper that takes arguments and
# need NNG (phase 3c).
#
# Declaring these without their data would give red targets, and tagging
# them manual would give targets that never run -- both worse than
# recording the gap here.
