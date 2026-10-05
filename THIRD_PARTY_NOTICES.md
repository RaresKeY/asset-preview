# Runtime dependency notices

Source builds use installed dynamic libraries. Release 0.0.1 bundles dynamically
linked dependencies, their notices and corresponding source packages.
Asset Preview's own source is licensed under MIT; see [LICENSE](LICENSE).
Dependencies retain their individual licenses.

| Dependency | Upstream license/source |
|---|---|
| Qt 6 Core, Gui, Widgets, Network, OpenGL | [Qt licensing](https://www.qt.io/licensing/open-source-lgpl-obligations), LGPL-3.0 / GPL / commercial options depending on distribution |
| F3D / libf3d | [F3D license](https://github.com/f3d-app/f3d/blob/v3.5.0/LICENSE.md), BSD-3-Clause |
| VTK (through F3D) | [VTK copyright](https://gitlab.kitware.com/vtk/vtk/-/blob/v9.4.2/Copyright.txt), BSD-style |
| Assimp (F3D reader) | [Assimp license](https://github.com/assimp/assimp/blob/master/LICENSE), BSD-3-Clause |
| jemalloc (installed F3D dependency, static TLS compatibility) | [jemalloc license](https://github.com/jemalloc/jemalloc/blob/dev/COPYING), BSD-2-Clause |
| libmpv (lazy video backend) | [mpv copyright](https://github.com/mpv-player/mpv/blob/v0.41.0/Copyright), Release: LGPL-2.1-or-later (`gpl=false`); installed system builds may use GPL-2.0-or-later |
| FFmpeg and transitive mpv codec/render dependencies | Release: LGPL-2.1-or-later with GPL/nonfree components disabled; system configurations vary; [FFmpeg licensing](https://ffmpeg.org/legal.html) |

## Release 0.0.1

The release uses Qt 6 from Ubuntu 24.04 under LGPL-3.0, F3D 3.5.0
(BSD-3-Clause), VTK 9.4.2 (BSD-style), Assimp 6.0.5 (BSD-3-Clause),
libmpv 0.41.0 built with `-Dgpl=false` (LGPL-2.1-or-later), and
FFmpeg 7.1.5 built with GPL/nonfree components disabled (LGPL-2.1-or-later).
Python 3.12 uses the PSF license and its included historical notices.
Qt image plugins and transitive libraries retain their individual licenses.

Each executable bundle contains `licenses/`, `dependency-manifest.json` and
`ubuntu-packages.txt`. The corresponding-source release archive includes original
upstream archives, exact Ubuntu source packages (including packaging patches),
application source and packaging scripts. The container also carries this archive
under `/opt/sources/`. VTK third-party notices and F3D embedded font/resource attributions are retained. System base-image
packages retain their notices under `/usr/share/doc`.

LGPL libraries are dynamically linked and can be replaced in the extracted
bundle's `lib/` and `qt/plugins/` directories. Dependency licenses permit the
reverse engineering needed to debug modifications to those libraries; no project
restriction is added here. Source redistribution and modification rights granted
by dependency licenses apply to their corresponding components. The application MIT license does not
replace these dependency licenses.

GPU drivers, the host glibc, optional GPL Python readline/GDBM modules, and the
workstation's GPL mpv/FFmpeg builds are not included in the standalone bundle.
Container system packages are separate distribution components. Rebuilds must
retain notices/sources and recheck their dependency closure; installing additional
codecs or replacing LGPL media libraries with GPL builds changes the licensing
assessment. Codec patent obligations are separate from copyright licensing.

Python's standard library runs the command client and tools; CMake/compiler,
Gamescope and fish are build, verification or desktop tools. Distribution beyond
this local system requires reviewing the exact packaged dependency notices.

The release omits Qt’s optional TIFF plugin because Ubuntu’s TIFF library links
GPL JBIG code. Convert TIFF image previews to PNG; this restriction does not
change the separately built VTK model readers. Minimal libmpv omits scripting.


## Desktop distribution formats

Release 0.0.2 includes the MIT application license in every payload. AppImage
adds AppImage/type2-runtime (MIT), libfuse 3.15 (LGPL-2.1), squashfuse (BSD-2-Clause),
musl and mimalloc (MIT), zstd (BSD-3-Clause option), and zlib (Zlib). Their notices
are under `licenses/appimage-runtime`; the corresponding-source archive carries
the checksum-pinned runtime binary, runtime work sources, libfuse patch/build
instructions and linked-component source/notice archives for rebuilding and
relinking the LGPL runtime component. See `packaging/appimage/runtime.json`.
The Freedesktop Flatpak runtime and graphics extensions are installed separately
and retain their own notices/licenses. Distribution package tools are build-only.
