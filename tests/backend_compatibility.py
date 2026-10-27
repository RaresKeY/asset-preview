#!/usr/bin/env python3
"""Reject rebuilt/legacy lazy modules before invoking their C++ factories."""
from pathlib import Path
import ctypes.util
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import integration as h

ROOT = h.ROOT


class BackendCompatibility(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="preview-backend-abi-")
        cls.root = Path(cls.temporary.name)
        cls.modules = {}
        # A bad factory terminates the process if the guard ever lets it run.
        factories = '\n'.join(
            f'extern "C" int asset_preview_create_{kind}() {{ _Exit(86); }}'
            for kind in ("model_worker", "video", "audio", "inspection")
        )
        signatures = {
            "legacy": "",
            "mismatch": 'extern "C" const char* asset_preview_backend_abi() { return "old-layout"; }',
            "null": 'extern "C" const char* asset_preview_backend_abi() { return nullptr; }',
            "matching": '#include "backend_abi_config.h"\nextern "C" const char* asset_preview_backend_abi() { return ASSET_PREVIEW_BACKEND_ABI; }',
        }
        for name, signature in signatures.items():
            source = cls.root / f"{name}.cpp"
            source.write_text('#include <cstdlib>\n' + signature + '\n' + factories)
            module = cls.root / f"{name}.so"
            subprocess.run(["c++", "-shared", "-fPIC", str(source), "-I" + str(ROOT / "build/generated"),
                            "-o", str(module)], check=True, capture_output=True)
            cls.modules[name] = module
        probe = cls.root / "probe.cpp"
        probe.write_text('''#include "backend_loader.h"
#include <QCoreApplication>
#include <iostream>
int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    QLibrary library(QString::fromLocal8Bit(argv[1]));
    library.setLoadHints(QLibrary::PreventUnloadHint);
    try { preview::resolveBackend(library,argv[2]); }
    catch (const std::exception& error) { std::cerr << error.what(); return 1; }
    return 0;
}
''')
        cls.probe = cls.root / "probe"
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "Qt6Core"], text=True))
        # Match the server's startup TLS shim and glibc allocation precedence.
        jemalloc = ctypes.util.find_library("jemalloc")
        if jemalloc:
            flags += ["-Wl,--no-as-needed", "-lstdc++", "-lc", "-l:" + jemalloc, "-Wl,--as-needed"]
        subprocess.run(["c++", "-std=c++20", "-fPIC", str(probe), "-I" + str(ROOT / "src"),
                        "-I" + str(ROOT / "build/generated"), "-o", str(cls.probe), *flags],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_contract_checked_before_factory_resolution(self):
        for name, module in self.modules.items():
            with self.subTest(signature=name):
                result = subprocess.run([str(self.probe), str(module), "asset_preview_create_video"],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0 if name == "matching" else 1, result.stderr)
                if name != "matching":
                    self.assertIn("restart Asset Preview", result.stderr)
        result = subprocess.run([str(self.probe), str(self.modules["matching"]), "missing_factory"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing_factory", result.stderr)

    def test_built_backend_contracts_match_server(self):
        for name, kinds in (("f3d", ("model_worker",)), ("mpv", ("video", "audio")), ("inspection", ("inspection",))):
            module = ROOT / f"build/asset-preview-{name}.so"
            if name == "inspection" and not module.exists():
                continue
            self.assertTrue(module.is_file(), f"Required backend is missing: {module}")
            for kind in kinds:
                with self.subTest(backend=kind):
                    result = subprocess.run([str(self.probe), str(module), f"asset_preview_create_{kind}"],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_running_server_accepts_the_matching_contract(self):
        root = self.root / "daemon-matching"
        (root / "build").mkdir(parents=True)
        shutil.copy2(ROOT / "build/asset-preview-server", root / "build/asset-preview-server")
        (root / "build/asset-preview-f3d.so").symlink_to(self.modules["matching"])
        environment = {"ASSET_PREVIEW_RUNTIME_DIR": str(root / "runtime"),
                       "ASSET_PREVIEW_STATE_DIR": str(root / "state"), "QT_QPA_PLATFORM": "offscreen"}
        with patch.dict(os.environ, environment), patch.object(h.client, "ROOT", root):
            h.client.start()
            process = h.client._SPAWNED[-1]
            try:
                path = root / "asset.obj"
                path.write_text("fixture")
                h.client.rpc({"method": "add", "entry": {"id": "model", "path": str(path)}})
                # The fixture factory exits with 86 only after the real server's
                # compiled fingerprint check succeeds, independently of the probe.
                try: h.client.rpc({"method": "show"})
                except (OSError,RuntimeError): pass
                # Backend resolution is asynchronous; the accepted worker factory
                # exits only after the shell has returned from Show.
                self.assertEqual(process.wait(timeout=5), 86)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)

    def test_incompatible_backends_leave_daemon_and_images_usable(self):
        for name in ("legacy", "mismatch", "null"):
            with self.subTest(signature=name):
                root = self.root / f"daemon-{name}"
                (root / "build").mkdir(parents=True)
                shutil.copy2(ROOT / "build/asset-preview-server", root / "build/asset-preview-server")
                for backend in ("f3d", "mpv", "inspection"):
                    (root / f"build/asset-preview-{backend}.so").symlink_to(self.modules[name])
                environment = {"ASSET_PREVIEW_RUNTIME_DIR": str(root / "runtime"),
                               "ASSET_PREVIEW_STATE_DIR": str(root / "state"),
                               "QT_QPA_PLATFORM": "offscreen"}
                with patch.dict(os.environ, environment), patch.object(h.client, "ROOT", root):
                    h.client.start()
                    process = h.client._SPAWNED[-1]
                    try:
                        for kind, extension in (("model", "obj"), ("video", "mp4"), ("audio", "wav")):
                            path = root / f"asset.{extension}"
                            path.write_bytes(b"fixture")
                            h.client.rpc({"method": "add", "entry": {"id": kind, "kind": kind, "path": str(path)}})
                            h.client.rpc({"method": "select", "id": kind})
                            h.client.rpc({"method": "show"})
                            h.eventually(lambda: "restart Asset Preview" in h.row(kind)["status"])
                            self.assertEqual(h.client.rpc({"method": "ping"})["pid"], process.pid)
                        image = root / "image.png"
                        h.png(image)
                        h.client.rpc({"method": "open", "paths": [str(image)]})
                        selected = h.client.rpc({"method": "list"})["selected"]
                        h.eventually(lambda: h.row(selected).get("metrics", {}).get("loads", 0) == 1)
                    finally:
                        if process.poll() is None:
                            try:
                                h.client.rpc({"method": "quit"})
                            except (OSError, RuntimeError):
                                process.terminate()
                        process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
