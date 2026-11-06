#!/usr/bin/env python3
"""Fetch hash-locked public SSHFS/dependency sources; never execute downloads."""
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
lock = json.loads((ROOT / 'sshfs-dependencies.lock.json').read_text())
for name, item in lock['files'].items():
    dest = ROOT / '.cache' / (name if name.startswith('sshfs/') else 'sources/' + name)
    if dest.is_file() and hashlib.sha256(dest.read_bytes()).hexdigest() == item['sha256']:
        continue
    data = subprocess.check_output([
        'curl', '--fail', '--silent', '--show-error', '--location',
        '--proto', '=https', '--proto-redir', '=https', '--max-time', '120', item['url'],
    ])
    if hashlib.sha256(data).hexdigest() != item['sha256']:
        raise SystemExit('Checksum mismatch; refusing dependency: ' + name)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
print(f"Verified {len(lock['files'])} SSHFS/dependency source files.")
