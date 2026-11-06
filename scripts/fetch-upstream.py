#!/usr/bin/env python3
"""Fetch the small, hash-locked upstream fixture set; never execute downloads."""
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
lock = json.loads((ROOT / 'upstream.lock.json').read_text())
for name, expected in lock['files'].items():
    dest = ROOT / '.cache/libfuse' / name
    if dest.exists() and hashlib.sha256(dest.read_bytes()).hexdigest() == expected:
        continue
    url = f"https://raw.githubusercontent.com/libfuse/libfuse/{lock['tag']}/{name}"
    data = subprocess.check_output([
        'curl', '--fail', '--silent', '--show-error', '--location',
        '--proto', '=https', '--proto-redir', '=https', '--max-time', '30', url,
    ])
    if hashlib.sha256(data).hexdigest() != expected:
        raise SystemExit(f'Checksum mismatch: {name}; refusing to use it')
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
print(f"Verified {len(lock['files'])} upstream files ({lock['tag']}).")
