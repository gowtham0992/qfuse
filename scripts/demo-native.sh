#!/usr/bin/env bash
# Functional recording on the running private QNX target; no credentials echoed.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$project_root"
printf 'QFuse: native QNX functional demonstration\n\n'
bash scripts/target-exec.sh 'uname -srm'
bash scripts/run-conformance.sh --local
bash scripts/run-conformance.sh --writable-sshfs
printf '\nNative filesystem and authenticated remote-write demonstration completed.\n'
