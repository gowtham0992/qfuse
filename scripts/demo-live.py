#!/usr/bin/env python3
"""Record a real prepared live lab. No synthetic PASS output or credentials."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ARCH = os.environ.get('QFUSE_ARCH', 'aarch64le')
if ARCH not in ('aarch64le', 'x86_64'): raise SystemExit('Unsupported architecture')
DOCKER = ['docker'] + (['--context', os.environ['QFUSE_DOCKER_CONTEXT']] if os.environ.get('QFUSE_DOCKER_CONTEXT') else [])
SDK = os.environ.get('QFUSE_SDK_CONTAINER', 'qfuse-dev')

def title(s): print('\n=== ' + s + ' ===', flush=True)
def live(action):
    subprocess.run([sys.executable, str(ROOT/'scripts/live-workflow.py'), action, '--arch', ARCH], check=True)
def target(command):
    print('QNX $ ' + command, flush=True)
    subprocess.run(DOCKER+['exec', '-e', 'QEMU_CPU=Nehalem', '-e', 'QFUSE_ARCH='+ARCH,
        SDK, 'bash', '/workspace/scripts/target-exec.sh', command], check=True)

source = ROOT/'build/live-share/src/hello.c'
original = source.read_bytes()
paused = False
try:
    title('Build on the laptop; run on QNX')
    source.write_bytes((ROOT/'examples/live/hello.c').read_bytes())
    live('build'); live('mount'); live('run'); live('status')
    title('Edit the laptop source; rebuild; run through the same mount')
    source.write_text(source.read_text().replace('First build from the laptop', 'Edited on the laptop; no deployment copy to QNX'))
    live('build'); live('run'); live('status')
    title('Pause the remote SFTP server')
    live('cut'); paused = True
    target('timeout 2 cat /data/qfuse-laptop/src/hello.c; reader_result=$?; echo reader_exit=$reader_result; test "$reader_result" = 124')
    live('status'); live('stop')
    title('Restore the server; remount; run again')
    live('restore'); paused = False
    live('mount'); live('run'); live('status'); live('stop')
    title('Native QNX callback isolation and client interruption')
    target('/data/qfuse/test-stall --timer')
finally:
    source.write_bytes(original)
    if paused: live('restore')
