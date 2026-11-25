#!/usr/bin/env python3
"""A delayed watcher and failed-import retry must not restart the current import."""
from pathlib import Path
import json
import time
import unittest
from unittest.mock import patch

import inspection_integration as harness
import thumbnail_publication as fixture_backend


@unittest.skipUnless(harness.GPU, harness.GPU_REASON)
class ModelReloadPublication(unittest.TestCase):
    setUpClass = classmethod(fixture_backend.ThumbnailPublication.setUpClass.__func__)

    def test_watcher_and_retry_preserve_inflight_recovery(self):
        fixture = harness.InspectionIntegration(methodName="runTest")
        fixture._testMethodName = self._testMethodName
        self.addCleanup(fixture.doCleanups)
        with patch.object(harness, "BINARY", self.binary):
            fixture.setUp()
        model = fixture.root / "reload.gltf"
        def valid(revision):
            return json.dumps({"asset": {"version": "2.0"}, "scene": 0,
                               "scenes": [{"nodes": [0]}], "nodes": [{}],
                               "extras": {"revision": revision}})
        def publish(text):
            candidate = model.with_suffix(".candidate")
            candidate.write_text(text)
            candidate.replace(model)
        publish(valid(1))
        fixture.rpc({"method": "add", "entry": {"id": "model", "path": str(model)}})
        fixture.rpc({"method": "layout", "layout": "single"})
        fixture.rpc({"method": "show"})
        harness.eventually(lambda: fixture.row("model").get("metrics", {}).get("loads") == 1)
        harness.eventually(lambda: not fixture.row("model")["metrics"]["command_pending"])
        self.addCleanup(lambda: Path(str(model) + ".release").touch())
        self.addCleanup(lambda: Path(str(model) + ".reload-release").touch())
        publish("broken export")
        harness.eventually(lambda: fixture.row("model")["status"].startswith("Waiting"))
        Path(str(model) + ".reload-hold").touch()
        publish(valid(2))
        # Start the current import before its output watcher and the failed
        # import's retry timer fire. Hold only the worker, keeping the GUI live.
        fixture.rpc({"method": "reload", "id": "model"})
        harness.eventually(lambda: Path(str(model) + ".reload-entered").exists())
        time.sleep(.7)
        self.assertTrue(fixture.row("model")["metrics"]["loading"])
        Path(str(model) + ".reload-release").touch()
        harness.eventually(lambda: not fixture.row("model")["metrics"]["loading"] and
                           not fixture.row("model")["metrics"]["command_pending"])
        time.sleep(.5)
        self.assertEqual(fixture.row("model")["metrics"]["loads"], 2,
                         "Watcher/retry imported the same recovering output twice")
        # A newer output must still supersede an import already in progress.
        Path(str(model) + ".reload-release").unlink()
        Path(str(model) + ".reload-entered").unlink()
        fixture.rpc({"method": "reload", "id": "model"})
        harness.eventually(lambda: Path(str(model) + ".reload-entered").exists())
        publish(valid(3))
        time.sleep(.7)
        Path(str(model) + ".reload-release").touch()
        harness.eventually(lambda: fixture.row("model")["metrics"]["loads"] == 4 and
                           not fixture.row("model")["metrics"]["loading"] and
                           not fixture.row("model")["metrics"]["command_pending"])
        self.assertEqual(fixture.row("model")["metrics"]["loads"], 4,
                         "A different output did not supersede the in-flight import")


if __name__ == "__main__":
    unittest.main()
