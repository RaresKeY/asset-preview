# Preview lifecycle and rendering

Source ownership: `src/viewer.{h,cpp}`, `src/model_view.{h,cpp}`, `src/video_view.{h,cpp}`, preview/card/window paths in `src/service.cpp`,
`examples/generate_cube.py`, `tests/integration.py`, `tools/ui_scenes.py`. Renderer decisions and dependency
provenance are in [vendored/rendering.md](../vendored/rendering.md).

Single view activates exactly one entry; grids have 2, 3 or 4 columns/rows,
activating at most 4 (default), 9 or 16 entries on the selected page.
Only those entries own decoded content, GPU engines and running generators.
Hidden/minimized/closed windows activate none. Unchanged visible entries are retained
when unrelated registrations change. Navigation destroys prior off-page views; camera
state persists during reload, but not across unloading.

Cards allocate the full surface to content, with native sibling overlays for
title/fit/options/remove and status. The single global toolbar owns navigation,
grid size and compact mode. Compact reduces margins/gaps, puts fit/remove in the
options menu and hides steady Live status; waiting/errors/building stay visible.
Elided titles retain full title/path tooltips. Only overlays intercept pointer
input; central content remains available for orbit/pan. Captures composite their
small native overlays after model GPU readback, with no recurring copy loop.

The core links Qt Widgets/Network and the packaged jemalloc TLS shim with
libstdc++/libc allocation taking precedence. A local shared adapter loads F3D/VTK/OpenGL
only on the first visible 3D preview, then remains resident for global-registration
safety. Hiding the final view releases all engines/images and trims glibc free heap
pages; shared-library/driver residency does not return to the fresh-process baseline.
Images use QImageReader with a 4096-edge decode target and 64 MiB allocation limit,
QPainter fit/zoom/pan and checker/dark/light/pixel options. Models and material samples
use external-context F3D native/Assimp readers. Material sample geometry has normals
and explicitly triangulated UV faces. The view is a native QOpenGLWindow embedded
inside the main window, with explicit color/depth/blend initialization on each paint.
There is no framebuffer copy loop; only requested captures read GPU frames back.
Baked color/normal/ORM maps apply to sphere/cube/plane geometry. F3D does not
evaluate native Godot or Material Maker source. Models load on the OpenGL GUI thread;
arbitrary-scene memory and import latency are not bounded by the file-size cap.

Turntable orbit defaults to Y-up / XZ floor and a locked horizon. Yaw rotates
around world up; pitch clamps to ±88.2 degrees, preventing pole flips. Z-up / XY
floor and unlocked orbit are per-view options. Fit, reload and display settings
respect horizon lock. Default studio lighting uses F3D embedded HDRI ambient,
normal scene lighting and tone mapping; light-kit mode omits HDRI. F3D installs
its five-light VTK kit when a scene has no enabled authored lights. Intensity
remains per preview. No custom renderer, animated lights or raytracing is added.

Materials/textures default on. Textures-off clears color/normal/ORM/emissive/matcap
textures while retaining scalar material properties; materials-off uses opaque
neutral gray, roughness .8, metallic 0 and no emissive output or textures. F3D
overrides mutate imported actor properties, so mode changes create a replacement
engine and reload the scene to restore authored shading, preserving the camera
and last-good behavior. Authored vertex-color neutralization is not verified.

Static and paused views repaint only after file changes, input, settings, exposure
or resize. Playing videos additionally repaint on libmpv frame notifications.
No idle render timer or file polling. QFileSystemWatcher observes
visible files and nearest existing parent directories, supporting file replacement
and later-created directories. Nanosecond mtime/size/inode fingerprints filter unrelated
directory notifications. A 220 ms single-shot debounce coalesces saves. Failed loads
keep prior content and retry three times, then wait for another notification.
Valid intermediate output saves refresh during a running generator as well;
the status remains Building until the generator exits.

The watcher derives glTF/GLB external resource URIs and ordinary OBJ/MTL texture
dependencies, plus explicit input paths/material maps. Complex MTL files can require
caller-supplied dependency paths. New files are discovered without recursive scans.
Image/model/material files have a 256 MiB cap; streaming videos are exempt.
Only current-page views can be active,
with the explicitly selected grid limit no greater than sixteen. Model
metrics report actual renderer and per-view load/paint counts; images report dimensions
and decoded residency. These counters are diagnostic, not a perceptual acceptance gate.

Verification: functional tests cover watcher/lifecycle invariants. The Gamescope
headless hardware lane adds real F3D model/MTL/material-map refresh, hardware renderer
identity, idle paints and captured appearance. [Evidence](../evidence/verification.md)
records actual verification and remaining limits.
Camera tests measure horizon roll through native input and pole limits, reload
retention and Z-up. Native overlay clicks open/toggle the real popup. Functional
checks verify grid paging, compact busy/error visibility and restart persistence.

## Video backend

`asset-preview-mpv.so` is a lazy module linked to installed libmpv and Qt OpenGL.
Image/background-only sessions do not map libmpv. Visible videos own independent
mpv cores and native embedded QOpenGLWindows, with fitted aspect ratio. The module
remains resident after use, while every off-page/hidden player, decoder and render
context is destroyed. No subprocess player, CPU frame-copy loop or video polling.

Playback defaults to unpaused, muted, looping; controls support pause/mute/loop,
absolute seek, five-second arrow seeks and Home restart. Space/double click toggles
pause on the focused surface. Global Alt navigation retains precedence. The
normal card fit action becomes pause/play; compact keeps playback options in ⋯.
Finite source-save generators can publish videos through the existing watcher.

The render API uses the owning Qt OpenGL context, supplies the X11 display for
interop, requests `hwdec=auto-safe` and permits codec/driver software fallback.
GPU renderer and decoder identity are reported separately. Frame/event callbacks
coalesce onto the GUI thread without calling mpv APIs inside callbacks. Core
commands/property writes use asynchronous APIs; event handling observes properties
without synchronous core reads. Advanced render control is disabled, and target
time blocking is disabled to avoid sleeping on the GUI thread. Static/paused views
have no continuing frame activity; active videos necessarily consume decode/render
resources. Decoder memory is not bounded by the demux queue limit.

Reload opens a silent candidate asynchronously, preserving the current player
until a decoded video frame can replace it. Successful swaps restart at time zero
and retain pause/loop/mute preferences. Unsupported/corrupt outputs retain the last
good player, with bounded retries. A 15-second candidate deadline prevents an
indefinite load. Load/revision counts advance only after successful publication.
Candidates and current render contexts are freed before their corresponding cores,
with the original OpenGL context current. Timers/callbacks are scoped to the view.

Local demux buffering is capped at 8 MiB with no backward queue/cache. Muted
playback disables the audio track; unmuting selects it again. mpv user
configs/scripts, online URL helpers, external media references, automatic audio
sidecars and subtitles are disabled. Common video extensions and animated GIF
route automatically; explicit `kind=video` handles other installed-codec formats.
