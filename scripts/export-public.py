#!/usr/bin/env python3
"""Copy the audited public source set into an empty publication checkout."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('public_files',root/'scripts/public-files.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('destination',type=Path)
a=parser.parse_args();destination=a.destination.resolve()
if destination==root or root in destination.parents and 'build' not in destination.relative_to(root).parts:
    raise SystemExit('Use an empty build/public directory or separate publication directory.')
if destination.exists() and any(destination.iterdir()):raise SystemExit('Publication destination must be empty.')
files=module.public_files(root)
for p in files:
    raw=p.read_bytes()
    if re.search(rb'^-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----',raw,re.M):raise SystemExit('Private key block rejected')
    if p.suffix in {'.md','.txt','.cast'} and re.search(rb'(?i)\b(Fable|Claude)\b',raw):raise SystemExit('Internal planning reference rejected: '+str(p.relative_to(root)))
for p in files:
    target=destination/p.relative_to(root);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
print('Exported',len(files),'public source files to',destination)
