# Runtime dependency notices

This project uses installed dynamic libraries and does not vendor their source.
No license is assigned to this project's own source by this file.

| Dependency | Upstream license/source |
|---|---|
| Qt 6 Core, Gui, Widgets, Network, OpenGL | [Qt licensing](https://www.qt.io/licensing/open-source-lgpl-obligations), LGPL-3.0 / GPL / commercial options depending on distribution |
| F3D / libf3d | [F3D license](https://github.com/f3d-app/f3d/blob/v3.5.0/LICENSE), BSD-3-Clause |
| VTK (through F3D) | [VTK copyright](https://gitlab.kitware.com/vtk/vtk/-/blob/master/Copyright.txt), BSD-style |
| Assimp (F3D reader) | [Assimp license](https://github.com/assimp/assimp/blob/master/LICENSE), BSD-3-Clause |
| jemalloc (installed F3D dependency, static TLS compatibility) | [jemalloc license](https://github.com/jemalloc/jemalloc/blob/dev/COPYING), BSD-2-Clause |
| libmpv (lazy video backend) | [mpv copyright](https://github.com/mpv-player/mpv/blob/v0.41.0/Copyright), GPL-2.0-or-later by default; optional LGPL build depends on the packaged configuration |
| FFmpeg and transitive mpv codec/render dependencies | Installed distribution package notices govern the exact build; [FFmpeg licensing](https://ffmpeg.org/legal.html) |

Python's standard library runs the command client and tools; CMake/compiler,
Gamescope and fish are build, verification or desktop tools. Distribution beyond
this local system requires reviewing the exact packaged dependency notices.
