"""CLI registration semantics; opt-in functional lane reuses the owning binary."""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader('preview_source_cli', str(ROOT / 'bin/asset-preview'))
spec = importlib.util.spec_from_loader(loader.name, loader)
client = importlib.util.module_from_spec(spec)
loader.exec_module(client)


def invoke(*args):
    with patch.object(sys, 'argv', ['asset-preview', *args]), contextlib.redirect_stdout(io.StringIO()):
        return client.main()


class SourceSettings(unittest.TestCase):
    def test_atomic_add_and_defaults_requests(self):
        with patch.object(client, 'start'), patch.object(client, 'rpc', return_value={'id': 'ship'}) as rpc:
            invoke('add', '/tmp/ship.glb', '--id', 'ship', '--settings', '{"up_axis":"y","materials":true}', '--exec', 'builder', '--literal')
            entry = rpc.call_args.args[0]['entry']
            self.assertEqual(entry['settings'], {'up_axis': 'y', 'materials': True})
            self.assertEqual(entry['command'], ['builder', '--literal'])
            invoke('defaults', '3d', '{"textures":true}')
            self.assertEqual(rpc.call_args.args[0], {'method': 'defaults', 'group': '3d', 'settings': {'textures': True}})

    def test_nonobject_is_not_sent(self):
        with patch.object(client, 'start'), patch.object(client, 'rpc') as rpc:
            for args in [('defaults', '3d', '[]'), ('add', '/tmp/a.glb', '--settings', '[]')]:
                with self.assertRaises(RuntimeError): invoke(*args)
            rpc.assert_not_called()

    @unittest.skipUnless(os.environ.get('ASSET_PREVIEW_SOURCE_TEST') == '1', 'Opt-in private native service')
    def test_real_preferences_same_id_override_and_preservation(self):
        with tempfile.TemporaryDirectory(prefix='preview-source-') as temporary:
            environment = {'ASSET_PREVIEW_RUNTIME_DIR': temporary + '/runtime', 'ASSET_PREVIEW_STATE_DIR': temporary + '/state', 'QT_QPA_PLATFORM': 'offscreen'}
            # Worktree CLI is exercised against the existing normal-checkout binary.
            binary_root = Path(os.environ.get('ASSET_PREVIEW_BINARY_PROJECT', ROOT))
            with patch.dict(os.environ, environment), patch.object(client, 'ROOT', binary_root):
                try:
                    client.start()
                    invoke('defaults', '3d', '{"up_axis":"z","materials":false,"textures":false,"edges":true}')
                    invoke('add', temporary + '/ship.glb', '--id', 'ship', '--kind', 'model')
                    before = client.rpc({'method': 'list'})
                    self.assertEqual(before['entries'][0]['settings']['up_axis'], 'z')
                    invoke('add', temporary + '/ship.glb', '--id', 'ship', '--kind', 'model', '--settings', '{"up_axis":"y","materials":true,"textures":true}')
                    after = client.rpc({'method': 'list'})
                    settings = after['entries'][0]['settings']
                    self.assertEqual([settings[k] for k in ('up_axis', 'materials', 'textures', 'edges')], ['y', True, True, True])
                    self.assertEqual(after['preferences'], before['preferences'])
                    for key in ('selected', 'layout', 'grid_size', 'compact', 'window_visible'):
                        self.assertEqual(after[key], before[key])
                    self.assertEqual(after['active'], 0)
                    invoke('defaults', '3d', '{"up_axis":"y","materials":true,"textures":true}')
                    self.assertEqual(client.rpc({'method': 'list'})['entries'], after['entries'])
                finally:
                    if client.running(): client.rpc({'method': 'quit'})


if __name__ == '__main__': unittest.main(verbosity=2)
