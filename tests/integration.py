#!/usr/bin/env python3
"""Real daemon/GUI tests. Run under Gamescope headless for the GPU lane."""
from __future__ import annotations
import concurrent.futures
import ctypes
import importlib.machinery
import importlib.util
import json
import math
import os
import shlex
import shutil
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zlib
import wave

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("preview_client", str(ROOT / "bin/asset-preview"))
spec = importlib.util.spec_from_loader(loader.name, loader)
client = importlib.util.module_from_spec(spec); loader.exec_module(client)
GPU = os.environ.get("ASSET_PREVIEW_GPU_TEST") == "1"

def png(path: Path, color=(220, 70, 80, 255), size=64) -> None:
    def chunk(kind, data): return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    pixels = b"".join(b"\0" + bytes(color) * size for _ in range(size))
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6 if len(color)==4 else 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    candidate = path.with_suffix(".candidate"); candidate.write_bytes(data); candidate.replace(path)

def video(path: Path, source="testsrc2=size=320x180:rate=12", duration=4):
    candidate=path.with_name(path.stem+".candidate.mp4")
    subprocess.run(["ffmpeg","-nostdin","-loglevel","error","-y","-f","lavfi","-i",source,
                    "-f","lavfi","-i","sine=frequency=440:sample_rate=48000","-t",str(duration),
                    "-c:v","libx264","-preset","ultrafast","-pix_fmt","yuv420p","-c:a","aac",
                    "-threads","2","-shortest",str(candidate)],check=True,capture_output=True)
    candidate.replace(path)

def audio(path: Path, duration=4, frequency=440):
    """Finite stereo PCM fixture, atomically published without audio tools."""
    candidate=path.with_name(path.stem+".candidate.wav")
    rate=48000
    samples=b"".join(struct.pack("<hh",*[int(8000*math.sin(2*math.pi*frequency*i/rate))]*2) for i in range(int(rate*duration)))
    with wave.open(str(candidate),"wb") as output:
        output.setparams((2,2,rate,0,"NONE","not compressed")); output.writeframes(samples)
    candidate.replace(path)

def eventually(fn, timeout=8):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = fn()
        if last: return last
        time.sleep(.05)
    raise AssertionError(f"Condition did not become true in {timeout}s (last: {last})")

def row(key):
    return next(r for r in client.rpc({"method":"list"})["entries"] if r["id"] == key)

def ticks(pid):
    # Linux stat field 14+15, after safely skipping the parenthesized comm.
    words = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return int(words[11]) + int(words[12])

def memory(pid):
    fields=Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()
    return {line.split(":")[0]+"_kib":int(line.split()[1]) for line in fields if line.startswith(("Rss:","Pss:"))}

def horizon_roll(camera, vertical=1):
    # VTK stores an orthogonalized view-up vector. The camera's right vector
    # must have no component along world up for a level horizon.
    d=[p-f for p,f in zip(camera["position"],camera["focal"])]; up=camera["up"]
    right=[d[1]*up[2]-d[2]*up[1],d[2]*up[0]-d[0]*up[2],d[0]*up[1]-d[1]*up[0]]
    return right[vertical]/math.sqrt(sum(v*v for v in right))

def png_pixels(path, coordinates):
    """Read pixels from a Qt RGB/RGBA screenshot in one decode, without Pillow."""
    wanted={}
    for x,y in coordinates: wanted.setdefault(y,[]).append(x)
    pixels={}
    data=Path(path).read_bytes(); offset=8; parts=[]
    while offset<len(data):
        length=struct.unpack_from(">I",data,offset)[0]; kind=data[offset+4:offset+8]; part=data[offset+8:offset+8+length]; offset+=length+12
        if kind==b"IHDR": width,height,depth,color,*_=struct.unpack(">IIBBBBB",part)
        if kind==b"IDAT": parts.append(part)
    if depth!=8 or color not in (2,6): raise AssertionError("Unexpected screenshot format")
    channels=4 if color==6 else 3; stride=width*channels; decoded=zlib.decompress(b"".join(parts)); previous=bytearray(stride)
    def paeth(a,b,c):
        p=a+b-c; distances=(abs(p-a),abs(p-b),abs(p-c))
        return (a,b,c)[distances.index(min(distances))]
    for line in range(max(wanted)+1):
        start=line*(stride+1); filter_type=decoded[start]; row=bytearray(decoded[start+1:start+1+stride])
        for i in range(stride):
            left=row[i-channels] if i>=channels else 0; up=previous[i]; corner=previous[i-channels] if i>=channels else 0
            predictor=(0,left,up,(left+up)//2,paeth(left,up,corner))[filter_type]
            row[i]=(row[i]+predictor)&255
        previous=row
        for x in wanted.get(line,[]): pixels[x,line]=tuple(row[x*channels:x*channels+3])
    return [pixels[x,y] for x,y in coordinates]

def png_pixel(path, x, y):
    return png_pixels(path,[(x,y)])[0]

class XInput:
    """Input exclusively in this test's Gamescope X server, using installed XTest."""
    def __init__(self):
        self.x=ctypes.CDLL("libX11.so.6"); self.xt=ctypes.CDLL("libXtst.so.6")
        pointer=ctypes.c_void_p; ulong=ctypes.c_ulong; uint=ctypes.c_uint; integer=ctypes.c_int
        signatures={"XOpenDisplay":([ctypes.c_char_p],pointer),"XDefaultRootWindow":([pointer],ulong),
                    "XQueryTree":([pointer,ulong,ctypes.POINTER(ulong),ctypes.POINTER(ulong),ctypes.POINTER(ctypes.POINTER(ulong)),ctypes.POINTER(uint)],integer),
                    "XFetchName":([pointer,ulong,ctypes.POINTER(ctypes.c_char_p)],integer),"XFree":([pointer],integer),
                    "XSetInputFocus":([pointer,ulong,integer,ulong],integer),"XFlush":([pointer],integer),
                    "XStringToKeysym":([ctypes.c_char_p],ulong),"XKeysymToKeycode":([pointer,ulong],uint),
                    "XTranslateCoordinates":([pointer,ulong,ulong,integer,integer,ctypes.POINTER(integer),ctypes.POINTER(integer),ctypes.POINTER(ulong)],integer),
                    "XCloseDisplay":([pointer],integer),"XSync":([pointer,integer],integer),
                    "XMoveWindow":([pointer,ulong,integer,integer],integer)}
        for key,(args,result) in signatures.items(): getattr(self.x,key).argtypes=args; getattr(self.x,key).restype=result
        self.xt.XTestFakeKeyEvent.argtypes=[pointer,uint,integer,ulong]
        self.xt.XTestFakeButtonEvent.argtypes=[pointer,uint,integer,ulong]
        self.xt.XTestFakeMotionEvent.argtypes=[pointer,integer,integer,integer,ulong]
        self.display=self.x.XOpenDisplay(None)
        if not self.display: raise RuntimeError("Cannot open test display")
        self.root=self.x.XDefaultRootWindow(self.display)
        self.window=self.find(self.root)
        if not self.window: raise RuntimeError("Cannot find the Asset Preview window")
        # Gamescope's XTest transform can apply a restored client offset twice.
        # Normalize this private fixture window before injecting tiny controls.
        self.x.XMoveWindow(self.display,self.window,0,0); self.x.XSync(self.display,0)
        self.x.XSetInputFocus(self.display,self.window,2,0); self.x.XFlush(self.display)
    def find(self,parent,depth=0):
        class Attributes(ctypes.Structure):
            _fields_=[("x",ctypes.c_int),("y",ctypes.c_int),("width",ctypes.c_int),("height",ctypes.c_int),("border",ctypes.c_int),("depth",ctypes.c_int),("visual",ctypes.c_void_p),("root",ctypes.c_ulong),("class_",ctypes.c_int),("bit_gravity",ctypes.c_int),("win_gravity",ctypes.c_int),("backing_store",ctypes.c_int),("backing_planes",ctypes.c_ulong),("backing_pixel",ctypes.c_ulong),("save_under",ctypes.c_int),("colormap",ctypes.c_ulong),("map_installed",ctypes.c_int),("map_state",ctypes.c_int),("all_events",ctypes.c_long),("your_events",ctypes.c_long),("do_not_propagate",ctypes.c_long),("override_redirect",ctypes.c_int),("screen",ctypes.c_void_p)]
        self.x.XGetWindowAttributes.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.POINTER(Attributes)]
        name=ctypes.c_char_p()
        if self.x.XFetchName(self.display,parent,ctypes.byref(name)) and name.value:
            text=name.value.decode(errors="replace"); self.x.XFree(name)
            if text=="Asset Preview":
                attributes=Attributes(); self.x.XGetWindowAttributes(self.display,parent,ctypes.byref(attributes))
                if attributes.map_state==2: return parent
        if depth>3: return None
        root=ctypes.c_ulong(); above=ctypes.c_ulong(); children=ctypes.POINTER(ctypes.c_ulong)(); count=ctypes.c_uint()
        self.x.XQueryTree(self.display,parent,ctypes.byref(root),ctypes.byref(above),ctypes.byref(children),ctypes.byref(count))
        ids=[children[i] for i in range(count.value)]
        if children: self.x.XFree(children)
        for child in ids:
            found=self.find(child,depth+1)
            if found: return found
        return None
    def key(self,name,pressed):
        code=self.x.XKeysymToKeycode(self.display,self.x.XStringToKeysym(name.encode()))
        self.xt.XTestFakeKeyEvent(self.display,code,pressed,0)
    def shortcut(self,key):
        self.key("Alt_L",1); self.key(key,1); self.key(key,0); self.key("Alt_L",0); self.x.XFlush(self.display)
    def press(self,key):
        self.key(key,1); self.key(key,0); self.x.XFlush(self.display)
    def chord(self,*keys):
        for key in keys: self.key(key,1)
        for key in reversed(keys): self.key(key,0)
        self.x.XFlush(self.display)
    def motion(self,x,y):
        ox=ctypes.c_int(); oy=ctypes.c_int(); child=ctypes.c_ulong()
        self.x.XTranslateCoordinates(self.display,self.window,self.root,0,0,ctypes.byref(ox),ctypes.byref(oy),ctypes.byref(child))
        self.xt.XTestFakeMotionEvent(self.display,-1,ox.value+x,oy.value+y,0)
    def click(self,x,y):
        self.motion(x,y)
        self.xt.XTestFakeButtonEvent(self.display,1,1,0)
        self.xt.XTestFakeButtonEvent(self.display,1,0,0); self.x.XFlush(self.display)
    def scroll(self,x,y,up=True):
        self.motion(x,y)
        button=4 if up else 5
        self.xt.XTestFakeButtonEvent(self.display,button,1,0)
        self.xt.XTestFakeButtonEvent(self.display,button,0,0); self.x.XFlush(self.display)
    def drag(self,x,y,dx=70,dy=25):
        self.motion(x,y)
        self.xt.XTestFakeButtonEvent(self.display,1,1,0)
        self.motion(x+dx,y+dy)
        self.xt.XTestFakeButtonEvent(self.display,1,0,0); self.x.XFlush(self.display)
    def close(self): self.x.XCloseDisplay(self.display)

class PreviewIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="asset-preview-tests-")
        cls.root = Path(cls.temp.name)
        os.environ["ASSET_PREVIEW_RUNTIME_DIR"] = str(cls.root / "runtime")
        os.environ["ASSET_PREVIEW_STATE_DIR"] = str(cls.root / "state")
        os.environ["ASSET_PREVIEW_SMALL_CACHE"] = str(cls.root / "small-cache")
        os.environ["ASSET_PREVIEW_TEXTURE_CACHE"] = str(cls.root / "texture-cache")
        # XTest's legacy wheel buttons are dropped by this Qt/XInput2 path.
        # Use native X11 core input in the isolated fixture; production keeps XI2.
        if GPU: os.environ["QT_XCB_NO_XI2"]="1"
        if not GPU: os.environ["QT_QPA_PLATFORM"] = "offscreen"
        os.environ["ASSET_PREVIEW_TEST_NULL_AUDIO"]="1"
        # Several real CLI processes racing to start must all use one daemon.
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            results = list(pool.map(lambda _: subprocess.run([str(ROOT/"bin/asset-preview"),"start"],capture_output=True,text=True),range(6)))
        for result in results:
            if result.returncode: raise AssertionError(result.stderr)
        cls.pid = client.rpc({"method":"ping"})["pid"]

    @classmethod
    def tearDownClass(cls):
        try: client.rpc({"method":"quit"})
        except Exception: pass
        try: eventually(lambda: not client.running(), timeout=4)
        except AssertionError: pass
        if os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR"):
            target = Path(os.environ["ASSET_PREVIEW_EVIDENCE_DIR"])
            target.mkdir(parents=True,exist_ok=True)
            for name in ("crash.log","crash.log.1"):
                log=cls.root/"state"/name
                if log.exists(): (target/name).write_bytes(log.read_bytes())
        cls.temp.cleanup()

    def setUp(self):
        client.rpc({"method":"hide"})
        for r in client.rpc({"method":"list"})["entries"]: client.rpc({"method":"remove","id":r["id"]})
        client.rpc({"method":"layout","layout":"single","grid_size":2})
        # Remembered defaults belong to the application, so isolate each test
        # through its own stopped fixture registry rather than a production reset API.
        if client.rpc({"method":"list"})["preferences"]:
            client.rpc({"method":"quit"}); eventually(lambda:not client.running())
            registry=self.root/"state/previews.json"; state=json.loads(registry.read_text())
            state["preferences"]={}; registry.write_text(json.dumps(state))
            client.start(); self.pid=client.rpc({"method":"ping"})["pid"]; type(self).pid=self.pid

    def add(self, key="image", **extra):
        asset=self.root/f"{key}.png"; png(asset)
        entry={"id":key,"path":str(asset),**extra}
        client.rpc({"method":"add","entry":entry})
        return asset

    def show(self): client.rpc({"method":"show"})

    def test_open_files_reuses_configuration_selects_and_shows(self):
        first=self.add('open-existing', settings={'nearest':True})
        second=self.root/'open second.png'; png(second)
        config=row('open-existing')
        result=subprocess.run([str(ROOT/'bin/asset-preview'),'open',str(first),str(second)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        state=client.rpc({'method':'list'})
        self.assertTrue(state['window_visible'])
        self.assertEqual(len(state['entries']),2)
        selected=state['selected']; self.assertEqual(row(selected)['path'],str(second))
        self.assertTrue(row('open-existing')['settings']['nearest'])
        client.rpc({'method':'hide'})
        result=subprocess.run([str(ROOT/'bin/asset-preview'),str(second),str(first)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(client.rpc({'method':'list'})['selected'],'open-existing')
        self.assertEqual(len(client.rpc({'method':'list'})['entries']),2)
        self.assertEqual(row('open-existing')['watch'],config['watch'])
        self.assertTrue(row('open-existing')['settings']['nearest'])
        eventually(lambda:row('open-existing').get('metrics',{}).get('loads',0)>0)

    def test_open_validates_batch_and_preserves_generators(self):
        asset=self.root/'generated-open.png'; png(asset)
        entry={'id':'open-generator','path':str(asset),'cwd':str(self.root),
               'watch':[str(self.root/'source.txt')],'command':[sys.executable,'-c','pass'],
               'settings':{'nearest':True}}
        client.rpc({'method':'add','entry':entry})
        other=self.add('open-other')
        client.rpc({'method':'select','id':'open-other'})
        result=client.rpc({'method':'open','paths':[str(asset),str(asset)]})
        self.assertEqual(result['ids'],['open-generator','open-generator'])
        for key in ('watch','cwd','command'):
            self.assertEqual(row('open-generator')[key],entry[key])
        self.assertTrue(row('open-generator')['settings']['nearest'])
        self.assertEqual(len(client.rpc({'method':'list'})['entries']),2)
        client.rpc({'method':'hide'})
        registry=self.root/'state/previews.json'; before=registry.read_bytes()
        for paths in [str(other), ['relative.png'], [42], [str(self.root/'new.png'),str(self.root)], [str(other)]*257]:
            with self.assertRaises(RuntimeError): client.rpc({'method':'open','paths':paths})
            self.assertEqual(registry.read_bytes(),before)
            self.assertFalse(client.rpc({'method':'list'})['window_visible'])

    @unittest.skipUnless(GPU, 'Native inspection mode switching needs Gamescope X11')
    def test_open_3d_switches_preview_parts_and_cuts(self):
        from inspection_fixture import write_fixture
        model=self.root/'open-modes.glb'; write_fixture(model)
        client.rpc({'method':'open','paths':[str(model)]})
        key=client.rpc({'method':'list'})['selected']
        eventually(lambda:row(key).get('metrics',{}).get('loads',0)>0,timeout=20)
        input=XInput()
        def mode(name):
            control=client.rpc({'method':'list'})['presentation']['mode_control']
            self.assertTrue(control['visible']); self.assertTrue(control['enabled'])
            input.click(control['x']+control['width']//2,control['y']+control['height']//2)
            time.sleep(.1); input.press('Home')
            for _ in range({'preview':0,'parts':1,'cuts':2}[name]): input.press('Down')
            input.press('Return')
            eventually(lambda:client.rpc({'method':'list'})['presentation']['inspection_mode']==name,timeout=20)
        try:
            mode('cuts')
            eventually(lambda:row(key).get('metrics',{}).get('backend')=='vtk-gltf' and row(key)['metrics'].get('loads',0)>0,timeout=20)
            state=client.rpc({'method':'inspect','id':key,'action':'state'})
            self.assertTrue(state['loaded'])
            renderer=row(key)['metrics']['renderer']; print('Mode switch renderer:',renderer,flush=True)
            self.assertFalse(any(s in renderer.lower() for s in ('llvmpipe','softpipe','software','unknown')),renderer)
            parts=client.rpc({'method':'inspect','id':key,'action':'parts'})['items']
            self.assertEqual(len(parts),4)
            client.rpc({'method':'inspect','id':key,'action':'select','target':parts[0]['id']})
            cut=client.rpc({'method':'inspect','id':key,'action':'cut','axis':'x','position':0,'keep':'positive','enabled':True})
            loads=row(key)['metrics']['loads']
            mode('parts')
            state=client.rpc({'method':'inspect','id':key,'action':'state'})
            self.assertEqual(state['cuts'],cut['cuts']); self.assertEqual(state['selected'],parts[0]['id'])
            self.assertEqual(row(key)['metrics']['loads'],loads)
            mode('cuts'); self.assertEqual(row(key)['metrics']['loads'],loads)
            mode('preview')
            eventually(lambda:row(key).get('metrics',{}).get('loads',0)>0 and row(key)['metrics'].get('backend')!='vtk-gltf',timeout=20)
            self.assertFalse(client.rpc({'method':'inspect','id':key,'action':'state'})['enabled'])
        finally: input.close()

    @unittest.skipUnless(GPU, 'Native inspection recovery needs Gamescope X11')
    def test_inspection_can_return_to_preview_when_adapter_is_missing(self):
        with tempfile.TemporaryDirectory(prefix='preview-missing-adapter-') as temporary:
            root=Path(temporary); (root/'build').mkdir()
            # A private executable copy gives Qt its own adjacent module path.
            shutil.copy2(ROOT/'build/asset-preview-server',root/'build/asset-preview-server')
            (root/'build/asset-preview-f3d.so').symlink_to(ROOT/'build/asset-preview-f3d.so')
            state=root/'state'; state.mkdir(mode=0o700)
            review=str(state/'review.json')
            entry={'id':'missing-adapter','path':str(root/'missing.glb'),'kind':'model',
                   'inspection':{'enabled':True,'review':review,'asset_id':'missing-adapter','variant':'default'}}
            (state/'previews.json').write_text(json.dumps({'version':1,'entries':[entry],'selected':'missing-adapter','layout':'single'}))
            environment={'ASSET_PREVIEW_RUNTIME_DIR':str(root/'runtime'),'ASSET_PREVIEW_STATE_DIR':str(state)}
            with patch.dict(os.environ,environment),patch.object(client,'ROOT',root):
                client.start(); process=client._SPAWNED[-1]
                try:
                    client.rpc({'method':'show'})
                    presentation=client.rpc({'method':'list'})['presentation']
                    self.assertEqual(presentation['inspection_mode'],'parts')
                    control=presentation['mode_control']; self.assertTrue(control['enabled'])
                    input=XInput()
                    try:
                        input.click(control['x']+control['width']//2,control['y']+control['height']//2)
                        time.sleep(.1); input.press('Home'); input.press('Return')
                        eventually(lambda:client.rpc({'method':'list'})['presentation']['inspection_mode']=='preview')
                        self.assertFalse(client.rpc({'method':'inspect','id':'missing-adapter','action':'state'})['enabled'])
                        self.assertFalse(client.rpc({'method':'list'})['presentation']['mode_control']['enabled'])
                    finally: input.close()
                finally:
                    if process.poll() is None:
                        try: client.rpc({'method':'quit'})
                        except (OSError,RuntimeError): process.terminate()
                    process.wait(timeout=5)

    @unittest.skipUnless(GPU, 'Native drag source needs Gamescope X11')
    def test_drop_files_on_native_surface_opens_without_moving_or_duplicating(self):
        from inspection_fixture import write_fixture
        model=self.root/'drop-target.glb'; write_fixture(model)
        client.rpc({'method':'open','paths':[str(model)]})
        key=client.rpc({'method':'list'})['selected']
        eventually(lambda:row(key).get('metrics',{}).get('loads',0)>0,timeout=15)
        renderer=row(key)['metrics']['renderer']
        self.assertFalse(any(s in renderer.lower() for s in ('llvmpipe','softpipe','software','unknown')),renderer)
        print('Drop target renderer:',renderer,flush=True)
        first=self.root/'drop first.png'; png(first)
        second=self.root/'drop second.png'; png(second)
        client.rpc({'method':'open','paths':[str(first)]})
        helper=self.root/'drop-source'
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Core','x11'],text=True))
        subprocess.run(['c++','-std=c++20','-O2','-fPIC',str(ROOT/'tests/drop_source.cpp'),'-o',str(helper),*flags],check=True,capture_output=True)
        input=XInput()
        try:
            for target,paths,expected in [(None,[first,second],second),(key,[second],second),(None,[model],model)]:
                if target: client.rpc({'method':'select','id':target})
                selected=target or client.rpc({'method':'list'})['selected']
                view=row(selected)['viewport']
                x=ctypes.c_int(); y=ctypes.c_int(); child=ctypes.c_ulong()
                input.x.XTranslateCoordinates(input.display,input.window,input.root,
                    view['x']+view['width']//2,view['y']+view['height']//2,ctypes.byref(x),ctypes.byref(y),ctypes.byref(child))
                result=subprocess.run([str(helper),*[str(path) for path in paths]],capture_output=True,text=True,timeout=10,
                                      env={**os.environ,'ASSET_PREVIEW_DROP_TARGET':str(input.window),
                                           'ASSET_PREVIEW_DROP_X':str(x.value),'ASSET_PREVIEW_DROP_Y':str(y.value)})
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertIn('drop finished: copy=1',result.stdout)
                eventually(lambda:row(client.rpc({'method':'list'})['selected'])['path']==str(expected))
                state=client.rpc({'method':'list'})
                self.assertEqual(len(state['entries']),3)
                self.assertEqual(row(state['selected'])['path'],str(expected))
                self.assertTrue(first.exists()); self.assertTrue(second.exists())
            control=client.rpc({'method':'list'})['presentation']['mode_control']
            self.assertTrue(control['visible']); self.assertTrue(control['enabled'])
            rejected=subprocess.run([str(helper),str(first)],capture_output=True,text=True,timeout=10,
                                    env={**os.environ,'ASSET_PREVIEW_DROP_TARGET':str(input.window),
                                         'ASSET_PREVIEW_DROP_X':str(x.value),'ASSET_PREVIEW_DROP_Y':str(y.value),
                                         'ASSET_PREVIEW_DROP_MOVE_ONLY':'1'})
            self.assertEqual(rejected.returncode,1,rejected.stdout+rejected.stderr)
            self.assertIn('drop rejected',rejected.stdout)
            self.assertEqual(client.rpc({'method':'list'})['selected'],key)
        finally:
            input.xt.XTestFakeButtonEvent(input.display,1,0,0); input.x.XFlush(input.display)
            input.close()

    def test_01_private_socket_inert_connect_and_hidden_unload(self):
        self.assertNotIn("libmpv.so",Path(f"/proc/{self.pid}/maps").read_text())
        self.add(); state=client.rpc({"method":"list"})
        self.assertEqual(state["active"],0); self.assertEqual(state["watched_files"],0)
        socket_path=client.locations()[0]/"preview.sock"
        self.assertEqual(socket_path.stat().st_mode & 0o077,0)
        self.show(); eventually(lambda:row("image").get("metrics",{}).get("loads",0)>0)
        client.rpc({"method":"hide"}); state=client.rpc({"method":"list"})
        self.assertEqual(state["active"],0); self.assertEqual(state["watched_files"],0)
        self.assertNotIn("metrics",row("image"))

    def test_02_atomic_save_reload_and_unrelated_files(self):
        asset=self.add(); self.show(); eventually(lambda:row("image").get("metrics",{}).get("loads",0)>0)
        loads=row("image")["metrics"]["loads"]
        png(asset,(70,160,240,255)); eventually(lambda:row("image")["metrics"]["loads"]>loads)
        loads=row("image")["metrics"]["loads"]
        (asset.parent/"unrelated.txt").write_text("No preview should reload")
        time.sleep(.4); self.assertEqual(row("image")["metrics"]["loads"],loads)
        png(asset,(70,240,130,255)); eventually(lambda:row("image")["metrics"]["loads"]>loads)

    def test_03_missing_nested_file_and_partial_save(self):
        path=self.root/"not-created"/"nested"/"later.png"
        client.rpc({"method":"add","entry":{"id":"missing","path":str(path)}})
        self.show(); self.assertTrue(row("missing")["status"].startswith("Waiting"))
        png(path); eventually(lambda:row("missing")["metrics"]["loads"]==1)
        path.write_bytes(b"incomplete png")
        eventually(lambda:row("missing")["status"].startswith("Waiting"))
        self.assertEqual(row("missing")["metrics"]["loads"],1)
        png(path); eventually(lambda:row("missing")["metrics"]["loads"]>1)

    def test_04_lazy_grid_and_page_selection(self):
        for i in range(6): self.add(f"image{i}")
        self.show(); self.assertEqual(client.rpc({"method":"list"})["active"],1)
        client.rpc({"method":"layout","layout":"grid"}); self.assertEqual(client.rpc({"method":"list"})["active"],4)
        client.rpc({"method":"select","id":"image5"}); state=client.rpc({"method":"list"})
        self.assertEqual(state["active"],2)
        self.assertEqual({e["id"] for e in state["entries"] if e["active"]},{"image4","image5"})
        self.assertNotIn("metrics",row("image0"))
        client.rpc({"method":"layout","layout":"single"}); self.assertEqual(client.rpc({"method":"list"})["active"],1)

    def test_05_no_continuous_rendering_or_idle_cpu(self):
        self.add(); self.show(); time.sleep(.5)
        before=row("image")["metrics"]["renders"]; start=ticks(self.pid)
        time.sleep(1.2)
        self.assertEqual(row("image")["metrics"]["renders"],before)
        self.assertLessEqual(ticks(self.pid)-start,2)

    def test_06_generator_coalescing_and_atomic_publish(self):
        asset=self.add("generated"); template=self.root/"template.png"; png(template)
        source=self.root/"source.txt"; source.write_text("start")
        script=self.root/"generate.py"
        script.write_text("import pathlib,time,sys\nprint('started',flush=True)\ntime.sleep(.9)\np=pathlib.Path(sys.argv[2]);c=p.with_suffix('.candidate');c.write_bytes(pathlib.Path(sys.argv[1]).read_bytes());c.replace(p)\n")
        client.rpc({"method":"add","entry":{"id":"generated","path":str(asset),"watch":[str(source)],"cwd":str(self.root),"command":[sys.executable,str(script),str(template),str(asset)]}})
        self.show(); eventually(lambda:row("generated")["building"])
        source.write_text("latest"); time.sleep(.3)
        eventually(lambda:row("generated")["builds"]==2)
        eventually(lambda:not row("generated")["building"])
        self.assertEqual(row("generated")["builds"],2)
        self.assertTrue(row("generated")["status"].startswith("Live"))
        self.assertIn("started",row("generated")["log"])

    def test_07_hide_stops_owned_generator_descendants(self):
        asset=self.add("cancelled"); child_pid=self.root/"child.pid"
        script=self.root/"long.py"
        script.write_text("import pathlib,subprocess,sys,time\np=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']);pathlib.Path(sys.argv[1]).write_text(str(p.pid));time.sleep(60)\n")
        client.rpc({"method":"add","entry":{"id":"cancelled","path":str(asset),"cwd":str(self.root),"command":[sys.executable,str(script),str(child_pid)]}})
        self.show(); eventually(child_pid.exists); pid=int(child_pid.read_text())
        client.rpc({"method":"hide"}); self.assertFalse(row("cancelled")["building"])
        def gone():
            try: return Path(f"/proc/{pid}/stat").read_text().rsplit(")",1)[1].strip().startswith("Z")
            except FileNotFoundError: return True
        eventually(gone)
        source=script.read_text(); script.write_text(source+"\n# hidden edit\n")
        time.sleep(.4); self.assertEqual(row("cancelled")["builds"],1)

    def test_08_validation_and_failed_generator(self):
        self.add()
        with self.assertRaises(RuntimeError): client.rpc({"method":"settings","id":"image","settings":{"light":99}})
        with self.assertRaises(RuntimeError): client.rpc({"method":"add","entry":{"path":"relative.png"}})
        client.rpc({"method":"add","entry":{"id":"image","path":str(self.root/"image.png"),"cwd":str(self.root),"command":["/definitely/missing-generator"]}})
        self.show(); eventually(lambda:row("image")["status"].startswith("Generator failed"))
        self.assertTrue(client.running())

    def test_09_malformed_protocol_and_timeout(self):
        sock=str(client.locations()[0]/"preview.sock")
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
            s.settimeout(2); s.connect(sock); s.sendall(b"not-json\n")
            self.assertFalse(json.loads(s.recv(4096))["ok"])
        self.assertTrue(client.running())
        asset=self.add("timeout")
        client.rpc({"method":"add","entry":{"id":"timeout","path":str(asset),"cwd":str(self.root),"timeout":1,"command":[sys.executable,"-c","import time;time.sleep(10)"]}})
        self.show(); eventually(lambda:row("timeout")["status"]=="Generator timed out")
        self.assertFalse(row("timeout")["building"])

    @unittest.skipUnless(GPU,"Hardware Gamescope lane")
    def test_10_f3d_hardware_model_material_and_idle(self):
        obj=self.root/"cube.obj"
        subprocess.run([sys.executable,str(ROOT/"examples/generate_cube.py"),str(obj)],check=True,capture_output=True)
        client.rpc({"method":"add","entry":{"id":"model","path":str(obj)}})
        self.show(); eventually(lambda:row("model").get("metrics",{}).get("engine"),timeout=30)
        renderer=row("model")["metrics"]["renderer"]
        self.assertFalse(any(s in renderer.lower() for s in ("llvmpipe","softpipe","software","unknown")),renderer)
        time.sleep(.5); initial=row("model")["metrics"]; time.sleep(1)
        self.assertEqual(row("model")["metrics"]["renders"],initial["renders"])
        obj.write_text(obj.read_text().replace("v -1.0","v -1.5"))
        eventually(lambda:row("model")["metrics"]["loads"]>initial["loads"],timeout=15)
        initial=row("model")["metrics"]["loads"]
        mtl=obj.with_suffix(".mtl"); mtl.write_text(mtl.read_text().replace("0.18 0.65 0.55","0.9 0.2 0.1"))
        eventually(lambda:row("model")["metrics"]["loads"]>initial,timeout=15)
        albedo=self.root/"albedo.png"; normal=self.root/"normal.png"; orm=self.root/"orm.png"
        png(albedo,(160,210,180,255)); png(normal,(128,128,255,255)); png(orm,(255,100,150,255))
        client.rpc({"method":"add","entry":{"id":"material","path":str(albedo),"kind":"material","maps":{"normal":str(normal),"orm":str(orm)}}})
        client.rpc({"method":"select","id":"material"})
        eventually(lambda:row("material").get("metrics",{}).get("engine"),timeout=30)
        initial=row("material")["metrics"]["loads"]; png(normal,(150,128,253,255))
        eventually(lambda:row("material")["metrics"]["loads"]>initial,timeout=15)
        client.rpc({"method":"settings","id":"material","settings":{"shape":"cube","axes":True}})
        self.assertGreater(row("material")["metrics"]["loads"],initial)
        evidence=os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR")
        if evidence:
            target=Path(evidence).resolve(); target.mkdir(parents=True,exist_ok=True)
            capture=target/"material.png"; capture.unlink(missing_ok=True)
            client.rpc({"method":"capture","path":str(capture)})
            view=row("material")["viewport"]
            pixel=png_pixel(capture,view["x"]+3,view["y"]+3)
            self.assertLess(max(pixel),80,f"The native 3D framebuffer must have a dark opaque background: {pixel}")
            (target/"hardware.json").write_text(json.dumps(client.rpc({"method":"list"}),indent=2))

    @unittest.skipUnless(GPU,"Hardware Gamescope lane")
    def test_material_sphere_flat_normal_preserves_surface(self):
        albedo=self.root/"sphere-white rgba.png"; rgb=self.root/"sphere-white-rgb.png"
        normal=self.root/"sphere-normal.png"
        png(albedo,(255,255,255,255)); png(rgb,(255,255,255)); png(normal,(128,128,255,255))
        def register(path,maps):
            client.rpc({"method":"add","entry":{"id":"sphere-normal","path":str(path),"kind":"material",
                "maps":maps,"settings":{"shape":"sphere","grid":False}}})
            if client.rpc({"method":"list"})["window_visible"]:
                eventually(lambda:row("sphere-normal").get("metrics",{}).get("engine"),timeout=30)
        register(rgb,{})
        self.show(); eventually(lambda:row("sphere-normal").get("metrics",{}).get("engine"),timeout=30)
        view=row("sphere-normal")["viewport"]
        def samples(label):
            target=self.root/f"sphere-{label}.png"
            client.rpc({"method":"capture","path":str(target)})
            return png_pixels(target,[(view["x"]+view["width"]//2+round(view["height"]*dx),
                view["y"]+round(view["height"]*(.5+dy)))
                for dx in (-.22,-.16,-.08,0,.08,.16,.22)
                for dy in (-.22,-.16,-.08,0,.08,.16,.22) if dx*dx+dy*dy<.24**2])
        # RGBA overrides previously forced even fully opaque samples through an
        # unsorted translucent pass. A three-quarter view and central pixels
        # missed the rear faces overwriting the upper front-view triangles.
        for camera in ("front","right","back","top","isometric"):
            register(rgb,{})
            client.rpc({"method":"camera_view","id":"sphere-normal","view":camera})
            bare=samples(camera+"-bare")
            register(albedo,{"normal":str(normal)})
            flat=samples(camera+"-flat")
            self.assertEqual(row("sphere-normal")["metrics"]["triangles"],2208)
            for expected,actual in zip(bare,flat):
                self.assertLess(max(abs(a-b) for a,b in zip(expected,actual)),20,
                    f"Opaque RGBA and a flat normal must preserve the RGB surface in {camera} view")
        initial=row("sphere-normal")["metrics"]["loads"]
        png(normal,(210,128,225,255))
        eventually(lambda:row("sphere-normal")["metrics"]["loads"]>initial,timeout=15)
        tilted=samples("tilted")
        self.assertGreater(max(abs(a-b) for p,q in zip(flat,tilted) for a,b in zip(p,q)),10,
            "The material sample must apply the normal map, rather than ignore it")

    def test_11_persistence_and_read_only_completions(self):
        self.add("persisted"); state=self.root/"state/previews.json"; before=state.read_bytes()
        result=subprocess.run([str(ROOT/"bin/asset-preview"),"--complete-ids"],capture_output=True,text=True)
        self.assertIn("persisted",result.stdout); self.assertEqual(state.read_bytes(),before)
        cwd=self.root/"removed-project"; cwd.mkdir()
        client.rpc({"method":"add","entry":{"id":"stale","path":str(cwd/"missing.png"),"cwd":str(cwd),"command":[sys.executable,"-c","pass"]}})
        client.rpc({"method":"quit"}); eventually(lambda:not client.running())
        cwd.rmdir()
        self.assertTrue(client.start()); self.pid=client.rpc({"method":"ping"})["pid"]
        type(self).pid=self.pid
        self.assertEqual(row("persisted")["path"],str(self.root/"persisted.png"))
        self.assertFalse(row("persisted")["active"])
        client.rpc({"method":"select","id":"stale"}); self.show()
        eventually(lambda:row("stale")["status"].startswith("Generator failed"))

    @unittest.skipUnless(GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland","Hardware Gamescope X11 input lane")
    def test_12_keyboard_navigation_and_native_model_orbit(self):
        obj=self.root/"input-cube.obj"
        subprocess.run([sys.executable,str(ROOT/"examples/generate_cube.py"),str(obj)],check=True,capture_output=True)
        client.rpc({"method":"add","entry":{"id":"orbit","path":str(obj)}})
        self.add("next"); self.show()
        eventually(lambda:row("orbit").get("metrics",{}).get("engine"))
        input=XInput()
        try:
            input.shortcut("Right"); eventually(lambda:client.rpc({"method":"list"})["selected"]=="next")
            input.shortcut("Left"); eventually(lambda:client.rpc({"method":"list"})["selected"]=="orbit")
            eventually(lambda:row("orbit").get("metrics",{}).get("engine"))
            time.sleep(.2); state=row("orbit"); view=state["viewport"]; renders=state["metrics"]["renders"]
            input.drag(view["x"]+view["width"]//2,view["y"]+view["height"]//2)
            eventually(lambda:row("orbit")["metrics"]["renders"]>renders)
            camera=row("orbit")["metrics"]["camera"]
            radius=math.dist(camera["position"],camera["focal"])
            self.assertAlmostEqual(horizon_roll(camera),0,places=8)
            self.assertGreater(camera["up"][1],0)
            # Repeated large drags hit both pitch limits without rolling or
            # crossing the pole, and preserve orbit radius.
            for dy in (-220,-220,-220,220,220,220):
                renders=row("orbit")["metrics"]["renders"]
                input.drag(view["x"]+view["width"]//2,view["y"]+view["height"]//2,100,dy)
                eventually(lambda:row("orbit")["metrics"]["renders"]>renders)
                camera=row("orbit")["metrics"]["camera"]
                self.assertAlmostEqual(horizon_roll(camera),0,places=8)
                self.assertGreater(camera["up"][1],0)
                self.assertAlmostEqual(math.dist(camera["position"],camera["focal"]),radius,places=5)
                self.assertLess(abs(camera["position"][1]-camera["focal"][1])/radius,.9999)
            client.rpc({"method":"reload","id":"orbit"})
            eventually(lambda:row("orbit")["metrics"]["loads"]>1)
            for key in ("position","focal","up"):
                for before,after in zip(camera[key],row("orbit")["metrics"]["camera"][key]):
                    self.assertAlmostEqual(before,after,places=5)
            client.rpc({"method":"settings","id":"orbit","settings":{"up_axis":"z"}})
            eventually(lambda:not row("orbit")["metrics"].get("command_pending") and not row("orbit")["metrics"].get("loading"),timeout=25)
            self.assertAlmostEqual(horizon_roll(row("orbit")["metrics"]["camera"],2),0,places=8)
            self.assertGreater(row("orbit")["metrics"]["camera"]["up"][2],0)
            # Click the real native overlay to test input stacking.
            time.sleep(.2)
            button=row("orbit")["controls"]["options"]
            input.click(button["x"]+button["width"]//2,button["y"]+button["height"]//2)
            eventually(lambda:client.rpc({"method":"list"})["options_open"])
            input.key("t",1); input.key("t",0); input.x.XFlush(input.display)
            eventually(lambda:row("orbit")["settings"].get("textures") is False)
            self.assertFalse(client.rpc({"method":"list"})["options_open"])
            # Global navigation also works while the native child has focus.
            input.press("Right"); eventually(lambda:client.rpc({"method":"list"})["selected"]=="next")
            view=row("next")["viewport"]
            input.click(view["x"]+view["width"]//2,view["y"]+view["height"]//2)
            input.press("Left"); eventually(lambda:client.rpc({"method":"list"})["selected"]=="orbit")
        finally: input.close()

    @unittest.skipUnless(GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland","Hardware Gamescope X11 input lane")
    def test_25_zoom_and_orbit_preserve_focal_point(self):
        obj=self.root/"zoom-cube.obj"
        subprocess.run([sys.executable,str(ROOT/"examples/generate_cube.py"),str(obj)],check=True,capture_output=True)
        client.rpc({"method":"add","entry":{"id":"zoom","path":str(obj)}})
        self.show()
        eventually(lambda:row("zoom").get("metrics",{}).get("engine"))
        renderer=row("zoom")["metrics"]["renderer"]
        self.assertFalse(any(s in renderer.lower() for s in ("llvmpipe","softpipe","software","unknown")),renderer)
        input=XInput()
        try:
            for orthographic in (False,True):
                for lock_horizon in (True,False):
                    with self.subTest(orthographic=orthographic,lock_horizon=lock_horizon):
                        client.rpc({"method":"settings","id":"zoom","settings":{"orthographic":orthographic,"lock_horizon":lock_horizon}})
                        time.sleep(.5)
                        view=row("zoom")["viewport"]
                        x=view["x"]+view["width"]*3//4; y=view["y"]+view["height"]//3
                        before_pan=row("zoom")["metrics"]["camera"]
                        input.key("Shift_L",1); input.drag(x,y,20,15); input.key("Shift_L",0); input.x.XFlush(input.display)
                        eventually(lambda:math.dist(before_pan["focal"],row("zoom")["metrics"]["camera"]["focal"])>1e-5)
                        before=row("zoom")["metrics"]["camera"]
                        input.scroll(x,y)
                        def zoom_changed():
                            current=row("zoom")["metrics"]["camera"]
                            return abs(before["parallel_span"]-current["parallel_span"])>1e-5 if orthographic else math.dist(before["position"],current["position"])>1e-5
                        eventually(zoom_changed)
                        after=row("zoom")["metrics"]["camera"]
                        self.assertEqual(before["focal"],after["focal"])
                        distance=math.dist(before["position"],before["focal"])
                        new_distance=math.dist(after["position"],after["focal"])
                        if orthographic:
                            self.assertEqual(before["position"],after["position"])
                            self.assertLess(after["parallel_span"],before["parallel_span"])
                        else: self.assertLess(new_distance,distance)
                        for a,b in zip(before["up"],after["up"]): self.assertAlmostEqual(a,b,places=12)
                        input.scroll(x,y,False)
                        def zoom_restored():
                            current=row("zoom")["metrics"]["camera"]
                            return abs(before["parallel_span"]-current["parallel_span"])<1e-6 if orthographic else math.dist(before["position"],current["position"])<1e-6
                        eventually(zoom_restored)
                        restored=row("zoom")["metrics"]["camera"]
                        for key in ("position","focal","up"):
                            for a,b in zip(before[key],restored[key]): self.assertAlmostEqual(a,b,places=6)
                        # Orbit the panned view without triggering double-click Fit.
                        time.sleep(.5); input.drag(x,y,45,20)
                        eventually(lambda:math.dist(restored["position"],row("zoom")["metrics"]["camera"]["position"])>1e-5)
                        orbited=row("zoom")["metrics"]["camera"]
                        self.assertEqual(restored["focal"],orbited["focal"])
                        self.assertAlmostEqual(math.dist(orbited["position"],orbited["focal"]),distance,places=6)
        finally: input.close()

    @unittest.skipUnless(GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland","Hardware Gamescope X11 input lane")
    def test_26_pointer_zoom_image(self):
        self.add("zoom-image",settings={"background":"dark"}); self.show()
        eventually(lambda:row("zoom-image").get("metrics",{}).get("loads"))
        time.sleep(.2)
        view=row("zoom-image")["viewport"]
        size=min(view["width"],view["height"])
        # Just outside the fitted square: center zoom would move the edge
        # across this point, while pointer zoom keeps it outside the image.
        x=view["x"]+(view["width"]+size)//2+5
        y=view["y"]+view["height"]//2
        capture=self.root/"image-zoom.png"
        client.rpc({"method":"capture","path":str(capture)})
        before=png_pixel(capture,x,y)
        self.assertEqual(before,(23,28,34))
        input=XInput()
        try:
            renders=row("zoom-image")["metrics"]["renders"]
            input.click(x,y); time.sleep(.1); input.scroll(x,y)
            eventually(lambda:row("zoom-image")["metrics"]["renders"]>renders)
            eventually(lambda:row("zoom-image")["metrics"]["image_view"]["zoom"]>1)
            time.sleep(.1)
            capture.unlink()
            client.rpc({"method":"capture","path":str(capture)})
            self.assertEqual(png_pixel(capture,x,y),before)
            # The edge itself moves away from the anchor as the image grows.
            self.assertEqual(png_pixel(capture,x-5,y),before)
        finally: input.close()

    @unittest.skipUnless(GPU,"Hardware model triangle counts and overlays")
    def test_21_triangle_counts_overlay_layout_and_refresh(self):
        obj=self.root/"counted-cube.obj"
        subprocess.run([sys.executable,str(ROOT/"examples/generate_cube.py"),str(obj)],check=True,capture_output=True)
        client.rpc({"method":"add","entry":{"id":"counts","path":str(obj),"label":"Counted cube"}})
        self.show()
        eventually(lambda:row("counts").get("metrics",{}).get("engine"))
        state=row("counts"); self.assertEqual(state["metrics"]["triangles"],12)
        renderer=state["metrics"]["renderer"]
        self.assertFalse(any(s in renderer.lower() for s in ("llvmpipe","softpipe","software","unknown")),renderer)
        viewport=state["viewport"]; overlays=state["overlays"]
        self.assertLess(overlays["name"]["width"],viewport["width"]//2)
        self.assertLess(overlays["status"]["width"],viewport["width"]//2)
        self.assertLess(overlays["name"]["x"]+overlays["name"]["width"],overlays["controls"]["x"])
        self.assertGreater(overlays["status"]["y"],overlays["name"]["y"])
        self.assertTrue(overlays["triangles"]["visible"])
        loads=state["metrics"]["loads"]
        for value in (False,True):
            client.rpc({"method":"settings","id":"counts","settings":{"triangles":value}})
            self.assertEqual(row("counts")["overlays"]["triangles"]["visible"],value)
            self.assertEqual(row("counts")["metrics"]["loads"],loads)
            self.assertEqual(row("counts")["metrics"]["camera"],state["metrics"]["camera"])
        self.assertTrue(row("counts")["status_visible"])
        self.assertTrue(row("counts")["overlays"]["triangles"]["visible"])
        # Atomic source edits update the count with the loaded model; a missing
        # replacement retains last-good geometry and its count.
        candidate=obj.with_suffix(".candidate")
        candidate.write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"); candidate.replace(obj)
        eventually(lambda:row("counts")["metrics"]["triangles"]==1)
        obj.unlink(); eventually(lambda:row("counts")["status"].startswith("Waiting"))
        self.assertEqual(row("counts")["metrics"]["triangles"],1)
        self.assertTrue(row("counts")["status_visible"])
        self.assertTrue(row("counts")["overlays"]["triangles"]["visible"])
        albedo=self.root/"counted-material.png"; png(albedo)
        client.rpc({"method":"add","entry":{"id":"sample","kind":"material","path":str(albedo),"settings":{"shape":"cube"}}})
        client.rpc({"method":"select","id":"sample"})
        eventually(lambda:row("sample").get("metrics",{}).get("engine"))
        self.assertEqual(row("sample")["metrics"]["triangles"],12)
        client.rpc({"method":"settings","id":"sample","settings":{"shape":"plane"}})
        eventually(lambda:row("sample")["metrics"]["triangles"]==2)
        with self.assertRaisesRegex(RuntimeError,"Boolean setting required"):
            client.rpc({"method":"settings","id":"sample","settings":{"triangles":"yes"}})
        client.rpc({"method":"hide"}); self.assertNotIn("metrics",row("sample"))
        self.show(); eventually(lambda:row("sample").get("metrics",{}).get("engine"))
        self.assertEqual(row("sample")["metrics"]["triangles"],2)

    @unittest.skipUnless(GPU,"Native top settings menu")
    def test_31_top_settings_menu_defaults_and_current(self):
        self.add("menu-image"); self.show()
        input=XInput()
        try:
            input.shortcut("v")
            eventually(lambda:client.rpc({"method":"list"})["options_open"])
            input.press("o"); time.sleep(.2)
            input.press("p")
            eventually(lambda:row("menu-image")["settings"].get("nearest") is True)
            input.shortcut("s"); time.sleep(.2)
            input.press("i"); time.sleep(.2)
            input.press("p")
            eventually(lambda:client.rpc({"method":"list"})["preferences"]["image"].get("nearest") is False)
            self.assertTrue(row("menu-image")["settings"]["nearest"])
            self.add("menu-new")
            self.assertFalse(row("menu-new")["settings"]["nearest"])
        finally: input.close()

    def test_30_explicit_defaults_are_independent_and_persisted(self):
        self.add("existing")
        original=row("existing")["settings"]
        client.rpc({"method":"defaults","group":"image","settings":{"nearest":True,"background":"light"}})
        self.assertEqual(row("existing")["settings"],original)
        self.add("new")
        self.assertEqual(row("new")["settings"],{"nearest":True,"background":"light"})
        for request in ({"group":"bad","settings":{}},{"group":"3d","settings":{"light":6}}):
            with self.assertRaises(RuntimeError): client.rpc({"method":"defaults",**request})
        before=client.rpc({"method":"list"})["preferences"]
        # Persistence is also checked after a clean restart.
        client.rpc({"method":"quit"}); eventually(lambda:not client.running())
        client.start()
        self.assertEqual(client.rpc({"method":"list"})["preferences"],before)

    def test_22_preferences_inheritance_reconnect_and_close_all(self):
        self.add("image-settings")
        image={"nearest":True,"background":"light"}
        client.rpc({"method":"settings","id":"image-settings","settings":image})
        self.add("other-image"); self.assertEqual(row("other-image")["settings"],image)
        obj=self.root/"preferences.obj"
        model={"grid":False,"axes":True,"edges":True,"orthographic":True,
               "light":2.5,"background":"dark","materials":False,"textures":False,
               "lock_horizon":False,"up_axis":"z","lighting":"lightkit","triangles":False}
        client.rpc({"method":"add","entry":{"id":"model-settings","path":str(obj)}})
        client.rpc({"method":"settings","id":"model-settings","settings":model})
        client.rpc({"method":"add","entry":{"id":"other-model","path":str(obj)}})
        self.assertEqual(row("other-model")["settings"],model)
        material={"shape":"cube","roughness":0.2,"metallic":0.6}
        client.rpc({"method":"add","entry":{"id":"material-settings","kind":"material","path":str(self.root/"image-settings.png")}})
        client.rpc({"method":"settings","id":"material-settings","settings":material})
        self.assertEqual(row("material-settings")["settings"],model|material)
        movie=self.root/"not-yet-published.mp4"
        playback={"paused":True,"muted":False,"loop":False,"background":"light"}
        client.rpc({"method":"add","entry":{"id":"video-settings","path":str(movie)}})
        client.rpc({"method":"settings","id":"video-settings","settings":playback})
        client.rpc({"method":"add","entry":{"id":"other-video","path":str(movie)}})
        self.assertEqual(row("other-video")["settings"],playback)
        # Explicit connection settings override defaults without changing them.
        client.rpc({"method":"add","entry":{"id":"explicit","path":str(obj),"settings":{"triangles":True}}})
        self.assertTrue(row("explicit")["settings"]["triangles"])
        remembered=client.rpc({"method":"list"})["preferences"]
        self.assertFalse(remembered["3d"]["triangles"])
        # A same-kind reconnection keeps its last inspection choices.
        client.rpc({"method":"add","entry":{"id":"explicit","path":str(obj),"settings":{"edges":False}}})
        self.assertTrue(row("explicit")["settings"]["triangles"])
        self.assertFalse(row("explicit")["settings"]["edges"])
        # CLI material registration must not silently reset the remembered shape.
        subprocess.run([str(ROOT/"bin/asset-preview"),"material",str(self.root/"image-settings.png"),"--id","cli-material"],check=True,capture_output=True)
        self.assertEqual(row("cli-material")["settings"]["shape"],"cube")
        client.rpc({"method":"layout","layout":"grid","grid_size":3})
        subprocess.run([str(ROOT/"bin/asset-preview"),"close-all"],check=True,capture_output=True)
        state=client.rpc({"method":"list"})
        self.assertEqual(state["entries"],[]); self.assertEqual(state["preferences"],remembered)
        self.assertTrue((self.root/"image-settings.png").exists())
        client.rpc({"method":"quit"}); eventually(lambda:not client.running())
        client.start(); self.pid=client.rpc({"method":"ping"})["pid"]; type(self).pid=self.pid
        state=client.rpc({"method":"list"})
        self.assertEqual(state["preferences"],remembered)
        self.assertEqual((state["layout"],state["grid_size"]),("grid",3))
        self.add("after-restart"); self.assertEqual(row("after-restart")["settings"],image)
        client.rpc({"method":"add","entry":{"id":"after-restart-model","path":str(obj)}})
        self.assertEqual(row("after-restart-model")["settings"],model|material)

    def test_23_settings_and_close_all_failed_saves_preserve_views(self):
        self.add("saved-a"); self.add("saved-b"); self.show()
        eventually(lambda:row("saved-a").get("metrics",{}).get("loads"))
        before=client.rpc({"method":"list"})
        registry=self.root/"state/previews.json"; backup=registry.with_suffix(".backup")
        registry.rename(backup); registry.mkdir()
        try:
            with self.assertRaises(RuntimeError):
                client.rpc({"method":"settings","id":"saved-a","settings":{"nearest":True}})
            with self.assertRaises(RuntimeError): client.rpc({"method":"close_all"})
            after=client.rpc({"method":"list"})
            self.assertEqual(after["preferences"],before["preferences"])
            self.assertEqual(after["selected"],before["selected"])
            for old,new in zip(before["entries"],after["entries"]):
                for key in ("id","path","settings","active","status","builds","revisions"):
                    self.assertEqual(new[key],old[key])
                if "metrics" in old: self.assertEqual(new["metrics"]["loads"],old["metrics"]["loads"])
        finally:
            registry.rmdir(); backup.replace(registry)
        client.rpc({"method":"close_all"})
        state=client.rpc({"method":"list"})
        self.assertEqual(state["entries"],[]); self.assertEqual(state["active"],0)
        self.assertTrue(state["window_visible"])
        self.assertEqual((state["watched_files"],state["watched_directories"]),(0,0))

    def test_24_close_all_stops_generators_and_preserves_preferences(self):
        asset=self.add("close-builder"); child_pid=self.root/"close-all-child.pid"
        script=self.root/"close-all-builder.py"
        script.write_text("import pathlib,subprocess,sys,time\np=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']);pathlib.Path(sys.argv[1]).write_text(str(p.pid));time.sleep(60)\n")
        client.rpc({"method":"settings","id":"close-builder","settings":{"nearest":True}})
        client.rpc({"method":"add","entry":{"id":"close-builder","path":str(asset),"cwd":str(self.root),"command":[sys.executable,str(script),str(child_pid)]}})
        self.show(); eventually(child_pid.exists); pid=int(child_pid.read_text())
        client.rpc({"method":"close_all"})
        def gone():
            try: return Path(f"/proc/{pid}/stat").read_text().rsplit(")",1)[1].strip().startswith("Z")
            except FileNotFoundError: return True
        eventually(gone)
        state=client.rpc({"method":"list"})
        self.assertEqual(state["entries"],[]); self.assertEqual(state["active"],0)
        self.assertEqual(state["preferences"]["image"],{"nearest":True})
        self.assertTrue(asset.exists())
        # A closed/stopped app remains stopped when close-all is called again.
        client.rpc({"method":"quit"}); eventually(lambda:not client.running())
        subprocess.run([str(ROOT/"bin/asset-preview"),"close-all"],check=True,capture_output=True)
        self.assertFalse(client.running())
        client.start(); self.pid=client.rpc({"method":"ping"})["pid"]; type(self).pid=self.pid

    def test_13_corrupt_state_is_preserved(self):
        runtime,state=client.locations(); bad_runtime=self.root/"bad-runtime"; bad_state=self.root/"bad-state"
        client.private_directory(bad_state); corrupt=bad_state/"previews.json"
        os.environ["ASSET_PREVIEW_RUNTIME_DIR"]=str(bad_runtime); os.environ["ASSET_PREVIEW_STATE_DIR"]=str(bad_state)
        try:
            for content in (b"{unfinished", json.dumps({"version":1,"entries":[],"preferences":{"3d":{"up_axis":"x"}}}).encode(),
                            json.dumps({"version":1,"entries":[],"preferences":{"unknown":{}}}).encode()):
                corrupt.write_bytes(content)
                with self.assertRaises(RuntimeError): client.start()
                self.assertEqual(corrupt.read_bytes(),content)
        finally:
            os.environ["ASSET_PREVIEW_RUNTIME_DIR"]=str(runtime); os.environ["ASSET_PREVIEW_STATE_DIR"]=str(state)

    def test_14_saved_progress_refreshes_before_generator_exit(self):
        asset=self.add("progress"); template=self.root/"progress-template.png"; png(template,(60,170,220,255))
        script=self.root/"progress.py"
        script.write_text("import pathlib,sys,time\np=pathlib.Path(sys.argv[2]);c=p.with_suffix('.candidate');c.write_bytes(pathlib.Path(sys.argv[1]).read_bytes());c.replace(p);print('published intermediate',flush=True);time.sleep(1.5)\n")
        client.rpc({"method":"add","entry":{"id":"progress","path":str(asset),"cwd":str(self.root),"command":[sys.executable,str(script),str(template),str(asset)]}})
        self.show()
        eventually(lambda:row("progress")["metrics"]["loads"]>=2 and row("progress")["building"])
        self.assertIn("Building",row("progress")["status"])
        eventually(lambda:not row("progress")["building"])

    def test_15_larger_grids_status_validation_and_persistence(self):
        for i in range(20): self.add(f"tile{i}")
        self.show()
        for size,count in ((3,9),(4,16)):
            client.rpc({"method":"select","id":"tile0"})
            client.rpc({"method":"layout","layout":"grid","grid_size":size})
            eventually(lambda:client.rpc({"method":"list"})["active"]==count)
            client.rpc({"method":"select","id":"tile19"})
            eventually(lambda:client.rpc({"method":"list"})["active"]==20-count*(19//count))
            self.assertNotIn("metrics",row("tile0"))
        client.rpc({"method":"select","id":"tile0"})
        eventually(lambda:row("tile0").get("metrics",{}).get("loads",0)>0)
        self.assertTrue(row("tile0")["status_visible"])
        client.rpc({"method":"add","entry":{"id":"tile0","path":str(self.root/"uncreated-grid.png")}})
        self.assertTrue(row("tile0")["status_visible"])
        self.assertTrue(row("tile0")["status"].startswith("Waiting"))
        for fields in ({"grid_size":5},{"grid_size":2.5},{"grid_size":"3"},{"layout":None}):
            with self.assertRaises(RuntimeError): client.rpc({"method":"layout",**fields})
        for fields in ({"up_axis":"x"},{"lighting":"unknown"},{"textures":1}):
            with self.assertRaises(RuntimeError): client.rpc({"method":"settings","id":"tile0","settings":fields})
        client.rpc({"method":"quit"}); eventually(lambda:not client.running())
        registry=self.root/"state/previews.json"
        saved=json.loads(registry.read_text())
        self.assertNotIn("compact",saved)
        saved["compact"]=True  # Old registries must still restore normally.
        registry.write_text(json.dumps(saved))
        self.assertTrue(client.start()); self.pid=client.rpc({"method":"ping"})["pid"]; type(self).pid=self.pid
        state=client.rpc({"method":"list"})
        self.assertEqual((state["layout"],state["grid_size"]),("grid",4))
        self.assertNotIn("compact",state)
        client.rpc({"method":"layout","grid_size":4})
        self.assertNotIn("compact",json.loads(registry.read_text()))
        self.assertEqual(state["active"],0)

    @unittest.skipUnless(GPU,"Hardware Gamescope lane")
    def test_16_material_modes_restore_authored_appearance_and_camera(self):
        albedo=self.root/"mode-albedo.png"; png(albedo,(210,35,20,255))
        client.rpc({"method":"add","entry":{"id":"modes","path":str(albedo),"kind":"material"}})
        self.show(); eventually(lambda:row("modes").get("metrics",{}).get("engine"),timeout=30)
        eventually(lambda:row("modes")["metrics"]["renders"]>0)
        state=row("modes"); view=state["viewport"]; camera=state["metrics"]["camera"]
        def color(name):
            time.sleep(.2)
            target=self.root/f"mode-{name}.png"; target.unlink(missing_ok=True)
            client.rpc({"method":"capture","path":str(target)})
            return png_pixel(target,view["x"]+view["width"]//2,view["y"]+view["height"]//2)
        original=color("original")
        client.rpc({"method":"settings","id":"modes","settings":{"textures":False}})
        bare=color("bare")
        self.assertGreater(sum(abs(a-b) for a,b in zip(original,bare)),30)
        client.rpc({"method":"settings","id":"modes","settings":{"materials":False}})
        clay=color("clay")
        self.assertGreater(sum(abs(a-b) for a,b in zip(bare,clay)),10)
        client.rpc({"method":"settings","id":"modes","settings":{"materials":True,"textures":True}})
        restored=color("restored")
        self.assertLessEqual(max(abs(a-b) for a,b in zip(original,restored)),1)
        for key in ("position","focal","up"):
            for before,after in zip(camera[key],row("modes")["metrics"]["camera"][key]):
                self.assertAlmostEqual(before,after,places=8)
        self.assertEqual(row("modes")["metrics"]["lighting"],"studio")
        client.rpc({"method":"settings","id":"modes","settings":{"lighting":"lightkit"}})
        self.assertEqual(row("modes")["metrics"]["lighting"],"lightkit")
        time.sleep(.5); renders=row("modes")["metrics"]["renders"]; time.sleep(.7)
        self.assertEqual(row("modes")["metrics"]["renders"],renders)

    def test_17_video_registration_types_streaming_limit_and_validation(self):
        for suffix in ("mp4","mov","webm","mkv","gif"):
            client.rpc({"method":"add","entry":{"id":suffix,"path":str(self.root/f"missing.{suffix}")}})
            self.assertEqual(row(suffix)["kind"],"video")
            self.assertFalse(row(suffix)["active"])
        large=self.root/"large.mp4"
        with large.open("wb") as f: f.truncate(300*1024*1024)
        client.rpc({"method":"add","entry":{"id":"large","path":str(large)}})
        self.assertEqual(row("large")["kind"],"video")
        with self.assertRaises(RuntimeError): client.rpc({"method":"seek","id":"large","seconds":0})
        with self.assertRaises(RuntimeError): client.rpc({"method":"settings","id":"large","settings":{"paused":1}})
        with self.assertRaises(RuntimeError): client.rpc({"method":"add","entry":{"id":"model-large","path":str(large),"kind":"model"}})
        self.assertEqual(client.rpc({"method":"list"})["active"],0)

    def test_42_audio_registration_cli_defaults_and_streaming(self):
        for suffix in ("wav","wave","mp3","flac","ogg","oga","opus","m4a","aac","aif","aiff","wma","ape"):
            result=subprocess.run([str(ROOT/"bin/asset-preview"),"add",str(self.root/f"missing.{suffix}"),"--id",f"audio-{suffix}"],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(row(f"audio-{suffix}")["kind"],"audio")
            self.assertFalse(row(f"audio-{suffix}")["active"])
        result=subprocess.run([str(ROOT/"bin/asset-preview"),"add",str(self.root/"audio.data"),"--kind","audio","--id","audio-explicit"],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(row("audio-explicit")["kind"],"audio")
        large=self.root/"large.wav"
        with large.open("wb") as output: output.truncate(300*1024*1024)
        client.rpc({"method":"add","entry":{"id":"audio-large","path":str(large)}})
        self.assertEqual(row("audio-large")["kind"],"audio")
        for seconds in (-1,"3",None):
            with self.assertRaises(RuntimeError): client.rpc({"method":"seek","id":"audio-large","seconds":seconds})
        with self.assertRaises(RuntimeError): client.rpc({"method":"settings","id":"audio-large","settings":{"paused":1}})
        with self.assertRaises(RuntimeError): client.rpc({"method":"settings","id":"audio-large","settings":{"muted":"false"}})
        with self.assertRaises(RuntimeError): client.rpc({"method":"add","entry":{"id":"audio-camera","kind":"audio","path":str(large),"image_view":{"zoom":1,"pan":[0,0]}}})
        client.rpc({"method":"defaults","group":"audio","settings":{"paused":False,"muted":True,"loop":False}})
        client.rpc({"method":"add","entry":{"id":"audio-inherit","path":str(self.root/"inherited.mp3")}})
        self.assertEqual(row("audio-inherit")["settings"],{"paused":False,"muted":True,"loop":False})
        client.rpc({"method":"add","entry":{"id":"video-separate","path":str(self.root/"separate.mp4")}})
        self.assertEqual(row("video-separate")["settings"],{})
        client.rpc({"method":"settings","id":"audio-inherit","settings":{"paused":True,"muted":False}})
        client.rpc({"method":"add","entry":{"id":"audio-inherit","path":str(self.root/"new.mp3")}})
        self.assertEqual(row("audio-inherit")["settings"],{"paused":True,"muted":False,"loop":False})
        client.rpc({"method":"quit"}); eventually(lambda:not client.running()); client.start()
        self.pid=client.rpc({"method":"ping"})["pid"]; type(self).pid=self.pid
        self.assertEqual(client.rpc({"method":"list"})["preferences"]["audio"],{"paused":True,"muted":False,"loop":False})
        self.assertEqual(client.rpc({"method":"list"})["active"],0)

    def test_43_audio_playback_reload_last_good_and_lifecycle(self):
        asset=self.root/"standalone.wav"; audio(asset,duration=6)
        client.rpc({"method":"add","entry":{"id":"music","path":str(asset)}})
        self.add("audio-next"); self.show()
        eventually(lambda:row("music").get("metrics",{}).get("loads",0)==1,timeout=20)
        eventually(lambda:row("music")["metrics"].get("paused") is True and row("music")["metrics"].get("muted") is False)
        state=row("music"); metrics=state["metrics"]
        self.assertEqual(state["kind"],"audio"); self.assertEqual(metrics["backend"],"libmpv")
        self.assertEqual(metrics["sample_rate"],48000); self.assertEqual(metrics["channels"],2)
        self.assertAlmostEqual(metrics["duration"],6,delta=.05)
        self.assertNotIn("renderer",metrics); self.assertNotIn("width",metrics)
        self.assertTrue(state["controls"]["timeline"]["enabled"])
        eventually(lambda:row("music")["metrics"]["waveform"]["ready"])
        self.assertTrue(row("music")["controls"]["timeline"]["waveform"])
        self.assertEqual(row("music")["metrics"]["waveform"]["bins"],2048)
        self.assertEqual(state["controls"]["play"]["name"],"Play audio")
        self.assertIn("0:06",state["controls"]["time"]["text"])
        time.sleep(.5); paints=row("music")["metrics"]["renders"]; cpu=ticks(self.pid)
        time.sleep(.6); self.assertEqual(row("music")["metrics"]["renders"],paints); self.assertLessEqual(ticks(self.pid)-cpu,3)
        client.rpc({"method":"seek","id":"music","seconds":2.0})
        eventually(lambda:abs(row("music")["metrics"].get("position",0)-2)<.2)
        self.assertTrue(row("music")["metrics"]["paused"])
        client.rpc({"method":"settings","id":"music","settings":{"paused":False}})
        eventually(lambda:row("music")["metrics"].get("position",0)>2.3)
        eventually(lambda:row("music")["metrics"].get("audio_output")=="null")
        client.rpc({"method":"settings","id":"music","settings":{"muted":True}})
        eventually(lambda:row("music")["metrics"]["muted"] is True)
        client.rpc({"method":"settings","id":"music","settings":{"muted":False,"paused":True}})
        eventually(lambda:row("music")["metrics"]["muted"] is False and row("music")["metrics"]["paused"] is True)
        initial=row("music")["metrics"]["loads"]; revision=row("music")["revisions"]
        audio(asset,duration=3,frequency=660)
        eventually(lambda:row("music")["metrics"]["loads"]>initial,timeout=20)
        eventually(lambda:row("music")["status"].startswith("Live"))
        self.assertGreater(row("music")["revisions"],revision)
        self.assertAlmostEqual(row("music")["metrics"]["duration"],3,delta=.05)
        self.assertTrue(row("music")["metrics"]["paused"])
        self.assertFalse(row("music")["metrics"]["muted"])
        eventually(lambda:row("music")["metrics"]["waveform"]["ready"])
        good=row("music")["metrics"]["loads"]; revision=row("music")["revisions"]
        asset.write_bytes(b"unfinished audio output")
        eventually(lambda:row("music")["status"].startswith("Waiting"),timeout=20)
        self.assertEqual(row("music")["metrics"]["loads"],good)
        self.assertEqual(row("music")["revisions"],revision)
        self.assertTrue(row("music")["metrics"]["engine"])
        self.assertTrue(row("music")["metrics"]["waveform"]["ready"])
        self.assertAlmostEqual(row("music")["metrics"]["waveform"]["duration"],3,delta=.05)
        self.assertAlmostEqual(row("music")["metrics"]["duration"],3,delta=.05)
        audio(asset,duration=2,frequency=330)
        eventually(lambda:row("music")["metrics"]["loads"]>good,timeout=20)
        client.rpc({"method":"settings","id":"music","settings":{"loop":True,"paused":False}})
        client.rpc({"method":"seek","id":"music","seconds":1.75})
        eventually(lambda:0<row("music")["metrics"].get("position",99)<1,timeout=6)
        self.assertFalse(row("music")["metrics"].get("paused"))
        client.rpc({"method":"settings","id":"music","settings":{"loop":False}})
        client.rpc({"method":"seek","id":"music","seconds":1.75})
        eventually(lambda:row("music")["metrics"].get("eof") is True,timeout=6)
        eventually(lambda:row("music")["settings"].get("paused") is True)
        client.rpc({"method":"settings","id":"music","settings":{"paused":False}})
        eventually(lambda:row("music")["metrics"].get("eof") is False and 0<row("music")["metrics"].get("position",99)<1,timeout=6)
        client.rpc({"method":"settings","id":"music","settings":{"paused":True}})
        if os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR"):
            target=Path(os.environ["ASSET_PREVIEW_EVIDENCE_DIR"]); target.mkdir(parents=True,exist_ok=True)
            output=target/"audio.png"; output.unlink(missing_ok=True)
            client.rpc({"method":"capture","path":str(output)})
            (target/"audio.json").write_text(json.dumps(row("music"),indent=2))
        client.rpc({"method":"select","id":"audio-next"})
        self.assertNotIn("metrics",row("music")); self.assertFalse(row("music")["active"])
        client.rpc({"method":"select","id":"music"})
        eventually(lambda:row("music").get("metrics",{}).get("loads",0)==1,timeout=20)
        client.rpc({"method":"hide"}); self.assertNotIn("metrics",row("music"))
        self.assertEqual(client.rpc({"method":"list"})["active"],0)
        self.assertEqual(client.rpc({"method":"list"})["watched_files"],0)

    def test_44_audio_common_formats_and_waiting_output(self):
        asset=self.root/"audio-not-created.wav"
        client.rpc({"method":"add","entry":{"id":"later-audio","path":str(asset)}})
        self.show(); self.assertFalse(row("later-audio")["controls"]["play"]["enabled"])
        audio(asset,duration=1)
        eventually(lambda:row("later-audio").get("metrics",{}).get("loads",0)==1,timeout=20)
        for extension in ("mp3","flac","ogg","m4a","opus"):
            output=self.root/f"encoded.{extension}"
            subprocess.run(["ffmpeg","-nostdin","-loglevel","error","-y","-i",str(asset),str(output)],check=True,capture_output=True)
            client.rpc({"method":"add","entry":{"id":f"encoded-{extension}","path":str(output)}})
            client.rpc({"method":"select","id":f"encoded-{extension}"})
            eventually(lambda:row(f"encoded-{extension}").get("metrics",{}).get("loads",0)==1,timeout=20)
            self.assertAlmostEqual(row(f"encoded-{extension}")["metrics"]["duration"],1,delta=.1)
            self.assertTrue(row(f"encoded-{extension}")["metrics"]["paused"])
            eventually(lambda:row(f"encoded-{extension}")["metrics"]["waveform"]["ready"])

    @unittest.skipUnless(GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland","Native audio controls and mixed hardware grid")
    def test_45_audio_native_input_grid_and_hardware_coexistence(self):
        asset=self.root/"music-controls.wav"; audio(asset,duration=12)
        client.rpc({"method":"add","entry":{"id":"music-controls","path":str(asset),"label":"Music controls"}})
        self.show(); eventually(lambda:row("music-controls").get("metrics",{}).get("loads",0)==1,timeout=20)
        eventually(lambda:row("music-controls")["controls"]["timeline"].get("waveform"))
        input=XInput()
        try:
            def click(name,fraction=.5):
                control=row("music-controls")["controls"][name]
                input.click(control["x"]+round(control["width"]*fraction),control["y"]+control["height"]//2)
            def check_playhead():
                state=row("music-controls"); control=state["controls"]["timeline"]
                x=round(control["x"]+5+(control["width"]-10)*control["position"]/control["duration"])
                capture=self.root/"audio-waveform-playhead.png"
                capture.unlink(missing_ok=True)
                client.rpc({"method":"capture","path":str(capture)})
                self.assertTrue(any(min(png_pixel(capture,x+dx,control["y"]+8))>190 for dx in (-1,0,1)),
                                "Waveform playhead does not match the timeline position")
            click("play"); eventually(lambda:row("music-controls")["metrics"].get("paused") is False)
            eventually(lambda:row("music-controls")["metrics"].get("position",0)>.2)
            self.assertFalse(row("music-controls")["metrics"]["muted"])
            self.assertEqual(row("music-controls")["metrics"]["audio_output"],"null")
            click("play"); eventually(lambda:row("music-controls")["metrics"].get("paused") is True)
            click("timeline",.5); eventually(lambda:abs(row("music-controls")["metrics"].get("position",0)-6)<.3)
            check_playhead()
            control=row("music-controls")["controls"]["timeline"]
            input.motion(control["x"]+control["width"]//2,control["y"]+control["height"]//2)
            input.xt.XTestFakeButtonEvent(input.display,1,1,0)
            input.motion(control["x"]+round(control["width"]*.75),control["y"]+control["height"]//2)
            input.x.XFlush(input.display)
            eventually(lambda:row("music-controls")["controls"]["timeline"]["dragging"])
            self.assertAlmostEqual(row("music-controls")["metrics"]["position"],6,delta=.3)
            self.assertAlmostEqual(row("music-controls")["controls"]["timeline"]["position"],9,delta=.3)
            check_playhead()
            input.xt.XTestFakeButtonEvent(input.display,1,0,0); input.x.XFlush(input.display)
            eventually(lambda:abs(row("music-controls")["metrics"].get("position",0)-9)<.3)
            check_playhead()
            click("restart"); eventually(lambda:row("music-controls")["metrics"].get("position",99)<.1)
            click("mute"); eventually(lambda:row("music-controls")["metrics"]["muted"] is True)
            view=row("music-controls")["viewport"]
            input.click(view["x"]+view["width"]//2,view["y"]+view["height"]//2)
            input.press("m"); eventually(lambda:row("music-controls")["metrics"]["muted"] is False)
            input.chord("Shift_L","Right"); eventually(lambda:abs(row("music-controls")["metrics"].get("position",0)-5)<.3)
            input.press("Home"); eventually(lambda:row("music-controls")["metrics"].get("position",99)<.1)
            input.press("space"); eventually(lambda:row("music-controls")["metrics"]["paused"] is False)
            input.press("space"); eventually(lambda:row("music-controls")["metrics"]["paused"] is True)
            obj=self.root/"audio-grid-cube.obj"
            subprocess.run([sys.executable,str(ROOT/"examples/generate_cube.py"),str(obj)],check=True,capture_output=True)
            client.rpc({"method":"add","entry":{"id":"audio-grid-model","path":str(obj)}})
            self.add("audio-grid-image")
            client.rpc({"method":"add","entry":{"id":"second-music","path":str(asset)}})
            client.rpc({"method":"layout","layout":"grid"})
            eventually(lambda:row("audio-grid-model").get("metrics",{}).get("engine"),timeout=30)
            renderer=row("audio-grid-model")["metrics"]["renderer"]
            self.assertFalse(any(s in renderer.lower() for s in ("llvmpipe","softpipe","software","unknown")),renderer)
            eventually(lambda:row("second-music").get("metrics",{}).get("loads",0)==1,timeout=20)
            self.assertEqual(client.rpc({"method":"list"})["active"],4)
            self.assertTrue(row("music-controls")["overlays"]["playback"]["visible"])
            self.assertTrue(row("music-controls")["controls"]["play"]["visible"])
            self.assertTrue(row("music-controls")["controls"]["timeline"]["visible"])
            if os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR"):
                target=Path(os.environ["ASSET_PREVIEW_EVIDENCE_DIR"]); target.mkdir(parents=True,exist_ok=True)
                output=target/"audio-grid.png"; output.unlink(missing_ok=True)
                client.rpc({"method":"capture","path":str(output)})
                (target/"audio-grid.json").write_text(json.dumps(client.rpc({"method":"list"}),indent=2))
            self.add("audio-other-page")
            client.rpc({"method":"select","id":"audio-other-page"})
            for key in ("music-controls","second-music","audio-grid-model"):
                self.assertFalse(row(key)["active"]); self.assertNotIn("metrics",row(key))
        finally: input.close()

    def test_46_audio_visualizer_tracks_signal_and_silence(self):
        asset=self.root/"reactive-music.wav"
        rate=48000
        # Silence, bass, quiet bass, treble, left-only, right-only, silence.
        segments=[(0,0,0),(100,.25,.25),(100,.025,.025),(5000,.25,.25),
                  (400,.25,0),(400,0,.25),(0,0,0)]
        frames=bytearray()
        for frequency,left,right in segments:
            for i in range(rate*2):
                sample=32767*math.sin(2*math.pi*frequency*i/rate)
                frames.extend(struct.pack("<hh",int(sample*left),int(sample*right)))
        with wave.open(str(asset),"wb") as output:
            output.setparams((2,2,rate,0,"NONE","not compressed")); output.writeframes(frames)
        client.rpc({"method":"add","entry":{"id":"reactive","path":str(asset),"settings":{"muted":True}}})
        self.show(); eventually(lambda:row("reactive").get("metrics",{}).get("loads")==1,timeout=20)
        client.rpc({"method":"settings","id":"reactive","settings":{"paused":False}})

        def measure(at,predicate):
            client.rpc({"method":"seek","id":"reactive","seconds":at})
            eventually(lambda:at+.15<row("reactive")["metrics"].get("position",0)<at+1.3)
            def ready():
                measured=row("reactive")["metrics"]["visualizer"]
                return measured if measured["available"] and predicate(measured) else None
            return eventually(ready)

        def capture(name):
            if os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR"):
                target=Path(os.environ["ASSET_PREVIEW_EVIDENCE_DIR"]); target.mkdir(parents=True,exist_ok=True)
                path=target/f"reactive-{name}.png"; path.unlink(missing_ok=True)
                client.rpc({"method":"capture","path":str(path)})
                (target/f"reactive-{name}.json").write_text(json.dumps(row("reactive"),indent=2))

        quiet=measure(.2,lambda m:max(m["rms_db"])<-85)
        self.assertLess(quiet["energy"],.01)
        capture("silence")
        bass=measure(2.2,lambda m:m["rms_db"][0]>-20 and m["centroid_hz"]<200)
        self.assertGreater(bass["energy"],.35)
        capture("bass")
        soft=measure(4.2,lambda m:-40<m["rms_db"][0]<-30)
        self.assertGreater(bass["rms_db"][0]-soft["rms_db"][0],18)
        high=measure(6.2,lambda m:m["centroid_hz"]>4000)
        self.assertGreater(high["centroid_hz"],bass["centroid_hz"]*20)
        capture("treble")
        # Muting the output must preserve source-driven visuals and decoding.
        client.rpc({"method":"settings","id":"reactive","settings":{"muted":False}})
        eventually(lambda:row("reactive")["metrics"].get("muted") is False)
        self.assertAlmostEqual(row("reactive")["metrics"]["visualizer"]["rms_db"][0],high["rms_db"][0],delta=.1)
        left=measure(8.2,lambda m:m["rms_db"][0]>-20 and m["rms_db"][1]<-85)
        self.assertGreater(left["updates"],high["updates"])
        capture("left")
        measure(10.2,lambda m:m["rms_db"][1]>-20 and m["rms_db"][0]<-85)
        measure(12.2,lambda m:max(m["rms_db"])<-85 and m["energy"]<.01)
        client.rpc({"method":"settings","id":"reactive","settings":{"paused":True}})
        eventually(lambda:row("reactive")["metrics"].get("paused") is True)
        # Allow queued transport/layout changes to settle before measuring idle.
        time.sleep(.7)
        paused=row("reactive")["metrics"]
        time.sleep(.6)
        still=row("reactive")["metrics"]
        self.assertEqual(still["renders"],paused["renders"])
        self.assertEqual(still["visualizer"]["updates"],paused["visualizer"]["updates"])
        capture("paused")
        client.rpc({"method":"settings","id":"reactive","settings":{"background":"light"}})
        capture("light")
        client.rpc({"method":"hide"})
        self.assertNotIn("metrics",row("reactive"))

    @unittest.skipUnless(GPU,"Hardware Gamescope video lane")
    def test_18_video_playback_pause_seek_reload_failure_and_unload(self):
        samples={"before_video":memory(self.pid)}
        asset=self.root/"clip.mp4"; video(asset,duration=12)
        client.rpc({"method":"add","entry":{"id":"clip","path":str(asset)}})
        self.add("image-after-video"); self.show()
        eventually(lambda:row("clip").get("metrics",{}).get("loads",0)==1,timeout=30)
        eventually(lambda:row("clip")["metrics"].get("muted") is True)
        renderer=row("clip")["metrics"]["renderer"]
        self.assertFalse(any(s in renderer.lower() for s in ("llvmpipe","softpipe","software","unknown")),renderer)
        self.assertEqual((row("clip")["metrics"]["width"],row("clip")["metrics"]["height"]),(320,180))
        renders=row("clip")["metrics"]["renders"]
        eventually(lambda:row("clip")["metrics"]["renders"]>renders+3)
        samples["playing"]=memory(self.pid)
        client.rpc({"method":"settings","id":"clip","settings":{"paused":True}})
        eventually(lambda:row("clip")["metrics"]["paused"] is True)
        # Let the finite 700 ms control concealment finish before measuring
        # paused rendering. Uncovering the native video can paint once.
        idle_input=XInput()
        try:
            view=row("clip")["viewport"]
            idle_input.motion(view["x"]+view["width"]//2,view["y"]+view["height"]//2)
            idle_input.x.XFlush(idle_input.display)
            eventually(lambda:not row("clip")["overlays"]["playback"]["visible"])
        finally:idle_input.close()
        time.sleep(.2); state=row("clip"); paints=state["metrics"]["renders"]; cpu=ticks(self.pid)
        time.sleep(.9)
        self.assertEqual(row("clip")["metrics"]["renders"],paints)
        self.assertLessEqual(ticks(self.pid)-cpu,3)
        samples["paused"]={**memory(self.pid),"extra_paints":row("clip")["metrics"]["renders"]-paints,"cpu_ticks":ticks(self.pid)-cpu,"idle_seconds":.9}
        client.rpc({"method":"seek","id":"clip","seconds":2})
        eventually(lambda:abs(row("clip")["metrics"].get("position",0)-2)<.2)
        with self.assertRaises(RuntimeError): client.rpc({"method":"seek","id":"clip","seconds":-1})
        # Controls work on the actual native child and retain global navigation.
        input=XInput()
        try:
            view=row("clip")["viewport"]; input.click(view["x"]+view["width"]//2,view["y"]+view["height"]//2)
            time.sleep(.15)
            input.key("space",1); input.key("space",0); input.x.XFlush(input.display)
            eventually(lambda:row("clip")["metrics"]["paused"] is False)
            client.rpc({"method":"settings","id":"clip","settings":{"paused":True}})
            input.key("Home",1); input.key("Home",0); input.x.XFlush(input.display)
            eventually(lambda:row("clip")["metrics"]["position"]<.2)
            input.key("Shift_L",1); input.press("Right"); input.key("Shift_L",0); input.x.XFlush(input.display)
            eventually(lambda:abs(row("clip")["metrics"]["position"]-5)<.2)
            self.assertEqual(client.rpc({"method":"list"})["selected"],"clip")
            input.key("Shift_L",1); input.press("Left"); input.key("Shift_L",0); input.x.XFlush(input.display)
            eventually(lambda:row("clip")["metrics"]["position"]<.2)
            input.press("Right"); eventually(lambda:client.rpc({"method":"list"})["selected"]=="image-after-video")
            self.assertNotIn("metrics",row("clip"))
            input.press("Left"); eventually(lambda:client.rpc({"method":"list"})["selected"]=="clip")
            eventually(lambda:row("clip").get("metrics",{}).get("loads",0)==1,timeout=20)
            # Navigation into a video leaves its keyboard controls usable,
            # including after an arrow at the first-preview boundary.
            input.press("Left")
            input.key("Shift_L",1); input.press("Right"); input.key("Shift_L",0); input.x.XFlush(input.display)
            eventually(lambda:abs(row("clip")["metrics"]["position"]-5)<.2)
            input.press("space"); eventually(lambda:row("clip")["metrics"]["paused"] is False)
            input.press("space"); eventually(lambda:row("clip")["metrics"]["paused"] is True)
            input.press("Home"); eventually(lambda:row("clip")["metrics"]["position"]<.2)
            input.press("m"); eventually(lambda:row("clip")["metrics"]["muted"] is False)
            input.press("m"); eventually(lambda:row("clip")["metrics"]["muted"] is True)
        finally: input.close()
        # Same watched filename, atomically replaced with a visibly different clip.
        initial=row("clip")["metrics"]["loads"]; video(asset,"color=c=red:size=320x180:rate=12")
        eventually(lambda:row("clip")["metrics"]["loads"]>initial,timeout=20)
        eventually(lambda:row("clip")["status"].startswith("Live"))
        snapshot=self.root/"video-frame.png"; client.rpc({"method":"capture","path":str(snapshot)})
        if os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR"):
            target=Path(os.environ["ASSET_PREVIEW_EVIDENCE_DIR"]).resolve(); target.mkdir(parents=True,exist_ok=True)
            (target/"video-red.png").write_bytes(snapshot.read_bytes())
            state=row("clip")
            (target/"video-red.json").write_text(json.dumps({k:state[k] for k in ("kind","status","settings","metrics")},indent=2))
        view=row("clip")["viewport"]; color=png_pixel(snapshot,view["x"]+view["width"]//2,view["y"]+view["height"]//2)
        self.assertGreater(color[0],180); self.assertLess(max(color[1:]),60)
        good=row("clip")["metrics"]["loads"]
        asset.write_bytes(b"unfinished video save")
        eventually(lambda:row("clip")["status"].startswith("Waiting"),timeout=20)
        self.assertEqual(row("clip")["metrics"]["loads"],good)
        self.assertTrue(row("clip")["metrics"]["engine"])
        video(asset,"color=c=blue:size=320x180:rate=12")
        eventually(lambda:row("clip")["metrics"]["loads"]>good,timeout=20)
        client.rpc({"method":"add","entry":{"id":"clip-grid","path":str(asset),"settings":{"paused":False}}})
        client.rpc({"method":"layout","layout":"grid"})
        eventually(lambda:row("clip-grid").get("metrics",{}).get("loads",0)==1)
        second=row("clip-grid")["metrics"]["renders"]
        eventually(lambda:row("clip-grid")["metrics"]["renders"]>second+2)
        self.assertEqual(client.rpc({"method":"list"})["active"],3)
        # Test mute, loop-off EOF and seek without producing audible automation.
        client.rpc({"method":"settings","id":"clip","settings":{"muted":False,"paused":True}})
        eventually(lambda:row("clip")["metrics"]["muted"] is False)
        client.rpc({"method":"settings","id":"clip","settings":{"muted":True,"loop":False,"paused":False}})
        client.rpc({"method":"seek","id":"clip","seconds":3.6})
        eventually(lambda:row("clip")["metrics"].get("eof") is True)
        eventually(lambda:row("clip")["settings"].get("paused") is True)
        evidence=os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR")
        if evidence:
            target=Path(evidence).resolve(); target.mkdir(parents=True,exist_ok=True)
            output=target/"video.png"; output.unlink(missing_ok=True)
            client.rpc({"method":"capture","path":str(output)})
            state=row("clip")
            (target/"video.json").write_text(json.dumps({k:state[k] for k in ("kind","status","settings","metrics")},indent=2))
        for i in range(4): self.add(f"video-offpage-{i}")
        client.rpc({"method":"select","id":"video-offpage-3"})
        self.assertNotIn("metrics",row("clip")); self.assertNotIn("metrics",row("clip-grid"))
        client.rpc({"method":"hide"}); self.assertNotIn("metrics",row("clip"))
        self.assertEqual(client.rpc({"method":"list"})["active"],0)
        cpu=ticks(self.pid); time.sleep(.8); self.assertLessEqual(ticks(self.pid)-cpu,2)
        samples["hidden"]={**memory(self.pid),"cpu_ticks":ticks(self.pid)-cpu,"idle_seconds":.8}
        if evidence: (target/"resources.json").write_text(json.dumps(samples,indent=2))

    @unittest.skipUnless(GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland","Native video controls and timeline input")
    def test_41_video_controls_timeline_input_and_responsive_layout(self):
        asset=self.root/"video-controls.mp4"
        client.rpc({"method":"add","entry":{"id":"controls","path":str(asset),"settings":{"paused":True,"muted":True,"loop":True}}})
        self.show()
        eventually(lambda:"timeline" in row("controls").get("controls",{}))
        self.assertFalse(row("controls")["controls"]["play"]["enabled"])
        self.assertFalse(row("controls")["controls"]["timeline"]["enabled"])
        self.assertEqual(row("controls")["controls"]["time"]["text"],"0:00 / --:--")
        video(asset,duration=12)
        eventually(lambda:row("controls").get("metrics",{}).get("loads",0)==1,timeout=30)
        eventually(lambda:row("controls")["controls"]["timeline"]["enabled"])
        renderer=row("controls")["metrics"]["renderer"]
        self.assertFalse(any(s in renderer.lower() for s in ("llvmpipe","softpipe","software","unknown")),renderer)
        self.assertEqual(row("controls")["controls"]["time"]["text"],"0:00 / 0:12")
        input=XInput()
        def click(name):
            control=row("controls")["controls"][name]
            input.motion(control["x"]+control["width"]//2,control["y"]+control["height"]//2); input.x.XFlush(input.display)
            eventually(lambda:row("controls")["controls"][name]["visible"])
            control=row("controls")["controls"][name]
            self.assertTrue(control["visible"],name); self.assertTrue(control["enabled"],name)
            input.click(control["x"]+control["width"]//2,control["y"]+control["height"]//2)
        try:
            click("play"); eventually(lambda:row("controls")["metrics"]["paused"] is False)
            eventually(lambda:row("controls")["controls"]["timeline"]["position"]>.3)
            self.assertEqual(row("controls")["controls"]["play"]["name"],"Pause video")
            click("play"); eventually(lambda:row("controls")["metrics"]["paused"] is True)
            timeline=row("controls")["controls"]["timeline"]
            input.click(timeline["x"]+timeline["width"]*3//4,timeline["y"]+timeline["height"]//2)
            eventually(lambda:abs(row("controls")["metrics"]["position"]-9)<.2)
            # Held scrubbing previews its own time without seeking on every move
            # or snapping back when the player's position changes.
            timeline=row("controls")["controls"]["timeline"]
            input.motion(timeline["x"]+timeline["width"]//2,timeline["y"]+timeline["height"]//2)
            input.xt.XTestFakeButtonEvent(input.display,1,1,0); input.x.XFlush(input.display)
            eventually(lambda:row("controls")["controls"]["timeline"]["dragging"])
            eventually(lambda:abs(row("controls")["controls"]["timeline"]["position"]-6)<.2)
            self.assertAlmostEqual(row("controls")["metrics"]["position"],9,delta=.2)
            client.rpc({"method":"seek","id":"controls","seconds":8})
            eventually(lambda:abs(row("controls")["metrics"]["position"]-8)<.2)
            self.assertAlmostEqual(row("controls")["controls"]["timeline"]["position"],6,delta=.2)
            input.motion(timeline["x"]+timeline["width"]//4,timeline["y"]+timeline["height"]//2)
            input.x.XFlush(input.display)
            eventually(lambda:abs(row("controls")["controls"]["timeline"]["position"]-3)<.2)
            input.xt.XTestFakeButtonEvent(input.display,1,0,0); input.x.XFlush(input.display)
            eventually(lambda:abs(row("controls")["metrics"]["position"]-3)<.2)
            click("restart"); eventually(lambda:row("controls")["metrics"]["position"]<.2)
            click("mute"); eventually(lambda:row("controls")["metrics"]["muted"] is False)
            self.assertFalse(row("controls")["controls"]["mute"]["checked"])
            # Space activates the focused button; M still reaches the player.
            input.press("space"); eventually(lambda:row("controls")["metrics"]["muted"] is True)
            self.assertTrue(row("controls")["metrics"]["paused"])
            input.press("m"); eventually(lambda:row("controls")["metrics"]["muted"] is False)
            input.press("m"); eventually(lambda:row("controls")["metrics"]["muted"] is True)
            click("loop"); eventually(lambda:row("controls")["settings"]["loop"] is False)
            self.assertFalse(row("controls")["controls"]["loop"]["checked"])
            timeline=row("controls")["controls"]["timeline"]
            input.click(timeline["x"]+timeline["width"]//2,timeline["y"]+timeline["height"]//2)
            eventually(lambda:abs(row("controls")["metrics"]["position"]-6)<.2)
            input.press("Home"); eventually(lambda:row("controls")["metrics"]["position"]<.2)
            input.press("End"); eventually(lambda:row("controls")["metrics"]["position"]>11.8)
            self.assertTrue(row("controls")["metrics"]["paused"])
            click("play"); eventually(lambda:row("controls")["metrics"].get("eof") is True)
            eventually(lambda:row("controls")["metrics"]["paused"] is True)
            click("play"); eventually(lambda:row("controls")["metrics"]["paused"] is False)
            eventually(lambda:row("controls")["metrics"]["position"]<2)
            click("play"); eventually(lambda:row("controls")["metrics"]["paused"] is True)
            time.sleep(.5); state=row("controls"); cpu=ticks(self.pid)
            time.sleep(.7)
            self.assertEqual(row("controls")["metrics"]["renders"],state["metrics"]["renders"])
            self.assertEqual(row("controls")["controls"]["timeline"],state["controls"]["timeline"])
            self.assertLessEqual(ticks(self.pid)-cpu,3)
            input.x.XResizeWindow.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_uint,ctypes.c_uint]
            for i in range(8): self.add(f"controls-grid-{i}")
            client.rpc({"method":"layout","layout":"grid","grid_size":3})
            for width,height in ((1100,800),(540,380)):
                input.x.XResizeWindow(input.display,input.window,width,height); input.x.XFlush(input.display)
                time.sleep(.3); state=row("controls"); bar=state["overlays"]["playback"]
                for name in ("play","timeline"):
                    control=state["controls"][name]; self.assertTrue(control["visible"],name)
                    self.assertGreaterEqual(control["x"],bar["x"])
                    self.assertGreaterEqual(control["y"],bar["y"])
                    self.assertLessEqual(control["x"]+control["width"],bar["x"]+bar["width"])
                    self.assertLessEqual(control["y"]+control["height"],bar["y"]+bar["height"])
                click("play"); eventually(lambda:row("controls")["metrics"]["paused"] is False)
                click("play"); eventually(lambda:row("controls")["metrics"]["paused"] is True)
            # Navigation still owns plain arrows while the timeline has focus.
            click("timeline"); input.press("Right")
            self.assertEqual(client.rpc({"method":"list"})["selected"],"controls")
            client.rpc({"method":"hide"})
            self.assertNotIn("metrics",row("controls")); self.assertNotIn("controls",row("controls"))
        finally: input.close()

    @unittest.skipUnless(GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland","Native fullscreen and video inactivity input")
    def test_46_fullscreen_video_inactivity_and_exit_notice(self):
        asset=self.root/"fullscreen.mp4"; video(asset,duration=12)
        client.rpc({"method":"add","entry":{"id":"fullscreen-video","path":str(asset),
            "settings":{"paused":True,"muted":True,"loop":True}}})
        for i in range(3): self.add(f"fullscreen-image-{i}")
        client.rpc({"method":"layout","layout":"grid"})
        self.show()
        eventually(lambda:row("fullscreen-video").get("metrics",{}).get("loads",0)==1,timeout=25)
        renderer=row("fullscreen-video")["metrics"]["renderer"]
        self.assertFalse(any(s in renderer.lower() for s in ("llvmpipe","softpipe","software","unknown")),renderer)
        input=XInput()
        def state(): return client.rpc({"method":"list"})["presentation"]
        def center():
            view=row("fullscreen-video")["viewport"]
            return view["x"]+view["width"]//2,view["y"]+view["height"]//2
        def move():
            input.motion(*center()); input.x.XFlush(input.display)
        def click(name):
            control=row("fullscreen-video")["controls"][name]
            input.motion(control["x"]+control["width"]//2,control["y"]+control["height"]//2); input.x.XFlush(input.display)
            eventually(lambda:row("fullscreen-video")["controls"][name]["visible"])
            control=row("fullscreen-video")["controls"][name]
            input.click(control["x"]+control["width"]//2,control["y"]+control["height"]//2)
        def capture(name):
            if os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR"):
                target=Path(os.environ["ASSET_PREVIEW_EVIDENCE_DIR"]).resolve(); target.mkdir(parents=True,exist_ok=True)
                path=target/(name+".png"); path.unlink(missing_ok=True)
                client.rpc({"method":"capture","path":str(path)})
                if name=="fullscreen-notice":
                    window=state()
                    subprocess.run(["ffmpeg","-nostdin","-loglevel","error","-y","-f","x11grab",
                        "-window_id",str(input.window),"-draw_mouse","0",
                        "-video_size",f'{window["width"]}x{window["height"]}',"-i",os.environ["DISPLAY"],
                        "-frames:v","1",str(target/"fullscreen-native.png")],check=True,capture_output=True)
        def fullscreen(value): eventually(lambda:state()["fullscreen"]==value)
        try:
            move()
            eventually(lambda:client.rpc({"method":"list"})["selected"]=="fullscreen-video")
            idle_started=time.monotonic()
            eventually(lambda:not row("fullscreen-video")["overlays"]["playback"]["visible"],timeout=1.3)
            self.assertLess(time.monotonic()-idle_started,1.3)
            self.assertFalse(row("fullscreen-video")["overlays"]["name"]["visible"])
            # Paused inactivity settles without a recurring render/CPU timer.
            time.sleep(.2); renders=row("fullscreen-video")["metrics"]["renders"]; cpu=ticks(self.pid)
            time.sleep(.7)
            self.assertEqual(row("fullscreen-video")["metrics"]["renders"],renders)
            self.assertLessEqual(ticks(self.pid)-cpu,3)
            input.motion(center()[0]+4,center()[1]); input.x.XFlush(input.display)
            eventually(lambda:row("fullscreen-video")["controls"]["play"]["visible"])
            # Hovering controls, a held seek, and keyboard focus keep them usable.
            play=row("fullscreen-video")["controls"]["play"]
            input.motion(play["x"]+play["width"]//2,play["y"]+play["height"]//2); input.x.XFlush(input.display)
            time.sleep(2.8); self.assertTrue(row("fullscreen-video")["controls"]["play"]["visible"])
            timeline=row("fullscreen-video")["controls"]["timeline"]
            input.motion(timeline["x"]+timeline["width"]//4,timeline["y"]+timeline["height"]//2)
            input.xt.XTestFakeButtonEvent(input.display,1,1,0); input.x.XFlush(input.display)
            eventually(lambda:row("fullscreen-video")["controls"]["timeline"]["dragging"])
            input.motion(*center()); input.x.XFlush(input.display)
            time.sleep(2.8); self.assertTrue(row("fullscreen-video")["controls"]["timeline"]["dragging"])
            input.xt.XTestFakeButtonEvent(input.display,1,0,0); input.x.XFlush(input.display)
            eventually(lambda:not row("fullscreen-video")["controls"]["timeline"]["dragging"])
            click("play"); eventually(lambda:row("fullscreen-video")["metrics"]["paused"] is False)
            click("play"); eventually(lambda:row("fullscreen-video")["metrics"]["paused"] is True)
            move()
            for _ in range(12):
                input.press("Tab"); time.sleep(.05)
                if any(control.get("focused") for name,control in row("fullscreen-video")["controls"].items()
                       if name in ("play","restart","timeline","mute","loop","fullscreen")): break
            self.assertTrue(any(control.get("focused") for control in row("fullscreen-video")["controls"].values()))
            time.sleep(2.8)
            self.assertTrue(row("fullscreen-video")["controls"]["play"]["visible"])
            input.motion(center()[0]+5,center()[1]); input.x.XFlush(input.display)
            eventually(lambda:not row("fullscreen-video")["overlays"]["playback"]["visible"],timeout=5)
            # A native double click opens just this video and does not pause/seek/reload it.
            client.rpc({"method":"seek","id":"fullscreen-video","seconds":3})
            eventually(lambda:abs(row("fullscreen-video")["metrics"]["position"]-3)<.2)
            before=row("fullscreen-video")["metrics"]
            windowed=state()
            input.click(*center()); time.sleep(.08); input.click(*center())
            fullscreen(True)
            eventually(lambda:state()["single_preview"] and client.rpc({"method":"list"})["active"]==1)
            self.assertEqual(client.rpc({"method":"list"})["layout"],"grid")
            self.assertFalse(row("fullscreen-video")["overlays"]["playback"]["visible"])
            self.assertFalse(row("fullscreen-video")["overlays"]["name"]["visible"])
            self.assertFalse(state()["menu_visible"]); self.assertFalse(state()["toolbar_visible"])
            self.assertEqual(row("fullscreen-video")["metrics"]["loads"],before["loads"])
            self.assertTrue(row("fullscreen-video")["metrics"]["paused"])
            self.assertAlmostEqual(row("fullscreen-video")["metrics"]["position"],3,delta=.2)
            view=row("fullscreen-video")["viewport"]
            self.assertGreaterEqual(view["width"],state()["width"]-2)
            self.assertGreaterEqual(view["height"],state()["height"]-2)
            eventually(lambda:state()["notice"]["visible"])
            capture("fullscreen-notice")
            top=state()["notice"]["y"]
            eventually(lambda:state()["notice"]["sliding"],timeout=4)
            eventually(lambda:state()["notice"]["y"]<top,timeout=1)
            capture("fullscreen-notice-exit")
            eventually(lambda:not state()["notice"]["visible"])
            eventually(lambda:not row("fullscreen-video")["overlays"]["playback"]["visible"],timeout=5)
            capture("fullscreen-idle")
            input.press("Escape"); fullscreen(False)
            eventually(lambda:(state()["width"],state()["height"])==(windowed["width"],windowed["height"]))
            eventually(lambda:client.rpc({"method":"list"})["active"]==4)
            self.assertTrue(state()["toolbar_visible"])
            self.assertEqual(row("fullscreen-video")["metrics"]["loads"],before["loads"])
            # The video button enters fullscreen; double click also exits it.
            move(); click("fullscreen"); fullscreen(True)
            move(); input.click(*center()); time.sleep(.08); input.click(*center()); fullscreen(False)
            # F11 applies to the whole grid and works from a native video child.
            move(); input.press("F11"); fullscreen(True)
            self.assertFalse(state()["single_preview"])
            self.assertEqual(client.rpc({"method":"list"})["active"],4)
            self.assertFalse(row("fullscreen-video")["overlays"]["playback"]["visible"])
            input.press("F11"); fullscreen(False)
            # The same shortcut and exit notice work on an image without media.
            client.rpc({"method":"layout","layout":"single"})
            client.rpc({"method":"select","id":"fullscreen-image-0"})
            view=row("fullscreen-image-0")["viewport"]
            input.click(view["x"]+view["width"]//2,view["y"]+view["height"]//2)
            input.press("F11"); fullscreen(True)
            self.assertTrue(state()["notice"]["visible"])
            input.press("Escape"); fullscreen(False)
            time.sleep(.3); self.assertFalse(state()["notice"]["visible"])
            # Returning to the baseline size/layout retains ordinary overlays.
            client.rpc({"method":"select","id":"fullscreen-video"})
            eventually(lambda:row("fullscreen-video").get("metrics",{}).get("loads",0)==1,timeout=25)
            move(); capture("after")
            client.rpc({"method":"hide"})
            self.assertEqual(client.rpc({"method":"list"})["active"],0)
            self.assertFalse(state()["notice"]["visible"])
        finally:
            input.close()

    @unittest.skipUnless(GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland","Native video close-button teardown")
    def test_47_close_missing_failed_and_loaded_video_under_pointer(self):
        self.add("close-video-survivor")
        self.show()
        input=XInput()
        valid=self.root/"close-loaded.mp4"; video(valid,duration=12)
        corrupt=self.root/"close-corrupt.mp4"; corrupt.write_bytes(b"incomplete video output")
        paths={"missing":self.root/"close-missing.mp4","corrupt":corrupt,"loaded":valid}
        try:
            for layout in ("single","grid"):
                client.rpc({"method":"layout","layout":layout})
                for kind,path in paths.items():
                    key=f"close-{layout}-{kind}"
                    with self.subTest(layout=layout,video=kind):
                        client.rpc({"method":"add","entry":{"id":key,"path":str(path),
                            "settings":{"paused":True,"muted":True}}})
                        client.rpc({"method":"select","id":key})
                        # Even a missing source owns a native OpenGL window.
                        eventually(lambda:row(key).get("metrics",{}).get("renderer"),timeout=20)
                        if kind=="loaded":
                            eventually(lambda:row(key)["metrics"]["loads"]==1,timeout=20)
                            renderer=row(key)["metrics"]["renderer"]
                            self.assertFalse(any(s in renderer.lower() for s in ("llvmpipe","softpipe","software","unknown")),renderer)
                        else:
                            eventually(lambda:row(key)["status"].startswith("Waiting"),timeout=20)
                            self.assertFalse(row(key)["metrics"]["engine"])
                        # Click the real top-right close button and leave the
                        # pointer there while Qt destroys its native children.
                        overlay=row(key)["overlays"]["controls"]
                        self.assertTrue(overlay["visible"])
                        input.click(overlay["x"]+overlay["width"]-20,overlay["y"]+overlay["height"]//2)
                        eventually(lambda:key not in {r["id"] for r in client.rpc({"method":"list"})["entries"]})
                        self.assertEqual(client.rpc({"method":"ping"})["pid"],self.pid)
                        self.assertTrue(row("close-video-survivor")["active"])
            # Pending load/retry callbacks also stay inert after removal/hide.
            client.rpc({"method":"hide"}); time.sleep(1.8)
            self.assertEqual(client.rpc({"method":"list"})["active"],0)
            self.assertEqual(client.rpc({"method":"ping"})["pid"],self.pid)
        finally:
            input.close()

    @unittest.skipUnless(GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland","Hardware Gamescope X11 input lane")
    def test_19_plain_arrows_navigate_grid_pages_and_clamp_at_edges(self):
        for i in range(6): self.add(f"page-{i}")
        client.rpc({"method":"layout","layout":"grid","grid_size":2})
        self.show()
        input=XInput()
        try:
            input.press("Right")
            eventually(lambda:client.rpc({"method":"list"})["selected"]=="page-4")
            self.assertEqual(client.rpc({"method":"list"})["active"],2)
            input.press("Right"); time.sleep(.1)
            self.assertEqual(client.rpc({"method":"list"})["selected"],"page-4")
            input.press("Left")
            eventually(lambda:client.rpc({"method":"list"})["selected"]=="page-0")
            self.assertEqual(client.rpc({"method":"list"})["active"],4)
            input.press("Left"); time.sleep(.1)
            self.assertEqual(client.rpc({"method":"list"})["selected"],"page-0")
            # A boundary arrow must preserve selections inside that page too.
            for selected,key in (("page-1","Left"),("page-5","Right")):
                client.rpc({"method":"select","id":selected})
                input.press(key); time.sleep(.1)
                self.assertEqual(client.rpc({"method":"list"})["selected"],selected)
        finally: input.close()

    def test_20_navigation_save_failure_preserves_selection(self):
        self.add("before-failure"); self.add("after-failure")
        self.show()
        eventually(lambda:row("before-failure")["viewport"]["height"]>100)
        height=row("before-failure")["viewport"]["height"]
        registry=self.root/"state/previews.json"; backup=registry.with_suffix(".backup")
        registry.rename(backup); registry.mkdir()
        try:
            with self.assertRaises(RuntimeError): client.rpc({"method":"select","id":"after-failure"})
            state=client.rpc({"method":"list"})
            self.assertEqual(state["selected"],"before-failure")
            self.assertEqual({e["id"] for e in state["entries"] if e["active"]},{"before-failure"})
            if GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland":
                input=XInput()
                try:
                    input.press("Right"); time.sleep(.2)
                    self.assertEqual(client.rpc({"method":"list"})["selected"],"before-failure")
                    # Save errors remain visible in the status bar.
                    self.assertEqual(row("before-failure")["viewport"]["height"],height)
                finally: input.close()
        finally:
            registry.rmdir(); backup.replace(registry)
        if GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland":
            input=XInput()
            try:
                input.press("Right")
                eventually(lambda:client.rpc({"method":"list"})["selected"]=="after-failure")
            finally: input.close()

    def test_32_close_returns_to_recent_preview(self):
        client.rpc({"method":"close_behavior","mode":"last"})
        self.addCleanup(lambda:client.rpc({"method":"close_behavior","mode":"left"}))
        for key in ("one","two","three","four","five"): self.add(key)
        client.rpc({"method":"select","id":"four"})
        client.rpc({"method":"select","id":"five"})
        client.rpc({"method":"remove","id":"five"})
        self.assertEqual(client.rpc({"method":"list"})["selected"],"four")
        client.rpc({"method":"select","id":"two"})
        client.rpc({"method":"remove","id":"three"})
        self.assertEqual(client.rpc({"method":"list"})["selected"],"two")
        client.rpc({"method":"remove","id":"two"})
        self.assertEqual(client.rpc({"method":"list"})["selected"],"four")
        client.rpc({"method":"remove","id":"four"})
        self.assertEqual(client.rpc({"method":"list"})["selected"],"one")
        client.rpc({"method":"remove","id":"one"})
        self.assertEqual(client.rpc({"method":"list"})["selected"],"")
        # No history after restart: choose a neighbor instead of entry one.
        for key in ("first","middle","last"): self.add(key)
        client.rpc({"method":"select","id":"last"})
        client.rpc({"method":"quit"}); eventually(lambda:not client.running()); client.start()
        client.rpc({"method":"remove","id":"last"})
        self.assertEqual(client.rpc({"method":"list"})["selected"],"middle")
        client.rpc({"method":"select","id":"first"})
        client.rpc({"method":"remove","id":"first"})
        self.assertEqual(client.rpc({"method":"list"})["selected"],"middle")

    def test_33_failed_close_preserves_view_and_history(self):
        self.add("close-before"); self.add("close-current"); self.show()
        client.rpc({"method":"select","id":"close-current"})
        eventually(lambda:row("close-current").get("metrics",{}).get("loads"))
        loads=row("close-current")["metrics"]["loads"]
        registry=self.root/"state/previews.json"; backup=registry.with_suffix(".backup")
        registry.rename(backup); registry.mkdir()
        try:
            with self.assertRaises(RuntimeError): client.rpc({"method":"remove","id":"close-current"})
            self.assertEqual(client.rpc({"method":"list"})["selected"],"close-current")
            self.assertEqual(row("close-current")["metrics"]["loads"],loads)
            self.assertTrue(row("close-current")["active"])
        finally: registry.rmdir(); backup.replace(registry)
        client.rpc({"method":"remove","id":"close-current"})
        self.assertEqual(client.rpc({"method":"list"})["selected"],"close-before")

    @unittest.skipUnless(GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland","Hardware camera persistence and reset")
    def test_34_camera_persistence_and_reset(self):
        obj=self.root/"saved-camera.obj"
        subprocess.run([sys.executable,str(ROOT/"examples/generate_cube.py"),str(obj)],check=True,capture_output=True)
        client.rpc({"method":"add","entry":{"id":"camera","path":str(obj)}})
        self.add("camera-other"); self.show()
        eventually(lambda:row("camera").get("metrics",{}).get("engine"))
        renderer=row("camera")["metrics"]["renderer"]
        self.assertFalse(any(s in renderer.lower() for s in ("llvmpipe","softpipe","software","unknown")),renderer)
        def check(saved):
            actual=row("camera")["metrics"]["camera"]
            for key in ("position","focal","up"):
                for a,b in zip(saved[key],actual[key]): self.assertAlmostEqual(a,b,places=6)
            if "parallel_span" in saved:
                self.assertAlmostEqual(saved["parallel_span"],actual["parallel_span"],places=6)
        for ortho in (False,True):
            client.rpc({"method":"settings","id":"camera","settings":{"orthographic":ortho}})
            time.sleep(.2)
            baseline_input=XInput()
            try:
                reset=row("camera")["controls"]["reset"]
                baseline_input.click(reset["x"]+reset["width"]//2,reset["y"]+reset["height"]//2)
                time.sleep(.2)
            finally: baseline_input.close()
            initial=row("camera")["metrics"]["camera"]
            input=XInput()
            try:
                view=row("camera")["viewport"]; x=view["x"]+view["width"]//2; y=view["y"]+view["height"]//2
                input.drag(x,y,60,35)
                eventually(lambda:bool(row("camera").get("camera")))
                input.key("Shift_L",1); input.drag(x,y,35,20); input.key("Shift_L",0); input.x.XFlush(input.display)
                time.sleep(.1); input.scroll(x,y)
                eventually(lambda:math.dist(initial["position"],row("camera")["camera"]["position"])>1e-4)
                time.sleep(.1); saved=row("camera")["camera"]
            finally: input.close()
            client.rpc({"method":"select","id":"camera-other"})
            client.rpc({"method":"select","id":"camera"})
            eventually(lambda:row("camera").get("metrics",{}).get("engine")); check(saved)
            self.assertAlmostEqual(row("camera")["camera"]["parallel_span"],saved["parallel_span"],places=6)
            client.rpc({"method":"add","entry":{"id":"camera","path":str(obj),"label":"Reconnected camera"}})
            eventually(lambda:row("camera").get("metrics",{}).get("engine")); check(saved)
            client.rpc({"method":"quit"}); eventually(lambda:not client.running()); client.start(); self.show()
            eventually(lambda:row("camera").get("metrics",{}).get("engine")); check(saved)
            input=XInput()
            try:
                time.sleep(.2); reset=row("camera")["controls"]["reset"]
                self.assertTrue(reset["visible"])
                input.click(reset["x"]+reset["width"]//2,reset["y"]+reset["height"]//2)
                eventually(lambda:math.dist(initial["position"],row("camera")["metrics"]["camera"]["position"])<1e-6)
                check(initial)
            finally: input.close()

    def test_35_camera_validation_and_default_isolation(self):
        camera={"position":[3,2,5],"focal":[0,0,0],"up":[0,1,0],"angle":30,"parallel_span":4}
        client.rpc({"method":"add","entry":{"id":"saved","path":str(self.root/"missing.obj"),"camera":camera}})
        self.assertEqual(row("saved")["camera"],camera)
        client.rpc({"method":"add","entry":{"id":"saved","path":str(self.root/"missing.obj")}})
        self.assertEqual(row("saved")["camera"],camera)
        client.rpc({"method":"add","entry":{"id":"fresh","path":str(self.root/"fresh.obj")}})
        self.assertNotIn("camera",row("fresh"))
        for bad in ({},dict(camera,up=[0,0,0]),dict(camera,position=[0,0,0]),dict(camera,parallel_span=-1),dict(camera,angle=180)):
            with self.assertRaises(RuntimeError):
                client.rpc({"method":"add","entry":{"id":"bad-camera","path":str(self.root/"bad.obj"),"camera":bad}})

    def test_36_reopen_restores_position_settings_and_save_failure(self):
        for key in ("reopen-first","reopen-middle","reopen-last"): self.add(key)
        client.rpc({"method":"select","id":"reopen-middle"})
        client.rpc({"method":"settings","id":"reopen-middle","settings":{"nearest":True}})
        client.rpc({"method":"remove","id":"reopen-middle"})
        registry=self.root/"state/previews.json"; backup=registry.with_suffix(".backup")
        registry.rename(backup); registry.mkdir()
        try:
            with self.assertRaises(RuntimeError): client.rpc({"method":"reopen"})
            self.assertNotIn("reopen-middle",[e["id"] for e in client.rpc({"method":"list"})["entries"]])
        finally: registry.rmdir(); backup.replace(registry)
        client.rpc({"method":"reopen"})
        state=client.rpc({"method":"list"})
        self.assertEqual([e["id"] for e in state["entries"]],["reopen-first","reopen-middle","reopen-last"])
        self.assertEqual(state["selected"],"reopen-middle")
        self.assertTrue(row("reopen-middle")["settings"]["nearest"])
        client.rpc({"method":"close_all"}); client.rpc({"method":"reopen"})
        self.assertEqual(client.rpc({"method":"list"})["selected"],"reopen-middle")

    @patch.dict(os.environ,{"CODEX_THREAD_ID":"","CODEX_SESSION_ID":""})
    def test_codex_origin_metadata_preserves_live_view(self):
        self.add("origin")
        self.show()
        eventually(lambda:row("origin").get("metrics",{}).get("loads"))
        original=row("origin"); loads=original["metrics"]["loads"]
        origin={"thread_id":"00000000-0000-4000-8000-000000000001","cwd":str(self.root),"socket":str(self.root/"codex.sock")}
        config={"id":"origin","path":str(self.root/"origin.png"),"codex":origin}
        for invalid in (dict(origin,thread_id="not-a-thread"),dict(origin,socket="relative.sock"),dict(origin,cwd="/tmp/escape\x1b")):
            with self.assertRaises(RuntimeError):client.rpc({"method":"add","entry":dict(config,codex=invalid)})
            self.assertNotIn("codex",row("origin"))
        client.rpc({"method":"add","entry":config})
        self.assertEqual(row("origin")["codex"],origin)
        self.assertEqual(row("origin")["metrics"]["loads"],loads)
        client.rpc({"method":"add","entry":{"id":"origin","path":str(self.root/"origin.png")}})
        self.assertEqual(row("origin")["codex"],origin)
        self.assertEqual(row("origin")["metrics"]["loads"],loads)
        other=dict(origin,thread_id="00000000-0000-4000-8000-000000000002")
        client.rpc({"method":"add","entry":dict(config,codex=other)})
        self.assertEqual(row("origin")["codex"],other)
        self.assertEqual(row("origin")["metrics"]["loads"],loads)

    @unittest.skipUnless(GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland","Native grid hover, image persistence and screenshots")
    def test_37_active_grid_image_shortcuts_and_screenshots(self):
        for key in ("grid-first","grid-second","grid-third"): self.add(key,settings={"background":"dark"})
        png(self.root/"grid-second.png", (220,70,80,80))
        client.rpc({"method":"layout","layout":"grid"}); self.show()
        eventually(lambda:row("grid-second").get("metrics",{}).get("loads"))
        input=XInput(); saved_files=[]
        try:
            time.sleep(.2); view=row("grid-second")["viewport"]; x=view["x"]+view["width"]//2; y=view["y"]+view["height"]//2
            loads={id:row(id)["metrics"]["loads"] for id in ("grid-first","grid-second","grid-third")}
            input.motion(x,y); input.x.XFlush(input.display)
            eventually(lambda:client.rpc({"method":"list"})["selected"]=="grid-second")
            time.sleep(.2); view=row("grid-second")["viewport"]; x=view["x"]+view["width"]//2; y=view["y"]+view["height"]//2
            for id,count in loads.items(): self.assertEqual(row(id)["metrics"]["loads"],count)
            self.assertTrue(row("grid-second")["selected"])
            input.motion(x+40,y); input.x.XFlush(input.display); time.sleep(.1)
            input.scroll(x+40,y); time.sleep(.5)
            self.assertGreater(row("grid-second")["metrics"]["image_view"]["zoom"],1,msg=str(row("grid-second")))
            input.drag(x,y,45,20)
            eventually(lambda:row("grid-second").get("image_view",{}).get("zoom",1)>1)
            time.sleep(.1); saved=row("grid-second")["image_view"]
            input.chord("Control_L","w")
            eventually(lambda:not any(e["id"]=="grid-second" for e in client.rpc({"method":"list"})["entries"]))
            input.chord("Control_L","Shift_L","t")
            eventually(lambda:client.rpc({"method":"list"})["selected"]=="grid-second")
            self.assertEqual(row("grid-second")["metrics"]["image_view"],saved)
            client.rpc({"method":"hide"}); self.show()
            eventually(lambda:row("grid-second").get("metrics",{}).get("loads"))
            self.assertEqual(row("grid-second")["metrics"]["image_view"],saved)
            captures=Path.home()/"Pictures/Asset Preview"; before=set(captures.glob("Preview-grid-second-*.png"))
            input.chord("Control_L","Shift_L","s")
            saved_files=list(eventually(lambda:set(captures.glob("Preview-grid-second-*.png"))-before))
            self.assertEqual(struct.unpack(">II",saved_files[0].read_bytes()[16:24]),(64,64))
            self.assertEqual(png_pixel(saved_files[0],0,0),(220,70,80))
            input.chord("Control_L","Shift_L","c")
            time.sleep(.1); self.assertEqual(set(captures.glob("Preview-grid-second-*.png"))-before,set(saved_files))
            # A second Qt process actually retrieves the copied image, proving
            # clipboard ownership and transfer rather than just the key binding.
            probe_source=self.root/"clipboard-reader.cpp"; probe=self.root/"clipboard-reader"
            probe_source.write_text(r'''#include <QApplication>
#include <QClipboard>
#include <QImage>
#include <QTimer>
#include <cstdio>
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    QTimer::singleShot(0,[&] {
        const auto image=app.clipboard()->image();
        if (image.isNull()) { app.exit(1); return; }
        std::printf("%d %d\n",image.width(),image.height()); app.exit(0);
    });
    return app.exec();
}
''')
            qt_flags=shlex.split(subprocess.run(["pkg-config","--cflags","--libs","Qt6Widgets"],check=True,capture_output=True,text=True).stdout)
            subprocess.run(["c++","-std=c++20","-fPIC",str(probe_source),"-o",str(probe),*qt_flags],check=True,capture_output=True)
            copied=subprocess.run([str(probe)],check=True,capture_output=True,text=True,timeout=10)
            copied_size=list(map(int,copied.stdout.split()))
            self.assertEqual(copied_size,[64,64])
            alpha_source=self.root/"saved-alpha.cpp"; alpha_probe=self.root/"saved-alpha"
            alpha_source.write_text('#include <QImage>\nint main(int argc,char** argv) { QImage image(argv[1]); return argc==2 && !image.isNull() && image.pixelColor(0,0).alpha()==80 ? 0 : 1; }')
            subprocess.run(["c++","-std=c++20","-fPIC",str(alpha_source),"-o",str(alpha_probe),*qt_flags],check=True,capture_output=True)
            subprocess.run([str(alpha_probe),str(saved_files[0])],check=True)
            client.rpc({"method":"quit"}); eventually(lambda:not client.running()); client.start(); self.show()
            eventually(lambda:row("grid-second").get("metrics",{}).get("loads"))
            self.assertEqual(row("grid-second")["metrics"]["image_view"],saved)
        finally:
            input.close()
            for path in saved_files: path.unlink(missing_ok=True)

    @unittest.skipUnless(GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland","Hardware comparison camera sync and presets")
    def test_38_comparison_sync_and_camera_views(self):
        obj=self.root/"compare-cube.obj"
        subprocess.run([sys.executable,str(ROOT/"examples/generate_cube.py"),str(obj)],check=True,capture_output=True)
        big=self.root/"compare-big.obj"
        big.write_text("\n".join("v "+" ".join(str(float(v)*2) for v in line.split()[1:]) if line.startswith("v ") else line for line in obj.read_text().splitlines()))
        for id,path in (("compare-one",obj),("compare-two",big)):
            client.rpc({"method":"add","entry":{"id":id,"path":str(path)}})
        self.add("compare-hidden"); self.show()
        client.rpc({"method":"compare","ids":["compare-one","compare-two"]})
        eventually(lambda:all(row(id).get("metrics",{}).get("engine") for id in ("compare-one","compare-two")))
        self.assertFalse(row("compare-hidden")["active"])
        renderer=row("compare-one")["metrics"]["renderer"]
        self.assertFalse(any(s in renderer.lower() for s in ("llvmpipe","softpipe","software","unknown")),renderer)
        input=XInput()
        try:
            time.sleep(.2); view=row("compare-one")["viewport"]; x=view["x"]+view["width"]//2; y=view["y"]+view["height"]//2
            before=row("compare-two")["metrics"]["camera"]
            input.drag(x,y,70,20)
            eventually(lambda:math.dist(before["position"],row("compare-two")["metrics"]["camera"]["position"])>1e-4)
            a=row("compare-one")["metrics"]["camera"]; b=row("compare-two")["metrics"]["camera"]
            for p,q in zip(a["position"],b["position"]): self.assertAlmostEqual(p*2,q,places=5)
            time.sleep(.5)
            input.key("Shift_L",1); input.drag(x,y,30,25); input.key("Shift_L",0); input.x.XFlush(input.display)
            eventually(lambda:math.dist(a["focal"],row("compare-one")["metrics"]["camera"]["focal"])>1e-4)
            time.sleep(.1); before_zoom=row("compare-one")["metrics"]["camera"]
            peer_before_zoom=row("compare-two")["metrics"]["camera"]
            input.scroll(x+20,y); time.sleep(.1)
            a=row("compare-one")["metrics"]["camera"]; b=row("compare-two")["metrics"]["camera"]
            self.assertLess(math.dist(a["position"],a["focal"]),math.dist(before_zoom["position"],before_zoom["focal"]))
            self.assertEqual(before_zoom["focal"],a["focal"])
            self.assertEqual(peer_before_zoom["focal"],b["focal"])
            for key in ("position","focal"):
                for p,q in zip(a[key],b[key]): self.assertAlmostEqual(p*2,q,places=5)
            input.press("7"); time.sleep(.1)
            a=row("compare-one")["metrics"]["camera"]
            self.assertAlmostEqual(a["position"][0],a["focal"][0],places=6)
            self.assertAlmostEqual(a["position"][2],a["focal"][2],places=6)
            self.assertGreater(a["position"][1],a["focal"][1])
            client.rpc({"method":"reload","id":"compare-one"})
            eventually(lambda:row("compare-one")["metrics"]["loads"]>1)
            self.assertAlmostEqual(row("compare-one")["metrics"]["camera"]["position"][0],a["position"][0],places=6)
            client.rpc({"method":"compare","camera_sync":False})
            before=row("compare-two")["metrics"]["camera"]
            input.press("1"); time.sleep(.1)
            self.assertEqual(row("compare-two")["metrics"]["camera"],before)
            client.rpc({"method":"compare","camera_sync":True})
            time.sleep(.5); view=row("compare-two")["viewport"]
            input.drag(view["x"]+view["width"]//2,view["y"]+view["height"]//2,35,15)
            eventually(lambda:client.rpc({"method":"list"})["selected"]=="compare-two")
            time.sleep(.1)
            a=row("compare-one")["metrics"]["camera"]; b=row("compare-two")["metrics"]["camera"]
            for p,q in zip(a["position"],b["position"]): self.assertAlmostEqual(p*2,q,places=5)
            client.rpc({"method":"quit"}); eventually(lambda:not client.running()); client.start(); self.show()
            eventually(lambda:all(row(id).get("metrics",{}).get("engine") for id in ("compare-one","compare-two")))
            self.assertEqual(client.rpc({"method":"list"})["comparison"],["compare-one","compare-two"])
            client.rpc({"method":"hide"}); self.assertEqual(client.rpc({"method":"list"})["active"],0)
        finally: input.close()

    def test_39_comparison_validation_and_failure_preserves_state(self):
        for id in ("pair-one","pair-two"):
            client.rpc({"method":"add","entry":{"id":id,"path":str(self.root/(id+".obj"))}})
        self.add("pair-image")
        client.rpc({"method":"compare","ids":["pair-one","pair-two"]})
        for bad in ({"ids":["pair-one"]},{"ids":["pair-one","pair-one"]},{"ids":["pair-one","pair-image"]},{"camera_sync":"yes"}):
            with self.assertRaises(RuntimeError): client.rpc({"method":"compare",**bad})
        registry=self.root/"state/previews.json"; backup=registry.with_suffix(".backup")
        registry.rename(backup); registry.mkdir()
        try:
            with self.assertRaises(RuntimeError): client.rpc({"method":"compare","ids":[]})
            self.assertEqual(client.rpc({"method":"list"})["comparison"],["pair-one","pair-two"])
        finally: registry.rmdir(); backup.replace(registry)
        client.rpc({"method":"select","id":"pair-image"})
        self.assertEqual(client.rpc({"method":"list"})["comparison"],[])
        client.rpc({"method":"compare","ids":["pair-one","pair-two"]})
        client.rpc({"method":"remove","id":"pair-one"})
        self.assertEqual(client.rpc({"method":"list"})["comparison"],[])

    @unittest.skipUnless(GPU and os.environ.get("QT_QPA_PLATFORM")!="wayland","Native hover targeting across model, image and video surfaces")
    def test_40_hover_targets_shortcuts_menus_and_native_focus(self):
        obj=self.root/"hover-cube.obj"
        subprocess.run([sys.executable,str(ROOT/"examples/generate_cube.py"),str(obj)],check=True,capture_output=True)
        for id in ("hover-one","hover-two"):
            client.rpc({"method":"add","entry":{"id":id,"path":str(obj),"settings":{"edges":False,"triangles":True}}})
        self.add("hover-image")
        clip=self.root/"hover-clip.mp4"; video(clip,duration=12)
        client.rpc({"method":"add","entry":{"id":"hover-video","path":str(clip),"settings":{"paused":True,"muted":True}}})
        client.rpc({"method":"layout","layout":"grid"}); self.show()
        ids=("hover-one","hover-two","hover-image","hover-video")
        eventually(lambda:all(row(id).get("metrics",{}).get("loads",0)>0 and row(id)["metrics"]["renders"]>0 for id in ids))
        loads={id:row(id)["metrics"]["loads"] for id in ids}
        input=XInput()
        def point(id,overlay=None):
            area=row(id)["overlays"][overlay] if overlay else row(id)["viewport"]
            return area["x"]+area["width"]//2,area["y"]+area["height"]//2
        def hover(id,overlay=None):
            input.motion(*point(id,overlay)); input.x.XFlush(input.display)
            eventually(lambda:client.rpc({"method":"list"})["selected"]==id)
            time.sleep(.1) # Native focus delivery follows the target update.
        try:
            hover("hover-one"); input.press("e")
            eventually(lambda:row("hover-one")["settings"]["edges"])
            self.assertFalse(row("hover-two")["settings"]["edges"])
            hover("hover-two"); input.press("m")
            eventually(lambda:not row("hover-two")["settings"]["triangles"])
            self.assertTrue(row("hover-one")["settings"]["triangles"])
            camera=row("hover-one")["metrics"]["camera"]
            input.press("7")
            eventually(lambda:abs(row("hover-two")["metrics"]["camera"]["position"][0]-row("hover-two")["metrics"]["camera"]["focal"][0])<1e-6)
            self.assertEqual(row("hover-one")["metrics"]["camera"],camera)
            input.chord("Control_L","r")
            eventually(lambda:row("hover-two")["metrics"]["loads"]>loads["hover-two"])
            self.assertEqual(row("hover-one")["metrics"]["loads"],loads["hover-one"])
            loads["hover-two"]=row("hover-two")["metrics"]["loads"]

            # Hovering a native video takes its keyboard focus without a click.
            hover("hover-video"); input.press("space")
            eventually(lambda:not row("hover-video")["metrics"]["paused"])
            input.press("space"); eventually(lambda:row("hover-video")["metrics"]["paused"])
            input.press("m"); eventually(lambda:not row("hover-video")["metrics"]["muted"])
            input.press("m"); eventually(lambda:row("hover-video")["metrics"]["muted"])
            # The previously focused video must not consume a model's M key.
            hover("hover-one","name"); input.press("m")
            eventually(lambda:not row("hover-one")["settings"]["triangles"])
            self.assertTrue(row("hover-video")["metrics"]["muted"])
            hover("hover-image"); input.scroll(*point("hover-image"))
            eventually(lambda:row("hover-image")["metrics"]["image_view"]["zoom"]>1)
            input.press("f"); eventually(lambda:row("hover-image")["metrics"]["image_view"]["zoom"]==1)
            input.press("m"); time.sleep(.1)
            self.assertTrue(row("hover-video")["metrics"]["muted"])

            # Moving behind an open menu cannot retarget its commands.
            hover("hover-two"); input.shortcut("v")
            eventually(lambda:client.rpc({"method":"list"})["options_open"])
            input.motion(*point("hover-image")); input.x.XFlush(input.display); time.sleep(.15)
            self.assertEqual(client.rpc({"method":"list"})["selected"],"hover-two")
            input.press("Escape")
            hover("hover-two")
            # A drag stays owned by its origin even while crossing another card.
            input.xt.XTestFakeButtonEvent(input.display,1,1,0)
            input.motion(*point("hover-image")); input.x.XFlush(input.display); time.sleep(.15)
            self.assertEqual(client.rpc({"method":"list"})["selected"],"hover-two")
            input.xt.XTestFakeButtonEvent(input.display,1,0,0); input.x.XFlush(input.display)

            # Failed hover saves preserve the old target; moving inside the
            # same card retries once state storage is writable again.
            hover("hover-one")
            registry=self.root/"state/previews.json"; backup=registry.with_suffix(".backup")
            registry.rename(backup); registry.mkdir()
            try:
                input.motion(*point("hover-two")); input.x.XFlush(input.display); time.sleep(.15)
                self.assertEqual(client.rpc({"method":"list"})["selected"],"hover-one")
            finally: registry.rmdir(); backup.replace(registry)
            x,y=point("hover-two"); input.motion(x+5,y); input.x.XFlush(input.display)
            eventually(lambda:client.rpc({"method":"list"})["selected"]=="hover-two")
            for id,count in loads.items(): self.assertEqual(row(id)["metrics"]["loads"],count)
            # Explicit selection wins over a stationary pointer until it moves.
            client.rpc({"method":"select","id":"hover-one"}); input.press("e")
            eventually(lambda:not row("hover-one")["settings"]["edges"])
            self.assertFalse(row("hover-two")["settings"]["edges"])
            input.motion(x+10,y); input.x.XFlush(input.display)
            eventually(lambda:client.rpc({"method":"list"})["selected"]=="hover-two")
            # Returning to the exact same point after using the selector/menu
            # area must still activate the preview, without an extra wiggle.
            input.motion(900,10); input.x.XFlush(input.display); time.sleep(.1)
            client.rpc({"method":"select","id":"hover-one"})
            input.motion(x+10,y); input.x.XFlush(input.display)
            eventually(lambda:client.rpc({"method":"list"})["selected"]=="hover-two")
            # Menus/toolbar space retain the last target for keyboard-only use.
            input.motion(900,10); input.x.XFlush(input.display); time.sleep(.1)
            input.press("e"); eventually(lambda:row("hover-two")["settings"]["edges"])
            self.assertFalse(row("hover-one")["settings"]["edges"])
        finally: input.close()

if __name__ == "__main__": unittest.main(verbosity=2)
