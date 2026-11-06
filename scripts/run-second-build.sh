#!/usr/bin/env bash
# The separately named binaries must come from verify-clean-build.sh's output.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$project_root"
bash scripts/target-exec.sh 'QFUSE_SERVER_BINARY=/data/qfuse/second-hello QFUSE_CONTEXT_BINARY=/data/qfuse/qfuse-context /data/qfuse/test-mount'
bash scripts/target-exec.sh 'QFUSE_PASSTHROUGH_BINARY=/data/qfuse/second-passthrough /data/qfuse/test-conformance'
bash scripts/target-exec.sh 'QFUSE_PASSTHROUGH_BINARY=/data/qfuse/second-passthrough /data/qfuse/test-lifecycle'
bash scripts/target-exec.sh 'QFUSE_SSHFS_BINARY=/data/qfuse/second-sshfs /data/qfuse/test-sshfs-write'
