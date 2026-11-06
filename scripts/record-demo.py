#!/usr/bin/env python3
"""Record actual command output as an asciicast v2 file and text transcript."""
import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import struct
import termios
import time
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('output',type=Path)
parser.add_argument('command',nargs=argparse.REMAINDER)
args=parser.parse_args()
command=args.command
if command[:1]==['--']:command=command[1:]
if not command:parser.error('provide a command after --')
args.output.parent.mkdir(parents=True,exist_ok=True)
started=time.monotonic()
pid,master=pty.fork()
if pid==0:os.execvp(command[0],command)
fcntl.ioctl(master,termios.TIOCSWINSZ,struct.pack('HHHH',32,110,0,0))
chunks=[]
with args.output.open('w') as stream:
    stream.write(json.dumps({'version':2,'width':110,'height':32,'timestamp':int(time.time()),'title':'QFuse native QNX functional demo','env':{'TERM':'xterm-256color'}})+'\n')
    while True:
        ready,_,_=select.select([master],[],[],1)
        if not ready:continue
        try:data=os.read(master,65536)
        except OSError as error:
            if error.errno==errno.EIO:break
            raise
        if not data:break
        value=data.decode('utf-8','replace');chunks.append(value)
        stream.write(json.dumps([time.monotonic()-started,'o',value])+'\n');stream.flush()
        print(value,end='',flush=True)
os.close(master)
_,status=os.waitpid(pid,0)
text=re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','', ''.join(chunks)).replace('\r','')
args.output.with_suffix('.txt').write_text(text)
if not os.WIFEXITED(status) or os.WEXITSTATUS(status):raise SystemExit('Recorded command failed; retain the recording as diagnostic evidence.')
print('Saved actual terminal recording:',args.output)
