#!/bin/sh
# The image contains no credentials. Only task-generated server keys are
# mounted at runtime; the SDK and client private key are not exposed here.
set -eu
mkdir -p /run/sshd /etc/qfuse
cp /keys/host_ed25519 /etc/ssh/ssh_host_ed25519_key
cp /keys/host_ed25519.pub /etc/ssh/ssh_host_ed25519_key.pub
cp /keys/authorized_keys /etc/qfuse/authorized_keys
chown root:root /etc/ssh/ssh_host_ed25519_key /etc/ssh/ssh_host_ed25519_key.pub /etc/qfuse/authorized_keys
chmod 600 /etc/ssh/ssh_host_ed25519_key
chmod 644 /etc/ssh/ssh_host_ed25519_key.pub /etc/qfuse/authorized_keys
python3 -Wignore::DeprecationWarning - <<'PY'
import crypt,secrets,subprocess
value=crypt.crypt(secrets.token_urlsafe(32),crypt.mksalt(crypt.METHOD_SHA512))
subprocess.run(['chpasswd','-e'],input='qfuse-share:'+value+'\n',text=True,check=True)
PY
exec /usr/sbin/sshd -D -e -f /etc/qfuse/sshd_config
