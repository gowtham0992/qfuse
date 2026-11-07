#!/usr/bin/env python3
"""Check selected build prerequisites without reading credentials or SDK contents."""
import argparse
import os
import platform
import shutil
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mode', choices=('host', 'sdk', 'lab'), default='sdk',
                    help='host checks only, native Linux SDK build (default), or SDK plus local QEMU lab')
parser.add_argument('--arch', choices=('x86_64', 'aarch64le'),
                    default=os.environ.get('QFUSE_ARCH', 'x86_64'))
sdk_default = os.environ.get('QNX_SDP_ROOT')
if not sdk_default and os.environ.get('QNX_HOST'):
    sdk_default = os.environ['QNX_HOST'].split('/host/')[0]
parser.add_argument('--sdk', type=Path, default=Path(sdk_default or Path.home()/'qnx800'))
args = parser.parse_args()
missing = []
print(f'Host: {platform.system()} {platform.machine()}')
print(f'Prerequisite profile: {args.mode}; target: {args.arch}')
commands = ['cc', 'python3', 'curl', 'make']
if args.mode != 'host':
    commands += ['bash', 'patch', 'pkg-config', 'tar', 'xz']
if args.mode == 'lab':
    commands += ['docker', 'qemu-system-aarch64' if args.arch == 'aarch64le' else 'qemu-system-x86_64']
for command in commands:
    found = shutil.which(command) is not None
    print(f'{command}: {"available" if found else "missing"}')
    if not found:
        missing.append(command)
if args.mode == 'lab' and shutil.which('docker'):
    try:
        result = subprocess.run(['docker', 'info', '--format', '{{.OSType}}'],
                                capture_output=True, text=True, timeout=10)
        running = result.returncode == 0 and result.stdout.strip() == 'linux'
    except (OSError, subprocess.TimeoutExpired):
        running = False
    print(f'Linux Docker engine: {"available" if running else "unavailable"}')
    if not running:
        missing.append('Linux Docker engine')
if args.mode != 'host':
    if platform.system() != 'Linux' or platform.machine().lower() not in ('x86_64', 'amd64'):
        missing.append('Linux x86_64 SDK host (a Linux container can supply this on a Mac)')
    sdk_present = (args.sdk/'host/linux/x86_64/usr/bin/qcc').is_file()
    print(f'QNX Linux host compiler in selected SDK: {"present" if sdk_present else "missing"}')
    print('License activation and QNX target access: manual verification required')
    if not sdk_present:
        missing.append('QNX Linux host compiler')
if missing:
    print('Prerequisites missing: ' + ', '.join(missing))
    raise SystemExit(1)
print('Selected prerequisites found; run the actual build and target mount to verify them.')
