# Verification scope

Native execution is the basis for QFuse's functionality claims. The current
profile has run on QNX 8.0 x86_64 and aarch64le under QEMU, with the owner's SDK
and private runtime. Both Linux build userlands use the same owner's host;
clean-clone rebuilds are not independent developer adoption.

## Processor configuration

Historical evidence predates the explicit SMP run and used one guest CPU;
receiver/worker counts in those transcripts are server threads, not processors.
The current lab defaults to four CPUs with multi-threaded TCG. Verify the actual
processor count with `pidin info` inside each guest. Held-callback tests prove
overlap and unrelated progress; append tests exercise serialization on one file.
No test asserts simultaneous callback execution on distinct CPUs. These are emulated SMP
functional checks, not physical-board or hardware-latency measurements.

[smp.txt](../proof/smp.txt) records completed native, namespace, saturation,
large-copy and SSHFS gates on both four-processor guests, plus the existing x86
bindfs port and shared-folder executable loading. It qualifies the failed setup
attempts and cleanup. The current source hashes and prior build manifests match;
the SMP run reused those binaries rather than performing another rebuild.

## Functional gates

- 51 named native-versus-mounted cases compare semantic returns, exact errno,
  deterministic bytes, sizes, owner/group/mode, link counts, explicit timestamps
  and selected filesystem geometry. Descriptor numbers are normalized. Device/
  inode identity, automatic timestamps and directory sizes are excluded.
- Separate permission, concurrency and lifecycle probes test authenticated
  clients, parent search/sticky permissions, appends, stable handles, native
  execution, unmount and process-death recovery.
- The 5,000-file gate uses actual native cp and diff, plus directory continuation,
  inode consistency, handle lifetime and remount checks.
- Stalled-callback probes hold an upstream read, require unrelated/default and
  priority-50 directory access, signal/kernel-timeout errors before release,
  descriptor close/exit and suppression of late replies to subsequent requests.
- Admission saturation fills all 32 payload slots and confirms kernel REPLY
  states before checking timeout errors. Read and open waiters are cancelled,
  then unrelated default/priority-50 access and absence of stranded mutex
  waiters are checked before releasing the original callback. Descriptor
  inheritance remains serviceable.
- Management probes verify kernel peer identity, root authorization, stale nonce
  rejection, bounded RPCs to a stopped process and graceful idle stop.
- Authenticated SSHFS tests cover readonly/writable operations, 200,037-byte data,
  larger partial I/O, wrong-host-key rejection, eight concurrent namespace
  clients and persisted remount.
- Namespace probes hold a read, mkdir or rename callback while sibling
  mkdir/create/write/rename/read/unlink workflows finish at default priority and
  priority 50. Queued condition waits are cancelled before the holder returns;
  queued descriptor reads survive parent rename; a queued truncate writer runs
  before a later reader observes EOF. An independently opened handle is closed
  during a held rename, and an unrelated release callback must still complete.
  Graceful stop preserves the active callback and queued reader until explicit
  release, then drains safely. Healthy serialized closes wait for their flush;
  supplied flush errors propagate. After interrupted I/O, close defers a
  deliberately stalled flush, returns `EAGAIN` with the fd demonstrably closed,
  and the flush executes and completes later in cleanup. The portable scheduler's FIFO/cancellation,
  atomic reservation, alias fallback and resource bounds also run on the host
  and QNX.
- The live workflow executes ELF binaries through SSHFS, changes laptop-side
  source/build output without target deployment copying, pauses SFTP, interrupts
  a reader, queries independent control, force-stops and remounts.

Reproduction on a prepared native target uses `scripts/run-conformance.sh` modes
`--local`, `--full`, `--stall`, `--saturation`, `--namespace`, `--readonly-sshfs` and
`--writable-sshfs`. The SFTP fixture profiles share a port and must run separately.
The live-share lab also uses that isolated port and must not overlap them.

`make check` runs host callbacks under address/undefined sanitizers and tests the
SDK-tool crash supervisor. It is not a filesystem mount test. Source/package
verification compiles locked upstream examples using installed pkg-config flags.

## Cancelled-waiter admission regression

The original saturation gate checked replies, then released the held callback.
It missed workers still blocked on the per-file operation mutex. The stronger
regression failed against the pre-fix ARM server: 32 cancelled clients returned,
but an unrelated open still failed with `EAGAIN` while the callback stayed held.
Both the read-lock and connect/open paths reproduced this admission exhaustion.
The `cancelled-waiters-before-*-arm.txt` files preserve those expected failures.

