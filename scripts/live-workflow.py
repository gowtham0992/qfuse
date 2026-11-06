#!/usr/bin/env python3
"""Optional local Docker/QEMU lab: share build outputs, mount, run and recover.

The share is read-only, key-only and container-loopback-only. SDK and client
private keys never enter the share container. Targets use their own licensed
runtime. The native qfuse-mount tool also works without this lab.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
SHARE=ROOT/'build/live-share'
SDK=os.environ.get('QFUSE_SDK_CONTAINER','qfuse-dev')
NAME='qfuse-live-share'
IMAGE='qfuse-share:local'
CONTEXT=os.environ.get('QFUSE_DOCKER_CONTEXT')
DOCKER=['docker']+(['--context',CONTEXT] if CONTEXT else [])
if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*',SDK):raise SystemExit('Invalid SDK container name')
def run(args,**kw):return subprocess.run(DOCKER+args,check=True,**kw)
def sdk(args,arch=None,root=False,**kw):
    flags=['-e','QEMU_CPU=Nehalem']+(['-u','0'] if root else [])+(['-e','QFUSE_ARCH='+arch] if arch else [])
    return run(['exec']+flags+[SDK]+args,**kw)
def inspect(name):
    p=subprocess.run(DOCKER+['inspect',name],capture_output=True,text=True)
    return json.loads(p.stdout)[0] if p.returncode==0 else None

def initialize():
    (SHARE/'src').mkdir(parents=True,exist_ok=True)
    if not (SHARE/'src/hello.c').exists():(SHARE/'src/hello.c').write_bytes((ROOT/'examples/live/hello.c').read_bytes())
    if not (SHARE/'message.txt').exists():(SHARE/'message.txt').write_text('Live settings from the laptop\n')
    for p in [SHARE,SHARE/'src']:p.chmod(0o755)
    (SHARE/'src/hello.c').chmod(0o644);(SHARE/'message.txt').chmod(0o644)

def up():
    initialize()
    owner=inspect(SDK)
    if not owner:raise SystemExit('Start your SDK container first.')
    volumes=[m for m in owner['Mounts'] if m['Destination']=='/home/qnx' and m['Type']=='volume']
    if len(volumes)!=1:raise SystemExit('This optional lab expects a private SDK home volume at /home/qnx.')
    workspace=[m for m in owner['Mounts'] if m['Destination']=='/workspace' and m['Type']=='bind']
    if len(workspace)!=1 or Path(workspace[0]['Source']).resolve()!=ROOT:raise SystemExit('SDK /workspace must bind this checkout.')
    code='''from pathlib import Path
import os,subprocess
root=Path('/home/qnx/qfuse-live');server=root/'server';client=root/'client'
for d in (root,server,client):d.mkdir(mode=0o700,exist_ok=True);d.chmod(0o700)
for p in (server/'host_ed25519',client/'client_ed25519'):
 if not p.exists():subprocess.run(['ssh-keygen','-q','-t','ed25519','-N','','-f',str(p)],check=True,env=dict(os.environ,QEMU_CPU='Nehalem'))
 p.chmod(0o600)
(server/'authorized_keys').write_bytes((client/'client_ed25519.pub').read_bytes())
public=(server/'host_ed25519.pub').read_text().split()
(client/'known_hosts').write_text('[10.0.2.100]:2223 '+' '.join(public[:2])+'\\n')
(client/'live_ssh_config').write_text('Host qfuse-laptop\\n    HostName 10.0.2.100\\n    Port 2223\\n    User qfuse-share\\n    IdentityFile /data/qfuse/live_ed25519\\n    UserKnownHostsFile /data/qfuse/live_known_hosts\\n    StrictHostKeyChecking yes\\n    IdentitiesOnly yes\\n    BatchMode yes\\n    ConnectTimeout 5\\n    ServerAliveInterval 30\\n    ServerAliveCountMax 2\\n')
'''
    sdk(['python3','-c',code])
    for writable in (False,True):sdk(['python3','/workspace/scripts/setup-sftp-test.py']+(['--writable'] if writable else [])+['--stop'],root=True,stdout=subprocess.DEVNULL)
    existing=inspect(NAME)
    if existing:
        if existing['Config'].get('Labels',{}).get('qfuse.role')!='live-share':raise SystemExit('Refusing to replace an unrelated container.')
        run(['rm','-f',NAME],stdout=subprocess.DEVNULL)
    run(['build','-f','Dockerfile.share','-t',IMAGE,'.'],cwd=ROOT,stdout=subprocess.DEVNULL)
    run(['run','-d','--rm','--name',NAME,'--label','qfuse.role=live-share','--network','container:'+SDK,
         '--mount',f'type=volume,src={volumes[0]["Name"]},dst=/keys,volume-subpath=qfuse-live/server,readonly',
         '--mount',f'type=bind,src={SHARE},dst=/srv/qfuse/data,readonly',IMAGE],stdout=subprocess.DEVNULL)
    for attempt in range(50):
        ready=subprocess.run(DOCKER+['exec',NAME,'python3','-c',
            "import socket; s=socket.create_connection(('127.0.0.1',2223),.5); assert s.recv(128).startswith(b'SSH-2.0-'); s.close()"],
            stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        if ready.returncode==0:break
        if not inspect(NAME):raise SystemExit('Live share exited before readiness; inspect its container logs.')
        time.sleep(.1)
    else:raise SystemExit('Live share did not become ready; inspect its container logs.')
    print('Read-only laptop share ready. Only build/live-share is exported; no host ports are published.')

def build(arch):
    initialize()
    result=sdk(['bash','/workspace/scripts/build-live-example.sh','/workspace/build/live-share/src/hello.c'],arch=arch,capture_output=True,text=True)
    records=[line for line in result.stdout.splitlines() if line.startswith('QFUSE_LIVE_ARTIFACT=')]
    if len(records)!=1:raise SystemExit('Build did not produce an artifact record.')
    artifact=json.loads(records[0].split('=',1)[1]);folder=SHARE/arch;versions=folder/'.versions';versions.mkdir(parents=True,exist_ok=True)
    temporary=versions/(artifact['sha256']+'.next');destination=versions/artifact['sha256']
    run(['cp',SDK+':'+artifact['path'],str(temporary)],stdout=subprocess.DEVNULL)
    if hashlib.sha256(temporary.read_bytes()).hexdigest()!=artifact['sha256']:raise SystemExit('Local artifact checksum mismatch')
    temporary.chmod(0o755);temporary.replace(destination)
    link=folder/'hello.next';link.unlink(missing_ok=True);link.symlink_to('.versions/'+destination.name);link.replace(folder/'hello')
    folder.chmod(0o755);versions.chmod(0o755)
    print(f'Built {arch}/hello in the laptop share. No deployment copy to QNX.')

def target(command,arch):return sdk(['bash','/workspace/scripts/target-exec.sh',command],arch=arch)
def mount(arch):
    sdk(['bash','/workspace/scripts/target-copy.sh','/home/qnx/qfuse-live/client/client_ed25519','/data/qfuse/live_ed25519'],arch=arch)
    sdk(['bash','/workspace/scripts/target-copy.sh','/home/qnx/qfuse-live/client/known_hosts','/data/qfuse/live_known_hosts'],arch=arch)
    sdk(['bash','/workspace/scripts/target-copy.sh','/home/qnx/qfuse-live/client/live_ssh_config','/data/qfuse/live_ssh_config'],arch=arch)
    target('LD_LIBRARY_PATH=/data/qfuse/lib /data/qfuse/qfuse-mount sshfs -F /data/qfuse/live_ssh_config qfuse-laptop:/data /data/qfuse-laptop',arch)

def share_action(action):
    container=inspect(NAME)
    if not container or container['Config'].get('Labels',{}).get('qfuse.role')!='live-share':raise SystemExit('No owned live-share container.')
    run([{'cut':'pause','restore':'unpause','down':'stop'}[action],NAME],stdout=subprocess.DEVNULL)
    print({'cut':'Remote SFTP service paused; target control is independent.','restore':'Remote SFTP service restored.','down':'Live share stopped.'}[action])

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('action',choices=['up','build','mount','run','status','stop','cut','restore','down'])
parser.add_argument('--arch',choices=['x86_64','aarch64le'],default='x86_64')
a=parser.parse_args()
if a.action=='up':up()
elif a.action=='build':build(a.arch)
elif a.action=='mount':mount(a.arch)
elif a.action=='run':target('/data/qfuse-laptop/'+a.arch+'/hello /data/qfuse-laptop/message.txt',a.arch)
elif a.action=='status':target('/data/qfuse/qfuse-mount status /data/qfuse-laptop',a.arch)
elif a.action=='stop':target('/data/qfuse/qfuse-mount stop --force /data/qfuse-laptop',a.arch)
else:share_action(a.action)
