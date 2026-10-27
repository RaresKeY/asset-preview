#!/usr/bin/env python3
"""Native split resizing/persistence and standard card behavior, on hardware."""
from pathlib import Path
import ctypes,json,os,subprocess,tempfile,time
import integration as h
ROOT=Path(__file__).resolve().parents[1]
OUT=Path(os.environ.get('ASSET_PREVIEW_EVIDENCE_DIR',ROOT/'.local/compact-layout'))
def main():
 assert os.environ.get('QT_QPA_PLATFORM')=='xcb'
 OUT.mkdir(parents=True,exist_ok=True);report={'checks':[],'sizes':[]};input=None
 with tempfile.TemporaryDirectory(prefix='preview-compact-layout-') as folder:
  fixture=Path(folder);os.environ['ASSET_PREVIEW_RUNTIME_DIR']=str(fixture/'runtime');os.environ['ASSET_PREVIEW_STATE_DIR']=str(fixture/'state');os.environ['QT_XCB_NO_XI2']='1'
  os.environ.pop('CODEX_THREAD_ID',None);os.environ.pop('CODEX_SESSION_ID',None)
  try:
   image=fixture/'image.png';h.png(image,(40,170,210,255),512)
   h.client.start();h.client.rpc({'method':'theme','mode':'dark'});h.client.rpc({'method':'add','entry':{'id':'image','path':str(image),'label':'Fixture image'}});h.client.rpc({'method':'show'})
   h.eventually(lambda:h.row('image').get('metrics',{}).get('loads'));input=h.XInput()
   input.x.XResizeWindow.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_uint,ctypes.c_uint]
   view=h.row('image')['viewport'];input.scroll(view['x']+view['width']//2,view['y']+view['height']//2);time.sleep(.2)
   saved_view=h.row('image')['metrics']['image_view'];loads=h.row('image')['metrics']['loads']
   def state():return h.client.rpc({'method':'list'})['presentation']
   def capture(name):
    path=OUT/(name+'.png');path.unlink(missing_ok=True);h.client.rpc({'method':'capture','path':str(path)});return str(path)
   def check(name,condition):
    assert condition,(name,state());report['checks'].append(name);print('PASS',name,flush=True)
   h.client.rpc({'method':'codex','action':'open'})
   for width,height in ((1000,720),(1920,1022),(700,500),(1400,900)):
    input.x.XResizeWindow(input.display,input.window,width,height);input.x.XFlush(input.display);time.sleep(.3)
    p=state();fraction=(p['codex']['width']+8)/p['width']
    check('default split maintained at '+str(width),abs(fraction-7/16)<.025)
    check('view retained at '+str(width),h.row('image')['metrics']['image_view']==saved_view and h.row('image')['metrics']['loads']==loads)
    report['sizes'].append({'width':p['width'],'height':p['height'],'codex_fraction':fraction,'capture':capture('compact-'+str(width))})
   card=h.row('image')['overlays'];check('original title row height',card['name']['height']==34)
   check('standard controls remain reachable',card['controls']['visible'] and card['controls']['width']>=100)
   # Native splitter drag changes the fraction; window resizing preserves it.
   p=state();x=p['codex']['x']-7;y=p['codex']['y']+80
   input.motion(x,y);input.xt.XTestFakeButtonEvent(input.display,1,1,0);input.x.XFlush(input.display);time.sleep(.15)
   input.motion(x-140,y);input.x.XFlush(input.display);time.sleep(.2)
   input.xt.XTestFakeButtonEvent(input.display,1,0,0);input.x.XFlush(input.display);time.sleep(.2)
   preferred=state()['codex_fraction'];check('splitter drag updates preference',preferred>.50)
   input.x.XResizeWindow(input.display,input.window,1100,800);input.x.XFlush(input.display);time.sleep(.3)
   check('dragged ratio survives window resize',abs((state()['codex']['width']+8)/state()['width']-preferred)<.025)
   h.client.rpc({'method':'codex','action':'collapse'});h.client.rpc({'method':'codex','action':'open'});time.sleep(.2)
   check('collapse reuses split preference',abs((state()['codex']['width']+8)/state()['width']-preferred)<.025)
   h.client.rpc({'method':'quit'});h.eventually(lambda:not h.client.running());h.client.start();h.client.rpc({'method':'show'});h.client.rpc({'method':'codex','action':'open'});time.sleep(.2)
   check('split preference persists across service restart',abs(state()['codex_fraction']-preferred)<.0001)
   # The compact 3D card retains a visible status/count and reset shortcut.
   model=fixture/'cube.obj';subprocess.run(['python3',str(ROOT/'examples/generate_cube.py'),str(model)],check=True,capture_output=True)
   h.client.rpc({'method':'add','entry':{'id':'cube','path':str(model)}});h.client.rpc({'method':'select','id':'cube'});h.eventually(lambda:h.row('cube').get('metrics',{}).get('engine'),timeout=45)
   renderer=h.row('cube')['metrics']['renderer'];check('hardware renderer',not any(t in renderer.lower() for t in ('llvmpipe','softpipe','software','unknown')));report['renderer']=renderer
   check('model status and triangle count remain visible',h.row('cube')['status_visible'] and h.row('cube')['overlays']['triangles']['visible'])
   capture('compact-model');h.client.rpc({'method':'hide'});check('hidden resources still unload',h.client.rpc({'method':'list'})['active']==0)
  finally:
   if input:input.close()
   if h.client.running():h.client.rpc({'method':'quit'});h.eventually(lambda:not h.client.running())
   (OUT/'layout-verification.json').write_text(json.dumps(report,indent=2))
if __name__=='__main__':main()
