#!/usr/bin/env python3
"""Focused optimization regressions against an isolated, real native daemon.

Functional:
  QT_QPA_PLATFORM=offscreen python tests/optimization_integration.py
Hardware:
  tools/offscreen.sh env ASSET_PREVIEW_GPU_TEST=1 python tests/optimization_integration.py

The inspection fixture owns startup, private socket/state and exact child
cleanup. ASSET_PREVIEW_BINARY can select the owning checkout's current build.
No test connects to the ordinary user service or asserts a latency/PSS budget.
"""
from __future__ import annotations

import contextlib
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import struct
import sys
import time
import unittest
import uuid

import inspection_integration as inspection_tests
from inspection_fixture import fixture_document
from integration import png, png_pixel


GPU = inspection_tests.GPU
GPU_REASON = inspection_tests.GPU_REASON
eventually = inspection_tests.eventually
STATUSES = ("open", "in_progress", "resolved", "obsolete")


class OptimizationIntegration(unittest.TestCase):
    """Compose the fixture without inheriting its existing test methods."""

    def setUp(self):
        self.fixture = inspection_tests.InspectionIntegration(methodName="runTest")
        self.fixture._testMethodName = self._testMethodName
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.root = self.fixture.root
        self.rpc = self.fixture.rpc
        self.row = self.fixture.row
        self.inspect = self.fixture.inspect
        self.annotations = self.fixture.annotations

    def add(self, entry):
        return self.rpc({"method": "add", "entry": entry})

    def image(self, key="image", size=64, **fields):
        path = self.root / f"{key}.png"
        png(path, size=size)
        entry = {"id": key, "path": str(path), "kind": "image", **fields}
        self.add(entry)
        return path, entry

    def show_loaded(self, key="image"):
        self.rpc({"method": "show"})
        return eventually(lambda: self.row(key) if self.row(key).get("metrics", {}).get("loads", 0) else None,
                          timeout=30, description=f"loaded {key}")

    def prime_reload(self, key="image"):
        before = self.row(key)["revisions"]
        self.rpc({"method": "reload", "id": key})
        eventually(lambda: self.row(key)["revisions"] > before, timeout=30,
                   description=f"explicit reload of {key}")
        result = self.row(key)
        self.assertGreater(result["metrics"]["loads"], 1,
                           "A reset-to-one load counter must not pass a reconnect check")
        return result

    def assert_retained(self, before, after):
        for field in ("id", "path", "kind", "settings", "active", "revisions", "builds"):
            self.assertEqual(after[field], before[field], field)
        for field in ("loads", "camera", "image_view", "decoded_bytes", "width", "height"):
            if field in before["metrics"]:
                self.assertEqual(after["metrics"][field], before["metrics"][field], field)

    def hardware_renderer(self, key="model"):
        metrics = self.row(key)["metrics"]
        renderer = metrics.get("renderer", "")
        self.assertTrue(metrics.get("engine"), metrics)
        self.assertTrue(renderer, metrics)
        self.assertFalse(any(word in renderer.lower() for word in
                             ("llvmpipe", "softpipe", "software", "unknown")), renderer)
        (self.root / "renderer.json").write_text(json.dumps({"renderer": renderer,
                                                             "binary": str(inspection_tests.BINARY)}) + "\n")

    @contextlib.contextmanager
    def failed_registry_save(self):
        registry = self.fixture.state_dir / "previews.json"
        backup = registry.with_suffix(".rollback-test")
        registry.rename(backup)
        registry.mkdir()
        try:
            yield
        finally:
            registry.rmdir()
            backup.replace(registry)

    def test_batch_open_rolls_back_and_preserves_first_path_registration(self):
        path, _ = self.image()
        self.show_loaded()
        self.rpc({"method": "defaults", "group": "image", "settings": {"nearest": True}})
        # Multiple registrations may deliberately share a path. Open reuses the
        # first one and preserves its configuration and decoded publication.
        self.add({"id": "alias", "path": str(path), "kind": "image", "settings": {"nearest": False}})
        before = self.row("image")
        new = [str(self.root / f"new-{i}.png") for i in range(2)]
        with self.failed_registry_save():
            with self.assertRaises(RuntimeError):
                self.rpc({"method": "open", "paths": [*new, str(path)]})
            self.assertEqual([e["id"] for e in self.rpc({"method": "list"})["entries"]], ["image", "alias"])
            self.assert_retained(before, self.row("image"))
        opened = self.rpc({"method": "open", "paths": [*new, new[0], str(path)]})["ids"]
        self.assertEqual(opened[0], opened[2])
        self.assertEqual(opened[-1], "image")
        self.assertEqual(len(self.rpc({"method": "list"})["entries"]), 4)
        self.assertTrue(self.row(opened[0])["settings"]["nearest"])
        self.assert_retained(before, self.row("image"))
        # A failed selection change must roll back the new entries too.
        with self.failed_registry_save():
            with self.assertRaises(RuntimeError):
                self.rpc({"method": "open", "paths": [str(self.root / "third.png")]})
        self.assertEqual(len(self.rpc({"method": "list"})["entries"]), 4)
        self.assertTrue(self.row("image")["selected"])

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_full_resolution_import_keeps_original_parts_cuts_and_lifetime(self):
        image = self.root / "orm.png"
        png(image, (255, 100, 70, 255), size=1024)
        document, original_binary = fixture_document()
        binary = bytearray(original_binary)
        uv_offset = len(binary)
        binary.extend(b"".join(struct.pack("<ff", float(i % 4 in (1, 2)), float(i % 4 >= 2)) for i in range(24)))
        uv_accessor = len(document["accessors"])
        document["accessors"].append({"bufferView": len(document["bufferViews"]), "count": 24,
                                      "componentType": 5126, "type": "VEC2"})
        document["bufferViews"].append({"buffer": 0, "byteOffset": uv_offset, "byteLength": 24 * 8})
        for mesh in document["meshes"]:
            for primitive in mesh["primitives"]:
                primitive["attributes"]["TEXCOORD_0"] = uv_accessor
        image_bytes = image.read_bytes()
        document["images"] = [{"bufferView": len(document["bufferViews"]), "mimeType": "image/png"}]
        document["bufferViews"].append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(image_bytes)})
        binary.extend(image_bytes)
        document["textures"] = [{"source": 0}]
        document["materials"][0]["pbrMetallicRoughness"]["metallicRoughnessTexture"] = {"index": 0}
        for index in range(32):
            document["scenes"][0]["nodes"].append(len(document["nodes"]))
            document["nodes"].append({"name": f"Repeated full-resolution part {index}", "mesh": 0,
                                       "translation": [(index % 8) * 3, 0, (index // 8) * 3]})
        document["buffers"][0]["byteLength"] = len(binary)
        encoded = json.dumps(document, separators=(",", ":")).encode()
        encoded += b" " * (-len(encoded) % 4)
        binary += b"\0" * (-len(binary) % 4)
        chunks = struct.pack("<I4s", len(encoded), b"JSON") + encoded + struct.pack("<I4s", len(binary), b"BIN\0") + binary
        self.fixture.model.write_bytes(struct.pack("<4sII", b"glTF", 2, 12 + len(chunks)) + chunks)
        digest = hashlib.sha256(self.fixture.model.read_bytes()).hexdigest()
        self.fixture.add_model()
        first = self.show_loaded("model")
        self.hardware_renderer()
        self.assertEqual(first["metrics"]["texture_preview"]["status"], "original")
        if not first["metrics"]["gltf_import"]["available"]:
            self.skipTest("Installed F3D build has no optional strided-byte reader")
        self.assertGreater(first["metrics"]["gltf_import"]["byte_component_fills"], 0)
        self.assertFalse((self.root / "texture-cache").exists())
        camera, triangles = first["metrics"]["camera"], first["metrics"]["triangles"]
        self.inspect("open")
        eventually(lambda: self.inspect().get("loaded"), timeout=30)
        self.assertEqual(self.inspect()["revision"], digest)
        parts = self.inspect("parts", limit=100)["items"]
        self.assertEqual(len(parts), 36)
        self.assertTrue(all(part["mapped"] for part in parts))
        target = parts[0]["id"]
        self.inspect("select", target=target)
        self.inspect("cut", axis="x", position=0)
        before = self.row()["metrics"]["loads"]
        controls = self.rpc({"method": "list"})["presentation"]["mode_control"]
        native = inspection_tests.XInput()
        self.addCleanup(native.close)
        native.click(controls["x"] + controls["width"] // 2, controls["y"] + controls["height"] // 2)
        time.sleep(.1)
        for key in ("Home", "Down", "Down", "Return"):
            native.press(key)
        eventually(lambda: self.rpc({"method": "list"})["presentation"]["inspection_mode"] == "cuts")
        self.assertEqual(self.row()["metrics"]["loads"], before)
        self.assertEqual(self.inspect()["selection"]["id"], target)
        self.assertEqual(self.inspect("parts", limit=100)["items"], parts)
        self.inspect("close")
        eventually(lambda: self.row().get("metrics", {}).get("gltf_import", {}).get("byte_component_fills", 0) > 0)
        self.assertEqual(self.row()["metrics"]["triangles"], triangles)
        restored_camera = self.row()["metrics"]["camera"]
        for field, values in camera.items():
            if isinstance(values, list):
                for actual, expected in zip(restored_camera[field], values):
                    self.assertAlmostEqual(actual, expected, places=10)
            else:
                self.assertAlmostEqual(restored_camera[field], values, places=10)
        self.rpc({"method": "hide"})
        self.assertNotIn("metrics", self.row())
        self.assertEqual(hashlib.sha256(self.fixture.model.read_bytes()).hexdigest(), digest)
        reopened = self.show_loaded("model")
        self.assertGreater(reopened["metrics"]["gltf_import"]["byte_component_fills"], 0)

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_fast_textures_keep_original_inspection_identity_and_unload(self):
        image = self.root / "embedded.png"
        png(image, size=2048)
        document, original_binary = fixture_document()
        binary = bytearray(original_binary)
        uv_offset = len(binary)
        binary.extend(b"".join(struct.pack("<ff", float(i % 4 in (1, 2)), float(i % 4 >= 2)) for i in range(24)))
        uv_view = len(document["bufferViews"])
        document["bufferViews"].append({"buffer": 0, "byteOffset": uv_offset, "byteLength": 24 * 8})
        uv_accessor = len(document["accessors"])
        document["accessors"].append({"bufferView": uv_view, "count": 24, "componentType": 5126, "type": "VEC2"})
        for mesh in document["meshes"]:
            for primitive in mesh["primitives"]:
                primitive["attributes"]["TEXCOORD_0"] = uv_accessor
        image_offset = len(binary)
        binary.extend(image.read_bytes())
        document["images"] = [{"bufferView": len(document["bufferViews"]), "mimeType": "image/png"}]
        document["bufferViews"].append({"buffer": 0, "byteOffset": image_offset, "byteLength": image.stat().st_size})
        document["textures"] = [{"source": 0}]
        for material in document["materials"]:
            material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
        document["buffers"][0]["byteLength"] = len(binary)
        encoded = json.dumps(document, separators=(",", ":")).encode()
        encoded += b" " * (-len(encoded) % 4)
        binary += b"\0" * (-len(binary) % 4)
        chunks = struct.pack("<I4s", len(encoded), b"JSON") + encoded + struct.pack("<I4s", len(binary), b"BIN\0") + binary
        self.fixture.model.write_bytes(struct.pack("<4sII", b"glTF", 2, 12 + len(chunks)) + chunks)
        digest = hashlib.sha256(self.fixture.model.read_bytes()).hexdigest()
        self.fixture.add_model(settings={"texture_quality": "preview"})
        cache = self.root / "texture-cache"
        self.assertFalse(cache.exists(), "Hidden registration prepared textures")
        first = self.show_loaded("model")
        self.hardware_renderer()
        self.assertEqual(first["metrics"]["texture_preview"]["status"], "preview")
        self.assertFalse(first["metrics"]["texture_preview"]["cache_hit"])
        self.assertEqual(first["metrics"]["texture_preview"]["source_sha256"], digest)
        camera, triangles = first["metrics"]["camera"], first["metrics"]["triangles"]
        self.rpc({"method": "hide"})
        self.assertNotIn("metrics", self.row())
        second = self.show_loaded("model")
        self.assertTrue(second["metrics"]["texture_preview"]["cache_hit"])
        diagnostic=self.rpc({"method":"cache"})["cache"]["diagnostics"]["counters"]
        self.assertGreaterEqual(diagnostic["glb_disk.write"],1)
        self.assertGreaterEqual(diagnostic["glb_disk.hit"],1)
        self.assertGreaterEqual(diagnostic["glb_disk.prune_scan"],1)
        self.assertEqual(second["metrics"]["camera"], camera)
        self.assertEqual(second["metrics"]["triangles"], triangles)
        self.rpc({"method": "settings", "id": "model", "settings": {"textures": False}})
        disabled = self.row()["metrics"]["texture_preview"]
        self.assertEqual(disabled["requested"], "preview")
        self.assertEqual(disabled["status"], "original")
        self.rpc({"method": "settings", "id": "model", "settings": {"textures": True, "texture_quality": "original"}})
        self.assertEqual(self.row()["metrics"]["texture_preview"]["status"], "original")
        self.rpc({"method": "settings", "id": "model", "settings": {"texture_quality": "preview"}})
        self.assertTrue(self.row()["metrics"]["texture_preview"]["cache_hit"])
        self.inspect("open")
        eventually(lambda: self.inspect().get("loaded"), timeout=30)
        self.assertEqual(self.inspect()["revision"], digest)
        self.assertEqual(self.row()["metrics"]["backend"], "vtk-gltf")
        part = self.inspect("parts")["items"][0]
        self.inspect("select", target=part["id"])
        self.inspect("cut", axis="x", position=0)
        self.assertEqual(self.inspect()["selection"]["id"], part["id"])
        note = self.annotations("add", expected_revision=0, note={"text": "Original texture identity", "status": "open"})["note"]
        self.assertEqual(note["created_against"], digest)
        self.assertEqual(note["targets"][0]["id"], part["id"])
        self.rpc({"method": "hide"})
        self.assertNotIn("metrics", self.row())
        self.assertEqual(hashlib.sha256(self.fixture.model.read_bytes()).hexdigest(), digest)

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_repeated_groups_require_one_to_one_child_topology(self):
        document, binary = fixture_document()
        document["nodes"] = [
            {"name": "Repeated group", "children": [2, 3, 4]},
            {"name": "Repeated group", "children": [5, 6, 7]},
            *[{"name": "Part", "mesh": 0, "translation": [x, 0, 0]} for x in (0, 0, 4, 0, 4, 4)],
        ]
        document["scenes"] = [{"nodes": [0, 1]}]
        encoded = json.dumps(document, separators=(",", ":")).encode()
        encoded += b" " * (-len(encoded) % 4)
        binary += b"\0" * (-len(binary) % 4)
        chunks = struct.pack("<I4s", len(encoded), b"JSON") + encoded + struct.pack("<I4s", len(binary), b"BIN\0") + binary
        self.fixture.model.write_bytes(struct.pack("<4sII", b"glTF", 2, 12 + len(chunks)) + chunks)
        state = self.fixture.open_inspection()
        self.hardware_renderer()
        self.assertEqual(state["mapped_actors"], 2)
        self.assertEqual(state["unmapped_actors"], 4)
        parts = self.inspect("parts")["items"]
        self.assertEqual({part["node_index"] for part in parts if part["mapped"]}, {4, 5})
        for part in parts:
            if part["mapped"]:
                self.assertEqual(self.inspect("select", target=part["id"])["selection"]["node_index"], part["node_index"])

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_thousand_part_identity_and_incremental_cut_updates(self):
        document, binary = fixture_document()
        document["nodes"] = [{"name": f"Part{i:04d}", "mesh": 0,
                              "translation": [float(i % 32) * 3, float(i // 32) * 3, 0]}
                             for i in range(1000)]
        document["nodes"] += [{"name": "Ambiguous", "mesh": 0}] * 2
        document["scenes"] = [{"nodes": list(range(1002))}]
        encoded = json.dumps(document, separators=(",", ":")).encode()
        encoded += b" " * (-len(encoded) % 4)
        binary += b"\0" * (-len(binary) % 4)
        chunks = struct.pack("<I4s", len(encoded), b"JSON") + encoded + struct.pack("<I4s", len(binary), b"BIN\0") + binary
        self.fixture.model.write_bytes(struct.pack("<4sII", b"glTF", 2, 12 + len(chunks)) + chunks)
        state = self.fixture.open_inspection()
        self.hardware_renderer()
        self.assertEqual(state["mapped_actors"], 1000)
        self.assertEqual(state["unmapped_actors"], 2)
        self.assertLess(self.row()["metrics"]["mapping_comparisons"], 10000)
        parts = []
        for offset in range(0, 1002, 100):
            parts.extend(self.inspect("parts", offset=offset, limit=100)["items"])
        self.assertTrue(all(part["mapped"] for part in parts[:1000]))
        self.assertTrue(all(not part["mapped"] for part in parts[1000:]))
        self.inspect("select", target=parts[0]["id"])
        self.inspect("hide", target=parts[1]["id"])
        before = self.inspect()
        original = self.fixture.capture("before-cut.png")
        self.inspect("cut", axis="x", position=40, keep="positive")
        first = self.row()["metrics"]
        clipped = self.fixture.capture("first-cut.png")
        self.assertGreater(sum(a != b for a, b in zip(original, clipped)), 100)
        self.inspect("cut", axis="x", position=65, keep="negative")
        second = self.row()["metrics"]
        moved = self.fixture.capture("moved-cut.png")
        self.assertGreater(sum(a != b for a, b in zip(clipped, moved)), 100)
        self.assertEqual(first["cut_mapper_bindings"], second["cut_mapper_bindings"])
        self.assertEqual(second["cut_plane_updates"], first["cut_plane_updates"] + 1)
        self.assertEqual(second["selection_actor_updates"], first["selection_actor_updates"])
        after = self.inspect()
        for key in ("selected", "selection", "hidden", "camera", "orthographic"):
            self.assertEqual(after[key], before[key], key)
        self.assertEqual(first["loads"], second["loads"])
        self.inspect("cut", axis="x", position=65, enabled=False)
        self.assertEqual(original, self.fixture.capture("disabled-cut.png"))
        stable = self.fixture.settle()
        time.sleep(.3)
        self.assertEqual(self.row()["metrics"]["renders"], stable)

    def reconnect_after_publication(self, entry, publish):
        """Queue Add before the owned daemon can start watcher debounce.

        The private process is stopped only while writing and queuing one RPC.
        This exercises immediate registration freshness without relying on a
        wall-clock deadline or adding a debug operation to the public protocol.
        """
        process = self.fixture.process
        self.assertIsNone(process.poll())
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        connection.settimeout(30)
        try:
            os.kill(process.pid, signal.SIGSTOP)
            try:
                eventually(lambda: "\nState:\tT" in Path(f"/proc/{process.pid}/status").read_text(),
                           description="owned server stopped for publication")
                publish()
                connection.connect(str(self.fixture.socket_path))
                connection.sendall((json.dumps({"method": "add", "entry": entry}) + "\n").encode())
            finally:
                os.kill(process.pid, signal.SIGCONT)
            response = bytearray()
            while b"\n" not in response:
                block = connection.recv(65536)
                self.assertTrue(block, "Queued reconnect closed without a response")
                response.extend(block)
                self.assertLess(len(response), 1024 * 1024)
            result = json.loads(response.split(b"\n", 1)[0])
            self.assertTrue(result.get("ok"), result)
            return self.row(entry["id"])
        finally:
            connection.close()

    def generator(self, mode="unchanged", initial=False):
        """A finite source-save builder with test-owned publication gates."""
        source = self.root / "generator-input.txt"
        source.write_text("unchanged source\n")
        template = self.root / "template-first.png"
        final = self.root / "template-final.png"
        png(template, size=64)
        png(final, (40, 170, 230, 255), size=96)
        output = self.root / "generated.png"
        if initial:
            png(output, (180, 80, 40, 255), size=32)
        script = self.root / "gated-generator.py"
        script.write_text(
            "from pathlib import Path\n"
            "import json,os,sys,time\n"
            "root=Path(sys.argv[1]); mode=sys.argv[2]\n"
            "with (root/'generator-pids.jsonl').open('a') as log:\n"
            "    log.write(json.dumps({'pid':os.getpid()})+'\\n')\n"
            "deadline=time.monotonic()+35\n"
            "def wait(name):\n"
            "    while not (root/name).exists():\n"
            "        if time.monotonic()>deadline: raise SystemExit('fixture gate expired')\n"
            "        time.sleep(.02)\n"
            "def publish(data):\n"
            "    candidate=root/'generated.candidate'\n"
            "    candidate.write_bytes(data); candidate.replace(root/'generated.png')\n"
            "(root/'generator-ready').touch()\n"
            "wait('allow-publication')\n"
            "if mode=='repair':\n"
            "    publish(b'x'*len((root/'template-final.png').read_bytes()))\n"
            "else: publish((root/'template-first.png').read_bytes())\n"
            "(root/'generator-published').touch()\n"
            "wait('allow-exit')\n"
            "if mode=='changed': publish((root/'template-final.png').read_bytes())\n"
            "elif mode=='repair':\n"
            "    output=root/'generated.png'; previous=output.stat()\n"
            "    with output.open('r+b') as target:\n"
            "        target.write((root/'template-final.png').read_bytes())\n"
            "    os.utime(output,ns=(previous.st_atime_ns,previous.st_mtime_ns))\n"
        )
        entry = {"id": "image", "path": str(output), "kind": "image", "watch": [str(source)],
                 "cwd": str(self.root), "command": [sys.executable, str(script), str(self.root), mode],
                 "timeout": 40, "settings": {"nearest": True, "background": "light"}}
        self.add(entry)
        self.rpc({"method": "show"})
        eventually(lambda: (self.root / "generator-ready").is_file() and self.row("image")["building"],
                   description="gated generator startup")
        return entry

    def publish_generator(self):
        (self.root / "allow-publication").touch()
        eventually(lambda: (self.root / "generator-published").is_file(),
                   description="generator publication gate")

    def finish_generator(self):
        (self.root / "allow-exit").touch()
        eventually(lambda: not self.row("image")["building"], description="finite generator completion")

    def generator_pids(self):
        return [json.loads(line)["pid"] for line in (self.root / "generator-pids.jsonl").read_text().splitlines()]

    def test_equivalent_merged_image_registration_retains_the_loaded_view(self):
        source = self.root / "unchanged-source.txt"
        source.write_text("unchanged source\n")
        path, entry = self.image(settings={"nearest": True, "background": "light"},
                                 image_view={"zoom": 1.5, "pan": [0.1, -0.05]}, watch=[str(source)])
        self.show_loaded()
        before = self.prime_reload()
        self.rpc({"method": "defaults", "group": "image",
                  "settings": {"nearest": False, "background": "dark"}})
        partial = {**entry, "settings": {"nearest": True}}
        partial.pop("image_view")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        self.add(partial)
        self.assert_retained(before, self.row("image"))
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest)
        self.assertEqual(source.read_text(), "unchanged source\n")

    def test_changed_output_reconnect_precedes_watcher_debounce(self):
        path, entry = self.image()
        self.show_loaded()
        before = self.prime_reload()

        def replace_output():
            candidate = path.with_suffix(".candidate")
            png(candidate, (40, 170, 230, 255), size=96)
            candidate.replace(path)

        after = self.reconnect_after_publication(entry, replace_output)
        self.assertGreater(after["revisions"], before["revisions"])
        self.assertEqual(after["metrics"]["width"], 96)
        self.assertEqual(after["metrics"]["height"], 96)

    def test_failed_registration_save_preserves_the_displayed_view(self):
        _, entry = self.image(image_view={"zoom": 1.5, "pan": [0.1, -0.05]})
        self.show_loaded()
        before = self.prime_reload()
        replacement = self.root / "replacement.png"
        png(replacement, size=96)
        with self.failed_registry_save():
            with self.assertRaises(RuntimeError):
                self.add({**entry, "path": str(replacement), "settings": {"nearest": True}})
            self.assert_retained(before, self.row("image"))
            self.assertEqual(self.rpc({"method": "list"})["active"], 1)

    def test_same_registration_retries_after_a_failed_first_load(self):
        path = self.root / "initially-invalid.png"
        candidate = self.root / "valid.png"
        png(candidate, size=96)
        path.write_bytes(b"x" * candidate.stat().st_size)
        entry = {"id": "image", "path": str(path), "kind": "image"}
        self.add(entry)
        self.rpc({"method": "show"})
        eventually(lambda: "waiting" in self.row("image")["status"].lower(),
                   description="failed initial decode")
        self.assertEqual(self.row("image")["revisions"], 0)
        self.assertEqual(self.row("image")["metrics"]["loads"], 0)

        def repair_output():
            previous = path.stat()
            with path.open("r+b") as output:
                output.write(candidate.read_bytes())
            os.utime(path, ns=(previous.st_atime_ns, previous.st_mtime_ns))

        after = self.reconnect_after_publication(entry, repair_output)
        self.assertGreater(after["revisions"], 0)
        self.assertEqual(after["metrics"]["width"], 96)

    def test_equivalent_running_generator_registration_does_not_restart(self):
        entry = self.generator(initial=True)
        eventually(lambda: self.row("image")["metrics"]["loads"] > 0,
                   description="initial view while generator is gated")
        before = self.row("image")
        pids = self.generator_pids()
        self.assertEqual(len(pids), 1)
        self.add({**entry, "settings": {"nearest": True}})
        after = self.row("image")
        self.assert_retained(before, after)
        self.assertTrue(after["building"])
        self.assertEqual(self.generator_pids(), pids)
        self.publish_generator()
        eventually(lambda: self.row("image")["metrics"].get("width") == 64,
                   description="retained generator publication")
        self.finish_generator()
        self.assertEqual(self.row("image")["builds"], 1)
        self.assertEqual(self.generator_pids(), pids)

    def test_successful_generator_completion_reuses_its_displayed_publication(self):
        entry = self.generator()
        self.publish_generator()
        eventually(lambda: self.row("image")["metrics"]["loads"] == 1 and self.row("image")["building"],
                   description="successful intermediate publication")
        before = self.row("image")
        self.finish_generator()
        after = self.row("image")
        self.assert_retained(before, after)
        self.assertTrue(after["status"].startswith("Live"), after["status"])
        self.add({**entry, "settings": {"nearest": True}})
        self.assert_retained(after, self.row("image"))
        self.assertEqual(len(self.generator_pids()), 1)

    def test_generator_output_changed_at_exit_is_published(self):
        self.generator(mode="changed")
        self.publish_generator()
        eventually(lambda: self.row("image")["metrics"].get("width") == 64,
                   description="first generator publication")
        before = self.row("image")["revisions"]
        self.finish_generator()
        after = self.row("image")
        self.assertGreater(after["revisions"], before)
        self.assertEqual(after["metrics"]["width"], 96)
        self.assertEqual(after["builds"], 1)

    def test_explicit_watched_dependency_refreshes_after_unchanged_output_build(self):
        # Explicit watches also cover imported dependencies beyond automatic
        # discovery. The common service must refresh even if the main asset
        # bytes remain unchanged, so an image keeps this decision test small.
        dependency = self.root / "external-dependency.texture"
        dependency.write_bytes(b"first dependency")
        path, _ = self.image(watch=[str(dependency)], cwd=str(self.root),
                             command=[sys.executable, "-c", "pass"])
        self.show_loaded()
        eventually(lambda: self.row("image")["builds"] == 1 and not self.row("image")["building"],
                   description="initial unchanged-output builder completion")
        before = self.row("image")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        dependency.write_bytes(b"second dependency")
        eventually(lambda: self.row("image")["builds"] > before["builds"] and
                   not self.row("image")["building"], description="watched dependency build completion")
        after = self.row("image")
        self.assertGreater(after["revisions"], before["revisions"])
        self.assertEqual(after["builds"], before["builds"] + 1)
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest)

    def test_failed_intermediate_candidate_does_not_authorize_skipping_final_import(self):
        self.generator(mode="repair", initial=True)
        eventually(lambda: self.row("image")["metrics"].get("width") == 32,
                   description="last-good view before invalid publication")
        self.publish_generator()
        eventually(lambda: "waiting" in self.row("image")["status"].lower(),
                   description="rejected intermediate publication")
        before = self.row("image")
        self.assertEqual(before["metrics"]["width"], 32)
        # Drain the documented three bounded retry delays before repair. A
        # later retry must not mask a skipped successful-exit publication.
        time.sleep(3.5)
        self.assertEqual(self.row("image")["revisions"], before["revisions"])
        self.finish_generator()
        after = self.row("image")
        self.assertGreater(after["revisions"], before["revisions"])
        self.assertEqual(after["metrics"]["width"], 96)
        self.assertEqual(after["builds"], 1)

    def test_close_all_save_rollback_and_final_release_stop_owned_resources(self):
        self.generator(initial=True)
        self.image("second", size=128)
        self.rpc({"method": "layout", "layout": "grid"})
        eventually(lambda: self.rpc({"method": "list"})["active"] == 2 and
                   self.row("second").get("metrics", {}).get("loads", 0) > 0,
                   description="mixed generator/image grid")
        before = {key: self.row(key) for key in ("image", "second")}
        pids = self.generator_pids()
        with self.failed_registry_save():
            with self.assertRaises(RuntimeError):
                self.rpc({"method": "close_all"})
            for key, original in before.items():
                self.assert_retained(original, self.row(key))
            self.assertTrue(self.row("image")["building"])
            self.assertEqual(self.generator_pids(), pids)
        self.rpc({"method": "close_all"})
        state = self.rpc({"method": "list"})
        self.assertEqual(state["entries"], [])
        self.assertEqual(state["active"], 0)
        self.assertEqual(state["watched_files"], 0)
        self.assertEqual(state["watched_directories"], 0)
        self.assertTrue(state["window_visible"])
        for pid in pids:
            eventually(lambda: not Path(f"/proc/{pid}").exists(), description="owned generator release")
        self.assertTrue((self.root / "generated.png").is_file())
        self.assertTrue((self.root / "second.png").is_file())

    def test_final_remove_save_rollback_unloads_and_reopens_configuration(self):
        path, _ = self.image(image_view={"zoom": 1.5, "pan": [0.1, -0.05]})
        self.show_loaded()
        before = self.prime_reload()
        with self.failed_registry_save():
            with self.assertRaises(RuntimeError):
                self.rpc({"method": "remove", "id": "image"})
            self.assert_retained(before, self.row("image"))
        self.rpc({"method": "remove", "id": "image"})
        state = self.rpc({"method": "list"})
        self.assertEqual(state["entries"], [])
        for field in ("active", "watched_files", "watched_directories"):
            self.assertEqual(state[field], 0, field)
        self.assertTrue(path.is_file())
        self.rpc({"method": "reopen"})
        reopened = eventually(lambda: self.row("image") if self.row("image").get("metrics", {}).get("loads") else None,
                              description="reopened configuration")
        self.assertEqual(reopened["metrics"]["loads"], 1)
        self.assertEqual(reopened["revisions"], 1)
        self.assertEqual(reopened["image_view"], before["image_view"])

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_equivalent_model_reconnect_keeps_camera_and_validates_the_merged_frame(self):
        source = self.root / "model-input.txt"
        source.write_text("unchanged source\n")
        entry = {"id": "model", "path": str(self.fixture.model), "kind": "model", "watch": [str(source)],
                 "settings": {"front_axis": "-z", "up_axis": "y", "edges": True,
                              "background": "light", "lighting": "lightkit"}}
        self.add(entry)
        self.show_loaded("model")
        self.hardware_renderer()
        self.rpc({"method": "camera_view", "id": "model", "view": "back"})
        before = self.prime_reload("model")
        self.rpc({"method": "defaults", "group": "3d", "settings": {"edges": False, "background": "dark"}})
        self.add({**entry, "settings": {"edges": True}})
        self.assert_retained(before, self.row())
        with self.assertRaises(RuntimeError):
            self.add({**entry, "settings": {"up_axis": "z"}})
        self.assert_retained(before, self.row())
        self.assertEqual(hashlib.sha256(self.fixture.model.read_bytes()).hexdigest(), self.fixture.sha)

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_replaced_external_model_dependency_reconnect_precedes_watcher_debounce(self):
        document, binary = fixture_document()
        dependency = self.root / "external.bin"
        dependency.write_bytes(binary)
        document["buffers"][0]["uri"] = dependency.name
        model = self.root / "external.gltf"
        model.write_text(json.dumps(document))
        entry = {"id": "model", "path": str(model), "kind": "model", "settings": {"lighting": "lightkit"}}
        self.add(entry)
        self.show_loaded("model")
        self.hardware_renderer()
        before = self.prime_reload("model")
        main_digest = hashlib.sha256(model.read_bytes()).hexdigest()

        def replace_dependency():
            previous = dependency.stat()
            _, changed = fixture_document(revision=2)
            self.assertEqual(len(changed), len(binary))
            candidate = dependency.with_suffix(".candidate")
            candidate.write_bytes(changed)
            os.utime(candidate, ns=(previous.st_atime_ns, previous.st_mtime_ns))
            candidate.replace(dependency)

        after = self.reconnect_after_publication(entry, replace_dependency)
        self.assertGreater(after["revisions"], before["revisions"])
        self.assertEqual(hashlib.sha256(model.read_bytes()).hexdigest(), main_digest)
        self.hardware_renderer()

    def external_svg(self, suffix):
        dependency = self.root / "svg-resource.png"
        replacement = self.root / "svg-replacement.png"
        png(dependency, (255, 0, 0, 255))
        png(replacement, (0, 0, 255, 255))
        source = self.root / f"external-image.{suffix}"
        source.write_text('<svg xmlns="http://www.w3.org/2000/svg" '
                          'xmlns:xlink="http://www.w3.org/1999/xlink" width="64" height="64">'
                          '<image xlink:href="svg-resource.png" width="64" height="64"/></svg>')
        entry = {"id": "image", "path": str(source), "kind": "image",
                 "settings": {"nearest": True, "background": "light"}}
        return entry, dependency, replacement

    def assert_image_color(self, color):
        capture = self.root / f"current-image-{uuid.uuid4().hex}.png"
        self.rpc({"method": "capture", "path": str(capture)})
        width, height = struct.unpack(">II", capture.read_bytes()[16:24])
        self.assertEqual(png_pixel(capture, width // 2, height // 2)[:3], color)

    def dependency_only_build(self, entry, dependency, replacement):
        ready = self.root / "dependency-builder-ready"
        allow = self.root / "allow-dependency-builder-exit"
        ready.unlink(missing_ok=True)
        allow.unlink(missing_ok=True)
        script = self.root / "dependency-builder.py"
        script.write_text("from pathlib import Path\nimport shutil,sys,time\n"
                          "root=Path(sys.argv[1]); (root/'dependency-builder-ready').touch()\n"
                          "deadline=time.monotonic()+35\n"
                          "while not (root/'allow-dependency-builder-exit').exists():\n"
                          "    if time.monotonic()>deadline: raise SystemExit('fixture gate expired')\n"
                          "    time.sleep(.02)\n"
                          "target=Path(sys.argv[2]); candidate=target.with_suffix('.candidate')\n"
                          "shutil.copyfile(sys.argv[3],candidate); candidate.replace(target)\n")
        self.add({**entry, "cwd": str(self.root), "timeout": 40,
                  "command": [sys.executable, str(script), str(self.root), str(dependency), str(replacement)]})
        self.show_loaded(entry["id"])
        eventually(lambda: ready.is_file() and self.row(entry["id"])["building"],
                   description="dependency-only generator startup")
        before = self.row(entry["id"])
        allow.touch()
        eventually(lambda: not self.row(entry["id"])["building"],
                   description="dependency-only generator completion")
        after = self.row(entry["id"])
        self.assertGreater(after["revisions"], before["revisions"])
        self.assertGreater(after["metrics"]["loads"], before["metrics"]["loads"])
        self.assertEqual(after["builds"], 1)

    def test_external_svg_reconnect_refreshes_actual_format_under_either_suffix(self):
        for suffix in ("svg", "png"):
            with self.subTest(suffix=suffix):
                self.rpc({"method": "close_all"})
                entry, dependency, replacement = self.external_svg(suffix)
                self.add(entry)
                self.show_loaded()
                before = self.prime_reload()
                self.assert_image_color((255, 0, 0))
                digest = hashlib.sha256(Path(entry["path"]).read_bytes()).hexdigest()
                after = self.reconnect_after_publication(entry, lambda: replacement.replace(dependency))
                self.assertGreater(after["revisions"], before["revisions"])
                self.assert_image_color((0, 0, 255))
                self.assertEqual(hashlib.sha256(Path(entry["path"]).read_bytes()).hexdigest(), digest)

    def test_external_svg_generator_completion_refreshes_actual_format_under_either_suffix(self):
        for suffix in ("svg", "png"):
            with self.subTest(suffix=suffix):
                self.rpc({"method": "close_all"})
                entry, dependency, replacement = self.external_svg(suffix)
                digest = hashlib.sha256(Path(entry["path"]).read_bytes()).hexdigest()
                self.dependency_only_build(entry, dependency, replacement)
                self.assert_image_color((0, 0, 255))
                self.assertEqual(hashlib.sha256(Path(entry["path"]).read_bytes()).hexdigest(), digest)

    def large_external_model(self):
        document, binary = fixture_document()
        dependency = self.root / "large-external.bin"
        dependency.write_bytes(binary)
        document["buffers"][0]["uri"] = dependency.name
        document["extras"] = {"padding": "x" * (4 * 1024 * 1024)}
        source = self.root / "large-external.gltf"
        source.write_text(json.dumps(document))
        self.assertGreater(source.stat().st_size, 4 * 1024 * 1024)
        self.assertLess(source.stat().st_size, 16 * 1024 * 1024)
        replacement = self.root / "large-replacement.bin"
        replacement.write_bytes(fixture_document(revision=2)[1])
        entry = {"id": "model", "path": str(source), "kind": "model",
                 "settings": {"lighting": "lightkit"}}
        return entry, dependency, replacement

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_incomplete_model_discovery_reconnect_refreshes_external_geometry(self):
        entry, dependency, replacement = self.large_external_model()
        self.add(entry)
        self.show_loaded("model")
        before = self.prime_reload("model")
        digest = hashlib.sha256(Path(entry["path"]).read_bytes()).hexdigest()
        after = self.reconnect_after_publication(entry, lambda: replacement.replace(dependency))
        self.assertGreater(after["revisions"], before["revisions"])
        self.assertEqual(hashlib.sha256(Path(entry["path"]).read_bytes()).hexdigest(), digest)
        self.hardware_renderer()

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_incomplete_model_discovery_generator_completion_refreshes_external_geometry(self):
        entry, dependency, replacement = self.large_external_model()
        digest = hashlib.sha256(Path(entry["path"]).read_bytes()).hexdigest()
        self.dependency_only_build(entry, dependency, replacement)
        self.assertEqual(hashlib.sha256(Path(entry["path"]).read_bytes()).hexdigest(), digest)
        self.hardware_renderer()

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_self_contained_material_registration_keeps_the_loaded_view(self):
        path = self.root / "material-color.png"
        normal = self.root / "material-normal.png"
        png(path)
        png(normal, (128, 128, 255, 255))
        entry = {"id": "model", "path": str(path), "kind": "material", "maps": {"normal": str(normal)},
                 "settings": {"shape": "plane", "lighting": "lightkit"}}
        self.add(entry)
        self.show_loaded("model")
        before = self.prime_reload("model")
        self.add(entry)
        self.assert_retained(before, self.row())
        self.hardware_renderer()

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_material_with_external_raw_texture_refreshes_after_generator_completion(self):
        dependency = self.root / "material.raw"
        dependency.write_bytes(bytes([32]) * (4 * 4 * 3))
        replacement = self.root / "replacement.raw"
        replacement.write_bytes(bytes([224]) * (4 * 4 * 3))
        source = self.root / "material.mhd"
        source.write_text("ObjectType = Image\nNDims = 2\nBinaryData = True\n"
                          "DimSize = 4 4\nElementNumberOfChannels = 3\nElementType = MET_UCHAR\n"
                          "ElementDataFile = material.raw\n")
        entry = {"id": "model", "path": str(source), "kind": "material",
                 "settings": {"shape": "plane", "lighting": "lightkit"}}
        self.dependency_only_build(entry, dependency, replacement)
        self.hardware_renderer()

    def review_document(self, dependencies="1" * 64, count=4096):
        notes = []
        for index in range(count):
            note = {"id": "note-" + str(uuid.UUID(int=index + 1, version=4)),
                    "text": f"Review fixture {index:04d}: " + "x" * (8100 if 1024 <= index < 1124 else 500),
                    "status": STATUSES[(index // 4) % len(STATUSES)], "tags": ["fixture"],
                    "author": {"kind": "agent", "name": "Optimization fixture"}, "targets": [],
                    "view": {"camera": {"position": [6, 4, 8], "focal": [0, 0, 0], "up": [0, 1, 0],
                                         "angle": 30, "parallel_span": 8},
                             "orthographic": False, "cuts": [], "hidden": []},
                    "created_against": self.fixture.sha if index % 4 != 1 else "0" * 64,
                    "created_at": "2026-10-16T08:00:00Z", "updated_at": "2026-10-16T08:00:00Z"}
            if index % 4 != 3:
                note["dependency_revision"] = dependencies if index % 4 != 2 else "0" * 64
            notes.append(note)
        return {"schema": "asset-preview-review/v1", "document_revision": 7,
                "asset": {"id": "model", "variant": "default", "lod": 0, "model": str(self.fixture.model),
                          "model_sha256": self.fixture.sha,
                          "frame": {"up": "+Y", "front": "-Z", "units": "model_units"}},
                "annotations": notes, "layer_presets": []}

    def publish_review(self, document):
        encoded = (json.dumps(document, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        self.assertLess(len(encoded), 16 * 1024 * 1024)
        candidate = self.fixture.review.with_suffix(".candidate")
        candidate.write_bytes(encoded)
        candidate.replace(self.fixture.review)
        return hashlib.sha256(encoded).hexdigest()

    def attach_review(self):
        self.fixture.add_model(inspection={"enabled": False, "review": str(self.fixture.review),
                                           "asset_id": "model", "variant": "default"})

    def assert_page(self, expected, offset=0, limit=7, status=None, bindings=None):
        fields = {"offset": offset, "limit": limit}
        if status is not None:
            fields["status"] = status
        result = self.annotations(**fields)
        self.assertEqual(result["total"], len(expected))
        self.assertEqual(result["offset"], offset)
        self.assertEqual([note["id"] for note in result["items"]],
                         [note["id"] for note in expected[offset:offset + limit]])
        self.assertEqual(result["has_more"], offset + len(result["items"]) < len(expected))
        self.assertEqual(len(result["items"]), len(expected[offset:offset + limit]))
        for note in result["items"]:
            expected_binding = bindings[note["id"]] if bindings else "unresolved"
            self.assertEqual(note["binding"], expected_binding, note["id"])
        self.assertLess(len(json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode()), 256 * 1024)
        return result

    def test_large_review_pagination_filter_order_and_response_bounds(self):
        document = self.review_document()
        digest = self.publish_review(document)
        self.attach_review()
        notes = document["annotations"]
        for offset, limit in ((0, 1), (127, 7), (4090, 100), (4096, 1), (5000, 100)):
            page = self.assert_page(notes, offset=offset, limit=limit)
            self.assertEqual(page["review_digest"], digest)
            self.assertEqual(page["document_revision"], 7)
        for status in STATUSES:
            expected = [note for note in notes if note["status"] == status]
            for offset in (0, 17, len(expected) - 3, len(expected)):
                self.assert_page(expected, offset=offset, status=status)
        bounded = self.annotations(offset=1024, limit=100)
        self.assertGreater(len(bounded["items"]), 0)
        self.assertLess(len(bounded["items"]), 100)
        self.assertEqual([note["id"] for note in bounded["items"]],
                         [note["id"] for note in notes[1024:1024 + len(bounded["items"])]])
        self.assertTrue(bounded["has_more"])
        self.assertLess(len(json.dumps(bounded, ensure_ascii=False, separators=(",", ":")).encode()), 256 * 1024)
        following = self.annotations(offset=1024 + len(bounded["items"]), limit=1)
        self.assertEqual(following["items"][0]["id"], notes[1024 + len(bounded["items"])]["id"])
        for bad in ({"offset": -1}, {"offset": 1.5}, {"limit": 0}, {"limit": 101},
                    {"limit": 1.5}, {"status": "unknown"}):
            with self.subTest(invalid=bad), self.assertRaises(RuntimeError):
                self.annotations(**bad)
        self.assertEqual(self.rpc({"method": "list"})["active"], 0)
        self.assertNotIn("metrics", self.row())

    def test_large_review_mutation_detects_external_same_revision_byte_edits(self):
        document = self.review_document()
        self.publish_review(document)
        self.attach_review()
        initial = self.annotations(offset=127, limit=1)
        note_id = initial["items"][0]["id"]
        updated = self.annotations("update", note_id=note_id, expected_revision=7,
                                   expected_digest=initial["review_digest"],
                                   note={"text": "local update 00127.", "status": "resolved"})
        self.assertEqual(updated["document_revision"], 8)
        committed = self.fixture.review.read_bytes()
        committed_digest = hashlib.sha256(committed).hexdigest()
        self.assertEqual(updated["review_digest"], committed_digest)
        self.assertEqual(self.annotations(offset=127, limit=1)["review_digest"], committed_digest)
        self.assertNotIn("binding", json.loads(committed)["annotations"][127])
        self.assertNotIn("_digest", json.loads(committed))

        # Preserve the former cache key deliberately. The same revision and
        # inode/size/mtime cannot authorize an old validated object for new bytes.
        before = self.fixture.review.stat()
        external = committed.replace(b"local update 00127.", b"other update 00127.")
        self.assertNotEqual(external, committed)
        self.assertEqual(len(external), len(committed))
        with self.fixture.review.open("r+b") as review:
            review.write(external)
        os.utime(self.fixture.review, ns=(before.st_atime_ns, before.st_mtime_ns))
        after = self.fixture.review.stat()
        self.assertEqual((before.st_ino, before.st_size, before.st_mtime_ns),
                         (after.st_ino, after.st_size, after.st_mtime_ns))
        external_digest = hashlib.sha256(external).hexdigest()
        page = self.annotations(offset=127, limit=1)
        self.assertEqual(page["document_revision"], 8)
        self.assertEqual(page["review_digest"], external_digest)
        self.assertEqual(page["items"][0]["text"], "other update 00127.")
        with self.assertRaises(RuntimeError):
            self.annotations("update", note_id=note_id, expected_revision=8,
                             expected_digest=committed_digest, note={"text": "must not replace external edit"})
        self.assertEqual(self.fixture.review.read_bytes(), external)

        # Invalid content outside the requested page must still receive full
        # schema validation; a small page is not a partial validation boundary.
        invalid = json.loads(external)
        invalid["annotations"][-1]["status"] = "invalid"
        self.publish_review(invalid)
        with self.assertRaises(RuntimeError):
            self.annotations(offset=0, limit=1)
        self.assertIn("review_error", self.inspect())
        self.assertEqual(self.inspect()["document_revision"], 8)
        self.publish_review(json.loads(external))
        recovered = self.annotations(offset=127, limit=1)
        self.assertEqual(recovered["items"][0]["text"], "other update 00127.")
        final = self.annotations("update", note_id=note_id, expected_revision=8,
                                 expected_digest=recovered["review_digest"], note={"status": "obsolete"})
        self.assertEqual(final["document_revision"], 9)
        self.assertEqual(final["review_digest"], hashlib.sha256(self.fixture.review.read_bytes()).hexdigest())
        self.assertEqual(self.annotations(offset=127, limit=1)["items"][0]["created_against"],
                         document["annotations"][127]["created_against"])

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_large_visible_review_pages_keep_binding_separate_from_workflow_status(self):
        self.fixture.open_inspection()
        dependencies = self.inspect()["dependency_revision"]
        document = self.review_document(dependencies=dependencies)
        self.publish_review(document)
        notes = document["annotations"]
        bindings = {note["id"]: ("exact", "stale", "stale", "unresolved")[index % 4]
                    for index, note in enumerate(notes)}
        before = self.row()
        for status in STATUSES:
            expected = [note for note in notes if note["status"] == status]
            self.assert_page(expected, offset=17, status=status, bindings=bindings)
        self.assert_page(notes, offset=4090, limit=100, bindings=bindings)
        self.assertEqual(self.row()["metrics"]["loads"], before["metrics"]["loads"])
        self.assertEqual(self.row()["revisions"], before["revisions"])
        self.assertEqual(self.row()["metrics"]["camera"], before["metrics"]["camera"])
        self.assertEqual(hashlib.sha256(self.fixture.model.read_bytes()).hexdigest(), self.fixture.sha)


if __name__ == "__main__":
    unittest.main(verbosity=2)
