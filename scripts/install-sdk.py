#!/usr/bin/env python3
"""Run locally inside qfuse-dev; credentials never need to enter chat."""
import argparse
import getpass
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--check', action='store_true', help='check prerequisites without logging in or installing')
args = parser.parse_args()
home = Path.home()
qsc = home / 'qnx/qnxsoftwarecenter/qnxsoftwarecenter_clt'
sdk = home / 'qnx800'
if not qsc.is_file():
    raise SystemExit('QNX Software Center must be installed in ~/qnx first.')
if args.check:
    print('Software Center launcher found. No account login or SDK installation attempted.')
    raise SystemExit(0)
if not os.isatty(0):
    raise SystemExit('Run interactively in your own Terminal; do not send credentials through chat.')
if sdk.exists():
    raise SystemExit('~/qnx800 already exists. Inspect it before attempting another installation.')
print('This will activate your deployed QNX licenses and install QNX SDP 8.0')
print('in this container’s private home volume, using your own myQNX account.')
print('Review your applicable non-commercial license: https://www.qnx.com/nc_qdl')
if input('Type INSTALL to consent and continue: ') != 'INSTALL':
    raise SystemExit('Cancelled; no account request made.')
username = input('myQNX username: ').strip()
password = getpass.getpass('myQNX password (hidden): ')
if not username or not password or any(c in username + password for c in '\r\n\x00'):
    raise SystemExit('Credentials must be nonempty, single-line values.')
# QNX explicitly recommends @options_file to avoid cleartext password logging.
# Keep the temporary file out of the project; mkstemp creates mode 0600.
fd, filename = tempfile.mkstemp(prefix='qfuse-qsc-', suffix='.options', dir='/tmp')
try:
    with os.fdopen(fd, 'w') as stream:
        stream.write('-myqnx.user\n' + username + '\n-myqnx.password\n' + password + '\n')
    del password
    commands = [
        ('Synchronizing and activating deployed licenses', ['-syncLicenseKeys', '-activateAll']),
        ('Downloading QNX 8.0 package metadata', ['-mirrorBaseline', 'qnx800']),
        ('Installing QNX SDP 8.0', ['-destination', str(sdk), '-installBaseline', 'com.qnx.qnx800']),
    ]
    for label, options in commands:
        print(label, flush=True)
        subprocess.run([str(qsc), '@' + filename] + options, check=True)
finally:
    Path(filename).unlink(missing_ok=True)
print('Installation command completed. Compiler and QNX target still need verification.')
