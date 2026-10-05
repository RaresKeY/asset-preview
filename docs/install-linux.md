# Linux installation

Downloads: [release 0.0.2](https://github.com/RaresKeY/asset-preview/releases/tag/0.0.2).
The repository, release and GHCR package are private; download while signed in
with access, or use `gh release download 0.0.2 -R RaresKeY/asset-preview`.
Verify downloads with `sha256sum -c SHA256SUMS` in the download directory.

All formats are Linux x86_64. AppImage, portable and native packages require
glibc 2.39+, X11/XWayland and installed OpenGL/EGL drivers and the Vulkan loader (`libvulkan.so.1`). They include private
Qt, F3D, media and Python libraries; compiler tools are unnecessary. Flatpak uses
the Freedesktop 25.08 runtime instead of the host's glibc and graphics libraries.
MIT applies to the application; dependency notices and corresponding sources
are included/provided with the release. TIFF image previews require conversion
to PNG because the GPL JBIG-linked Qt TIFF plugin is excluded.

## AppImage

```sh
chmod +x asset-preview-0.0.2-linux-x86_64.AppImage
./asset-preview-0.0.2-linux-x86_64.AppImage
./asset-preview-0.0.2-linux-x86_64.AppImage add /absolute/project/chair.glb --id chair
```

If FUSE mounting is unavailable, use `--appimage-extract-and-run` before the
application arguments. The AppImage contains the same CLI as the portable bundle. On first launch it
caches its payload under `~/.cache/asset-preview` so the persistent preview server
keeps access to its libraries after a CLI invocation unmounts the AppImage.
For persistent desktop/fish/skill integration, extract with `--appimage-extract`
and use the extracted bundle's installer; keep that directory in place.
The AppImage runtime and its linked-library notices/rebuild sources accompany
this release's corresponding-source archive.

## Portable archive

```sh
tar -xzf asset-preview-0.0.2-linux-x86_64.tar.gz
./asset-preview/asset-preview
./asset-preview/asset-preview add /absolute/project/chair.glb --id chair
./asset-preview/asset-preview --python ./asset-preview/tools/install.py
```

The last command optionally installs user desktop/fish/skill integration. It
refuses unrelated existing entries. Keep the extracted directory in place.

## Native packages

These packages install the bundle under `/opt/asset-preview`, the CLI as
`/usr/bin/asset-preview`, and standard desktop/icon/AppStream/license files.
They use private dependency libraries rather than replacing distribution Qt/VTK.
Run `asset-preview` afterward. Project generators execute on the host.

| Distribution | Install | Remove |
| --- | --- | --- |
| Ubuntu 24.04+ / Debian 13+ | `sudo apt install ./asset-preview_0.0.2_amd64.deb` | `sudo apt remove asset-preview` |
| Fedora 42+ | `sudo dnf install ./asset-preview-0.0.2-1.x86_64.rpm` | `sudo dnf remove asset-preview` |
| Current Arch / CachyOS | `sudo pacman -U ./asset-preview-0.0.2-1-x86_64.pkg.tar.zst` | `sudo pacman -R asset-preview` |

Packages are unsigned local download packages, verified through release
checksums. No apt/dnf repository or public AUR listing is configured. Updates
use a newer downloaded package and the same install command. The committed
`packaging/native/PKGBUILD` is a template; the release's `PKGBUILD` has its
portable archive SHA256 filled in for `makepkg` rebuilds.

## Flatpak

```sh
flatpak install --user ./asset-preview-0.0.2-linux-x86_64.flatpak
flatpak run io.github.RaresKeY.AssetPreview
```

Flatpak may offer to install Freedesktop 25.08 from Flathub. The single-file app
bundle is distributed through GitHub Releases; it is not a Flathub submission
or an automatic-update remote. Install the next release's bundle to update.

Install the host command bridge for agents:

```sh
tar -xzf asset-preview-0.0.2-flatpak-client.tar.gz
python3 asset-preview-flatpak/tools/install-client.py
~/.local/bin/asset-preview-flatpak add /absolute/project/chair.glb --id chair
~/.local/bin/asset-preview-flatpak status --json
~/.local/bin/asset-preview-flatpak stop
```

The bridge uses host Python 3, starts the Flatpak's foreground service once,
and communicates through its private same-user socket at
`$XDG_RUNTIME_DIR/asset-preview-flatpak/preview.sock`. State is under
`~/.var/app/io.github.RaresKeY.AssetPreview/data/asset-preview`. Keep the bridge's
extracted directory in place. Flatpak uses separate state/socket defaults from
native installations so switching installations does not silently reuse another
service. The supported display backend is X11/XWayland.

This developer tool has broad host/project filesystem access and permission to
run **explicitly registered generators on the host** through `flatpak-spawn`.
It is not a tightly confined read-only viewer. Host generator tools need to be
installed on the host; the app does not install them. Host Python 3 also runs the generator supervisor. Flatpak's watch-bus helper
cancels that supervisor, which sends TERM/KILL to the generator's own child
process group when the viewer's generator is stopped.
Read-only preview registration needs no generator. The same `--watch`, `--cwd`
and `--exec` syntax works through `asset-preview-flatpak`.

## Container

`ghcr.io/rareskey/asset-preview:0.0.2`; private pulls require `read:packages`.
See [container requirements](../packaging/release-notes.md). GUI display/GPU
access, user mapping and project mounts are supplied at launch. The container
is an environment channel; release artifacts above are the desktop installers.

## Build packaging

`VERSION` controls the release version. `packaging/Containerfile` builds the
portable payload, `.run`, notices and corresponding sources. Its `artifacts`
stage exports the release directory. `packaging/extra-formats.py RELEASE_DIR
--formats appimage deb rpm` runs in the packaging-tools image; its `flatpak`
format runs on a host with Flatpak and the Freedesktop SDK/runtime installed.
`packaging/build-arch.sh RELEASE_DIR` runs in an Arch base-devel container and
uses makepkg. Native templates, Flatpak manifest and desktop metadata are under
`packaging/`. GitHub Actions supports both a fresh build and publishing a locally
validated draft's artifacts through its scoped package token.
