#!/usr/bin/env python3
"""Ship exact Ubuntu source packages and notices for bundled runtime libraries."""
import hashlib, json, shutil, subprocess, sys
from urllib.parse import urlencode, urlparse
from urllib.request import urlopen
from pathlib import Path
bundle=Path(sys.argv[1]);out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
licenses=bundle/'licenses';licenses.mkdir()
shutil.copytree('/usr/share/common-licenses',licenses/'common-licenses')
packages=set(Path('/runtime-packages.txt').read_text().splitlines());sources=set()
for path in json.loads((bundle/'dependency-manifest.json').read_text()).values():
    if path.startswith('/opt/dependencies/'):continue
    result=subprocess.run(['dpkg-query','-S',path],text=True,capture_output=True)
    if result.returncode:
        # usrmerge aliases are recorded under /lib in some packages.
        result=subprocess.run(['dpkg-query','-S',path.replace('/usr/lib/','/lib/',1)],text=True,capture_output=True)
    if result.returncode:raise RuntimeError('No package owner: '+path)
    packages.add(result.stdout.split(': /',1)[0])
packages.add('fontconfig-config');packages.add('libopengl0');packages.add('python3.12-minimal');packages.add('libpython3.12-stdlib')
for package in sorted(packages):
    info=subprocess.check_output(['dpkg-query','-W','-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n',package],text=True).strip().split('\t')
    source,version=info[2:];sources.add(source+'='+version)
    notice=Path('/usr/share/doc')/package.split(':')[0]/'copyright'
    if not notice.is_file():raise RuntimeError('Missing copyright: '+package)
    shutil.copy2(notice,licenses/(package.replace(':','_')+'.copyright'))
for component in ('ffmpeg','mpv','f3d','vtk','assimp'):
    tree=Path('/dependency-sources')/component
    dest=licenses/component;dest.mkdir()
    for p in tree.iterdir():
        if p.is_file() and (p.name.startswith(('COPYING','LICENSE','Copyright'))):shutil.copy2(p,dest/p.name)
    for p in Path('/dependency-sources').glob(component+'.tar.*'):shutil.copy2(p,out/p.name)
# F3D includes font, colormap and embedded-library attributions beyond its BSD license.
f3d=Path('/dependency-sources/f3d')
shutil.copy2(f3d/'THIRD_PARTY_LICENSES.md',licenses/'f3d/THIRD_PARTY_LICENSES.md')
shutil.copy2(f3d/'resources/colormaps/licenses.md',licenses/'f3d/colormap-licenses.md')
# VTK embeds permissive dependencies; retain their notices as well.
for p in (Path('/dependency-sources/vtk/ThirdParty')).rglob('*'):
    if p.is_file() and p.name.lower().startswith(('copyright','copying','license')):
        dest=licenses/'vtk-third-party'/p.relative_to('/dependency-sources/vtk/ThirdParty');dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
for p in Path('/dependency-sources/assimp/contrib').rglob('*'):
    if p.is_file() and p.name.lower().startswith(('copyright','copying','license')):
        dest=licenses/'assimp-third-party'/p.relative_to('/dependency-sources/assimp/contrib');dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
subprocess.run(['apt-get','update'],check=True)
ubuntu=Path('/dependency-archive-cache') if Path('/dependency-archive-cache').is_dir() else out/'ubuntu'
ubuntu.mkdir(exist_ok=True)
for item in sorted(sources):
    result=subprocess.run(['apt-get','source','--download-only',item],cwd=ubuntu)
    if result.returncode:
        # Superseded base-image packages remain available in Ubuntu's source archive.
        name, version=item.split('=',1)
        query=urlencode({'ws.op':'getPublishedSources','source_name':name,'version':version,'exact_match':'true'})
        with urlopen('https://api.launchpad.net/1.0/ubuntu/+archive/primary?'+query) as response:
            entries=json.load(response)['entries']
        if not entries:raise RuntimeError('Exact Ubuntu source unavailable: '+item)
        with urlopen(entries[0]['self_link']+'?ws.op=sourceFileUrls') as response:urls=json.load(response)
        for url in urls:
            target=ubuntu/Path(urlparse(url).path).name
            subprocess.run(['curl','--fail','--location','--retry','3',url,'--output',str(target)],check=True)
        dsc=next(ubuntu/Path(urlparse(url).path).name for url in urls if url.endswith('.dsc'))
        checking=False
        for line in dsc.read_text().splitlines():
            if line=='Checksums-Sha256:':checking=True;continue
            if checking and not line.startswith(' '):checking=False
            if checking:
                digest,size,filename=line.split()
                with (ubuntu/filename).open('rb') as stream:actual=hashlib.file_digest(stream,'sha256').hexdigest()
                if actual!=digest:raise RuntimeError('Ubuntu source checksum mismatch: '+filename)
if ubuntu!=out/'ubuntu':shutil.copytree(ubuntu,out/'ubuntu')
(bundle/'ubuntu-packages.txt').write_text('\n'.join(sorted(packages))+'\n')
(out/'ubuntu-sources.txt').write_text('\n'.join(sorted(sources))+'\n')
# Include this application's exact source and packaging instructions, without
# credentials, build outputs, workstation logs or unrelated project assets.
shutil.copytree('/src',out/'asset-preview',ignore=shutil.ignore_patterns('.git','build','evidence','release','__pycache__','*.pyc'))
