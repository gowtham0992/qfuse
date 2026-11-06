#!/usr/bin/env bash
# Snapshot project code and public upstream fixtures onto the Linux filesystem.
# Avoid compiling from the macOS virtiofs bind mount under x86 emulation.
set -eo pipefail
export QEMU_CPU=${QFUSE_BUILD_CPU:-Nehalem}
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_root/scripts/target-config.sh"
build_parent=${QFUSE_BUILD_PARENT:-/home/qnx/qfuse-builds}
build_parent=$(realpath -m -- "$build_parent")
sdk_guard=$(realpath -m -- "$QNX_SDP_ROOT")
[[ $build_parent == /* && $build_parent != / && $build_parent != "$sdk_guard" && $build_parent != "$sdk_guard/"* ]] || exit 2
mode=${1:---mount}
[[ $mode == --mount || $mode == --callbacks || $mode == --sshfs ]] || exit 2
mkdir -p "$build_parent"
build_root=$(mktemp -d "$build_parent/build.XXXXXX")
mkdir -p "$build_root/.cache"
cp -R "$project_root/include" "$project_root/src" "$project_root/tests" \
    "$project_root/scripts" "$project_root/patches" "$project_root/upstream.lock.json" "$build_root/"
cp -R "$project_root/.cache/libfuse" "$build_root/.cache/"
if [[ $mode == --sshfs ]]; then
    cp "$project_root/sshfs-dependencies.lock.json" "$build_root/"
    cp -R "$project_root/.cache/sshfs" "$build_root/.cache/"
fi
cd "$build_root"
if [[ $mode == --sshfs ]]; then
    bash scripts/build-sshfs.sh
elif [[ $mode == --mount ]]; then
    bash scripts/build-qnx-mount.sh
else
    source "${QNX_SDP_ROOT:-/home/qnx/qnx800}/qnxsdp-env.sh" >/dev/null
    python3 scripts/test-host.py --qnx
fi
if [[ -e "$build_parent/latest" && ! -L "$build_parent/latest" ]]; then
    echo 'Refusing to replace a non-symlink latest path.' >&2; exit 1
fi
ln -sfn "$build_root" "$build_parent/latest"
python3 - "$build_root" <<'PYTHON'
import hashlib,json,sys
from pathlib import Path
root=Path(sys.argv[1]);manifest={}
for directory in ('src','include','tests','scripts','patches'):
 for p in (root/directory).rglob('*'):
  if p.is_file():manifest[str(p.relative_to(root))]=hashlib.sha256(p.read_bytes()).hexdigest()
(root/'build/qnx/build-source.json').write_text(json.dumps(manifest,sort_keys=True,indent=2)+'\n')
PYTHON
printf 'Built artifacts: %s/latest/build/qnx\n' "$build_parent"
