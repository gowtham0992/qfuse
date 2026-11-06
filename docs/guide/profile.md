# Supported profile

QFuse provides source compatibility for a tested subset of high-level FUSE API
31 on QNX 8.0. The verified targets are x86_64 and aarch64le under QEMU. Physical
boards and independent developer reproduction require their own verification.

| Area | Supported behavior |
| --- | --- |
| Mounting | Root, canonical absolute mount paths, one filesystem per process. Foreground execution, with optional background supervision through qfuse-mount. |
| Objects | Regular files, directories and symlinks. |
| Operations | getattr, readdir, open/create/read/write/truncate, unlink/mkdir/rmdir/rename, symlink/readlink, chmod/chown, utimens, statfs and supplied flush/fsync callbacks. |
| Handles | Read/write offsets, pread/pwrite, descriptor duplication, stable handles over supported rename/unlink/replacement and loader/pager references. |
| Credentials | Kernel-authenticated caller UID/GID/PID, parent search permissions and native owner/group/mode checks. Set-user-ID execution is disabled. |
| Execution | Native ELF loading through QNX's mmap helper. Actual passthrough and SSHFS execution are tested. |
| Failure | Client signal/timeouts, ordinary busy/idle unmount, explicit forced detach and process-level recovery. |

## Resource and concurrency limits

The default profile admits 32 ordinary requests, retains at most 1,024 physical
OCBs including deferred cleanup, indexes at most 8,192 namespace nodes, limits a
directory snapshot to 1 MiB and individual data messages to 64 KiB. Larger client
transfers use normal partial I/O. Admission exhaustion returns `EAGAIN`; physical
OCB exhaustion returns `EMFILE`. The request pool can grow to 64 threads, leaving
capacity for descriptor lifecycle and control messages.

Callbacks on different files may run concurrently. Same-file I/O is serialized
by a priority-inheriting operation mutex. Namespace mutations hold an exclusive
transaction gate; conflicting operations can wait, and their waits remain
interruptible. `-s` preserves callback serialization while cancellation and
administration remain separate.

A cancelled client wait does not terminate the foreign callback. Private buffers,
SDK bindings and file handles remain owned until the operation unwinds. Writes
may have an uncertain outcome after cancellation. Callers must not blindly retry
side-effecting operations. Explicit flush/fsync errors are reported; deferred
release cannot report errors to a descriptor that has already closed.

Under pressure or while single-threaded callbacks are busy, close may return
`EAGAIN` after deferring a flush. The descriptor is already closed; do not retry
its numeric fd. Deferred cleanup retains the handle and attempts the pending
flush before final release.

## Boundaries

Low-level FUSE, non-root mounting, xattrs, advisory locking, generic ioctl/poll,
hard-link/device/FIFO creation, general writable mmap and transparent SSHFS
reconnect recovery are outside the supported profile. Linux FUSE caching and
configuration semantics are not generally emulated. Unknown mount options are
rejected. Supported compatibility options include `use_ino` (honor nonzero
callback inode numbers), `allow_other` (native permissions already govern
access), and exact zero `attr_timeout`, `entry_timeout` and `negative_timeout`
values (metadata is refreshed). Positive Linux cache TTLs are not implemented.

The verified SSHFS profile disables directory cache/readahead and uses synchronous
readdir. The passthrough example mirrors the backing namespace and does not
confine exports. Out-of-band backing-object replacement has no general identity
or race guarantee; the live build workflow uses immutable versioned executables
and atomically replaced relative symlinks.

Functional deadline assertions in QEMU are not hardware latency measurements,
performance evaluations, hard-real-time guarantees or safety certification. A
third-party filesystem must support concurrent callbacks for multithreaded use.
An uncooperative callback can delay graceful shutdown; explicit process killing
is the recovery boundary.
