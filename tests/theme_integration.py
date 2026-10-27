#!/usr/bin/env python3
"""Private-daemon theme persistence and native menu acceptance checks."""
from __future__ import annotations
import contextlib
import ctypes
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

from integration import XInput, eventually
from inspection_fixture import write_fixture

ROOT = Path(__file__).resolve().parents[1]
GPU = os.environ.get("ASSET_PREVIEW_GPU_TEST") == "1"


class ThemeIntegration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ap-theme-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root / "state"
        self.state.mkdir(mode=0o700)
        self.registry = self.state / "previews.json"
        self.socket = self.root / "preview.sock"
        self.log = self.root / "server.log"
        self.process = None
        self.addCleanup(self.retain)
        self.addCleanup(self.stop)
        self.start()

    def start(self):
        env = os.environ.copy()
        env.update(QT_QPA_PLATFORM="xcb" if GPU else "offscreen", QT_SCALE_FACTOR="1", QT_XCB_NO_XI2="1")
        with self.log.open("ab") as log:
            self.process = subprocess.Popen([str(ROOT / "build/asset-preview-server"), "--socket", str(self.socket),
                                             "--state", str(self.state)], env=env, stdout=log, stderr=log,
                                            stdin=subprocess.DEVNULL, start_new_session=True)
        eventually(self.ready)

    def ready(self):
        if self.process.poll() is not None:
            raise AssertionError(f"Server exited {self.process.returncode}: {self.log.read_text()}")
        try:
            return self.rpc({"method": "ping"})
        except OSError:
            return False

    def rpc(self, request):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(15)
            connection.connect(str(self.socket))
            connection.sendall(json.dumps(request).encode() + b"\n")
            data = bytearray()
            while b"\n" not in data:
                block = connection.recv(65536)
                if not block or len(data) + len(block) > 1024 * 1024:
                    raise RuntimeError("Incomplete or excessive theme test response")
                data.extend(block)
        result = json.loads(data.split(b"\n", 1)[0])
        if not result["ok"]:
            raise RuntimeError(result["error"])
        return result

    def stop(self):
        if not self.process or self.process.poll() is not None:
            return
        with contextlib.suppress(OSError, RuntimeError):
            self.rpc({"method": "quit"})
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)

    def retain(self):
        if not os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR"):
            return
        target = Path(os.environ["ASSET_PREVIEW_EVIDENCE_DIR"]) / self._testMethodName
        target.mkdir(parents=True, exist_ok=True)
        for path in [self.log, *self.root.glob("*.png")]:
            shutil.copy2(path, target / path.name)

    def theme(self, mode=None):
        return self.rpc({"method": "theme", **({"mode": mode} if mode is not None else {})})

    def capture(self, name):
        self.rpc({"method": "capture", "path": str(self.root / f"{name}.png")})

    def row(self):
        return self.rpc({"method": "list"})["entries"][0]

    def test_01_persistence_validation_and_save_failure(self):
        original = self.theme()
        self.assertEqual(original["theme"], "auto")
        for mode in ("dark", "light", "auto"):
            self.assertEqual(self.theme(mode)["theme"], mode)
            self.assertEqual(json.loads(self.registry.read_text())["theme"], mode)
            self.assertEqual(self.registry.stat().st_mode & 0o077, 0)
            self.stop(); self.start()
            restored = self.theme()
            self.assertEqual(restored["theme"], mode)
            self.assertEqual(restored["effective_theme"], original["effective_theme"] if mode == "auto" else mode)
            self.assertFalse(self.rpc({"method": "list"})["window_visible"])
            self.rpc({"method": "close_all"})
            self.assertEqual(self.theme()["theme"], mode)
        before = self.registry.read_bytes()
        for invalid in ("system", "Dark", "", False, 1, [], {}, None):
            with self.assertRaisesRegex(RuntimeError, "Theme must be"):
                self.rpc({"method": "theme", "mode": invalid})
            self.assertEqual(self.registry.read_bytes(), before)
            self.assertEqual(self.theme(), original)
        self.registry.rename(self.root / "saved.json")
        self.registry.mkdir()
        with self.assertRaises(RuntimeError):
            self.theme("dark")
        self.assertEqual(self.theme(), original)
        self.registry.rmdir()
        (self.root / "saved.json").rename(self.registry)

    def test_02_legacy_and_invalid_saved_theme(self):
        self.stop()
        self.registry.write_text(json.dumps({"version": 1, "entries": []}))
        self.start()
        self.assertEqual(self.theme()["theme"], "auto")
        self.stop()
        for invalid in ("sepia", False, None):
            data = json.dumps({"version": 1, "entries": [], "theme": invalid})
            self.registry.write_text(data)
            with self.assertRaisesRegex(AssertionError, "Server exited 1:"):
                self.start()
            self.assertEqual(self.registry.read_text(), data)

    @unittest.skipUnless(GPU, "Requires headless Gamescope and hardware OpenGL")
    def test_03_native_menu_and_model_lifecycle(self):
        model = self.root / "fixture.glb"
        write_fixture(model)
        self.rpc({"method": "add", "entry": {"id": "model", "path": str(model)}})
        self.rpc({"method": "show"})
        eventually(lambda: self.row().get("metrics", {}).get("loads", 0) > 0)
        initial = self.row()
        renderer = initial["metrics"]["renderer"]
        self.assertFalse(any(word in renderer.lower() for word in ("llvmpipe", "softpipe", "software", "unknown")), renderer)
        print("Theme verification renderer:", renderer, flush=True)
        controls = XInput()
        self.addCleanup(controls.close)
        def open_theme():
            controls.press("Escape"); controls.press("Escape"); time.sleep(.1)
            controls.shortcut("s"); time.sleep(.1)
            controls.press("Home"); controls.press("Right"); time.sleep(.2)
        def choose(steps):
            open_theme()
            controls.press("Home")
            for _ in range(steps):
                controls.press("Down")
            controls.press("Return")
        self.capture("auto-window")
        for mode, steps in (("dark", 1), ("light", 2), ("auto", 0)):
            choose(steps)
            try:
                eventually(lambda: self.theme()["theme"] == mode)
            except AssertionError:
                self.capture(f"failed-{mode}-selection")
                self.fail(f"Native menu selected {self.theme()} instead of {mode}")
            time.sleep(.2)
            self.capture(f"{mode}-window-chosen")
            current = self.row()
            self.assertEqual(current["metrics"]["loads"], initial["metrics"]["loads"])
            self.assertEqual(current["metrics"]["camera"], initial["metrics"]["camera"])
            self.assertEqual(current["settings"], initial["settings"])
            time.sleep(.2)
            renders = self.row()["metrics"]["renders"]
            time.sleep(.4)
            self.assertEqual(self.row()["metrics"]["renders"], renders)
            open_theme()
            self.capture(f"{mode}-settings")
            controls.press("Escape"); controls.press("Escape")
        self.registry.rename(self.root / "saved.json")
        self.registry.mkdir()
        choose(1); time.sleep(.2)
        self.assertEqual(self.theme()["theme"], "auto")
        self.capture("failed-theme-save")
        open_theme()
        self.capture("failed-theme-menu")
        controls.press("Escape"); controls.press("Escape")
        self.registry.rmdir()
        (self.root / "saved.json").rename(self.registry)
        self.rpc({"method": "inspect", "id": "model", "action": "open", "review": str(self.root / "review.json")})
        eventually(lambda: self.row().get("metrics", {}).get("backend") == "vtk-gltf")
        for mode in ("dark", "light"):
            self.theme(mode); time.sleep(.2)
            self.capture(f"{mode}-inspection")
        controls.x.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
        controls.x.XResizeWindow(controls.display, controls.window, 540, 380)
        controls.x.XFlush(controls.display)
        for mode in ("dark", "light"):
            self.theme(mode); time.sleep(.2)
            self.capture(f"{mode}-small-inspection")
        self.rpc({"method": "hide"})
        self.theme("auto")
        state = self.rpc({"method": "list"})
        self.assertEqual(state["active"], 0)
        self.assertEqual(state["watched_files"], 0)
        self.assertNotIn("metrics", self.row())


if __name__ == "__main__":
    unittest.main(verbosity=2)
