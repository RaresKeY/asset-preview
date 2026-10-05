# Preview lifecycle and rendering

Source ownership: `src/viewer.{h,cpp}`, `src/model_view.{h,cpp}`, preview/card/window paths in `src/service.cpp`,
`examples/generate_cube.py`, `tests/integration.py`. Renderer decisions and dependency
provenance are in [vendored/rendering.md](../vendored/rendering.md).

Single view activates exactly one entry; grid activates a page of at most four.
Only those entries own decoded content, GPU engines and running generators.
Hidden/minimized/closed windows activate none. Unchanged visible entries are retained
when unrelated registrations change. Navigation destroys prior off-page views; camera
state persists during reload, but not across unloading.

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

Views repaint only after file changes, input, settings, exposure or resize. No
continuous animation, idle render timer or file polling. QFileSystemWatcher observes
visible files and nearest existing parent directories, supporting file replacement
and later-created directories. Nanosecond mtime/size/inode fingerprints filter unrelated
directory notifications. A 220 ms single-shot debounce coalesces saves. Failed loads
keep prior content and retry three times, then wait for another notification.
Valid intermediate output saves refresh during a running generator as well;
the status remains Building until the generator exits.

The watcher derives glTF/GLB external resource URIs and ordinary OBJ/MTL texture
dependencies, plus explicit input paths/material maps. Complex MTL files can require
caller-supplied dependency paths. New files are discovered without recursive scans.
Registration/output files have a 256 MiB cap; only four views can be active. Model
metrics report actual renderer and per-view load/paint counts; images report dimensions
and decoded residency. These counters are diagnostic, not a perceptual acceptance gate.

Verification: functional tests cover watcher/lifecycle invariants. The Gamescope
headless hardware lane adds real F3D model/MTL/material-map refresh, hardware renderer
identity, idle paints and captured appearance. [Evidence](../evidence/verification.md)
records actual verification and remaining limits.
