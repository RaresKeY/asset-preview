#!/usr/bin/env python3
"""Hold a departure capture across cache and registration lifecycle changes."""
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

import inspection_integration as harness
from integration import png


@unittest.skipUnless(harness.GPU, harness.GPU_REASON)
class ThumbnailPublication(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="preview-capture-backend-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        cls.binary = cls.root / "asset-preview-server"
        shutil.copy2(harness.BINARY, cls.binary)
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "Qt6Widgets", "Qt6OpenGL"], text=True))
        subprocess.run(["c++", "-std=c++20", "-O2", "-fPIC", "-shared", "-Wall", "-Wextra", "-Werror",
                        str(harness.ROOT / "tests/thumbnail_worker_fixture.cpp"),
                        "-I" + str(harness.PROJECT_ROOT / "src"),
                        "-I" + str(harness.PROJECT_ROOT / "build/generated"),
                        "-o", str(cls.root / "asset-preview-f3d.so"), *flags], check=True)

    def setUp(self):
        self.fixture = harness.InspectionIntegration(methodName="runTest")
        self.fixture._testMethodName = self._testMethodName
        self.addCleanup(self.fixture.doCleanups)
        with patch.object(harness, "BINARY", self.binary):
            self.fixture.setUp()
        self.rpc = self.fixture.rpc
        self.model = self.fixture.root / "departure.obj"
        self.model.write_text("v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n")
        self.entry = {"id": "model", "path": str(self.model)}
        self.rpc({"method": "add", "entry": self.entry})
        image = self.fixture.root / "next.png"
        png(image)
        self.rpc({"method": "add", "entry": {"id": "next", "path": str(image)}})
        self.rpc({"method": "select", "id": "model"})
        self.rpc({"method": "show"})
        harness.eventually(lambda: self.fixture.row("model").get("metrics", {}).get("loads") == 1)
        harness.eventually(lambda: not self.fixture.row("model")["metrics"]["command_pending"])
        self.sequence = self.rpc({"method": "list"})["cache"]["diagnostics"]["last_seq"]
        # Always release before server cleanup, even if an assertion fails.
        self.addCleanup(lambda: Path(str(self.model) + ".release").touch())
        self.rpc({"method": "select", "id": "next"})
        harness.eventually(lambda: Path(str(self.model) + ".capture").exists(), description="capture barrier")

    def finish(self, expected):
        Path(str(self.model) + ".release").touch()
        harness.eventually(lambda: Path(str(self.model) + ".destroyed").exists(), description="capture and scene retirement")
        # Publication is queued before retirement. Allow the GUI event loop to
        # deliver it before inspecting both RAM admission and disk writes.
        time.sleep(.15)
        events = self.rpc({"method": "cache", "action": "events", "after": self.sequence})["events"]
        writes = [event for event in events if event["action"] in ("admit", "write") and
                  event.get("detail", {}).get("source") == str(self.model)]
        self.assertEqual(bool(writes), expected, writes)

    def test_departure_publishes_before_retirement(self):
        self.finish(True)

    def test_clear_discards_inflight_capture(self):
        self.rpc({"method": "cache", "action": "clear"})
        self.finish(False)

    def test_hide_then_show_discards_inflight_capture(self):
        self.rpc({"method": "hide"})
        self.rpc({"method": "show"})
        self.finish(False)

    def test_reused_registration_id_discards_inflight_capture(self):
        self.rpc({"method": "remove", "id": "model"})
        self.rpc({"method": "add", "entry": self.entry})
        self.finish(False)


if __name__ == "__main__":
    unittest.main()
