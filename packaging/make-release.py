#!/usr/bin/env python3
"""Export the bundle, a self-extracting executable and exact corresponding sources."""
import hashlib, shutil, subprocess, sys
from pathlib import Path

def digest(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

out=Path(sys.argv[1]);bundle=out/'asset-preview'
version=(bundle/'VERSION').read_text().strip();release=out/'release';release.mkdir()
archive=release/f'asset-preview-{version}-linux-x86_64.tar.gz'
subprocess.run(['tar','-czf',str(archive),'-C',str(out),'asset-preview'],check=True)
header='''#!/bin/sh
set -eu
umask 077
preview_cache="${XDG_CACHE_HOME:-$HOME/.cache}/asset-preview/VERSION-BUNDLE_HASH"
if [ ! -x "$preview_cache/asset-preview/asset-preview" ]; then
 mkdir -p -- "$(dirname -- "$preview_cache")"
 preview_tmp=$(mktemp -d "${preview_cache}.XXXXXX")
 trap 'rm -rf -- "$preview_tmp"' EXIT HUP INT TERM
 tail -n +HEADER_LINES "$0" | tar -xz -C "$preview_tmp"
 if [ ! -d "$preview_cache" ]; then mv -- "$preview_tmp" "$preview_cache"; fi
 rm -rf -- "$preview_tmp"
 trap - EXIT HUP INT TERM
fi
exec "$preview_cache/asset-preview/asset-preview" "$@"
'''
header=header.replace('VERSION',version).replace('BUNDLE_HASH',digest(archive)[:16])
header=header.replace('HEADER_LINES',str(len(header.splitlines())+1))
p=release/f'asset-preview-{version}-linux-x86_64.run'
with p.open('wb') as f:
    f.write(header.encode())
    with archive.open('rb') as a:shutil.copyfileobj(a,f)
p.chmod(0o755)
source=release/f'asset-preview-{version}-corresponding-sources.tar.xz'
subprocess.run(['tar','-I','xz -T2 -0','-cf',str(source),'-C',str(out),'sources'],check=True)
(release/'SHA256SUMS').write_text(''.join(f'{digest(p)}  {p.name}\n' for p in (archive,p,source)))
