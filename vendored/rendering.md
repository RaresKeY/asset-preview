# Rendering and desktop conventions

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
CPU framebuffer readback is used only for a requested capture.
The verified desktop path is `xcb` (XWayland on KDE Wayland). Native Wayland
embedding failed the current hardware check, so the launcher uses `xcb` unless
the caller explicitly overrides it. This setting is local to the daemon.
Reader plugins are restricted to native and Assimp. This reuses mature scene loading while
retaining control of activation and scheduling. Qt Quick 3D's runtime loader is a
viable alternative for OBJ/glTF, but would add a QML scene framework to this small
Widgets app. A custom Assimp/OpenGL renderer would require owning material and scene
semantics. PySide6 would reuse the same Qt/renderer work while adding a resident
Python runtime; no comparative PySide6 memory benchmark is claimed.

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
no animation, high-quality raytracing, idle polling or continuous frames are enabled.
After first use the backend module remains resident because VTK/driver global
registrations are unsafe to unload opportunistically. Hidden views release their
engines/images; glibc's free heap pages are trimmed when the last preview is hidden.
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
