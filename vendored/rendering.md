# Rendering and desktop conventions

An optional static-model inspection adapter uses installed VTK 9.7+ directly,
through public importer/hierarchy, external-window, picking and clipping APIs.
It is a separate lazy module and does not change ordinary F3D rendering. The
current distro VTK CMake export references an unavailable header-only FastFloat
development target, so CMake resolves installed matching public headers/shared
libraries without installing anything or fabricating a dependency target.
The adapter initializes VTK's OpenGL module explicitly. It uses authored lights
or LightKit, with different shading from F3D's embedded HDRI. See
[inspection contracts](../specs/inspection.md) for identity, resources and limits.

Current foundation: native C++/Qt Widgets with embedded libf3d. The CLI is a short
standard-library Python process; the background service and GUI share one native
process. The F3D/OpenGL adapter is a separately linked shared module, loaded on the
first visible 3D preview. A fresh background/image-only process does not load F3D/VTK.
Use existing packages, no copied renderer or hand-written PBR stack.
The current package's dependency graph requires jemalloc's static TLS to be mapped
at process startup. CMake links that dependency after libstdc++ and libc; F3D/VTK remains
lazy and the application's allocator remains glibc. This is a package compatibility
requirement, not an allocator migration.

F3D supplies scene import, materials, camera and rendering behind an external
OpenGL context. The owning Qt window supplies that context. The view is a
QOpenGLWindow child inside a QWidget container. This avoids the framebuffer
and depth-state corruption observed with the installed F3D/Qt QOpenGLWidget path.
Paint explicitly restores the external color/depth/blend contract. There are no
separate desktop windows for individual previews and no display readback/copy loop;
CPU framebuffer readback is used for requested captures and a finite departure
thumbnail before a visible preview unloads; no background renderer is retained.
The verified desktop path is `xcb` (XWayland on KDE Wayland). Native Wayland
embedding failed the current hardware check, so the launcher uses `xcb` unless
the caller explicitly overrides it. This setting is local to the daemon.
Built-in reader plugins are restricted to native and Assimp. This reuses mature scene loading while
retaining control of activation and scheduling. Qt Quick 3D's runtime loader is a
viable alternative for OBJ/glTF, but would add a QML scene framework to this small
Widgets app. A custom Assimp/OpenGL renderer would require owning material and scene
semantics. PySide6 would reuse the same Qt/renderer work while adding a resident
Python runtime; no comparative PySide6 memory benchmark is claimed.

The host also supports F3D 4.x through `src/f3d_compat.h`. On that API, an
optional reader in the existing lazy module uses the installed F3D plugin SDK
factory ABI and public `vtkF3DGLTFImporter`/VTK image-array APIs. It retains the
native importer, including armature shaders, and only hoists tuple-count/stride
invariants from unsigned-byte component fills. No importer/shader source is
copied. Packaged 3.5 builds and missing SDK prerequisites use the ordinary readers.
See [Models](../specs/models.md) and the current profiling evidence for the finite
parity/performance boundary. The installed 4.0/VTK 9.7.1 host graph additionally
requires `libcudart.so.13`; this workstation currently needs an explicit loader
path to an existing local runtime. Profiling records that path/hash without
installing packages or embedding a container-storage path into the application.

The workstation's F3D thumbnailer invokes `f3d --output` for a single generated
image. Thumbnail generation is useful for a file browser; it is not an interactive
camera or a preview-session manager. This app uses the library directly. Its
desktop entry follows the existing Playpad pattern: owned application name,
absolute executable/icon paths, no terminal, no startup spinner, no global shortcut
rewrite. Root `play.sh` provides Playpad eligibility. Fish uses two owned autoload
symlinks and read-only completion.

PBR conventions: sRGB base color; linear tangent normal and packed ORM; true ORM
roughness/metallic multipliers set to 1. Assimp's F3D texture importer enables
mipmaps and interpolation. F3D's renderer-specific overrides and its default
environment differ from Godot's procedural shaders and Material Maker graphs.
Use the owning engine's PNG capture for exact shader behavior.

Studio mode uses F3D's built-in HDRI ambient environment with tone mapping. F3D
3.5 adds VTK LightKit when there are no enabled scene lights, including with HDRI
active. The kit has key, fill, head and two back lights; this is a five-light kit,
not a custom three-point setup. Light-kit mode disables HDRI while keeping the
same normal light handling. Imported authored lights remain F3D-owned.

F3D texture optionals have different semantics: unset retains imported textures;
a present empty filesystem path explicitly clears one in 3.5. Texture/clay
display modes clear the applicable texture overrides. Their authored restoration
uses a replacement scene because clearing/resetting options alone does not undo
mutations to the imported actor's properties. Camera state is transferred.
The orbit controller uses F3D's public camera-state API to enforce world-up yaw
and bounded pitch; grid and HDRI up-direction follow the selected Y/Z up axis.

