#!/usr/bin/env python3
"""Repackage the verified portable payload into desktop/native formats."""
import argparse, hashlib, json, shutil, subprocess, tarfile, tempfile, time
from pathlib import Path

APP_ID='io.github.RaresKeY.AssetPreview'
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('release',type=Path)
parser.add_argument('--formats',nargs='+',choices=['appimage','deb','rpm','flatpak'],default=['appimage','deb','rpm','flatpak'])
args=parser.parse_args();release=args.release.resolve();version=(ROOT/'VERSION').read_text().strip()
archive=release/f'asset-preview-{version}-linux-x86_64.tar.gz'
def run(*command):subprocess.run([str(c) for c in command],check=True)
def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def copy_desktop(bundle,root):
    for relative,source in [(f'usr/share/applications/{APP_ID}.desktop',bundle/'desktop'/f'{APP_ID}.desktop'),
        (f'usr/share/metainfo/{APP_ID}.metainfo.xml',bundle/'desktop'/f'{APP_ID}.metainfo.xml'),
        (f'usr/share/icons/hicolor/scalable/apps/{APP_ID}.svg',bundle/'icon.svg'),
        ('usr/share/licenses/asset-preview/LICENSE',bundle/'LICENSE'),
        ('usr/share/doc/asset-preview/THIRD_PARTY_NOTICES.md',bundle/'THIRD_PARTY_NOTICES.md')]:
        p=root/relative;p.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,p)

