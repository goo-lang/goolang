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

# NOT MIGRATED, and each for a measured reason rather than an assumption. All
# seven pass under make; all seven fail in a sandbox, and the causes differ:
#
#   arc_concat_operand_probe   |
#   arc_map_key_local_probe    | read examples/<name>.goo after `cd "$ROOT"`,
#   arc_multi_assign_probe     | and still do not find it under runfiles
#   arc_reassign_probe         |
#   arena_rss_probe            reads tests/examples/, a SECOND fixture tree
#   goo_test_probe             needs whole test PACKAGES with expected.norm.txt
#   dead_package_code_probe    imports "strings", so it needs GOOROOT, which
#                              the macro deliberately does not set
#
# The COMPILER contract is necessary for these and not sufficient. Each wants
# its own data, and one wants a GOOROOT the macro cannot name because
# //goostd:files is a filegroup $(rootpath) will not expand. Declaring them
# without that data would give seven red targets, and tagging them manual would
# give seven that never run -- both worse than recording the gap here.
