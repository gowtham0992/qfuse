#!/usr/bin/env python3
"""Report prerequisite availability without reading credentials or SDK files."""
import argparse
import os
import platform
import shutil
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk', type=Path, default=Path(os.environ.get('QNX_SDP_ROOT', str(Path.home() / 'qnx800'))))
args = parser.parse_args()
missing = []
print(f'Host: {platform.system()} {platform.machine()}')
for command in ['cc', 'python3', 'curl', 'docker', 'qemu-system-x86_64']:
    found = shutil.which(command) is not None
    print(f'{command}: {"available" if found else "missing"}')
    if not found:
        missing.append(command)
if shutil.which('docker'):
    try:
        result = subprocess.run(['docker', 'info', '--format', '{{.OSType}}'],
                                capture_output=True, text=True, timeout=10)
        running = result.returncode == 0 and result.stdout.strip() == 'linux'
    except subprocess.TimeoutExpired:
        running = False
    print(f'Linux Docker engine: {"available" if running else "unavailable"}')
    if not running:
        missing.append('Linux Docker engine')
sdk_present = (args.sdk / 'host/linux/x86_64/usr/bin/qcc').is_file()
print(f'QNX Linux host compiler in selected SDK: {"present" if sdk_present else "missing"}')
print('License activation and QNX target access: manual verification required')
if not sdk_present:
    missing.append('QNX Linux host compiler')
if missing:
    print('QNX gate blocked: ' + ', '.join(missing))
    raise SystemExit(1)
print('Local prerequisites found; boot and compile on QNX to verify the actual target.')
