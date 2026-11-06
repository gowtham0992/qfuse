#!/usr/bin/env bash
# Private target deployment; no SDK runtime or keys enter the repository.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_root/scripts/target-config.sh"
cd "$project_root"
build=${QFUSE_ARTIFACT_ROOT:-/home/qnx/qfuse-builds/latest/build/qnx}
python3 scripts/stage-sshfs-runtime.py
python3 - <<'PY'
from pathlib import Path
import shutil
stage=Path('/home/qnx/qfuse-completion-stage');stage.mkdir(mode=0o700,exist_ok=True);stage.chmod(0o700)
for name,root in [('remote',Path('/home/qnx/sshfs-test')),('write',Path('/home/qnx/sshfs-write-test'))]:
 for source,target in [('client_ed25519',name+'_ed25519'),('known_hosts',name+'_known_hosts'),('qnx_ssh_config',name+'_ssh_config'),('bad_known_hosts',name+'_bad_known_hosts'),('bad_ssh_config',name+'_bad_ssh_config')]:
  if (root/source).is_file():
   shutil.copyfile(root/source,stage/target);(stage/target).chmod(0o600 if source=='client_ed25519' else 0o644)
reference=Path('/srv/qfuse-sftp/data/nested/binary.dat')
if reference.is_file():shutil.copyfile(reference,stage/'reference-binary.dat')
PY
bash scripts/target-exec.sh 'mkdir -p /data/qfuse/lib && chmod 700 /data/qfuse /data/qfuse/lib'
bash scripts/target-copy.sh "$build"/qfuse-{mount,hello,context,passthrough,sshfs,unmount,priority,inflight} "$build"/test-{dependencies,admin,stall-load,stall,passthrough-plus,dirent-adapter,operation-report,inflight-death,abi,mount-identity,core,mutation-core,mount,passthrough,conformance,lifecycle,write-permissions,concurrency,priority,sshfs,sshfs-write} "$build/exec-probe" /data/qfuse/
bash scripts/target-copy.sh $QFUSE_TARGET_DIR/runtime/* /data/qfuse/lib
bash scripts/target-copy.sh /home/qnx/qfuse-completion-stage/* /data/qfuse/
bash scripts/target-exec.sh 'for fixture_key in /data/qfuse/remote_ed25519 /data/qfuse/write_ed25519; do if test -f "$fixture_key"; then chmod 600 "$fixture_key"; fi; done'
