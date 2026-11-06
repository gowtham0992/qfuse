# Mount, inspect and stop

Run these tools on QNX as root, with their dependency libraries available.
Configure OpenSSH key authentication and a pinned server key before mounting.
Choose a dedicated data directory outside the management state directory.

```sh
qfuse-mount sshfs -F /root/.ssh/laptop_config developer@laptop:/build-output /data/laptop
qfuse-mount status /data/laptop
/data/laptop/hello
qfuse-mount stop /data/laptop
```

The convenience command defaults to read-only access. Add `--rw` only when you
intend remote writes and the SFTP server permits them. It uses the verified
SSHFS directory-cache/readahead settings.

For another compatible foreground filesystem:

```sh
qfuse-mount start /data/example -- /path/to/filesystem -f /data/example
```

The launcher starts a new process group, captures a private log and waits for
native readiness. It stores kernel process/channel identity and an instance
nonce in `/data/qfuse/run`, mode 0700. `QFUSE_STATE_DIR` can select another trusted
native directory outside the mount. Use the same canonical absolute mount path
for later commands. Status and stop do not traverse the mounted folder.

Stop refuses live handles unless explicitly forced:

```sh
qfuse-mount stop --force /data/laptop
```

Force first requests shutdown and then can kill the identified process group if
callbacks do not drain. It waits for process disappearance before reporting a
forced stop. Forced termination can leave backing cleanup or writes incomplete.
A changed instance is rejected rather than signalled using an unchecked PID.

The lower-level `qfuse-unmount` utility remains available for native detach with
mount identity checking. `-f` requests explicit forced detach. The library itself
uses foreground execution; the launcher supplies background lifecycle handling.

A QNX application can arm `TimerTimeout()` for SEND and REPLY waits. The native
probes exercise this API directly. Ordinary signal-based timeout utilities also
work for the tested stalled-request scenario. Client cancellation frees the wait;
the foreign operation and its buffers may remain live until it returns or the
filesystem process is killed.