with tempfile.TemporaryDirectory(prefix='asset-preview-formats-') as temporary:
    work=Path(temporary)
    with tarfile.open(archive) as tar:tar.extractall(work,filter='data')
    bundle=work/'asset-preview'
    assert (bundle/'VERSION').read_text().strip()==version
    payload=work/'payload';(payload/'opt').mkdir(parents=True)
    shutil.copytree(bundle,payload/'opt/asset-preview')
    (payload/'usr/bin').mkdir(parents=True)
    (payload/'usr/bin/asset-preview').write_text('#!/bin/sh\nexec /opt/asset-preview/asset-preview "$@"\n')
    (payload/'usr/bin/asset-preview').chmod(0o755)
    copy_desktop(bundle,payload)
    if 'appimage' in args.formats:
        lock=json.loads((ROOT/'packaging/appimage/runtime.json').read_text())
        runtime=release.parent/'appimage-runtime'
        if not runtime.exists():run('curl','--fail','--location','--retry','3',lock['url'],'--output',runtime)
        assert digest(runtime)==lock['sha256'],'AppImage runtime checksum mismatch'
        appdir=work/'AppDir';appdir.mkdir()
        shutil.copytree(bundle,appdir/'asset-preview')
        (appdir/'AppRun').write_text(f'''#!/bin/sh
set -eu
umask 077
preview_appdir=${{APPDIR:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}}
# The CLI starts a persistent server. Retain its libraries after this AppImage
# invocation unmounts instead of leaving the server tied to a temporary mount.
preview_cache="${{XDG_CACHE_HOME:-$HOME/.cache}}/asset-preview/{version}-{digest(archive)[:16]}"
if [ ! -x "$preview_cache/asset-preview/asset-preview" ]; then
 mkdir -p -- "$(dirname -- "$preview_cache")"
 preview_tmp=$(mktemp -d "${{preview_cache}}.XXXXXX")
 trap 'rm -rf -- "$preview_tmp"' EXIT HUP INT TERM
 cp -a "$preview_appdir/asset-preview" "$preview_tmp/asset-preview"
 if [ ! -d "$preview_cache" ]; then mv -T -- "$preview_tmp" "$preview_cache"; fi
 rm -rf -- "$preview_tmp"
 trap - EXIT HUP INT TERM
fi
exec "$preview_cache/asset-preview/asset-preview" "$@"
''')
        (appdir/'AppRun').chmod(0o755)
        shutil.copy2(bundle/'desktop'/f'{APP_ID}.desktop',appdir/f'{APP_ID}.desktop')
        shutil.copy2(bundle/'icon.svg',appdir/f'{APP_ID}.svg')
        (appdir/'.DirIcon').symlink_to(f'{APP_ID}.svg')
        squash=work/'payload.squashfs'
        run('mksquashfs',appdir,squash,'-root-owned','-noappend','-comp','zstd','-processors','2')
        appimage=release/f'asset-preview-{version}-linux-x86_64.AppImage'
        with appimage.open('wb') as stream:
            for part in (runtime,squash):
                with part.open('rb') as source:shutil.copyfileobj(source,stream)
        appimage.chmod(0o755)
    if 'deb' in args.formats:
        control=payload/'DEBIAN';control.mkdir()
        size=sum(p.stat().st_size for p in payload.rglob('*') if p.is_file() and not p.is_symlink())//1024
        (control/'control').write_text(f'''Package: asset-preview
Version: {version}
Architecture: amd64
Maintainer: RaresKeY <158580472+RaresKeY@users.noreply.github.com>
Section: graphics
Priority: optional
Homepage: https://github.com/RaresKeY/asset-preview
Installed-Size: {size}
Depends: libc6 (>= 2.39), libgl1, libegl1, libopengl0, libvulkan1, fontconfig-config, fonts-dejavu-core
Description: Live asset previews for AI agent workflows
 Viewer and command client with private Qt/F3D/media libraries under /opt.
 Dependency licenses and corresponding sources accompany the release.
''')
        run('dpkg-deb','--root-owner-group','--build',payload,release/f'asset-preview_{version}_amd64.deb')
        shutil.rmtree(control)
    if 'rpm' in args.formats:
        top=work/'rpm';top.mkdir()
        spec=(ROOT/'packaging/native/asset-preview.spec').read_text().replace('@VERSION@',version).replace('@PAYLOAD@',str(payload))
        (top/'asset-preview.spec').write_text(spec)
        run('rpmbuild','-bb','--define',f'_topdir {top}',top/'asset-preview.spec')
        for p in (top/'RPMS').rglob('*.rpm'):shutil.copy2(p,release/p.name)
    if 'flatpak' in args.formats:
        manifest=json.loads((ROOT/'packaging/flatpak'/f'{APP_ID}.json').read_text())
        build=work/'flatpak-build';repo=work/'flatpak-repo'
        run('flatpak','build-init','--arch=x86_64',build,APP_ID,manifest['sdk'],manifest['runtime'],manifest['runtime-version'])
        files=build/'files';shutil.copytree(bundle,files/'asset-preview')
        (files/'bin').mkdir();shutil.copy2(ROOT/'packaging/flatpak/launcher.sh',files/'bin/asset-preview');(files/'bin/asset-preview').chmod(0o755)
        copy_desktop(bundle,files) # Relocate share/ from the native usr/ layout.
        shutil.move(files/'usr/share',files/'share');shutil.rmtree(files/'usr')
        run('flatpak','build-finish','--command=asset-preview',*manifest['finish-args'],build)
        run('flatpak','build-export','--arch=x86_64',repo,build,'stable')
        run('flatpak','build-bundle','--arch=x86_64','--runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo',repo,release/f'asset-preview-{version}-linux-x86_64.flatpak',APP_ID,'stable')
        bridge=work/'asset-preview-flatpak';(bridge/'bin').mkdir(parents=True);(bridge/'tools').mkdir()
        shutil.copy2(bundle/'bin/client.py',bridge/'bin/client.py')
        shutil.copy2(ROOT/'packaging/flatpak/client.py',bridge/'bin/asset-preview-flatpak');(bridge/'bin/asset-preview-flatpak').chmod(0o755)
        shutil.copy2(ROOT/'packaging/flatpak/install-client.py',bridge/'tools/install-client.py')
        for name in ('VERSION','LICENSE','THIRD_PARTY_NOTICES.md'):shutil.copy2(bundle/name,bridge/name)
        run('tar','-czf',release/f'asset-preview-{version}-flatpak-client.tar.gz','-C',work,'asset-preview-flatpak')
# Hash every public artifact; build scratch directories and OCI staging archives
# are kept outside this directory by callers.
(release/'SHA256SUMS').write_text(''.join(f'{digest(p)}  {p.name}\n' for p in sorted(release.iterdir()) if p.is_file() and p.name!='SHA256SUMS'))
