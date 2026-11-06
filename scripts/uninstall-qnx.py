#!/usr/bin/env python3
"""Remove only unchanged files recorded by the QFuse package installer."""
import hashlib
import json
from pathlib import Path
import sys
if len(sys.argv)!=2:raise SystemExit('Usage: uninstall-qnx.py PREFIX')
prefix=Path(sys.argv[1]).resolve()
if prefix==Path('/') or '/qnx800' in str(prefix):raise SystemExit('Refusing an unsafe prefix')
manifest=prefix/'share/qfuse/install-manifest.json'
items=json.loads(manifest.read_text())
failed=[]
for name,digest in items.items():
 path=prefix/name
 if Path(name).is_absolute() or '..' in Path(name).parts or prefix not in path.resolve().parents:raise SystemExit('Unsafe manifest path')
 if not path.exists():continue
 if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest()!=digest:failed.append(name);continue
 path.unlink()
if failed:raise SystemExit('Preserved modified installed files: '+', '.join(failed))
manifest.unlink()
directories={manifest.parent}
for name in items:
 for directory in (prefix/name).parents:
  if directory==prefix:break
  directories.add(directory)
for directory in sorted(directories,key=lambda p:len(p.parts),reverse=True):
 try:directory.rmdir()
 except OSError:pass
print('Removed only unchanged QFuse installation files.')
