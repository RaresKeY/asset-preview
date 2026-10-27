# Preview lifecycle and rendering

[Projects](projects.md) owns persistent agent grouping, chosen current previews,
the interactive Projects grid and Overview grouping/filtering.

Source ownership: `src/viewer.{h,cpp}`, `src/backend_loader.h`, `src/backend_abi.cpp`, `src/backend_abi_config.h.in`, `src/model_view.{h,cpp}`, `src/triangle_counts.h`, `src/dependency_metadata.h`, `src/video_view.{h,cpp}`, `src/video_controls.{h,cpp}`, `src/animation_controls.{h,cpp}`, `src/seek_slider.h`, `src/fullscreen_notice.{h,cpp}`, `src/audio_view.{h,cpp}`, `src/audio_visualizer.{h,cpp}`, `src/mpv_player.h`, preview/card/window paths in `src/service.cpp`,
`examples/generate_cube.py`, `tests/integration.py`, `tools/ui_scenes.py`. Renderer decisions and dependency
provenance are in [vendored/rendering.md](../vendored/rendering.md).

Single view activates exactly one entry; grids have 2, 3 or 4 columns/rows,
activating at most 4 (default), 9 or 16 entries on the selected page.
Left/Right and Alt+Left/Right use the toolbar's navigation path: one preview in
single mode or one page in grid mode, clamped at either end. These shortcuts
also work while a native 3D or video surface has focus. Boundary arrows leave
selection and focus unchanged, matching disabled toolbar buttons. Successful
navigation focuses the selected surface so video controls work without a click.
The video container forwards playback keys when Qt retains widget focus instead
of handing it to the native video window.
Only those entries own active decoded content, GPU engines and running generators.
The [bounded cache and all-open overview](cache-overview.md) may retain small image
pixels, waveform envelopes and thumbnails; overview activates no ordinary views.
Visible model/material tiles may use its single finite offscreen thumbnail child;
this child shares the ordinary importer and visual settings and exits after one
frame, with no retained engine, playback or asset generator.
Card removal detaches its published view/surface/overlay pointers before native
children are destroyed. Input targeting and video activity ignore inactive or
detached cards, including Qt's synchronous synthetic enter/leave/focus events
during close. This also applies to inspection backend replacement. Native input
tests click the close button while the pointer remains over missing, failed and
loaded videos in single and grid layouts, then verify the service stays alive.
Hidden/minimized/closed windows activate none. Unchanged visible entries are retained
when unrelated registrations change. Navigation destroys prior off-page views;
camera angle, focal point/pan, view angle and orthographic scale persist per
model/material across reloads, unloading, same-kind reconnections and restarts.
Models may declare `settings.front_axis` as `+x`, `-x`, `+y`, `-y`, `+z` or
`-z`, perpendicular to `up_axis` (default y). This source convention drives the
initial three-quarter angle, Reset and cardinal views; top uses the opposite
front as screen up. Omitting it preserves legacy camera angles and presets.
Changing the effective up/front frame discards stale persisted inspection cameras;
material/texture changes and same-frame reconnections preserve them. Front axis
is per-model and is not remembered as a preference for unrelated new previews.
Completed drags and scroll/fit/reset input save camera state without idle polling.
Image zoom and pan also persist per preview; pan uses viewport-relative offsets
so restored views behave consistently after resizing.

The Open dialog and local-file drops share the explicit `open` request with the
CLI. They reuse existing path registrations and select the last opened file;
errors appear in the window status bar. Drops over native preview surfaces bubble
to the window. Only local file URLs are accepted, directories and remote URLs are
ignored, and accepted drops use Copy so the source never moves or disappears.
Move-only sources are rejected. Native surface drops defer file opening until
the drop event completes, so selecting a file cannot destroy its active receiver.
Opening files preserves the chosen layout. Ordinary agent `add` remains independent
of focus and selection.
The toolbar's model-only Preview / Part mode / Cut mode selector uses the existing
static GLB/glTF inspection adapter. Parts and Cuts share a scene and visible cut
sliders; their session tool choice shows or hides component tools without
reloading geometry.
`presentation` reports the selected inspection mode and the selector's native
geometry, visibility and availability for input verification.

