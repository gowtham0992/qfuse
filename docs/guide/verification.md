# Verification scope

Native execution is the basis for QFuse's functionality claims. The current
profile has run on QNX 8.0 x86_64 and aarch64le under QEMU, with the owner's SDK
and private runtime. Both Linux build userlands use the same owner's host;
clean-clone rebuilds are not independent developer adoption.

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
  larger partial I/O, wrong-host-key rejection and persisted remount.
- The live workflow executes ELF binaries through SSHFS, changes laptop-side
  source/build output without target deployment copying, pauses SFTP, interrupts
  a reader, queries independent control, force-stops and remounts.

Reproduction on a prepared native target uses `scripts/run-conformance.sh` modes
`--local`, `--full`, `--stall`, `--saturation`, `--readonly-sshfs` and
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
filesystem failures or isolation for namespace mutations.

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
