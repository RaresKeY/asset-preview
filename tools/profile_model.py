#!/usr/bin/env python3
"""Finite full-service model import benchmark; run through tools/offscreen.sh.

Private state/socket, fixed saved camera, first frame and hide/reopen, bounded
idle checks and exact owned-child cleanup. No ordinary service or source writes.
Use alternating --binary paths for matched comparisons. --perf is attribution,
not a clean latency sample. Filesystem/driver caches are deliberately not flushed.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time


def digest(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def rpc(path, request, timeout=120):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(str(path))
        client.sendall((json.dumps(request) + "\n").encode())
        data = bytearray()
        while b"\n" not in data:
            block = client.recv(65536)
            if not block:
                raise RuntimeError("Private socket closed")
            data.extend(block)
            if len(data) > 8 * 1024 * 1024:
                raise RuntimeError("Oversized private response")
    result = json.loads(data.split(b"\n", 1)[0])
    if not result.get("ok"):
        raise RuntimeError(result)
    return result


def resources(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    result = {"cpu_seconds": (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")}
    for filename, keys in (("smaps_rollup", ("Rss:", "Pss:", "Private_Clean:", "Private_Dirty:")),
                           ("status", ("VmHWM:", "Threads:"))):
        for line in Path(f"/proc/{pid}/{filename}").read_text().splitlines():
            if line.startswith(keys):
                key, value, *_ = line.split()
                result[key[:-1]] = int(value)
    return result


def host():
    result = {"loadavg": Path("/proc/loadavg").read_text().strip()}
    for name, query in (("gpu", ["--query-gpu=name,utilization.gpu,memory.used,temperature.gpu,clocks.sm,power.draw",
                                "--format=csv,noheader"]),
                        ("compute_processes", ["--query-compute-apps=pid,process_name,used_memory",
                                               "--format=csv,noheader"])):
        sample = subprocess.run(["nvidia-smi", *query], capture_output=True, text=True, timeout=5)
        result[name] = sample.stdout.strip() if sample.returncode == 0 else sample.stderr.strip()
    return result


def inspect_model(path, key, prefix):
    def inspect(action="state", **fields):
        return rpc(path, {"method": "inspect", "id": key, "action": action, **fields})
    assert not inspect("open")["loaded"], "Inspection raised a hidden window"
    rpc(path, {"method": "show"})
    deadline = time.monotonic() + 120
    while not inspect().get("loaded"):
        assert time.monotonic() < deadline, "Inspection load timeout"
        time.sleep(.02)
    parts = []
    while True:
        page = inspect("parts", offset=len(parts), limit=100)
        parts.extend(page["items"])
        assert len(parts) <= 65536
        if not page["has_more"]:
            break
        assert page["items"], "Empty nonterminal parts page"
    prefix.with_suffix(".parts.json").write_text(json.dumps(parts, indent=2) + "\n")
    state = inspect()
    result = {"revision": state["revision"], "part_count": len(parts),
              "mapped": sum(bool(part["mapped"]) for part in parts),
              "parts_sha256": hashlib.sha256(json.dumps(parts, sort_keys=True).encode()).hexdigest()}
    target = next(part["id"] for part in parts if part["mapped"])
    inspect("select", target=target)
    inspect("cut", axis="x", position=0)
    cut = inspect()
    assert cut["selection"]["id"] == target and cut["cuts"]
    result["cut_state"] = {field: cut[field] for field in ("selection", "cuts", "hidden")}
    rpc(path, {"method": "capture", "path": str(prefix) + "-cut.png"})
    before = rpc(path, {"method": "list"})["entries"][0]["metrics"]["renders"]
    time.sleep(.75)
    after = rpc(path, {"method": "list"})["entries"][0]["metrics"]["renders"]
    assert after == before, "Idle inspection rendered"
    rpc(path, {"method": "hide"})
    assert rpc(path, {"method": "list"})["active"] == 0
    return result


def run(binary, registry, output, index, perf=False, inspection=False):
    prefix = output / f"{index:02d}-{binary.parent.name}"
    record = {"binary": str(binary), "server_sha256": digest(binary), "host_before": host()}
    record["modules_sha256"] = {p.name: digest(p) for p in binary.parent.glob("asset-preview-*.so")}
    with tempfile.TemporaryDirectory(prefix="ap-import-") as directory:
        root = Path(directory)
        state, runtime = root / "s", root / "r"
        state.mkdir(mode=0o700)
        runtime.mkdir(mode=0o700)
        (state / "previews.json").write_text(json.dumps(registry))
        path = runtime / "preview.sock"
        env = os.environ.copy()
        env.update(ASSET_PREVIEW_RUNTIME_DIR=str(runtime), ASSET_PREVIEW_STATE_DIR=str(state),
                   ASSET_PREVIEW_TEXTURE_CACHE=str(root / "textures"), XDG_CACHE_HOME=str(root / "cache"),
                   QT_QPA_PLATFORM="xcb", QT_SCALE_FACTOR="1", QT_XCB_NO_XI2="1")
        command = [str(binary), "--socket", str(path), "--state", str(state)]
        if perf:
            command = ["perf", "record", "-e", "cycles:u", "-F", "199", "--call-graph", "dwarf,8192", "--user-callchains",
                       "-o", str(prefix) + ".perf.data", "--", *command]
        with prefix.with_suffix(".log").open("w") as log:
            started = time.perf_counter()
            process = subprocess.Popen(command, env=env, stdout=log, stderr=log, start_new_session=True)
            try:
                deadline = time.monotonic() + 30
                while True:
                    try:
                        info = rpc(path, {"method": "ping"}, .2)
                        break
                    except (OSError, RuntimeError):
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise
                        time.sleep(.01)
                pid = info["pid"]
                if not perf:
                    assert pid == process.pid
                record["service_ready_ms"] = (time.perf_counter() - started) * 1000
                for event in ("first", "reopen"):
                    before = resources(pid)
                    start = time.perf_counter()
                    rpc(path, {"method": "show"})
                    deadline = time.monotonic() + 120
                    while True:
                        row = rpc(path, {"method": "list"})["entries"][0]
                        metrics = row.get("metrics", {})
                        if "backend unavailable" in row.get("status", "").lower() or "failed" in row.get("status", "").lower():
                            raise RuntimeError(row["status"])
                        if metrics.get("engine") and metrics.get("load_timing", {}).get("first_render_ms"):
                            break
                        if time.monotonic() > deadline:
                            raise RuntimeError(f"Frame readiness timeout: {row}")
                        time.sleep(.01)
                    ready_ms = (time.perf_counter() - start) * 1000
                    renderer = metrics["renderer"]
                    assert "NVIDIA" in renderer, renderer
                    after = resources(pid)
                    record[event] = {"ready_ms": ready_ms, "metrics": metrics, "resources_at_frame": after,
                                     "cpu_seconds": after["cpu_seconds"] - before["cpu_seconds"]}
                    print(json.dumps({"binary": binary.parent.name, "index": index, "event": event,
                                      "ready_ms": ready_ms, "phases": metrics["load_timing"]}), flush=True)
                    # Static idle: capture and hide costs are outside readiness.
                    time.sleep(.25)
                    idle = rpc(path, {"method": "list"})["entries"][0]
                    idle_resources = resources(pid)
                    time.sleep(.75)
                    settled = rpc(path, {"method": "list"})["entries"][0]
                    record[event]["idle_render_delta"] = settled["metrics"]["renders"] - idle["metrics"]["renders"]
                    record[event]["idle_cpu_seconds"] = resources(pid)["cpu_seconds"] - idle_resources["cpu_seconds"]
                    assert record[event]["idle_render_delta"] == 0
                    assert not settled["builds"] and not settled["building"]
                    record[event]["resources_settled"] = resources(pid)
                    rpc(path, {"method": "capture", "path": str(prefix) + f"-{event}.png"})
                    rpc(path, {"method": "hide"})
                    hidden = rpc(path, {"method": "list"})
                    assert hidden["active"] == 0 and "metrics" not in hidden["entries"][0]
                    record[event]["hidden_resources"] = resources(pid)
                if inspection:
                    record["inspection"] = inspect_model(path, registry["entries"][0]["id"], prefix)
                rpc(path, {"method": "quit"})
                process.wait(timeout=15)
                assert process.returncode == 0, process.returncode
            finally:
                if process.poll() is None:
                    assert os.getpgid(process.pid) == process.pid
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=5)
    record["host_after"] = host()
    prefix.with_suffix(".json").write_text(json.dumps(record, indent=2) + "\n")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", required=True, type=Path)
    parser.add_argument("--binary", action="append", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--perf", action="store_true")
    parser.add_argument("--inspection", action="store_true", help="Separate original-source Parts/Cuts parity check after timings")
    args = parser.parse_args()
    assert 1 <= args.rounds <= 10
    registry = copy.deepcopy(json.loads(args.registry.read_text()))
    assert len(registry["entries"]) == 1 and registry["layout"] == "single"
    entry = registry["entries"][0]
    assert entry["kind"] == "model" and not entry.get("command") and not entry.get("watch")
    assert entry["settings"].get("texture_quality", "original") == "original"
    assert not entry.get("inspection", {}).get("enabled")
    source = Path(entry["path"])
    original = digest(source)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"source": str(source), "source_sha256": original, "source_bytes": source.stat().st_size,
              "registry": registry, "instrumented_perf": args.perf, "runs": []}
    try:
        for iteration in range(args.rounds):
            binaries = args.binary if iteration % 2 == 0 else args.binary[::-1]
            for binary in binaries:
                report["runs"].append(run(binary.resolve(), registry, args.output, len(report["runs"]), args.perf, args.inspection))
                assert digest(source) == original, "Source changed"
                (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    finally:
        report["source_preserved"] = digest(source) == original
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
