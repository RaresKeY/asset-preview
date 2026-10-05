#!/usr/bin/env python3
"""Host-side command bridge for the Flatpak's private Unix socket."""
import contextlib
import fcntl
import os
from pathlib import Path
import runpy
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
APP_ID = "io.github.RaresKeY.AssetPreview"
os.environ.setdefault('ASSET_PREVIEW_RUNTIME_DIR', str(Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}'))/'asset-preview-flatpak'))
os.environ.setdefault('ASSET_PREVIEW_STATE_DIR', str(Path.home()/'.var/app'/APP_ID/'data/asset-preview'))
client = runpy.run_path(str(ROOT/'bin/client.py'), run_name='preview_client')
globals_ = client['main'].__globals__

def start_flatpak():
    runtime, state = client['locations']()
    client['private_directory'](runtime); client['private_directory'](state)
    fd = os.open(runtime/'start.lock', os.O_CREAT|os.O_RDWR|os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as lock:
        deadline = time.monotonic()+30
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB); break
            except BlockingIOError:
                if time.monotonic()>deadline:raise RuntimeError('Concurrent Flatpak startup timed out')
                time.sleep(.05)
        if client['running'](timeout=15):return False
        log_fd=os.open(state/'server.log',os.O_CREAT|os.O_WRONLY|os.O_TRUNC|os.O_NOFOLLOW,0o600)
        with os.fdopen(log_fd,'wb') as log:
            command=['flatpak','run','--user',
                '--env=ASSET_PREVIEW_RUNTIME_DIR='+str(runtime),
                '--env=ASSET_PREVIEW_STATE_DIR='+str(state),
                '--filesystem='+str(runtime)+':rw',
                '--filesystem='+str(state)+':rw',
                '--command=asset-preview',APP_ID,'--foreground']
            process=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=log,stderr=log,start_new_session=True)
        globals_['_SPAWNED'].append(process)
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            if client['running']():return True
            if process.poll() is not None:raise RuntimeError('Flatpak exited; see '+str(state/'server.log'))
            time.sleep(.05)
        with contextlib.suppress(ProcessLookupError):os.killpg(process.pid,15)
        raise RuntimeError('Flatpak startup timed out; see '+str(state/'server.log'))

globals_['start']=start_flatpak
if __name__=='__main__':
    try:raise SystemExit(client['main']())
    except (OSError,RuntimeError,ValueError) as error:raise SystemExit('asset-preview-flatpak: '+str(error))
