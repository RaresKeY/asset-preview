#!/usr/bin/env python3
"""Canonical document/video thumbnails through the real shared queue."""
from pathlib import Path
import os
import subprocess
import sys
import unittest

import inspection_integration as harness
from integration import video


class ContentThumbnails(unittest.TestCase):
    def setUp(self):
        self.fixture = harness.InspectionIntegration(methodName="runTest")
        self.fixture._testMethodName = self._testMethodName
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root, self.rpc = self.fixture.root, self.fixture.rpc

    def state(self):
        return self.rpc({"method": "list"})

    def ready(self, *ids):
        def loaded():
            state = self.state()
            tiles = {tile["id"]: tile for tile in state["presentation"]["overview"]["tiles"]}
            return state if all(tiles.get(key, {}).get("thumbnail_ready") for key in ids) else None
        return harness.eventually(loaded, timeout=25, description="content thumbnails")

    def capture(self, name):
        path = self.root / (name + ".png")
        self.rpc({"method": "capture", "path": str(path)})
        return harness.png_rgb(path)

    def add(self, path, key, **extra):
        self.rpc({"method": "add", "entry": {"id": key, "path": str(path), "project": "content", **extra}})

    def test_document_code_video_queue_and_gallery(self):
        markdown = self.root / "guide.md"
        markdown.write_text("# Asset guide\n\nA rendered **Markdown** preview.\n\n- One\n- Two\n\n```cpp\nreturn 42;\n```\n")
        code = self.root / "example.py"
        code.write_text("# Source preview\nclass Example:\n    def value(self):\n        return 42\n" * 8)
        clip = self.root / "scene.mp4"
        video(clip, source="color=c=red:s=320x180:r=12,drawbox=color=blue:t=fill:enable='gte(t,1)'", duration=4)
        wide = self.root / "wide.mp4"
        video(wide, source="color=c=blue:s=320x180:r=12,setsar=2", duration=1)
        marker = self.root / "must-not-run"
        self.add(markdown, "markdown", command=["/usr/bin/touch", str(marker)], cwd=str(self.root))
        self.add(code, "code")
        self.add(clip, "video", settings={"paused": True})
        self.add(wide, "wide", settings={"paused": True})
        self.rpc({"method": "layout", "layout": "overview"})
        self.rpc({"method": "show"})
        state = self.ready("markdown", "code", "video", "wide")
        self.assertEqual(state["active"], 0)
        self.assertFalse(marker.exists(), "Thumbnails ran an asset generator")
        maps = Path(f"/proc/{self.fixture.process.pid}/maps").read_text()
        self.assertNotIn("libmpv.so", maps, "Overview started a player")
        self.assertNotIn("asset-preview-f3d.so", maps, "Document/video thumbnails loaded a 3D engine")
        self.assertLessEqual(state["presentation"]["overview"]["render_worker_limit"], 1)
        _, _, rows = self.capture("content-overview")
        tiles = {tile["id"]: tile for tile in state["presentation"]["overview"]["tiles"]}
        def center_color(key):
            tile = tiles[key]
            x, y = tile["x"]+tile["width"]//2, tile["y"]+55
            return tuple(rows[y][x*3:x*3+3])
        red, green, blue = center_color("video")
        self.assertGreater(blue, 180, "Expected the blue midpoint, not the red opening frame")
        self.assertLess(red, 60)
        tile = tiles["wide"]
        x = tile["x"]+tile["width"]//2
        blue_height = sum(rows[y][x*3+2] > 180 and rows[y][x*3] < 60
                          for y in range(tile["y"]+6, tile["y"]+106))
        self.assertTrue(43 <= blue_height <= 47, "Video thumbnail must preserve display aspect ratio, including SAR")
        generated = state["presentation"]["overview"]["rendered"]
        self.assertGreaterEqual(generated, 4)

        # Disk reuse and source invalidation work without visiting an asset.
        self.rpc({"method": "hide"})
        self.assertEqual(self.state()["cache"]["thumbnail_bytes"], 0)
        self.rpc({"method": "show"})
        self.ready("markdown", "code", "video", "wide")
        self.assertEqual(self.state()["presentation"]["overview"]["rendered"], generated)
        code.write_text("# Updated source\n" + "print('fresh bytes')\n"*20)
        harness.eventually(lambda: self.state()["presentation"]["overview"]["rendered"] > generated, timeout=15)
        self.ready("code")

        self.rpc({"method": "select", "id": "code"})
        self.rpc({"method": "layout", "layout": "gallery", "project_filter": "content"})
        harness.eventually(lambda: self.fixture.row("code").get("metrics", {}).get("loads"))
        self.rpc({"method": "cache", "action": "clear"})
        self.ready("code", "markdown", "video")
        self.assertEqual(self.state()["active"], 1)
        self.capture("content-gallery")
        if harness.GPU:
            self.rpc({"method": "select", "id": "video"})
            metrics = harness.eventually(lambda: self.fixture.row("video").get("metrics", {}) if
                self.fixture.row("video").get("metrics", {}).get("loads") else None)
            self.assertTrue(metrics["paused"])
            self.assertTrue(metrics["renderer"])
            self.assertNotIn("llvmpipe", metrics["renderer"].lower())
            self.rpc({"method": "cache", "action": "clear"})
            state = self.ready("markdown", "code", "video", "wide")
            _, _, rows = self.capture("video-gallery")
            tile = next(t for t in state["presentation"]["overview"]["tiles"] if t["id"] == "video")
            x, y = tile["x"]+tile["width"]//2, tile["y"]+45
            self.assertGreater(rows[y][x*3+2], 180, "Selected video's tile must retain the canonical midpoint")
            self.assertLess(rows[y][x*3], 60)
            self.assertEqual(self.state()["active"], 1)
        self.rpc({"method": "hide"})
        harness.eventually(lambda: not self.state()["presentation"]["overview"]["worker_active"])
        self.assertEqual(self.state()["active"], 0)
        self.assertEqual(self.state()["cache"]["thumbnail_bytes"], 0)

    def test_invalid_documents_and_video_keep_placeholders(self):
        for key, suffix, data in (("binary", ".txt", b"\0binary"), ("broken", ".mp4", b"not a video"),
                                  ("oversize", ".md", b"x"*(2*1024*1024+1))):
            path = self.root/(key+suffix)
            path.write_bytes(data)
            self.add(path, key)
        self.rpc({"method": "layout", "layout": "overview"})
        self.rpc({"method": "show"})
        harness.eventually(lambda: self.state()["presentation"]["overview"]["visible_ready"] and
                           not self.state()["presentation"]["overview"]["worker_active"], timeout=15)
        state = self.state()
        self.assertEqual(state["active"], 0)
        self.assertTrue(all(not tile["thumbnail_ready"] for tile in state["presentation"]["overview"]["tiles"]))
        self.assertEqual(state["cache"]["thumbnail_bytes"], 0)

    def test_video_decoder_cancels_with_window_and_owner(self):
        clip = self.root / "held.mp4"
        video(clip, duration=1)
        fake = self.root / "helpers"
        fake.mkdir()
        marker = self.root / "decoder-pids"
        decoder = fake / "ffmpeg"
        decoder.write_text(f"#!{sys.executable}\nimport os,time\nfrom pathlib import Path\n"
                           f"Path({str(marker)!r}).write_text(str(os.getpid())+' '+str(os.getppid()))\ntime.sleep(60)\n")
        decoder.chmod(0o700)
        self.rpc({"method": "quit"})
        self.fixture.process.wait(timeout=5)
        self.fixture.env["PATH"] = str(fake)+os.pathsep+self.fixture.env["PATH"]
        with self.fixture.log_path.open("ab") as log:
            self.fixture.process = subprocess.Popen([str(harness.BINARY), "--socket", str(self.fixture.socket_path),
                "--state", str(self.fixture.state_dir)], cwd=harness.PROJECT_ROOT, env=self.fixture.env,
                stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
        harness.eventually(self.fixture.ping)
        self.add(clip, "held")
        self.rpc({"method": "layout", "layout": "overview"})
        def dead(pid):
            status = Path(f"/proc/{pid}/stat")
            return not status.exists() or status.read_text().split(") ", 1)[1].startswith("Z")
        for action in ("hide", "owner_exit"):
            marker.unlink(missing_ok=True)
            self.rpc({"method": "show"})
            harness.eventually(marker.exists, description="held decoder startup")
            child, worker = map(int, marker.read_text().split())
            self.assertEqual(os.getpgid(child), worker, "Decoder escaped the owned thumbnail job group")
            if action == "hide":
                self.rpc({"method": "hide"})
                harness.eventually(lambda: not self.state()["presentation"]["overview"]["worker_active"])
                self.assertEqual(self.state()["cache"]["thumbnail_bytes"], 0)
            else:
                self.fixture.process.kill()
                self.fixture.process.wait(timeout=3)
            harness.eventually(lambda: dead(child) and dead(worker), timeout=3, description="owned decoder cancellation")


if __name__ == "__main__":
    unittest.main()
