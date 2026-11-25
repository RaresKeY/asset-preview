#!/usr/bin/env python3
"""Gallery selection, ownership, persistence and native input acceptance."""
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import time
import unittest
import zlib

import inspection_integration as harness
from integration import png, XInput


class GalleryIntegration(unittest.TestCase):
    def setUp(self):
        self.fixture = harness.InspectionIntegration(methodName="runTest")
        self.fixture._testMethodName = self._testMethodName
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.root, self.rpc, self.row = self.fixture.root, self.fixture.rpc, self.fixture.row

    def image(self, key, project="art", **fields):
        path = self.root / (key + ".png")
        png(path, (70, 135, 185, 255), 128)
        self.rpc({"method": "add", "entry": {"id": key, "path": str(path), "project": project, **fields}})
        return path

    def state(self):
        return self.rpc({"method": "list"})

    def loaded(self, key):
        return harness.eventually(lambda: self.row(key) if self.row(key).get("metrics", {}).get("loads", 0) else None)

    def gallery(self, project="art"):
        self.rpc({"method": "layout", "layout": "gallery", "project_filter": project})
        self.rpc({"method": "show"})
        harness.eventually(lambda: self.state()["presentation"]["gallery"]["visible"])

    @unittest.skipUnless(harness.GPU, harness.GPU_REASON)
    def test_native_move_gallery_to_new_project_menu(self):
        self.image('first',project='art')
        self.rpc({'method':'layout','layout':'overview','project_filter':'','group_projects':True})
        self.rpc({'method':'show'})
        input=XInput(); self.addCleanup(input.close)
        overview=self.state()['presentation']['overview']; header=overview['groups'][0]
        input.motion(overview['viewport']['x']+40,header['gallery_button']['y']+10)
        input.xt.XTestFakeButtonEvent(input.display,3,1,0)
        input.xt.XTestFakeButtonEvent(input.display,3,0,0); input.x.XFlush(input.display)
        time.sleep(.15); input.press('Down'); input.press('Right'); time.sleep(.15)
        input.press('Return')
        # Let the popup retire before sending text to its native dialog. X11
        # tree enumeration can race those transient popup window IDs.
        time.sleep(.25)
        for letter in 'game': input.press(letter)
        input.press('Return')
        harness.eventually(lambda:any(p['id']=='art' and p.get('parent') for p in self.state()['projects']))
        projects={p['id']:p for p in self.state()['projects']}
        self.assertEqual(projects[projects['art']['parent']]['label'],'game')
        self.assertEqual(self.state()['layout'],'overview')
        self.assertEqual(self.state()['active'],0)

    @unittest.skipUnless(harness.GPU, harness.GPU_REASON)
    def test_native_project_group_hierarchy(self):
        for key, group in [('hero','characters'),('crate','props'),('barrel','props'),('elsewhere','other')]:
            self.image(key,project=group)
        self.rpc({'method':'project','id':'game','label':'My Game'})
        for group in ('characters','props'):
            self.rpc({'method':'project','id':group,'parent':'game','label':group.title()})
        self.rpc({'method':'theme','mode':'dark'})
        self.rpc({'method':'layout','layout':'overview','project_filter':'','group_projects':True})
        self.rpc({'method':'show'})
        input=XInput(); self.addCleanup(input.close)
        def click(rect): input.click(rect['x']+rect['width']//2,rect['y']+rect['height']//2)
        def groups(): return {g['id']:g for g in self.state()['presentation']['overview']['groups']}
        self.rpc({'method':'capture','path':str(self.root/'project-hierarchy-overview.png')})
        click(groups()['game']['gallery_button'])
        harness.eventually(lambda:self.state()['layout']=='gallery' and self.state()['project_filter']=='game')
        self.assertEqual(self.state()['presentation']['overview']['items'],3)
        self.loaded('hero')
        click(self.state()['presentation']['explorer']['toggle'])
        harness.eventually(lambda:self.state()['presentation']['explorer']['visible'])
        def folders(): return {g['id']:g for g in self.state()['presentation']['explorer']['groups']}
        self.assertEqual(folders()['props']['parent'],'game')
        self.assertTrue(all(not g['expanded'] for g in folders().values()))
        click(folders()['game']); input.press('Right')
        harness.eventually(lambda:folders()['game']['expanded'])
        click(folders()['props']); input.press('Right')
        harness.eventually(lambda:folders()['props']['expanded'])
        click(folders()['props']['files'][1])
        harness.eventually(lambda:self.state()['layout']=='single' and self.state()['selected']=='barrel')
        self.loaded('barrel')
        click(folders()['props']); input.press('Return')
        harness.eventually(lambda:self.state()['layout']=='gallery' and self.state()['project_filter']=='props')
        self.assertEqual(self.state()['presentation']['overview']['items'],2)
        self.rpc({'method':'capture','path':str(self.root/'project-hierarchy-gallery.png')})
        input.x.XResizeWindow.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_uint,ctypes.c_uint]
        input.x.XResizeWindow(input.display,input.window,620,520); input.x.XSync(input.display,0)
        harness.eventually(lambda:self.state()['presentation']['width']==620)
        self.rpc({'method':'capture','path':str(self.root/'project-hierarchy-narrow.png')})
        input.x.XResizeWindow(input.display,input.window,1100,800); input.x.XSync(input.display,0)
        harness.eventually(lambda:self.state()['presentation']['width']==1100)
        input.press('Escape')
        harness.eventually(lambda:self.state()['layout']=='overview' and not self.state()['project_filter'])
        viewport=self.state()['presentation']['overview']['viewport']
        for _ in range(12): input.scroll(viewport['x']+30,viewport['y']+30,up=True)
        harness.eventually(lambda:self.state()['presentation']['overview']['scroll']==0)
        click(groups()['game']['close_button'])
        harness.eventually(lambda:[e['id'] for e in self.state()['entries']]==['elsewhere'])
        self.rpc({'method':'reopen'})
        self.assertEqual(next(p for p in self.state()['projects'] if p['id']=='props')['parent'],'game')
        # A real native model establishes the renderer used by this display.
        self.rpc({'method':'add','entry':{'id':'model','path':str(self.fixture.model),'project':'other'}})
        self.rpc({'method':'zoom','id':'model'})
        renderer=harness.eventually(lambda:self.row('model').get('metrics',{}).get('renderer'))
        self.assertFalse(any(x in renderer.lower() for x in ('llvmpipe','softpipe','software','unknown')),renderer)
        (self.root/'project-hierarchy-result.json').write_text(json.dumps({'renderer':renderer,'native_input':True,'hierarchy':self.state()['projects']},indent=2))

    def test_file_name_survives_category_filter_mismatch(self):
        self.image("first")
        path = self.root / "README.md"
        path.write_text("# Community Customer Portraits\n\nExample document.\n")
        label = "Community customers · Portraits, roles & dialogue"
        self.rpc({"method": "add", "entry": {"id": "readme", "path": str(path), "project": "docs", "label": label}})
        self.gallery("art")
        # Opening a file outside the gallery's category used to leave index -1
        # in the top picker, even though the file itself was visible.
        self.rpc({"method": "zoom", "id": "readme"})
        self.loaded("readme")
        state = self.state()
        self.assertEqual(state["project_filter"], "")
        self.assertEqual(state["presentation"]["selection_control"]["text"], label)
        self.assertIn(str(path), state["presentation"]["selection_control"]["tooltip"])
        # A gallery selection made by another route can also sit outside its
        # filter; the name must remain truthful until the next layout change.
        self.gallery("art")
        self.rpc({"method": "select", "id": "readme"})
        self.assertEqual(self.state()["presentation"]["selection_control"]["id"], "readme")
        self.rpc({"method": "layout", "layout": "single"})
        self.loaded("readme")
        self.assertEqual(self.state()["project_filter"], "")
        # A failed save rolls back both the selection and category clearing.
        self.gallery("art")
        registry = self.fixture.state_dir / "previews.json"
        backup = self.fixture.state_dir / "before-explorer.json"
        registry.rename(backup)
        registry.mkdir()
        self.addCleanup(lambda: registry.rmdir() if registry.is_dir() else None)
        with self.assertRaises(RuntimeError):
            self.rpc({"method": "zoom", "id": "readme"})
        self.assertEqual(self.state()["selected"], "first")
        self.assertEqual(self.state()["project_filter"], "art")
        self.assertEqual(self.state()["presentation"]["selection_control"]["text"], "first.png")
        registry.rmdir()
        backup.rename(registry)

    @unittest.skipUnless(harness.GPU, harness.GPU_REASON)
    def test_native_folder_explorer(self):
        self.image("first")
        self.image("second")
        self.image("other", project="elsewhere")
        self.image("loose", project="")
        path = self.root / "README.md"
        path.write_text("# Community Customer Portraits\n\nSelect files in the folder explorer.\n")
        self.rpc({"method": "add", "entry": {"id": "readme", "path": str(path), "project": "docs", "label": "Community customers · Portraits, roles & dialogue"}})
        self.rpc({"method": "theme", "mode": "dark"})
        self.gallery("art")
        input = XInput()
        self.addCleanup(input.close)

        def tree():
            return self.state()["presentation"]["explorer"]

        def click(rect):
            input.click(rect["x"] + min(65, rect["width"]//2), rect["y"] + rect["height"]//2)

        self.assertFalse(tree()["visible"])
        click(tree()["toggle"])
        harness.eventually(lambda: tree()["visible"])
        self.assertEqual([g["id"] for g in tree()["groups"]], ["art", "elsewhere", "docs", "@ungrouped"])
        self.rpc({"method": "capture", "path": str(self.root/"explorer-default-collapsed.png")})
        self.assertTrue(all(not g["expanded"] for g in tree()["groups"]), {g["id"]: g["expanded"] for g in tree()["groups"]})
        docs = next(g for g in tree()["groups"] if g["id"] == "docs")
        click(docs)
        input.press("Right")
        harness.eventually(lambda: next(g for g in tree()["groups"] if g["id"] == "docs")["expanded"])
        self.image("new-folder-entry", project="new-folder")
        groups = {g["id"]: g for g in tree()["groups"]}
        self.assertTrue(groups["docs"]["expanded"])
        self.assertFalse(groups["art"]["expanded"])
        self.assertFalse(groups["new-folder"]["expanded"])
        self.rpc({"method": "remove", "id": "new-folder-entry"})
        docs = next(g for g in tree()["groups"] if g["id"] == "docs")
        click(docs["files"][0])
        harness.eventually(lambda: self.state()["layout"] == "single" and self.state()["selected"] == "readme")
        self.loaded("readme")
        self.assertEqual(self.state()["project_filter"], "")
        self.assertEqual(self.state()["active"], 1)
        self.assertTrue(self.state()["presentation"]["selection_control"]["text"].startswith("Community customers"))
        self.rpc({"method": "capture", "path": str(self.root/"explorer-file-expanded.png")})
        input.chord("Control_L", "b")
        harness.eventually(lambda: not tree()["visible"])
        self.assertLess(self.row("readme")["viewport"]["x"], 30)
        self.rpc({"method": "capture", "path": str(self.root/"explorer-single-collapsed.png")})
        input.chord("Control_L", "b")
        harness.eventually(lambda: tree()["visible"])
        input.x.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
        input.x.XResizeWindow.restype = ctypes.c_int
        input.x.XResizeWindow(input.display, input.window, 620, 520)
        input.x.XSync(input.display, 0)
        harness.eventually(lambda: self.state()["presentation"]["width"] == 620)
        self.assertLess(tree()["width"], 230)
        self.assertGreater(self.row("readme")["viewport"]["width"], 200)
        self.rpc({"method": "capture", "path": str(self.root/"explorer-file-narrow.png")})
        input.x.XResizeWindow(input.display, input.window, 1000, 720)
        input.x.XSync(input.display, 0)
        harness.eventually(lambda: self.state()["presentation"]["width"] == 1000)
        click(next(g for g in tree()["groups"] if g["id"] == "docs")["files"][0])
        # Left/Right belong to the tree while it has focus, preserving native
        # folder expansion rather than accidentally navigating previews.
        input.press("Left")
        input.press("Left")
        harness.eventually(lambda: not next(g for g in tree()["groups"] if g["id"] == "docs")["expanded"])
        self.assertEqual(self.state()["selected"], "readme")
        input.press("Right")
        harness.eventually(lambda: next(g for g in tree()["groups"] if g["id"] == "docs")["expanded"])
        input.press("Return")
        harness.eventually(lambda: self.state()["layout"] == "gallery" and self.state()["project_filter"] == "docs")
        self.loaded("readme")
        input.chord("Control_L", "b")
        harness.eventually(lambda: not tree()["visible"])
        self.rpc({"method": "capture", "path": str(self.root/"explorer-file-collapsed.png")})
        input.chord("Control_L", "b")
        harness.eventually(lambda: tree()["visible"])
        # The sidebar is global: Escape restores all-category Overview while
        # retaining the explorer, and hiding unloads ordinary file content.
        input.press("Escape")
        harness.eventually(lambda: self.state()["layout"] == "overview")
        self.assertTrue(tree()["visible"])
        self.assertEqual(self.state()["active"], 0)
        self.rpc({"method": "capture", "path": str(self.root/"explorer-overview.png")})
        input.press("F11")
        harness.eventually(lambda: self.state()["presentation"]["fullscreen"])
        self.assertFalse(tree()["visible"])
        input.press("Escape")
        harness.eventually(lambda: tree()["visible"])
        self.rpc({"method": "remove", "id": "other"})
        self.assertNotIn("elsewhere", [g["id"] for g in tree()["groups"]])
        self.rpc({"method": "reopen"})
        self.assertIn("elsewhere", [g["id"] for g in tree()["groups"]])
        self.rpc({"method": "hide"})
        self.assertEqual(self.state()["active"], 0)
        self.rpc({"method": "quit"})
        self.assertEqual(self.fixture.process.wait(timeout=5), 0)
        with self.fixture.log_path.open("ab") as log:
            self.fixture.process = subprocess.Popen(
                [str(harness.BINARY), "--socket", str(self.fixture.socket_path), "--state", str(self.fixture.state_dir)],
                env=self.fixture.env, cwd=harness.PROJECT_ROOT, stdout=log, stderr=log, start_new_session=True)
        harness.eventually(self.fixture.ping)
        self.rpc({"method": "show"})
        self.assertTrue(tree()["visible"])
        self.assertTrue(tree()["expanded"])
        self.assertTrue(all(not g["expanded"] for g in tree()["groups"]))

    def test_small_image_thumbnails_preserve_exact_colors(self):
        colors = {(240, 40, 80), (30, 190, 230)}
        palette = sorted(colors)
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        pixels = b"".join(b"\0" + b"".join(bytes(palette[(x+y) % 2]) for x in range(7)) for y in range(5))
        data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 7, 5, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))
        for key in ("selected", "queued"):
            path = self.root / (key + ".png")
            path.write_bytes(data)
            self.rpc({"method": "add", "entry": {"id": key, "path": str(path), "project": "art"}})
        self.rpc({"method": "select", "id": "selected"})
        self.gallery()
        if harness.GPU:
            # A pointer left by another native test can tint the thumbnail on hover.
            input = XInput()
            self.addCleanup(input.close)
            input.motion(5, 5)
            input.x.XSync(input.display, 0)
            time.sleep(0.1)

        def verify(label):
            if self.state()["layout"] != "overview":
                self.loaded(self.state()["selected"])
            state = harness.eventually(lambda: self.state() if all(
                tile["thumbnail_ready"] for tile in self.state()["presentation"]["overview"]["tiles"]) else None)
            capture = self.root / (label + ".png")
            self.rpc({"method": "capture", "path": str(capture)})
            _, _, rows = harness.png_rgb(capture)
            for tile in state["presentation"]["overview"]["tiles"]:
                # Sample the central strip, away from chrome and image edges.
                center = tile["x"] + tile["width"] // 2
                y = tile["y"] + 41
                observed = {tuple(rows[y][x*3:x*3+3]) for x in range(center-20, center+20)}
                self.assertEqual(observed, colors, f"{label}: {tile['id']} must enlarge without blended pixels")

        verify("small-images-initial")
        self.rpc({"method": "select", "id": "queued"})
        self.loaded("queued")
        self.rpc({"method": "select", "id": "selected"})
        self.loaded("selected")
        # Change only the thumbnail identity while the decoded payload is cached.
        self.rpc({"method": "settings", "id": "queued", "settings": {"nearest": True}})
        self.rpc({"method": "select", "id": "queued"})
        self.loaded("queued")
        self.assertTrue(self.row("queued")["metrics"]["load_timing"]["cache_hit"])
        verify("small-images-payload-cache-hit")
        self.rpc({"method": "hide"})
        self.rpc({"method": "show"})
        verify("small-images-disk-cache")
        self.rpc({"method": "layout", "layout": "overview", "project_filter": "art", "group_projects": True})
        verify("small-images-grouped-overview")
        self.rpc({"method": "layout", "group_projects": False})
        verify("small-images-flat-overview")
        self.rpc({"method": "hide"})
        self.rpc({"method": "show"})
        verify("small-images-overview-disk-cache")

    def test_filtered_selection_ownership_watch_and_restart(self):
        marker = self.root / "hidden-generator-must-not-run"
        self.image("first")
        second = self.image("second", command=["/usr/bin/touch", str(marker)], cwd=str(self.root))
        self.image("other", project="elsewhere")
        self.rpc({"method": "select", "id": "other"})
        self.gallery()
        self.loaded("first")
        state = self.state()
        self.assertEqual(state["selected"], "first")
        self.assertEqual(state["active"], 1)
        self.assertEqual(state["presentation"]["overview"]["items"], 2)
        self.assertTrue(state["presentation"]["overview"]["compact"])
        self.assertFalse(marker.exists())
        # A save refreshes the actual selected image while the grid stays present.
        before = self.row("first")["revisions"]
        png(self.root / "first.png", (190, 90, 60, 255), 128)
        harness.eventually(lambda: self.row("first")["revisions"] > before)
        self.rpc({"method": "select", "id": "second"})
        self.loaded("second")
        self.assertTrue(marker.exists())
        self.assertEqual(self.state()["active"], 1)
        self.assertFalse(self.row("first")["active"])
        self.rpc({"method": "hide"})
        self.assertEqual(self.state()["active"], 0)
        self.rpc({"method": "quit"})
        self.fixture.process.wait(timeout=5)
        with self.fixture.log_path.open("ab") as log:
            self.fixture.process = subprocess.Popen(
                [str(harness.BINARY), "--socket", str(self.fixture.socket_path), "--state", str(self.fixture.state_dir)],
                env=self.fixture.env, cwd=harness.PROJECT_ROOT, stdout=log, stderr=log, start_new_session=True)
        harness.eventually(self.fixture.ping)
        self.assertEqual(self.state()["layout"], "gallery")
        self.assertEqual(self.state()["selected"], "second")
        self.assertEqual(self.state()["project_filter"], "art")
        self.rpc({"method": "show"})
        self.loaded("second")

    def test_failed_filter_save_preserves_selection_and_visible_card(self):
        self.image("first")
        self.image("other", project="elsewhere")
        self.gallery()
        self.loaded("first")
        registry = self.fixture.state_dir / "previews.json"
        backup = self.fixture.state_dir / "before-gallery.json"
        registry.rename(backup)
        registry.mkdir()
        try:
            with self.assertRaises(RuntimeError):
                self.rpc({"method": "layout", "layout": "gallery", "project_filter": "elsewhere"})
        finally:
            registry.rmdir()
            backup.replace(registry)
        self.assertEqual(self.state()["selected"], "first")
        self.assertEqual(self.state()["project_filter"], "art")
        self.assertTrue(self.row("first")["active"])

    @unittest.skipUnless(harness.GPU, harness.GPU_REASON)
    def test_native_overview_group_gallery_buttons(self):
        self.image("first")
        self.image("second")
        self.image("other", project="elsewhere")
        self.image("loose", project="")
        self.rpc({"method": "theme", "mode": "dark"})
        self.rpc({"method": "layout", "layout": "overview", "group_projects": True, "project_filter": ""})
        self.rpc({"method": "show"})
        input = XInput()
        self.addCleanup(input.close)

        def groups():
            return self.state()["presentation"]["overview"]["groups"]

        def open_group(project, expected):
            # A lower header may need scrolling after resizing the private window.
            for _ in range(12):
                group = next(g for g in groups() if g["id"] == project)
                if "gallery_button" in group:
                    break
                viewport = self.state()["presentation"]["overview"]["viewport"]
                input.scroll(viewport["x"]+30, viewport["y"]+50, up=False)
                time.sleep(0.05)
            button = group["gallery_button"]
            input.click(button["x"]+button["width"]//2, button["y"]+button["height"]//2)
            harness.eventually(lambda: self.state()["layout"] == "gallery")
            state = self.state()
            self.assertEqual(state["project_filter"], project or "@ungrouped")
            self.assertEqual(state["selected"], expected)
            self.loaded(expected)
            self.assertEqual(self.state()["active"], 1)
            self.assertTrue(all("gallery_button" not in g and "close_button" not in g for g in groups()))
            input.press("Escape")
            harness.eventually(lambda: self.state()["layout"] == "overview")
            self.assertEqual(self.state()["project_filter"], "")

        state = self.state()
        viewport = state["presentation"]["overview"]["viewport"]
        group = next(g for g in groups() if g["id"] == "art")
        button, close = group["gallery_button"], group["close_button"]
        self.assertEqual(close["x"]+close["width"], viewport["x"]+viewport["width"]-12)
        self.assertEqual(button["x"]+button["width"]+8, close["x"])
        self.assertEqual(button["y"], viewport["y"]+15)
        self.assertEqual(close["y"], button["y"])
        self.assertEqual(button["height"], 20)
        self.assertEqual(close["height"], 20)
        harness.eventually(lambda: self.state()["presentation"]["overview"]["visible_ready"])
        self.rpc({"method": "capture", "path": str(self.root/"overview-group-gallery-desktop.png")})
        open_group("elsewhere", "other")
        self.rpc({"method": "select", "id": "second"})
        open_group("art", "second")
        open_group("", "loose")
        input.x.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
        input.x.XResizeWindow.restype = ctypes.c_int
        input.x.XResizeWindow(input.display, input.window, 620, 520)
        input.x.XSync(input.display, 0)
        harness.eventually(lambda: self.state()["presentation"]["width"] == 620)
        self.rpc({"method": "project", "id": "art", "label": "Flow character results with a deliberately long category name to verify header elision"})
        self.rpc({"method": "capture", "path": str(self.root/"overview-group-gallery-narrow.png")})
        open_group("", "loose")
        self.rpc({"method": "layout", "group_projects": False})
        self.assertEqual(groups(), [])
        # Close every group member, including entries below the scroll viewport.
        for index in range(12):
            self.image("extra-%02d" % index)
        self.rpc({"method": "layout", "group_projects": True, "project_filter": "art"})
        paths = [Path(row["path"]) for row in self.state()["entries"]]
        viewport = self.state()["presentation"]["overview"]["viewport"]
        for _ in range(12):
            input.scroll(viewport["x"]+30, viewport["y"]+50, up=True)
        input.x.XSync(input.display, 0)
        harness.eventually(lambda: self.state()["presentation"]["overview"]["scroll"] == 0)
        close = groups()[0]["close_button"]
        self.rpc({"method": "capture", "path": str(self.root/"overview-before-category-close.png")})
        input.click(close["x"]+close["width"]//2, close["y"]+close["height"]//2)
        try:
            harness.eventually(lambda: len(self.state()["entries"]) == 2)
        except AssertionError:
            (self.root/"category-close-failure.json").write_text(json.dumps(self.state(), indent=2))
            self.rpc({"method": "capture", "path": str(self.root/"overview-category-close-failure.png")})
            raise
        state = self.state()
        self.assertEqual({row["id"] for row in state["entries"]}, {"other", "loose"})
        self.assertEqual(state["project_filter"], "")
        self.assertNotIn("art", {project["id"] for project in state["projects"]})
        self.assertTrue(all(path.is_file() for path in paths))
        self.rpc({"method": "reopen"})
        self.assertIn("art", {project["id"] for project in self.state()["projects"]})
        self.rpc({"method": "layout", "project_filter": "@ungrouped"})
        close = groups()[0]["close_button"]
        input.click(close["x"]+close["width"]//2, close["y"]+close["height"]//2)
        harness.eventually(lambda: all(row["id"] != "loose" for row in self.state()["entries"]))
        self.assertTrue((self.root/"loose.png").is_file())
        self.assertIn("other", {row["id"] for row in self.state()["entries"]})

    @unittest.skipUnless(harness.GPU, harness.GPU_REASON)
    def test_native_grid_click_arrows_resize_escape_and_real_art(self):
        for i in range(27):
            self.image("image-%02d" % i)
        self.rpc({"method": "theme", "mode": "dark"})
        self.gallery()
        self.loaded("image-00")
        input = XInput()
        self.addCleanup(input.close)
        state = harness.eventually(lambda: self.state() if self.state()["presentation"]["overview"]["visible_ready"] else None)
        tile = state["presentation"]["overview"]["tiles"][1]
        input.click(tile["x"] + 20, tile["y"] + 55)
        harness.eventually(lambda: self.state()["selected"] == "image-01")
        self.loaded("image-01")
        self.assertEqual(self.state()["layout"], "gallery")
        self.assertEqual(self.state()["active"], 1)
        harness.eventually(lambda: any(t["id"]=="image-01" and t["thumbnail_ready"] for t in self.state()["presentation"]["overview"]["tiles"]))
        input.press("Right")
        harness.eventually(lambda: self.state()["selected"] == "image-02")
        self.loaded("image-02")
        state = self.state()
        viewport = self.row("image-02")["viewport"]
        thumbs = state["presentation"]["overview"]["viewport"]
        self.assertGreater(viewport["x"], thumbs["x"] + thumbs["width"])
        # Ordinary dragging and dragging after another zoom keep the same live
        # image and persist framing without disturbing the gallery selection.
        x, y = viewport["x"] + viewport["width"] // 2, viewport["y"] + viewport["height"] // 2
        before = self.row("image-02")["metrics"]["image_view"]
        input.drag(x, y, 35, 20)
        harness.eventually(lambda: self.row("image-02").get("image_view", {}).get("pan") != before["pan"])
        input.scroll(x, y)
        harness.eventually(lambda: self.row("image-02")["metrics"]["image_view"]["zoom"] > before["zoom"])
        before = self.row("image-02")["metrics"]["image_view"]
        input.drag(x, y, -25, -15)
        harness.eventually(lambda: self.row("image-02").get("image_view", {}).get("pan") != before["pan"])
        self.assertEqual(self.row("image-02")["metrics"]["loads"], 1)
        self.assertEqual(self.state()["selected"], "image-02")
        self.assertEqual(self.state()["active"], 1)
        self.rpc({"method": "capture", "path": str(self.root / "gallery-desktop.png")})
        # Gallery scrolling must not switch or activate the offscreen thumbnails.
        for _ in range(5):
            input.scroll(thumbs["x"] + 40, thumbs["y"] + 80, up=False)
        harness.eventually(lambda: self.state()["presentation"]["overview"]["scroll"] > 0)
        self.assertEqual(self.state()["active"], 1)
        input.x.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
        input.x.XResizeWindow.restype = ctypes.c_int
        input.x.XResizeWindow(input.display, input.window, 620, 520)
        input.x.XSync(input.display, 0)
        harness.eventually(lambda: self.state()["presentation"]["width"] == 620)
        narrow = self.state()
        self.assertLess(narrow["presentation"]["gallery"]["sidebar_width"], 230)
        self.rpc({"method": "capture", "path": str(self.root / "gallery-narrow.png")})
        input.press("Escape")
        harness.eventually(lambda: self.state()["layout"] == "overview")
        self.assertEqual(self.state()["active"], 0)
        self.rpc({"method": "layout", "layout": "gallery", "project_filter": "art"})
        self.loaded("image-02")
        # Exercise a native GL surface in the selected slot and verify the GPU.
        self.rpc({"method": "add", "entry": {"id": "model", "path": str(self.fixture.model), "project": "art"}})
        self.rpc({"method": "select", "id": "model"})
        model = harness.eventually(lambda: self.row("model") if self.row("model").get("metrics", {}).get("renderer") else None)
        renderer = model["metrics"]["renderer"]
        self.assertFalse(any(x in renderer.lower() for x in ("llvmpipe", "softpipe", "software", "unknown")), renderer)
        self.rpc({"method": "select", "id": "image-02"})
        self.loaded("image-02")
        self.assertEqual(self.state()["active"], 1)
        (self.root / "gallery-verification.json").write_text(json.dumps({"renderer": renderer, "native_input": True, "one_active_preview": True}, indent=2))


if __name__ == "__main__":
    unittest.main()
