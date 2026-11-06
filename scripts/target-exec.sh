#!/usr/bin/env bash
# Run inside qfuse-dev. Uses the identity private to this disposable QNX target.
# Connection setup allowance for the emulated guest; remote test deadlines are separate.
set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/target-config.sh"
target_root=${QFUSE_TARGET_DIR:-/home/qnx/targets/qfuse-x86_64}
exec ssh -F /dev/null -i "$QFUSE_TARGET_IDENTITY" \
    -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile="$QFUSE_TARGET_KNOWN_HOSTS" -o ConnectTimeout=60 \
    -p "$QFUSE_TARGET_PORT" "root@$QFUSE_TARGET_HOST" "$@"
