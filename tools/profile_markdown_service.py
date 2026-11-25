#!/usr/bin/env python3
"""Verify real Markdown scroll/resize and cache lifetime in a private service.

Run through tools/offscreen.sh with an explicit server and document. Native
input uses only that headless X server; the ordinary desktop is untouched.
"""
import ctypes
import argparse
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from files_integration import FilesIntegration
from integration import XInput, eventually


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('document', type=Path)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('--allow-image-eviction', action='store_true',
                        help='Exercise guides exceeding the pixel cache; visible misses may decode, resize must not sweep offscreen images')
    args = parser.parse_args()
    binary, document, evidence = (value.resolve() for value in (args.binary, args.document, args.evidence))
    if not os.environ.get('DISPLAY') or os.environ.get('QT_QPA_PLATFORM') != 'xcb':
        parser.error('Run through tools/offscreen.sh on its private X11 display')
    evidence.mkdir(parents=True, exist_ok=True)
    os.environ['ASSET_PREVIEW_BINARY'] = str(binary)
    os.environ['ASSET_PREVIEW_FILES_NATIVE'] = '1'
    os.environ['QT_XCB_NO_XI2'] = '1'
    fixture = FilesIntegration(methodName='runTest')
    native = None
    try:
        fixture.setUp()
        fixture.rpc({'method': 'add', 'entry': {'id': 'guide', 'kind': 'file', 'path': str(document)}})
        fixture.show('guide')
        fixture.rpc({'method': 'capture', 'path': str(evidence / 'start.png')})
        initial = fixture.row('guide')['metrics']
        assert initial['file_mode'] == 'rendered'
        assert initial['file_scroll_max'] > 0, 'Expected a scrollable Markdown document'
        # Native focus can consume a few pixels of the 14 px document inset
        # while making the start cursor visible. No content may be scrolled off.
        assert 0 <= initial['file_scroll_y'] < 14, ('Fresh Markdown should show its first line', initial)
        assert initial['file_image_decodes'] > 0
        native = XInput()
        native.x.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
        positions, resize_decodes = [], []
        for i in range(24):
            if i % 4 == 0:
                decodes_before = fixture.row('guide')['metrics']['file_image_decodes']
                width = 700 if i % 8 == 0 else 1050
                native.x.XResizeWindow(native.display, native.window, width, 750)
                native.x.XSync(native.display, 0)
                # XSync acknowledges the X server, not Qt's event handling.
                # Wait for the resulting application geometry before scrolling.
                def resized():
                    geometry = fixture.rpc({'method': 'list'})['presentation']
                    return geometry['width'] == width and geometry['height'] == 750
                eventually(resized)
                fixture.rpc({'method': 'capture', 'path': str(evidence / f'resized-{i:02}.png')})
                decoded = fixture.row('guide')['metrics']['file_image_decodes'] - decodes_before
                # These 700/1050 x 750 guide viewports expose at most three
                # screenshots. Layout must not reload the entire resource set.
                assert 0 <= decoded <= (3 if args.allow_image_eviction else 0), decoded
                resize_decodes.append(decoded)
            before = fixture.row('guide')['metrics']
            assert before['file_scroll_max'] > 0
            up = i >= 12
            if before['file_scroll_y'] == 0:
                up = False
            elif before['file_scroll_y'] == before['file_scroll_max']:
                up = True
            native.scroll(450, 300, up=up)
            native.x.XSync(native.display, 0)
            def scrolled():
                position = fixture.row('guide')['metrics']['file_scroll_y']
                moved = position < before['file_scroll_y'] if up else position > before['file_scroll_y']
                return {'y': position} if moved else None
            positions.append(eventually(scrolled)['y'])
        fixture.rpc({'method': 'capture', 'path': str(evidence / 'interacted.png')})
        interacted = fixture.row('guide')['metrics']
        if not args.allow_image_eviction:
            assert interacted['file_image_decodes'] == initial['file_image_decodes'], (initial, interacted)
        assert interacted['file_image_cache_cost'] <= 64 * 1024 * 1024
        fixture.rpc({'method': 'hide'})
        assert fixture.rpc({'method': 'list'})['active'] == 0
        assert 'metrics' not in fixture.row('guide')
        fixture.show('guide')
        fixture.rpc({'method': 'capture', 'path': str(evidence / 'reentered.png')})
        reentered = fixture.row('guide')['metrics']
        assert reentered['loads'] == 1, 'Expected a fresh FileView after hidden unload'
        assert 0 <= reentered['file_scroll_y'] < 14, reentered
        assert reentered['file_image_decodes'] == initial['file_image_decodes']
        assert reentered['file_image_cache_cost'] <= 64 * 1024 * 1024
        output = {'binary': str(binary), 'initial': initial, 'interacted': interacted, 'reentered': reentered,
                  'platform': fixture.rpc({'method': 'list'})['platform'],
                  'scroll_positions': positions,
                  'resize_image_decodes': resize_decodes,
                  'allows_visible_image_eviction': args.allow_image_eviction,
                  'checks': '24 acknowledged wheel actions, six captured resizes without offscreen decode sweeps, bounded cache, hide/unload and fresh reentry'}
        (evidence / 'service.json').write_text(json.dumps(output, indent=2) + '\n')
        print(json.dumps(output))
    finally:
        if native:
            native.close()
        fixture.doCleanups()


if __name__ == '__main__':
    main()
