Linux x86_64 desktop distribution release, including the MIT application license.

- AppImage: `asset-preview-0.0.2-linux-x86_64.AppImage`.
- Portable: `asset-preview-0.0.2-linux-x86_64.tar.gz`; `.run` remains available.
- Native installers: `.deb` (Ubuntu 24.04+/Debian 13+), `.rpm` (Fedora 42+),
  `.pkg.tar.zst` (current Arch/CachyOS), plus the verified Arch `PKGBUILD`.
- Flatpak: `asset-preview-0.0.2-linux-x86_64.flatpak` and host CLI bridge archive.
- Private container: `ghcr.io/rareskey/asset-preview:0.0.2`; `latest` matches it.
- `SHA256SUMS` covers every download. The corresponding-source archive includes
  exact Ubuntu/upstream dependency sources and the AppImage runtime work sources.

[Installation instructions](https://github.com/RaresKeY/asset-preview/blob/main/docs/install-linux.md).
AppImage/portable/native require glibc 2.39+, X11/XWayland and host OpenGL/EGL
drivers. Flatpak uses Freedesktop 25.08 and has broad filesystem/host-generator
permissions appropriate to this trusted developer tool. The host CLI bridge
requires Python 3. Containers need explicit display/GPU/project mounts; their
generators execute inside the container. GPU drivers are supplied by the host
(or Flatpak's driver extensions), not copied from the workstation.

Application: MIT. Bundled dependencies retain their licenses. Qt/media libraries
remain dynamically replaceable. FFmpeg is built without GPL/nonfree components,
mpv uses LGPL mode without scripting, and the GPL JBIG-linked TIFF image plugin
is excluded. TIFF images should be converted to PNG. Codec availability differs
from full system mpv builds; codec patent questions are separate from copyright.
No public package store or automatic-update distribution repository is configured.
