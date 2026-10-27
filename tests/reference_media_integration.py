#!/usr/bin/env python3
"""Hardware captures of frozen image/video/model references and annotation tools."""
from pathlib import Path
import json,os,subprocess,tempfile,time,struct
import integration as h
ROOT=Path(__file__).resolve().parents[1]
OUT=Path(os.environ.get('ASSET_PREVIEW_EVIDENCE_DIR',Path.home()/'Pictures/Asset Preview/asset-preview-codex-20261021'))
def dimensions(path):return struct.unpack('!II',path.read_bytes()[16:24])
def main():
 assert os.environ.get('QT_QPA_PLATFORM')=='xcb'
 OUT.mkdir(parents=True,exist_ok=True);report={};input=None
 with tempfile.TemporaryDirectory(prefix='preview-reference-media-') as folder:
  fixture=Path(folder);os.environ['ASSET_PREVIEW_RUNTIME_DIR']=str(fixture/'runtime');os.environ['ASSET_PREVIEW_STATE_DIR']=str(fixture/'state');os.environ['QT_XCB_NO_XI2']='1';os.environ['ASSET_PREVIEW_TEST_NULL_AUDIO']='1'
  os.environ.pop('CODEX_THREAD_ID',None);os.environ.pop('CODEX_SESSION_ID',None)
  try:
   h.client.start();h.client.rpc({'method':'theme','mode':'dark'})
   image=fixture/'image.png';h.png(image,(40,180,90,255),128)
   video=fixture/'video.mp4';subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=320x180:rate=12','-t','3','-pix_fmt','yuv420p',str(video)],check=True)
   model=fixture/'cube.obj';subprocess.run(['python3',str(ROOT/'examples/generate_cube.py'),str(model)],check=True,capture_output=True)
   for key,path in [('image',image),('video',video),('model',model)]:h.client.rpc({'method':'add','entry':{'id':key,'path':str(path),'settings':{'paused':True} if key=='video' else {}}})
   h.client.rpc({'method':'show'});input=h.XInput()
   def ui():return h.client.rpc({'method':'list'})['presentation']['reference_controls']
   def click(name):
    box=ui()['editor'][name];input.click(box['x']+box['width']//2,box['y']+box['height']//2);time.sleep(.15)
   def capture(name):
    path=OUT/(name+'.png');path.unlink(missing_ok=True);h.client.rpc({'method':'capture','path':str(path)});return path
   for key in ('image','video','model'):
    h.client.rpc({'method':'select','id':key});h.eventually(lambda:h.row(key).get('metrics',{}).get('loads'),timeout=45)
    if key=='video':
     h.client.rpc({'method':'seek','id':key,'seconds':1.25});h.eventually(lambda:abs(h.row(key)['metrics']['position']-1.25)<.2)
    view=h.row(key)['viewport']
    if key in ('image','model'):input.drag(view['x']+view['width']//2,view['y']+view['height']//2,55,30);time.sleep(.2)
    if key=='image':input.scroll(view['x']+100,view['y']+100);time.sleep(.2)
    before=h.row(key)['metrics'];original=capture('media-'+key+'-view')
    input.chord('Control_L','Shift_L','a');h.eventually(lambda:'editor' in ui())
    # Unmarked save must preserve exact viewport dimensions and frame pixels.
    previous=set((Path.home()/'Pictures/Asset Preview').glob('Preview-'+key+'-*.png'));click('Save PNG')
    saved=list(h.eventually(lambda:set((Path.home()/'Pictures/Asset Preview').glob('Preview-'+key+'-*.png'))-previous));assert len(saved)==1
    assert dimensions(saved[0])==(view['width'],view['height']),(key,dimensions(saved[0]),view)
    points=[(view['width']//2,view['height']//2),(view['width']//3,view['height']//2)]
    actual=h.png_pixels(saved[0],points);expected=h.png_pixels(original,[(x+view['x'],y+view['y']) for x,y in points])
    assert actual==expected,(key,actual,expected)
    # Text, freehand and Undo are exercised using real input.
    if key=='model':
     click('Text');box=ui()['editor']['referenceCanvas'];input.click(box['x']+box['width']//2,box['y']+box['height']//2);time.sleep(.2)
     for char in 'detail':input.press(char)
     input.press('Return');time.sleep(.2);click('Pen');input.drag(box['x']+box['width']//3,box['y']+box['height']//3,80,45);click('Undo')
     click('Save PNG');after=list(set((Path.home()/'Pictures/Asset Preview').glob('Preview-model-*.png'))-previous-set(saved));assert len(after)==1
     report['text_annotation']=str(after[0])
    click('Close');h.eventually(lambda:'editor' not in ui())
    current=h.row(key)['metrics'];check={'capture':str(saved[0]),'dimensions':dimensions(saved[0]),'pixel_match':True}
    if key=='video':
     assert current['paused'] and abs(current['position']-before['position'])<.05;check['paused_position']=current['position']
    if key=='model':
     renderer=current['renderer'];assert not any(t in renderer.lower() for t in ('llvmpipe','softpipe','software','unknown'));check['renderer']=renderer
    report[key]=check;print('PASS frozen',key,'reference',flush=True)
   h.client.rpc({'method':'hide'});assert h.client.rpc({'method':'list'})['active']==0
  finally:
   if input:input.close()
   if h.client.running():h.client.rpc({'method':'quit'});h.eventually(lambda:not h.client.running())
   (OUT/'media-verification.json').write_text(json.dumps(report,indent=2))
if __name__=='__main__':main()