Hovering a visible card, overlay or native surface in the active window targets
that preview without clicking, reloading or rearranging the page. Menus,
shortcuts, playback keys and the selector follow that target; keyboard focus
also moves to its surface so a previously focused video cannot consume a model's
M shortcut. Cards have no selection tint or border change. Mouse entry/movement
drives targeting, with no polling timer. Outside the previews, the last active
preview remains the target for menus and keyboard use. Open popups/dialogs and
mouse drags freeze hover targeting; explicit clicks and keyboard/CLI selection
still work. Reflow beneath a stationary pointer does not override keyboard/CLI
selection; movement, wheel input or a click resumes mouse targeting. Accessible
card names identify the active preview.
Clicking a native preview surface transfers keyboard focus back from its
playback controls even if the pointer has stayed at the same position.
Ctrl+W closes it and Ctrl+Shift+T reopens the latest closed
preview with its position, display options and inspection state. Closed entries
retain configuration only; no decoded content or generator is kept running.
Each live card and overview tile has a top-right checkbox for a separate,
session-only close selection. Checkbox clicks and Ctrl+clicks on content toggle
one preview; Shift+click selects the inclusive registration-order range from
the last toggled preview, including off-page entries. Ctrl+Shift adds that range
to the checked set. Ordinary content clicks also set the range anchor; without
an anchor, the active preview supplies it. Hover,
ordinary content interactions, opening a tile and layout changes preserve the
checked set and continue to target the ordinary active preview independently.
In overview, Ctrl+Space toggles the keyboard target; live checkboxes use native
Tab/Space focus. The toolbar shows Close selected with its count when anything
is checked, and Close All otherwise. File > Close Selected Previews is always
available and disabled for an empty selection; File > Close All remains separate.
Narrow windows keep the selected-close action visible with a compact count.
Batch close validates all IDs, saves once before unloading, and reconciles once;
save failure preserves the checked set and every live view. Successful closure
prunes checks; reopening does not restore them. The configured close destination
skips every closed item. Reopen restores batch entries in reverse closing order
with their original relative positions, subject to the existing 32-entry history.
Closing selects the left neighbor by default (the right neighbor for the first
entry). Settings > After Closing a Preview can instead select the last viewed
surviving preview; without history it uses the neighbor rule. While the window
is visible, one inactive return target may be prepared through the
[bounded cache preloader](cache-overview.md); this creates no hidden view.

View > Camera Views supports front/back/right/left/top/bottom/isometric, with
1/3/7/9 and Ctrl+1/3/7 shortcuts including the numpad. Presets retain focal point
and distance. Exact top/bottom views survive reload; horizon-locked dragging
still clamps pitch when orbit resumes. F fits; Ctrl+Home resets the camera.

View > Compare With activates exactly two model/material previews side by side.
Sync camera links orbit, pan, zoom and camera presets in either direction, with
translation and distance scaled around each scene's square-fit reference center.
Projection and up-axis/horizon conventions follow the source camera. Sync can be
disabled for independent views. Comparison and sync preference survive restart;
closing a compared preview or choosing a normal layout ends comparison.

Cards allocate the full surface to content, with separate native overlays:
text-width title at top left, fit/reset/options/remove/selection checkbox at top right for 3D views, text-width status
below the title, and a triangle count at bottom left for loaded 3D views. The
single global toolbar owns navigation, grid size and Close all / Close selected. Live status, waiting/errors/building and enabled
triangle counts stay visible. Cards use 6-pixel insets and a 34-pixel title/control row. The main toolbar uses
32-pixel controls. Narrow cards move fit/reset/remove into the menu. Fit preserves angle and fits the bounds;
Reset restores the initial three-quarter angle, framing and zoom. Long title/status text elides within the card.
Elided titles retain full title/path tooltips. Only overlays intercept pointer
input; central content remains available for orbit/pan. Captures composite their
small native overlays after model GPU readback, with no recurring copy loop.

