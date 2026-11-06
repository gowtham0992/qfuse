#!/usr/bin/env python3
"""Exercise the real installer/uninstaller against disposable user entries."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='qfuse-uninstall.') as directory:
    prefix=Path(directory)
    env=dict(os.environ,QFUSE_INSTALL_PREFIX=directory)
    subprocess.run(['bash',str(root/'scripts/package-qnx.sh')],env=env,check=True)
    pc=prefix/'lib/pkgconfig/fuse3.pc';original=pc.read_bytes()
    pc.write_bytes(original+b'# user modification\n')
    keep=prefix/'keep.txt';keep.write_text('unrelated user file\n')
    empty=prefix/'user-empty-directory';empty.mkdir()
    command=['python3',str(root/'scripts/uninstall-qnx.py'),directory]
    result=subprocess.run(command,capture_output=True,text=True)
    assert result.returncode!=0 and 'Preserved modified installed files' in result.stderr
    assert pc.is_file() and keep.is_file() and empty.is_dir()
    print('PASS uninstall negative: modified installed file and unrelated entries preserved',flush=True)
    pc.write_bytes(original)
    subprocess.run(command,check=True)
    assert not pc.exists() and keep.read_text()=='unrelated user file\n' and empty.is_dir()
    print('PASS uninstall: unchanged package files removed; unrelated file and directory preserved')
