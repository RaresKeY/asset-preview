#!/usr/bin/env python3
"""Real image/model loading: responsive socket, cancellation and loading feedback."""
import json
import os
from pathlib import Path
import threading
import sys
import time
import unittest

import integration as f
from animation_fixture import write_animation

class Responsiveness(unittest.TestCase):
    setUpClass=classmethod(f.PreviewIntegration.setUpClass.__func__)
    tearDownClass=classmethod(f.PreviewIntegration.tearDownClass.__func__)
    setUp=f.PreviewIntegration.setUp

    def measure(self,action,done):
        samples=[];stop=threading.Event();errors=[]
        def observe():
            while not stop.is_set():
                began=time.perf_counter()
                try:f.client.rpc({'method':'ping'})
                except Exception as error:errors.append(str(error));return
                samples.append(time.perf_counter()-began);stop.wait(.01)
        thread=threading.Thread(target=observe);thread.start();time.sleep(.025)
        began=time.perf_counter()
        try:
            action();f.eventually(done,timeout=30)
        finally:stop.set();thread.join(5)
        self.assertFalse(errors,errors);self.assertGreater(len(samples),2)
        self.assertLess(max(samples),.5,'Loading monopolized the UI thread for over 500 ms')
        return {'completed_ms':(time.perf_counter()-began)*1000,'ping_max_ms':max(samples)*1000,'samples':len(samples)}

    def test_many_project_images_publish_cancel_and_stay_responsive(self):
        for i in range(16):
            image=self.root/f'large-{i}.png';f.png(image,(30+i,90,170,255),2048)
            f.client.rpc({'method':'add','entry':{'id':f'image-{i}','path':str(image),'project':f'group-{i}'}})
        # Qt offscreen's 800-pixel screen fits only three project columns. The
        # native lane exercises 16 project panes; the portable lane admits the
        # same 16 image workers in a regular grid without requiring off-page views.
        layout={'layout':'projects','project_grid_size':4} if f.GPU else {'layout':'grid','grid_size':4}
        f.client.rpc({'method':'layout',**layout})
        timing=self.measure(lambda:f.client.rpc({'method':'show'}),lambda:f.client.rpc({"method":"list"})["active"]==16 and all(
            e.get('metrics',{}).get('loads')==1 and not e['metrics'].get('loading')
            for e in f.client.rpc({'method':'list'})['entries'] if e['active']))
        state=f.client.rpc({'method':'list'});self.assertEqual(state['active'],16)
        self.assertTrue(all('decode_ms' in e['metrics']['load_timing'] for e in state['entries']))
        # Unload during a fresh generation: no completion may resurrect cards.
        for i in range(16):f.client.rpc({'method':'reload','id':f'image-{i}'})
        f.client.rpc({'method':'hide'});time.sleep(.25)
        state=f.client.rpc({'method':'list'});self.assertEqual(state['active'],0)
        self.assertFalse(any('metrics' in e for e in state['entries']))
        self.assertEqual(state['cache']['bytes'],0)
        self.record('images',timing)

    def test_generator_failure_survives_queued_decode_and_retries(self):
        image=self.root/'invalid.png';image.write_bytes(b'incomplete image')
        f.client.rpc({'method':'add','entry':{'id':'failed','kind':'image','path':str(image),
            'cwd':str(self.root),'command':[sys.executable,'-c','raise SystemExit(9)']}})
        f.client.rpc({'method':'show'})
        f.eventually(lambda:f.row('failed')['status'].startswith('Generator failed'))
        time.sleep(.7)
        state=f.row('failed');self.assertTrue(state['status'].startswith('Generator failed'),state['status'])
        self.assertFalse(state['overlays']['status']['loading']);self.assertEqual(state['builds'],1)

    @unittest.skipUnless(f.GPU,'Real native GPU model worker')
    def test_model_loading_spinner_navigation_and_fresh_frame_step(self):
        model=self.root/'worker.glb';write_animation(model)
        image=self.root/'return.png';f.png(image)
        f.client.rpc({'method':'add','entry':{'id':'model','path':str(model),'settings':{'paused':True}}})
        f.client.rpc({'method':'add','entry':{'id':'image','path':str(image)}})
        f.client.rpc({'method':'select','id':'model'})
        f.client.rpc({'method':'show'})
        loading=f.eventually(lambda:f.row('model') if f.row('model').get('metrics',{}).get('loading') else None)
        self.assertTrue(loading['overlays']['status']['loading'])
        target=Path(os.environ.get('ASSET_PREVIEW_EVIDENCE_DIR',self.root))
        target.mkdir(parents=True,exist_ok=True)
        f.client.rpc({'method':'capture','path':str(target/'model-loading.png')})
        # Navigate immediately during native import; a stale candidate cannot
        # publish into a replacement or keep the old view active.
        self.measure(lambda:f.client.rpc({'method':'select','id':'image'}),lambda:f.row('image').get('metrics',{}).get('loads')==1)
        self.assertFalse(f.row('model')['active']);self.assertNotIn('metrics',f.row('model'))
        timing=self.measure(lambda:f.client.rpc({'method':'select','id':'model'}),lambda:
            f.row('model').get('metrics',{}).get('loads')==1 and not f.row('model')['metrics'].get('command_pending'))
        renderer=f.row('model')['metrics']['renderer'].lower()
        self.assertFalse(any(name in renderer for name in ('unknown','llvmpipe','softpipe')),renderer)
        for direction in (0,2,True,'1'):
            with self.assertRaises(RuntimeError):f.client.rpc({'method':'animation_step','id':'model','direction':direction})
        f.client.rpc({'method':'animation_step','id':'model','direction':1})
        f.eventually(lambda:not f.row('model')['metrics']['command_pending'])
        self.assertAlmostEqual(f.row('model')['metrics']['position'],1/12)
        self.assertTrue(f.row('model')['settings']['paused'])
        self.assertFalse(f.row('model')['overlays']['status']['loading'])
        time.sleep(.3);renders=f.row('model')['metrics']['renders'];cpu=f.ticks(self.pid);time.sleep(.4)
        self.assertEqual(f.row('model')['metrics']['renders'],renders)
        self.assertLessEqual(f.ticks(self.pid)-cpu,3)
        self.record('models',timing)

    @unittest.skipUnless(f.GPU,'Native input must overtake queued imports')
    def test_loaded_view_input_precedes_remaining_imports(self):
        model=self.root/'interactive.glb';write_animation(model)
        f.client.rpc({'method':'add','entry':{'id':'interactive','path':str(model)}})
        f.client.rpc({'method':'show'})
        f.eventually(lambda:f.row('interactive').get('metrics',{}).get('loads')==1,timeout=25)
        texture=self.root/'large-material.png';f.png(texture,(90,140,200,255),4096)
        ids=[f'material-{i}' for i in range(3)]
        for key in ids:
            f.client.rpc({'method':'add','entry':{'id':key,'kind':'material','path':str(texture),
                'settings':{'shape':'cube','lighting':'lightkit'}}})
        f.client.rpc({'method':'layout','layout':'grid','grid_size':2})
        f.eventually(lambda:all(f.row(key)['active'] for key in ids) and any(
            f.row(key).get('metrics',{}).get('loading') for key in ids),timeout=25)
        before=f.row('interactive')['metrics']['camera']
        timing=self.measure(lambda:f.client.rpc({'method':'camera_view','id':'interactive','view':'front'}),lambda:
            f.row('interactive')['metrics'].get('camera')!=before and not f.row('interactive')['metrics'].get('command_pending'))
        self.assertFalse(all(f.row(key).get('metrics',{}).get('loads',0)>0 for key in ids),
            'Input waited behind every queued import')
        f.eventually(lambda:all(f.row(key).get('metrics',{}).get('loads',0)>0 for key in ids),timeout=30)
        self.record('input-priority',timing)

    def record(self,name,value):
        if os.environ.get('ASSET_PREVIEW_EVIDENCE_DIR'):
            p=Path(os.environ['ASSET_PREVIEW_EVIDENCE_DIR']);p.mkdir(parents=True,exist_ok=True)
            (p/f'responsiveness-{name}.json').write_text(json.dumps(value,indent=2))

if __name__=='__main__':unittest.main(verbosity=2)