The core links Qt Widgets/Network and the packaged jemalloc TLS shim with
libstdc++/libc allocation taking precedence. A local shared adapter loads F3D/VTK/OpenGL
only on the first visible 3D preview, then remains resident for global-registration
safety. Hiding, closing all or removing the final active view releases its owned
resources and trims free glibc heap pages once after the complete release batch.
The optional F3D 4 native glTF byte-fill reader lives in that same lazy module;
its shared image-buffer views have exactly the imported scene's lifetime, as
described in [Models](models.md).
Failed registry saves retain live resources. Shared-library/driver residency
does not return to the fresh-process baseline.
Before invoking any F3D, media or inspection factory, the server checks a C-only
ABI fingerprint against its own compiled value. CMake derives it from the shared
view, cache and waveform contracts and compiler/Qt build settings, regenerating
it when those headers change. Missing or mismatched fingerprints report a backend
error asking for a completed build and restart; no C++ objects cross the boundary.
This prevents a running server from mixing its old callback/object layout with
new lazy modules after an in-place rebuild. Compatible modules stay resident.
`tools/test.sh backend-compatibility` checks every built adapter and injects
legacy/mismatched/null signatures with fatal factories into isolated servers,
verifying that rejection preserves daemon and image-preview operation.
The same lane also invokes a matching fixture through the real server, ensuring
its compiled fingerprint agrees with the generated contract and permits factories.
Images use QImageReader with a 4096-edge decode target and 64 MiB allocation limit,
QPainter fit/zoom/pan and checker/dark/light/pixel options. Models and material samples
use external-context F3D native/Assimp readers. Material sample geometry has normals
and explicitly triangulated UV faces. The view is a native QOpenGLWindow embedded
inside the main window, with explicit color/depth/blend initialization on each paint.
There is no framebuffer copy loop; requested captures and a finite departure
thumbnail may read GPU frames back before a visible view unloads.
Baked color/normal/ORM maps apply to sphere/cube/plane geometry. F3D does not
evaluate native Godot or Material Maker source. Models load on the OpenGL GUI thread;
arbitrary-scene memory and import latency are not bounded by the file-size cap.

Scroll zoom anchors images at the pointer, respecting the existing zoom limits.
Model/material scroll zoom preserves the camera's focal point and orientation,
independent of pointer position. Orthographic zoom changes parallel scale;
perspective zoom changes camera distance. Orbit dragging also preserves the
focal point, including after a pan. Explicit pan, fit and reset can change it.

Turntable orbit defaults to Y-up / XZ floor and a locked horizon. Yaw rotates
around world up; pitch clamps to ±88.2 degrees, preventing pole flips. Vertical
orbit dragging is inverted: dragging down raises camera elevation, in both locked
and unlocked modes. Z-up / XY
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

Triangle counts default visible and can be toggled per model/material via options
or the boolean `triangles` setting without reloading geometry. Window shortcuts
E toggles edges/lines and M toggles the triangle-count overlay for the targeted
model/material preview; holding a key does not repeatedly toggle it. The video
surface retains M for mute. The badge abbreviates thousands, millions and billions
with `K`, `M` and `B`, rounded to at most one decimal with no trailing zero;
rounding promotes to the next suffix when needed. Counts below 1,000 remain exact.
Display numbers follow the Qt locale, and hovering shows the exact count.
`metrics.triangles` retains the full integer. Counts derive from
GLB/glTF primitive metadata in the selected scene (including mesh instances),
streamed OBJ face sizes after triangulation, or the generated material sample.
Each visible model view retains one source-identity-keyed scalar count across
unchanged-source reloads. It is discarded with the view and stores no geometry.
Source fingerprints are checked before and after a scan; raced results are not
cached. OBJ token counting avoids temporary token arrays while preserving the
existing continuation, whitespace, comment, overflow and line-bound behavior.
This runs only after successful loads, preserves last-good counts on rejected
replacements and does not decode a second geometry copy or poll while idle.
GLB/glTF JSON is capped at 16 MiB; OBJ lines at 1 MiB. Other formats/unavailable
metadata display `Tris —` with an explanatory tooltip. Counts describe source
primitives, including degenerate strip/fan triangles, rather than GPU frame work.
Models expose nullable `metrics.triangles`; entry `overlays` reports ephemeral
name/control/status/count geometry and visibility for isolated UI checks.

