#!/usr/bin/env python3
"""Stage the target's dependency closure privately; never write SDK libs to repo."""
from pathlib import Path
import re
import os
import shutil
import subprocess
import sys

build = Path(os.environ.get('QFUSE_ARTIFACT_ROOT','/home/qnx/qfuse-builds/latest/build/qnx'))
arch=os.environ.get('QFUSE_ARCH','x86_64')
if arch not in ('x86_64','aarch64le'):raise SystemExit('Unsupported target architecture')
private = Path(os.environ.get('QFUSE_DEPS_ROOT','/home/qnx/qfuse-deps'))/'sysroot/usr/lib'
sdk_root=Path(os.environ.get('QNX_SDP_ROOT','/home/qnx/qnx800'))
sdk = sdk_root/'target/qnx'/arch
bin_prefix='ntox86_64' if arch=='x86_64' else 'ntoaarch64'
readelf = str(sdk_root/'host/linux/x86_64/usr/bin'/(bin_prefix+'-readelf'))
destination = Path(os.environ.get('QFUSE_RUNTIME_DIR',str(Path(os.environ.get('QFUSE_TARGET_DIR','/home/qnx/targets/qfuse-x86_64'))/'runtime')))
destination.mkdir(parents=True, exist_ok=True)
queue = [build/'qfuse-sshfs']
if (build/'test-dependencies').is_file():queue.append(build/'test-dependencies')
seen = set()
marker = build/'runtime-tool-failure'
if marker.exists():raise SystemExit('Earlier SDK tool failure invalidated runtime staging.')
tool_env = dict(os.environ, QFUSE_COMPILER_FAILURE_FILE=str(marker))
while queue:
    obj = queue.pop()
    result = subprocess.run([sys.executable, str(Path(__file__).with_name('qnx-compiler.py')), readelf, '-d', str(obj)], check=True, capture_output=True, text=True, env=tool_env)
    for name in re.findall(r'\(NEEDED\).*\[([^\]]+)\]', result.stdout):
        if name in seen: continue
        if '/' in name: raise SystemExit('Unexpected dependency name: '+name)
        seen.add(name)
        provider = next((p/name for p in [private, sdk/'usr/lib', sdk/'lib'] if (p/name).is_file()), None)
        if provider is None: raise SystemExit('Unresolved target dependency: '+name)
        shutil.copyfile(provider.resolve(), destination/name)
        (destination/name).chmod(0o644)
        queue.append(provider.resolve())
print('Privately staged target libraries:', ', '.join(sorted(seen)))
