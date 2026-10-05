#!/usr/bin/env python3
import hashlib, subprocess, sys
from pathlib import Path
out=Path(sys.argv[1]); name='asset-preview-0.0.1-linux-x86_64.run'
archive=out/'bundle.tar.gz'
subprocess.run(['tar','-czf',str(archive),'-C',str(out),'asset-preview'],check=True)
header='''#!/bin/sh
set -eu
umask 077
preview_cache="${XDG_CACHE_HOME:-$HOME/.cache}/asset-preview/0.0.1-BUNDLE_HASH"
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
header=header.replace('BUNDLE_HASH',hashlib.sha256(archive.read_bytes()).hexdigest()[:16])
header=header.replace('HEADER_LINES',str(len(header.splitlines())+1))
p=out/name
with p.open('wb') as f:
 f.write(header.encode())
 with archive.open('rb') as a:
  import shutil;shutil.copyfileobj(a,f)
p.chmod(0o755);archive.unlink()
source=out/'asset-preview-0.0.1-corresponding-sources.tar.xz'
subprocess.run(['tar','-cJf',str(source),'-C',str(out),'sources'],check=True)
(out/'SHA256SUMS').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in (out/name,source)))
