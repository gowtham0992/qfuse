#!/usr/bin/env bash
# Run in qfuse-verify:ubuntu24 with the owner's SDK/dependency volume mounted.
set -euo pipefail
export QEMU_CPU=${QFUSE_BUILD_CPU:-Nehalem}
checks=/home/qnx/qfuse-clean-checks
mkdir -p "$checks"
run=$(mktemp -d "$checks/run.XXXXXX")
mode=${1:---clone}
case "$mode" in
 --clone)
  git -c safe.directory=/workspace clone --no-local /workspace "$run/qfuse"
  git -C "$run/qfuse" status --porcelain | python3 -c 'import sys; assert not sys.stdin.read()'
  git -C "$run/qfuse" rev-parse HEAD
  python3 - "$run/qfuse" <<'PYTHON'
from pathlib import Path
import sys
root=Path(sys.argv[1])
assert not (root/'.cache').exists() and not (root/'build').exists()
print('PASS pristine Git checkout: no project download cache or generated build directory')
PYTHON
  ;;
 --archive) tar -xzf /workspace/build/qfuse-source.tar.gz -C "$run";;
 *) echo 'Usage: verify-clean-build.sh [--clone|--archive]' >&2; exit 2;;
esac
cd "$run/qfuse"
python3 scripts/fetch-upstream.py
python3 scripts/fetch-sshfs.py
export QFUSE_BUILD_PARENT=$run/artifacts
bash scripts/build-linux.sh --sshfs
export QFUSE_ARTIFACT_ROOT=$QFUSE_BUILD_PARENT/latest/build/qnx
export QFUSE_INSTALL_PREFIX=$run/install
bash scripts/package-qnx.sh
source /home/qnx/qnx800/qnxsdp-env.sh >/dev/null
project_root=$run/qfuse
source scripts/qnx-compiler-env.sh
python3 scripts/verify-package.py
if [[ $mode == --clone ]]; then
 git status --porcelain | python3 -c 'import sys; assert not sys.stdin.read()'
fi
ln -sfn "$run" "$checks/latest"
python3 - <<'PY'
from pathlib import Path
print(next(line for line in Path('/etc/os-release').read_text().splitlines() if line.startswith('PRETTY_NAME=')))
print('PASS fresh source rebuild and installed pkg-config examples in the reported Linux userland.')
print('Licensed SDK and prebuilt target GLib are shared inputs; project sources, objects and install prefix are fresh.')
PY
