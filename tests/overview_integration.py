#!/usr/bin/env python3
"""Bounded cache and overview acceptance against private native processes."""
from __future__ import annotations
import ctypes
import json
import os
import signal
import statistics
import subprocess
import sys
import time
import unittest
from pathlib import Path
import inspection_integration as harness
from integration import png, audio, video, ticks, memory, XInput, png_pixel

class OverviewIntegration(unittest.TestCase):
    def setUp(self):
        self.fixture=harness.InspectionIntegration(methodName="runTest")
        self.fixture._testMethodName=self._testMethodName
        self.addCleanup(self.fixture.doCleanups); self.fixture.setUp()
        self.root=self.fixture.root; self.rpc=self.fixture.rpc; self.row=self.fixture.row

    def image(self,key,size=256,color=(80,140,200,255),**fields):
        path=self.root/(key+".png"); png(path,color,size)
        self.rpc({"method":"add","entry":{"id":key,"path":str(path),**fields}})
        return path

    def loaded(self,key):
        return harness.eventually(lambda:self.row(key) if self.row(key).get("metrics",{}).get("loads",0)>0 else None)

    def overview(self):
        self.rpc({"method":"layout","layout":"overview"}); self.rpc({"method":"show"})
        return harness.eventually(lambda:self.rpc({"method":"list"}) if self.rpc({"method":"list"})["presentation"]["overview"]["visible"] else None)

    def settled(self):
        return harness.eventually(lambda:self.rpc({"method":"list"}) if not self.rpc({"method":"list"})["presentation"]["overview"]["worker_active"] else None)

    def preloaded(self,key,state="cached"):
        return harness.eventually(lambda:self.rpc({"method":"list"}) if
            (p:=self.rpc({"method":"list"})["preload"])["target"]==key and p["state"]==state and not p["worker_active"] else None)

    def restart(self):
        self.rpc({"method":"quit"}); self.fixture.process.wait(timeout=5)
        self.restart_stopped()

    def restart_stopped(self):
        with self.fixture.log_path.open("ab") as log:
            self.fixture.process=subprocess.Popen([str(harness.BINARY),"--socket",str(self.fixture.socket_path),"--state",str(self.fixture.state_dir)],
                env=self.fixture.env,cwd=harness.PROJECT_ROOT,stdin=subprocess.DEVNULL,stdout=log,stderr=log,start_new_session=True)
        harness.eventually(self.fixture.ping)

    def test_close_default_left_boundaries_and_nonselected(self):
        for key in ("one","two","three","four"): self.image(key)
        self.assertEqual(self.rpc({"method":"list"})["close_behavior"],"left")
        self.rpc({"method":"select","id":"four"}); self.rpc({"method":"select","id":"two"})
        self.rpc({"method":"remove","id":"two"})
        self.assertEqual(self.rpc({"method":"list"})["selected"],"one","left wins over last viewed four")
        self.rpc({"method":"remove","id":"four"})
        self.assertEqual(self.rpc({"method":"list"})["selected"],"one")
        self.rpc({"method":"remove","id":"one"})
        self.assertEqual(self.rpc({"method":"list"})["selected"],"three","first falls back to its right neighbor")
        self.rpc({"method":"remove","id":"three"})
        state=self.rpc({"method":"list"})
        self.assertEqual(state["selected"],""); self.assertEqual(state["close_target"],"")

    def test_close_many_validation_atomic_save_destination_and_reopen(self):
        original=["one","two","three","four","five"]
        for key in original:self.image(key)
        self.rpc({"method":"select","id":"four"}); self.rpc({"method":"show"}); self.loaded("four")
        marker=self.root/"closed-generator-must-not-run"
        self.rpc({"method":"add","entry":{"id":"three","path":str(self.root/"three.png"),"cwd":str(self.root),
            "command":[sys.executable,"-c",f"from pathlib import Path; Path({str(marker)!r}).touch()"]}})
        def ids():return [r["id"] for r in self.rpc({"method":"list"})["entries"]]
        for values in ([],None,"two",["two","unknown"],["two",True],["two"]*257):
            with self.assertRaises(RuntimeError):self.rpc({"method":"remove_many","ids":values})
            self.assertEqual(ids(),original)
        registry=self.fixture.state_dir/"previews.json"; backup=registry.with_suffix(".before")
        registry.rename(backup); registry.mkdir()
        try:
            with self.assertRaises(RuntimeError):self.rpc({"method":"remove_many","ids":["three","four"]})
            state=self.rpc({"method":"list"})
            self.assertEqual(ids(),original); self.assertEqual(state["selected"],"four")
            self.assertEqual(state["closed_count"],0); self.assertTrue(self.row("four")["active"])
        finally:registry.rmdir(); backup.rename(registry)
        self.rpc({"method":"remove_many","ids":["four","three","four"]})
        self.assertEqual(ids(),["one","two","five"])
        self.assertEqual(self.rpc({"method":"list"})["selected"],"two")
        self.assertFalse(marker.exists(),"batch close must not activate an intermediate closed neighbor")
        self.rpc({"method":"reopen"}); self.rpc({"method":"reopen"})
        self.assertEqual(ids(),original)
        self.rpc({"method":"select","id":"five"}); self.rpc({"method":"select","id":"two"})
        self.rpc({"method":"close_behavior","mode":"last"})
        self.rpc({"method":"remove_many","ids":["one","two","three","four"]})
        self.assertEqual(self.rpc({"method":"list"})["selected"],"five")
        self.rpc({"method":"remove_many","ids":["five"]})
        state=self.rpc({"method":"list"})
        self.assertEqual(state["selected"],""); self.assertEqual(state["active"],0)

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_native_multiselect_grid_overview_modifiers_close_and_small_window(self):
        self.fixture.add_model(); self.rpc({"method":"show"}); model=self.loaded("model")
        renderer=model["metrics"]["renderer"]
        self.assertFalse(any(x in renderer.lower() for x in ("llvmpipe","softpipe","software","unknown")),renderer)
        (self.root/"renderer.json").write_text(json.dumps({"renderer":renderer}))
        keys=[f"image-{i}" for i in range(30)]
        for i,key in enumerate(keys):self.image(key,color=(40+i*5,110,190,255))
        self.rpc({"method":"layout","layout":"grid","grid_size":2})
        controls=XInput(); self.addCleanup(controls.close)
        def click(rect,*modifiers):
            for key in modifiers:controls.key(key,1)
            controls.click(rect["x"]+rect["width"]//2,rect["y"]+rect["height"]//2)
            for key in reversed(modifiers):controls.key(key,0)
            controls.x.XFlush(controls.display)
        def marked(expected):
            def check():
                state=self.rpc({"method":"list"})
                return state if state["presentation"]["marked"]==expected else None
            return harness.eventually(check)
        def box(key):return self.row(key)["overlays"]["selection"]
        def surface(key):return self.row(key)["viewport"]
        click(box("model")); marked(["model"])
        click(surface("image-2"),"Control_L"); marked(["model","image-2"])
        click(surface("image-1"),"Shift_L"); marked(["image-1","image-2"])
        click(box("image-0"),"Control_L","Shift_L"); marked(keys[:3])
        click(box("image-1")); marked(["image-0","image-2"])
        camera=self.row("model")["metrics"]["camera"]
        click(surface("model"),"Control_L"); marked(["model","image-0","image-2"])
        click(surface("model"),"Control_L"); marked(["image-0","image-2"])
        # Hovering the model reveals its toolbar mode control, which can resize
        # the card and adjust projection. Selection must preserve the pose.
        after=self.row("model")["metrics"]["camera"]
        for field in ("position","focal","up"):
            for actual,expected in zip(after[field],camera[field]):self.assertAlmostEqual(actual,expected,places=9)
        self.rpc({"method":"capture","path":str(self.root/"multiselect-grid.png")})
        self.overview(); self.settled(); marked(["image-0","image-2"])
        def tile(key):return next(t for t in self.rpc({"method":"list"})["presentation"]["overview"]["tiles"] if t["id"]==key)
        click(tile("image-4"),"Control_L"); marked(["image-0","image-2","image-4"])
        click(tile("image-3"),"Shift_L"); marked(keys[3:5])
        click(tile("image-0")["checkbox"],"Control_L","Shift_L"); marked(keys[:5])
        click(tile("image-1")["checkbox"]); marked(["image-0",*keys[2:5]])
        # Shift selection follows registration order, including offscreen tiles.
        controls.press("End")
        harness.eventually(lambda:any(t["id"]==keys[-1] for t in self.rpc({"method":"list"})["presentation"]["overview"]["tiles"]))
        click(tile(keys[-1]),"Shift_L"); state=marked(keys[1:])
        self.assertEqual(state["active"],0); self.assertEqual(state["layout"],"overview")
        self.settled(); self.rpc({"method":"theme","mode":"dark"})
        self.rpc({"method":"capture","path":str(self.root/"multiselect-overview-dark.png")})
        self.rpc({"method":"theme","mode":"light"})
        self.rpc({"method":"capture","path":str(self.root/"multiselect-overview-light.png")})
        controls.x.XResizeWindow.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_uint,ctypes.c_uint]
        controls.x.XResizeWindow(controls.display,controls.window,540,380); controls.x.XFlush(controls.display); time.sleep(.2)
        state=marked(keys[1:]); button=state["presentation"]["close_selected"]
        self.assertTrue(button["visible"]); self.assertGreaterEqual(button["x"],0)
        self.assertLessEqual(button["x"]+button["width"],state["presentation"]["width"])
        self.rpc({"method":"capture","path":str(self.root/"multiselect-small.png")})
        registry=self.fixture.state_dir/"previews.json"; backup=registry.with_suffix(".before")
        registry.rename(backup); registry.mkdir()
        try:
            click(button); time.sleep(.15); state=marked(keys[1:])
            self.assertEqual(len(state["entries"]),31); self.assertEqual(state["closed_count"],0)
        finally:registry.rmdir(); backup.rename(registry)
        click(button)
        state=marked([])
        self.assertEqual([r["id"] for r in state["entries"]],["model","image-0"])
        self.assertEqual(state["active"],0)
        for _ in keys[1:]:self.rpc({"method":"reopen"})
        self.assertEqual([r["id"] for r in self.rpc({"method":"list"})["entries"]],["model",*keys])
        self.assertEqual(self.rpc({"method":"list"})["presentation"]["marked"],[])
        controls.press("Home"); time.sleep(.1)
        controls.chord("Control_L","space"); state=marked(["model"])
        click(state["presentation"]["close_selected"]); marked([])
        self.assertNotIn("model",[r["id"] for r in self.rpc({"method":"list"})["entries"]])

    def test_close_preference_persistence_validation_and_failed_save(self):
        for key in ("one","two","three"): self.image(key)
        self.rpc({"method":"select","id":"three"}); self.rpc({"method":"select","id":"two"})
        self.assertEqual(self.rpc({"method":"close_behavior","mode":"last"})["close_target"],"three")
        registry=self.fixture.state_dir/"previews.json"; original=registry.read_bytes()
        for mode in ("recent","",True,1,[],{},None):
            with self.assertRaises(RuntimeError):self.rpc({"method":"close_behavior","mode":mode})
            self.assertEqual(registry.read_bytes(),original)
        backup=registry.with_suffix(".backup"); registry.rename(backup); registry.mkdir()
        try:
            with self.assertRaises(RuntimeError):self.rpc({"method":"close_behavior","mode":"left"})
            self.assertEqual(self.rpc({"method":"close_behavior"})["close_behavior"],"last")
            self.assertEqual(self.rpc({"method":"list"})["close_target"],"three")
            with self.assertRaises(RuntimeError):self.rpc({"method":"remove","id":"two"})
            self.assertEqual(self.rpc({"method":"list"})["selected"],"two")
        finally:registry.rmdir(); backup.rename(registry)
        self.rpc({"method":"remove","id":"two"})
        self.assertEqual(self.rpc({"method":"list"})["selected"],"three")
        self.restart()
        self.assertEqual(self.rpc({"method":"close_behavior"})["close_behavior"],"last")
        self.assertEqual(self.rpc({"method":"list"})["close_target"],"one","no history survives restart")
        self.rpc({"method":"close_all"})
        self.assertEqual(self.rpc({"method":"close_behavior"})["close_behavior"],"last")

    def test_close_legacy_default_and_invalid_saved_preference(self):
        self.image("first")
        registry=self.fixture.state_dir/"previews.json"
        self.rpc({"method":"quit"}); self.fixture.process.wait(timeout=5)
        state=json.loads(registry.read_text()); state.pop("close_behavior")
        registry.write_text(json.dumps(state)); self.restart_stopped()
        self.assertEqual(self.rpc({"method":"close_behavior"})["close_behavior"],"left")
        self.rpc({"method":"quit"}); self.fixture.process.wait(timeout=5)
        for mode in ("recent",False,None):
            state["close_behavior"]=mode; registry.write_text(json.dumps(state)); original=registry.read_bytes()
            result=subprocess.run([str(harness.BINARY),"--socket",str(self.fixture.socket_path),"--state",str(self.fixture.state_dir)],
                env=self.fixture.env,capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,1,result.stderr)
            self.assertEqual(registry.read_bytes(),original)

    def test_return_preload_unvisited_image_freshness_and_idle(self):
        path=self.image("left",1024); self.image("current"); self.image("other")
        self.rpc({"method":"select","id":"current"}); self.rpc({"method":"show"}); self.loaded("current")
        state=self.preloaded("left")
        state=self.settled()
        self.assertEqual(state["active"],1); self.assertNotIn("metrics",self.row("left"))
        self.assertGreaterEqual(state["cache"]["bytes"],1024*1024*4)
        self.assertLessEqual(state["cache"]["bytes"],state["cache"]["limit"])
        sequence=state["cache"]["diagnostics"]["last_seq"]
        before=ticks(self.fixture.process.pid); time.sleep(.6)
        self.assertLessEqual(ticks(self.fixture.process.pid)-before,3)
        self.assertEqual(self.rpc({"method":"list"})["cache"]["diagnostics"]["last_seq"],sequence,
            self.rpc({'method':'cache','action':'events','after':sequence})['events'])
        png(path,(10,210,40,255),1024)
        harness.eventually(lambda:any(e["tier"]=="preload" and e["action"]=="complete" for e in
            self.rpc({"method":"cache","action":"events","after":sequence})["events"]))
        self.preloaded("left")
        self.rpc({"method":"remove","id":"current"})
        self.assertTrue(self.loaded("left")["metrics"]["cache_hit"])
        capture=self.root/"preloaded-image.png"; self.rpc({"method":"capture","path":str(capture)})
        self.assertEqual(png_pixel(capture,450,350),(10,210,40))

    def test_return_preload_follows_preference_skips_hidden_and_clears(self):
        self.image("first"); self.image("left"); self.image("current"); self.image("recent")
        self.rpc({"method":"select","id":"recent"}); self.rpc({"method":"select","id":"current"})
        self.rpc({"method":"show"}); self.preloaded("left")
        self.rpc({"method":"close_behavior","mode":"last"}); self.preloaded("recent")
        self.rpc({"method":"hide"}); time.sleep(.2)
        state=self.rpc({"method":"list"})
        self.assertEqual(state["cache"]["bytes"],0); self.assertEqual(state["preload"]["target"],"")
        self.rpc({"method":"show"}); self.preloaded("recent")
        self.rpc({"method":"cache","action":"clear"}); time.sleep(.2)
        self.assertEqual(self.rpc({"method":"list"})["cache"]["bytes"],0)
        self.rpc({"method":"layout","layout":"overview"}); self.settled()
        self.assertEqual(self.rpc({"method":"list"})["preload"]["target"],"")
        self.rpc({"method":"layout","layout":"grid"})
        harness.eventually(lambda:self.rpc({"method":"list"})["active"]==4,
            description="finite grid admission before checking preload ownership")
        self.assertEqual(self.rpc({"method":"list"})["preload"]["target"],"","visible target needs no speculative decode")

    def test_return_preload_missing_large_and_model_without_generator(self):
        missing=self.root/"future.png"
        self.rpc({"method":"add","entry":{"id":"future","path":str(missing)}})
        self.image("current"); self.rpc({"method":"select","id":"current"}); self.rpc({"method":"show"})
        self.preloaded("future","missing")
        png(missing); self.preloaded("future")
        # Huge dimensions in a small compressed source must not be decoded.
        self.image("large",3072); self.image("after-large")
        self.rpc({"method":"select","id":"after-large"}); self.preloaded("large","skipped")
        marker=self.root/"must-not-run"; model=self.root/"model.obj"
        with model.open("wb") as output:output.truncate(64*1024*1024)
        self.rpc({"method":"add","entry":{"id":"model","path":str(model),"cwd":str(self.root),"command":[sys.executable,"-c",f"from pathlib import Path; Path({str(marker)!r}).touch()"]}})
        self.image("after-model"); self.rpc({"method":"select","id":"after-model"})
        self.preloaded("model","warmed")
        self.assertFalse(marker.exists()); self.assertNotIn("metrics",self.row("model"))
        self.assertFalse(self.row("model")["building"])
        events=self.rpc({"method":"cache","action":"events","limit":128})["events"]
        warmed=next(e for e in reversed(events) if e["tier"]=="preload" and e["reason"]=="warmed")
        self.assertEqual(warmed["bytes"],32*1024*1024)

    def test_return_preload_rapid_navigation_and_inflight_hide_clear(self):
        for key in ("first","second","third"):self.image(key,2048)
        self.image("current"); self.rpc({"method":"select","id":"current"})
        for _ in range(4):
            self.rpc({"method":"show"}); self.rpc({"method":"hide"})
            harness.eventually(lambda:not self.rpc({"method":"list"})["preload"]["worker_active"])
            self.assertEqual(self.rpc({"method":"list"})["cache"]["bytes"],0)
        self.rpc({"method":"show"})
        for _ in range(3):
            for key in ("second","third","current"):self.rpc({"method":"select","id":key})
        state=self.preloaded("third"); self.assertEqual(state["active"],1)
        self.rpc({"method":"cache","action":"clear"})
        self.rpc({"method":"select","id":"third"}); self.rpc({"method":"cache","action":"clear"})
        harness.eventually(lambda:not self.rpc({"method":"list"})["preload"]["worker_active"])
        self.assertEqual(self.rpc({"method":"list"})["cache"]["bytes"],0)

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_return_preload_native_menu_keyboard_close_and_small_window(self):
        self.fixture.add_model(); self.rpc({"method":"show"}); model=self.loaded("model")
        renderer=model["metrics"]["renderer"]
        self.assertFalse(any(x in renderer.lower() for x in ("llvmpipe","softpipe","software","unknown")),renderer)
        (self.root/"renderer.json").write_text(json.dumps({"renderer":renderer}))
        for key in ("left","current","recent"):self.image(key)
        self.rpc({"method":"select","id":"recent"}); self.rpc({"method":"select","id":"current"})
        controls=XInput(); self.addCleanup(controls.close)
        def menu():
            controls.shortcut("s"); time.sleep(.1)
            controls.press("Home"); controls.press("Down"); controls.press("Right"); time.sleep(.2)
        def screen(name):
            # Gamescope redirects top-level windows, so its X11 root capture
            # is black. Capture the actual raster menu windows individually.
            root=ctypes.c_ulong(); parent=ctypes.c_ulong(); children=ctypes.POINTER(ctypes.c_ulong)(); count=ctypes.c_uint()
            controls.x.XQueryTree(controls.display,controls.root,ctypes.byref(root),ctypes.byref(parent),ctypes.byref(children),ctypes.byref(count))
            ids=[children[i] for i in range(count.value)]
            if children:controls.x.XFree(children)
            captures=0
            for window in ids:
                prop=subprocess.run(["xprop","-id",str(window),"_NET_WM_WINDOW_TYPE"],capture_output=True,text=True,check=True)
                if "_NET_WM_WINDOW_TYPE_POPUP_MENU" not in prop.stdout:continue
                target=self.root/f"{Path(name).stem}-{captures}.png"
                result=subprocess.run(["ffmpeg","-nostdin","-loglevel","error","-f","x11grab","-window_id",str(window),
                    "-i",os.environ["DISPLAY"],"-frames:v","1","-threads","1",str(target)],capture_output=True,timeout=5)
                if result.returncode==0:captures+=1
            self.assertGreaterEqual(captures,2,"Settings and its close-destination submenu are visible")
        for mode,key in (("last","recent"),("left","left")):
            menu()
            screen(f"close-menu-{mode}.png")
            controls.press("Home")
            if mode=="last":controls.press("Down")
            controls.press("Return")
            harness.eventually(lambda:self.rpc({"method":"close_behavior"})["close_behavior"]==mode)
            self.preloaded(key)
        controls.chord("Control_L","w")
        harness.eventually(lambda:self.rpc({"method":"list"})["selected"]=="left")
        self.assertTrue(self.loaded("left")["metrics"]["cache_hit"])
        controls.chord("Control_L","Shift_L","t")
        harness.eventually(lambda:self.rpc({"method":"list"})["selected"]=="current")
        controls.x.XResizeWindow.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_uint,ctypes.c_uint]
        controls.x.XResizeWindow(controls.display,controls.window,540,380); controls.x.XFlush(controls.display); time.sleep(.2)
        menu(); screen("close-menu-small.png")
        controls.press("Escape"); controls.press("Escape")

    def test_image_cache_transfer_freshness_force_reload_and_hide(self):
        asset=self.image("first",1024); self.image("second")
        self.rpc({"method":"show"}); first=self.loaded("first")
        self.assertFalse(first["metrics"]["cache_hit"])
        self.rpc({"method":"select","id":"second"}); self.loaded("second")
        cached=self.rpc({"method":"list"})["cache"]
        self.assertGreaterEqual(cached["bytes"],4*1024*1024)
        self.rpc({"method":"select","id":"first"}); self.assertTrue(self.loaded("first")["metrics"]["cache_hit"])
        self.rpc({"method":"reload","id":"first"}); self.assertFalse(self.row("first")["metrics"]["cache_hit"])
        self.rpc({"method":"select","id":"second"}); png(asset,(220,30,50,255),1024)
        self.rpc({"method":"select","id":"first"}); self.assertFalse(self.loaded("first")["metrics"]["cache_hit"])
        self.rpc({"method":"hide"}); state=self.rpc({"method":"list"})
        self.assertEqual(state["active"],0); self.assertEqual(state["cache"]["bytes"],0)

    def test_cache_diagnostics_cli_limits_reclamation_and_idle(self):
        self.image("first"); self.image("second"); self.rpc({"method":"show"}); self.loaded("first")
        self.rpc({"method":"select","id":"second"}); self.rpc({"method":"select","id":"first"})
        self.rpc({"method":"hide"})
        result=self.rpc({"method":"cache","action":"events","limit":128})
        events=result["events"]; diagnostics=result["cache"]["diagnostics"]
        self.assertTrue(any(e["action"]=="take" and e["detail"]["source"].endswith("first.png") for e in events))
        self.assertTrue(any(e["action"]=="clear" and e["reason"]=="hidden" for e in events))
        if sys.platform.startswith("linux"):
            self.assertTrue(any(e["tier"]=="heap" and e["action"]=="trim" and "released" in e["detail"] for e in events))
        self.assertLessEqual(diagnostics["event_bytes"],64*1024)
        for limit,after in [(0,0),(129,0),(1.5,0),(8,-1),(8,"0"),(8,1.25)]:
            with self.assertRaises(RuntimeError): self.rpc({"method":"cache","action":"events","limit":limit,"after":after})
        self.assertEqual(self.rpc({"method":"cache","action":"events","after":result["last_seq"]})["events"],[])
        cli=subprocess.run([str(harness.CLI),"cache","events","--limit","3","--json"],env=self.fixture.env,capture_output=True,text=True,check=True)
        self.assertLessEqual(len(json.loads(cli.stdout)["events"]),3)
        stats=subprocess.run([str(harness.CLI),"cache"],env=self.fixture.env,capture_output=True,text=True,check=True)
        self.assertIn("refaults:",stats.stdout)
        logfile=self.fixture.state_dir/"cache-churn-v1.jsonl"
        harness.eventually(lambda:logfile.exists() and logfile.stat().st_size>100)
        lines=[json.loads(line) for line in logfile.read_text().splitlines()]
        self.assertTrue(any(e.get("action")=="take" for e in lines))
        self.assertLessEqual(logfile.stat().st_size,256*1024)
        before=self.rpc({"method":"cache"})["cache"]["diagnostics"]; time.sleep(.6)
        after=self.rpc({"method":"cache"})["cache"]["diagnostics"]
        self.assertEqual(before["last_seq"],after["last_seq"],"reading diagnostics and idling produce no cache events")
        self.assertEqual(before["log_writes"],after["log_writes"],"no continuing log flush timer")
        self.rpc({"method":"quit"}); self.fixture.process.wait(timeout=5)
        stopped=subprocess.run([str(harness.CLI),"cache","--json"],env=self.fixture.env,capture_output=True,text=True,check=True)
        self.assertEqual(json.loads(stopped.stdout),{"running":False})

    def test_overview_visible_priority_all_open_warming_and_idle(self):
        for i in range(80): self.image(f"image-{i}",color=(30+i*2,100,200,255))
        marker=self.root/"must-not-run"
        self.rpc({"method":"add","entry":{"id":"generator","path":str(self.root/"waiting.png"),"cwd":str(self.root),
            "command":[sys.executable,"-c",f"from pathlib import Path; Path({str(marker)!r}).touch()"]}})
        state=self.overview()
        harness.eventually(lambda:self.rpc({"method":"list"})["cache"]["disk_entries"]>=4)
        state=self.settled(); overview=state["presentation"]["overview"]
        self.assertEqual(state["active"],0); self.assertEqual(overview["items"],81)
        self.assertLess(len(overview["tiles"]),40); self.assertEqual(state["cache"]["disk_entries"],80)
        self.assertEqual(overview['image_worker_limit'],2); self.assertEqual(overview['render_worker_limit'],1)
        self.assertLessEqual(state["cache"]["thumbnail_bytes"],16*1024*1024)
        self.assertFalse(marker.exists()); self.assertTrue(all(not r["building"] for r in state["entries"]))
        time.sleep(.2); before=ticks(self.fixture.process.pid); time.sleep(.6); after=ticks(self.fixture.process.pid)
        self.assertLessEqual(after-before,3,"overview must settle without a polling/render loop")
        self.rpc({"method":"zoom","id":"image-79"})
        self.assertEqual(self.rpc({"method":"list"})["layout"],"single"); self.assertTrue(self.loaded("image-79")["active"])

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_unvisited_models_and_materials_get_serial_low_priority_thumbnails(self):
        marker=self.root/'thumbnail-must-not-run-generator'
        for key in ('model-a','model-b'):
            self.rpc({'method':'add','entry':{'id':key,'kind':'model','path':str(self.fixture.model),
                'settings':{'front_axis':'+z','materials':key=='model-a'},'cwd':str(self.root),
                'command':[sys.executable,'-c',f'from pathlib import Path; Path({str(marker)!r}).touch()']}})
        albedo=self.root/'albedo.png'; png(albedo,(70,160,210,255))
        self.rpc({'method':'add','entry':{'id':'material','kind':'material','path':str(albedo),'settings':{'shape':'sphere'}}})
        broken=self.root/'broken.obj'; broken.write_text('not a mesh')
        self.rpc({'method':'add','entry':{'id':'broken','kind':'model','path':str(broken)}})
        before=self.rpc({'method':'list'}); self.assertEqual(before['active'],0)
        self.overview()
        running=harness.eventually(lambda:(s if s['presentation']['overview']['worker_pid'] else None)
            if (s:=self.rpc({'method':'list'})) else None)
        pid=int(running['presentation']['overview']['worker_pid'])
        harness.eventually(lambda:os.getpriority(os.PRIO_PROCESS,pid)==19)
        observed=set()
        def finished():
            state=self.rpc({'method':'list'}); overview=state['presentation']['overview']
            self.assertEqual(state['active'],0)
            self.assertEqual(overview['worker_limit'],3)
            self.assertEqual(overview['render_worker_limit'],1)
            if overview['worker_pid']: observed.add(int(overview['worker_pid']))
            self.assertTrue(all(not entry['building'] and 'metrics' not in entry for entry in state['entries']))
            return state if not overview['worker_active'] and overview['rendered']==3 else None
        state=harness.eventually(finished,timeout=40,description='fresh model/material thumbnail queue')
        self.assertFalse(marker.exists()); self.assertEqual(state['selected'],before['selected'])
        renderer=state['presentation']['overview']['renderer']
        events=self.rpc({'method':'cache','action':'events','limit':128})['events']
        generated=[event for event in events if event['tier']=='thumbnail' and event['action']=='generated' and event['reason']=='visible_model']
        self.assertEqual(len(generated),3)
        for event in generated:
            renderer=event['detail']['renderer']
            self.assertFalse(any(token in renderer.lower() for token in ('llvmpipe','softpipe','software','unknown')),renderer)
            self.assertEqual(event['detail']['nice'],19)
        capture=self.root/'unvisited-thumbnails.png'; self.rpc({'method':'capture','path':str(capture)})
        _,_,rows=harness.png_rgb(capture)
        for tile in state['presentation']['overview']['tiles']:
            if tile['id']=='broken': continue
            colors={tuple(rows[y][x*3:x*3+3]) for y in range(tile['y']+12,tile['y']+90,4)
                for x in range(tile['x']+12,tile['x']+tile['width']-12,4)}
            self.assertGreater(len(colors),20,tile['id']+' should show a rendered asset')
        time.sleep(.2); start=ticks(self.fixture.process.pid); time.sleep(.6)
        self.assertLessEqual(ticks(self.fixture.process.pid)-start,3)
        for child in observed: self.assertFalse(Path(f'/proc/{child}').exists(),'thumbnail child must exit')
        failures=sum(event['tier']=='thumbnail' and event['action']=='skipped' for event in events)
        (self.root/'unrelated.note').write_text('An unrelated directory event must not retry a failed import.')
        time.sleep(.4)
        events=self.rpc({'method':'cache','action':'events','limit':128})['events']
        self.assertEqual(sum(event['tier']=='thumbnail' and event['action']=='skipped' for event in events),failures)
        self.restart(); self.rpc({'method':'show'}); state=self.settled()
        self.assertEqual(state['presentation']['overview']['rendered'],0,'restart reuses disk thumbnails')
        self.assertGreaterEqual(state['cache']['disk_hits'],3)
        # Invalidate one visual identity, then hide as soon as its child starts.
        self.rpc({'method':'settings','id':'model-a','settings':{'edges':True}})
        state=harness.eventually(lambda:s if (s:=self.rpc({'method':'list'}))['presentation']['overview']['worker_pid'] else None)
        child=int(state['presentation']['overview']['worker_pid'])
        self.rpc({'method':'hide'})
        harness.eventually(lambda:not Path(f'/proc/{child}').exists())
        self.assertEqual(self.rpc({'method':'list'})['cache']['bytes'],0)
        self.rpc({'method':'show'})
        state=harness.eventually(lambda:s if (s:=self.rpc({'method':'list'}))['presentation']['overview']['rendered']==1 and not s['presentation']['overview']['worker_active'] else None,timeout=20)
        png(albedo,(210,80,40,255))
        state=harness.eventually(lambda:s if (s:=self.rpc({'method':'list'}))['presentation']['overview']['rendered']==2 and not s['presentation']['overview']['worker_active'] else None,timeout=20)
        self.assertFalse(marker.exists())
        (self.root/'unvisited-thumbnails-result.json').write_text(json.dumps({'renderer':renderer,'observed_children':sorted(observed),
            'route':'Prepared native F3D backend in an isolated finite process for independent hard cancellation; ordinary importer and cached frames reused.',
            'final':state,'memory':memory(self.fixture.process.pid)},indent=2))
        self.rpc({'method':'settings','id':'model-a','settings':{'edges':False}})
        state=harness.eventually(lambda:s if (s:=self.rpc({'method':'list'}))['presentation']['overview']['worker_pid'] else None)
        child=int(state['presentation']['overview']['worker_pid'])
        harness.eventually(lambda:os.getpriority(os.PRIO_PROCESS,child)==19)
        os.kill(child,signal.SIGSTOP)
        scratch=Path(Path(f'/proc/{child}/cmdline').read_bytes().split(b'\0')[2].decode())
        self.assertEqual(scratch.parent,self.fixture.runtime/'thumbnail-jobs')
        (scratch/'partial-material.glb').write_bytes(b'interrupted material import')
        self.fixture.process.kill(); self.fixture.process.wait(timeout=5)
        harness.eventually(lambda:not Path(f'/proc/{child}').exists(),description='thumbnail exits when its owning server dies')
        self.assertTrue(scratch.exists(),'abrupt owner death leaves scratch for startup reclamation')
        self.restart_stopped()
        self.assertFalse(scratch.exists(),'the next locked service startup reclaims its orphaned job and intermediates')

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_thumbnail_resize_cancels_an_offscreen_tile_and_retries_on_return(self):
        for index in range(10): self.image(f'image-{index}')
        self.rpc({'method':'add','entry':{'id':'last-model','kind':'model','path':str(self.fixture.model)}})
        self.overview()
        state=harness.eventually(lambda:s if (s:=self.rpc({'method':'list'}))['presentation']['overview']['worker_id']=='last-model' and s['presentation']['overview']['worker_pid'] else None)
        child=int(state['presentation']['overview']['worker_pid'])
        harness.eventually(lambda:os.getpriority(os.PRIO_PROCESS,child)==19)
        os.kill(child,signal.SIGSTOP)
        self.addCleanup(lambda:os.kill(child,signal.SIGKILL) if Path(f'/proc/{child}').exists() else None)
        input=XInput(); self.addCleanup(input.close)
        input.x.XResizeWindow.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_uint,ctypes.c_uint]
        input.x.XResizeWindow(input.display,input.window,1000,350); input.x.XFlush(input.display)
        harness.eventually(lambda:all(t['id']!='last-model' for t in self.rpc({'method':'list'})['presentation']['overview']['tiles']))
        harness.eventually(lambda:not Path(f'/proc/{child}').exists(),timeout=3,description='resize cancels the offscreen thumbnail')
        state=self.settled(); self.assertEqual(state['presentation']['overview']['rendered'],1,'quiet background warming retries the cancelled offscreen model')
        self.assertFalse(list((self.fixture.runtime/'thumbnail-jobs').iterdir()),'cancelled jobs remove their scratch')
        input.press('End')
        state=harness.eventually(lambda:s if (s:=self.rpc({'method':'list'}))['presentation']['overview']['rendered']==1 and not s['presentation']['overview']['worker_active'] else None,timeout=20)
        self.assertEqual(state['active'],0)

    def test_projects_warm_registered_thumbnails_and_prefetch_neighbor_without_activating_them(self):
        paths=[]
        for index in range(24):paths.append(self.image(f'warm-{index}',project='warm-project'))
        self.rpc({'method':'project','id':'warm-project','current':'warm-0'})
        self.rpc({'method':'layout','layout':'projects'}); self.rpc({'method':'show'}); self.loaded('warm-0')
        self.preloaded('warm-1')
        state=self.settled(); overview=state['presentation']['overview']
        self.assertFalse(overview['visible']); self.assertTrue(overview['warming'])
        self.assertEqual(state['active'],1); self.assertEqual(overview['decoded'],23)
        self.assertTrue(all(not e['active'] and 'metrics' not in e for e in state['entries'] if e['id']!='warm-0'))
        before=overview['decoded']; png(paths[20],(200,80,40,255))
        harness.eventually(lambda:self.rpc({'method':'list'})['presentation']['overview']['decoded']>before)
        state=self.settled(); self.assertEqual(state['layout'],'projects'); self.assertEqual(state['selected'],'warm-0')
        self.assertLessEqual(state['cache']['thumbnail_bytes'],16*1024*1024)
        events=self.rpc({'method':'cache','action':'events','limit':128})['events']
        generated=[e for e in events if e['tier']=='thumbnail' and e['action']=='generated']
        self.assertTrue(generated); self.assertTrue(all(e['detail']['nice']==19 for e in generated))
        self.rpc({'method':'cache','action':'clear'}); time.sleep(.35)
        self.assertEqual(self.rpc({'method':'list'})['cache']['bytes'],0,'Completion callbacks must respect an explicit cache clear')
        self.rpc({'method':'select','id':'warm-1','make_current':True}); self.loaded('warm-1'); self.preloaded('warm-2')
        self.rpc({'method':'hide'}); time.sleep(.3)
        state=self.rpc({'method':'list'}); self.assertEqual(state['cache']['bytes'],0)
        self.assertFalse(state['presentation']['overview']['worker_active'])

    def test_project_neighbor_prefetch_waits_for_a_settled_target(self):
        for group in ('a','b'):
            for index in range(2):self.image(f'{group}-{index}',1024,project=group)
            self.rpc({'method':'project','id':group,'current':f'{group}-0'})
        self.rpc({'method':'layout','layout':'projects'}); self.rpc({'method':'show'})
        self.loaded('a-0'); self.loaded('b-0'); self.rpc({'method':'cache','action':'clear'})
        for _ in range(5):
            for key in ('a-0','b-0'):
                self.rpc({'method':'select','id':key}); time.sleep(.03)
        time.sleep(.05)
        state=self.rpc({'method':'list'})
        self.assertEqual(state['cache']['bytes'],0,'Sweeping across project panes must not decode every neighbor')
        self.assertFalse(state['preload']['worker_active'])
        self.preloaded('b-1')

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_visible_image_refresh_continues_while_a_model_thumbnail_is_stopped(self):
        self.rpc({'method':'add','entry':{'id':'slow','kind':'model','path':str(self.fixture.model)}})
        paths=[self.image(f'parallel-{i}') for i in range(8)]
        self.overview()
        state=harness.eventually(lambda:s if (s:=self.rpc({'method':'list'}))['presentation']['overview']['worker_pid'] else None)
        child=int(state['presentation']['overview']['worker_pid']); os.kill(child,signal.SIGSTOP)
        self.addCleanup(lambda:os.kill(child,signal.SIGKILL) if Path(f'/proc/{child}').exists() else None)
        harness.eventually(lambda:self.rpc({'method':'list'})['presentation']['overview']['decoded']==8)
        before=8; png(paths[0],(210,70,50,255))
        harness.eventually(lambda:self.rpc({'method':'list'})['presentation']['overview']['decoded']>before)
        state=self.rpc({'method':'list'}); overview=state['presentation']['overview']
        self.assertEqual(overview['worker_pid'],child); self.assertEqual(overview['image_peak'],2)
        self.assertEqual(state['active'],0)
        capture=self.root/'parallel-while-model-stopped.png'; self.rpc({'method':'capture','path':str(capture)})
        tile=next(t for t in overview['tiles'] if t['id']=='parallel-0')
        self.assertEqual(png_pixel(capture,tile['x']+tile['width']//2,tile['y']+40),(210,70,50))
        self.rpc({'method':'hide'})
        harness.eventually(lambda:not Path(f'/proc/{child}').exists())
        self.assertEqual(self.rpc({'method':'list'})['cache']['bytes'],0)

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_live_input_preempts_background_thumbnail_work(self):
        self.image('live',size=512); self.rpc({'method':'show'}); self.loaded('live')
        source=self.root/'background-input.obj'
        source.write_text('v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n')
        self.rpc({'method':'add','entry':{'id':'background-input','kind':'model','path':str(source)}})
        child=int(harness.eventually(lambda:self.rpc({'method':'list'})['presentation']['overview'].get('worker_pid')))
        os.kill(child,signal.SIGSTOP)
        self.addCleanup(lambda:os.kill(child,signal.SIGKILL) if Path(f'/proc/{child}').exists() else None)
        input=XInput()
        try:
            view=self.row('live')['viewport']
            input.drag(view['x']+view['width']//2,view['y']+view['height']//2,30,15)
            harness.eventually(lambda:not Path(f'/proc/{child}').exists())
            self.assertFalse(self.rpc({'method':'list'})['presentation']['overview']['warming'])
            harness.eventually(lambda:self.rpc({'method':'list'})['presentation']['overview']['rendered']==1,timeout=30)
            self.assertFalse(self.row('background-input')['active'])
        finally:input.close()

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_background_obj_manifest_revalidates_new_texture_membership_on_navigation(self):
        self.image('current'); self.rpc({'method':'show'}); self.loaded('current')
        first=self.root/'first-map.png'; second=self.root/'second-map.png'
        png(first,(210,40,30,255)); png(second,(30,210,60,255))
        mtl=self.root/'material.mtl'; mtl.write_text('newmtl paint\nKd 1 1 1\nmap_Kd first-map.png\n')
        obj=self.root/'background.obj'
        obj.write_text('mtllib material.mtl\nv -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\n'
            'vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nusemtl paint\nf 1/1 2/2 3/3 4/4\n')
        stamp=obj.stat().st_mtime_ns
        self.rpc({'method':'add','entry':{'id':'background','path':str(obj),'kind':'model'}})
        harness.eventually(lambda:self.rpc({'method':'list'})['presentation']['overview']['rendered']==1,timeout=25)
        self.settled(); self.assertFalse(self.row('background')['active'])
        mtl.write_text('newmtl paint\nKd 1 1 1\nmap_Kd second-map.png\n')
        self.overview()
        harness.eventually(lambda:self.rpc({'method':'list'})['presentation']['overview']['rendered']==2,timeout=25)
        self.settled()
        png(second,(60,40,210,255))
        harness.eventually(lambda:self.rpc({'method':'list'})['presentation']['overview']['rendered']==3,timeout=25)
        self.assertEqual(obj.stat().st_mtime_ns,stamp,'Manifest changes were tested without an OBJ save')

    def test_zoom_and_overview_save_failure_are_atomic(self):
        self.image("first"); self.image("second"); self.rpc({"method":"show"}); self.loaded("first")
        registry=self.fixture.state_dir/"previews.json"; backup=registry.with_suffix(".before")
        registry.rename(backup); registry.mkdir()
        try:
            with self.assertRaises(RuntimeError): self.rpc({"method":"layout","layout":"overview"})
            state=self.rpc({"method":"list"}); self.assertEqual(state["layout"],"single"); self.assertTrue(self.row("first")["active"])
        finally: registry.rmdir(); backup.rename(registry)
        self.overview(); registry.rename(backup); registry.mkdir()
        try:
            with self.assertRaises(RuntimeError): self.rpc({"method":"zoom","id":"second"})
            state=self.rpc({"method":"list"}); self.assertEqual(state["layout"],"overview"); self.assertEqual(state["selected"],"first")
            self.assertEqual(state["active"],0)
        finally: registry.rmdir(); backup.rename(registry)

    def test_waveform_ram_and_disk_reuse_without_retained_player(self):
        path=self.root/"audio.wav"; audio(path,duration=1)
        self.rpc({"method":"add","entry":{"id":"audio","path":str(path),"settings":{"paused":True,"muted":True}}})
        self.image("still"); self.rpc({"method":"show"})
        harness.eventually(lambda:self.row("audio").get("metrics",{}).get("waveform",{}).get("ready"))
        self.rpc({"method":"select","id":"still"}); self.loaded("still")
        self.assertNotIn("metrics",self.row("audio"))
        self.rpc({"method":"select","id":"audio"})
        harness.eventually(lambda:self.row("audio").get("metrics",{}).get("waveform",{}).get("cache_hit"))
        self.rpc({"method":"hide"}); self.assertEqual(self.rpc({"method":"list"})["cache"]["bytes"],0)
        self.rpc({"method":"show"})
        harness.eventually(lambda:self.row("audio").get("metrics",{}).get("waveform",{}).get("cache_hit"))
        self.assertGreater(self.rpc({"method":"list"})["cache"]["disk_hits"],0)

    def test_disk_thumbnail_restart_and_switch_measurements(self):
        self.image("large",2048); self.image("other"); self.rpc({"method":"show"}); self.loaded("large")
        cached=[]; decoded=[]
        for trial in range(7):
            self.rpc({"method":"select","id":"other"})
            start=time.perf_counter(); self.rpc({"method":"select","id":"large"}); self.loaded("large"); hit_ms=(time.perf_counter()-start)*1000
            self.assertTrue(self.row("large")["metrics"]["cache_hit"])
            self.rpc({"method":"select","id":"other"})
            path=self.root/"large.png"; stamp=path.stat(); os.utime(path,ns=(stamp.st_atime_ns,stamp.st_mtime_ns+1))
            start=time.perf_counter(); self.rpc({"method":"select","id":"large"}); self.loaded("large"); miss_ms=(time.perf_counter()-start)*1000
            self.assertFalse(self.row("large")["metrics"]["cache_hit"])
            if trial: cached.append(hit_ms); decoded.append(miss_ms)
        self.overview(); state=self.settled()
        report={"scope":"Six paired warm-file 2048x2048 synthetic PNG switches after one warmup; same navigation/widget/save boundary. Miss invalidates only source timestamps, preserving identical PNG bytes. Completed image load, no GPU presentation or general performance guarantee; ordinary host load uncontrolled.",
            "cache_switch_ms":cached,"decode_switch_ms":decoded,"cache_switch_median_ms":statistics.median(cached),
            "decode_switch_median_ms":statistics.median(decoded),"cache":state["cache"],"memory":memory(self.fixture.process.pid)}
        (self.root/"cache-measurements.json").write_text(json.dumps(report,indent=2))
        print(f"Matched image switch medians: cached {statistics.median(cached):.2f} ms, decoded {statistics.median(decoded):.2f} ms",flush=True)
        self.rpc({"method":"quit"}); self.fixture.process.wait(timeout=5)
        with self.fixture.log_path.open("ab") as log:
            self.fixture.process=subprocess.Popen([str(harness.BINARY),"--socket",str(self.fixture.socket_path),"--state",str(self.fixture.state_dir)],
                env=self.fixture.env,cwd=harness.PROJECT_ROOT,stdin=subprocess.DEVNULL,stdout=log,stderr=log,start_new_session=True)
        harness.eventually(self.fixture.ping)
        self.assertEqual(self.rpc({"method":"list"})["layout"],"overview")
        self.rpc({"method":"show"}); harness.eventually(lambda:self.rpc({"method":"list"})["cache"]["disk_hits"]>0)
        self.assertEqual(self.rpc({"method":"list"})["active"],0)

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_overview_new_assets_keep_visible_cards_anchored(self):
        self.fixture.add_model(); self.rpc({'method':'show'}); self.loaded('model')
        for project in ('early','late'):
            self.rpc({'method':'project','id':project,'label':project.title()})
            for i in range(20):self.image(f'{project}-{i}',project=project)
        self.rpc({'method':'layout','layout':'overview','group_projects':True})
        self.settled()
        controls=XInput(); self.addCleanup(controls.close)
        controls.press('End')
        harness.eventually(lambda:self.rpc({'method':'list'})['presentation']['overview']['scroll']>0)
        before=self.rpc({'method':'list'})['presentation']['overview']
        self.assertFalse(before['native'])
        self.assertEqual(before['native_ancestors'],0)
        anchor=before['tiles'][0]
        positions={tile['id']:(tile['x'],tile['y']) for tile in before['tiles']}
        for i in range(5):
            self.image(f'new-{i}',project='early')
            state=self.rpc({'method':'list'})
            self.assertEqual(state['active'],0)
            current=state['presentation']['overview']
            tile=next(tile for tile in current['tiles'] if tile['id']==anchor['id'])
            self.assertEqual((tile['x'],tile['y']),(anchor['x'],anchor['y']))
            for tile in current['tiles']:
                if tile['id'] in positions:self.assertEqual((tile['x'],tile['y']),positions[tile['id']])
        self.settled()
        state=self.rpc({'method':'list'})
        final=next(tile for tile in state['presentation']['overview']['tiles'] if tile['id']==anchor['id'])
        self.assertEqual(final['y'],anchor['y'])
        self.rpc({'method':'capture','path':str(self.root/'overview-stable-arrivals.png')})

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_native_overview_frames_click_keyboard_scroll_and_responsive(self):
        self.fixture.add_model(); self.rpc({"method":"show"}); model=self.loaded("model")
        renderer=model["metrics"]["renderer"]
        self.assertFalse(any(x in renderer.lower() for x in ("llvmpipe","softpipe","software","unknown")),renderer)
        (self.root/"renderer.json").write_text(json.dumps({"renderer":renderer}))
        for i in range(40): self.image(f"texture-{i}",color=(35+i*4,110,200-i*2,255),label="Very long texture filename for truncation" if i==0 else f"Texture {i+1}")
        clip=self.root/"clip.mp4"; video(clip,duration=1)
        self.rpc({"method":"add","entry":{"id":"video","path":str(clip),"settings":{"paused":True,"muted":True}}})
        self.rpc({"method":"select","id":"video"}); self.loaded("video")
        self.rpc({"method":"select","id":"model"}); self.loaded("model")
        input=XInput(); self.addCleanup(input.close)
        input.chord("Control_L","0")
        harness.eventually(lambda:self.rpc({"method":"list"})["layout"]=="overview")
        harness.eventually(lambda:self.rpc({"method":"list"})["cache"]["disk_entries"]>=8)
        state=self.settled(); self.assertEqual(state["active"],0)
        self.assertFalse(state['presentation']['overview']['native'])
        self.assertEqual(state['presentation']['overview']['native_ancestors'],0)
        self.assertTrue(all("metrics" not in r for r in state["entries"]))
        capture=self.root/"overview-light.png"; self.rpc({"method":"capture","path":str(capture)})
        tile=next(t for t in state["presentation"]["overview"]["tiles"] if t["id"]=="texture-0")
        pixel=png_pixel(capture,tile["x"]+tile["width"]//2,tile["y"]+40)
        self.assertLess(sum(abs(a-b) for a,b in zip(pixel,(35,110,200))),12,"tile shows actual image pixels")
        model_tile=state["presentation"]["overview"]["tiles"][0]
        input.click(model_tile["x"]+30,model_tile["y"]+40)
        harness.eventually(lambda:self.rpc({"method":"list"})["layout"]=="single"); self.loaded("model")
        input.chord("Control_L","0"); harness.eventually(lambda:self.rpc({"method":"list"})["layout"]=="overview")
        input.press("Right"); input.press("Return")
        harness.eventually(lambda:self.rpc({"method":"list"})["layout"]=="single")
        self.assertEqual(self.rpc({"method":"list"})["selected"],"texture-0"); self.loaded("texture-0")
        self.overview(); self.rpc({"method":"theme","mode":"dark"}); self.settled()
        self.rpc({"method":"capture","path":str(self.root/"overview-dark.png")})
        input.x.XResizeWindow.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_uint,ctypes.c_uint]
        input.x.XResizeWindow(input.display,input.window,1818,1024); input.x.XFlush(input.display)
        harness.eventually(lambda:self.rpc({"method":"list"})["presentation"]["width"]==1818)
        self.settled()
        self.rpc({"method":"capture","path":str(self.root/"overview-wide.png")})
        input.x.XResizeWindow(input.display,input.window,540,380); input.x.XFlush(input.display); time.sleep(.3)
        self.rpc({"method":"capture","path":str(self.root/"overview-small.png")})
        self.assertGreaterEqual(self.rpc({"method":"list"})["presentation"]["overview"]["columns"],2)
        input.press("End"); time.sleep(.1)
        harness.eventually(lambda:any(t["id"]=="texture-39" for t in self.rpc({"method":"list"})["presentation"]["overview"]["tiles"]))
        self.rpc({"method":"capture","path":str(self.root/"overview-scrolled.png")})
        input.x.XResizeWindow(input.display,input.window,1000,720); input.x.XFlush(input.display)
        self.rpc({"method":"close_all"}); self.rpc({"method":"capture","path":str(self.root/"overview-empty.png")})

if __name__=="__main__": unittest.main(verbosity=2)
