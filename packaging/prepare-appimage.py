#!/usr/bin/env python3
"""Retain the pinned AppImage runtime, corresponding work sources and notices."""
import hashlib,json,shutil,subprocess,tarfile,tempfile
from pathlib import Path
root=Path('/src');lock=json.loads((root/'packaging/appimage/runtime.json').read_text())
out=Path('/appimage-sources');out.mkdir(exist_ok=True)
components=[{'name':'runtime-x86_64','url':lock['url'],'sha256':lock['sha256']},*lock['sources']]
for component in components:
    p=out/component['name']
    subprocess.run(['curl','--fail','--silent','--show-error','--location','--retry','3',component['url'],'-o',str(p)],check=True)
    with p.open('rb') as f:sha=hashlib.file_digest(f,'sha256').hexdigest()
    if sha!=component['sha256']:raise RuntimeError('AppImage input checksum mismatch: '+p.name)
notices=out/'licenses';notices.mkdir()
for archive in out.glob('*.tar.*'):
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            name=Path(member.name).name.lower()
            if member.isfile() and (name.startswith(('license','copying','copyright')) or name=='readme' and archive.name.startswith('zlib')):
                p=notices/archive.name/Path(member.name)
                p.parent.mkdir(parents=True,exist_ok=True)
                with tar.extractfile(member) as source,p.open('wb') as dest:shutil.copyfileobj(source,dest)
shutil.copy2(root/'packaging/appimage/runtime.json',out/'runtime.json')
