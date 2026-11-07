#!/usr/bin/env bash
# Run inside qfuse-dev; tests use /data/qfuse binaries, never stale IFS copies.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_root/scripts/target-config.sh"
cd "$project_root"
mode=${1:---local}
case "$mode" in
 --local|--full)
  bash scripts/target-exec.sh '/data/qfuse/test-mount-identity'
  bash scripts/target-exec.sh '/data/qfuse/test-abi && /data/qfuse/test-core && /data/qfuse/test-mutation-core && /data/qfuse/test-path-gate'
  bash scripts/target-exec.sh 'QFUSE_SERVER_BINARY=/data/qfuse/qfuse-hello QFUSE_CONTEXT_BINARY=/data/qfuse/qfuse-context /data/qfuse/test-mount'
  bash scripts/target-exec.sh 'QFUSE_CONTEXT_BINARY=/data/qfuse/qfuse-context QFUSE_CONTEXT_LIFECYCLE=1 /data/qfuse/test-mount --security-only'
  bash scripts/target-exec.sh '/data/qfuse/test-stall && /data/qfuse/test-stall --timer && /data/qfuse/test-stall --exit && /data/qfuse/test-stall --close-stall'
  bash scripts/target-exec.sh '/data/qfuse/test-admin'
  bash scripts/target-exec.sh '/data/qfuse/test-conformance'
  bash scripts/target-exec.sh '/data/qfuse/test-dirent-adapter && /data/qfuse/test-passthrough-plus && /data/qfuse/test-operation-report'
  bash scripts/target-exec.sh '/data/qfuse/test-write-permissions'
  bash scripts/target-exec.sh '/data/qfuse/test-concurrency'
  bash scripts/target-exec.sh '/data/qfuse/test-priority'
  bash scripts/target-exec.sh '/data/qfuse/test-lifecycle'
  bash scripts/target-exec.sh '/data/qfuse/test-inflight-death'
  if [[ $mode == --full ]]; then bash scripts/target-exec.sh 'QFUSE_PASSTHROUGH_BINARY=/data/qfuse/qfuse-passthrough /data/qfuse/test-passthrough'; fi
  ;;
 --stall) bash scripts/target-exec.sh '/data/qfuse/test-stall && /data/qfuse/test-stall --timer && /data/qfuse/test-stall --exit && /data/qfuse/test-stall --close-stall';;
 --namespace) bash scripts/target-exec.sh '/data/qfuse/test-namespace read && /data/qfuse/test-namespace mkdir && /data/qfuse/test-namespace rename && /data/qfuse/test-namespace fair && /data/qfuse/test-namespace grace && /data/qfuse/test-namespace serial-close && /data/qfuse/test-namespace flush-error';;
 --saturation) bash scripts/target-exec.sh '/data/qfuse/test-stall-load && /data/qfuse/test-stall-load --open';;
 --report) bash scripts/target-exec.sh '/data/qfuse/test-operation-report';;
 --readonly-sshfs)
  bash scripts/target-exec.sh '/data/qfuse/test-sshfs && QFUSE_TEST_HIGH_PRIORITY=1 /data/qfuse/test-sshfs'
  bash scripts/target-exec.sh 'QFUSE_SSHFS_CONFIG=/data/qfuse/remote_bad_ssh_config /data/qfuse/test-sshfs --expect-connect-failure'
  ;;
 --writable-sshfs) bash scripts/target-exec.sh '/data/qfuse/test-sshfs-write';;
 *) echo 'Usage: run-conformance.sh --local|--full|--stall|--saturation|--namespace|--report|--readonly-sshfs|--writable-sshfs' >&2;exit 2;;
esac
