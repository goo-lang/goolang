# Build image for reproducible goolang builds.
#
# PINNED BY DIGEST, NOT TAG. A tag is a moving pointer, so `ubuntu:24.04` today
# and in six months are different images and would produce different binaries.
#
# The package list matches .github/workflows/tests.yml exactly, so this image
# reproduces the environment CI already validates rather than adding a fourth
# toolchain to reason about.
FROM docker.io/library/ubuntu@sha256:1e0a86e57d247923571b75e0aaf48a1449cf8c543d51fb3e07a4a7d7bfa79316

# ccache is deliberately ABSENT. See scripts/podman_image_probe.sh for why.
RUN apt-get update && apt-get install -y --no-install-recommends \
        make gcc-14 bison llvm-dev clang libblocksruntime-dev \
        valgrind cmake python3 \
        libjson-c-dev libcurl4-openssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Record what actually resolved. A build that cannot say what produced it is
# hard to audit later.
#
# record_toolchain.sh runs under `set -euo pipefail` and checks each command's
# own exit status, so a broken tool fails THIS BUILD, not just the file it
# writes. An inline `RUN { cmd1; cmd2; } > file` group has no `set -e` of its
# own — one broken command inside it used to write nothing for that line, and
# the build still succeeded with a shorter, plausible-looking file.
COPY scripts/record_toolchain.sh /usr/local/bin/record_toolchain.sh
RUN chmod +x /usr/local/bin/record_toolchain.sh \
    && /usr/local/bin/record_toolchain.sh \
    && rm -f /usr/local/bin/record_toolchain.sh

WORKDIR /src