Static and paused views repaint only after file changes, input, settings, exposure
or resize, plus one waveform-completion update for audio. Playing videos additionally repaint on libmpv frame notifications. Playing
3D animations request bounded frames only while exposed and playing; see
[Animations](animations.md).
No idle render timer or file polling. QFileSystemWatcher observes
visible files and nearest existing parent directories, supporting file replacement
and later-created directories. Device/inode/size/mode and nanosecond mtime/ctime
fingerprints filter unrelated
directory notifications. A 220 ms single-shot debounce coalesces saves. Failed loads
keep prior content and retry three times, then wait for another notification.
Successful loads invalidate older scheduled retries so a recovered source is
not reloaded again by an earlier failure's timer.
Valid intermediate output saves refresh during a running generator as well;
the status remains Building until the generator exits.

The watcher derives glTF/GLB external resource URIs and ordinary OBJ/MTL texture
dependencies, plus explicit input paths/material maps. Complex MTL files can require
caller-supplied dependency paths. New files are discovered without recursive scans.
Each active entry retains one bounded derived dependency manifest, invalidated
by its main source and referenced MTL fingerprints, including initially missing
MTLs. Deactivation clears it. GLB discovery reads the validated header and at
most 4 MiB of JSON without reading the binary chunk. Source/MTL discovery remains
best effort with 4 MiB read bounds, at most 256 derived paths and 4 MiB retained
path text. Explicit config watch/maps/review paths keep their existing bounds.

Successful visible publications retain before/after-verified output/dependency
fingerprints, including explicit watch paths that can cover undiscovered
imported resources, separately from observed watcher stamps. Ordinary generator exit
updates status without reimporting when that displayed publication is still
current. Failed, changing or asynchronous pending candidates cannot establish a
current publication. Final output changes and explicit Reload still refresh.
After a verified publication, observed output stamps advance to the exact
imported fingerprints. A pending watcher event for that same save then does not
reload it again. Explicit generator inputs retain their observed stamps until
normal source-change processing, so publishing output cannot consume a rebuild.
Reuse also requires complete dependency coverage in both load snapshots.
Bounded, fully parsed glTF/GLB manifests can establish it; truncated/invalid
metadata, path/text caps, unverified extension/URI forms and best-effort OBJ/MTL or other
model formats retain the normal refresh path. External image resources and
material maps must have a detected self-contained raster format. SVG, including
SVG content under a raster filename, and sidecar formats such as MHD do not
authorize reuse. Format checks retain only fingerprinted boolean results for
the current bounded image paths and clear on deactivation. They decode no pixels.
Audio/video reuse retains its disabled-external-reference boundary.
Image/model/material files have a 256 MiB cap; streaming audio/videos are exempt.
Only current-page views can be active,
with the explicitly selected grid limit no greater than sixteen. Model
metrics report actual renderer and per-view load/paint counts; images report dimensions
and decoded residency. These counters are diagnostic, not a perceptual acceptance gate.

