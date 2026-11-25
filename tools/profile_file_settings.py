#!/usr/bin/env python3
"""Finite, isolated file settings latency, including registry commits.

Run under tools/offscreen.sh with an explicit server binary. Each invocation
owns fresh state and its server; it never connects to the ordinary preview.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from files_integration import FilesIntegration


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    os.environ["ASSET_PREVIEW_BINARY"] = str(binary)
    os.environ["ASSET_PREVIEW_FILES_NATIVE"] = "1"
    fixture = FilesIntegration(methodName="runTest")
    try:
        fixture.setUp()
        before, after = fixture.root / "before.cpp", fixture.root / "after.cpp"
        lines = [f"int value_{i} = {i}; // " + ("long comment " * 24 if i % 13 == 0 else "comment") + "\n" for i in range(1000)]
        before.write_text("".join(lines))
        after.write_text("".join(f"int value_{i} = {i+1}; // replacement\n" if i % 97 == 0 else line for i, line in enumerate(lines)))
        for i in range(16):
            fixture.rpc({"method": "add", "entry": {"id": f"spare-{i}", "kind": "file", "path": str(after)}})
        fixture.rpc({"method": "add", "entry": {"id": "comparison", "kind": "file", "path": str(after), "before": str(before),
            "settings": {"file_mode": "inline", "file_wrap": True}}})
        fixture.show("comparison")
        initial_loads = fixture.row("comparison")["metrics"]["loads"]
        load_before = os.getloadavg()
        cases = []
        for full in (False, True):
            fixture.rpc({"method": "settings", "id": "comparison", "settings": {"file_context": full}})
            samples = []
            for i in range(6):
                wrap = bool(i % 2)
                clock = time.perf_counter()
                fixture.rpc({"method": "settings", "id": "comparison", "settings": {"file_wrap": wrap}})
                response = (time.perf_counter() - clock) * 1000
                fixture.rpc({"method": "ping"})
                settled = (time.perf_counter() - clock) * 1000
                row = fixture.row("comparison")
                assert row["metrics"]["file_wrap"] == wrap
                assert row["metrics"]["loads"] == initial_loads
                registry = json.loads((fixture.state / "previews.json").read_text())
                saved = next(entry for entry in registry["entries"] if entry["id"] == "comparison")
                assert saved["settings"]["file_wrap"] == wrap
                if i >= 2:
                    samples.append({"wrap": wrap, "response_ms": response, "following_ping_ms": settled})
            cases.append({"full_context": full, "samples": samples})
        status = fixture.rpc({"method": "list"})
        assert status["active"] == 1
        print(json.dumps({"binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "load_before": load_before, "load_after": os.getloadavg(), "rows": 1000, "registrations": 17,
            "platform": status["platform"], "theme": status["effective_theme"], "viewport": row["viewport"],
            "scope": "settings RPC round trip includes validation, atomic registry commit, native view settings, status, overview and watchers; ping is an event-loop check, not scanout",
            "cases": cases}))
    finally:
        fixture.doCleanups()


if __name__ == "__main__":
    main()
