#!/usr/bin/env python3
"""Own the packaged foreground service for GUI/file opens and forward stop signals."""
import fcntl
import os
import runpy
import signal
import subprocess
import sys
import time
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parent
    client = runpy.run_path(str(root / 'bin/client.py'), run_name='preview_client')
    # This private read-only completion path is not an ordinary CLI action.
    if sys.argv[1:] in (['--complete-ids'], ['--complete-projects']):
        return client['main']()
    args = client['parse_arguments'](sys.argv[1:])
    action = args.action or 'gui'
    if action not in {'gui', 'open', 'start'}:
        return client['main']()
    request = ({'method': 'open', 'paths': [client['path'](name) for name in args.files]} if action == 'open'
               else {'method': 'ping' if action == 'start' else 'show'})
    runtime, state = client['locations']()
    client['private_directory'](runtime)
    client['private_directory'](state)
    child = None
    stopped = False

    def stop(signum, frame):
        nonlocal stopped
        stopped = True
        if child is not None and child.poll() is None:
            child.send_signal(signum)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        fd = os.open(runtime / 'start.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'w') as lock:
            deadline = time.monotonic() + 15
            while True:
                if stopped: return 0
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() > deadline: raise RuntimeError('Concurrent preview startup timed out')
                    time.sleep(.05)
            if not client['running'](timeout=15):
                if stopped: return 0
                environment = client['viewer_environment']()
                child = subprocess.Popen([str(root / 'build/asset-preview-server'), '--socket', str(runtime / 'preview.sock'),
                                          '--state', str(state)], env=environment)
                deadline = time.monotonic() + 15
                while not client['running']():
                    if stopped: return 0
                    if child.poll() is not None: raise RuntimeError('Preview service exited during startup')
                    if time.monotonic() > deadline: raise RuntimeError('Preview startup timed out')
                    time.sleep(.05)
            if stopped: return 0
            client['rpc'](request)
        # A second desktop activation only sends its request. The supervisor
        # that created the native child retains ownership until shutdown.
        return child.wait() if child is not None else 0
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try: child.wait(timeout=15)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        raise SystemExit('asset-preview: ' + str(error))
