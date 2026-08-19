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
RUN { echo "# resolved $(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
      gcc-14 --version | head -1; \
      clang --version | head -1; \
      bison --version | head -1; \
      valgrind --version; \
      cmake --version | head -1; \
      ls /usr/bin/llvm-config-* 2>/dev/null | sort -V | tail -1 | xargs -r -I{} sh -c '{} --version' | sed 's/^/llvm-config /'; \
      dpkg-query -W -f='${Package}=${Version}\n' make gcc-14 bison llvm-dev clang cmake python3; \
    } > /etc/goo-toolchain.txt

WORKDIR /src
