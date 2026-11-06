# Build on the laptop, run on QNX

The optional local lab exports only `build/live-share` through a read-only,
key-authenticated SFTP container. It does not enable the Mac's SSH service or
publish host ports. Server keys are mounted through a private volume subpath;
the share container cannot access the SDK or client private key.

Requirements: the licensed SDK container, a booted QNX target with SFTP lab
forwarding, deployed QFuse tools/libraries, and Docker with volume-subpath support.
Set `QFUSE_DOCKER_CONTEXT` if your SDK container uses a named context.

```sh
export QFUSE_DOCKER_CONTEXT=colima-qfuse  # omit for your default Docker context
python3 scripts/live-workflow.py up
python3 scripts/live-workflow.py build --arch x86_64
python3 scripts/live-workflow.py mount --arch x86_64
python3 scripts/live-workflow.py run --arch x86_64
```

Edit `build/live-share/src/hello.c` on the laptop. Rebuild and run again through
the same mount. `message.txt` demonstrates data edits visible without rebuilding.
The build publishes immutable executable versions and atomically replaces a
relative symlink. This avoids exposing a partially linked executable or reusing
an old executable identity. No executable is copied to the QNX deployment path.

Select `--arch aarch64le` for the ARM target. Both architectures have actually
executed an ELF through SSHFS in this lab.

Fault demonstration:

```sh
python3 scripts/live-workflow.py cut
# On QNX, a reader with a configured timeout returns to the prompt:
timeout 2 cat /data/qfuse-laptop/src/hello.c
qfuse-mount status /data/qfuse-laptop
qfuse-mount stop --force /data/qfuse-laptop
# On the laptop:
python3 scripts/live-workflow.py restore
python3 scripts/live-workflow.py mount --arch x86_64
python3 scripts/live-workflow.py run --arch x86_64
```

`cut` pauses the scoped SFTP container, leaving the local filesystem process and
its independent administration channel running. The client wait can end while
the foreign callback remains pending. Force stops that instance; remount after
restoring the remote service is explicit. Transparent reconnect is not claimed.

Stop the managed mounts before `python3 scripts/live-workflow.py down`. Generated
keys, images and binaries remain private and outside source distributions.
