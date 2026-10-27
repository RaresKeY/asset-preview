#!/usr/bin/env python3
"""Private graph daemon: background registration, watched maps, publication and suspension."""
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]


def eventually(condition, message):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError(message)


def png(path, color):
    def chunk(kind, value):
        return struct.pack(">I", len(value)) + kind + value + struct.pack(">I", zlib.crc32(kind + value) & 0xffffffff)
    pixels = b"".join(b"\0" + bytes(color) * 32 for _ in range(32))
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 32, 32, 8, 2, 0, 0, 0))
    candidate = path.with_suffix(".candidate")
    candidate.write_bytes(data + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))
    candidate.replace(path)


with tempfile.TemporaryDirectory(prefix="ap-graph-") as temporary:
    root = Path(temporary)
    runtime, state = root / "runtime", root / "state"
    runtime.mkdir(mode=0o700)
    state.mkdir(mode=0o700)
    (root / "project.godot").write_text("config_version=5\n")
    (root / "materials").mkdir()
    (root / "assets/materials").mkdir(parents=True)
    recipe = root / "materials/fixture.ptex"
    recipe.write_text(json.dumps({"name": "fixture", "nodes": [
        {"name": "Surface", "type": "shader"}, {"name": "Material", "type": "material"}],
        "connections": [{"from": "Surface", "to": "Material", "from_port": 0, "to_port": 0}]}))
    albedo = root / "assets/materials/fixture_albedo.png"
    png(albedo, (92, 130, 76))
    def rpc(request):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(15)
            client.connect(str(runtime / "preview.sock"))
            client.sendall((json.dumps(request) + "\n").encode())
            data = b""
            while b"\n" not in data:
                chunk = client.recv(65536)
                if not chunk:
                    raise AssertionError("Daemon disconnected")
                data += chunk
            result = json.loads(data)
            assert result.get("ok"), result
            return result
    with (root / "server.log").open("w") as log:
        process = subprocess.Popen([str(ROOT / "build/asset-preview-server"), "--socket", str(runtime / "preview.sock"), "--state", str(state)],
                                   stdout=log, stderr=log, env={**os.environ, "QT_QPA_PLATFORM": "xcb"})
        try:
            eventually(lambda: (runtime / "preview.sock").exists(), "Socket not ready")
            rpc({"method": "add", "entry": {"id": "graph-fixture", "kind": "auto", "path": str(recipe)}})
            def entry():
                return next(e for e in rpc({"method": "list"})["entries"] if e["id"] == "graph-fixture")
            row = entry()
            assert row["kind"] == "graph", row
            assert not row["active"], "Registration opened or activated the graph"
            rpc({"method": "show"})
            eventually(lambda: entry().get("metrics", {}).get("graph_nodes") == 3, "Graph never loaded")
            before = entry()["metrics"]["graph_revisions"]
            sidecar = Path(str(recipe) + ".live.json")
            sidecar.write_text(json.dumps({"version": 1, "nodes": {"Surface": {"state": "running"}}, "operations": []}))
            eventually(lambda: entry().get("metrics", {}).get("graph_revisions", 0) > before, "Sidecar did not refresh")
            before = entry()["metrics"]["graph_revisions"]
            png(albedo, (140, 88, 60))
            eventually(lambda: entry().get("metrics", {}).get("graph_revisions", 0) > before, "Atomic map save did not refresh")
            eventually(lambda: entry().get("metrics", {}).get("material_preview", {}).get("engine"), "No shaded material engine")
            assert entry()["metrics"]["inspector_side"] == "right", "Inspector stayed below graph"
            model = root / "preview model.glb"
            geometry = struct.pack('<12f12f8f6I', -1,0,-1,1,0,-1,1,0,1,-1,0,1, *([0,1,0]*4), 0,0,1,0,1,1,0,1, 0,2,1,0,3,2)
            model_doc = {"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
                         "buffers":[{"byteLength":len(geometry)}],"bufferViews":[{"buffer":0,"byteOffset":offset,"byteLength":length} for offset,length in [(0,48),(48,48),(96,32),(128,24)]],
                         "accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-1,0,-1],"max":[1,0,1]},
                                      {"bufferView":1,"componentType":5126,"count":4,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":4,"type":"VEC2"},{"bufferView":3,"componentType":5125,"count":6,"type":"SCALAR"}],
                         "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3}]}]}
            def save_model():
                text=json.dumps(model_doc).encode();text+=b' '*(-len(text)%4)
                data=struct.pack('<III',0x46546c67,2,28+len(text)+len(geometry))+struct.pack('<II',len(text),0x4e4f534a)+text+struct.pack('<II',len(geometry),0x004e4942)+geometry
                candidate=model.with_suffix('.candidate');candidate.write_bytes(data);candidate.replace(model)
            save_model();original=model.read_bytes()
            rpc({"method":"settings","id":"graph-fixture","settings":{"preview_model":str(model)}})
            eventually(lambda: entry().get("metrics",{}).get("material_preview",{}).get("material_model")==str(model), "Selected model did not receive the material")
            assert model.read_bytes()==original, "Applying material modified the source model"
            assert entry()["metrics"]["material_preview"]["triangles"]==2, "Preview rendered a sphere instead of the model"
            before_model=entry()["metrics"]["material_preview"]["loads"]
            model_doc['nodes'][0]['name']='Updated live model';save_model()
            eventually(lambda: entry().get("metrics",{}).get("material_preview",{}).get("loads",0)>before_model, "Selected model save did not refresh")
            image = root / "graph.png"
            rpc({"method": "capture", "id": "graph-fixture", "path": str(image)})
            assert image.exists() and image.stat().st_size > 1000, "Graph snapshot failed"
            if len(sys.argv)>1:
                shutil.copyfile(image, sys.argv[1])
            for i in range(3):
                rpc({"method": "add", "entry": {"id": f"compact-fixture-{i}", "kind": "image", "path": str(albedo)}})
            rpc({"method": "layout", "layout": "grid", "grid_size": 4})
            eventually(lambda: not entry().get("metrics", {}).get("material_preview"), "Compact graph retained a hidden shaded renderer")
            assert entry()["metrics"]["graph_nodes"] == 3, "Compact presentation lost the graph"
            rpc({"method": "layout", "layout": "single"})
            eventually(lambda: entry().get("metrics", {}).get("material_preview", {}).get("engine"), "Expanded graph did not restore the shaded preview")
            rpc({"method": "hide"})
            eventually(lambda: not entry()["active"], "Hidden graph stayed active")
            row = entry()
            assert not row.get("metrics"), row
            rpc({"method": "show"})
            eventually(lambda: entry().get("metrics", {}).get("graph_nodes") == 3, "Graph failed to restore")
            print("PASS: auto detection, background registration, live sidecar/map updates, shaded output, snapshot, compact mode and hidden-resource release")
        except BaseException:
            log.flush()
            print((root / "server.log").read_text()[-6000:])
            raise
        finally:
            process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
