#!/usr/bin/env python3
"""Supervised SDK tools: never cache an emulator SIGSEGV as a feature result.

Only detected compiler crashes are retried. Ordinary compile errors are returned
unchanged. An exhausted crash creates a marker that the build supervisor must
check before using any generated configuration.
"""
import os
from pathlib import Path
import subprocess
import sys
import re

marker = os.environ.get('QFUSE_COMPILER_FAILURE_FILE')
if not marker or not Path(marker).is_absolute() or len(sys.argv) < 2:
    raise SystemExit('qnx-compiler.py requires a supervised build failure-marker path and compiler command')
for attempt in range(5):
    try:
        result = subprocess.run(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as error:
        Path(marker).write_text('Compiler could not start; discard this configuration.\n')
        print(f'QFuse build infrastructure: cannot start compiler: {error}', file=sys.stderr)
        raise SystemExit(99)
    diagnostic = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', result.stderr).lower()
    crashed = result.returncode in (-11, 139) or any(text in diagnostic for text in (
        b'caught signal 11', b'terminated with signal 11', b'segmentation fault (core dumped)',
        b'uncaught target signal 11',
        b'internal compiler error: segmentation fault',
    ))
    if not crashed and (result.returncode < 0 or b'internal compiler error:' in diagnostic):
        Path(marker).write_text('Compiler infrastructure failed; discard this configuration.\n')
        sys.stdout.buffer.write(result.stdout)
        sys.stderr.buffer.write(result.stderr)
        raise SystemExit(99)
    if not crashed:
        sys.stdout.buffer.write(result.stdout)
        sys.stderr.buffer.write(result.stderr)
        raise SystemExit(result.returncode if result.returncode >= 0 else 128 - result.returncode)
    print(f'QFuse build infrastructure: SDK tool SIGSEGV attempt {attempt + 1}/5', file=sys.stderr)
Path(marker).write_text('Compiler emulation failed after five attempts; discard this configuration.\n')
sys.stderr.buffer.write(result.stderr)
raise SystemExit(99)
