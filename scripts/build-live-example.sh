#!/usr/bin/env bash
# Build on the SDK host. This never deploys or copies an executable to QNX.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_root/scripts/target-config.sh"
source "$QNX_SDP_ROOT/qnxsdp-env.sh" >/dev/null
[[ $# == 1 && -f $1 ]] || { echo 'Usage: build-live-example.sh SOURCE' >&2; exit 2; }
mkdir -p "$QFUSE_BUILD_PARENT"
live_root=$(mktemp -d "$QFUSE_BUILD_PARENT/live.XXXXXX")
cp -- "$1" "$live_root/hello.c"
export QFUSE_COMPILER_FAILURE_FILE="$live_root/compiler-failure"
python3 "$project_root/scripts/qnx-compiler.py" "$QNX_HOST/usr/bin/qcc" \
    -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -O1 -g \
    "$live_root/hello.c" -o "$live_root/hello"
python3 - "$live_root/hello" "$QFUSE_ARCH" <<'PY'
import hashlib,json,sys
from pathlib import Path
p=Path(sys.argv[1]);print('QFUSE_LIVE_ARTIFACT='+json.dumps({'path':str(p),'arch':sys.argv[2],'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}))
PY
