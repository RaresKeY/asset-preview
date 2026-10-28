#!/usr/bin/env python3
"""Matched, private mixed-thumbnail readiness and socket latency under Gamescope.

Alternate the saved baseline and candidate against one immutable model plus
eight images. Each run gets fresh state/cache and owns its exact child process.
Times measure published thumbnails, rather than command acceptance or imports.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from inspection_fixture import write_fixture
from integration import memory,png,ticks
from profile_responsiveness import percentile,rpc

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--candidate',type=Path,default=ROOT/'build/asset-preview-server')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args(); args.output.mkdir(parents=True,exist_ok=True)
    binaries={name:path.resolve() for name,path in (('baseline',args.baseline),('candidate',args.candidate))}
    report={'scope':'Same immutable model and eight 1024px images; model registered first; fresh app state/cache per run. Ping RTT is event-loop latency, not display frame time.',
        'binaries':{name:{'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()} for name,path in binaries.items()},
        'host_load_before':Path('/proc/loadavg').read_text().strip(),'runs':[]}
    with tempfile.TemporaryDirectory(prefix='ap-thumbnail-profile-') as folder:
        root=Path(folder); model=root/'model.glb'; write_fixture(model)
        entries=[{'id':'model','kind':'model','path':str(model),'settings':{}}]
        images=set()
        for i in range(8):
            path=root/f'image-{i}.png'; png(path,(40+i*15,130,210,255),1024); images.add(str(path))
            entries.append({'id':f'image-{i}','kind':'image','path':str(path),'settings':{}})
        # Create all parent directories before the first watcher is installed.
        order=['baseline','candidate','candidate','baseline','baseline','candidate']
        for index in range(len(order)):(root/f'run-{index}').mkdir(mode=0o700)
        for index,name in enumerate(order):
            run=root/f'run-{index}'; state=run/'state'; state.mkdir(mode=0o700); endpoint=run/'preview.sock'
            (state/'previews.json').write_text(json.dumps({'version':1,'entries':entries,'layout':'overview','selected':'model','group_projects':False,'theme':'dark'}))
            env=os.environ|{'QT_QPA_PLATFORM':'xcb','QT_SCALE_FACTOR':'1','ASSET_PREVIEW_SMALL_CACHE':str(run/'cache'),
                'ASSET_PREVIEW_TEXTURE_CACHE':str(run/'textures'),'ASSET_PREVIEW_TEST_NULL_AUDIO':'1'}
            item={'binary':name,'index':index}; samples=[]; errors=[]; stop=threading.Event(); observer=None
            with (args.output/f'{index}-{name}.log').open('w') as log:
                process=subprocess.Popen([str(binaries[name]),'--socket',str(endpoint),'--state',str(state)],env=env,stdout=log,stderr=log,start_new_session=True)
                try:
                    deadline=time.monotonic()+15
                    while not endpoint.exists():
                        if process.poll() is not None or time.monotonic()>deadline:raise RuntimeError('Server did not start')
                        time.sleep(.01)
                    rpc(endpoint,{'method':'ping'})
                    def observe():
                        while not stop.is_set():
                            began=time.perf_counter()
                            try:rpc(endpoint,{'method':'ping'})
                            except Exception as error:errors.append(str(error));return
                            samples.append((time.perf_counter()-began)*1000);stop.wait(.01)
                    observer=threading.Thread(target=observe);observer.start()
                    cpu=ticks(process.pid); began=time.perf_counter(); rpc(endpoint,{'method':'show'})
                    peak=0; rendered=''; seen=set(); model_ready=False
                    deadline=time.monotonic()+40
                    while not model_ready or len(seen)<8:
                        if time.monotonic()>deadline:raise RuntimeError('Thumbnails did not publish')
                        events=rpc(endpoint,{'method':'cache','action':'events','limit':128})['events']
                        for event in events:
                            if event['tier']!='thumbnail' or event['action']!='generated':continue
                            source=event.get('detail',{}).get('source','')
                            if source in images:seen.add(source)
                            if source==str(model):model_ready=True;rendered=event.get('detail',{}).get('renderer',rendered)
                        elapsed=(time.perf_counter()-began)*1000
                        if seen and 'first_image_ms' not in item:item['first_image_ms']=elapsed
                        if len(seen)==8 and 'all_images_ms' not in item:item['all_images_ms']=elapsed
                        if model_ready and 'model_ms' not in item:item['model_ms']=elapsed
                        peak=max(peak,memory(process.pid).get('Pss_kib',0))
                        time.sleep(.01)
                    item['all_thumbnails_ms']=(time.perf_counter()-began)*1000
                    time.sleep(.6)
                    final=rpc(endpoint,{'method':'list'}); overview=final['presentation']['overview']
                    assert not overview['worker_active'],overview
                    assert final['active']==0 and all(not e['building'] and 'metrics' not in e for e in final['entries'])
                    assert rendered and not any(token in rendered.lower() for token in ('unknown','llvmpipe','softpipe','software')),rendered
                    item.update(cpu_seconds=(ticks(process.pid)-cpu)/os.sysconf('SC_CLK_TCK'),peak_parent_pss_kib=peak,cache_bytes=final['cache']['bytes'],renderer=rendered,
                        ping_p50_ms=percentile(samples,.5),ping_p95_ms=percentile(samples,.95),ping_max_ms=max(samples,default=0),observer_errors=errors)
                    print(json.dumps(item),flush=True);report['runs'].append(item)
                finally:
                    stop.set()
                    if observer:observer.join(2)
                    try:rpc(endpoint,{'method':'quit'},2)
                    except (OSError,RuntimeError):process.terminate()
                    try:process.wait(timeout=5)
                    except subprocess.TimeoutExpired:process.kill();process.wait()
        report['host_load_after']=Path('/proc/loadavg').read_text().strip()
        (args.output/'performance.json').write_text(json.dumps(report,indent=2))

if __name__=='__main__':main()
