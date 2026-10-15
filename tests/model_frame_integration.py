"""Run tools/offscreen.sh python3 tests/model_frame_integration.py.
ASSET_PREVIEW_FRAME_MODEL must name a Y-up/-Z-front GLB; optional
ASSET_PREVIEW_FRAME_PROOF retains actual renderer/capture evidence externally.
Uses an isolated daemon and never the normal service state.
"""
import importlib.machinery,importlib.util,os,json,time
from pathlib import Path
root=Path(__file__).resolve().parents[1]
loader=importlib.machinery.SourceFileLoader('client',str(root/'bin/asset-preview'));spec=importlib.util.spec_from_loader(loader.name,loader);c=importlib.util.module_from_spec(spec);loader.exec_module(c)
import tempfile
fixture=tempfile.TemporaryDirectory(prefix='preview-frame-')
out=Path(os.environ.get('ASSET_PREVIEW_FRAME_PROOF',fixture.name));out.mkdir(exist_ok=True)
os.environ['ASSET_PREVIEW_RUNTIME_DIR']=str(Path(fixture.name)/'runtime');os.environ['ASSET_PREVIEW_STATE_DIR']=str(Path(fixture.name)/'state')
def rpc(**r):return c.rpc(r)
def row():return rpc(method='list')['entries'][0]
def wait():
 for _ in range(200):
  r=row()
  if r.get('metrics',{}).get('engine'):return r
  time.sleep(.05)
 raise AssertionError(row())
def camera():return row()['metrics']['camera']
def sign(expected):
 a=camera(); d=[p-f for p,f in zip(a['position'],a['focal'])]
 assert all(abs(v)<1e-5 if e==0 else v*e>0 for v,e in zip(d,expected)),d
 return d
try:
 c.start()
 path=os.environ['ASSET_PREVIEW_FRAME_MODEL']
 entry={'id':'ship','path':path,'kind':'model','settings':{'up_axis':'y','front_axis':'-z','materials':True,'textures':True,'grid':False}}
 rpc(method='add',entry=entry);rpc(method='show');r=wait();sign([1,1,-1]);assert 'llvmpipe' not in r['metrics']['renderer'].lower()
 capture=out/f'default-front-materials-{time.time_ns()}.png'
 rpc(method='capture',path=str(capture))
 for view,expected in [('front',[0,0,-1]),('back',[0,0,1]),('right',[1,0,0]),('top',[0,1,0])]:
  rpc(method='camera_view',id='ship',view=view);sign(expected)
 rpc(method='camera_view',id='ship',view='back');before=camera()
 rpc(method='settings',id='ship',settings={'materials':False});assert camera()==before
 rpc(method='settings',id='ship',settings={'materials':True});assert camera()==before
 rpc(method='add',entry=entry);wait();assert camera()==before
 rpc(method='settings',id='ship',settings={'front_axis':'+z'});sign([-1,1,1])
 rpc(method='hide');rpc(method='settings',id='ship',settings={'front_axis':'-z'});assert 'camera' not in row()
 rpc(method='show');wait();sign([1,1,-1])
 for bad in ['z','+y','garbage']:
  try:rpc(method='settings',id='ship',settings={'front_axis':bad})
  except RuntimeError:pass
  else:raise AssertionError(bad)
 (out/'result.json').write_text(json.dumps({'passed':True,'renderer':row()['metrics']['renderer'],'entry':row(),'checks':['signed cardinal frame','material camera preservation','same-frame reconnect preservation','frame-change reset visible and hidden','validation'],'capture':str(capture)},indent=2))
 print('PASS',row()['metrics']['renderer'])
finally:
 if c.running():rpc(method='quit')
