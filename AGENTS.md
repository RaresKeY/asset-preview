# Asset Preview

Read `specs/_readme.md` and the owning spec before changes; update contracts with code.
The user's preferred home view is Overview, all projects, grouped by project.
Esc returns there from other views (exit fullscreen first). Agent sends preserve
the user's current layout; use the preferred view when explicitly choosing one.
Work in this checkout. Native C++/Qt development uses the host's existing CMake,
compiler, Qt 6 and libf3d. The Python CLI uses only the standard library.
Do not install dependencies automatically or create PRs unless requested.
Use Gamescope's headless backend for automated visual runs and verify the
actual OpenGL renderer. User-requested visible launches are allowed.
Only visible ordinary previews may own F3D engines, players or running generators.
The window-owned thumbnail queue may run two low-priority raster decodes plus
one finite content thumbnail child (model/material, file or video). Visible images lead;
nearby and other open assets warm after a quiet period while the window is
visible and ordinary previews are ready. Thumbnail work never activates an
ordinary preview, media player or asset generator; hiding cancels publication.
Decoded images/waveforms and thumbnails may survive unloading only in the bounded
service cache in specs/cache-overview.md; hidden windows clear RAM retention.
Static/paused views have no perpetual rendering/polling timer. Only visible, exposed, playing 3D animations may schedule animation frames. Playing audio updates controls from libmpv property events. Playing videos
render only when libmpv requests frames; unload all media players off-page/hidden. Preserve the socket's same-user boundary,
bounded protocol/logs and exact child-process-group ownership.
Keep `icon.svg` at the root. This is a tool, not a game; `play.sh` exists for
the workstation's Playpad launcher integration. The project is MIT licensed; preserve dependency notices and licenses.
