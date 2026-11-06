# Port a high-level FUSE filesystem

Inventory the application's callbacks, helper symbols, mount options and external
dependencies first. Compare them with the [supported profile](profile.md). Start
with a read-only fixture and use native QNX clients to inspect actual data and
errors before enabling mutation.

Build and install QFuse using your own SDK. Existing build systems can query
`pkg-config fuse3 --cflags --libs` with `PKG_CONFIG_LIBDIR` set to the installed
package. Compile with the matching QNX target selector and API version 31.

The original hello example uses ordinary `fuse3` metadata. The original passthrough
example additionally selects `pkg-config fuse3 qfuse-dirent --cflags --libs`.
Its explicit optional header converts native directory entries into separately
owned records containing `d_type`; it never casts native memory to a Linux
layout. It reads the SDK's validated lstat extras and returns `DT_UNKNOWN` when
type information is unavailable. Apply the adapter only to translation units
using its documented direct-call interface.

The adapter supports opendir/readdir/closedir, dirfd, rewinddir, telldir and
seekdir. Function-pointer use, native QNX directory extras/control interfaces,
fdopendir, scandir, readdir_r and explicit 64-bit variants are outside that
adapter. Unsupported complete-directory metadata fills return ENOTSUP.

Preserve third-party licenses and keep source differences explicit. SSHFS 3.7.3
changes one guard to skip Linux transport-FD handling. Dependency/build work and
compatibility-layer code are separate from that small application source delta.

Use the native probes for actual mounts and permissions. `make check` verifies
portable callback behavior only. Check error/cancellation behavior while a real
callback is held, late completion after cancellation, busy/forced detach and
remount. Multithreaded applications must make their own state safe for concurrent
callbacks. Single-thread mode preserves callback serialization.

The local qnx-ports overlay builds the static library/package from a separate
source checkout. Maintainer review and external builds are independent evidence;
providing a recipe alone does not establish adoption or acceptance.

## Optional bindfs port

`make bindfs` fetches hash-locked bindfs 1.18.4, runs real SDK configuration,
and builds it with the installed package and directory adapter. It needs
Autoconf, Automake and Libtool on the Linux host. The small QNX patch selects
`O_DIRECTORY` for the backing directory descriptor. Bindfs requests FUSE API 34;
its exercised callbacks share the supported high-level operations. This does
not extend QFuse's general compatibility promise to all API 34 applications.

On QNX, after deploying the built `bindfs` executable:

```sh
qfuse-mount start /data/view -- /data/qfuse/bindfs --multithreaded -f --perms=a-w /data/backing /data/view
qfuse-mount status /data/view
qfuse-mount stop /data/view
```

This masks write permission bits in the view. It is not an export-security
boundary for root or a replacement for a read-only mount. Bindfs's own options,
unsupported callbacks and out-of-band backing changes require application-level
review before broader use.
