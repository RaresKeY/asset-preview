#!/usr/bin/env python3
"""Inspection acceptance tests using real, isolated native server processes.

Functional lane:
  QT_QPA_PLATFORM=offscreen python tests/inspection_integration.py
Hardware lane:
  tools/offscreen.sh env ASSET_PREVIEW_GPU_TEST=1 python tests/inspection_integration.py

ASSET_PREVIEW_BINARY and ASSET_PREVIEW_CLI can point at an integrated build in
another checkout. ASSET_PREVIEW_EVIDENCE_DIR retains requested PNGs and logs.
ASSET_PREVIEW_HAULER_MODEL opts into one read-only real fleet model round trip.
The harness never connects to the user's usual runtime directory.
"""
from __future__ import annotations

import contextlib
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib

from inspection_fixture import PARTS, fixture_bytes, write_fixture
from integration import XInput, png


ROOT = Path(__file__).resolve().parents[1]
PROJECT_ROOT = Path(os.environ.get("ASSET_PREVIEW_PROJECT_ROOT", ROOT)).resolve()
BINARY = Path(os.environ.get("ASSET_PREVIEW_BINARY", PROJECT_ROOT / "build/asset-preview-server")).resolve()
CLI = Path(os.environ.get("ASSET_PREVIEW_CLI", PROJECT_ROOT / "bin/asset-preview")).resolve()
GPU = os.environ.get("ASSET_PREVIEW_GPU_TEST") == "1"
GPU_REASON = "Requires tools/offscreen.sh with ASSET_PREVIEW_GPU_TEST=1"
LIMIT = 1024 * 1024


def eventually(function, timeout=12, description="condition"):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = function()
        if last:
            return last
        time.sleep(0.05)
    raise AssertionError(f"Timed out waiting for {description}: {last!r}")


