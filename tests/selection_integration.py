#!/usr/bin/env python3
"""Exercise selection registration, native service lifecycle and durable reads."""
from __future__ import annotations
import contextlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest
from integration import eventually

ROOT = Path(__file__).resolve().parents[1]


class SelectionIntegration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ap-selection-service-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.runtime = self.root / 'runtime'; self.runtime.mkdir(mode=0o700)
        self.state = self.root / 'state'; self.state.mkdir(mode=0o700)
        self.socket = self.runtime / 'preview.sock'
        self.env = dict(os.environ, ASSET_PREVIEW_RUNTIME_DIR=str(self.runtime),
                        ASSET_PREVIEW_STATE_DIR=str(self.state))
        self.env.setdefault('QT_QPA_PLATFORM', 'offscreen')
        self.process = None
        self.log = (self.root / 'server.log').open('ab')
        self.addCleanup(self.log.close); self.addCleanup(self.stop)
        self.start()

    def start(self):
        self.process = subprocess.Popen([str(ROOT / 'build/asset-preview-server'), '--socket', str(self.socket),
                                         '--state', str(self.state)], env=self.env, stdout=self.log, stderr=self.log)
        def ready():
            if self.process.poll() is not None:
                raise AssertionError((self.root / 'server.log').read_text())
            try: return self.rpc({'method': 'ping'})
            except OSError: return False
        eventually(ready)

    def stop(self):
        if self.process and self.process.poll() is None:
            with contextlib.suppress(OSError): self.rpc({'method': 'quit'})
            self.process.wait(timeout=8)

    def rpc(self, obj):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(15); client.connect(str(self.socket)); client.sendall(json.dumps(obj).encode()+b'\n')
            data = bytearray()
            while b'\n' not in data:
                part = client.recv(65536)
                if not part: raise OSError('Service disconnected')
                data.extend(part)
        result = json.loads(data)
        if not result.get('ok'): raise RuntimeError(result.get('error'))
        return result

    def row(self):
        return next(x for x in self.rpc({'method': 'list'})['entries'] if x['id'] == 'choices')

    def test_registration_reload_results_and_restart(self):
        source = self.root / 'choices.selection.json'; output = self.root / 'answers.json'
        manifest = {'schema': 'asset-preview-selection/v1', 'id': 'release-options', 'title': 'Release packages',
                    'items': [{'id': 'a', 'title': 'First package', 'markdown': '**Exact version:** 1.0'}]}
        source.write_text(json.dumps(manifest))
        cli = subprocess.run([str(ROOT / 'bin/asset-preview'), 'add', str(source), '--id', 'choices',
                              '--decisions', str(output), '--ungrouped'], env=self.env, text=True, capture_output=True)
        self.assertEqual(cli.returncode, 0, cli.stderr)
        self.assertEqual(self.row()['kind'], 'selection')
        self.assertFalse(self.rpc({'method': 'list'})['window_visible'])
        self.assertFalse(output.exists())
        self.rpc({'method': 'layout', 'layout': 'single'}); self.rpc({'method': 'select', 'id': 'choices'}); self.rpc({'method': 'show'})
        eventually(lambda: self.row().get('metrics', {}).get('loads', 0) > 0)
        metrics = self.row()['metrics']; self.assertEqual(metrics['selection_result_path'], str(output))
        self.assertEqual(metrics['selection_pending'], 1)
        self.assertFalse(output.exists(), 'viewing must never record acceptance')
        # Simulate another local viewer's result; native clicks are covered by
        # selection_preview.cpp, using private fixture files only.
        output.write_text(json.dumps({'schema': 'asset-preview-decisions/v1', 'selection_id': 'release-options',
                                     'decisions': {'a': dict(metrics['selection_decisions'][0], actor='user', decision='accepted')}}))
        eventually(lambda: self.row()['metrics']['selection_accepted'] == 1)
        manifest['items'][0]['markdown'] = '**Exact version:** 2.0'
        source.write_text(json.dumps(manifest))
        eventually(lambda: self.row()['metrics']['selection_pending'] == 1)
        self.rpc({'method': 'hide'}); self.assertNotIn('metrics', self.row())
        self.assertEqual(json.loads(output.read_text())['decisions']['a']['decision'], 'accepted')
        self.stop(); self.start()
        self.assertEqual(self.row()['decisions'], str(output))
        self.rpc({'method': 'show'})
        eventually(lambda: self.row().get('metrics', {}).get('selection_pending') == 1)
        source.write_text('{unfinished')
        eventually(lambda: self.row()['status'].startswith('Waiting'))
        self.assertFalse(self.row()['metrics']['selection_writable'])
        self.assertEqual(self.row()['metrics']['selection_total'], 1)
        output.write_text(output.read_text() + '\n')
        (self.root / 'unrelated-save.txt').write_text('directory change')
        # Result/directory notifications must not re-enable a stale document.
        import time
        time.sleep(.2)
        self.assertFalse(self.row()['metrics']['selection_writable'])
        manifest['items'] = []; source.write_text(json.dumps(manifest))
        eventually(lambda: self.row()['metrics']['selection_total'] == 0)


if __name__ == '__main__': unittest.main()
