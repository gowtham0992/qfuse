#!/usr/bin/env python3
"""Compile upstream examples using only the installed fuse3 pkg-config package."""
import os
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
root=Path(__file__).resolve().parents[1]
prefix=Path(os.environ.get('QFUSE_INSTALL_PREFIX','/home/qnx/qfuse-install'))
env=dict(os.environ,PKG_CONFIG_LIBDIR=str(prefix/'lib/pkgconfig'))
env.pop('PKG_CONFIG_SYSROOT_DIR',None)
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','fuse3'],env=env,text=True))
libs=shlex.split(subprocess.check_output(['pkg-config','--libs','fuse3'],env=env,text=True))
out=prefix/'verify';out.mkdir(exist_ok=True)
import shutil
lock=json.loads((root/'upstream.lock.json').read_text())
for name in ('example/hello.c','example/passthrough.c','example/passthrough_helpers.h'):
    digest=hashlib.sha256((root/'.cache/libfuse'/name).read_bytes()).hexdigest()
    if digest!=lock['files'][name]:raise SystemExit('Changed upstream source: '+name)
    print('Verified unchanged upstream source:',name,digest,flush=True)
shutil.copyfile(root/'.cache/libfuse/example/hello.c',out/'hello.c')
shutil.copyfile(root/'.cache/libfuse/example/passthrough_helpers.h',out/'passthrough_helpers.h')
shutil.copyfile(root/'.cache/libfuse/example/passthrough.c',out/'passthrough.c')
cc=shlex.split(os.environ.get('CC','qcc'))
target_arch=os.environ.get('QFUSE_ARCH','x86_64')
if target_arch not in ('x86_64','aarch64le'):raise SystemExit('Unsupported QFUSE_ARCH')
common=cc+['-Vgcc_nto'+target_arch,'-std=c11','-D_QNX_SOURCE']+flags
subprocess.run(common+[str(out/'hello.c')]+libs+['-o',str(out/'hello')],check=True)
# The opt-in adapter adds d_type without changing the source or native SDK ABI.
adapter_flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','qfuse-dirent'],env=env,text=True))
subprocess.run(common+adapter_flags+['-DHAVE_UTIMENSAT',str(out/'passthrough.c')]+libs+['-o',str(out/'passthrough')],check=True)
print('PASS installed package: unchanged upstream hello and passthrough link using installed fuse3 and opt-in qfuse-dirent metadata.')
