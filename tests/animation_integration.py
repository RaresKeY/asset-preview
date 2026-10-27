#!/usr/bin/env python3
"""Animated 3D publication, rendered deformation, native input and idle lifetime."""
import ctypes
import json
import os
import subprocess
from pathlib import Path
import time
import unittest

import integration as f
from animation_fixture import write_animation


def rpc(request):
    # Native commands return after enqueueing. Observe completion and rendered
    # publication before checking a pose or capturing pixels.
    if request.get("method")=="capture":
        f.eventually(lambda: all(not e.get("metrics",{}).get("command_pending") and
            not e.get("metrics",{}).get("loading") for e in f.client.rpc({"method":"list"})["entries"] if e["active"]))
    result=f.client.rpc(request)
    if request.get("method") in ("settings","seek","camera_view","reload") and request.get("id"):
        key=request["id"]
        f.eventually(lambda: not f.row(key).get("metrics",{}).get("command_pending") and not f.row(key).get("metrics",{}).get("loading"),timeout=25)
    return result


class AnimationIntegration(unittest.TestCase):
    setUpClass=classmethod(f.PreviewIntegration.setUpClass.__func__)
    tearDownClass=classmethod(f.PreviewIntegration.tearDownClass.__func__)
    setUp=f.PreviewIntegration.setUp
    add=f.PreviewIntegration.add
    show=f.PreviewIntegration.show

    def tearDown(self):
        if os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR"):
            target=Path(os.environ["ASSET_PREVIEW_EVIDENCE_DIR"]);target.mkdir(parents=True,exist_ok=True)
            (target/(self._testMethodName+".json")).write_text(json.dumps(rpc({"method":"list"}),indent=2))

    def test_01_registration_validation_and_per_asset_settings(self):
        path=self.root/"waiting-animation.glb"
        rpc({"method":"add","entry":{"id":"animation","path":str(path),
            "settings":{"animation":1,"animation_speed":.5,"paused":True,"loop":False,"animation_stepped":True,"animation_fps":12}}})
        self.assertEqual(f.row("animation")["kind"],"model")
        for patch in ({"animation":-1},{"animation":1.5},{"animation":256},{"animation":True},
                      {"animation_speed":0},{"animation_speed":5},{"animation_speed":"fast"},
                      {"animation_fps":0},{"animation_fps":121},{"animation_fps":12.5},{"animation_fps":True},{"animation_stepped":1}):
            with self.subTest(patch=patch),self.assertRaises(RuntimeError):
                rpc({"method":"settings","id":"animation","settings":patch})
        rpc({"method":"settings","id":"animation","settings":{"animation":2,"paused":False,"animation_speed":2}})
        self.assertEqual(f.row("animation")["settings"]["animation"],2)
        preferences=rpc({"method":"list"})["preferences"].get("3d",{})
        self.assertFalse(set(preferences)&{"animation","animation_speed","animation_stepped","animation_fps","paused","loop"})
        for patch in ({"animation":1},{"animation_speed":2},{"animation_stepped":True},{"animation_fps":24}):
            with self.assertRaises(RuntimeError):rpc({"method":"defaults","group":"3d","settings":patch})
        with self.assertRaises(RuntimeError):rpc({"method":"seek","id":"animation","seconds":1})
        rpc({"method":"quit"});f.eventually(lambda:not f.client.running());f.client.start()
        self.assertEqual(f.row("animation")["settings"]["animation_speed"],2)
        self.assertTrue(f.row("animation")["settings"]["animation_stepped"])
        self.assertEqual(f.row("animation")["settings"]["animation_fps"],12)

    @unittest.skipUnless(f.GPU,"Hardware F3D skeletal, node and morph deformation")
    def test_02_rendered_rig_node_and_morph_poses(self):
        target=Path(os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR",self.root/"poses"));target.mkdir(parents=True,exist_ok=True)
        for kind in ("rig","node","morph"):
            with self.subTest(kind=kind):
                path=self.root/(kind+".glb");write_animation(path,kind)
                rpc({"method":"add","entry":{"id":kind,"path":str(path),"settings":{"paused":True,"grid":False}}})
                rpc({"method":"select","id":kind});self.show()
                f.eventually(lambda:f.row(kind).get("metrics",{}).get("loads",0)==1,timeout=25)
                state=f.row(kind);metrics=state["metrics"]
                self.assertEqual(metrics["animations"],[kind+" deformation","Slide"])
                self.assertAlmostEqual(metrics["duration"],2)
                self.assertTrue(metrics["paused"]);self.assertFalse(metrics["animation_timer"])
                self.assertFalse(any(x in metrics["renderer"].lower() for x in ("llvmpipe","softpipe","unknown")))
                self.assertTrue(state["overlays"]["animation"]["visible"])
                def capture(suffix):
                    image=target/(kind+"-"+suffix+".png");image.unlink(missing_ok=True)
                    rpc({"method":"capture","id":kind,"path":str(image)})
                    view=f.row(kind)["viewport"]
                    # Compare only rendered content. Timeline labels, status,
                    # focus and toolbar changes cannot satisfy this assertion.
                    crop=f"crop={view['width']}:{view['height']-180}:{view['x']}:{view['y']+80}"
                    return subprocess.run(["ffmpeg","-nostdin","-loglevel","error","-i",str(image),
                        "-vf",crop,"-frames:v","1","-f","rawvideo","-pix_fmt","rgb24","-threads","1","pipe:1"],
                        check=True,capture_output=True).stdout
                first=capture("start");self.assertTrue(first==capture("start-repeat"))
                rpc({"method":"seek","id":kind,"seconds":1.8})
                self.assertAlmostEqual(f.row(kind)["metrics"]["position"],1.8)
                self.assertTrue(first!=capture("deformed"),"The rendered mesh must deform, not just advance a counter")
                rpc({"method":"settings","id":kind,"settings":{"animation":1}})
                self.assertEqual(f.row(kind)["metrics"]["animation"],1)
                self.assertAlmostEqual(f.row(kind)["metrics"]["position"],0)
                rpc({"method":"seek","id":kind,"seconds":1.8})
                self.assertTrue(capture("slide")!=first)
                rpc({"method":"settings","id":kind,"settings":{"animation":0}})
                reset=capture("reset")
                difference=sum(abs(a-b)>5 for a,b in zip(first,reset))/len(first)
                self.assertLess(difference,.002,"Switching clips must restore the start pose")
                with self.assertRaises(RuntimeError):rpc({"method":"settings","id":kind,"settings":{"animation":2}})
                self.assertEqual(f.row(kind)["metrics"]["loads"],3)
                time.sleep(.3);renders=f.row(kind)["metrics"]["renders"];cpu=f.ticks(self.pid)
                time.sleep(.7)
                self.assertEqual(f.row(kind)["metrics"]["renders"],renders)
                self.assertLessEqual(f.ticks(self.pid)-cpu,3)

    @unittest.skipUnless(f.GPU,"Native animated-model transport and visible-only frames")
    def test_03_playback_native_controls_and_lifecycle(self):
        path=self.root/"playback.glb";write_animation(path)
        rpc({"method":"add","entry":{"id":"playback","path":str(path),"settings":{"paused":True,"loop":True}}})
        self.show();f.eventually(lambda:f.row("playback").get("metrics",{}).get("loads",0)==1,timeout=25)
        input=f.XInput()
        def metrics():return f.row("playback")["metrics"]
        def click(name):
            control=f.row("playback")["controls"][name]
            self.assertTrue(control["visible"],name);self.assertTrue(control["enabled"],name)
            input.click(control["x"]+control["width"]//2,control["y"]+control["height"]//2)
        try:
            click("play");f.eventually(lambda:metrics()["position"]>.2)
            self.assertFalse(metrics()["paused"])
            click("play");f.eventually(lambda:metrics()["paused"])
            timeline=f.row("playback")["controls"]["timeline"]
            input.click(timeline["x"]+timeline["width"]//2,timeline["y"]+timeline["height"]//2)
            f.eventually(lambda:abs(metrics()["position"]-1)<.08)
            input.motion(timeline["x"]+timeline["width"]//4,timeline["y"]+timeline["height"]//2)
            input.xt.XTestFakeButtonEvent(input.display,1,1,0);input.x.XFlush(input.display)
            f.eventually(lambda:f.row("playback")["controls"]["timeline"]["dragging"])
            self.assertAlmostEqual(metrics()["position"],1,delta=.08)
            input.xt.XTestFakeButtonEvent(input.display,1,0,0);input.x.XFlush(input.display)
            f.eventually(lambda:abs(metrics()["position"]-.5)<.08)
            input.press("Right");f.eventually(lambda:abs(metrics()["position"]-7.0/12)<1e-7)
            input.press("Left");f.eventually(lambda:abs(metrics()["position"]-.5)<1e-7)
            input.press("End");f.eventually(lambda:abs(metrics()["position"]-2)<1e-7)
            input.press("Home");f.eventually(lambda:metrics()["position"]==0)
            self.assertTrue(metrics()["paused"])
            input.press("space");f.eventually(lambda:not metrics()["paused"])
            input.press("space");f.eventually(lambda:metrics()["paused"])
            click("speed");f.eventually(lambda:f.row("playback")["controls"]["speed"]["focused"])
            rpc({"method":"settings","id":"playback","settings":{"animation_speed":2}})
            f.eventually(lambda:f.row("playback")["controls"]["speed"]["value"]==2)
            rpc({"method":"settings","id":"playback","settings":{"animation_speed":1}})
            click("clip");f.eventually(lambda:rpc({"method":"list"})["options_open"])
            input.press("End");input.press("Return")
            f.eventually(lambda:metrics()["animation"]==1)
            view=f.row("playback")["viewport"]
            input.click(view["x"]+view["width"]//2,view["y"]+view["height"]//2)
            input.chord("Shift_L","Tab")
            f.eventually(lambda:f.row("playback")["controls"]["timeline"]["focused"])
            input.click(view["x"]+view["width"]//2,view["y"]+view["height"]//2)
            input.press("Tab");f.eventually(lambda:f.row("playback")["controls"]["play"]["focused"])
            input.press("Tab");f.eventually(lambda:f.row("playback")["controls"]["restart"]["focused"])
            input.click(view["x"]+view["width"]//2,view["y"]+view["height"]//2)
            input.press("space");f.eventually(lambda:not metrics()["paused"])
            input.press("space");f.eventually(lambda:metrics()["paused"])
            input.press("Home");f.eventually(lambda:metrics()["position"]<.01)
            rpc({"method":"settings","id":"playback","settings":{"loop":False,"animation_speed":4}})
            rpc({"method":"seek","id":"playback","seconds":1.8});click("play")
            f.eventually(lambda:metrics()["eof"] and metrics()["paused"])
            f.eventually(lambda:f.row("playback")["settings"]["paused"])
            click("play");f.eventually(lambda:not metrics()["paused"] and metrics()["position"]<1)
            click("play");f.eventually(lambda:metrics()["paused"])
            rpc({"method":"settings","id":"playback","settings":{"loop":True,"paused":False}})
            time.sleep(.8);self.assertFalse(metrics()["paused"]);self.assertFalse(metrics()["eof"])
            rpc({"method":"hide"});self.assertNotIn("metrics",f.row("playback"))
            self.show();f.eventually(lambda:f.row("playback").get("metrics",{}).get("loads",0)==1,timeout=25)
            rpc({"method":"settings","id":"playback","settings":{"paused":True,"animation_speed":1}})
            # Static and animated assets coexist; off-page animation owns no engine.
            self.add("static-image");rpc({"method":"select","id":"static-image"})
            self.assertNotIn("metrics",f.row("playback"))
            rpc({"method":"layout","layout":"grid"})
            f.eventually(lambda:f.row("playback").get("metrics",{}).get("loads",0)==1,timeout=25)
            self.assertTrue(f.row("playback")["metrics"]["paused"])
            # Native F3D animation and libmpv video/audio remain independent.
            movie=self.root/"mixed-video.mp4";f.video(movie,duration=4)
            track=self.root/"mixed-audio.wav";f.audio(track,duration=4)
            rpc({"method":"add","entry":{"id":"mixed-video","path":str(movie),"settings":{"paused":False,"muted":True}}})
            rpc({"method":"add","entry":{"id":"mixed-audio","path":str(track),"settings":{"paused":False,"muted":True}}})
            f.eventually(lambda:f.row("mixed-video").get("metrics",{}).get("loads")==1,timeout=25)
            f.eventually(lambda:f.row("mixed-audio").get("metrics",{}).get("loads")==1,timeout=25)
            rpc({"method":"settings","id":"playback","settings":{"paused":False}})
            before_animation=metrics()["renders"];before_video=f.row("mixed-video")["metrics"]["renders"]
            f.eventually(lambda:metrics()["renders"]>before_animation+2 and f.row("mixed-video")["metrics"]["renders"]>before_video+2)
            for key in ("playback","mixed-video","mixed-audio"):
                rpc({"method":"settings","id":key,"settings":{"paused":True}})
            for i in range(5):self.add("animation-grid-"+str(i))
            rpc({"method":"layout","layout":"grid","grid_size":3})
            input.x.XResizeWindow.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_uint,ctypes.c_uint]
            input.x.XResizeWindow(input.display,input.window,540,380);input.x.XFlush(input.display);time.sleep(.3)
            bar=f.row("playback")["overlays"]["animation"]
            header=f.row("playback")["overlays"]["name"]
            self.assertGreaterEqual(bar["y"],header["y"]+header["height"])
            for name in ("play","clip","timeline"):
                item=f.row("playback")["controls"][name]
                self.assertTrue(item["visible"])
                self.assertGreaterEqual(item["x"],bar["x"]);self.assertLessEqual(item["x"]+item["width"],bar["x"]+bar["width"])
        finally:input.close()

    @unittest.skipUnless(f.GPU,"Rendered stepped poses and native frame controls")
    def test_stepped_playback_and_frame_controls(self):
        path=self.root/"stepped.glb";write_animation(path)
        rpc({"method":"add","entry":{"id":"stepped","path":str(path),
            "settings":{"paused":True,"animation_stepped":True,"animation_fps":2,"grid":False}}})
        self.show();f.eventually(lambda:f.row("stepped").get("metrics",{}).get("loads")==1,timeout=25)
        def metrics():return f.row("stepped")["metrics"]
        def capture(name):
            image=self.root/(name+".png")
            rpc({"method":"capture","path":str(image)})
            view=f.row("stepped")["viewport"]
            crop=f"crop={view['width']}:{view['height']-220}:{view['x']}:{view['y']+80}"
            return subprocess.run(["ffmpeg","-nostdin","-loglevel","error","-i",str(image),"-vf",crop,
                "-frames:v","1","-f","rawvideo","-pix_fmt","rgb24","-threads","1","pipe:1"],check=True,capture_output=True).stdout
        rpc({"method":"seek","id":"stepped","seconds":.51})
        self.assertAlmostEqual(metrics()["sample_position"],.5);first=capture("held-first")
        rpc({"method":"seek","id":"stepped","seconds":.74})
        self.assertAlmostEqual(metrics()["sample_position"],.5)
        self.assertEqual(first,capture("held-second"),"A stepped interval must hold the rendered geometry")
        rpc({"method":"seek","id":"stepped","seconds":1.01})
        self.assertGreater(sum(a!=b for a,b in zip(first,capture("next-pose"))),500)
        input=f.XInput()
        try:
            for name,expected in (("previous",1.0),("previous",.5),("next",1.0)):
                control=f.row("stepped")["controls"][name]
                input.click(control["x"]+control["width"]//2,control["y"]+control["height"]//2)
                f.eventually(lambda:abs(metrics()["position"]-expected)<1e-7)
                self.assertTrue(metrics()["paused"]);self.assertFalse(metrics()["animation_timer"])
            view=f.row("stepped")["viewport"]
            input.click(view["x"]+view["width"]//2,view["y"]+view["height"]//2)
            input.press("period");f.eventually(lambda:abs(metrics()["position"]-1.5)<1e-7)
            input.press("comma");f.eventually(lambda:abs(metrics()["position"]-1)<1e-7)
            rpc({"method":"settings","id":"stepped","settings":{"paused":False}})
            f.eventually(lambda:metrics()["position"]>1.1)
            current=metrics();self.assertAlmostEqual(current["sample_position"]*2,round(current["sample_position"]*2))
            rpc({"method":"settings","id":"stepped","settings":{"paused":True,"animation_stepped":False}})
            rpc({"method":"seek","id":"stepped","seconds":.74})
            self.assertAlmostEqual(metrics()["sample_position"],.74)
            self.assertEqual(metrics()["loads"],1,"Sampling changes must reuse the loaded engine")
        finally:input.close()

    @unittest.skipUnless(f.GPU,"Frame-boundary scheduling on the hardware renderer")
    def test_low_rate_playback_sleeps_between_poses(self):
        path=self.root/"cadence.glb";write_animation(path)
        rpc({"method":"add","entry":{"id":"cadence","path":str(path),
            "settings":{"paused":False,"animation_stepped":True,"animation_fps":2,"loop":True}}})
        self.show();f.eventually(lambda:f.row("cadence").get("metrics",{}).get("loads")==1,timeout=25)
        time.sleep(.6)
        before=f.row("cadence")["metrics"]
        time.sleep(2.2)
        after=f.row("cadence")["metrics"]
        # Qt may repaint once more when the native overlay changes. Even with
        # that allowance, a 30 Hz renderer cannot pass this low-rate contract.
        self.assertLessEqual(after["renders"]-before["renders"],12)
        self.assertGreaterEqual(after["renders"]-before["renders"],3)
        self.assertFalse(after["paused"]);self.assertFalse(after["eof"])
        self.assertEqual(after["loads"],1)
        rpc({"method":"settings","id":"cadence","settings":{"animation_speed":4,"loop":False}})
        f.eventually(lambda:f.row("cadence")["metrics"]["eof"])
        final=f.row("cadence")["metrics"]
        self.assertEqual(final["sample_position"],final["duration"])
        self.assertTrue(final["paused"]);self.assertFalse(final["animation_timer"])

    @unittest.skipUnless(f.GPU,"Native frame stepping between low-rate pose deadlines")
    def test_frame_step_uses_position_after_pausing(self):
        path=self.root/"pause-step.glb";write_animation(path)
        rpc({"method":"add","entry":{"id":"pause-step","path":str(path),
            "settings":{"paused":True,"animation_stepped":True,"animation_fps":1}}})
        self.show();f.eventually(lambda:f.row("pause-step").get("metrics",{}).get("loads")==1,timeout=25)
        input=f.XInput()
        try:
            for native_key in (False,True):
                rpc({"method":"seek","id":"pause-step","seconds":1})
                rpc({"method":"settings","id":"pause-step","settings":{"paused":False}})
                time.sleep(.15)
                if native_key:
                    view=f.row("pause-step")["viewport"]
                    input.click(view["x"]+view["width"]//2,view["y"]+view["height"]//2)
                    input.press("comma")
                else:
                    control=f.row("pause-step")["controls"]["previous"]
                    input.click(control["x"]+control["width"]//2,control["y"]+control["height"]//2)
                f.eventually(lambda:f.row("pause-step")["metrics"]["paused"] and not f.row("pause-step")["metrics"]["command_pending"])
                self.assertEqual(f.row("pause-step")["metrics"]["position"],1,
                    "Previous frame used the stale displayed pose instead of the paused clock")
        finally:input.close()

    @unittest.skipUnless(f.GPU,"Animated source refresh and last-good publication")
    def test_04_reload_last_good_and_static_replacement(self):
        path=self.root/"refresh-animation.glb";write_animation(path,"rig")
        rpc({"method":"add","entry":{"id":"refresh","path":str(path),
            "settings":{"paused":True,"animation_speed":.5}}})
        self.show();f.eventually(lambda:f.row("refresh").get("metrics",{}).get("loads")==1,timeout=25)
        rpc({"method":"seek","id":"refresh","seconds":1})
        before=f.row("refresh")["metrics"]
        bad=path.with_suffix(".candidate");bad.write_bytes(b"incomplete animated export");bad.replace(path)
        f.eventually(lambda:f.row("refresh")["status"].startswith("Waiting"))
        current=f.row("refresh")["metrics"]
        self.assertEqual(current["loads"],before["loads"]);self.assertEqual(current["animations"],before["animations"])
        self.assertAlmostEqual(current["position"],1)
        rpc({"method":"settings","id":"refresh","settings":{"animation":1}})
        self.assertEqual(f.row("refresh")["metrics"]["animation"],0,"Rejected clip import keeps the last-good effective clip")
        write_animation(path,"node",duration=3)
        f.eventually(lambda:f.row("refresh")["metrics"]["loads"]==before["loads"]+1,timeout=25)
        current=f.row("refresh")["metrics"]
        self.assertEqual(current["animations"][0],"node deformation");self.assertAlmostEqual(current["duration"],3)
        self.assertEqual(current["animation"],1)
        self.assertAlmostEqual(current["position"],0);self.assertTrue(current["paused"])
        self.assertEqual(f.row("refresh")["settings"]["animation_speed"],.5)
        rpc({"method":"seek","id":"refresh","seconds":1.2})
        rpc({"method":"settings","id":"refresh","settings":{"materials":False}})
        self.assertAlmostEqual(f.row("refresh")["metrics"]["position"],1.2)
        loads=f.row("refresh")["metrics"]["loads"]
        time.sleep(.8)
        self.assertEqual(f.row("refresh")["metrics"]["loads"],loads,"A stale retry must not reload a recovered scene")
        self.assertAlmostEqual(f.row("refresh")["metrics"]["position"],1.2)
        # Keep ordinary static model behavior after an animated source is replaced.
        import inspection_fixture
        candidate=path.with_suffix(".candidate");candidate.write_bytes(inspection_fixture.fixture_bytes());candidate.replace(path)
        f.eventually(lambda:not f.row("refresh")["metrics"]["animations"],timeout=25)
        self.assertFalse(f.row("refresh")["overlays"]["animation"]["visible"])
        with self.assertRaises(RuntimeError):rpc({"method":"seek","id":"refresh","seconds":1})

    @unittest.skipUnless(f.GPU,"Imported outputs acknowledge delayed watcher notifications")
    def test_05_reload_before_watcher_preserves_selected_pose(self):
        path=self.root/"reload-before-watcher.glb";write_animation(path)
        rpc({"method":"add","entry":{"id":"reload","path":str(path),"settings":{"paused":True}}})
        self.show();f.eventually(lambda:f.row("reload").get("metrics",{}).get("loads")==1,timeout=25)
        write_animation(path,"node",duration=3)
        rpc({"method":"reload","id":"reload"})
        state=f.row("reload")["metrics"]
        self.assertEqual(state["animations"][0],"node deformation")
        self.assertEqual(state["loads"],2)
        rpc({"method":"seek","id":"reload","seconds":1.25})
        time.sleep(.8)
        state=f.row("reload")["metrics"]
        self.assertEqual(state["loads"],2,"A delayed watcher event must not reload already imported output")
        self.assertAlmostEqual(state["position"],1.25)


if __name__=="__main__":unittest.main(verbosity=2)
