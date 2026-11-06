#!/usr/bin/env bash
# Run as qnx inside qfuse-dev, after booting the private test guest.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$project_root"
build=/home/qnx/qfuse-builds/latest/build/qnx
fixture=/home/qnx/sshfs-test
python3 scripts/stage-sshfs-runtime.py
bash scripts/target-exec.sh 'mkdir -p /data/qfuse/lib && chmod 700 /data/qfuse /data/qfuse/lib'
bash scripts/target-copy.sh "$build/qfuse-sshfs" "$build/test-sshfs" /data/qfuse/
bash scripts/target-copy.sh "$fixture"/runtime/* /data/qfuse/lib
bash scripts/target-copy.sh "$fixture/client_ed25519" /data/qfuse/remote_ed25519
bash scripts/target-exec.sh 'chmod 600 /data/qfuse/remote_ed25519'
bash scripts/target-copy.sh "$fixture/known_hosts" /data/qfuse/remote_known_hosts
bash scripts/target-copy.sh "$fixture/qnx_ssh_config" /data/qfuse/remote_ssh_config
bash scripts/target-copy.sh "$fixture/bad_known_hosts" /data/qfuse/remote_bad_known_hosts
bash scripts/target-copy.sh "$fixture/bad_ssh_config" /data/qfuse/remote_bad_ssh_config
# Independent native reference, transferred outside the SSHFS mount.
bash scripts/target-copy.sh /srv/qfuse-sftp/data/nested/binary.dat /data/qfuse/reference-binary.dat
