#!/usr/bin/env python3
"""One scene, four camera panes; exercise actual native input and lifecycle."""
import ctypes
import json
import time
import unittest
import inspection_integration as harness
from integration import XInput


@unittest.skipUnless(harness.GPU, harness.GPU_REASON)
class FourView(unittest.TestCase):
    def setUp(self):
        self.fixture = harness.InspectionIntegration(methodName="runTest")
        self.fixture._testMethodName = self._testMethodName
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.rpc = self.fixture.rpc
        self.row = self.fixture.row

    def settled(self):
        harness.eventually(lambda: self.row().get("metrics", {}).get("engine") and
                           not self.row()["metrics"].get("command_pending"))

    def toggle(self, enabled):
        self.rpc({"method": "settings", "id": "model", "settings": {"four_view": enabled}})
        harness.eventually(lambda: self.row()["metrics"].get("four_view") == enabled)
        self.settled()

    def same_camera(self, actual, expected):
        for key in expected:
            a,b=actual[key],expected[key]
            if isinstance(b,list):
                for x,y in zip(a,b): self.assertAlmostEqual(x,y,places=9)
            else: self.assertAlmostEqual(a,b,places=9)

    def capture(self, name):
        path = self.fixture.root / name
        self.rpc({"method": "capture", "path": str(path)})
        return harness.png_rgb(path)

    def test_four_cameras_input_capture_idle_and_reload(self):
        self.fixture.add_model(settings={"grid": False, "axes": False})
        self.rpc({"method": "show"})
        self.settled()
        renderer = self.row()["metrics"]["renderer"]
        self.assertTrue(renderer)
        self.assertFalse(any(x in renderer.lower() for x in ("unknown", "llvmpipe", "softpipe")))
        baseline = self.row()["metrics"]
        self.toggle(True)
        four = self.row()["metrics"]
        self.assertEqual(four["loads"], baseline["loads"])
        self.assertEqual(four["display_readbacks"], 0)
        self.same_camera(four["camera"], baseline["camera"])
        labels = self.row()["overlays"]["axis_panes"]
        self.assertEqual([p["label"] for p in labels], ["Perspective", "Front", "Right", "Top"])
        self.assertTrue(all(p["visible"] for p in labels))
        first = self.capture("four-view-desktop.png")
        self.settled()
        self.same_camera(self.row()["metrics"]["camera"], baseline["camera"])
        # All four crops contain different, nonempty views, beyond overlay chrome.
        viewport = self.row()["viewport"]
        crops = []
        for pane in range(4):
            x = viewport["x"] + pane % 2 * viewport["width"] // 2
            y = viewport["y"] + pane // 2 * viewport["height"] // 2
            rows = first[2][y + 100:y + viewport["height"] // 2 - 40]
            crop = b"".join(row[(x+40)*3:(x+viewport["width"]//2-40)*3] for row in rows)
            self.assertGreater(len(set(crop)), 20)
            crops.append(crop)
        self.assertEqual(len(set(crops)), 4)
        inputs = XInput(self.rpc)
        self.addCleanup(inputs.close)
        viewport = self.row()["viewport"]
        for pane in range(4):
            x = viewport["x"] + (pane % 2 * 2 + 1) * viewport["width"] // 4
            y = viewport["y"] + (pane // 2 * 2 + 1) * viewport["height"] // 4
            inputs.scroll(x, y)
            harness.eventually(lambda: self.row()["metrics"]["input_pane"] == pane)
            self.settled()
            if pane:
                self.assertGreater(self.row()["metrics"]["axis_panes"][pane-1]["zoom"], 1)
            inputs.drag(x, y, 25, 15)
            self.settled()
            if pane:
                harness.eventually(lambda: self.row()["metrics"]["axis_panes"][pane-1]["pan"] != [0, 0])
        self.capture("four-view-interacted.png")
        self.settled()
        before = self.row()["metrics"]
        time.sleep(.6)
        self.assertEqual(self.row()["metrics"]["renders"], before["renders"], "4 View rendered while idle")
        self.assertEqual(self.row()["metrics"]["display_readbacks"], before["display_readbacks"])
        self.assertEqual(self.row()["metrics"]["loads"], baseline["loads"])
        self.toggle(False)
        self.assertTrue(all(not p["visible"] for p in self.row()["overlays"]["axis_panes"]))
        self.toggle(True)
        self.rpc({"method": "hide"})
        harness.eventually(lambda: self.rpc({"method": "list"})["active"] == 0)
        self.rpc({"method": "show"})
        self.settled()
        self.assertTrue(self.row()["settings"]["four_view"])
        self.assertTrue(self.row()["metrics"]["four_view"])
        inputs.x.XResizeWindow.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_uint,ctypes.c_uint]
        inputs.x.XResizeWindow(inputs.display,inputs.window,640,480); inputs.x.XFlush(inputs.display)
        time.sleep(.3)
        self.settled()
        self.capture("four-view-narrow.png")
        inputs.chord("Control_L","Alt_L","q")
        harness.eventually(lambda: not self.row()["metrics"]["four_view"])
        inputs.chord("Control_L","Alt_L","q")
        harness.eventually(lambda: self.row()["metrics"]["four_view"])
        (self.fixture.root / "four-view-results.json").write_text(json.dumps({"renderer": renderer, "metrics": self.row()["metrics"]}, indent=2))

    def test_projection_axes_material_and_validation(self):
        self.fixture.add_model(settings={"four_view":True,"grid":False,"orthographic":True})
        self.rpc({"method":"show"}); self.settled()
        for up,front in (("y","-z"),("z","-y")):
            self.rpc({"method":"settings","id":"model","settings":{"up_axis":up,"front_axis":front}})
            self.settled()
            before=self.row()["metrics"]["camera"]
            self.capture("four-view-ortho-"+up+".png")
            self.same_camera(self.row()["metrics"]["camera"],before)
            self.toggle(False); self.toggle(True)
            self.same_camera(self.row()["metrics"]["camera"],before)
        for patch in ({"four_view":"yes"},):
            with self.assertRaises(RuntimeError): self.rpc({"method":"settings","id":"model","settings":patch})
        with self.assertRaises(RuntimeError):
            self.rpc({"method":"defaults","group":"3d","settings":{"four_view":True}})
        image=self.fixture.root/"texture.png"
        from integration import png
        png(image,(180,100,70))
        with self.assertRaises(RuntimeError):
            self.rpc({"method":"add","entry":{"id":"image","kind":"image","path":str(image),"settings":{"four_view":True}}})
        self.fixture.inspect("open",review=str(self.fixture.review))
        harness.eventually(lambda: self.row().get("metrics",{}).get("backend")=="vtk-gltf" and self.row()["metrics"].get("engine"))
        self.assertTrue(all(not p["visible"] for p in self.row()["overlays"]["axis_panes"]))
        with self.assertRaises(RuntimeError):
            self.rpc({"method":"settings","id":"model","settings":{"four_view":False}})
        self.fixture.inspect("close")
        self.settled()
        self.rpc({"method":"add","entry":{"id":"material","kind":"material","path":str(image),"settings":{"four_view":True}}})
        self.rpc({"method":"select","id":"material"})
        harness.eventually(lambda: self.fixture.row("material").get("metrics",{}).get("engine") and
                           not self.fixture.row("material")["metrics"].get("command_pending"))
        self.assertTrue(self.fixture.row("material")["metrics"]["four_view"])
        self.capture("four-view-material.png")

    def test_animation_pose_and_pause(self):
        from animation_fixture import write_animation
        write_animation(self.fixture.model)
        self.fixture.add_model(settings={"four_view":True,"grid":False,"paused":True})
        self.rpc({"method":"show"}); self.settled()
        start=self.capture("four-view-animation-start.png")
        self.rpc({"method":"seek","id":"model","seconds":1.5}); self.settled()
        posed=self.capture("four-view-animation-pose.png")
        self.assertNotEqual(start[2],posed[2],"Seeking must change the rendered four-pane pose")
        self.assertAlmostEqual(self.row()["metrics"]["position"],1.5)
        self.rpc({"method":"settings","id":"model","settings":{"paused":False,"loop":True}})
        harness.eventually(lambda: self.row()["metrics"].get("animation_timer"))
        before=self.row()["metrics"]
        time.sleep(.4)
        after=self.row()["metrics"]
        self.assertGreater(after["renders"],before["renders"])
        self.assertEqual(after["display_readbacks"],before["display_readbacks"])
        self.rpc({"method":"settings","id":"model","settings":{"paused":True}}); self.settled()
        paused=self.row()["metrics"]
        time.sleep(.3)
        self.assertEqual(self.row()["metrics"]["renders"],paused["renders"])

if __name__ == "__main__":
    unittest.main()
