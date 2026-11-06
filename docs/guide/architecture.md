# Native QNX implementation

QFuse links a selected high-level FUSE filesystem into a QNX resource-manager
process. Applications use native connect/I/O messages; no Linux FUSE device
transport is involved.

| Component | Responsibility |
| --- | --- |
| src/core.c | Typed callback dispatch, argument/bounds validation and request-local FUSE context. |
| src/qnx_backend.c | Namespace nodes, OCBs, native attribute integration, reads, metadata and lifecycle. |
| src/qnx_mutations.inc | Writable/namespace operations, symlink redirection, statfs and native controls. |
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

Namespace changes serialize under a mount-wide exclusive gate. Existing-file
I/O and metadata hold its shared side across callbacks. Consequently a stalled
read blocks all namespace mutations, and a stalled namespace mutation blocks
other payload operations on the mount. Client waits remain interruptible. The
current namespace try-lock loop uses short sleeps; writer fairness and an
event-driven namespace wait remain separate limitations. Descriptor duplication, paging-handle setup,
close and local controls avoid waiting on a file's operation mutex. Any
control path attempting a foreign callback must acquire admission and a safe
namespace scope first.

The SDK retains live message bindings. Physical OCB references also pin namespace
nodes against garbage collection. Final releases are published only after the
SDK dispatcher unwinds, then processed by a dedicated cleanup worker. A release
callback cannot consume the request's old receive ID or hold an IPC receiver.

## Cancellation and deadlines

The connect and I/O unblock handlers check `MsgInfo()` and the kernel's pending
unblock flag before returning the appropriate signal/timeout error. A separate
monitor provides a receiver-independent fallback. Requests remain registered
until their SDK dispatch completes; cancelled contexts cannot reply to a later
send reusing a receive ID. Private I/O buffers remain alive while callbacks run.

Threads waiting for a per-file operation mutex use QNX's
`pthread_mutex_clocklock` with short `CLOCK_MONOTONIC` deadlines. This blocks in
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
