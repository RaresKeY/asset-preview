#!/usr/bin/env python3
"""Isolated native service checks for files, comparisons and durable settings."""
from __future__ import annotations
import contextlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

from integration import eventually

ROOT = Path(__file__).resolve().parents[1]


class FilesIntegration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ap-files-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root / "state"
        self.state.mkdir(mode=0o700)
        self.runtime = self.root / "runtime"
        self.runtime.mkdir(mode=0o700)
        self.socket = self.runtime / "preview.sock"
        platform = os.environ.get("QT_QPA_PLATFORM", "xcb") if os.environ.get("ASSET_PREVIEW_FILES_NATIVE") else "offscreen"
        self.environment = dict(os.environ, QT_QPA_PLATFORM=platform, ASSET_PREVIEW_RUNTIME_DIR=str(self.runtime),
                                ASSET_PREVIEW_STATE_DIR=str(self.state))
        self.process = None
        self.addCleanup(self.stop)
        self.start()

    def start(self):
        self.log = (self.root / "server.log").open("ab")
        binary = os.environ.get("ASSET_PREVIEW_BINARY", str(ROOT / "build/asset-preview-server"))
        self.process = subprocess.Popen([binary, "--socket", str(self.socket),
                                         "--state", str(self.state)], env=self.environment,
                                        stdin=subprocess.DEVNULL, stdout=self.log, stderr=self.log)
        def ready():
            if self.process.poll() is not None:
                raise AssertionError((self.root / "server.log").read_text())
            try:
                return self.rpc({"method": "ping"})
            except OSError:
                return False
        eventually(ready)

    def stop(self):
        if self.process and self.process.poll() is None:
            with contextlib.suppress(OSError):
                self.rpc({"method": "quit"})
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if hasattr(self, "log"):
            self.log.close()

    def rpc(self, request):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(15)
            connection.connect(str(self.socket))
            connection.sendall(json.dumps(request).encode() + b"\n")
            data = bytearray()
            while b"\n" not in data:
                part = connection.recv(65536)
                if not part or len(data) > 8 * 1024 * 1024:
                    raise RuntimeError("Invalid response")
                data.extend(part)
        result = json.loads(data.split(b"\n", 1)[0])
        if not result["ok"]:
            raise RuntimeError(result["error"])
        return result

    def row(self, identifier):
        return next(row for row in self.rpc({"method": "list"})["entries"] if row["id"] == identifier)

    def cli(self, *arguments):
        result = subprocess.run([str(ROOT / "bin/asset-preview"), *map(str, arguments)], env=self.environment,
                                text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip()

    def show(self, identifier):
        self.rpc({"method": "layout", "layout": "single"})
        self.rpc({"method": "select", "id": identifier})
        self.rpc({"method": "show"})
        return eventually(lambda: self.row(identifier).get("metrics", {}).get("loads", 0) > 0)

    def test_01_detection_open_and_lifecycle(self):
        for name in ("README.md", "source.cpp", "player.gd", "world.tscn", "project.godot", "settings.toml",
                     "data.json", "data.yaml", "events.jsonl", "table.csv", "server.log", "Makefile", ".env"):
            path = self.root / name
            path.write_text("# Example\n\ntext\n")
            identifier = self.cli("add", path, "--ungrouped")
            self.assertEqual(self.row(identifier)["kind"], "file", name)
        self.assertFalse(self.rpc({"method": "list"})["window_visible"])
        self.show(identifier)
        self.assertEqual(self.row(identifier)["metrics"]["file_type"], "text")
        self.assertEqual(self.rpc({"method": "list"})["active"], 1)
        self.rpc({"method": "hide"})
        self.assertEqual(self.rpc({"method": "list"})["active"], 0)
        self.assertNotIn("metrics", self.row(identifier))
        self.cli("open", self.root / "README.md")
        self.assertEqual(len(self.rpc({"method": "list"})["entries"]), 13)

    def test_06_html_images_reload_and_reenter(self):
        from integration import png

        png(self.root / "portrait.png", (200, 100, 50, 255), 128)
        document = self.root / "compatibility.md"
        document.write_text("# Customer catalogue\n\n**Repair requests**<br>Eligible for repair visits.\n\n<img src='portrait.png' width=120 height=80>\n")
        self.rpc({"method": "add", "entry": {"id": "html", "path": str(document), "kind": "file"}})
        self.show("html")
        eventually(lambda: self.row("html")["metrics"]["file_image_decodes"] == 1)
        before = self.row("html")["revisions"]
        document.write_text("# Updated catalogue\n\nFirst<br/>Second<br>Third.\n\n<img src='portrait.png' width=90>\n")
        eventually(lambda: self.row("html")["revisions"] > before)
        eventually(lambda: self.row("html")["metrics"]["file_image_decodes"] > 1)
        self.rpc({"method": "hide"})
        self.assertNotIn("metrics", self.row("html"))
        self.rpc({"method": "show"})
        eventually(lambda: self.row("html").get("metrics", {}).get("file_image_decodes") == 1)
        if os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR") and os.environ.get("ASSET_PREVIEW_FILES_NATIVE"):
            target = Path(os.environ["ASSET_PREVIEW_EVIDENCE_DIR"]) / "window-html-reentry.png"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.unlink(missing_ok=True)
            self.rpc({"method": "capture", "path": str(target.resolve())})

    def test_02_diff_watches_both_inputs_and_restores(self):
        before, after = self.root / "before.cpp", self.root / "after.cpp"
        before.write_text("int count = 1;\nreturn count;\n")
        after.write_text("int count = 2;\nreturn count;\n")
        self.assertEqual(self.cli("diff", before, after, "--id", "comparison", "--mode", "split", "--ungrouped"), "comparison")
        self.show("comparison")
        self.assertEqual(self.row("comparison")["metrics"]["added"], 1)
        self.assertEqual(self.row("comparison")["metrics"]["file_mode"], "split")
        revision = self.row("comparison")["revisions"]
        replacement = self.root / "atomic.cpp"
        replacement.write_text("int count = 1;\nreturn changed;\n")
        replacement.replace(before)
        eventually(lambda: self.row("comparison")["revisions"] > revision)
        self.assertEqual(self.row("comparison")["metrics"]["added"], 2)
        revision = self.row("comparison")["revisions"]
        after.write_text(before.read_text())
        eventually(lambda: self.row("comparison")["revisions"] > revision)
        self.assertEqual(self.row("comparison")["metrics"]["added"], 0)
        self.rpc({"method": "settings", "id": "comparison", "settings": {"file_mode": "inline", "file_wrap": False, "file_context": True}})
        registry = (self.state / "previews.json").read_bytes()
        self.stop()
        self.start()
        self.assertEqual((self.state / "previews.json").read_bytes(), registry)
        self.show("comparison")
        self.assertEqual(self.row("comparison")["metrics"]["file_mode"], "inline")
        self.assertFalse(self.row("comparison")["metrics"]["file_wrap"])
        revision = self.row("comparison")["revisions"]
        loads = self.row("comparison")["metrics"]["loads"]
        after.write_bytes(b"unfinished \xf0\x9f\x98")
        eventually(lambda: self.row("comparison")["status"].startswith("Waiting"))
        self.assertEqual(self.row("comparison")["revisions"], revision)
        self.assertEqual(self.row("comparison")["metrics"]["loads"], loads)
        after.write_bytes(before.read_bytes())
        eventually(lambda: self.row("comparison")["revisions"] > revision)
        revision = self.row("comparison")["revisions"]
        after.write_bytes(b"bad\0binary")
        eventually(lambda: self.row("comparison")["status"].startswith("Waiting"))
        self.assertTrue(self.row("comparison")["overlays"]["status"]["visible"])
        self.assertEqual(self.row("comparison")["revisions"], revision)
        self.assertGreater(self.row("comparison")["metrics"]["loads"], 0)

    def test_03_missing_patch_validation_and_defaults(self):
        patch = self.root / "future.diff"
        self.cli("add", patch, "--id", "patch", "--ungrouped")
        self.rpc({"method": "layout", "layout": "single"})
        self.rpc({"method": "show"})
        eventually(lambda: self.row("patch")["status"].startswith("Waiting"))
        patch.write_text("--- a/settings.json\n+++ b/settings.json\n@@ -1 +1 @@\n-{}\n+{\"live\": true}\n")
        eventually(lambda: self.row("patch").get("metrics", {}).get("file_type") == "diff")
        loaded = self.row("patch")
        self.assertEqual(loaded["metrics"]["file_mode"], "inline")
        self.assertTrue(loaded["metrics"]["file_wrap"])
        self.assertFalse(loaded["overlays"]["name"]["visible"])
        self.assertFalse(loaded["overlays"]["status"]["visible"])
        self.assertLess(loaded["overlays"]["controls"]["y"] - loaded["viewport"]["y"], 12)
        for settings in ({"file_mode": "unknown"}, {"file_wrap": "yes"}, {"file_context": 1}):
            with self.assertRaises(RuntimeError):
                self.rpc({"method": "settings", "id": "patch", "settings": settings})
        with self.assertRaises(RuntimeError):
            self.rpc({"method": "add", "entry": {"id": "bad", "path": str(patch), "before": "relative"}})
        self.cli("defaults", "file", '{"file_wrap":false}')
        self.assertEqual(json.loads((self.state / "previews.json").read_text())["preferences"]["file"], {"file_wrap": False})
        self.stop()
        self.start()
        self.assertEqual(self.row("patch")["kind"], "file")

    @unittest.skipUnless(os.environ.get("ASSET_PREVIEW_FILES_NATIVE"), "Native mouse and keyboard lane")
    def test_markdown_zoom_keyboard_and_wheel(self):
        from integration import XInput

        self.environment["QT_XCB_NO_XI2"] = "1"
        self.stop(); self.start()
        markdown = self.root / "zoom.md"
        markdown.write_text("# Markdown zoom\n\n" + "A readable paragraph with **bold** text.\n\n" * 90)
        self.rpc({"method": "add", "entry": {"id": "guide", "path": str(markdown), "kind": "file"}})
        self.show("guide")
        input = XInput(); self.addCleanup(input.close)
        area = self.row("guide")["viewport"]
        point = area["x"] + area["width"]//2, area["y"] + area["height"]//2
        input.click(*point)
        def zoom(): return self.row("guide")["metrics"]["file_zoom"]
        input.chord("Control_L", "equal"); eventually(lambda: zoom() == 110)
        input.chord("Control_L", "Shift_L", "equal"); eventually(lambda: zoom() == 120)
        input.chord("Control_L", "minus"); eventually(lambda: zoom() == 110)
        input.key("Control_L", 1)
        try: input.scroll(*point)
        finally: input.key("Control_L", 0); input.x.XFlush(input.display)
        eventually(lambda: zoom() == 120)
        input.key("Control_L", 1)
        try: input.scroll(*point, up=False)
        finally: input.key("Control_L", 0); input.x.XFlush(input.display)
        eventually(lambda: zoom() == 110)
        previous = self.row("guide")["metrics"]["file_scroll_y"]
        input.scroll(*point, up=False)
        eventually(lambda: self.row("guide")["metrics"]["file_scroll_y"] > previous)
        self.assertEqual(zoom(), 110)
        input.chord("Control_L", "minus"); eventually(lambda: zoom() == 100)
        input.chord("Control_L", "0")
        eventually(lambda: self.rpc({"method": "list"})["layout"] == "overview")

    @unittest.skipUnless(os.environ.get("ASSET_PREVIEW_FILES_NATIVE"), "Native mouse and keyboard lane")
    def test_05_find_retains_focus_during_mouse_movement(self):
        from integration import XInput

        markdown = self.root / "README.md"
        markdown.write_text("# Search focus\n\nFind the needle in this document.\n\nAnother needle.\n")
        other = self.root / "other.txt"
        other.write_text("Another preview.\n")
        for identifier, path in (("guide", markdown), ("other", other)):
            self.rpc({"method": "add", "entry": {"id": identifier, "path": str(path), "kind": "file"}})
        self.show("guide")
        input = XInput()
        self.addCleanup(input.close)

        def search():
            return self.row("guide")["metrics"]["file_search"]

        def point(identifier, dx=0, dy=0):
            area = self.row(identifier)["viewport"]
            return area["x"] + area["width"]//2 + dx, area["y"] + area["height"]//2 + dy

        for mode in ("rendered", "source"):
            self.rpc({"method": "settings", "id": "guide", "settings": {"file_mode": mode}})
            input.click(*point("guide"))
            input.chord("Control_L", "f")
            eventually(lambda: search()["focused"])
            for index, letter in enumerate("needle"):
                input.motion(*point("guide", 15 + index*5, 15))
                input.x.XSync(input.display, 0)
                time.sleep(0.05)
                self.assertTrue(search()["focused"], f"Find lost focus after mouse movement in {mode}")
                input.press(letter)
                eventually(lambda: search()["text"] == "needle"[:index+1])
            input.press("Return")
            self.assertTrue(search()["focused"])

        # Merely hovering another live card must not interrupt typing, while an
        # actual click still transfers selection and normal hover can resume.
        self.rpc({"method": "layout", "layout": "grid", "grid_size": 2})
        eventually(lambda: self.row("other").get("metrics", {}).get("loads", 0) > 0)
        input.click(*point("guide"))
        input.chord("Control_L", "f")
        eventually(lambda: search()["focused"])
        input.motion(*point("other"))
        input.x.XSync(input.display, 0)
        time.sleep(0.1)
        self.assertTrue(search()["focused"])
        self.assertEqual(self.rpc({"method": "list"})["selected"], "guide")
        input.press("n")
        eventually(lambda: search()["text"] == "n")
        input.click(*point("other"))
        eventually(lambda: self.rpc({"method": "list"})["selected"] == "other")
        self.assertFalse(search()["focused"])
        input.motion(*point("guide", 20, 20))
        input.x.XSync(input.display, 0)
        eventually(lambda: self.rpc({"method": "list"})["selected"] == "guide")

    @unittest.skipUnless(os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR") and os.environ.get("ASSET_PREVIEW_FILES_NATIVE"), "Native window evidence lane")
    def test_04_window_captures(self):
        evidence = Path(os.environ["ASSET_PREVIEW_EVIDENCE_DIR"])
        evidence.mkdir(parents=True, exist_ok=True)
        before, after = self.root / "theme.before.css", self.root / "theme.css"
        original = "/* Preview controls */\n\n"
        for selector in ("compare-action", "pane-title", "pane-timer", "section-label", "file-summary", "changed-file"):
            original += (f".{selector} {{\n    display: inline-flex;\n    align-items: center;\n"
                         "    gap: 5px;\n    min-height: 25px;\n    padding: 3px 8px;\n"
                         "    background: transparent;\n    color: var(--fg);\n    border: 1px solid var(--border);\n"
                         "    border-radius: 5px;\n    cursor: pointer;\n    font-size: 10px;\n"
                         "    font-family: inherit;\n    opacity: 0.78;\n}\n\n")
        before.write_text(original)
        after.write_text(original.replace("font-size: 10px", "font-size: 11px").replace("min-height: 25px", "min-height: 28px"))
        self.cli("diff", before, after, "--id", "changes", "--ungrouped")
        self.rpc({"method": "theme", "mode": "dark"})
        self.show("changes")
        def capture(name):
            target = evidence / name
            target.unlink(missing_ok=True)
            self.rpc({"method": "capture", "path": str(target)})
            self.assertGreater(target.stat().st_size, 8000)
        capture("window-inline.png")
        self.rpc({"method": "settings", "id": "changes", "settings": {"file_mode": "split"}})
        capture("window-split.png")
        markdown = self.root / "README.md"
        markdown.write_text("# Asset Preview\n\nRead documents and review changes alongside your assets.\n\n"
                            "## Files and changes\n\n| View | Use |\n|---|---|\n| Rendered | Read Markdown |\n"
                            "| Source | Inspect code and configuration |\n| Inline diff | Review edits in context |\n"
                            "| Side by side | Compare two versions |\n\n"
                            "```sh\nasset-preview open README.md\nasset-preview diff before.cpp after.cpp --mode split\n```\n\n"
                            "- [x] Watch both files after saving\n- [x] Search with Ctrl+F and F3\n"
                            "- [x] Keep the last good preview during incomplete saves\n")
        self.cli("add", markdown, "--id", "guide", "--ungrouped")
        self.rpc({"method": "zoom", "id": "guide"})
        eventually(lambda: self.row("guide").get("metrics", {}).get("file_type") == "markdown")
        self.rpc({"method": "settings", "id": "guide", "settings": {"file_mode": "rendered"}})
        capture("window-markdown.png")
        # Exercise clipboard ownership across processes in the actual window,
        # where card context-menu filters also participate in input dispatch.
        from integration import XInput, png
        from inspection_integration import png_rgb
        orange, green = (215, 90, 35), (17, 205, 95)
        png(self.root / "copy-menu.png", (*orange, 255), 64)
        png(self.root / "copy-key.png", (*green, 255), 64)
        markdown.write_text("# Image clipboard\n\n![menu](copy-menu.png)\n\n![keyboard](copy-key.png)\n")
        eventually(lambda: self.row("guide").get("metrics", {}).get("file_image_decodes", 0) >= 2)
        input = XInput()
        self.addCleanup(input.close)
        capture("window-image-copy.png")
        _, _, rows = png_rgb(evidence / "window-image-copy.png")
        def image_point(color):
            points = [(x//3, y) for y, row in enumerate(rows) for x in range(0, len(row), 3) if tuple(row[x:x+3]) == color]
            self.assertGreater(len(points), 1000)
            return (min(x for x, _ in points)+max(x for x, _ in points))//2, (min(y for _, y in points)+max(y for _, y in points))//2
        def verify_paste(name, color):
            destination = evidence / name
            destination.unlink(missing_ok=True)
            eventually(lambda: subprocess.run([os.environ["ASSET_PREVIEW_CLIPBOARD_PROBE"], "--read-clipboard", str(destination)],
                                              env=self.environment, timeout=10).returncode == 0)
            width, height, pasted = png_rgb(destination)
            self.assertEqual((width, height), (64, 64))
            self.assertTrue(all(tuple(row[x:x+3]) == color for row in pasted for x in range(0, len(row), 3)))
        x, y = image_point(orange)
        input.motion(x, y)
        input.xt.XTestFakeButtonEvent(input.display, 3, 1, 0)
        input.xt.XTestFakeButtonEvent(input.display, 3, 0, 0)
        input.x.XFlush(input.display)
        input.press("Down"); input.press("Return")
        verify_paste("clipboard-context.png", orange)
        input.click(*image_point(green)); input.chord("Control_L", "c")
        verify_paste("clipboard-keyboard.png", green)
        (evidence / "window-status.json").write_text(json.dumps(self.rpc({"method": "list"}), indent=2))


if __name__ == "__main__":
    unittest.main(verbosity=2)
