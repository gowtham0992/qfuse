# Third-party sources

The dependency baseline is [libfuse fuse-3.16.2](https://github.com/libfuse/libfuse/tree/fuse-3.16.2).
`upstream.lock.json` lists every fetched file and its SHA-256 digest. Hashes
detect changes to these initial downloads; they are not an independent
verification of the upstream release signature.

The upstream `LICENSE` states that files under `include/` and `lib/` are
LGPL 2.1, while other files, including the hello example, are GPLv2.
The fetch script obtains `LICENSE`, `LGPL2.txt`, and `GPL2.txt` alongside the
sources in `.cache/libfuse/`. Source copyright notices remain intact.

The locked `passthrough.c` example retains GPLv2 terms. Its unmodified helper
header has its own two-clause BSD license notice. It builds unchanged with the
optional MIT directory-entry adapter; see `patches/README.md`.

The host build generates a copy of `lib/fuse_opt.c` that removes its includes
of `fuse_config.h`, `fuse_i.h`, and `fuse_misc.h`, and adds `fuse_log.h`.
This removes private build dependencies for the prototype; it changes no
option-parsing logic. It also generates an empty `libfuse_config.h` for the
callback fixture. That minimal configuration has now compiled and run the
callback tests on QNX 8.0; it does not cover the rest of the FUSE API.

The GPL hello fixture is linked into the host and QNX callback tests and the
native `qfuse-hello` demonstration executable. The passthrough example is also
linked into the `qfuse-passthrough` demonstration. These are not published release
artifacts. Any redistribution of combined executables must
preserve the applicable upstream terms and corresponding source. MIT applies
to original QFuse code, not to the fetched sources or combined executable.

No QNX SDK, header, binary, image, or license is fetched or redistributed here.

## SSHFS dependency baseline

`sshfs-dependencies.lock.json` pins SSHFS 3.7.3 sources and its GPLv2 text,
GLib 2.84.3, libffi 3.4.6, and the QNX port's atomic-library detection patch
at commit `1964dba24df0b5360bf98b817717f1c5b04224e5`. The one-line SSHFS
portability patch is GPL-2.0-only. The combined SSHFS executable must preserve
its applicable GPL terms and corresponding source if distributed.

GLib retains its LGPL 2.1-or-later notices; libffi retains its permissive
license in the downloaded release. GLib's Meson fallback downloads PCRE2
10.44 using the upstream wrap's hashes; retain its license texts as well.
Sources, dependency build trees and their license files remain intact in
ignored caches/private build storage. Meson 1.7.2 is a build tool installed
in a private Python environment. These are tested baselines, not latest-version
claims. SDK-provided runtime dependencies are staged only in the licensed
container and guest; they are not repository or public release artifacts.

The in-flight death fixture compiles the same locked, unchanged upstream
passthrough source with test-only entry-point and fuse_main symbol renaming.
Its wrapper holds one callback before reply for deterministic failure testing;
normal data reads still use the upstream callback. The combined fixture retains
the example's GPLv2 obligations, just like the passthrough demonstration.

The optional directory-entry adapter is original MIT QFuse code. It includes the
owner's SDK headers at build time and converts native entries; no SDK declarations
or structures are copied into the source distribution.

## Optional bindfs baseline

`bindfs.lock.json` pins bindfs 1.18.4 by commit and archive hash. Bindfs retains
GPL-2.0-or-later terms. The QNX patch adds `O_DIRECTORY` to its source-directory
open; the separate directory-entry adapter remains MIT. Build copies and combined
executables stay private. Redistributing a combined executable requires preserving
bindfs's applicable license and corresponding source.

The libffi build selects its existing executable-memory allocator using
`FFI_MMAP_EXEC_WRIT=1`; its generic malloc fallback cannot execute closures on the
tested QNX target. No configure test result is replaced by this port setting.
