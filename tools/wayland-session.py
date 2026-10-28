#!/usr/bin/env python3
"""Run a full, private Wayland compositor inside the owning headless Gamescope."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) < 2:
        raise SystemExit('Usage: wayland-session.py COMMAND [ARGS...]')
    environment = os.environ.copy()
    # KWin's nested Wayland backend also needs wl_subcompositor. Use the
    # headless Gamescope X server only for KWin's outer output; application
    # clients connect exclusively to KWin's full native Wayland compositor.
    parent_display = environment['DISPLAY']
    with tempfile.TemporaryDirectory(prefix='preview-compositor-') as temporary:
        root = Path(temporary)
        for key, name in (('XDG_RUNTIME_DIR', 'runtime'), ('XDG_CONFIG_HOME', 'config'),
                          ('XDG_DATA_HOME', 'data'), ('XDG_STATE_HOME', 'state'), ('XDG_CACHE_HOME', 'cache')):
            directory = root / name
            directory.mkdir(mode=0o700)
            environment[key] = str(directory)
        environment.pop('WAYLAND_DISPLAY', None)
        environment.pop('WAYLAND_SOCKET', None)
        # The Gamescope WSI Vulkan layer is for applications, not a nested
        # compositor's device enumeration/teardown (the installed layer crashes).
        environment.update(QT_QPA_PLATFORM='xcb', DISABLE_GAMESCOPE_WSI='1', QT_NO_XDG_DESKTOP_PORTAL='1')
        environment['VK_LOADER_LAYERS_DISABLE'] = 'VK_LAYER_FROG_gamescope_wsi_x86_64'
        status = root / 'status.json'
        session = root / 'session.py'
        session.write_text('#!' + sys.executable + '\nimport json, subprocess\nfrom pathlib import Path\n'
                           + f'result = subprocess.run({sys.argv[1:]!r})\n'
                           + f'Path({str(status)!r}).write_text(json.dumps(result.returncode))\n')
        session.chmod(0o700)
        process = subprocess.Popen(['dbus-run-session', '--', 'kwin_wayland',
                                    '--x11-display', parent_display, '--socket', 'preview-test',
                                    '--width', environment.get('ASSET_PREVIEW_TEST_WIDTH', '1100'),
                                    '--height', environment.get('ASSET_PREVIEW_TEST_HEIGHT', '800'),
                                    '--no-lockscreen', '--no-global-shortcuts', '--no-kactivities',
                                    '--exit-with-session', str(session)], env=environment, start_new_session=True)
        try:
            process.wait(timeout=180)
            if not status.is_file():
                raise RuntimeError(f'Private Wayland session did not complete (compositor exit {process.returncode})')
            if process.returncode:
                raise RuntimeError(f'Private Wayland compositor exited {process.returncode}')
            return json.loads(status.read_text())
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


if __name__ == '__main__':
    raise SystemExit(main())