This design minimizes ongoing work rather than proving a global minimum memory
footprint. GPU drivers and the VTK dependency have a material baseline cost. Views
are created only for visible entries and destroyed on navigation/hide/minimize;
no high-quality raytracing or idle polling is enabled. Static/paused scenes do
not request continuous frames. Playing 3D clips use F3D's public animation/time
API with a Qt-owned 33 ms single-shot frame driver, stopped off-page/hidden or
paused. F3D owns node/morph/skin evaluation through the existing backend. See
[Animations](../specs/animations.md).
After first use the backend module remains resident because VTK/driver global
registrations are unsafe to unload opportunistically. Hidden views release their
engines/players; eligible pixels and waveform envelopes may enter the explicitly
bounded [preview cache](../specs/cache-overview.md), which clears RAM on hide.
glibc's free heap pages are trimmed when the last preview is hidden.
Retained libraries and GPU-driver caches keep RSS above the fresh-process baseline.
Read actual performance evidence before claiming resource usage.

Primary references, checked October 5, 2026:

- [libf3d external-context contract](https://f3d.app/docs/libf3d/CLASSES/)
- [F3D Qt example](https://github.com/f3d-app/f3d/blob/v3.5.0/examples/libf3d/cpp/qt6/main.cxx)
- [F3D external render window](https://github.com/f3d-app/f3d/blob/v3.5.0/vtkext/private/module/vtkF3DExternalRenderWindow.cxx)
- [F3D Assimp texture import](https://github.com/f3d-app/f3d/blob/v3.5.0/plugins/assimp/module/vtkF3DAssimpImporter.cxx)
- [F3D material options](https://f3d.app/docs/libf3d/OPTIONS/)
- [F3D light and texture configuration](https://github.com/f3d-app/f3d/blob/v3.5.0/vtkext/private/module/vtkF3DRenderer.cxx)
- [VTK LightKit](https://vtk.org/doc/nightly/html/classvtkLightKit.html)
- [Qt runtime asset loader](https://doc.qt.io/qt-6/qml-qtquick3d-assetutils-runtimeloader.html)
- [Qt embedded window ownership](https://doc.qt.io/qt-6/qwidget.html#createWindowContainer)

## Audio and video playback

Installed libmpv 0.41.0 (client API pkg-config version 2.5.0) supplies the audio/video
backend. It lives in a separate lazy module, with one core per visible video;
no external mpv process or Qt Multimedia stack is added. Qt Multimedia is installed
and viable, but libmpv's existing public render API fits the native OpenGL-window
adapter and provides decoder identity, bounded local demux buffering and explicit
configuration without inheriting global player scripts.

The OpenGL render API is preferred by mpv over embedding an externally owned
`wid`. Hardware decoding uses `hwdec=auto-safe`, the Qt GL context and the native
X11 display for interop. Software fallback is permitted. Frame requests and core
events are queued/coalesced separately so time-position events do not repaint
paused/static views or add frames to active playback. Writes/commands are async;
no core API is called from a callback. Advanced control is left disabled to avoid
promising the stronger threading contract on the GUI thread. Requested captures
render the current frame before reading the native back buffer.

Muted video previews disable the audio track as well as muting it; unmuting selects the
audio track again. This avoids decoding audio merely to discard it in the common
muted-preview case. Candidate reloads are always silent until accepted. Demux
queues have an 8 MiB forward limit and no backward queue; decoder/GPU surfaces
have separate costs. Hidden/off-page views free their complete players.

Primary references checked October 5, 2026:

- [mpv public render API and threading/lifecycle contracts](https://github.com/mpv-player/mpv/blob/v0.41.0/include/mpv/render.h)
- [mpv OpenGL integration and hardware interop](https://github.com/mpv-player/mpv/blob/v0.41.0/include/mpv/render_gl.h)
- [mpv client API and async commands/properties](https://github.com/mpv-player/mpv/blob/v0.41.0/include/mpv/client.h)
- [mpv hardware decoding and options](https://mpv.io/manual/stable/)

Standalone audio uses the same lazy libmpv module and local-only configuration,
with a QWidget track card and no video renderer. It retains the audio decoder
while muted so it can establish readiness, duration and progress. New audio
previews are paused and unmuted; silent candidates publish only after valid audio
decoder parameters arrive. No new dependency or subprocess player is introduced.
Its painted music visualization observes `af-metadata/preview-levels` from the
same core's metadata-only `astats` and `aspectralstats` filters. Channel RMS,
overall peak and spectral centroid provide measurements without changing PCM
samples. A bounded RMS history drives the simple green bars. Property events are the only update
source; paused/hidden views have no recurring visualization work. Both filters
are included in the packaged LGPL FFmpeg allowlist. Reference:
[FFmpeg analysis filters](https://ffmpeg.org/ffmpeg-filters.html#astats) and
[mpv filter metadata properties](https://mpv.io/manual/stable/#af-metadata).

The lazy media module also links the existing LGPL FFmpeg libavformat,
libavcodec and libavutil libraries directly for a finite, cancellable waveform
overview decode. No CLI executable or extra dependency is installed. It retains
only bounded stereo min/max bins, and the native seek slider paints them with a
position-driven vertical playhead. Lifetime, local-format limits and fallback
behavior are owned by [Audio](../specs/audio.md). API reference:
[FFmpeg audio decoding](https://ffmpeg.org/doxygen/7.1/decode_audio_8c-example.html).
