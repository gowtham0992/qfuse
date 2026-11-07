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
transfers use normal partial I/O. Admission exhaustion returns `EAGAIN`. New opens fail when the physical
OCB budget is exhausted. The request pool can grow to 64 threads, leaving
capacity for descriptor lifecycle and control messages.

Callbacks on different files may overlap. The tests demonstrate request
concurrency, callback overlap and clean four-CPU scheduling runs; no test asserts
simultaneous callback execution on distinct CPUs. Same-file I/O is serialized
by a priority-inheriting operation mutex, including open and stat on the same
file. Cancelled requests waiting for that mutex unwind and return their worker
and admission slot; a callback already running retains its resources until it
returns. A long listing (`ls -l`) of a directory containing a stalled file
can wait while it obtains that file's metadata, even though other files work.
Operation-mutex waiters use kernel timed waits with a 10 ms cancellation check;
the condition-variable namespace scheduler does not replace that separate retry.

Namespace operations use bounded, queued path leases. Reads share a path;
mutations reserve their affected path, and rename reserves source and destination
atomically. Ancestor/descendant overlap counts as a conflict. Conflicting queued
leases preserve arrival order, so new readers cannot continually bypass an
already queued writer. Unrelated paths, including siblings in one directory,
can proceed concurrently. A queued directory rename behind a stalled read
also queues later requests beneath that directory until it can proceed or its
wait is cancelled; this is the tradeoff for preventing writer starvation.

Namespace waiters sleep on condition variables and are woken on release or
cancellation. A queued descriptor request revalidates its node path after a
rename. Directory reads reserve their subtree while building a snapshot; a
conflicting child mutation can therefore wait for a slow directory callback.
Noncanonical path spellings conservatively reserve the whole namespace. `-s`
preserves callback serialization while cancellation and administration remain
separate.

The namespace scheduler bounds active/queued leases at 64, with up to three
path keys of at most 4,096 bytes each; normal requests remain subject to the
32-request admission budget. Shared locks or transports inside a third-party
filesystem can still block otherwise independent callbacks.

A cancelled client wait does not terminate the foreign callback. Private buffers,
SDK bindings and file handles remain owned until the operation unwinds. Writes
may have an uncertain outcome after cancellation. Callers must not blindly retry
side-effecting operations. Explicit flush/fsync errors are reported; deferred
release cannot report errors to a descriptor that has already closed.

With a supplied flush callback, an interrupted operation marks its open handle.
A later close defers that flush, returns `EAGAIN`, and leaves the descriptor
closed; do not retry its numeric fd. Deferred cleanup runs the pending flush
before final release, after retained operations have unwound. A handle without a
flush callback can close normally. Interruption history is shared by duplicate
handles and lasts for that open handle's lifetime.

Healthy single-threaded closes queue for their flush and report its actual
result. Their waits remain interruptible. Multithreaded closes or admission
pressure can also defer flush and return `EAGAIN` if a safe scope is unavailable.
Explicit fsync and ordinary synchronous flush errors remain visible to callers;
a deferred flush cannot report an error to an already closed descriptor.

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

The filesystem implementation and SSHFS/SSH dependencies execute as the mount
owner, currently root. Native caller-permission checks do not sandbox that
process. QNX security-policy integration for these servers has not been verified.
