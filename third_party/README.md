# third_party

Vendored dependencies. Policy: pinned release tarballs (sha256 recorded in
the Makefile and here), extracted and built into `build/` at compile time —
source trees are never committed, only tarballs.

| Dependency | Version | File | sha256 |
|---|---|---|---|
| NNG (nanomsg-next-gen) | v1.12.0 | nng-1.12.0.tar.gz | `50b7264bd8f0901f7ebdf3ec7c48f4e23dd689bbe7b2917d9d8fad58ffd09e5c` |

NNG backs the `far` shim package (lanes M2-B1 far transport). MIT license.
The build deliberately ignores any system/`/usr/local` nng install.

Note: upstream's v1.12.0 tag ships an unbumped version header —
`include/nng/nng.h` still has `NNG_MINOR_VERSION 11`, so the built
library's `nng_version()` self-reports `1.11.0` at runtime, not `1.12.0`.
Confirmed against GitHub's tag directly (not a local extraction fluke).
Provenance is the sha256-pinned tag tarball above, not the runtime
self-report.
