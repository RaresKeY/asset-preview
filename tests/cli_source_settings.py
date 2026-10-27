"""CLI registration semantics; opt-in functional lane reuses the owning binary."""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
import shlex
import shutil
import subprocess
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
    def test_layout_has_no_compact_option(self):
        with patch.object(client, 'start'), patch.object(client, 'rpc', return_value={'ok': True}) as rpc:
            invoke('layout', 'grid', '--size', '3')
            self.assertEqual(rpc.call_args.args[0], {'method':'layout','layout':'grid','grid_size':3})
            for flag in ('--compact', '--no-compact'):
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                    invoke('layout', 'grid', flag)
                self.assertEqual(error.exception.code, 2)

    def test_project_send_identity_and_supporting_views(self):
        with tempfile.TemporaryDirectory() as temporary, patch.dict(os.environ, {}, clear=False):
            os.environ.pop('ASSET_PREVIEW_PROJECT_ID', None)
            os.environ.pop('ASSET_PREVIEW_PROJECT_LABEL', None)
            root=Path(temporary); (root/'.git').mkdir(); (root/'exports').mkdir()
            identity=client.project_identity(root)
            self.assertEqual(client.project_identity(root/'exports'),identity)
            with patch.object(client,'start'), patch.object(client,'rpc',return_value={'ok':True,'id':'image'}) as rpc:
                invoke('add',str(root/'exports/image.png'),'--id','image','--cwd',str(root))
                request=rpc.call_args.args[0]
                self.assertEqual(request['entry']['project'],identity[0]); self.assertTrue(request['make_current'])
                invoke('add',str(root/'image.png'),'--project','chair','--project-label','Chair','--no-current')
                request=rpc.call_args.args[0]
                self.assertEqual(request['entry']['project'],'chair'); self.assertEqual(request['project_label'],'Chair')
                self.assertFalse(request['make_current'])
                invoke('material',str(root/'albedo.png'),'--ungrouped')
                self.assertEqual(rpc.call_args.args[0]['entry']['project'],'')
                invoke('layout','projects','--size','auto','--project','all')
                self.assertEqual(rpc.call_args.args[0],{'method':'layout','layout':'projects','project_grid_size':0,'project_filter':''})
                invoke('layout','overview','--no-group-projects','--project','chair')
                self.assertEqual(rpc.call_args.args[0]['group_projects'],False)
                invoke('project','current','chair','image')
                self.assertEqual(rpc.call_args.args[0],{'method':'project','id':'chair','current':'image'})

    def test_open_paths_shorthand_and_option_separator(self):
        with patch.object(client, 'start') as start, patch.object(client, 'rpc', return_value={'ok': True}) as rpc:
            for arguments, names in [
                (('open', 'chair.glb', 'a texture.png'), ['chair.glb', 'a texture.png']),
                (('chair.glb', 'a texture.png'), ['chair.glb', 'a texture.png']),
                (('--', '-texture.png'), ['-texture.png']),
                (('open', '--', '-texture.png'), ['-texture.png']),
                (('open',), []),
                ((), None),
            ]:
                invoke(*arguments)
                expected = {'method': 'show'} if names is None else {'method': 'open', 'paths': [client.path(name) for name in names]}
                self.assertEqual(rpc.call_args.args[0], expected)
            self.assertEqual(start.call_count, 6)
            with tempfile.TemporaryDirectory(prefix='preview-command-') as temporary, contextlib.chdir(temporary):
                Path('open').touch()
                invoke('open', 'texture.png')
                self.assertEqual(rpc.call_args.args[0], {'method':'open','paths':[client.path('texture.png')]})

    @unittest.skipUnless(shutil.which('fish'), 'Fish is not installed')
    def test_fish_file_completions_are_read_only(self):
        with tempfile.TemporaryDirectory(prefix='preview-fish-') as temporary:
            directory = Path(temporary)
            (directory / 'a texture.png').touch()
            (directory / 'chair.glb').touch()
            runtime, state = directory / 'runtime', directory / 'state'
            environment = {**os.environ, 'ASSET_PREVIEW_RUNTIME_DIR': str(runtime), 'ASSET_PREVIEW_STATE_DIR': str(state),
                           'ASSET_PREVIEW_PROJECT': str(ROOT)}
            source = f'source {shlex.quote(str(ROOT / "fish/functions/asset-preview.fish"))}; source {shlex.quote(str(ROOT / "fish/completions/asset-preview.fish"))}; '
            for line, expected in [('asset-preview ', 'open'), ('asset-preview a', 'a texture.png'),
                                   ('asset-preview open a', 'a texture.png'),
                                   ('asset-preview open chair.glb a', 'a texture.png'),
                                   ('asset-preview chair.glb a', 'a texture.png')]:
                result = subprocess.run(['fish', '--no-config', '-c', source + 'complete -C ' + shlex.quote(line)],
                                        cwd=directory, env=environment, capture_output=True, text=True, check=True)
                self.assertIn(expected, result.stdout.replace('\\ ', ' '))
                self.assertEqual(result.stderr, '')
            subprocess.run(['fish', '--no-config', '-c', source + 'complete -C "asset-preview select "'],
                           cwd=directory, env=environment, capture_output=True, check=True)
            self.assertFalse(runtime.exists())
            self.assertFalse(state.exists())

    def test_atomic_add_and_defaults_requests(self):
        with patch.object(client, 'start'), patch.object(client, 'rpc', return_value={'id': 'ship'}) as rpc:
            invoke('add', '/tmp/ship.glb', '--id', 'ship', '--settings', '{"up_axis":"y","materials":true}', '--exec', 'builder', '--literal')
            entry = rpc.call_args.args[0]['entry']
            self.assertEqual(entry['settings'], {'up_axis': 'y', 'materials': True})
            self.assertEqual(entry['command'], ['builder', '--literal'])
            invoke('defaults', '3d', '{"textures":true}')
            self.assertEqual(rpc.call_args.args[0], {'method': 'defaults', 'group': '3d', 'settings': {'textures': True}})

    @unittest.skipUnless(shutil.which('fish'), 'Fish is not installed')
    def test_fish_does_not_treat_paths_or_option_values_as_commands(self):
        with tempfile.TemporaryDirectory(prefix='preview-fish-context-') as temporary:
            directory=Path(temporary)
            (directory/'a texture.png').touch()
            state=directory/'state'; state.mkdir()
            registry=state/'previews.json'
            registry.write_text(json.dumps({'entries':[{'id':'existing-preview','label':'Saved preview'}], 'projects':[{'id':'saved-project','label':'Saved project','current':'existing-preview'}]}))
            before=registry.read_bytes()
            environment={**os.environ,'ASSET_PREVIEW_PROJECT':str(ROOT),'ASSET_PREVIEW_STATE_DIR':str(state),
                         'ASSET_PREVIEW_RUNTIME_DIR':str(directory/'runtime')}
            source=f'source {shlex.quote(str(ROOT / "fish/functions/asset-preview.fish"))}; source {shlex.quote(str(ROOT / "fish/completions/asset-preview.fish"))}; '
            def complete(line):
                result=subprocess.run(['fish','--no-config','-c',source+'complete -C '+shlex.quote(line)],
                                      cwd=directory,env=environment,capture_output=True,text=True,check=True)
                self.assertEqual(result.stderr,'')
                return result.stdout.replace('\\ ', ' ')
            opened=complete('asset-preview open select ')
            self.assertIn('a texture.png',opened)
            self.assertNotIn('existing-preview',opened)
            self.assertNotIn('--size',complete('asset-preview open layout --'))
            flags=complete('asset-preview add chair.glb --label layout --')
            self.assertIn('--label',flags); self.assertNotIn('--size',flags)
            self.assertIn('existing-preview',complete('asset-preview select '))
            self.assertIn('saved-project',complete('asset-preview add chair.glb --project '))
            self.assertEqual(registry.read_bytes(),before)
            self.assertFalse((directory/'runtime').exists())

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
                    for key in ('selected', 'layout', 'grid_size', 'window_visible'):
                        self.assertEqual(after[key], before[key])
                    self.assertEqual(after['active'], 0)
                    invoke('defaults', '3d', '{"up_axis":"y","materials":true,"textures":true}')
                    self.assertEqual(client.rpc({'method': 'list'})['entries'], after['entries'])
                finally:
                    if client.running(): client.rpc({'method': 'quit'})


