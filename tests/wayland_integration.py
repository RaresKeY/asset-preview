"""Native Wayland hardware checks, without DISPLAY. Run tools/test.sh wayland-gpu."""
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

from integration import audio, client, eventually, memory, png, png_pixel, row, video
from inspection_fixture import PARTS, write_fixture
from inspection_integration import capture_pixels


class WaylandIntegration(unittest.TestCase):
    def setUp(self):
        self.assertTrue(os.environ.get('WAYLAND_DISPLAY'), 'Use tools/test.sh wayland-gpu')
        temporary = tempfile.TemporaryDirectory(prefix='preview-wayland-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.output = Path(os.environ.get('ASSET_PREVIEW_EVIDENCE_DIR', self.root)) / self._testMethodName
        self.output.mkdir(parents=True)
        environment = os.environ.copy()
        for key in ('DISPLAY', 'XAUTHORITY', 'QT_QPA_PLATFORM', 'CODEX_THREAD_ID', 'CODEX_SESSION_ID'):
            environment.pop(key, None)
        environment.update(ASSET_PREVIEW_RUNTIME_DIR=str(self.root / 'runtime'),
                           ASSET_PREVIEW_STATE_DIR=str(self.root / 'state'),
                           ASSET_PREVIEW_SMALL_CACHE=str(self.root / 'small-cache'),
                           ASSET_PREVIEW_TEXTURE_CACHE=str(self.root / 'texture-cache'),
                           ASSET_PREVIEW_TEST_NULL_AUDIO='1')
        scope = patch.dict(os.environ, environment, clear=True)
        scope.start()
        self.addCleanup(scope.stop)
        self.addCleanup(self.stop)
        spawn = client.subprocess.Popen
        with (self.output / 'server.log').open('wb') as log:
            def logged_spawn(*args, **kwargs):
                return spawn(*args, **{**kwargs, 'stdout': log, 'stderr': log})
            with patch.object(client.subprocess, 'Popen', side_effect=logged_spawn):
                client.start()
        state = client.rpc({'method': 'ping'})
        self.pid = state['pid']
        self.assertEqual(state['platform'], 'wayland')
        maps = Path(f'/proc/{self.pid}/maps').read_text()
        self.assertIn('/platforms/libqwayland.so', maps)
        self.assertNotIn('/platforms/libqxcb.so', maps)

    def stop(self):
        if client.running():
            client.rpc({'method': 'quit'})
            eventually(lambda: not client.running())
        for name in ('crash.log', 'crash.log.1'):
            path = self.root / 'state' / name
            if path.exists():
                (self.output / name).write_bytes(path.read_bytes())

    def add(self, key, path, **fields):
        client.rpc({'method': 'add', 'entry': {'id': key, 'path': str(path), **fields}})

    def hardware(self, key):
        renderer = row(key)['metrics']['renderer']
        self.assertTrue(renderer)
        self.assertFalse(any(word in renderer.lower() for word in ('llvmpipe', 'softpipe', 'software', 'unknown')), renderer)
        return renderer

    def capture(self, key, label):
        path = self.output / f'{label}.png'
        client.rpc({'method': 'capture', 'path': str(path)})
        pixels = capture_pixels(path, row(key)['viewport'])
        self.assertGreater(len(set(pixels)), 10, 'Rendered content must contain visible geometry/video')
        return pixels

    def receipt(self, **fields):
        (self.output / 'result.json').write_text(json.dumps({
            'passed': True, 'platform': client.rpc({'method': 'ping'})['platform'],
            'display_removed': 'DISPLAY' not in os.environ, 'memory': memory(self.pid), **fields,
        }, indent=2) + '\n')

    def test_images_models_and_materials(self):
        image = self.root / 'image.png'
        png(image, (220, 70, 80, 255))
        self.add('image', image)
        client.rpc({'method': 'show'})
        eventually(lambda: row('image').get('metrics', {}).get('loads', 0) == 1)
        png(image, (70, 180, 90, 255))
        eventually(lambda: row('image')['metrics']['loads'] == 2)
        capture = self.output / 'image.png'
        client.rpc({'method': 'capture', 'path': str(capture)})
        view = row('image')['viewport']
        self.assertEqual(png_pixel(capture, view['x'] + view['width']//2, view['y'] + view['height']//2), (70, 180, 90))

        model = self.root / 'model.glb'
        write_fixture(model)
        self.add('model', model)
        client.rpc({'method': 'select', 'id': 'model'})
        eventually(lambda: row('model').get('metrics', {}).get('engine'), timeout=30)
        renderer = self.hardware('model')
        before = self.capture('model', 'model')
        camera = row('model')['metrics']['camera']
        renders = row('model')['metrics']['renders']
        client.rpc({'method': 'camera_view', 'id': 'model', 'view': 'front'})
        eventually(lambda: row('model')['metrics']['camera'] != camera)
        eventually(lambda: row('model')['metrics']['renders'] > renders and not row('model')['metrics']['command_pending'])
        self.assertTrue(before != self.capture('model', 'model-front'), 'Camera change must publish a new visible frame')

        self.add('material', image, kind='material', settings={'shape': 'cube'})
        client.rpc({'method': 'select', 'id': 'material'})
        eventually(lambda: row('material').get('metrics', {}).get('engine'), timeout=30)
        self.hardware('material')
        self.capture('material', 'material')
        client.rpc({'method': 'settings', 'id': 'material', 'settings': {'shape': 'plane'}})
        eventually(lambda: row('material')['metrics']['triangles'] == 2)
        eventually(lambda: not row('material')['metrics']['command_pending'])
        time.sleep(1)
        renders = row('material')['metrics']['renders']
        time.sleep(.6)
        self.assertEqual(row('material')['metrics']['renders'], renders)
        client.rpc({'method': 'hide'})
        self.assertTrue(all('metrics' not in entry for entry in client.rpc({'method': 'list'})['entries']))
        self.receipt(renderer=renderer, checks=['image refresh pixels', 'model pixels and camera', 'material shape', 'idle rendering', 'hidden unload'])

    def test_video_audio_and_mixed_grid(self):
        clip = self.root / 'clip.mp4'
        video(clip, duration=8)
        self.add('video', clip, settings={'muted': True})
        client.rpc({'method': 'show'})
        eventually(lambda: row('video').get('metrics', {}).get('loads', 0) == 1, timeout=30)
        renderer = self.hardware('video')
        renders = row('video')['metrics']['renders']
        eventually(lambda: row('video')['metrics']['renders'] > renders + 3)
        client.rpc({'method': 'settings', 'id': 'video', 'settings': {'paused': True}})
        eventually(lambda: row('video')['metrics'].get('paused') is True)
        client.rpc({'method': 'seek', 'id': 'video', 'seconds': 2})
        eventually(lambda: abs(row('video')['metrics'].get('position', 0) - 2) < .2)
        self.capture('video', 'video-paused')

        sound = self.root / 'tone.wav'
        audio(sound)
        self.add('audio', sound, settings={'muted': True})
        image = self.root / 'image.png'
        png(image)
        self.add('image', image)
        model = self.root / 'model.glb'
        write_fixture(model)
        self.add('model', model)
        client.rpc({'method': 'layout', 'layout': 'grid', 'grid_size': 2})
        eventually(lambda: client.rpc({'method': 'list'})['active'] == 4)
        eventually(lambda: row('audio').get('metrics', {}).get('loads', 0) > 0, timeout=30)
        eventually(lambda: row('model').get('metrics', {}).get('engine'), timeout=30)
        self.hardware('model')
        self.capture('video', 'mixed-grid')
        client.rpc({'method': 'hide'})
        self.assertEqual(client.rpc({'method': 'list'})['active'], 0)
        self.assertTrue(all('metrics' not in entry for entry in client.rpc({'method': 'list'})['entries']))
        self.receipt(renderer=renderer, checks=['video playback', 'pause and seek', 'video pixels', 'audio load', 'four-type grid', 'hidden unload'])

    def test_inspection_parts_and_cuts(self):
        model = self.root / 'model.glb'
        write_fixture(model)
        self.add('model', model)
        def inspect(action='state', **fields):
            return client.rpc({'method': 'inspect', 'id': 'model', 'action': action, **fields})
        inspect('open', review=str(self.root / 'review.json'))
        self.assertEqual(client.rpc({'method': 'list'})['active'], 0)
        client.rpc({'method': 'show'})
        eventually(lambda: inspect().get('loaded'), timeout=30)
        eventually(lambda: row('model').get('metrics', {}).get('renders', 0) > 0)
        self.assertEqual(inspect()['part_count'], len(PARTS))
        renderer = self.hardware('model')
        before = self.capture('model', 'parts')
        loads = row('model')['metrics']['loads']
        inspect('cut', axis='x', position=0, keep='positive', enabled=True)
        after = self.capture('model', 'cuts')
        self.assertGreater(sum(a != b for a, b in zip(before, after)), 100)
        self.assertEqual(row('model')['metrics']['loads'], loads)
        client.rpc({'method': 'hide'})
        self.assertFalse(inspect()['loaded'])
        self.receipt(renderer=renderer, checks=['part inventory', 'inspection pixels', 'cut pixels', 'no reimport', 'hidden unload'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
