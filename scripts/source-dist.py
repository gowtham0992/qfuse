#!/usr/bin/env python3
"""Emit a deterministic public source archive without SDK/private development data."""
import gzip
import importlib.util
from pathlib import Path
import tarfile
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('public_files',root/'scripts/public-files.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
files=module.public_files(root)
out=root/'build/qfuse-source.tar.gz';out.parent.mkdir(exist_ok=True)
with out.open('wb') as raw,gzip.GzipFile(filename='',fileobj=raw,mode='wb',mtime=0) as gz,tarfile.open(fileobj=gz,mode='w') as tar:
    for path in files:
        info=tar.gettarinfo(str(path),arcname='qfuse/'+str(path.relative_to(root)))
        info.uid=info.gid=info.mtime=0;info.uname=info.gname=''
        with path.open('rb') as source:tar.addfile(info,source)
print('Public source archive:',out)
print('Files:',len(files),'(no SDK, runtime/image, keys, credentials, caches or internal planning)')
