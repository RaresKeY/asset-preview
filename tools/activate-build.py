#!/usr/bin/env python3
"""Check a complete staged build before replacing the installed local viewer."""
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader('preview_activation_client', str(ROOT / 'bin/asset-preview'))
spec = importlib.util.spec_from_loader(loader.name, loader)
client = importlib.util.module_from_spec(spec)
loader.exec_module(client)


def activate(staging: Path, *, restart_running: bool = True) -> bool:
    staging = staging.resolve()
    destination = ROOT / 'build'
    manifest = json.loads((staging.parent / 'backend-manifest.json').read_text())
    names = ['asset-preview-server', 'asset-preview-f3d.so', 'asset-preview-mpv.so']
    if manifest['inspection']:
        names.append('asset-preview-inspection.so')
    for name in names:
        if not (staging / name).is_file():
            raise RuntimeError(f'Incomplete build: {name} is missing; running viewer retained')
    environment = client.viewer_environment()
    repair = Path.home() / '.local/share/f3d-runtime/lib'
    if (repair / 'libcudart.so.13').is_file():
        environment['LD_LIBRARY_PATH'] = str(repair) + ':' + environment.get('LD_LIBRARY_PATH', '')
    subprocess.run([str(staging / 'asset-preview-server'), '--check-backends'], env=environment, check=True)

    def digest(path):
        if not path.is_file():
            return None
        with path.open('rb') as source:
            return hashlib.file_digest(source, 'sha256').digest()

    changed = [name for name in names if digest(staging / name) != digest(destination / name)]
    running = client.running(timeout=15)
    stale = False
    if running:
        pid = client.rpc({'method': 'ping'})['pid']
        live, disk = Path(f'/proc/{pid}/exe').stat(), (destination / 'asset-preview-server').stat()
        stale = (live.st_dev, live.st_ino) != (disk.st_dev, disk.st_ino)
    if not changed and not stale:
        print('Build and running viewer are current.' if running else 'Build is current; viewer remains stopped.')
        return False
    if running and not restart_running:
        print('Validated build staged; running viewer preserved (--no-restart).')
        return False

    def publish():
        # All copies and rollback files exist before the first rename. The
        # exact service is stopped while the complete set is published.
        with tempfile.TemporaryDirectory(prefix='.activate-', dir=destination) as temporary:
            prepared = Path(temporary)
            existed = {name: (destination / name).exists() for name in changed}
            for name in changed:
                shutil.copy2(staging / name, prepared / name)
                if existed[name]:
                    shutil.copy2(destination / name, prepared / (name + '.previous'))
            replaced = []
            try:
                for name in changed:
                    os.replace(prepared / name, destination / name)
                    replaced.append(name)
            except OSError:
                for name in reversed(replaced):
                    if existed[name]:
                        os.replace(prepared / (name + '.previous'), destination / name)
                    else:
                        (destination / name).unlink()
                raise

    restarted = client.restart(if_running=True, before_start=publish)
    print('Activated complete build and restored the running viewer.' if restarted else 'Activated complete build; viewer remains stopped.')
    return True


if __name__ == '__main__':
    activate(Path(sys.argv[1]), restart_running='--no-restart' not in sys.argv[2:])
