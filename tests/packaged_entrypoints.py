"""Exercise the packaged supervisor against an isolated real native service."""
import contextlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader('packaged_test_client', str(ROOT / 'bin/asset-preview'))
spec = importlib.util.spec_from_loader(loader.name, loader)
client = importlib.util.module_from_spec(spec)
loader.exec_module(client)


def eventually(check):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if check(): return
        time.sleep(.05)
    raise AssertionError('Packaged service did not reach the expected state')


class PackagedEntrypoints(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='preview-package-test-')
        self.root = Path(self.temporary.name)
        self.bundle = self.root / 'bundle'
        (self.bundle / 'bin').mkdir(parents=True)
        (self.bundle / 'build').mkdir()
        shutil.copy2(ROOT / 'packaging/container-service.py', self.bundle / 'container-service.py')
        shutil.copy2(ROOT / 'bin/asset-preview', self.bundle / 'bin/client.py')
        for name in ('VERSION', 'THIRD_PARTY_NOTICES.md'):
            shutil.copy2(ROOT / name, self.bundle / name)
        (self.bundle / 'build/asset-preview-server').symlink_to(ROOT / 'build/asset-preview-server')
        self.environment = patch.dict(os.environ, {
            'ASSET_PREVIEW_RUNTIME_DIR': str(self.root / 'runtime'),
            'ASSET_PREVIEW_STATE_DIR': str(self.root / 'state'),
            'QT_QPA_PLATFORM': 'offscreen', 'ASSET_PREVIEW_TEST_NULL_AUDIO': '1',
        })
        self.environment.start()
        self.processes = []
        self.log = (self.root / 'supervisor.log').open('wb')

    def tearDown(self):
        with contextlib.suppress(OSError, RuntimeError):
            client.rpc({'method': 'quit'})
        for process in self.processes:
            if process.poll() is None: process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=3)
        self.log.close()
        self.environment.stop()
        self.temporary.cleanup()

    def spawn(self, *arguments):
        process = subprocess.Popen([sys.executable, str(self.bundle / 'container-service.py'), *arguments],
                                   stdin=subprocess.DEVNULL, stdout=self.log, stderr=self.log, start_new_session=True)
        self.processes.append(process)
        return process

    def command(self, *arguments):
        return subprocess.run([sys.executable, str(self.bundle / 'container-service.py'), *arguments],
                              capture_output=True, text=True, timeout=10, check=True)

    def ready(self):
        return client.running() and client.rpc({'method': 'list'})['window_visible']

    def test_file_open_keeps_owner_alive_and_reuses_it(self):
        first, second = self.root / 'a texture.png', self.root / '-second.png'
        owner = self.spawn('open', str(first))
        eventually(self.ready)
        state = client.rpc({'method': 'list'})
        pid = state['pid']
        self.assertEqual(state['entries'][0]['path'], str(first))
        self.assertIsNone(owner.poll())
        self.command('--', str(second))
        state = client.rpc({'method': 'list'})
        self.assertEqual(state['pid'], pid)
        self.assertEqual(len(state['entries']), 2)
        self.assertEqual(next(row for row in state['entries'] if row['selected'])['path'], str(second))
        self.assertIsNone(owner.poll())
        owner.terminate()
        self.assertEqual(owner.wait(timeout=5), 0)
        eventually(lambda: not client.running())

    def test_concurrent_file_activations_have_one_owner(self):
        first = self.spawn('open', str(self.root / 'first.png'))
        second = self.spawn(str(self.root / 'second.png'))
        eventually(lambda: self.ready() and len(client.rpc({'method': 'list'})['entries']) == 2)
        eventually(lambda: sum(process.poll() is None for process in (first, second)) == 1)
        exited = next(process for process in (first, second) if process.poll() is not None)
        self.assertEqual(exited.returncode, 0)
        self.assertTrue(json.loads(self.command('status', '--json').stdout)['running'])

    def test_read_only_commands_and_completions_do_not_start_service(self):
        for arguments in [('paths',), ('--version',), ('--help',), ('--complete-ids',), ('status', '--json')]:
            self.command(*arguments)
        self.assertFalse((self.root / 'runtime').exists())
        self.assertFalse((self.root / 'state').exists())

    def test_start_remains_hidden_and_gui_activation_reuses_its_owner(self):
        owner = self.spawn('start')
        eventually(client.running)
        # Serialize behind the supervisor's activation request.
        eventually(lambda: (self.root / 'runtime/start.lock').exists())
        with (self.root / 'runtime/start.lock').open('r') as lock:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX)
        state = client.rpc({'method': 'list'})
        self.assertFalse(state['window_visible'])
        self.assertEqual(state['active'], 0)
        self.assertIsNone(owner.poll())
        pid = state['pid']
        self.command('start')
        self.assertFalse(client.rpc({'method': 'list'})['window_visible'])
        self.command('gui')
        eventually(self.ready)
        self.assertEqual(client.rpc({'method': 'list'})['pid'], pid)
        self.assertIsNone(owner.poll())


if __name__ == '__main__':
    unittest.main(verbosity=2)
