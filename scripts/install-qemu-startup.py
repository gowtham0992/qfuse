#!/usr/bin/env python3
"""Install the official ARM QEMU startup component using the owner's QSC account.

Run interactively inside the licensed SDK container. No credentials are saved
in the checkout, environment, command arguments or output.
"""
import getpass
import os
from pathlib import Path
import signal
import subprocess
import tempfile

home = Path.home()
sdk = Path(os.environ.get('QNX_SDP_ROOT', str(home / 'qnx800')))
qsc = Path(os.environ.get('QNX_SOFTWARE_CENTER', str(home / 'qnx/qnxsoftwarecenter/qnxsoftwarecenter_clt')))
package = ('com.qnx.qnx800.target.qemuvirt/0.2.1.00087T202507241124L,'
           'com.qnx.qnx800.target.driver.virtio.devb/0.1.0.00011T202411230104L')
if not qsc.is_file() or not (sdk / 'qnxsdp-env.sh').is_file():
    raise SystemExit('Use a Linux environment with your licensed SDP 8.0 and QNX Software Center.')
print('Installing the official QNX SDP 8.0 QEMU startup and VirtIO block-driver components.')
print('Use your own myQNX account. Enter the password here, never in chat.')
username = input('myQNX username: ').strip()
password = getpass.getpass('myQNX password (hidden): ')
if not username or not password or any(c in username + password for c in '\r\n\x00'):
    raise SystemExit('Credentials must be nonempty single-line values.')

def interrupted(number, _frame):
    raise SystemExit(128 + number)

for number in (signal.SIGTERM, signal.SIGHUP):
    signal.signal(number, interrupted)
fd, name = tempfile.mkstemp(prefix='qfuse-qsc-', suffix='.options', dir='/tmp')
try:
    with os.fdopen(fd, 'w') as stream:
        stream.write('-myqnx.user\n' + username + '\n-myqnx.password\n' + password + '\n')
    result = subprocess.run([str(qsc), '@' + name, '-offline', '-destination', str(sdk),
                             '-installPackage', package], env=dict(os.environ, QEMU_CPU='Nehalem'),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        # Never echo SDK diagnostics containing account identifiers or secrets.
        print('QNX Software Center could not complete the installation (exit', result.returncode, ').')
        print('Check account entitlement and this component in QNX Software Center.')
        raise SystemExit(result.returncode)
finally:
    Path(name).unlink(missing_ok=True)
    del password
print('Installed ARM QEMU startup and block-driver components. Image build and target execution still need verification.')
