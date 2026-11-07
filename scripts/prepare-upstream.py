#!/usr/bin/env python3
"""Verify locked public fixture sources and prepare the minimal FUSE parser."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
a = parser.parse_args()
upstream = root/'.cache/libfuse'
lock = json.loads((root/'upstream.lock.json').read_text())
for name, expected in lock['files'].items():
    path = upstream/name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise SystemExit('Missing or changed '+name+'; run scripts/fetch-upstream.py')
source = (upstream/'lib/fuse_opt.c').read_text()
for header in ('fuse_config.h', 'fuse_i.h', 'fuse_misc.h'):
    include = '#include "'+header+'"'
    if source.count(include) != 1:
        raise SystemExit('Unexpected upstream include: '+header)
    source = source.replace(include, '/* QFuse callback fixture: private include removed. */')
source = '#include "fuse_log.h"\n'+source
a.output.mkdir(parents=True, exist_ok=True)
for name, content in [('fuse_opt.c', source), ('libfuse_config.h', '/* Callback fixture only. */\n')]:
    destination = a.output/name
    data = content.encode()
    if destination.is_file() and destination.read_bytes() == data:
        continue
    with tempfile.NamedTemporaryFile(dir=a.output, delete=False) as temporary:
        temporary.write(data)
        os.fchmod(temporary.fileno(), 0o644)
        temp_path = Path(temporary.name)
    temp_path.replace(destination)
print('Verified locked FUSE sources; prepared', a.output)
