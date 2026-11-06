#!/usr/bin/env python3
"""Render an actual asciicast as a captioned MP4 (requires Pillow and ffmpeg).

Playback is deliberately slowed for reading; this is not a performance video.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import textwrap
from PIL import Image, ImageDraw, ImageFont

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('cast', type=Path);p.add_argument('output', type=Path)
p.add_argument('--font',default='/System/Library/Fonts/Menlo.ttc')
p.add_argument('--slow',type=float,default=4)
a=p.parse_args()
if a.slow < 1: p.error('slow must be at least 1')
records=[json.loads(s) for s in a.cast.read_text().splitlines()]
events=[r for r in records[1:] if r[1]=='o']
font=ImageFont.truetype(a.font,23);small=ImageFont.truetype(a.font,20);big=ImageFont.truetype(a.font,42)
width,height,fps=1920,1080,12
intro,outro=10,15
duration=intro+events[-1][0]*a.slow+outro
encoder=subprocess.Popen(['ffmpeg','-y','-loglevel','error','-f','rawvideo','-pixel_format','rgb24','-video_size',f'{width}x{height}',
    '-framerate',str(fps),'-i','-','-an','-c:v','libx264','-preset','fast','-crf','22','-pix_fmt','yuv420p','-movflags','+faststart',str(a.output)],stdin=subprocess.PIPE)
index=0;terminal='';chapter='Build on the laptop; run on QNX'
for frame in range(int(duration*fps)+1):
    t=frame/fps
    while index<len(events) and events[index][0] <= max(0,(t-intro)/a.slow):
        terminal+=events[index][2];index+=1
    cleaned=re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',terminal).replace('\r','')
    chapters=re.findall(r'=== (.*?) ===',cleaned)
    if chapters:chapter=chapters[-1]
    picture=Image.new('RGB',(width,height),'#0b1118');draw=ImageDraw.Draw(picture)
    draw.text((64,40),'QFuse',font=big,fill='#eaf0f5')
    draw.text((310,58),'Existing FUSE filesystems as native QNX processes',font=small,fill='#83a5bb')
    draw.line((64,117,width-64,117),fill='#284152',width=2)
    if t<intro:
        draw.text((92,235),'Build on your laptop. Run from QNX.',font=big,fill='#70e5cf')
        for i,line in enumerate(['Mount build output through SSHFS.', 'Edit and rebuild through the same mount.',
            'Interrupt a reader when the remote server stalls.', 'Query, stop and remount through separate native control.']):
            draw.text((92,360+i*68),line,font=font,fill='#eaf0f5')
        draw.text((92,755),'Actual QNX 8.0 aarch64le execution in QEMU',font=font,fill='#83a5bb')
    elif t < duration-outro:
        draw.text((64,148),chapter,font=font,fill='#70e5cf')
        lines=[]
        for line in cleaned.splitlines():lines.extend(textwrap.wrap(line,width=116,replace_whitespace=False,drop_whitespace=False) or [''])
        draw.rounded_rectangle((55,205,width-55,985),radius=14,fill='#111c27',outline='#284152',width=2)
        for j,line in enumerate(lines[-24:]):
            color='#70e5cf' if line.startswith(('PASS','===','Operations:','Stopped')) else '#eaf0f5'
            draw.text((78,226+j*30),line,font=font,fill=color)
    else:
        draw.text((92,232),'A stalled remote read. An interrupted client wait.',font=big,fill='#70e5cf')
        for j,line in enumerate(['The reader returned at its timeout.', 'Status stayed available while its callback remained pending.',
            'Explicit force stop and remount restored access.', 'Namespace changes can wait behind a stalled read.', 'Source compatibility is a tested subset; mounting requires root.',
            'QEMU functional evidence does not establish hard real-time guarantees.']):
            draw.text((92,357+j*61),line,font=font,fill='#eaf0f5' if j<3 else '#83a5bb')
    draw.text((64,1025),f'Actual terminal output | Playback {a.slow:g}x slower for readability | No performance claim',font=small,fill='#83a5bb')
    encoder.stdin.write(picture.tobytes())
encoder.stdin.close()
if encoder.wait():raise SystemExit('Video encoder failed')
print(a.output.resolve())
