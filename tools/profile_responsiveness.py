#!/usr/bin/env python3
"""Opt-in completed-load timings and concurrent socket responsiveness.

Run under tools/offscreen.sh. Each run owns private state/cache and never runs
registered generators. Ping round trips measure event-loop service latency, not
display frame time. Filesystem/driver caches and other desktop load are retained.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from integration import png, XInput, ticks, memory

def rpc(path,request,timeout=60):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(timeout); connection.connect(str(path))
        connection.sendall(json.dumps(request).encode()+b'\n'); data=bytearray()
        while b'\n' not in data:
            part=connection.recv(65536)
            if not part: raise RuntimeError('Service closed')
            data.extend(part)
            if len(data)>8*1024*1024: raise RuntimeError('Response exceeded profiling bound')
    result=json.loads(data.split(b'\n',1)[0])
    if not result.get('ok'): raise RuntimeError(result)
    return result

def percentile(values,p):
    return sorted(values)[min(len(values)-1,int((len(values)-1)*p))] if values else 0

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,default=ROOT/'build/asset-preview-server')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--real-model',type=Path)
    parser.add_argument('--perf',action='store_true',help='Collect attributed samples; do not compare these as clean timings')
    parser.add_argument('--count',type=int,default=96)
    parser.add_argument('--edge',type=int,default=2048)
    args=parser.parse_args(); args.binary=args.binary.resolve()
    assert 16<=args.count<=256 and 64<=args.edge<=4096
    args.output.mkdir(parents=True,exist_ok=True)
    report={'binary':str(args.binary),'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        'instrumented_perf':args.perf,
        'scope':'Private native service; ping round trips during each completed workload, not display latency. Host filesystem/driver caches are retained.',
        'host_load_before':Path('/proc/loadavg').read_text().strip(),'phases':[]}
    with tempfile.TemporaryDirectory(prefix='ap-latency-') as directory:
        root=Path(directory); runtime=root/'r'; state=root/'s'
        runtime.mkdir(mode=0o700); state.mkdir(mode=0o700); path=runtime/'preview.sock'
        entries=[]; groups=[]
        for i in range(args.count):
            image=root/f'image-{i}.png'; png(image,(40+i%160,100+i%100,200-i%140,255),args.edge)
            entries.append({'id':f'image-{i}','path':str(image),'kind':'image','label':f'Image {i}',
                'project':f'group-{i%16}','settings':{}})
        for i in range(16): groups.append({'id':f'group-{i}','label':f'Project {i}','current':f'image-{i}'})
        registry={'version':1,'entries':entries,'selected':'image-0','layout':'overview','projects':groups,
            'project_grid_size':4,'grid_size':4,'theme':'dark'}
        (state/'previews.json').write_text(json.dumps(registry))
        env=os.environ|{'QT_QPA_PLATFORM':'xcb','QT_SCALE_FACTOR':'1','QT_XCB_NO_XI2':'1',
            'ASSET_PREVIEW_SMALL_CACHE':str(root/'cache'),'ASSET_PREVIEW_TEXTURE_CACHE':str(root/'textures'),
            'ASSET_PREVIEW_TEST_NULL_AUDIO':'1'}
        log=(args.output/'server.log').open('w'); started=time.perf_counter()
        command=[str(args.binary),'--socket',str(path),'--state',str(state)]
        if args.perf:
            command=['perf','record','-e','cycles:u','-F','199','--call-graph','dwarf,8192','--user-callchains','-o',str(args.output/'profile.perf.data'),'--',*command]
        process=subprocess.Popen(command,env=env,stdout=log,stderr=log,start_new_session=True)
        stop=threading.Event(); samples=[]; errors=[]; observer=None; input=None
        try:
            deadline=time.monotonic()+20
            while True:
                try: service_pid=rpc(path,{'method':'ping'},.1)['pid']; break
                except (OSError,RuntimeError):
                    if process.poll() is not None or time.monotonic()>deadline: raise
                    time.sleep(.01)
            report['startup_service_ms']=(time.perf_counter()-started)*1000
            def observe():
                while not stop.is_set():
                    start=time.perf_counter()
                    try:
                        rpc(path,{'method':'ping'},30)
                        samples.append((start,(time.perf_counter()-start)*1000))
                    except (OSError,RuntimeError) as error: errors.append(str(error))
                    stop.wait(.01)
            observer=threading.Thread(target=observe,daemon=True); observer.start()
            def state_now(): return rpc(path,{'method':'list'})
            def wait(condition,timeout=90):
                deadline=time.monotonic()+timeout
                while True:
                    result=state_now()
                    if condition(result): return result
                    if time.monotonic()>deadline: raise RuntimeError('Profile phase did not settle')
                    if memory(service_pid).get('Pss_kib',0)>2*1024*1024: raise RuntimeError('Profiling PSS exceeded 2 GiB')
                    time.sleep(.02)
            def phase(name,action,settled=lambda _:True):
                time.sleep(.1); begin=time.perf_counter(); cpu=ticks(service_pid)
                action(); after=wait(settled); elapsed=time.perf_counter()-begin
                time.sleep(.05)
                latency=[ms for at,ms in samples if at<=begin+elapsed and at+ms/1000>=begin]
                item={'name':name,'completed_ms':elapsed*1000,'ping_samples':len(latency),
                    'ping_p50_ms':percentile(latency,.5),'ping_p95_ms':percentile(latency,.95),
                    'ping_max_ms':max(latency,default=0),'cpu_seconds':(ticks(service_pid)-cpu)/os.sysconf('SC_CLK_TCK'),
                    'memory':memory(service_pid),'active':after['active'],'cache':after['cache'],
                    'preload':after['preload'],'performance':after.get('performance',{})}
                report['phases'].append(item); print(json.dumps({k:v for k,v in item.items() if k not in ('cache','preload')}),flush=True)
                return after
            visible_ready=lambda s:s['presentation']['overview'].get('visible_ready',not s['presentation']['overview']['worker_active'])
            phase('overview-cold',lambda:rpc(path,{'method':'show'}),visible_ready)
            phase('overview-all-open-warm',lambda:None,lambda s:not s['presentation']['overview']['worker_active'])
            input=XInput()
            def hover():
                overview=state_now()['presentation']['overview']; tiles=overview['tiles']
                for i in range(50):
                    tile=tiles[i%len(tiles)]; input.motion(tile['x']+40,tile['y']+40); input.x.XFlush(input.display); time.sleep(.015)
            phase('overview-hover',hover)
            loaded=lambda s:s['active']==16 and all(e.get('metrics',{}).get('loads',0)>0 for e in s['entries'] if e['active'])
            phase('projects-cold-16',lambda:rpc(path,{'method':'layout','layout':'projects'}),loaded)
            def project_hover():
                active=[e for e in state_now()['entries'] if e['active']]
                for i in range(40):
                    r=active[i%len(active)]['viewport']; input.motion(r['x']+r['width']//2,r['y']+r['height']//2); input.x.XFlush(input.display); time.sleep(.025)
            phase('projects-hover',project_hover)
            phase('overview-return',lambda:rpc(path,{'method':'layout','layout':'overview'}),visible_ready)
            phase('projects-warm-16',lambda:rpc(path,{'method':'layout','layout':'projects'}),loaded)
            phase('single-last',lambda:(rpc(path,{'method':'layout','layout':'single'}),rpc(path,{'method':'select','id':f'image-{args.count-1}'})),
                lambda s:s['preload']['state'] in ('cached','skipped') and not s['preload']['worker_active'] and any(e['id']==s['selected'] and e.get('metrics',{}).get('loads',0)>0 and not e['metrics'].get('loading') for e in s['entries']))
            def close_left():
                for i in range(5):
                    selected=state_now()['selected']; rpc(path,{'method':'remove','id':selected})
                    wait(lambda s:not s['preload']['worker_active'] and any(e['id']==s['selected'] and e.get('metrics',{}).get('loads',0)>0 and not e['metrics'].get('loading') for e in s['entries'])); time.sleep(.05)
            phase('close-left-preload',close_left)
            if args.real_model:
                model=args.real_model.resolve(); report['real_model']={'path':str(model),'sha256':hashlib.sha256(model.read_bytes()).hexdigest()}
                rpc(path,{'method':'add','entry':{'id':'real-model','kind':'model','path':str(model),'settings':{'lighting':'lightkit'}}})
                ready=lambda s:any(e['id']=='real-model' and e.get('metrics',{}).get('load_timing',{}).get('first_render_ms') for e in s['entries'])
                s=phase('model-first',lambda:rpc(path,{'method':'select','id':'real-model'}),ready)
                model_row=next(e for e in s['entries'] if e['id']=='real-model'); report['model_metrics']=model_row['metrics']
                phase('model-hide',lambda:rpc(path,{'method':'hide'}),lambda s:s['active']==0)
                phase('model-reopen',lambda:rpc(path,{'method':'show'}),ready)
            report['observer_errors']=errors
        finally:
            stop.set()
            if observer: observer.join(timeout=2)
            if input: input.close()
            try: rpc(path,{'method':'quit'},2)
            except (OSError,RuntimeError): process.kill()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
            log.close()
            report['exit_code']=process.returncode
            if (state/'crash.log').exists(): shutil.copy2(state/'crash.log',args.output/'crash.log')
            report['host_load_after']=Path('/proc/loadavg').read_text().strip()
            (args.output/'performance.json').write_text(json.dumps(report,indent=2))

if __name__=='__main__': main()