class DesktopEnvironment(unittest.TestCase):
    def test_agent_inherits_only_theme_metadata_and_preserves_overrides(self):
        caller = {'DISPLAY': ':headless', 'WAYLAND_DISPLAY': 'test-wayland', 'QT_STYLE_OVERRIDE': 'Fusion',
                  'QT_PLUGIN_PATH': '/bundle/plugins', 'XDG_CONFIG_DIRS': '/isolated/config',
                  'ASSET_PREVIEW_RUNTIME_DIR': '/private/runtime', 'PATH': '/caller/bin'}
        desktop = {'XDG_CURRENT_DESKTOP': 'KDE', 'KDE_FULL_SESSION': 'true', 'KDE_SESSION_VERSION': '6',
                   'QT_STYLE_OVERRIDE': 'Breeze', 'XDG_CONFIG_DIRS': '/desktop/config', 'DISPLAY': ':0',
                   'WAYLAND_DISPLAY': 'wayland-0', 'QT_PLUGIN_PATH': '/host/plugins', 'PATH': '/host/bin',
                   'ASSET_PREVIEW_RUNTIME_DIR': '/other/runtime', 'XDG_STATE_HOME': '/other/state',
                   'DBUS_SESSION_BUS_ADDRESS': 'other-bus', 'UNRELATED_PRIVATE_VALUE': 'test-sentinel'}
        result = subprocess.CompletedProcess([], 0, json.dumps(desktop).encode())
        with patch.dict(os.environ, caller, clear=True), patch.object(client.subprocess, 'run', return_value=result):
            environment = client.viewer_environment()
            self.assertEqual(dict(os.environ), caller)
        self.assertEqual(environment, {**caller, 'QT_QPA_PLATFORM': 'xcb', 'XDG_CURRENT_DESKTOP': 'KDE',
                                       'KDE_FULL_SESSION': 'true', 'KDE_SESSION_VERSION': '6'})

    def test_explicit_desktop_theme_and_headless_platforms_do_not_query_session(self):
        for caller in ({'XDG_CURRENT_DESKTOP': 'GNOME'}, {'XDG_CURRENT_DESKTOP': ''},
                       {'QT_QPA_PLATFORMTHEME': 'qt6ct'}, {'QT_QPA_PLATFORMTHEME': ''},
                       {'QT_QPA_PLATFORM': 'offscreen'}, {'QT_QPA_PLATFORM': 'minimal'}):
            with self.subTest(caller=caller), patch.dict(os.environ, caller, clear=True), \
                    patch.object(client.subprocess, 'run') as query:
                self.assertEqual(client.viewer_environment(), {'QT_QPA_PLATFORM': 'xcb', **caller})
                query.assert_not_called()

    def test_missing_or_invalid_session_is_optional(self):
        failures = [FileNotFoundError(), subprocess.TimeoutExpired('systemctl', 1),
                    subprocess.CompletedProcess([], 1, b''), subprocess.CompletedProcess([], 0, b'not JSON'),
                    subprocess.CompletedProcess([], 0, b'[]'), subprocess.CompletedProcess([], 0, b'\xff'),
                    subprocess.CompletedProcess([], 0, b' ' * (128 * 1024 + 1)),
                    subprocess.CompletedProcess([], 0, json.dumps({'XDG_CURRENT_DESKTOP': False,
                                                                 'QT_STYLE_OVERRIDE': 'bad\0value',
                                                                 'XDG_DATA_DIRS': 'x' * 4097}).encode())]
        for failure in failures:
            with self.subTest(failure=failure), patch.dict(os.environ, {}, clear=True), \
                    patch.object(client.subprocess, 'run', side_effect=failure if isinstance(failure, Exception) else None,
                                 return_value=failure):
                self.assertEqual(client.viewer_environment(), {'QT_QPA_PLATFORM': 'xcb'})

    def test_read_only_commands_and_existing_service_do_not_query_session(self):
        with tempfile.TemporaryDirectory(prefix='preview-session-read-') as temporary, \
                patch.dict(os.environ, {'ASSET_PREVIEW_RUNTIME_DIR': temporary + '/runtime',
                                        'ASSET_PREVIEW_STATE_DIR': temporary + '/state'}), \
                patch.object(client, 'running', return_value=False), \
                patch.object(client, 'viewer_environment') as environment:
            invoke('paths')
            invoke('status', '--json')
            invoke('--complete-ids')
            self.assertFalse(Path(temporary, 'runtime').exists())
            self.assertFalse(Path(temporary, 'state').exists())
            with patch.object(client, 'running', return_value=True):
                self.assertFalse(client.start())
            environment.assert_not_called()


if __name__ == '__main__': unittest.main(verbosity=2)
