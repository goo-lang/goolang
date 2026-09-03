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
    needs_compiler = False,
    needs_cc = True,
    data = ["//include:goo_assert.h"],
    tags = ["nosan"],
)
goo_script_probe(
    name = "goo_check_probe",
    script = "//:scripts/goo_check_probe.sh",
    needs_compiler = False,
    needs_cc = True,
    data = ["//tests/unit:goo_check.h"],
    tags = ["nosan"],
)
goo_script_probe(
    name = "goo_check_selftest",
    script = "//:scripts/goo_check_probe.sh",
    args = ["--self-test"],
    needs_compiler = False,
    needs_cc = True,
    data = ["//tests/unit:goo_check.h"],
    tags = ["nosan"],
)
goo_script_probe(
    name = "goo_testcase_probe",
    script = "//:scripts/goo_testcase_probe.sh",
    needs_compiler = False,
    needs_cc = True,
    data = ["//include:goo_assert.h", "//:scripts/testcase_report.sh"],
    tags = ["nosan"],
)
goo_script_probe(
    name = "goo_testcase_selftest",
    script = "//:scripts/goo_testcase_probe.sh",
    args = ["--self-test"],
    needs_compiler = False,
    needs_cc = True,
    data = ["//include:goo_assert.h", "//:scripts/testcase_report.sh"],
    tags = ["nosan"],
)

# NOT DECLARED HERE. 15 of the 29 script gates are not one of the targets
# above, each for a measured reason rather than an assumption, held in
# docs/superpowers/plans/2026-08-25-bazel-phase4-probes.md, task 14:
#
#   7 cannot run in a sandbox at all -- podman_image_probe.sh exits 0 with no
#     podman, so a sandboxed target would be green while asserting nothing;
#     repro_build_probe.sh and archive_determinism_probe.sh each rebuild
#     `git archive HEAD`, and a sandbox has no .git; far-collective,
#     far-halo, far-jacobi and far-stencil-r2 share a helper that takes
#     arguments and need NNG (phase 3c).
#   5 read or compile src/** and need ten per-package filegroups first -- a
#     glob does not cross a package boundary, so there cannot be a single
#     //:c_sources the way //goostd:files covers the stdlib: alloc_doors,
#     ast_free_leak, doc_claims, proof_cache_shell, release_package.
#   3 compile C fixtures against include/ or tests/unit/ through a shell C
#     compiler ($CC, gcc): goo_assert, goo_check, goo_testcase. They need
#     those two filegroups and a C compiler the sandbox can see.
#
# 7 + 5 + 3 = 15.
#
# Declaring these without their data would give red targets, and tagging
# them manual would give targets that never run -- both worse than
# recording the gap here.
