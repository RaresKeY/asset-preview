#!/usr/bin/env python3
"""Validate release previews in an isolated hardware-rendered display session."""
import argparse, json, os, shutil, struct, subprocess, tempfile, time, zlib
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('executable');parser.add_argument('--output',type=Path,required=True);parser.add_argument('--video',type=Path);args=parser.parse_args()
exe=str(Path(args.executable).resolve());args.output.mkdir(parents=True,exist_ok=True)
def call(*words):
    result=subprocess.run([exe,*words],check=True,capture_output=True,text=True)
    return json.loads(result.stdout) if words[:1]==('status',) else result.stdout
with tempfile.TemporaryDirectory(prefix='asset-preview-release-check-') as d:
    fixture=Path(d);os.environ['ASSET_PREVIEW_RUNTIME_DIR']=str(fixture/'runtime');os.environ['ASSET_PREVIEW_STATE_DIR']=str(fixture/'state')
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    image=fixture/'texture.png';image.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',64,64,8,2,0,0,0))+chunk(b'IDAT',zlib.compress((b'\0'+bytes((70,150,190))*64)*64))+chunk(b'IEND',b''))
    model=fixture/'cube.obj';model.write_text('v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\nv -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\nf 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 4 8 7 3\nf 1 5 8 4\nf 2 3 7 6\n')
    video=fixture/'video.mp4'
    if args.video:shutil.copy2(args.video,video)
    else:subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=320x240:rate=12','-t','3','-c:v','mpeg4','-an',str(video)],check=True)
    try:
        generator=fixture/'generate.sh'
        generator.write_text('#!/bin/sh\nset -eu\n[ -z "${ASSET_PREVIEW_BUNDLE_ROOT:-}" ]\n[ -z "${PYTHONHOME:-}" ]\ncp "$1" "$2"\n')
        generator.chmod(0o755)
        generated=fixture/'generated.png'
        call('add',str(generated),'--id','image','--label','Generated image','--watch',str(generator),'--exec',str(generator),str(image),str(generated))
        call('add',str(model),'--id','model','--label','Model')
        call('material',str(image),'--id','material','--shape','sphere')
        call('add',str(video),'--id','video','--label','Video')
        call('layout','grid','--size','2','--compact');call('gui')
        deadline=time.monotonic()+60
        while True:
            status=call('status','--json')
            if len(status['entries'])==4 and all(e.get('active') and e.get('metrics',{}).get('loads',0)>0 for e in status['entries']):break
            if time.monotonic()>deadline:raise RuntimeError('Preview loading timed out: '+json.dumps(status))
            time.sleep(.2)
        renderers=sorted({e['metrics']['renderer'] for e in status['entries'] if 'renderer' in e.get('metrics',{})})
        if not renderers or any(any(t in r.lower() for t in ('llvmpipe','softpipe','software','unknown')) for r in renderers):raise RuntimeError('Hardware renderer not confirmed: '+str(renderers))
        call('settings','video','{"paused":true}');time.sleep(.5)
        call('capture',str((args.output/'release-grid.png').resolve()))
        # Record summary only; no personal paths or local process IDs.
        report={'generator_environment_restored':next(e for e in status['entries'] if e['id']=='image')['builds']>=1,'renderers':renderers,'platform':status['platform'],'previews':[{ 'id':e['id'],'kind':e['kind'],'loaded':e['metrics']['loads']} for e in status['entries']]}
        (args.output/'validation.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report))
    finally:
        call('stop')
