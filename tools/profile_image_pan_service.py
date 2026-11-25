#!/usr/bin/env python3
"""Opt-in standalone-server acceptance: native drags during large-image reloads.

Run through tools/offscreen.sh with ASSET_PREVIEW_GPU_TEST=1. Uses only the
standard library and existing private-process/native-input test adapters.
Socket latency includes the event loop and compositor waits, not scanout.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
import inspection_integration as harness
from integration import XInput, png


def distribution(values):
    values = sorted(values)
    return {"samples": len(values), "p50_ms": values[(len(values)-1)//2],
            "p95_ms": values[int((len(values)-1)*.95)],
            "p99_ms": values[int((len(values)-1)*.99)], "max_ms": values[-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not harness.GPU:
        parser.error("Requires the private GPU/native-input lane")
    harness.BINARY = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"binary": str(harness.BINARY), "sha256": hashlib.sha256(harness.BINARY.read_bytes()).hexdigest(),
              "host_load": os.getloadavg(), "cases": [],
              "scope": "Final standalone binary; XTest motion at 125 Hz and socket round trips during reload. No scanout measurement."}
    for layout, count in (("single", 1), ("gallery", 1), ("grid", 4), ("compare", 2)):
        fixture = harness.InspectionIntegration(methodName="runTest")
        try:
            fixture.setUp()
            for i in range(count):
                path = fixture.root / f"large-{i}.png"
                png(path, (80+i*30, 160, 220, 127), 4096)
                fixture.rpc({"method": "add", "entry": {"id": f"large-{i}", "project": "pan",
                            "path": str(path), "image_view": {"zoom": 3, "pan": [0, 0]}}})
            if layout == "compare":
                fixture.rpc({"method": "compare", "ids": ["large-0", "large-1"], "camera_sync": True})
            else:
                fixture.rpc({"method": "layout", "layout": layout, "project_filter": "pan", "grid_size": 4})
            fixture.rpc({"method": "show"})
            harness.eventually(lambda: all(fixture.row(f"large-{i}").get("metrics", {}).get("loads") for i in range(count)))
            native = XInput()
            try:
                native.x.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
                native.x.XResizeWindow(native.display, native.window, 1400, 850)
                native.x.XFlush(native.display)
                time.sleep(.5)
                viewport = fixture.row("large-0")["viewport"]
                x, y = viewport["x"]+viewport["width"]//2, viewport["y"]+viewport["height"]//2
                initial = fixture.row("large-0")["metrics"]["image_view"]
                target = f"large-{count-1}"
                samples, completions = [], []
                for repeat in range(3):
                    def inject():
                        native.motion(x, y)
                        native.xt.XTestFakeButtonEvent(native.display, 1, 1, 0)
                        start = time.monotonic()
                        for move in range(150):
                            native.motion(x+move//3, y+move//6)
                            native.x.XFlush(native.display)
                            time.sleep(max(0, start+(move+1)*.008-time.monotonic()))
                        native.xt.XTestFakeButtonEvent(native.display, 1, 0, 0)
                        native.x.XFlush(native.display)
                    thread = threading.Thread(target=inject)
                    thread.start()
                    try:
                        time.sleep(.2)
                        loads = fixture.row(target)["metrics"]["loads"]
                        fixture.rpc({"method": "reload", "id": target})
                        while thread.is_alive():
                            start = time.perf_counter()
                            fixture.rpc({"method": "ping"})
                            samples.append((time.perf_counter()-start)*1000)
                            time.sleep(.002)
                    finally:
                        thread.join()
                    harness.eventually(lambda: fixture.row(target)["metrics"]["loads"] == loads+1)
                    completions.append(fixture.row(target)["metrics"]["load_timing"])
                state = fixture.row("large-0")["metrics"]
                assert state["image_view"] != initial, "Native drag failed to change framing"
                assert state["decoded_width"] == 4096 and state["decoded_height"] == 4096
                assert state["paint_backend"] == "opengl"
                if layout == "compare":
                    assert state["image_view"] == fixture.row("large-1")["metrics"]["image_view"]
                renders = state["renders"]
                time.sleep(.2)
                assert fixture.row("large-0")["metrics"]["renders"] == renders, "Idle rendering continued"
                fixture.rpc({"method": "capture", "path": str((args.output / f"{layout}.png").resolve())})
                case = {"layout": layout, "views": count, "viewport": viewport, "socket_rtt": distribution(samples),
                        "loads": completions, "state": state,
                        "memory": Path(f"/proc/{fixture.process.pid}/status").read_text()}
                report["cases"].append(case)
                print(json.dumps({"layout": layout, "socket_rtt": case["socket_rtt"]}), flush=True)
                # Exercise teardown and reentry in the actual binary as well.
                framing = state["image_view"]
                fixture.rpc({"method": "hide"})
                assert fixture.rpc({"method": "list"})["active"] == 0
                fixture.rpc({"method": "show"})
                harness.eventually(lambda: fixture.row("large-0").get("metrics", {}).get("loads"))
                assert fixture.row("large-0")["metrics"]["image_view"] == framing
            finally:
                native.close()
        finally:
            if hasattr(fixture, "log_path") and fixture.log_path.exists():
                shutil.copy2(fixture.log_path, args.output / f"{layout}-server.log")
            fixture.doCleanups()
    (args.output / "report.json").write_text(json.dumps(report, indent=2)+"\n")


if __name__ == "__main__":
    main()
