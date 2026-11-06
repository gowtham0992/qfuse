#!/usr/bin/env python3
"""Create a loopback-only, key-authenticated, read-only SFTP test fixture.

Run as root only inside qfuse-dev. No personal keys/accounts or SDK data are
exported; the SFTP user is chrooted to synthetic data.
"""
import crypt
import os
from pathlib import Path
import pwd
import secrets
import signal
import subprocess
import sys

if sys.platform != 'linux' or os.geteuid() != 0 or not Path('/.dockerenv').exists():
    raise SystemExit('Run only as root inside the private development container.')
if not Path('/workspace/sshfs-dependencies.lock.json').is_file():
    raise SystemExit('Expected QFuse workspace is not mounted.')
if any(arg not in ('--stop','--writable','--prepare-only') for arg in sys.argv[1:]) or len(set(sys.argv[1:]))!=len(sys.argv[1:]):
    raise SystemExit('Usage: setup-sftp-test.py [--writable] [--stop|--prepare-only]')
stopping = '--stop' in sys.argv[1:]
writable = '--writable' in sys.argv[1:]
preparing = '--prepare-only' in sys.argv[1:]
if stopping and preparing:raise SystemExit('Choose stop or prepare-only, not both.')
fixture_uid = 21002 if writable else 21001
key_name = 'write' if writable else 'remote'
source_name = 'qfuse-writable' if writable else 'qfuse-source'
config_root = Path('/etc/ssh/qfuse-write-test' if writable else '/etc/ssh/qfuse-test')
client_root = Path('/home/qnx/sshfs-write-test' if writable else '/home/qnx/sshfs-test')
export_root = Path('/srv/qfuse-sftp-write' if writable else '/srv/qfuse-sftp')
for path in (config_root, export_root):
    if path.is_symlink():
        raise SystemExit('Refusing a symlink at a privileged fixture path.')
    path.mkdir(parents=True, exist_ok=True)
    os.chown(path, 0, 0); path.chmod(0o755)
pidfile = config_root/'sshd.pid'
if pidfile.exists():
    pid = int(pidfile.read_text())
    cmd = Path(f'/proc/{pid}/cmdline')
    if cmd.exists():
        if str(config_root/'sshd_config').encode() not in cmd.read_bytes():
            raise SystemExit('PID file refers to another process; refusing to alter it.')
        if stopping:
            os.kill(pid, signal.SIGTERM)
            print('Stopped the dedicated QFuse SFTP test server.')
        else:
            print('QFuse SFTP test server is already running.')
        raise SystemExit(0)
if stopping:
    print('QFuse SFTP test server is not running.')
    raise SystemExit(0)
user = 'qfuse-sftp-write' if writable else 'qfuse-sftp'
try:
    account = pwd.getpwnam(user)
    if account.pw_uid != fixture_uid or account.pw_dir != str(export_root):
        raise SystemExit('Existing account does not match this disposable fixture.')
except KeyError:
    subprocess.run(['useradd', '-M', '-u', str(fixture_uid), '-d', str(export_root), '-s', '/bin/sh', user], check=True)
    # An unknown random password keeps the account unlocked for public-key auth.
    # Password authentication is disabled in the dedicated server configuration.
    password_hash = crypt.crypt(secrets.token_urlsafe(32), crypt.mksalt(crypt.METHOD_SHA512))
    subprocess.run(['usermod', '-p', password_hash, user], check=True)
qnx = pwd.getpwnam('qnx')
client_root.mkdir(mode=0o700, exist_ok=True)
os.chown(client_root, qnx.pw_uid, qnx.pw_gid); client_root.chmod(0o700)
for key in (config_root/'host_ed25519', client_root/'client_ed25519'):
    if not key.exists():
        subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(key)], check=True)
    key.chmod(0o600)
    if key.parent == client_root:
        os.chown(key, qnx.pw_uid, qnx.pw_gid)
        os.chown(Path(str(key)+'.pub'), qnx.pw_uid, qnx.pw_gid)
authorized = config_root/'authorized_keys'
authorized.write_text((client_root/'client_ed25519.pub').read_text())
authorized.chmod(0o644)
host_public = (config_root/'host_ed25519.pub').read_text().split()
known = client_root/'known_hosts'
known.write_text('[10.0.2.100]:2223 ' + ' '.join(host_public[:2]) + '\n' +
                 '[127.0.0.1]:2223 ' + ' '.join(host_public[:2]) + '\n')
known.chmod(0o644); os.chown(known, qnx.pw_uid, qnx.pw_gid)
client_config = client_root/'qnx_ssh_config'
client_config.write_text(f'''Host {source_name}
    HostName 10.0.2.100
    Port 2223
    User {user}
    IdentityFile /data/qfuse/{key_name}_ed25519
    UserKnownHostsFile /data/qfuse/{key_name}_known_hosts
    StrictHostKeyChecking yes
    IdentitiesOnly yes
    BatchMode yes
    ConnectTimeout 5
    ServerAliveInterval 1
    ServerAliveCountMax 2
''')
client_config.chmod(0o644); os.chown(client_config, qnx.pw_uid, qnx.pw_gid)
# A different valid public key tests host authentication without weakening it.
wrong_public = (client_root/'client_ed25519.pub').read_text().split()
bad_known = client_root/'bad_known_hosts'
bad_known.write_text('[10.0.2.100]:2223 ' + ' '.join(wrong_public[:2]) + '\n')
bad_config = client_root/'bad_ssh_config'
bad_config.write_text(client_config.read_text().replace(
    f'/data/qfuse/{key_name}_known_hosts', f'/data/qfuse/{key_name}_bad_known_hosts'))
for path in (bad_known, bad_config):
    path.chmod(0o644); os.chown(path, qnx.pw_uid, qnx.pw_gid)
data = export_root/'data'; data.mkdir(mode=0o755, exist_ok=True)
nested = data/'nested'; nested.mkdir(mode=0o755, exist_ok=True)
fixtures = {
    data/'greeting.txt': b'QFuse SSHFS over authenticated SSH\n',
    nested/'binary.dat': bytes(((i * 37) ^ (i >> 8) ^ (i >> 16)) & 255 for i in range(200037)),
    data/'empty': b'',
}
for path, contents in fixtures.items():
    path.write_bytes(contents); path.chmod(0o600 if writable else 0o444); os.chown(path, fixture_uid, fixture_uid)
if writable:
    for path in (data,nested):
        os.chown(path,fixture_uid,fixture_uid); path.chmod(0o700)
config = config_root/'sshd_config'
config.write_text(f'''Port 2223
ListenAddress 127.0.0.1
HostKey {config_root}/host_ed25519
PidFile {pidfile}
AuthorizedKeysFile {authorized}
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
PermitRootLogin no
AllowUsers {user}
AuthenticationMethods publickey
StrictModes yes
ChrootDirectory {export_root}
ForceCommand internal-sftp {' ' if writable else '-R'} -d /data
Subsystem sftp internal-sftp
PermitTTY no
AllowTcpForwarding no
AllowAgentForwarding no
X11Forwarding no
LogLevel ERROR
''')
Path('/run/sshd').mkdir(exist_ok=True)
subprocess.run(['/usr/sbin/sshd', '-t', '-f', str(config)], check=True)
if preparing:
    print('Prepared synthetic SFTP fixture without starting a service.')
    raise SystemExit(0)
subprocess.run(['/usr/sbin/sshd', '-f', str(config), '-E', str(config_root/'server.log')], check=True)
print('Started '+('writable' if writable else 'read-only')+' synthetic SFTP service on container loopback port 2223.')
