#!/usr/bin/env python3
"""Explicit public source selection; excludes private planning and old captures."""
from pathlib import Path
ROOT_FILES = ('LICENSE','README.md','THIRD_PARTY.md','Makefile','Dockerfile.dev','Dockerfile.verify',
              'Dockerfile.target','Dockerfile.share','.gitignore','.gitattributes','.dockerignore',
              '.clang-format','upstream.lock.json','sshfs-dependencies.lock.json','bindfs.lock.json')
EXTENSIONS={'.c','.h','.inc','.py','.sh','.patch','.md','.txt','.cast','.json','.tsv','.yml','.yaml'}

def public_files(root:Path):
    files=[]
    for name in ('src','include','tests','scripts','patches','integration','examples','.github','docs/guide','docs/proof'):
        base=root/name
        if not base.exists():continue
        for path in base.rglob('*'):
            if set(path.relative_to(root).parts) & {'build','stage','.cache','.git','__pycache__'}:continue
            if path.is_symlink():raise ValueError('Source export does not follow symlinks: '+str(path))
            if path.is_file() and '__pycache__' not in path.parts and (path.suffix in EXTENSIONS or path.name in {'Makefile','LICENSE','sshd_config','.gitignore'}):
                files.append(path)
    for name in ROOT_FILES:
        path=root/name
        if path.is_file():files.append(path)
    return sorted(set(files))
