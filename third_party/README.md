# third_party

Vendored dependencies. Policy: pinned release tarballs (sha256 recorded in
the Makefile or the bootstrap script that builds them, and in this table),
extracted and built into `build/` at compile time — source trees are never
committed, only tarballs.

| Dependency | Version | File | sha256 |
|---|---|---|---|
| NNG (nanomsg-next-gen) | v1.12.0 | nng-1.12.0.tar.gz | `50b7264bd8f0901f7ebdf3ec7c48f4e23dd689bbe7b2917d9d8fad58ffd09e5c` |
| SPIN model checker | version-6.5.2, commit 4883fbb | spin-6.5.2.tar.gz | `e46a3bd308c4cd213cc466a8aaecfd5cedc02241190f3cb9a1d1b87e5f37080a` |

NNG backs the `far` shim package (lanes M2-B1 far transport). MIT license.
The build deliberately ignores any system/`/usr/local` nng install.

Note: upstream's v1.12.0 tag ships an unbumped version header —
`include/nng/nng.h` still has `NNG_MINOR_VERSION 11`, so the built
library's `nng_version()` self-reports `1.11.0` at runtime, not `1.12.0`.
Confirmed against GitHub's tag directly (not a local extraction fluke).
Provenance is the sha256-pinned tag tarball above, not the runtime
self-report.

SPIN backs Track B's Promela model of the runtime deadlock detector (tasks
5-7 of the 2026-09-02 verification tooling plan; not yet added on this
branch). BSD 3-Clause license. `scripts/spin_bootstrap.sh` checks the
sha256 above, and is the only place that builds it — no Makefile recipe.
Fedora ships no SPIN package, so this tarball is the only source; CI reads
no network, so a bootstrap that only used a PATH `spin` would be a gate
that silently skips itself on a machine with none installed.

The tarball is GitHub's archive of tag `version-6.5.2` of
github.com/nimble-code/Spin, commit 4883fbb: 6,084,903 bytes, confirmed
with `ls -la` on 2026-09-02. Its sha256 above is confirmed with
`sha256sum`, same date, both before and after the copy into this
directory. Its top directory is `Spin-version-6.5.2/`; the source is
`Src/` (a `makefile` and a `LICENSE`); `Bin/` holds prebuilt binaries the
bootstrap does not use.

MEASURED 2026-09-02, gcc 16.2.1 (Red Hat 16.2.1-2), this tag: it does not
build with gcc 14 or later as shipped — `Src/spin.y:1099` calls `free` with
no `<stdlib.h>` in scope, and gcc 14+ makes an implicit function
declaration an error by default, where the makefile's own
`CFLAGS?=-O2 -DNXT -Wall -pedantic` does not disable that. Built with
`CFLAGS="-O2 -DNXT -Wall -pedantic -Wno-error=implicit-function-declaration"`
and timed with the shell's `time` builtin, same date: under one second
wall-clock on a 32-core machine, `-j$(nproc)`. The result reports
`Spin Version 6.5.2 -- 6 December 2019` for `spin -V`. This is the exact
flag `scripts/spin_bootstrap.sh` uses.
