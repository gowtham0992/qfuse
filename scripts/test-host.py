#!/usr/bin/env python3
"""Compile original upstream hello.c against the portable callback core.

This is an in-process callback test. It does NOT mount a filesystem.
"""
import hashlib
import argparse
import json
import os
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / '.cache/libfuse'
arguments = argparse.ArgumentParser(description=__doc__)
arguments.add_argument('--qnx', action='store_true', help='cross-compile with qcc; do not run the target executable on the host')
args = arguments.parse_args()
BUILD = ROOT / ('build/qnx' if args.qnx else 'build/host')
lock = json.loads((ROOT / 'upstream.lock.json').read_text())
for name, expected in lock['files'].items():
    path = UPSTREAM / name
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise SystemExit(f'Missing or changed {name}; run python3 scripts/fetch-upstream.py')
BUILD.mkdir(parents=True, exist_ok=True)
# No optional libfuse features enabled for this callback fixture.
(BUILD / 'libfuse_config.h').write_text('/* Callback fixture only. */\n')
# Port the upstream option parser by replacing private include dependencies.
# No parser logic or upstream example is changed.
parser = (UPSTREAM / 'lib/fuse_opt.c').read_text()
for include in ['fuse_config.h', 'fuse_i.h', 'fuse_misc.h']:
    old = f'#include "{include}"'
    if parser.count(old) != 1:
        raise SystemExit(f'Unexpected upstream include: {include}')
    parser = parser.replace(old, '/* QFuse callback fixture: private include removed. */')
parser = '#include "fuse_log.h"\n' + parser
(BUILD / 'fuse_opt.c').write_text(parser)
cc = shlex.split(os.environ.get('CC', 'qcc' if args.qnx else 'cc'))
flags = ['-std=c11', '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE', '-D_FILE_OFFSET_BITS=64',
         '-g', '-O1', '-fno-omit-frame-pointer',
         '-I' + str(ROOT / 'include'), '-I' + str(UPSTREAM / 'include'), '-I' + str(BUILD)]
target_arch=os.environ.get('QFUSE_ARCH','x86_64')
if target_arch not in ('x86_64','aarch64le'):raise SystemExit('Unsupported QFUSE_ARCH')
flags += ['-Vgcc_nto'+target_arch, '-D_QNX_SOURCE'] if args.qnx else ['-fsanitize=address,undefined']

def compile_one(source, output, extra=()):
    subprocess.run(cc + flags + list(extra) + ['-c', str(source), '-o', str(output)], check=True)

objects = []
for name, source, extra in [
    ('core', ROOT / 'src/core.c', ['-Wall', '-Wextra', '-Werror']),
    ('hello', UPSTREAM / 'example/hello.c', ['-Dmain=qf_upstream_hello_main']),
    ('fuse_opt', BUILD / 'fuse_opt.c', []),
    ('fuse_log', UPSTREAM / 'lib/fuse_log.c', []),
    ('test_core', ROOT / 'tests/test_core.c', ['-Wall', '-Wextra', '-Werror']),
]:
    obj = BUILD / (name + '.o')
    compile_one(source, obj, extra)
    objects.append(str(obj))
binary = BUILD / 'test-core'
subprocess.run(cc + flags + objects + ['-pthread', '-o', str(binary)], check=True)
if args.qnx:
    print(f'Cross-compiled QNX {target_arch} callback test: {binary}; target execution still required.')
else:
    subprocess.run([str(binary)], check=True)

mutation = BUILD / 'test-mutation-core'
subprocess.run(cc + flags + ['-Wall', '-Wextra', '-Werror', str(ROOT/'tests/test_mutation_core.c'), str(BUILD/'core.o'), '-o', str(mutation)], check=True)
if not args.qnx:
    subprocess.run([str(mutation)], check=True)
    print('PASS writable core: callback state, caller context, handles, bounds and missing operations')
