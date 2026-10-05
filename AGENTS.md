# Asset Preview

Read `specs/_readme.md` and the owning spec before changes; update contracts with code.
Work in this checkout. Native C++/Qt development uses the host's existing CMake,
compiler, Qt 6 and libf3d. The Python CLI uses only the standard library.
Do not install dependencies automatically or create PRs unless requested.
Use Gamescope's headless backend for automated visual runs and verify the
actual OpenGL renderer. User-requested visible launches are allowed.
Only visible previews may own decoded images, F3D engines or running generators.
Static/paused views have no perpetual rendering/polling timer. Playing videos
render only when libmpv requests frames; unload players off-page/hidden. Preserve the socket's same-user boundary,
bounded protocol/logs and exact child-process-group ownership.
Keep `icon.svg` at the root. This is a tool, not a game; `play.sh` exists for
the workstation's Playpad launcher integration. The project is MIT licensed; preserve dependency notices and licenses.
