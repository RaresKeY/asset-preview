#!/usr/bin/env python3
"""Create a relocatable dynamically linked Linux bundle from the build image."""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
src = Path('/src'); deps = Path('/opt/dependencies')
for name in ('bin', 'skills', 'fish', 'examples'):
    shutil.copytree(src/name, out/name, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
shutil.copy2(src/'packaging/container-service.py', out/'container-service.py')
for name in ('README.md', 'THIRD_PARTY_NOTICES.md', 'icon.svg'):
    shutil.copy2(src/name, out/name)
shutil.copytree(src/'docs', out/'docs')
# Preserve the documented bin/asset-preview entrypoint without host Python.
(out/'bin/asset-preview').rename(out/'bin/client.py')
(out/'bin/asset-preview').write_text('#!/bin/sh\nset -eu\npreview_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)\nexec "$preview_root/asset-preview" "$@"\n')
(out/'bin/asset-preview').chmod(0o755)
(out/'tools').mkdir(); shutil.copy2(src/'tools/install.py', out/'tools/install.py')
(out/'build').mkdir(); (out/'lib').mkdir()
roots = []
for name in ('asset-preview-server', 'asset-preview-f3d.so', 'asset-preview-mpv.so'):
    target=out/'build'/name; shutil.copy2(src/'build'/name,target); roots.append(src/'build'/name)
# Match the bundled interpreter's stdlib; omit optional GPL readline/GDBM modules.
pyver=f'python{sys.version_info.major}.{sys.version_info.minor}'
stdlib=Path('/usr/lib')/pyver
shutil.copytree(stdlib,out/'python/lib'/pyver,
    ignore=shutil.ignore_patterns('__pycache__','*.pyc','test','tests','ensurepip','tkinter','idlelib','dist-packages','lib-dynload'))
shutil.copy2(Path(sys.executable).resolve(),out/'python/python3'); roots.append(Path(sys.executable).resolve())
excluded_extensions={'_gdbm','_dbm','readline','_tkinter'}
extdir=out/'python/lib'/pyver/'lib-dynload';extdir.mkdir()
for p in (stdlib/'lib-dynload').glob('*.so'):
    if p.name.split('.')[0] not in excluded_extensions and not p.name.startswith('_test'): shutil.copy2(p,extdir/p.name);roots.append(p)
plugins=Path('/usr/lib/x86_64-linux-gnu/qt6/plugins')
for group in ('platforms','imageformats','xcbglintegrations','iconengines'):
    dst=out/'qt/plugins'/group;dst.mkdir(parents=True)
    for p in (plugins/group).glob('*.so'):
        if group=='platforms' and p.name not in ('libqxcb.so','libqoffscreen.so'):continue
        shutil.copy2(p,dst/p.name);roots.append(p)
# Keep glibc and the display/GPU dispatch libraries on the host. Vendor GPU
# drivers must match the running kernel; bundling them breaks portability.
external=re.compile(r'^(ld-linux|lib(c|m|pthread|dl|rt|resolv|util|anl)\.so|lib(GL|GLX|EGL|OpenGL|GLdispatch|vulkan|drm).*\.so)')
manifest={('@'+str(p)):str(p.resolve()) for p in roots if str(p).startswith('/usr/')};pending=list(roots);seen=set()
while pending:
    p=pending.pop();real=p.resolve()
    if real in seen:continue
    seen.add(real)
    result=subprocess.run(['ldd',str(p)],text=True,capture_output=True)
    if 'not found' in result.stdout:raise RuntimeError(f'Unresolved dependency for {p}: {result.stdout}')
    for line in result.stdout.splitlines():
        match=re.search(r'^\s*(\S+) => (/\S+)',line)
        if not match:continue
        soname,path=match.groups();dep=Path(path)
        if external.match(soname):continue
        dest=out/'lib'/soname
        if not dest.exists():shutil.copy2(dep.resolve(),dest)
        manifest[soname]=str(dep.resolve());pending.append(dep)
(out/'dependency-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
(out/'asset-preview').write_text('''#!/bin/sh
set -eu
preview_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ "${ASSET_PREVIEW_BUNDLE_ROOT:-}" != "$preview_root" ]; then
 export ASSET_PREVIEW_ORIGINAL_LD_LIBRARY_PATH="${LD_LIBRARY_PATH-}"
 export ASSET_PREVIEW_ORIGINAL_QT_PLUGIN_PATH="${QT_PLUGIN_PATH-}"
 export ASSET_PREVIEW_ORIGINAL_PYTHONHOME="${PYTHONHOME-}"
 export ASSET_PREVIEW_ORIGINAL_PYTHONNOUSERSITE="${PYTHONNOUSERSITE-}"
fi
export ASSET_PREVIEW_BUNDLE_ROOT="$preview_root"
export LD_LIBRARY_PATH="$preview_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$preview_root/qt/plugins"
export PYTHONHOME="$preview_root/python"
export PYTHONNOUSERSITE=1
if [ "${1:-}" = "--python" ]; then
 shift
 exec "$preview_root/python/python3" "$@"
fi
if [ "${1:-}" = "--foreground" ]; then
 shift
 exec "$preview_root/python/python3" "$preview_root/container-service.py" "$@"
fi
exec "$preview_root/python/python3" "$preview_root/bin/client.py" "$@"
''')
(out/'asset-preview').chmod(0o755)
# Dynamic linkage permits replacement of LGPL libraries. No static Qt/mpv link.
