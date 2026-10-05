#!/usr/bin/env python3
"""Keep the container's service in the foreground and forward stop signals."""
import os, runpy, signal, subprocess, time
from pathlib import Path
root=Path(__file__).resolve().parent
client=runpy.run_path(str(root/'bin/asset-preview'),run_name='preview_client')
runtime,state=client['locations']()
client['private_directory'](runtime);client['private_directory'](state)
child=subprocess.Popen([str(root/'build/asset-preview-server'),'--socket',str(runtime/'preview.sock'),'--state',str(state)])
def stop(signum,frame):
    if child.poll() is None:child.send_signal(signum)
signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
try:
    deadline=time.monotonic()+15
    while not client['running']():
        if child.poll() is not None:raise RuntimeError('Preview service exited during startup')
        if time.monotonic()>deadline:raise RuntimeError('Preview startup timed out')
        time.sleep(.05)
    client['rpc']({'method':'show'})
    raise SystemExit(child.wait())
finally:
    if child.poll() is None:
        child.terminate()
        try:child.wait(timeout=15)
        except subprocess.TimeoutExpired:child.kill();child.wait()