Verification: functional tests cover watcher/lifecycle invariants. The Gamescope
headless hardware lane adds real F3D model/MTL/material-map refresh, hardware renderer
identity, idle paints and captured appearance. [Evidence](../evidence/verification.md)
records actual verification and remaining limits.
The isolated XTest input fixture uses X11 core input (`QT_XCB_NO_XI2=1`), since
its synthetic wheel buttons are dropped on this Qt/XInput2 path. Production
retains the normal device input path. Wheel tests assert actual zoom changes.
Camera tests measure horizon roll through native input and pole limits, reload
retention and Z-up. Native overlay clicks open/toggle the real popup. Functional
checks verify grid paging, live/busy/error visibility and restart persistence.
`tools/test.sh metadata` checks bounded triangle metadata parsing without OpenGL;
the hardware count test checks save refresh, last-good counts, overlay placement
and toggling without reloading or moving the camera.

README component screenshots use isolated Gamescope hardware sessions with KDE
Breeze Dark chrome and dark preview backgrounds, recording the historical 2×2
and 3×3 layouts. `docs/screenshots/capture-settings.json` records their current
source hashes, renderer, display settings and fitted camera states. The GPU's
inspection root is turned to retain the prior fan-facing presentation, leaving
its exported geometry and material buffer unchanged. Other current PC exports
and the authoritative chef/patron GLBs are loaded directly.

## Asset families

Read the focused spec for the asset being changed:

| Asset family | Spec | Runtime kind |
|---|---|---|
| Images and texture inspection | [Images](images.md) | `image` |
| Audio, music and sound effects | [Audio](audio.md) | `audio` |
| Video and rendered animation | [Video](video.md) | `video` |
| Exported meshes and ordinary model viewing | [Models](models.md) | `model` |
| Complete 3D scenes and assemblies | [Scenes](scenes.md) | `model` |
| Modular kits, components and assembled hosts | [Modular assets](modular.md) | `model` and proof images |
| Baked material samples | [Materials](materials.md) | `material` |
| Skeletal, node and morph animation | [Animations](animations.md) | `model` with discovered clips |
| Static component selection, cuts and review pins | [Inspection](inspection.md) | Optional model backend |

These are supported asset workflows, not new file extensions or separate daemon
kinds. Images, media and 3D exports can coexist in the same grid. Shared camera,
publication, native overlay and lifetime contracts apply across families.

## Component inspection

The optional single-model Inspect 3D workspace has its own lazy VTK adapter,
component identity, clipping, visibility and review contract in
[3D inspection](inspection.md). Ordinary model previews continue using F3D and
do not import inspection metadata or allocate a picker. Backend switches transfer
camera state and retain the existing generator connection.

## Shared window controls and captures

Native File, View, Go, Settings and Help menus use the desktop Qt style, theme
icons, standard shortcut labels and disabled actions where applicable. A native
toolbar retains navigation, selection, grid size controls; secondary
controls move to menus in small windows. View > Preview Options and card popups
share display controls. Settings > Theme selects Auto, Dark or Light for all
native app controls, saved independently of scene backgrounds. Auto follows the
platform palette; forced themes use Qt palette roles with the same native style.
Settings also owns independent image/audio/video/3D defaults for new previews.
Numeric dialogs accept light intensity 0–5 and roughness/metallic
0–1; sample scalar controls describe their no-ORM scope. Checker is image-only.

File and card popups expose Save Screenshot (Ctrl+Shift+S) and Copy Image / Frame
(Ctrl+Shift+C). All screenshot saves use the user's Pictures/Asset Preview with
unique timestamped PNG names. Save Screenshot and Copy Image use only decoded image pixels with
alpha, or the rendered frame for other kinds. Copy View Screenshot preserves
the visible framing/background. File also saves the whole interface and opens
the screenshot folder. Window captures composite native surfaces and overlays.
Ordinary clipboard copies create no file. The optional right Codex terminal and
frozen reference annotation editor are owned by [Visual references](references.md).

Large layouts admit at most two new cards per event-loop batch, yielding after
approximately 8 ms of construction work. RPC layout selection persists before
admission; clients waiting for a complete layout observe its expected active
count and each view's loading metrics. A pending callback recomputes current
visibility instead of retaining an old page. Project panes show a spinner while
waiting for a card. Existing cards retain their lifetime and generator ownership.