The updated read and open variants run on both architectures. While the original
callback remains held, all clients return their expected timeout/signal errors,
an unrelated file works at default priority and priority 50, and `pidin` shows
no stranded mutex waiters. No extra held read callback is entered. See
`cancelled-waiters-arm.txt` and `cancelled-waiters-x86.txt`.

This reclaims cancelled requests waiting for an operation lock. Requests already
executing foreign callbacks retain their slots, locks and buffers until those
callbacks return. Many independently stalled callbacks can still exhaust the
finite budget. This is not a claim of unlimited availability under arbitrary
filesystem failures or unlimited progress when shared backing dependencies are stalled.

## Honest limits

Functional deadlines are assertions on the QNX monotonic clock in controlled
QEMU scenarios. No elapsed performance numbers, hardware latency guarantees or
safety certification are claimed. Cancellation does not roll back side effects
or stop arbitrary foreign code.

A few local runs encountered SSH handshake setup timeouts before their next
native command began. Connection setup allowance was raised independently of
filesystem deadlines; missing gates were run separately. Historical failures
remain in the private development record.

Independent-person builds, real-world adoption and upstream acceptance must be
reported only when an outside participant actually provides them. Public CI
host checks and another container do not substitute for those outcomes.

## Reproduce the recording

With the optional live lab prepared and an idle ARM mount, run
`scripts/record-demo.py build/demo/live.cast -- python3 scripts/demo-live.py`
from the host, setting `QFUSE_DOCKER_CONTEXT` and `QFUSE_ARCH` as needed.
The recording captures actual command output, edits only the generated example,
restores its source afterward, and restores SFTP if the demo exits while paused.
`docs/proof/live-current.txt` is the captured successful transcript.

The optional `scripts/render-demo.py` uses Pillow and ffmpeg to make a captioned
MP4. Supply `--font` for a local monospace font on Linux. It labels deliberately
slowed playback; the video is a functional demonstration, not a benchmark.

The current ARM 5,000-file transcript includes native cp and diff. An x86 rerun
under nested host emulation was manually stopped to free the target for final
checks; its partial copy is not counted as a passed run. Historical transport or
SDK-tool crashes are not silently treated as native test passes.

Public host CI has passed. The [upstream recipe PR](https://github.com/qnx-ports/build-files/pull/334)
is open, and [independent reproduction](https://github.com/gowtham0992/qfuse/issues/1)
is requested. Neither an open PR nor the author's own extra userland build is
an accepted contribution or an independent user result.

## Current source and native package

`core-builds.json` records the core source hashes of the current native builds.
`namespace-arm.txt` and `namespace-x86.txt` cover scoped transactions, cancellation,
cleanup independence, writer ordering, graceful shutdown and close/flush error
handling. The `namespace-before`, `namespace-cleanup-before`, `serial-close-before`
and `interrupted-close-before` records are expected negative controls on earlier
builds. Historical walkthroughs and clone records retain their original scope.

`native-sdk-build.txt` summarizes conventional QNX make build/staging, installed
pkg-config checks, header-only installation and safe uninstall. The
`native-sdk-runtime-*` records run the named 51-case oracle using unchanged
passthrough linked from that installed native package, on each QEMU guest.
The proposed upstream SDK workflow is a compile/package job; it is not a claim
that public GitHub CI has booted QNX or that a maintainer has approved the job.

The native recipe has four host regressions for recursive staging configuration.
They model SDK defaults overriding exported variables and verify environment and
command-line staging, the private default, and rejection of an SDK destination.
The regression failed on the old recipe; the SDK dry run also checks all six
binary install destinations.

A clean checkout of the published staging fix and upstream fork recipe also
built and verified both architectures in Ubuntu 24.04, with every output in the
private staging root and no QFuse products under the SDK. This remains the
author's own SDK, not independent-user reproduction. One earlier attempt failed
in the host emulator and is not counted as a successful build.

## Guest payload hygiene

`scripts/deploy-completion-tests.sh` names the current applications and native
probes. `exec-probe` is a maintained helper built from `tests/exec_probe.c` and
loaded through the mount by `tests/test_lifecycle.c`; it is not an obsolete
control binary. Optional bindfs and its probe come from the bindfs recipe.

Historical `second-*`, clone/package-check copies and pre-fix control binaries
have been removed from the guests. Repeating an optional clean-clone or package
check requires explicitly deploying its freshly built outputs; the current suite
does not rely on historical binaries being left on the target. Historical
transcripts retain their recorded scope. The licensed SDK, private runtime and
current build outputs remain in the owner's private environment.
