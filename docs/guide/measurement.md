# One SSHFS small-file measurement

One author-run sample measured a 5,000-file synthetic tree (10 directories,
500 files each, 97 deterministic bytes per file; 485,000 bytes total).

| Operation | Native QNX tree | SSHFS tree |
| --- | ---: | ---: |
| `ls -lR`, output discarded | 0.507 s | 91.834 s |
| Sequential verified read loop | 2.632 s | 95.400 s |

The read loop opens every named file, checks all 97 bytes, checks EOF and closes
the descriptor. The native and remote data must match the published generator.
This is a small-file operation measurement, not large-file transfer throughput.
The listing phase ran first, followed by the read phase; native ran before SSHFS
within each phase.

## Environment and cache state

- Apple M4 Mac; Colima Linux ARM64 Docker host with four CPUs.
- QNX 8.0.0 aarch64le guest, four active Cortex-A57 vCPUs, 512 MiB RAM,
  native-host QEMU 8.2.2 with multi-threaded TCG.
- Native reference: newly generated files on the guest's QNX6 data filesystem.
- Server: native ARM64 Ubuntu 24.04 OpenSSH internal-SFTP container serving a
  read-only Mac bind-mounted synthetic folder. Host filesystem sharing is part
  of this setup's cost.
- Transport: QEMU user networking/guest forwarding through loopback and `nc`.
  There is no physical network, injected network delay, or CPU isolation.
- The normal QFuse launcher SSHFS profile disables directory cache/readahead and
  uses zero attribute/entry TTLs. No operating-system cache was flushed. Both
  fixtures had just been written; the SSHFS mount was fresh. Reads followed the
  listings, so this must not be described as a cold-cache benchmark.

The [timing helper](../../examples/measure_share.c) uses QNX
`CLOCK_MONOTONIC` around the native command or read loop. SSH connection setup,
fixture creation, mounting and cleanup are outside the timings. `ls` process
startup is inside the listing timing. No tuning or optimization was applied.
The [machine-readable record](../proof/share-measurement.json) includes the source
hash, dataset and nanosecond results. The QFuse core was reused unchanged from
public revision `9c4e672`; this was not a fresh core rebuild.

## Interpretation

Metadata-heavy walks and many small-file opens have substantial overhead in this
setup. This result does not support a fast build-tree traversal claim. Focused
file access and source portability remain the demonstrated use; real workload
suitability needs its own measurement. This single sample provides no average,
p95, hardware latency, real-network estimate, comparison with NFS, or evidence
about another server implementation.

## Reproduce the workload

Use a prepared [local live-share lab](live-workflow.md). Generate a dedicated
subdirectory under `build/live-share` on the host, with the following contents:

```python
from pathlib import Path
root = Path("build/live-share/measure-5000")
root.mkdir(mode=0o755)  # use a new name; do not overwrite existing data
for d in range(10):
    folder = root / f"d{d:02d}"
    folder.mkdir(mode=0o755)
    for f in range(500):
        p = folder / f"file-{f:04d}.bin"
        p.write_bytes(bytes((i*37 + (d*500+f)*19) & 255 for i in range(97)))
        p.chmod(0o644)
```

Compile `examples/measure_share.c` with the matching SDK `qcc` target and stage
the resulting helper privately on QNX. For this run the selector was
`-Vgcc_ntoaarch64le -std=c11 -O1 -Wall -Wextra -Werror`. Mount the live share, then
run the helper with the absolute mounted fixture path, for example:

```sh
timeout 900 /data/qfuse/measure-share /data/qfuse-laptop/measure-5000
```

The helper creates and normally removes its own native reference tree, printing
its path before timing. On an interrupted run, stop the owned mount/processes
and remove only that identified reference tree. Finally stop the share and
remove the generated host fixture and staged measurement helper. Do not leave
ad-hoc binaries on a shared verification target.
