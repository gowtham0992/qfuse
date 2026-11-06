#!/usr/bin/env bash
# Copy local build artifacts into the private target, from inside qfuse-dev.
# Connection setup allowance for the emulated guest; remote test deadlines are separate.
set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/target-config.sh"
target_root=${QFUSE_TARGET_DIR:-/home/qnx/targets/qfuse-x86_64}
if [[ $# -lt 2 ]]; then echo 'Usage: target-copy.sh LOCAL_FILE... /absolute/target/destination' >&2; exit 2; fi
destination=${!#}
[[ $destination =~ ^/data/qfuse/([A-Za-z0-9_.-]+)?$ ]] || exit 2
[[ $destination != /data/qfuse/. && $destination != /data/qfuse/.. ]] || exit 2
sources=("${@:1:$#-1}")
for i in "${!sources[@]}"; do sources[$i]=$(realpath -- "${sources[$i]}"); done
exec scp -O -F /dev/null -i "$QFUSE_TARGET_IDENTITY" \
    -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile="$QFUSE_TARGET_KNOWN_HOSTS" -o ConnectTimeout=60 \
    -P "$QFUSE_TARGET_PORT" "${sources[@]}" "root@$QFUSE_TARGET_HOST:$destination"
