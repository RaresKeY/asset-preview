"""Real native rebuild activation with private files and service sockets."""
import contextlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('preview_activation', ROOT / 'tools/activate-build.py')
activation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(activation)


class RebuildActivation(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='ap-rebuild-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.stage = self.root / 'build/.staging'
        self.stage.mkdir(parents=True)
        for source in (ROOT / 'build/.staging').glob('asset-preview-*'):
            if source.is_file():
                shutil.copy2(source, self.stage / source.name)
                shutil.copy2(source, self.root / 'build' / source.name)
        shutil.copy2(ROOT / 'build/backend-manifest.json', self.root / 'build/backend-manifest.json')
        self.addCleanup(patch.stopall)
        patch.object(activation, 'ROOT', self.root).start()
        patch.object(activation.client, 'ROOT', self.root).start()
        patch.dict(os.environ, {'ASSET_PREVIEW_RUNTIME_DIR': str(self.root / 'r'),
                              'ASSET_PREVIEW_STATE_DIR': str(self.root / 's'),
                              'QT_QPA_PLATFORM': 'offscreen'}).start()
        self.addCleanup(self.stop)
        activation.client.start()
        self.pid = activation.client.rpc({'method': 'ping'})['pid']
        asset = self.root / 'asset.txt'
        asset.write_text('A preserved preview')
        activation.client.rpc({'method': 'add', 'entry': {'id': 'retained', 'path': str(asset)}})
        self.binary = self.root / 'build/asset-preview-server'
        self.previous = self.binary.read_bytes()

    def stop(self):
        with contextlib.suppress(OSError, RuntimeError):
            activation.client.rpc({'method': 'quit'})
        for process in activation.client._SPAWNED:
            with contextlib.suppress(subprocess.TimeoutExpired):
                process.wait(timeout=5)

    def different_candidate(self):
        # A harmless ELF trailer changes the artifact identity while keeping
        # this test on the real compiled server and current backend contract.
        with (self.stage / 'asset-preview-server').open('ab') as stream:
            stream.write(b'\nrebuild-activation-fixture\n')

    def test_incomplete_and_incompatible_candidates_preserve_live_service(self):
        module = self.stage / 'asset-preview-mpv.so'
        previous = module.read_bytes()
        module.unlink()
        with self.assertRaises(RuntimeError):
            activation.activate(self.stage)
        self.assertEqual(activation.client.rpc({'method': 'ping'})['pid'], self.pid)
        source = self.root / 'mismatch.cpp'
        source.write_text('extern "C" const char* asset_preview_backend_abi() { return "incompatible"; }')
        subprocess.run(['c++', '-shared', '-fPIC', str(source), '-o', str(module)], check=True, capture_output=True)
        with self.assertRaises(subprocess.CalledProcessError):
            activation.activate(self.stage)
        self.assertEqual(activation.client.rpc({'method': 'ping'})['pid'], self.pid)
        self.assertEqual(self.binary.read_bytes(), self.previous)
        module.write_bytes(previous)

    def test_complete_build_restarts_once_and_unchanged_build_is_inert(self):
        self.assertFalse(activation.activate(self.stage))
        self.assertEqual(activation.client.rpc({'method': 'ping'})['pid'], self.pid)
        self.different_candidate()
        self.assertTrue(activation.activate(self.stage))
        current = activation.client.rpc({'method': 'list'})
        self.assertNotEqual(current['pid'], self.pid)
        self.assertEqual(current['selected'], 'retained')
        self.assertFalse(current['window_visible'])
        self.assertFalse(activation.activate(self.stage))
        self.assertEqual(activation.client.rpc({'method': 'ping'})['pid'], current['pid'])

    def test_no_restart_keeps_changed_artifacts_staged(self):
        self.different_candidate()
        self.assertFalse(activation.activate(self.stage, restart_running=False))
        self.assertEqual(activation.client.rpc({'method': 'ping'})['pid'], self.pid)
        self.assertEqual(self.binary.read_bytes(), self.previous)

    def test_legacy_method_routing_migrates_without_losing_registration(self):
        rpc = activation.client.rpc
        for message in ('Unknown method', 'Unknown preview: '):
            with self.subTest(legacy_error=message):
                self.different_candidate()
                def legacy_request(request, *args, **kwargs):
                    if request.get('method') == 'restart_state':
                        raise RuntimeError(message)
                    return rpc(request, *args, **kwargs)
                with patch.object(activation.client, 'rpc', side_effect=legacy_request):
                    self.assertTrue(activation.activate(self.stage))
                current = activation.client.rpc({'method': 'list'})
                self.assertEqual(current['selected'], 'retained')
                self.assertFalse(current['window_visible'])

    def test_failed_publication_rolls_back_before_restarting(self):
        self.different_candidate()
        with (self.stage / 'asset-preview-mpv.so').open('ab') as stream:
            stream.write(b'\npublication-fixture\n')
        replace = os.replace
        calls = 0
        def fail_second(source, target):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError('Injected second-artifact publication failure')
            return replace(source, target)
        with patch.object(activation.os, 'replace', side_effect=fail_second), self.assertRaises(OSError):
            activation.activate(self.stage)
        self.assertEqual(self.binary.read_bytes(), self.previous)
        current = activation.client.rpc({'method': 'list'})
        self.assertEqual(current['selected'], 'retained')
        self.assertFalse(current['window_visible'])


if __name__ == '__main__':
    unittest.main()
