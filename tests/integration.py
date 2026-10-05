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
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("preview_client", str(ROOT / "bin/asset-preview"))
spec = importlib.util.spec_from_loader(loader.name, loader)
client = importlib.util.module_from_spec(spec); loader.exec_module(client)
GPU = os.environ.get("ASSET_PREVIEW_GPU_TEST") == "1"

def png(path: Path, color=(220, 70, 80, 255), size=64) -> None:
    def chunk(kind, data): return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    pixels = b"".join(b"\0" + bytes(color) * size for _ in range(size))
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    candidate = path.with_suffix(".candidate"); candidate.write_bytes(data); candidate.replace(path)

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

def horizon_roll(camera, vertical=1):
    # VTK stores an orthogonalized view-up vector. The camera's right vector
    # must have no component along world up for a level horizon.
    d=[p-f for p,f in zip(camera["position"],camera["focal"])]; up=camera["up"]
    right=[d[1]*up[2]-d[2]*up[1],d[2]*up[0]-d[0]*up[2],d[0]*up[1]-d[1]*up[0]]
    return right[vertical]/math.sqrt(sum(v*v for v in right))

def png_pixel(path, x, y):
    """Read one pixel from Qt's un-interlaced RGB/RGBA screenshot, without Pillow."""
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
    for line in range(y+1):
        start=line*(stride+1); filter_type=decoded[start]; row=bytearray(decoded[start+1:start+1+stride])
        for i in range(stride):
            left=row[i-channels] if i>=channels else 0; up=previous[i]; corner=previous[i-channels] if i>=channels else 0
            predictor=(0,left,up,(left+up)//2,paeth(left,up,corner))[filter_type]
            row[i]=(row[i]+predictor)&255
        previous=row
    return tuple(previous[x*channels:x*channels+3])

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
                    "XCloseDisplay":([pointer],integer)}
        for key,(args,result) in signatures.items(): getattr(self.x,key).argtypes=args; getattr(self.x,key).restype=result
        self.xt.XTestFakeKeyEvent.argtypes=[pointer,uint,integer,ulong]
        self.xt.XTestFakeButtonEvent.argtypes=[pointer,uint,integer,ulong]
        self.xt.XTestFakeMotionEvent.argtypes=[pointer,integer,integer,integer,ulong]
        self.display=self.x.XOpenDisplay(None)
        if not self.display: raise RuntimeError("Cannot open test display")
        self.root=self.x.XDefaultRootWindow(self.display)
        self.window=self.find(self.root)
        if not self.window: raise RuntimeError("Cannot find the Asset Preview window")
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
    def motion(self,x,y):
        ox=ctypes.c_int(); oy=ctypes.c_int(); child=ctypes.c_ulong()
        self.x.XTranslateCoordinates(self.display,self.window,self.root,0,0,ctypes.byref(ox),ctypes.byref(oy),ctypes.byref(child))
        self.xt.XTestFakeMotionEvent(self.display,-1,ox.value+x,oy.value+y,0)
    def click(self,x,y):
        self.motion(x,y)
        self.xt.XTestFakeButtonEvent(self.display,1,1,0)
        self.xt.XTestFakeButtonEvent(self.display,1,0,0); self.x.XFlush(self.display)
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
        if not GPU: os.environ["QT_QPA_PLATFORM"] = "offscreen"
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
            (target/"server.log").write_text((cls.root/"state/server.log").read_text())
        cls.temp.cleanup()

    def setUp(self):
        client.rpc({"method":"hide"})
        for r in client.rpc({"method":"list"})["entries"]: client.rpc({"method":"remove","id":r["id"]})
        client.rpc({"method":"layout","layout":"single","grid_size":2,"compact":False})

    def add(self, key="image", **extra):
        asset=self.root/f"{key}.png"; png(asset)
        entry={"id":key,"path":str(asset),**extra}
        client.rpc({"method":"add","entry":entry})
        return asset

    def show(self): client.rpc({"method":"show"})

    def test_01_private_socket_inert_connect_and_hidden_unload(self):
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
            self.assertAlmostEqual(horizon_roll(row("orbit")["metrics"]["camera"],2),0,places=8)
            self.assertGreater(row("orbit")["metrics"]["camera"]["up"][2],0)
            # Click the real native overlay, including when compact mode
            # removes the persistent Live footer. This tests input stacking.
            client.rpc({"method":"layout","compact":True})
            time.sleep(.2)
            button=row("orbit")["controls"]["options"]
            input.click(button["x"]+button["width"]//2,button["y"]+button["height"]//2)
            eventually(lambda:client.rpc({"method":"list"})["options_open"])
            input.key("t",1); input.key("t",0); input.x.XFlush(input.display)
            eventually(lambda:row("orbit")["settings"].get("textures") is False)
            self.assertFalse(client.rpc({"method":"list"})["options_open"])
            # Global navigation also works while the native child has focus.
            input.shortcut("Right"); eventually(lambda:client.rpc({"method":"list"})["selected"]=="next")
        finally: input.close()

    def test_13_corrupt_state_is_preserved(self):
        runtime,state=client.locations(); bad_runtime=self.root/"bad-runtime"; bad_state=self.root/"bad-state"
        client.private_directory(bad_state); corrupt=bad_state/"previews.json"; content=b"{unfinished"; corrupt.write_bytes(content)
        os.environ["ASSET_PREVIEW_RUNTIME_DIR"]=str(bad_runtime); os.environ["ASSET_PREVIEW_STATE_DIR"]=str(bad_state)
        try:
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

    def test_15_larger_grids_compact_status_validation_and_persistence(self):
        for i in range(20): self.add(f"tile{i}")
        self.show()
        for size,count in ((3,9),(4,16)):
            client.rpc({"method":"select","id":"tile0"})
            client.rpc({"method":"layout","layout":"grid","grid_size":size})
            self.assertEqual(client.rpc({"method":"list"})["active"],count)
            client.rpc({"method":"select","id":"tile19"})
            self.assertEqual(client.rpc({"method":"list"})["active"],20-count*(19//count))
            self.assertNotIn("metrics",row("tile0"))
        client.rpc({"method":"select","id":"tile0"})
        eventually(lambda:row("tile0").get("metrics",{}).get("loads",0)>0)
        initial=row("tile0")["metrics"]["loads"]
        client.rpc({"method":"layout","compact":True})
        self.assertEqual(row("tile0")["metrics"]["loads"],initial)
        self.assertFalse(row("tile0")["status_visible"])
        client.rpc({"method":"add","entry":{"id":"tile0","path":str(self.root/"uncreated-grid.png")}})
        self.assertTrue(row("tile0")["status_visible"])
        self.assertTrue(row("tile0")["status"].startswith("Waiting"))
        for fields in ({"grid_size":5},{"grid_size":2.5},{"grid_size":"3"},{"compact":1},{"layout":None}):
            with self.assertRaises(RuntimeError): client.rpc({"method":"layout",**fields})
        for fields in ({"up_axis":"x"},{"lighting":"unknown"},{"textures":1}):
            with self.assertRaises(RuntimeError): client.rpc({"method":"settings","id":"tile0","settings":fields})
        client.rpc({"method":"quit"}); eventually(lambda:not client.running())
        self.assertTrue(client.start()); self.pid=client.rpc({"method":"ping"})["pid"]; type(self).pid=self.pid
        state=client.rpc({"method":"list"})
        self.assertEqual((state["layout"],state["grid_size"],state["compact"]),("grid",4,True))
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

if __name__ == "__main__": unittest.main(verbosity=2)
