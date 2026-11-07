# Native QNX implementation

QFuse links a selected high-level FUSE filesystem into a QNX resource-manager
process. Applications use native connect/I/O messages; no Linux FUSE device
transport is involved.

| Component | Responsibility |
| --- | --- |
| src/core.c | Typed callback dispatch, argument/bounds validation and request-local FUSE context. |
| src/qnx_backend.c | Namespace nodes, OCBs, native attribute integration, reads, metadata and lifecycle. |
| src/qnx_mutations.inc | Writable/namespace operations, symlink redirection, statfs and native controls. |
| src/path_gate.inc | Bounded FIFO path leases, atomic multi-path reservation and cancellation wakeups. |
| src/qnx_requests.inc | Short state locking, callback scheduling hooks, request identity and cancellation. |
| src/qnx_pool.inc | Dispatch admission, namespace transactions, worker/monitor lifecycle and draining. |
| src/qnx_guards.inc | Typed SDK handler boundaries and reply suppression for cancelled requests. |
| src/qnx_admin.inc | Root-authorized administration on a separate native channel. |
| src/mount.c | Background launch, readiness, instance records, status and safe stop. |

## Locking and ownership

SDK attribute callouts map to a recursive priority-inheriting state mutex.
Per-file operation mutexes serialize open, stat and I/O across OCBs. State locks are
released around foreign callbacks, while the operation's namespace gate protects
its path references. Directory fillers reenter only short protected state
sections. No arbitrary third-party callback thread is forcibly cancelled.

Namespace transactions reserve affected canonical paths through a private,
portable scheduler. A read lease conflicts with an overlapping write lease;
ancestor/descendant paths overlap, while siblings do not. Rename acquires source
and destination in one grant. A queued conflicting writer keeps its place when
later readers arrive. Unrelated requests can bypass that queue. Waits use
condition variables, with wakeups on lease release or request cancellation.

The former global read/write gate coupled every mutation to every in-flight
read. Copying callback path strings alone would leave rename/open-handle
semantics unresolved. Scoped leases protect both object identity and path
lifetimes while permitting independent transactions. The scheduler caps its
lease count and path-key storage; it executes no filesystem callback while its
short mutex is held.

Descriptor requests copy a node path under the state mutex, wait without that
mutex, then revalidate the path before using it. A parent rename can therefore
move a queued request's node safely. Hidden-name transactions make a nonblocking
additional reservation before renaming an open file; they retain that reservation
through rollback or commit. Final hidden cleanup obtains the appropriate write
lease and recomputes it if the node moved while waiting.

Directory snapshots reserve a subtree during collection. Noncanonical spellings
use a conservative whole-namespace scope. Single-thread mode also uses a global
exclusive lease. Ordinary descriptor duplication, paging-handle setup and local
controls bypass namespace waits. Close bookkeeping bypasses the per-file
operation mutex. Healthy single-thread closes queue for their flush; other
control callbacks obtain a nonblocking safe scope before invoking foreign code.
Interrupted I/O atomically marks its shared open handle before the client is
unblocked. A later close on a handle with a flush callback defers that callback,
reports `EAGAIN` and closes the descriptor, so cleanup cannot trap the caller in
a second uncooperative callback. Deferred flush executes after the retained
operation unwinds. Ordinary synchronous flush errors remain visible. Shared backend
locks and remote transports remain possible sources of blocking.

The SDK retains live message bindings. Physical OCB references also pin namespace
nodes against garbage collection. Final releases are published only after the
SDK dispatcher unwinds, then processed by a dedicated cleanup worker. It tries
queued paths without blocking, skipping namespace-conflicted jobs so unrelated
releases can complete. Queue arrivals and namespace changes wake it through a
condition variable and an epoch check, closing the scan-to-sleep lost-wakeup
race. A release callback cannot consume the request's old receive ID or hold an
IPC receiver. A foreign release callback that itself fails to return still
retains the cleanup worker; process-level recovery remains the boundary.

## Cancellation and deadlines

The connect and I/O unblock handlers check `MsgInfo()` and the kernel's pending
unblock flag before returning the appropriate signal/timeout error. A separate
monitor provides a receiver-independent fallback. Requests remain registered
until their SDK dispatch completes; cancelled contexts cannot reply to a later
send reusing a receive ID. Private I/O buffers remain alive while callbacks run.

Threads waiting for a per-file operation mutex use QNX's
`pthread_mutex_clocklock` with 10 ms `CLOCK_MONOTONIC` deadlines. This blocks in
the kernel with the mutex's priority-inheritance protocol, while allowing a
cancelled waiter to unwind without acquiring the mutex. Both the OCB-dispatch
and connect/open paths use it. A thread already inside a foreign callback keeps
its admission slot, buffers and lock until the callback returns; these cannot be
reclaimed safely merely because the client stopped waiting.

The default admission budget reserves dispatcher capacity for descriptor and
control work. A full budget produces backpressure rather than adding unbounded
callback jobs. The limits and cancellation qualifications are in
[the supported profile](profile.md).

## Administration

The launcher receives the process's management-channel identity through an
inherited pipe. It validates kernel peer PID/channel and a random instance nonce.
Subsequent commands use direct native messaging and never open the mounted tree.

Management packets have fixed bounds and require local root credentials. State
records live in a root-owned private native directory outside the mount. A stale
record cannot stop a different instance. Each RPC uses a fresh connection; its
channel permits kernel timeout handling even if the filesystem process is
stopped. A management command invokes no filesystem callbacks.

Ordinary stop respects live handles. Explicit force first requests shutdown,
then can terminate the identified instance and its owned process group. Graceful
shutdown drains callbacks and deferred release before destroying application
state. Uncooperative code may require force.

## Execution and identity

Native executable loading uses `iofunc_mmap_default` and retained FUSE handles
across pager OCBs. Read permission exceptions require authenticated loader flags
or the local system manager. Ordinary execute-only clients cannot read file data.
Each filesystem leases its own native filesystem device identity. Namespace nodes
preserve supported open-handle behavior over rename/unlink/replacement.