def png_rgb(path: Path) -> tuple[int, int, list[bytes]]:
    """Decode Qt's bounded, non-interlaced RGB/RGBA captures without Pillow."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n" or len(data) > 32 * LIMIT:
        raise AssertionError("Expected a bounded PNG capture")
    offset, parts = 8, []
    width = height = color = depth = interlace = None
    while offset + 12 <= len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        part = data[offset + 8:offset + 8 + length]
        if len(part) != length:
            raise AssertionError("Truncated capture")
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", part)
        elif kind == b"IDAT":
            parts.append(part)
        offset += length + 12
    if depth != 8 or color not in (2, 6) or interlace != 0 or not width or not height:
        raise AssertionError("Unexpected Qt screenshot format")
    if width * height > 16_000_000:
        raise AssertionError("Capture is larger than the acceptance fixture needs")
    channels = 4 if color == 6 else 3
    stride = width * channels
    pixels = zlib.decompress(b"".join(parts))
    if len(pixels) != height * (stride + 1):
        raise AssertionError("Unexpected PNG pixel length")
    previous, result = bytearray(stride), []

    def paeth(left, up, corner):
        estimate = left + up - corner
        distances = (abs(estimate - left), abs(estimate - up), abs(estimate - corner))
        return (left, up, corner)[distances.index(min(distances))]

    for line in range(height):
        start = line * (stride + 1)
        filter_type = pixels[start]
        row = bytearray(pixels[start + 1:start + 1 + stride])
        if filter_type not in range(5):
            raise AssertionError("Unknown PNG filter")
        for index in range(stride):
            left = row[index - channels] if index >= channels else 0
            up = previous[index]
            corner = previous[index - channels] if index >= channels else 0
            predictor = (0, left, up, (left + up) // 2, paeth(left, up, corner))[filter_type]
            row[index] = (row[index] + predictor) & 255
        result.append(bytes(value for index, value in enumerate(row) if index % channels < 3))
        previous = row
    return width, height, result


def capture_pixels(path: Path, viewport: dict) -> list[tuple[int, int, int]]:
    width, height, rows = png_rgb(path)
    # Ignore toolbar, title, names and controls. Appearance assertions refer to
    # the model surface, not to nonblank application chrome.
    left = max(0, int(viewport["x"] + viewport["width"] * 0.08))
    right = min(width, int(viewport["x"] + viewport["width"] * 0.92))
    top = max(0, int(viewport["y"] + viewport["height"] * 0.16))
    bottom = min(height, int(viewport["y"] + viewport["height"] * 0.86))
    return [tuple(rows[y][x * 3:x * 3 + 3]) for y in range(top, bottom, 3) for x in range(left, right, 3)]


class FixtureContract(unittest.TestCase):
    def test_true_bounded_glb_and_identity_cases(self):
        data = fixture_bytes()
        self.assertLess(len(data), 32 * 1024)
        self.assertEqual(struct.unpack_from("<4sII", data), (b"glTF", 2, len(data)))
        json_size, kind = struct.unpack_from("<I4s", data, 12)
        self.assertEqual(kind, b"JSON")
        document = json.loads(data[20:20 + json_size])
        binary_size, kind = struct.unpack_from("<I4s", data, 20 + json_size)
        self.assertEqual(kind, b"BIN\0")
        self.assertEqual(binary_size, document["buffers"][0]["byteLength"])
        self.assertEqual({i for i, node in enumerate(document["nodes"]) if "mesh" in node}, set(PARTS))
        self.assertEqual(document["nodes"][2]["mesh"], document["nodes"][4]["mesh"])
        self.assertEqual(document["nodes"][2]["name"], document["nodes"][4]["name"])
        self.assertEqual(len(document["meshes"]), 3)
        self.assertEqual(len(document["meshes"][1]["primitives"]), 2)
        self.assertNotIn("semantic_id", document["nodes"][8]["extras"])
        self.assertNotEqual(data, fixture_bytes(2))


class InspectionIntegration(unittest.TestCase):
    """Each test owns exactly one daemon and its private temporary runtime."""

    def setUp(self):
        self.assertTrue(BINARY.is_file(), f"Build the native server first: {BINARY}")
        self.temp = tempfile.TemporaryDirectory(prefix="ap-inspect-", dir="/tmp")
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        self.runtime = self.root / "r"
        self.state_dir = self.root / "s"
        for path in (self.runtime, self.state_dir, self.root / "reviews"):
            path.mkdir(mode=0o700)
        self.socket_path = self.runtime / "preview.sock"
        self.assertLess(len(os.fsencode(self.socket_path)), 100)
        self.env = os.environ.copy()
        self.env.update(ASSET_PREVIEW_RUNTIME_DIR=str(self.runtime), ASSET_PREVIEW_STATE_DIR=str(self.state_dir),
                        ASSET_PREVIEW_TEXTURE_CACHE=str(self.root / "texture-cache"),
                        ASSET_PREVIEW_SMALL_CACHE=str(self.root / "small-cache"), ASSET_PREVIEW_TEST_NULL_AUDIO="1",
                        QT_QPA_PLATFORM="xcb" if GPU else "offscreen", QT_SCALE_FACTOR="1", QT_XCB_NO_XI2="1",
                        PYTHONDONTWRITEBYTECODE="1")
        self.model = self.root / "fixture.glb"
        self.sha = write_fixture(self.model)
        self.review = self.root / "reviews/review.json"
        self.log_path = self.state_dir / "server.log"
        log_fd = os.open(self.log_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(log_fd, "wb") as log:
            self.process = subprocess.Popen([str(BINARY), "--socket", str(self.socket_path), "--state", str(self.state_dir)],
                                            cwd=PROJECT_ROOT, env=self.env, stdin=subprocess.DEVNULL,
                                            stdout=log, stderr=log, start_new_session=True)
        self.addCleanup(self.stop_server)
        self.addCleanup(self.retain_evidence)
        eventually(self.ping, description="owned inspection server startup")
        self.assertEqual(self.rpc({"method": "ping"})["pid"], self.process.pid)
        self.assertEqual(stat.S_IMODE(self.socket_path.stat().st_mode) & 0o077, 0)

    def retain_evidence(self):
        evidence = os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR")
        if not evidence:
            return
        target = Path(evidence).resolve() / self._testMethodName
        target.mkdir(parents=True, exist_ok=True)
        for path in [self.log_path, *self.root.glob("*.png"), *self.root.glob("*.json"), self.review]:
            if path.is_file():
                shutil.copy2(path, target / path.name)

    def stop_server(self):
        if self.process.poll() is not None:
            self.process.wait()
            return
        with contextlib.suppress(OSError, RuntimeError, ValueError):
            self.rpc({"method": "quit"}, timeout=2)
        try:
            self.process.wait(timeout=4)
        except subprocess.TimeoutExpired:
            # Popen owns this exact child. Never search for or terminate a
            # process by a shared name or a PID read from persistent state.
            for child_signal in (signal.SIGTERM, signal.SIGKILL):
                if self.process.poll() is not None:
                    break
                if os.getpgid(self.process.pid) != self.process.pid:
                    raise AssertionError("Owned server unexpectedly changed process group")
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(self.process.pid, child_signal)
                try:
                    self.process.wait(timeout=3)
                    break
                except subprocess.TimeoutExpired:
                    pass
            self.process.wait(timeout=2)

    def ping(self):
        if self.process.poll() is not None:
            raise AssertionError(f"Server exited {self.process.returncode}: {self.log_path.read_text(errors='replace')}")
        try:
            return self.rpc({"method": "ping"}, timeout=0.25)
        except (OSError, RuntimeError, ValueError):
            return None

    def rpc(self, request, timeout=15):
        data = (json.dumps(request, ensure_ascii=False, allow_nan=False) + "\n").encode()
        if len(data) > LIMIT:
            raise ValueError("Test request exceeds the bounded protocol")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(timeout)
            connection.connect(str(self.socket_path))
            connection.sendall(data)
            response = bytearray()
            while b"\n" not in response:
                block = connection.recv(65536)
                if not block:
                    raise RuntimeError("Native server closed without a response")
                response.extend(block)
                if len(response) > 8 * LIMIT:
                    raise RuntimeError("Native server response exceeded its bound")
        result = json.loads(response.split(b"\n", 1)[0])
        if not result.get("ok"):
            raise RuntimeError(result.get("error", "Native request failed"))
        return result

    def row(self, key="model"):
        return next(item for item in self.rpc({"method": "list"})["entries"] if item["id"] == key)

    def inspect(self, action="state", **fields):
        return self.rpc({"method": "inspect", "id": "model", "action": action, **fields})

    def annotations(self, action="list", **fields):
        return self.rpc({"method": "annotations", "id": "model", "action": action, **fields})

    def add_model(self, **fields):
        self.rpc({"method": "add", "entry": {"id": "model", "path": str(self.model), "kind": "model", **fields}})

    def open_inspection(self, visible=True, **entry):
        self.add_model(**entry)
        state = self.inspect("open", review=str(self.review))
        self.assertFalse(state["loaded"], "inspect open must not raise a hidden window")
        self.assertEqual(self.rpc({"method": "list"})["active"], 0)
        if visible:
            self.rpc({"method": "show"})
            eventually(lambda: self.inspect().get("loaded"), timeout=30, description="visible inspection model")
            eventually(lambda: self.row().get("metrics", {}).get("renders", 0) > 0, description="first inspection frame")
            self.hardware_renderer()
        return self.inspect()

    def hardware_renderer(self):
        metrics = self.row()["metrics"]
        renderer = metrics["renderer"]
        self.assertTrue(renderer, metrics)
        self.assertFalse(any(word in renderer.lower() for word in ("llvmpipe", "softpipe", "software", "unknown")), renderer)
        self.assertEqual(metrics["backend"], "vtk-gltf")
        self.assertTrue(metrics["engine"])
        (self.root / "renderer.json").write_text(json.dumps({"renderer": renderer, "backend": metrics["backend"],
                                                            "binary": str(BINARY), "model": str(self.model),
                                                            "model_sha256": self.sha}, indent=2) + "\n")
        return renderer

    def parts(self):
        result = self.inspect("parts", offset=0, limit=100)
        self.assertEqual(result["total"], len(PARTS))
        self.assertFalse(result["has_more"])
        self.assertEqual(result["offset"], 0)
        return result["items"]

    def by_node(self):
        return {part["node_index"]: part for part in self.parts()}

    def module_maps(self):
        return Path(f"/proc/{self.process.pid}/maps").read_text(errors="replace")

    def assert_inspection_module_absent(self):
        modules = [line for line in self.module_maps().splitlines() if "asset-preview" in line and ".so" in line]
        self.assertFalse(any("inspect" in Path(line.split()[-1]).name for line in modules), modules)

    def settle(self):
        # A single-shot render/exposure can still be queued when the preceding
        # RPC returns. Demand a stable sample before measuring idle behavior.
        previous = None
        equal = 0
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            value = self.row()["metrics"]["renders"]
            equal = equal + 1 if value == previous else 0
            if equal >= 3:
                return value
            previous = value
            time.sleep(0.15)
        raise AssertionError("The static inspection view did not stop rendering")

    def capture(self, name):
        self.settle()
        path = self.root / name
        self.rpc({"method": "capture", "path": str(path)})
        pixels = capture_pixels(path, self.row()["viewport"])
        colored = sum(max(pixel) - min(pixel) > 30 and max(pixel) > 70 for pixel in pixels)
        self.assertGreater(colored, 80, f"No colored fixture geometry in {path}")
        return pixels

    def hit_points(self):
        """Find source-resolved points using surface-local, logical pixels."""
        viewport = self.row()["viewport"]
        points = {}
        for yi in range(2, 10):
            for xi in range(1, 12):
                x = viewport["width"] * xi / 12
                y = viewport["height"] * yi / 11
                result = self.inspect("pick", x=x, y=y)
                if result.get("selected"):
                    points.setdefault(result["selected"], (x, y, result.get("selection", {})))
                    if len(points) == len(PARTS):
                        return points
        return points

    def test_01_hidden_opt_in_and_ordinary_image_are_inert(self):
        self.assert_inspection_module_absent()
        self.assertNotIn("asset-preview-f3d.so", self.module_maps())
        self.open_inspection(visible=False)
        # Repeated reads of a missing review must leave its cached document
        # empty, so its first mutation can establish the asset identity.
        for _ in range(3):
            state = self.inspect()
            self.rpc({"method": "list"})
            self.assertNotIn("review_error", state)
            self.assertEqual(state["document_revision"], 0)
            self.assertFalse(self.review.exists())
        self.assertFalse(state["loaded"])
        self.assertEqual(Path(state["review_path"]), self.review)
        with self.assertRaises(RuntimeError):
            self.inspect("parts")
        with self.assertRaises(RuntimeError):
            self.inspect("select", target="Shell")
        self.assert_inspection_module_absent()
        self.assertNotIn("asset-preview-f3d.so", self.module_maps())
        self.assertNotIn("metrics", self.row())
        image = self.root / "image.png"
        png(image)
        self.rpc({"method": "add", "entry": {"id": "image", "path": str(image)}})
        self.rpc({"method": "select", "id": "image"})
        with self.assertRaises(RuntimeError):
            self.rpc({"method": "inspect", "id": "image", "action": "open"})
        self.rpc({"method": "show"})
        eventually(lambda: self.row("image").get("metrics", {}).get("loads"), description="ordinary image decode")
        self.assert_inspection_module_absent()
        self.assertNotIn("asset-preview-f3d.so", self.module_maps())
        self.assertFalse(self.inspect()["loaded"])
        self.rpc({"method": "hide"})
        self.rpc({"method": "settings", "id": "image", "settings": {"background": "dark"}})
        saved = json.loads((self.state_dir / "previews.json").read_text())
        saved_image = next(entry for entry in saved["entries"] if entry["id"] == "image")
        self.assertNotIn("inspection", saved_image, "Ordinary preview reads inserted inspection state")

    def test_02_offline_annotation_reads_do_not_start_a_service(self):
        self.stop_server()
        offline_runtime = self.root / "offline-runtime"
        offline_state = self.root / "offline-state"
        document = {
            "schema": "asset-preview-review/v1", "document_revision": 1,
            "asset": {"id": "inspection-fixture", "variant": "editorial", "lod": 0, "model": "fixture.glb",
                      "model_sha256": self.sha, "frame": {"up": "+Y", "front": "-Z", "units": "metres"}},
            "annotations": [{"id": "note-11111111-1111-4111-8111-111111111111", "text": "Check the fixture model.", "status": "open", "tags": [],
                             "author": {"kind": "human", "name": "Fixture author"}, "targets": [],
                             "view": {"camera": {"position": [6, 4, 8], "focal": [0, 0, 0], "up": [0, 1, 0], "angle": 30, "parallel_span": 8},
                                      "orthographic": False, "cuts": [], "hidden": []},
                             "created_against": self.sha, "created_at": "2026-10-16T08:00:00Z", "updated_at": "2026-10-16T08:00:00Z"}],
            "layer_presets": [],
        }
        self.review.write_text(json.dumps(document))
        environment = {**self.env, "ASSET_PREVIEW_RUNTIME_DIR": str(offline_runtime), "ASSET_PREVIEW_STATE_DIR": str(offline_state)}
        before = self.review.read_bytes()
        listed = subprocess.run([str(CLI), "annotations", "list", str(self.review), "--json"], env=environment,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(listed.returncode, 0, listed.stderr)
        self.assertIn("note-11111111-1111-4111-8111-111111111111", listed.stdout)
        json.loads(listed.stdout)
        exported = subprocess.run([str(CLI), "annotations", "export", str(self.review), "--format", "markdown"], env=environment,
                                  capture_output=True, text=True, timeout=10)
        self.assertEqual(exported.returncode, 0, exported.stderr)
        # CommonMark permits escaped punctuation in literal note text and IDs.
        plain_export = re.sub(r"\\([^\w\s])", r"\1", exported.stdout)
        self.assertIn("Check the fixture model.", plain_export)
        self.assertIn("note-11111111-1111-4111-8111-111111111111", plain_export)
        self.assertEqual(self.review.read_bytes(), before)
        self.assertFalse(offline_runtime.exists(), "Offline review reads created a runtime")
        self.assertFalse(offline_state.exists(), "Offline review reads created application state")

    def test_02b_review_identity_and_source_aliases_are_guarded_without_geometry(self):
        source = self.root / "source.json"
        source.write_text("generator input")
        source_dir = self.root / "sources"
        source_dir.mkdir()
        alias_dir = self.root / "source-alias"
        alias_dir.symlink_to(source_dir, target_is_directory=True)
        future_source = source_dir / "future.json"
        model_alias = self.root / "model-alias.json"
        model_alias.symlink_to(self.model)
        source_alias = self.root / "input-alias.json"
        source_alias.symlink_to(source)
        aliases = (model_alias, source_alias, alias_dir / "future.json")
        self.add_model(watch=[str(source), str(future_source)])
        for alias in aliases:
            with self.subTest(review_alias=str(alias)):
                with self.assertRaisesRegex(RuntimeError, "separate|input|watch"):
                    self.inspect("open", review=str(alias))
                with self.assertRaisesRegex(RuntimeError, "separate|input|watch"):
                    self.rpc({"method": "add", "entry": {"id": "unsafe", "kind": "model", "path": str(self.model),
                              "watch": [str(source), str(future_source)],
                              "inspection": {"enabled": True, "review": str(alias)}}})
                self.assertFalse(any(entry["id"] == "unsafe" for entry in self.rpc({"method": "list"})["entries"]))
        document = {"schema": "asset-preview-review/v1", "document_revision": 1,
                    "asset": {"id": "fixture/owner", "variant": "editorial", "lod": 0, "model": str(self.model),
                              "model_sha256": self.sha, "frame": {"up": "+Y", "front": "-Z", "units": "metres"}},
                    "annotations": [], "layer_presets": []}
        original = json.dumps(document).encode()
        self.review.write_bytes(original)
        self.inspect("open", review=str(self.review), asset_id="fixture/owner", variant="editorial")
        options = self.row()["inspection"]
        with self.assertRaises(RuntimeError):
            self.inspect("open", review=str(self.review), asset_id="another/owner")
        self.assertEqual(self.row()["inspection"], options)
        for field, different in (("id", "another/owner"), ("variant", "another-variant"), ("lod", 1),
                                 ("model", str(self.root / "another.glb"))):
            with self.subTest(changed_asset_field=field):
                changed = json.loads(original)
                changed["asset"][field] = different
                self.review.write_text(json.dumps(changed))
                before = self.review.read_bytes()
                try:
                    with self.assertRaisesRegex(RuntimeError, "identity"):
                        self.annotations()
                    self.assertIn("review_error", self.inspect())
                    self.assertEqual(self.row()["inspection"], options)
                    self.assertEqual(self.review.read_bytes(), before)
                finally:
                    self.review.write_bytes(original)
                self.assertEqual(self.annotations()["items"], [])
        self.assertNotIn("review_error", self.inspect())
        self.assertFalse(self.inspect()["loaded"])
        self.assert_inspection_module_absent()

    def test_02c_reconnecting_to_new_review_resets_only_committed_metadata(self):
        document = {"schema": "asset-preview-review/v1", "document_revision": 7,
                    "asset": {"id": "fixture/owner", "variant": "editorial", "lod": 0, "model": str(self.model),
                              "model_sha256": self.sha, "frame": {"up": "+Y", "front": "-Z", "units": "metres"}},
                    "annotations": [], "layer_presets": []}
        self.review.write_text(json.dumps(document))
        self.add_model()
        self.inspect("open", review=str(self.review), asset_id="fixture/owner", variant="editorial")
        self.assertEqual(self.inspect()["document_revision"], 7)
        fresh = self.root / "reviews/new-review.json"
        options = {**self.row()["inspection"], "review": str(fresh)}
        registry = self.state_dir / "previews.json"
        backup = self.state_dir / "registry-before-reconnect.json"
        registry.rename(backup)
        registry.mkdir(mode=0o700)
        try:
            with self.assertRaises(RuntimeError):
                self.add_model(inspection=options)
        finally:
            registry.rmdir()
            backup.replace(registry)
        retained = self.inspect()
        self.assertEqual(retained["document_revision"], 7)
        self.assertEqual(retained["review_path"], str(self.review))
        self.add_model(inspection=options)
        changed = self.inspect()
        self.assertEqual(changed["review_path"], str(fresh))
        self.assertEqual(changed["document_revision"], 0)
        self.assertNotIn("review_error", changed)
        self.assertEqual(self.annotations()["items"], [])
        self.assertFalse(fresh.exists())
        self.assert_inspection_module_absent()

    def test_02d_leaving_inspection_publishes_selection_once_and_preserves_failed_selection(self):
        self.open_inspection(visible=False)
        image = self.root / "image.png"
        png(image)
        self.rpc({"method": "add", "entry": {"id": "image", "path": str(image)}})
        registry = self.state_dir / "previews.json"
        backup = self.state_dir / "registry-before-selection.json"
        original = registry.read_bytes()
        registry.rename(backup)
        registry.mkdir(mode=0o700)
        try:
            with self.assertRaises(RuntimeError):
                self.rpc({"method": "select", "id": "image"})
        finally:
            registry.rmdir()
            backup.replace(registry)
        self.assertEqual(self.rpc({"method": "list"})["selected"], "model")
        self.assertTrue(self.inspect()["enabled"])
        self.assertEqual(registry.read_bytes(), original)
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        libc.inotify_init1.argtypes = [ctypes.c_int]
        libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
        watch = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
        self.assertGreaterEqual(watch, 0)
        try:
            self.assertGreaterEqual(libc.inotify_add_watch(watch, os.fsencode(self.state_dir), 0x100 | 0x80 | 0x200), 0)
            self.rpc({"method": "select", "id": "image"})
            events = bytearray()
            while True:
                try:
                    events.extend(os.read(watch, 65536))
                except BlockingIOError:
                    break
        finally:
            os.close(watch)
        publications, offset = 0, 0
        while offset < len(events):
            _, mask, _, length = struct.unpack_from("iIII", events, offset)
            filename = events[offset + 16:offset + 16 + length].split(b"\0", 1)[0]
            publications += bool(mask & 0x80 and filename == b"previews.json")
            offset += 16 + length
        self.assertEqual(publications, 1, "Leaving inspection and selecting an asset must share one atomic registry save")
        self.assertEqual(self.rpc({"method": "list"})["selected"], "image")
        self.assertFalse(self.inspect()["enabled"])
        self.assert_inspection_module_absent()

    def test_02e_cached_review_rejects_a_same_inode_symlink_and_recovers(self):
        document = {"schema": "asset-preview-review/v1", "document_revision": 1,
                    "asset": {"id": "fixture/owner", "variant": "editorial", "lod": 0, "model": str(self.model),
                              "model_sha256": self.sha, "frame": {"up": "+Y", "front": "-Z", "units": "metres"}},
                    "annotations": [], "layer_presets": []}
        original = json.dumps(document).encode()
        self.review.write_bytes(original)
        self.add_model()
        self.inspect("open", review=str(self.review), asset_id="fixture/owner", variant="editorial")
        self.assertEqual(self.inspect()["document_revision"], 1)
        backing = self.review.with_name("backing.json")
        self.review.rename(backing)
        self.review.symlink_to(backing)
        try:
            self.assertIn("symlink", self.inspect().get("review_error", "").lower())
            with self.assertRaisesRegex(RuntimeError, "[Ss]ymlink"):
                self.annotations()
            self.assertEqual(backing.read_bytes(), original)
        finally:
            self.review.unlink()
            backing.rename(self.review)
        self.assertNotIn("review_error", self.inspect())
        self.assertEqual(self.annotations()["items"], [])
        self.assert_inspection_module_absent()

    def test_02f_registration_keeps_inspection_transitions_explicit(self):
        options = {"enabled": True, "review": str(self.review)}
        with self.assertRaisesRegex(RuntimeError, "inspect open"):
            self.add_model(inspection=options)
        self.assertEqual(self.rpc({"method": "list"})["entries"], [])
        self.add_model(inspection={**options, "enabled": False})
        self.assertFalse(self.inspect()["enabled"])
        self.inspect("open")
        for key in ("review", "asset_id", "variant"):
            incomplete = {name: value for name, value in self.row()["inspection"].items() if name != key}
            with self.subTest(missing_inspection_field=key):
                with self.assertRaisesRegex(RuntimeError, "Enabled inspection requires"):
                    self.add_model(inspection=incomplete)
                self.assertEqual(self.inspect()["review_path"], str(self.review))
        self.add_model()
        self.assertTrue(self.inspect()["enabled"])
        self.add_model(inspection=self.row()["inspection"])
        self.assertTrue(self.inspect()["enabled"])
        self.inspect("close")
        with self.assertRaisesRegex(RuntimeError, "inspect open"):
            self.add_model(inspection=options)
        self.assertFalse(self.inspect()["enabled"])
        self.assert_inspection_module_absent()

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_03_inventory_resolves_instances_paths_and_multiple_primitives(self):
        self.assert_inspection_module_absent()
        state = self.open_inspection()
        self.assertEqual(state["revision"], self.sha)
        self.assertEqual(state["part_count"], len(PARTS))
        self.assertNotIn("asset-preview-f3d.so", self.module_maps(), "Inspection retained an ordinary geometry engine")
        parts = self.by_node()
        self.assertEqual(set(parts), set(PARTS))
        self.assertEqual(len({part["id"] for part in parts.values()}), len(PARTS))
        self.assertEqual(len({part["path"] for part in parts.values()}), len(PARTS))
        for index, expected in PARTS.items():
            part = parts[index]
            self.assertEqual(part["exported_name"], expected["exported_name"])
            self.assertEqual(part["name"], expected["name"])
            self.assertEqual(part.get("semantic_id") or None, expected.get("semantic_id"))
            result = self.inspect("select", target=part["id"])
            self.assertEqual(result["selected"], part["id"])
            self.assertEqual(result["selection"]["node_index"], index)
        first = self.inspect("parts", offset=0, limit=2)
        second = self.inspect("parts", offset=2, limit=2)
        self.assertTrue(first["has_more"])
        self.assertFalse(second["has_more"])
        self.assertEqual([item["id"] for item in first["items"] + second["items"]], [item["id"] for item in self.parts()])
        for bad in ({"target": "Shell"}, {"target": "no-such-part"}):
            with self.assertRaises(RuntimeError):
                self.inspect("select", **bad)
        with self.assertRaises(RuntimeError):
            self.inspect("parts", limit=101)

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_04_visibility_cuts_capture_and_idle_do_not_reload_geometry(self):
        self.open_inspection()
        parts = self.by_node()
        loads = self.row()["metrics"]["loads"]
        port = parts[2]["id"]
        self.inspect("hide", target=port)
        self.assertIn(port, self.inspect()["hidden"])
        self.inspect("restore", last=True)
        self.assertNotIn(port, self.inspect()["hidden"])
        self.inspect("isolate", target=parts[8]["id"])
        self.assertEqual(set(self.inspect()["hidden"]), {part["id"] for index, part in parts.items() if index != 8})
        self.inspect("restore", all=True)
        self.assertEqual(self.inspect()["hidden"], [])
        original = self.capture("inspection-original.png")
        self.inspect("cut", axis="x", position=0.0, keep="positive", enabled=True)
        cuts = self.inspect()["cuts"]
        self.assertTrue(any(cut["axis"] == "x" and cut["enabled"] for cut in cuts), cuts)
        clipped = self.capture("inspection-clipped.png")
        self.assertEqual(len(original), len(clipped))
        self.assertGreater(sum(a != b for a, b in zip(original, clipped)), 100, "The cut did not change rendered geometry")
        for fields in ({"axis": "q", "position": 0}, {"axis": "x", "position": "0"},
                       {"axis": "x", "position": 0, "keep": "both"}, {"axis": "x", "position": 0, "enabled": "yes"}):
            with self.subTest(invalid_cut=fields):
                with self.assertRaises(RuntimeError):
                    self.inspect("cut", **fields)
        self.inspect("cut", axis="x", position=0, keep="positive", enabled=False)
        self.assertEqual(self.row()["metrics"]["loads"], loads)
        stable_renders = self.settle()
        time.sleep(1.0)
        self.assertEqual(self.row()["metrics"]["renders"], stable_renders)
        self.assertEqual(self.row()["metrics"]["loads"], loads)

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_05_notes_persist_displayed_identity_without_running_generator(self):
        source = self.root / "source.txt"
        source.write_text("fixture source")
        template = self.root / "template.glb"
        write_fixture(template)
        counter = self.root / "generator-count.txt"
        generator = self.root / "generate.py"
        generator.write_text("from pathlib import Path\nimport sys\n"
                             "template,target,counter=map(Path,sys.argv[1:])\n"
                             "counter.write_text(str(int(counter.read_text())+1) if counter.exists() else '1')\n"
                             "candidate=target.with_suffix('.candidate')\n"
                             "candidate.write_bytes(template.read_bytes())\n"
                             "candidate.replace(target)\n")
        self.open_inspection(watch=[str(source)], cwd=str(self.root),
                             command=[sys.executable, str(generator), str(template), str(self.model), str(counter)])
        eventually(lambda: counter.is_file() and not self.row()["building"], description="finite fixture generator")
        self.assertEqual(counter.read_text(), "1")
        part = self.by_node()[2]
        self.inspect("select", target=part["id"])
        self.inspect("cut", axis="z", position=0, keep="negative", enabled=True)
        self.inspect("hide", target=self.by_node()[4]["id"])
        self.settle()
        loads = self.row()["metrics"]["loads"]
        created = self.annotations("add", expected_revision=0, note={"text": "Check hull clearance.", "status": "open", "tags": ["fixture"]})
        self.assertEqual(created["document_revision"], 1)
        note = created["note"]
        document = json.loads(self.review.read_text())
        self.assertEqual(document["schema"], "asset-preview-review/v1")
        self.assertEqual(document["asset"]["model_sha256"], self.sha)
        self.assertEqual(note["created_against"], self.sha)
        target = note["targets"][0]
        self.assertEqual(target["semantic_id"], PARTS[2]["semantic_id"])
        self.assertEqual(target["exported_name"], "Shell")
        self.assertEqual(target.get("instance_selector", target.get("path")), part["path"])
        self.assertTrue(note["view"]["cuts"])
        self.assertTrue(note["view"]["hidden"])
        before = self.review.read_bytes()
        with self.assertRaises(RuntimeError):
            self.annotations("update", note_id=note["id"], expected_revision=0, note={"text": "Conflicting write"})
        self.assertEqual(self.review.read_bytes(), before)
        with self.assertRaises(RuntimeError):
            self.annotations("update", note_id=note["id"], expected_revision=1,
                             expected_digest="0" * 64, note={"text": "Conflicting document digest"})
        self.assertEqual(self.review.read_bytes(), before)
        with self.assertRaises(RuntimeError):
            self.annotations("add", expected_revision=1, note={"text": "x" * 8193, "status": "open"})
        self.assertEqual(self.review.read_bytes(), before)
        changed = self.annotations("update", note_id=note["id"], expected_revision=1,
                                   expected_digest=created["review_digest"], note={"text": "Clearance checked.", "status": "resolved"})
        self.assertEqual(changed["document_revision"], 2)
        listed = self.annotations()
        self.assertEqual(listed["items"][0]["text"], "Clearance checked.")
        self.assertEqual(listed["items"][0]["binding"], "exact")
        self.settle()
        time.sleep(0.5)
        self.assertEqual(counter.read_text(), "1", "Saving review data reran the model generator")
        self.assertEqual(self.row()["metrics"]["loads"], loads, "Saving review data reimported geometry")
        self.assertEqual(self.model.read_bytes(), fixture_bytes())
        self.annotations("remove", note_id=note["id"], expected_revision=2)
        self.assertEqual(self.annotations()["items"], [])

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_06_failed_replacement_preserves_displayed_revision_and_notes_become_stale(self):
        self.open_inspection()
        part = self.by_node()[5]
        self.inspect("select", target=part["id"])
        first = self.annotations("add", expected_revision=0, note={"text": "Original cabin review.", "status": "open"})
        loads = self.row()["metrics"]["loads"]
        broken = self.model.with_suffix(".candidate")
        broken.write_bytes(b"This is an intentionally incomplete GLB save.")
        broken.replace(self.model)
        eventually(lambda: "waiting" in self.row()["status"].lower(), timeout=15, description="rejected model save")
        last_good = self.inspect()
        self.assertTrue(last_good["loaded"])
        self.assertEqual(last_good["revision"], self.sha)
        self.assertEqual(self.row()["metrics"]["loads"], loads)
        second = self.annotations("add", expected_revision=first["document_revision"],
                                  note={"text": "Note while replacement is invalid.", "status": "open"})
        self.assertEqual(second["note"]["created_against"], self.sha)
        self.assertEqual(json.loads(self.review.read_text())["asset"]["model_sha256"], self.sha)
        new_sha = write_fixture(self.model, revision=2)
        eventually(lambda: self.inspect().get("revision") == new_sha, timeout=20, description="replacement revision publication")
        self.assertGreater(self.row()["metrics"]["loads"], loads)
        notes = self.annotations()["items"]
        self.assertEqual(len(notes), 2)
        self.assertTrue(all(note["binding"] == "stale" for note in notes), notes)
        self.assertTrue(all(note["created_against"] == self.sha for note in notes))
        self.assertTrue(all(note["targets"][0]["semantic_id"] == PARTS[5]["semantic_id"] for note in notes))

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_07_hidden_inspection_releases_scene_and_defers_source_changes(self):
        self.open_inspection()
        part = self.by_node()[8]
        self.inspect("select", target=part["id"])
        self.annotations("add", expected_revision=0, note={"text": "Unnamed semantic target stays source resolved.", "status": "open"})
        self.rpc({"method": "hide"})
        state = self.rpc({"method": "list"})
        self.assertEqual(state["active"], 0)
        self.assertEqual(state["watched_files"], 0)
        self.assertNotIn("metrics", self.row())
        self.assertFalse(self.inspect()["loaded"])
        with self.assertRaises(RuntimeError):
            self.inspect("pick", x=10, y=10)
        replacement = write_fixture(self.model, revision=2)
        time.sleep(0.4)
        self.assertFalse(self.inspect()["loaded"])
        self.assertNotIn("metrics", self.row())
        self.rpc({"method": "show"})
        eventually(lambda: self.inspect().get("revision") == replacement and self.inspect().get("loaded"), timeout=30,
                   description="inspection reentry after hidden save")
        self.assertEqual(self.annotations()["items"][0]["binding"], "stale")
        self.inspect("close")
        eventually(lambda: self.row().get("metrics", {}).get("backend") != "vtk-gltf" and
                   self.row().get("metrics", {}).get("engine"), timeout=30, description="return to ordinary preview")

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_08_native_click_drag_and_transformed_surface_pin(self):
        self.open_inspection()
        self.rpc({"method": "camera_view", "id": "model", "view": "front"})
        self.settle()
        parts = self.by_node()
        points = self.hit_points()
        nozzle = parts[8]["id"]
        self.assertIn(nozzle, points, "The transformed nozzle was not source pickable")
        x, y, selection = points[nozzle]
        anchor = selection["anchor"]
        self.assertEqual(anchor["kind"], "surface_point")
        position = anchor["position_local"]
        normal = anchor["normal_local"]
        self.assertEqual(len(position), 3)
        self.assertEqual(len(normal), 3)
        self.assertTrue(all(math.isfinite(value) for value in position + normal))
        for value, half_extent in zip(position, (0.4, 0.3, 0.85)):
            self.assertLessEqual(abs(value), half_extent + 1e-3, f"The pin was not transformed into component local space: {anchor}")
        self.assertAlmostEqual(math.sqrt(sum(value * value for value in normal)), 1, places=4)
        self.assertTrue(any(abs(abs(value) - extent) < 1e-3 for value, extent in zip(position, (0.4, 0.3, 0.85))), anchor)
        loads = self.row()["metrics"]["loads"]
        self.inspect("hide", target=nozzle)
        self.assertNotEqual(self.inspect("pick", x=x, y=y).get("selected"), nozzle, "Hidden geometry remained pickable")
        self.inspect("show", target=nozzle)
        self.assertEqual(self.inspect("pick", x=x, y=y)["selected"], nozzle)
        self.inspect("cut", axis="x", position=10, keep="positive", enabled=True)
        self.assertFalse(self.inspect("pick", x=x, y=y).get("selected"), "Picking ignored a cut which rejects the whole fixture")
        self.inspect("cut", axis="x", position=10, keep="positive", enabled=False)
        self.assertEqual(self.inspect("pick", x=x, y=y)["selected"], nozzle)
        self.assertEqual(self.row()["metrics"]["loads"], loads)
        other = next(part["id"] for part in parts.values() if part["id"] != nozzle)
        self.inspect("select", target=other)
        input_device = XInput()
        try:
            viewport = self.row()["viewport"]
            input_device.click(round(viewport["x"] + x), round(viewport["y"] + y))
            eventually(lambda: self.inspect().get("selected") == nozzle, description="native short click selects component")
            self.assertEqual(self.inspect()["selection"]["name"], PARTS[8]["name"])
            created = self.annotations("add", expected_revision=0, note={"text": "Inspect the transformed nozzle surface.", "status": "open"})
            saved_anchor = created["note"]["targets"][0]["anchor"]
            self.assertEqual(saved_anchor["kind"], "surface_point")
            self.assertTrue(all(math.isfinite(value) for value in saved_anchor["position_local"]))
            self.inspect("select", target=other)
            time.sleep(0.6)  # Keep the next press outside Qt's double-click interval.
            camera = self.row()["metrics"]["camera"]
            input_device.drag(round(viewport["x"] + x), round(viewport["y"] + y), 70, 25)
            eventually(lambda: self.row()["metrics"]["camera"] != camera, description="native drag orbits inspection camera")
            self.assertEqual(self.inspect()["selected"], other, "Orbit drag was treated as a component click")
        finally:
            input_device.close()

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_09_ordinary_model_loads_inspection_only_on_request_and_keeps_camera(self):
        self.add_model()
        self.rpc({"method": "show"})
        eventually(lambda: self.row().get("metrics", {}).get("engine"), timeout=30, description="ordinary F3D model")
        self.assert_inspection_module_absent()
        renderer = self.row()["metrics"]["renderer"]
        self.assertFalse(any(word in renderer.lower() for word in ("llvmpipe", "softpipe", "software", "unknown")), renderer)
        self.rpc({"method": "camera_view", "id": "model", "view": "right"})
        before = self.row()["metrics"]["camera"]
        self.inspect("open", review=str(self.review))
        eventually(lambda: self.inspect().get("loaded"), timeout=30, description="ordinary to inspection transition")
        self.hardware_renderer()
        after = self.row()["metrics"]["camera"]
        for field in ("position", "focal", "up"):
            self.assertLess(math.dist(before[field], after[field]), 1e-5, f"Switching backend moved camera {field}")
        self.assertEqual(self.rpc({"method": "list"})["active"], 1)
        self.inspect("close")
        eventually(lambda: self.row().get("metrics", {}).get("backend") != "vtk-gltf" and
                   self.row().get("metrics", {}).get("engine"), timeout=30, description="inspection to ordinary transition")
        restored = self.row()["metrics"]["camera"]
        for field in ("position", "focal", "up"):
            self.assertLess(math.dist(after[field], restored[field]), 1e-5, f"Returning to preview moved camera {field}")
        self.rpc({"method": "camera_view", "id": "model", "view": "back"})
        self.rpc({"method": "settings", "id": "model", "settings": {"orthographic": True}})
        changed = self.row()["metrics"]["camera"]
        self.assertGreater(math.dist(changed["position"], after["position"]), 0.1)
        self.inspect("open", review=str(self.review))
        eventually(lambda: self.inspect().get("loaded"), description="reentered inspection")
        reentered = self.inspect()["camera"]
        self.assertTrue(self.inspect()["orthographic"])
        self.assertTrue(self.row()["settings"]["orthographic"])
        for field in ("position", "focal", "up"):
            self.assertLess(math.dist(changed[field], reentered[field]), 1e-5, f"Reentering inspection restored an older camera {field}")

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_10_restore_projection_and_failed_apply_preserve_settings_and_defaults(self):
        self.open_inspection()
        parts = self.by_node()
        self.rpc({"method": "settings", "id": "model", "settings": {"orthographic": True}})
        self.rpc({"method": "camera_view", "id": "model", "view": "front"})
        self.inspect("select", target=parts[5]["id"])
        self.inspect("cut", axis="x", position=0.1, keep="positive", enabled=True)
        self.inspect("hide", target=parts[2]["id"])
        created = self.annotations("add", expected_revision=0, note={"text": "Orthographic saved view.", "status": "open"})
        saved_view = created["note"]["view"]
        self.assertTrue(saved_view["orthographic"])
        self.rpc({"method": "settings", "id": "model", "settings": {"orthographic": False}})
        self.rpc({"method": "camera_view", "id": "model", "view": "back"})
        self.inspect("restore", all=True)
        self.inspect("cut", axis="x", position=0.1, keep="positive", enabled=False)
        preferences = self.rpc({"method": "list"})["preferences"]
        self.assertFalse(preferences["3d"]["orthographic"])
        document_before = self.review.read_bytes()
        restored = self.annotations("restore", note_id=created["note"]["id"])
        self.assertTrue(restored["orthographic"])
        self.assertTrue(self.row()["settings"]["orthographic"])
        self.assertEqual(restored["cuts"], saved_view["cuts"])
        self.assertEqual(set(restored["hidden"]), set(saved_view["hidden"]))
        self.assertEqual(restored["selected"], saved_view["selected"])
        for field in ("position", "focal", "up"):
            self.assertLess(math.dist(restored["camera"][field], saved_view["camera"][field]), 1e-6)
        persisted = json.loads((self.state_dir / "previews.json").read_text())
        stored_camera = next(entry for entry in persisted["entries"] if entry["id"] == "model")["camera"]
        self.assertEqual(stored_camera, restored["camera"], "Restored comment camera was not saved for service restart")
        self.assertEqual(self.rpc({"method": "list"})["preferences"], preferences)
        self.settle()
        before = self.inspect()
        settings_before = self.row()["settings"]
        changed_camera = json.loads(json.dumps(before["camera"]))
        changed_camera["position"][0] += 2.0
        failed_view = {"orthographic": False, "camera": changed_camera, "selected": parts[8]["id"],
                       "cuts": [{"axis": "y", "position": 0.2, "keep": "negative", "enabled": True}],
                       "hidden": [parts[4]["id"]]}
        registry = self.state_dir / "previews.json"
        backup = self.state_dir / "previews.before-failed-apply.json"
        registry.rename(backup)
        registry.mkdir(mode=0o700)
        try:
            with self.assertRaises(RuntimeError):
                self.inspect("apply", view=failed_view)
        finally:
            registry.rmdir()
            backup.replace(registry)
        rolled_back = self.inspect()
        for field in ("orthographic", "cuts", "hidden", "selected"):
            self.assertEqual(rolled_back[field], before[field], f"Failed save did not restore {field}")
        for field in ("position", "focal", "up"):
            self.assertLess(math.dist(rolled_back["camera"][field], before["camera"][field]), 1e-6)
        for field in ("angle", "parallel_span"):
            self.assertAlmostEqual(rolled_back["camera"][field], before["camera"][field], places=6)
        self.assertEqual(self.row()["settings"], settings_before)
        self.assertEqual(self.rpc({"method": "list"})["preferences"], preferences)
        self.assertEqual(self.review.read_bytes(), document_before)

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_11_closed_inspected_preview_reopens_in_inspection_with_review(self):
        self.open_inspection()
        parts = self.by_node()
        self.inspect("select", target=parts[8]["id"])
        self.inspect("hide", target=parts[2]["id"])
        self.inspect("cut", axis="z", position=0.1, keep="negative", enabled=True)
        created = self.annotations("add", expected_revision=0, note={"text": "Review survives preview close.", "status": "open"})
        preferences = self.rpc({"method": "list"})["preferences"]
        self.rpc({"method": "remove", "id": "model"})
        closed = self.rpc({"method": "list"})
        self.assertEqual(closed["entries"], [])
        self.assertEqual(closed["active"], 0)
        self.rpc({"method": "reopen"})
        eventually(lambda: self.inspect().get("loaded"), timeout=30, description="reopened inspection view")
        self.hardware_renderer()
        reopened = self.inspect()
        self.assertEqual(reopened["review_path"], str(self.review))
        self.assertEqual(reopened["document_revision"], 1)
        self.assertEqual(reopened["revision"], self.sha)
        self.assertEqual(self.rpc({"method": "list"})["active"], 1)
        self.assertEqual(self.rpc({"method": "list"})["preferences"], preferences)
        note = self.annotations()["items"][0]
        self.assertEqual(note["id"], created["note"]["id"])
        self.assertEqual(note["binding"], "exact")
        restored = self.annotations("restore", note_id=note["id"])
        self.assertEqual(restored["cuts"], note["view"]["cuts"])
        self.assertEqual(set(restored["hidden"]), set(note["view"]["hidden"]))

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_12_apply_validation_and_failed_save_preserve_visibility_history(self):
        self.open_inspection()
        parts = self.by_node()
        self.inspect("select", target=parts[8]["id"])
        self.inspect("cut", axis="x", position=0.1, keep="positive", enabled=True)
        self.inspect("hide", target=parts[2]["id"])
        first_hidden = self.inspect()["hidden"]
        self.inspect("hide", target=parts[4]["id"])
        self.settle()
        before = self.inspect()
        bad_views = [None, [], {"cuts": "bad"}, {"hidden": {}}, {"camera": []},
                     {"selection": "bad"}, {"selected": []}, {"orthographic": 1},
                     {"revision": "0" * 64}, {"dependency_revision": "0" * 64},
                     {"selected": parts[8]["id"], "selection": {"id": parts[8]["id"], "anchor": {
                         "kind": "surface_point", "position_local": [0, 0, 0], "normal_local": [0, 0, 0]}}}]
        for invalid in bad_views:
            with self.subTest(invalid=invalid):
                with self.assertRaises(RuntimeError):
                    self.inspect("apply", view=invalid)
                after = self.inspect()
                for field in ("camera", "orthographic", "hidden", "cuts", "selected", "selection"):
                    self.assertEqual(after[field], before[field], f"Rejected view changed {field}")
        derived_overflow = {"orthographic": False, "camera": {
            "position": [1e12, 0, 0], "focal": [0, 0, 0], "up": [0, 1, 0], "angle": 179, "parallel_span": 1}}
        with self.assertRaises(RuntimeError):
            self.inspect("apply", view=derived_overflow)
        self.assertEqual(self.inspect()["camera"], before["camera"])
        failed_view = {**before, "orthographic": not before["orthographic"], "hidden": [parts[5]["id"]]}
        failed_view["camera"] = json.loads(json.dumps(before["camera"]))
        failed_view["camera"]["position"][0] += 1
        registry = self.state_dir / "previews.json"
        backup = self.state_dir / "registry-before-failed-view.json"
        registry.rename(backup)
        registry.mkdir(mode=0o700)
        try:
            with self.assertRaises(RuntimeError):
                self.inspect("apply", view=failed_view)
        finally:
            registry.rmdir()
            backup.replace(registry)
        self.assertEqual(self.inspect()["hidden"], before["hidden"])
        self.assertEqual(self.inspect("restore", last=True)["hidden"], first_hidden)
        self.assertEqual(self.inspect("restore", last=True)["hidden"], [])
        anchor = {"kind": "surface_point", "position_local": [0, 0, 0], "normal_local": [0, 0, 1],
                  "extension": "x" * (300 * 1024)}
        applied = self.inspect("apply", view={"selected": parts[8]["id"],
                                             "selection": {"id": parts[8]["id"], "anchor": anchor},
                                             "cuts": [{"axis": "x", "position": 0, "keep": "positive", "enabled": False,
                                                       "extension": "x" * (300 * 1024)}]})
        self.assertNotIn("extension", applied["selection"]["anchor"])
        self.assertNotIn("extension", applied["cuts"][0])
        self.assertLess(len(json.dumps(applied).encode()), 250 * 1024)
        with self.assertRaises(RuntimeError):
            self.inspect("commit_hidden", hidden=[])

    @unittest.skipUnless(GPU, GPU_REASON)
    def test_13_frame_projection_reconnect_and_grid_backend_transitions(self):
        self.open_inspection()
        self.rpc({"method": "settings", "id": "model", "settings": {"up_axis": "z"}})
        self.assertGreater(abs(self.inspect()["camera"]["up"][2]), 0.99)
        self.rpc({"method": "hide"})
        self.rpc({"method": "settings", "id": "model", "settings": {"up_axis": "y", "orthographic": True}})
        self.rpc({"method": "show"})
        eventually(lambda: self.inspect().get("loaded"), description="inspection after hidden frame/projection change")
        self.assertTrue(self.inspect()["orthographic"])
        self.assertGreater(abs(self.inspect()["camera"]["up"][1]), 0.99)
        explicit_camera = self.inspect()["camera"]
        explicit_camera["position"][0] += 2
        self.add_model(camera=explicit_camera)
        eventually(lambda: self.inspect().get("loaded"), description="visible explicit-camera reconnection")
        for field in ("position", "focal", "up"):
            self.assertLess(math.dist(self.inspect()["camera"][field], explicit_camera[field]), 1e-6)
        self.rpc({"method": "hide"})
        explicit_camera["position"][0] += 1
        explicit_camera["focal"][0] += 1
        self.add_model(camera=explicit_camera)
        self.rpc({"method": "show"})
        eventually(lambda: self.inspect().get("loaded"), description="hidden explicit-camera reconnection")
        for field in ("position", "focal", "up"):
            self.assertLess(math.dist(self.inspect()["camera"][field], explicit_camera[field]), 1e-6)
        image = self.root / "image.png"
        png(image)
        self.rpc({"method": "add", "entry": {"id": "image", "path": str(image)}})
        self.rpc({"method": "layout", "layout": "grid", "grid_size": 2})
        self.assertEqual(self.rpc({"method": "list"})["active"], 1)
        self.rpc({"method": "select", "id": "image"})
        eventually(lambda: self.row().get("metrics", {}).get("loads", 0) > 0 and
                   self.row()["metrics"].get("backend") != "vtk-gltf", description="ordinary grid model after inspection")
        state = self.rpc({"method": "list"})
        self.assertEqual(state["selected"], "image")
        self.assertEqual(state["active"], 2)
        self.assertFalse(self.inspect()["enabled"])
        self.assertFalse(self.inspect()["loaded"])
        self.assertTrue(self.row()["metrics"]["engine"])
        self.assertIn("asset-preview-f3d.so", self.module_maps())

    @unittest.skipUnless(GPU and os.environ.get("ASSET_PREVIEW_HAULER_MODEL"), "Optional read-only Hauler model not selected")
    def test_99_real_hauler_annotation_round_trip(self):
        self.model = Path(os.environ["ASSET_PREVIEW_HAULER_MODEL"]).resolve()
        self.assertTrue(self.model.is_file())
        source_stat = self.model.stat()
        self.sha = hashlib.sha256(self.model.read_bytes()).hexdigest()
        self.review = self.root / "reviews/hauler-review.json"
        self.add_model(label="Hauler", settings={"up_axis": "y", "front_axis": "-z"})
        self.inspect("open", review=str(self.review), asset_id="fleet/hauler", variant="active")
        self.rpc({"method": "show"})
        eventually(lambda: self.inspect().get("loaded"), timeout=45, description="real Hauler inspection")
        renderer = self.hardware_renderer()
        pages = [self.inspect("parts", offset=offset, limit=100) for offset in (0, 100)]
        inventory = pages[0]["items"] + pages[1]["items"]
        self.assertEqual(len(inventory), 170)
        self.assertEqual(self.inspect()["part_count"], 170)
        self.assertEqual(self.inspect()["unmapped_actors"], 0)
        self.assertEqual(len({part["id"] for part in inventory}), 170)
        targets = [part for part in inventory if part.get("semantic_id") == "cargo_port_1_clamp_pad"]
        self.assertEqual(len(targets), 1)
        part = targets[0]
        self.inspect("select", target=part["id"])
        input_device = XInput()
        try:
            input_device.x.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
            input_device.x.XResizeWindow(input_device.display, input_device.window,
                                        min(1280, int(os.environ.get("ASSET_PREVIEW_TEST_WIDTH", "1100")) - 20),
                                        min(1560, int(os.environ.get("ASSET_PREVIEW_TEST_HEIGHT", "800")) - 20))
            input_device.x.XFlush(input_device.display)
            time.sleep(0.2)
            camera = {"position": [-46.26, 10.67, 44.43], "focal": [-16.26, -3.33, 23.43],
                      "up": [0, 1, 0], "angle": 30, "parallel_span": 22}
            self.inspect("apply", view={"camera": camera, "orthographic": False, "selected": part["id"], "cuts": [], "hidden": []})
            created = self.annotations("add", expected_revision=0, note={"text": "Test annotation round trip", "status": "open"})
            self.assertEqual(created["note"]["created_against"], self.sha)
            self.assertEqual(created["note"]["targets"][0]["semantic_id"], "cargo_port_1_clamp_pad")
            self.settle()
            capture = self.root / "hauler-comment-review.png"
            self.rpc({"method": "capture", "path": str(capture)})
            pixels = capture_pixels(capture, self.row()["viewport"])
            self.assertGreater(len(set(pixels)), 100, "The Hauler capture does not contain rendered model detail")
        finally:
            input_device.close()
        proof = {"source_model": str(self.model), "model_sha256": self.sha, "renderer": renderer, "part_count": 170,
                 "selected": part, "note": created["note"], "displayed_view": self.inspect()}
        self.rpc({"method": "hide"})
        self.assertFalse(self.inspect()["loaded"])
        self.assertNotIn("metrics", self.row())
        self.assertEqual(self.rpc({"method": "list"})["active"], 0)
        self.stop_server()
        self.assertEqual(self.process.returncode, 0, "Hauler server did not shut down cleanly")
        self.assertEqual(hashlib.sha256(self.model.read_bytes()).hexdigest(), self.sha)
        self.assertEqual(self.model.stat().st_mtime_ns, source_stat.st_mtime_ns)
        proof["hidden_loaded"] = False
        proof["server_exit_code"] = self.process.returncode
        proof["source_unchanged"] = True
        (self.root / "hauler-roundtrip.json").write_text(json.dumps(proof, indent=2) + "\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
