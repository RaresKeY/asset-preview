"""Native Wayland hardware checks, without DISPLAY. Run tools/test.sh wayland-gpu."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct
import tempfile
import time
import unittest
from unittest.mock import patch

from integration import audio, client, eventually, memory, png, png_pixel, row, video
from inspection_fixture import PARTS, write_fixture
from inspection_integration import capture_pixels, png_rgb


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
        if self._testMethodName == 'test_overview_stays_raster_after_native_layouts':
            environment.update(QT_SCALE_FACTOR='1.25', QT_STYLE_OVERRIDE='Breeze')
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

    def compositor_capture(self, key, label):
        # The service's capture composites the source texture into QWidget::grab.
        # Only a compositor screenshot catches display draw-buffer, alpha or
        # stacking failures when the source texture itself is correct.
        self.assertTrue(shutil.which('spectacle'), 'Wayland display checks require installed Spectacle')
        eventually(lambda: not row(key)['metrics'].get('command_pending'), timeout=30)
        path = self.output / f'{label}-compositor.png'
        subprocess.run(['spectacle', '-b', '-n', '-a', '-e', '-S', '-o', str(path)],
                       check=True, capture_output=True, timeout=15)
        state = client.rpc({'method': 'list'})
        width, height = struct.unpack('>II', path.read_bytes()[16:24])
        self.assertEqual((width, height), (state['presentation']['width'], state['presentation']['height']))
        pixels = capture_pixels(path, row(key)['viewport'])
        self.assertGreater(len(set(pixels)), 10, 'The compositor must display the native scene, not a blank card')
        expected = self.capture(key, f'{label}-texture')
        self.assertEqual(len(pixels), len(expected))
        errors = [max(abs(a-b) for a, b in zip(actual, reference)) for actual, reference in zip(pixels, expected)]
        self.assertLess(sum(error > 3 for error in errors) / len(errors), .02,
                        'Composited native content must match the rendered scene')
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
        self.assertEqual(row('model')['metrics']['frame_transport'], 'shared_gpu_texture')
        self.assertEqual(row('model')['metrics']['display_readbacks'], 0)
        self.compositor_capture('model', 'model-initial')
        before = self.capture('model', 'model')
        reads = row('model')['metrics']['display_readbacks']
        camera = row('model')['metrics']['camera']
        renders = row('model')['metrics']['renders']
        client.rpc({'method': 'camera_view', 'id': 'model', 'view': 'front'})
        eventually(lambda: row('model')['metrics']['camera'] != camera)
        eventually(lambda: row('model')['metrics']['renders'] > renders and not row('model')['metrics']['command_pending'])
        self.assertEqual(row('model')['metrics']['display_readbacks'], reads, 'Live camera frames must remain on the GPU')
        self.assertTrue(before != self.capture('model', 'model-front'), 'Camera change must publish a new visible frame')
        self.compositor_capture('model', 'model-front')

        self.add('material', image, kind='material', settings={'shape': 'cube'})
        client.rpc({'method': 'select', 'id': 'material'})
        eventually(lambda: row('material').get('metrics', {}).get('engine'), timeout=30)
        self.hardware('material')
        self.compositor_capture('material', 'material')
        self.capture('material', 'material')
        client.rpc({'method': 'settings', 'id': 'material', 'settings': {'shape': 'plane'}})
        eventually(lambda: row('material')['metrics']['triangles'] == 2)
        eventually(lambda: not row('material')['metrics']['command_pending'])
        self.compositor_capture('material', 'material-plane')
        client.rpc({'method': 'layout', 'layout': 'overview'})
        eventually(lambda: client.rpc({'method': 'list'})['active'] == 0)
        client.rpc({'method': 'layout', 'layout': 'single'})
        client.rpc({'method': 'select', 'id': 'model'})
        eventually(lambda: row('model').get('metrics', {}).get('engine'), timeout=30)
        self.compositor_capture('model', 'model-from-overview')
        client.rpc({'method': 'hide'})
        client.rpc({'method': 'show'})
        eventually(lambda: row('model').get('metrics', {}).get('engine'), timeout=30)
        self.compositor_capture('model', 'model-reopened')
        time.sleep(1)
        renders = row('model')['metrics']['renders']
        time.sleep(.6)
        self.assertEqual(row('model')['metrics']['renders'], renders)
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
        self.compositor_capture('video', 'video-paused')

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
        self.compositor_capture('model', 'mixed-grid-model')
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
        self.compositor_capture('model', 'parts-native')
        before = self.capture('model', 'parts')
        loads = row('model')['metrics']['loads']
        inspect('cut', axis='x', position=0, keep='positive', enabled=True)
        after = self.capture('model', 'cuts')
        self.compositor_capture('model', 'cuts-native')
        self.assertGreater(sum(a != b for a, b in zip(before, after)), 100)
        self.assertEqual(row('model')['metrics']['loads'], loads)
        client.rpc({'method': 'hide'})
        self.assertFalse(inspect()['loaded'])
        self.receipt(renderer=renderer, checks=['part inventory', 'inspection pixels', 'cut pixels', 'no reimport', 'hidden unload'])

    def test_overview_stays_raster_after_native_layouts(self):
        model = self.root / 'model.glb'
        write_fixture(model)
        self.add('model', model, project='models')
        clip = self.root / 'clip.mp4'
        video(clip, duration=1)
        self.add('video', clip, project='media', settings={'paused': True, 'muted': True})
        for i in range(16):
            image = self.root / f'image-{i}.png'
            png(image, (40 + i * 10, 110, 190, 255))
            self.add(f'image-{i}', image, project='references')
        client.rpc({'method': 'theme', 'mode': 'dark'})
        client.rpc({'method': 'layout', 'layout': 'projects', 'project_grid_size': 2})
        client.rpc({'method': 'show'})
        eventually(lambda: row('model').get('metrics', {}).get('engine'), timeout=30)
        eventually(lambda: row('video').get('metrics', {}).get('loads'), timeout=30)
        renderer = self.hardware('model')
        checks = []
        for layout in ('projects', 'single', 'grid'):
            if layout != 'projects':
                client.rpc({'method': 'layout', 'layout': layout})
                client.rpc({'method': 'select', 'id': 'model'})
                eventually(lambda: row('model').get('metrics', {}).get('engine'), timeout=30)
            client.rpc({'method': 'layout', 'layout': 'overview', 'group_projects': False})
            eventually(lambda: client.rpc({'method': 'list'})['presentation']['overview']['visible_ready'])
            time.sleep(.2)
            state = client.rpc({'method': 'list'})
            overview = state['presentation']['overview']
            self.assertEqual(state['active'], 0)
            self.assertFalse(overview['native'])
            self.assertEqual(overview['native_ancestors'], 0)
            actual = self.output / f'from-{layout}-compositor.png'
            environment = {key: value for key, value in os.environ.items() if key != 'QT_SCALE_FACTOR'}
            subprocess.run(['spectacle', '-b', '-n', '-a', '-e', '-S', '-o', str(actual)],
                           env=environment, check=True, capture_output=True, timeout=15)
            expected = self.output / f'from-{layout}-reference.png'
            client.rpc({'method': 'capture', 'path': str(expected)})
            actual_size = struct.unpack('>II', actual.read_bytes()[16:24])
            expected_size = struct.unpack('>II', expected.read_bytes()[16:24])
            self.assertTrue(all(abs(a-b) <= 1 for a, b in zip(actual_size, expected_size)))
            scale = actual_size[0] / state['presentation']['width']
            self.assertAlmostEqual(scale, 1.25, places=2)
            # QWidget::grab and the compositor round the outer fractional frame
            # differently. Check unique thumbnail colors at independent layout
            # coordinates; the raster harness compares partial/clean pixels.
            viewport = overview['viewport']
            points = []
            for tile in overview['tiles']:
                if not tile['id'].startswith('image-'):
                    continue
                y = tile['y'] + 50
                if not viewport['y'] + 2 < y < viewport['y'] + viewport['height'] - 2:
                    continue
                index = int(tile['id'].split('-')[1])
                points.append((int((tile['x'] + tile['width'] / 2) * scale), int(y * scale), (40 + index * 10, 110, 190)))
            self.assertGreaterEqual(len(points), 4)
            for path in (actual, expected):
                _, _, rows = png_rgb(path)
                for x, y, color in points:
                    self.assertLessEqual(max(abs(a-b) for a, b in zip(rows[y][x*3:x*3+3], color)), 3,
                                         f'{path.name}: thumbnail at ({x}, {y}) must match its asset')
            checks.append(f'{layout} to Overview: fractional compositor thumbnail positions and raster ancestry')
        self.receipt(renderer=renderer, scale=scale, checks=checks)


if __name__ == '__main__':
    unittest.main(verbosity=2)
