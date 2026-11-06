#!/usr/bin/env python3
"""Fetch the pinned optional bindfs source archive; keep upstream Git history out."""
from pathlib import Path
import hashlib,json,urllib.request
root=Path(__file__).resolve().parents[1]
lock=json.loads((root/'bindfs.lock.json').read_text())
path=root/'.cache'/('bindfs-'+lock['version']+'.tar.gz');path.parent.mkdir(exist_ok=True)
if not path.exists():
    data=urllib.request.urlopen(lock['url'],timeout=60).read()
    if hashlib.sha256(data).hexdigest()!=lock['sha256']:raise SystemExit('bindfs download checksum mismatch')
    path.write_bytes(data)
if hashlib.sha256(path.read_bytes()).hexdigest()!=lock['sha256']:raise SystemExit('Changed bindfs archive')
print('Verified bindfs',lock['version'],lock['commit'])
