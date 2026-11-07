# Build and mount hello in your own environment

This is the shortest independent reproduction: compile the installed-package
hello example, mount it on QNX, read a file, and stop the filesystem. It needs
your own activated QNX SDP 8.0 on a Linux x86_64 host and a root shell on a matching
QNX 8.0 target. Docker, QEMU, SSHFS and the author's lab are optional.

## Build on the SDK host

Clone this repository and record its revision. Select your SDK and target CPU:

```sh
git rev-parse HEAD
export QNX_SDP_ROOT=/opt/qnx800
source "$QNX_SDP_ROOT/qnxsdp-env.sh"
export QFUSE_ARCH=x86_64             # or aarch64le
python3 scripts/doctor.py
make install
source scripts/target-config.sh
python3 scripts/verify-package.py
```

The checker reports availability, not license validity or successful execution.
The build downloads and verifies the pinned public fixtures. The last command
links hello and passthrough against the installed static library and headers.
Outputs are under `$QFUSE_INSTALL_PREFIX`: `verify/hello`, `bin/qfuse-mount` and
`bin/qfuse-unmount`. Those three target executables are sufficient for this test;
the headers, library and pkg-config metadata stay on the build host.

For host-only checks without an SDK, use `python3 scripts/doctor.py --mode host`
and `make check`. For a Linux lab host with the SDK, Docker and QEMU installed together, use
`--mode lab --arch aarch64le` (or `x86_64`). In a split-container lab, run the
SDK profile in the build container and check the emulator on its own host.

## Deploy your own binaries

Use your existing deployment process. If `qnx-target` is an SSH alias for root
on your target, with your own key and verified host key:

```sh
ssh qnx-target 'mkdir -p /data/qfuse-repro'
scp "$QFUSE_INSTALL_PREFIX/verify/hello" \
    "$QFUSE_INSTALL_PREFIX/bin/qfuse-mount" \
    "$QFUSE_INSTALL_PREFIX/bin/qfuse-unmount" qnx-target:/data/qfuse-repro/
```

No target, key or SDK is supplied by this repository. Use a new dedicated
`/data/qfuse-repro` directory; keep unrelated data and existing mounts elsewhere.
The target must provide its normal matching QNX runtime libraries.

## Mount and read on QNX

In the target's root shell:

```sh
cd /data/qfuse-repro
chmod 755 hello qfuse-mount qfuse-unmount
mkdir -p mnt
export QFUSE_STATE_DIR=/data/qfuse-repro/run
./qfuse-mount start /data/qfuse-repro/mnt -- /data/qfuse-repro/hello -f /data/qfuse-repro/mnt
./qfuse-mount status /data/qfuse-repro/mnt
cat /data/qfuse-repro/mnt/hello
./qfuse-mount stop /data/qfuse-repro/mnt
```

The read must print `Hello World!`. Record the actual status and stop output.
A successful compile alone does not establish that a mount worked. If normal
stop reports live handles, close them and retry; use `stop --force` only when you
intend to terminate that instance. Keep its private launcher log for local
troubleshooting and share only reviewed, sanitized error excerpts.

## Report what happened

Use the [build-report issue form](https://github.com/gowtham0992/qfuse/issues/new?template=build-report.yml)
or [reproduction issue #1](https://github.com/gowtham0992/qfuse/issues/1).
Include the QFuse revision, Linux distribution, SDP version, CPU, QEMU or board,
and whether build, mount, read and stop each passed. A failed attempt with the
exact failing command is useful. Do not attach SDK/runtime files, license
material, credentials, private paths or environment dumps.

For SSHFS, continue with [build and deployment](build.md) and
[usage](usage.md). The [verification guide](verification.md) describes the larger
suite and separates author-run evidence from independent reproduction.
