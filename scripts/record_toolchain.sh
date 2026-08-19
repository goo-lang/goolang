#!/bin/bash
# Writes /etc/goo-toolchain.txt during the podman-image build.
#
# A Containerfile `RUN { cmd1; cmd2; ...; } > file` group has no `set -e` of
# its own: one broken command inside it writes nothing for that line, and the
# group still exits 0, so the build succeeds with a shorter, plausible-looking
# file. This script fails LOUDLY instead — non-zero exit, a message naming the
# tool — so a broken toolchain component fails the image build, not just the
# audit trail.
#
# scripts/podman_image_probe.sh checks that each tool's line is present by
# name, not merely that the file is non-empty, so the two fixes cover both
# ends: this script stops a broken build from producing a bad file, and the
# probe would still catch a bad file if one got through some other way.
set -euo pipefail

OUT=/etc/goo-toolchain.txt

record() {  # $1 = tool label (used as the line prefix and in error messages), $@ = version command
    local tool="$1"; shift
    local full first
    if ! full="$("$@" 2>&1)"; then
        echo "goo-toolchain: FAILED to record $tool (command failed: $*)" >&2
        printf '%s\n' "$full" >&2
        exit 1
    fi
    first="${full%%$'\n'*}"
    if [ -z "$first" ]; then
        echo "goo-toolchain: $tool produced no output ($*)" >&2
        exit 1
    fi
    printf '%s: %s\n' "$tool" "$first" >> "$OUT"
}

echo "# resolved $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$OUT"

record gcc-14 gcc-14 --version
record clang clang --version
record bison bison --version
record valgrind valgrind --version
record cmake cmake --version
record python3 python3 --version

llvmcfg=$(ls /usr/bin/llvm-config-* 2>/dev/null | sort -V | tail -1 || true)
if [ -z "$llvmcfg" ]; then
    echo "goo-toolchain: no /usr/bin/llvm-config-* binary found" >&2
    exit 1
fi
record llvm-config "$llvmcfg" --version

if ! dpkg-query -W -f='${Package}=${Version}\n' make gcc-14 bison llvm-dev clang cmake python3 >> "$OUT"; then
    echo "goo-toolchain: dpkg-query failed to resolve one or more packages" >&2
    exit 1
fi
