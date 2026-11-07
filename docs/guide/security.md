# Dedicated service deployment on QNX 8.0

An optional enforcing-policy example runs SSHFS and SSH as UID/GID 65534 instead
of root, while root retains authenticated mount management. It was executed on
an aarch64le QNX 8.0 QEMU guest. This is a target-specific integration example,
not a complete production security policy or a claim of arbitrary-code isolation.
The ordinary deployment continues to use root management and root-owned servers.

The [policy fragment](../../integration/security/qfuse.rules) allows the service
to attach only `/data/qfuse-secure`. It grants the storage, entropy, pipe and
network peer connections needed by this lab. The service has no physical-memory,
I/O, arbitrary setuid, or file-permission-bypass grant. Its default exec child
type, `qfuse_ssh_t`, has no mount attachment authority. A compromised mount process
still has its own service authority and can explicitly create its own type;
this child default is not an extra boundary against a compromised parent.

## Integrate with a reviewed system policy

1. Reserve a dedicated non-root UID/GID and locked account with a non-login shell.
   The example uses 65534; choose another if it is already assigned and update
   the policy ranges. OpenSSH needs a valid account-database entry even for an
   outgoing key-authenticated connection. The lab account was named `qfuse`.
2. Merge the fragment with your system policy. Adapt the peer and trusted
   launcher type names to your image. The fragment references `devb_t`,
   `random_t`, `pipe_t`, `io_sock_t`, `user_root_t`, `sshd_t` and the declared
   `network/jailroot` dynamic ability. Its trusted-administrator transitions
   are specific to this SSH-admin lab; they are not grants for ordinary users.
3. Compile and boot an enforcing policy image. Discovery modes such as
   `mkqnximage --secpol=open` and `develop` do not prove restriction. The lab
   compiled a discovery-generated **private** system policy plus the fragment
   and booted with `--secpol=secure --policy=/private/service-policy.txt`.
   Review the whole surrounding policy before deployment. A generated lab
   system policy is not a hardened production system.
4. Keep executables, libraries, host-key pins and SSH configuration root-owned
   and unwritable by the service. Give only the service ownership of its private
   home/key (0700 directory, 0600 key). Grant no directory-search/read bypass
   ability. Use key authentication, strict host-key checking and a scoped SFTP
   account/export. This example does not filter network destinations.
5. Build the optional launcher using your activated SDK, for example:

```sh
qcc -Vgcc_ntoaarch64le -std=c11 -D_QNX_SOURCE -Wall -Wextra -Werror \
  integration/security/service-exec.c -lsecpol -o /private/stage/qfuse-service-exec
```

Deploy it root-owned, mode 0755, **without setuid**. The image must provide its
own `libsecpol`. It requires root, clears supplementary groups, derives the
service type, drops real/effective UID/GID and uses QNX explicit-type
`POSIX_SPAWN_EXEC` to execute in place. This preserves the PID/group and admin
advertisement that `qfuse-mount` authenticates. Wrapping the command with `on`
would introduce another process and break that identity contract. Missing policy,
derivation or identity permission is a launch failure; there is no root fallback.

With paths/configuration adapted to your target:

```sh
qfuse-mount start /data/qfuse-secure -- \
  /usr/local/bin/qfuse-service-exec 65534 -- \
  /usr/local/bin/qfuse-sshfs laptop:/build /data/qfuse-secure -f \
  -F /etc/qfuse/laptop_config -o ro,dir_cache=no,no_readahead,sync_readdir
qfuse-mount status /data/qfuse-secure
qfuse-mount stop /data/qfuse-secure
```

Kernel PATHSPACE and policy rules authorize service mounts; QFuse supplies no
privilege elevation to ordinary users. The management command remains root-only.
Policy requirements should be revalidated whenever the image or dependency
versions change. This optional launcher is built separately; the default
library/package does not gain a `libsecpol` dependency.

## Observed enforcement

The [profile transcript](../proof/service-profile.txt) records a managed SSHFS
mount, comparison of all 200,037 synthetic bytes, actual `qfuse_t`/`qfuse_ssh_t`
process types, stop and remount. The same UID passed permission probes denying:

- Becoming root or reading a root-owned 0600 fixture.
- Physical-memory mapping and I/O privilege, even after attempted ability changes.
- An unauthorized process-manager link and an additional mount path.
- Mounting as the lower SSH child type, and management as non-root.

The probe checks actual privileged operations. QNX may silently ignore changes
to locked abilities; a successful ability-change return alone is not evidence
that the privilege was granted. Its physical-address probe is specific to this
ARM QEMU lab and must be adapted before use on another target.

Existing native tests verify admin-channel caller identity and unauthorized
messages. This profile adds a non-root server deployment, not a new management
protocol. The full 51-case suites ran in the ordinary profile on both supported
architectures; they were not all rerun under this optional policy. No physical
board, safety, network-destination confinement or complete sandbox claim follows
from these checks.

References: QNX [security policy language](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/secpol_language.html),
[security policies](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/security_policies.html),
and [process abilities](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/process_manager_abilities.html).
