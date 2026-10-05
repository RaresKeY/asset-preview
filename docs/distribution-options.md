# Linux distribution options

Assessment for Asset Preview: a Qt GUI with a local agent CLI/socket, live project
file watching, F3D model rendering, libmpv video and optional host generators.

| Format | User experience | Fit and complications |
| --- | --- | --- |
| AppImage | Download one executable; no root install | Best next desktop format. Bundle Qt/F3D/media libraries, add desktop integration. Build against the oldest supported glibc; changing the extension does not improve ABI compatibility. Some systems need FUSE or extraction. |
| Portable tar.gz / tar.zst | Extract a folder and run; optionally install a launcher | Simplest transparent bundle, easy to inspect or replace LGPL libraries. Needs desktop/PATH integration and an explicit update process. |
| Self-extracting .run | Download, chmod +x, execute | Current standalone release. No FUSE required; extracts privately into a versioned cache. Less conventional than AppImage and needs cache cleanup/update handling. |
| .deb / apt repository | Install and update with the distribution package manager | Good for Ubuntu/Debian. Separate builds per supported release; F3D/VTK versions may require a private dependency prefix. An apt repository adds signing and maintenance. |
| .rpm / dnf repository | Native Fedora/openSUSE installation and updates | Same tradeoffs as .deb; another distribution family to maintain. |
| Arch PKGBUILD / AUR | Build from source, or install a binary package | Convenient for Arch/CachyOS. Rolling dependencies need continued compatibility checks; AUR recipes are public, which conflicts with the current private distribution unless hosted privately. |
| Flatpak / Flathub or private remote | Desktop integration, updates, shared runtimes | Attractive for a viewer. This app needs deliberate filesystem/socket permissions and a host CLI bridge; arbitrary host generator execution complicates the sandbox. Better after designing that boundary. |
| Snap | Managed installation and automatic updates | Strict confinement needs filesystem/socket/generator integration. Classic confinement grants broader access and involves store review; snapd is another user requirement. |
| Nix flake/package | Reproducible dependencies and environment | Useful for developers already using Nix. Extra ecosystem knowledge and graphics integration make it a secondary channel. |
| OCI image / GHCR | Pull a versioned image | Current reproducible/container channel. GUI display authorization, GPU access, UID mapping and project mounts make it less convenient for casual desktop users. Generator tools must exist in the image. |
| Source archive / Git clone | Build with CMake and install the CLI | Best for contributors; requires compiler, Qt, recent F3D/VTK and libmpv. Slowest initial setup. |

Recommended order: keep the current `.run` and OCI image; add a portable archive
and AppImage for desktop sharing; add native packages when supported distro
versions are decided. Defer sandboxed formats until the host CLI and generators
have an explicit integration contract. `pipx`/PyPI alone is not a good installer
for this app's native GUI and graphics dependency graph.

References: [AppImage portability guidance](https://docs.appimage.org/reference/best-practices.html),
[Flatpak permissions](https://docs.flatpak.org/en/latest/sandbox-permissions.html),
[Snap confinement](https://snapcraft.io/docs/classic-confinement/),
[Debian packaging](https://www.debian.org/doc/manuals/maint-guide/),
[Arch package guidelines](https://wiki.archlinux.org/title/Arch_package_guidelines),
[Nix packaging](https://nixos.org/manual/nixpkgs/stable/).

## License

Asset Preview's own code uses [MIT](../LICENSE): permissive reuse, including
commercial use, with copyright and license notices retained. Distributed forks
are not required to publish their changes. Recipients may redistribute the app,
including publicly; a private repository controls access rather than imposing
confidentiality.

Dependencies retain their own licenses. MIT does not relicense Qt or the LGPL
media libraries, remove codec patent concerns, or replace dependency notice and
corresponding-source obligations. Qt LGPL libraries remain replaceable; see
[third-party notices](../THIRD_PARTY_NOTICES.md) and
[Qt LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations).
