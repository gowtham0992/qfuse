#!/usr/bin/env python3
"""Install/verify only public QFuse SDK files after QNX qtargets builds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('action', choices=('headers', 'install', 'verify', 'uninstall'))
p.add_argument('--stage', required=True, type=Path)
p.add_argument('--prefix', default='/usr/local')
p.add_argument('--generated', type=Path)
p.add_argument('--architectures', nargs='+', choices=('aarch64le','x86_64'), required=True)
a = p.parse_args()
stage = a.stage.resolve()
sdk = Path(os.environ['QNX_TARGET']).resolve() if os.environ.get('QNX_TARGET') else None
if stage == Path('/') or (sdk and (stage == sdk or sdk in stage.parents)):
    raise SystemExit('Use a private staging root outside the SDP.')
relative = Path(a.prefix)
if not relative.is_absolute() or relative == Path('/') or '..' in relative.parts:
    raise SystemExit('PREFIX must be an absolute non-root prefix without parent traversal.')
lock = json.loads((root/'upstream.lock.json').read_text())
for arch in a.architectures:
    prefix = stage/arch/str(relative).lstrip('/')
    if a.action == 'uninstall':
        subprocess.run([sys.executable, str(root/'scripts/uninstall-qnx.py'), str(prefix)], check=True)
        continue
    if a.action == 'verify':
        env = dict(os.environ, QFUSE_INSTALL_PREFIX=str(prefix), QFUSE_ARCH=arch)
        if os.environ.get('QFUSE_TOOL_WRAPPER'):
            env['CC'] = os.environ['QFUSE_TOOL_WRAPPER']+' qcc'
        subprocess.run([sys.executable, str(root/'scripts/verify-package.py')], env=env, check=True)
        for tool in ('qfuse-mount','qfuse-unmount'):
            header = (prefix/'bin'/tool).read_bytes()[:20]
            machine = 183 if arch == 'aarch64le' else 62
            if header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != machine:
                raise SystemExit('Wrong architecture for '+tool)
        print('PASS native recipe target tools:', arch, flush=True)
        continue
    if not a.generated:
        p.error('install requires --generated')
    installed = [prefix/'lib/libfuse3.a', prefix/'bin/qfuse-mount', prefix/'bin/qfuse-unmount']
    if a.action == 'headers':
        installed = [item for item in installed if item.is_file()]
    if a.action == 'install' and any(not item.is_file() for item in installed):
        raise SystemExit('Native qtargets install is incomplete for '+arch+': '+str(prefix))
    sources = []
    for name, digest in lock['files'].items():
        if name.startswith('include/') and name.endswith('.h'):
            source = root/'.cache/libfuse'/name
            if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                raise SystemExit('Changed locked header: '+name)
            sources.append((source, 'include/fuse3/'+source.name))
    sources += [(a.generated/'libfuse_config.h','include/fuse3/libfuse_config.h'),
                (root/'include/qfuse/control.h','include/qfuse/control.h'),
                (root/'include/qfuse/admin.h','include/qfuse/admin.h'),
                (root/'include/qfuse/compat/dirent.h','include/qfuse/compat/dirent.h'),
                (root/'LICENSE','share/licenses/qfuse/QFuse-MIT'),
                (root/'.cache/libfuse/LGPL2.txt','share/licenses/qfuse/libfuse-LGPL2.txt')]
    for source, target in sources:
        path = prefix/target
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, path); path.chmod(0o644); installed.append(path)
    pc = prefix/'lib/pkgconfig'; pc.mkdir(parents=True, exist_ok=True)
    (pc/'fuse3.pc').write_text('prefix='+str(prefix)+'''\nexec_prefix=${prefix}
libdir=${prefix}/lib
includedir=${prefix}/include
Name: fuse3
Description: QFuse tested high-level FUSE subset for QNX 8.0
Version: 3.16.2
Libs: -L${libdir} -lfuse3
Cflags: -I${includedir}/fuse3 -D_FILE_OFFSET_BITS=64
''')
    (pc/'qfuse-dirent.pc').write_text('prefix='+str(prefix)+'''\nincludedir=${prefix}/include
Name: qfuse-dirent
Description: Opt-in QNX directory-entry source adapter
Version: 1.0
Cflags: -I${includedir}/qfuse/compat -D_QNX_SOURCE
''')
    installed += [pc/'fuse3.pc',pc/'qfuse-dirent.pc']
    manifest = prefix/'share/qfuse/install-manifest.json'; manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps({str(path.relative_to(prefix)):hashlib.sha256(path.read_bytes()).hexdigest() for path in installed},sort_keys=True,indent=2)+'\n')
    print('Installed public QFuse SDK files:',prefix,flush=True)
