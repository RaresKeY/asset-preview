# Asset Preview

Asset Preview is a live desktop viewer for images, video, audio, 3D models, baked
materials, Markdown, and file diffs. Open files directly or send them from scripts
and agents through the CLI or [Codex skill](skills/asset-preview/SKILL.md).
Previews refresh when files change. Background sends preserve window placement
and selection.

![Asset Preview Gallery showing a selected render beside project thumbnails](docs/screenshots/hauler-gallery.png)

Use `add FILE --project my-game --group props` to send a file to the project's
Props gallery. Overview shows all galleries. Gallery puts thumbnails beside the
selected preview, and Projects (Ctrl+9) shows current previews side by side.
Right-click a gallery to move it between projects. Sends without an explicit
project use the caller's owning checkout. See the [project guide](skills/asset-preview/references/projects.md).
More views are shown in the [image comparison](docs/screenshots/image-preview-hauler.png)
and [3D model preview](docs/screenshots/model-preview-hauler.png).

Source builds with F3D 4 and a separately installed TRELLIS reader can
open `.post`, O-Voxel `.vxz` or lossless decoder `.ovoxel` files directly.
O-Voxel previews offer **Preview Options → O-Voxel View** with extracted raw
surface, raw voxel cells, occupied cell centers and decoded vertex positions.
Cells use their exact grid size; point markers scale with zoom. The same file
remains authoritative in every mode; see
[model support](specs/models.md) for the display and format boundaries.

Material recipes can also be presented as live graphs:
`./bin/asset-preview add /absolute/material.ptex --kind graph`.
The graph arranges saved connections automatically, with node thumbnails,
expandable parameters and a shaded saved-map preview; see
[material graphs](specs/material-graphs.md).

## Downloads

[Release 0.0.2](https://github.com/RaresKeY/asset-preview/releases/tag/0.0.2):
AppImage, portable archive, Flatpak, `.deb`, `.rpm`, Arch package and `.run`.
See [Linux installation](docs/install-linux.md) for commands and requirements.
Build from source for the newer features documented below.
Private container: `ghcr.io/rareskey/asset-preview:0.0.2`.
[Dependency notices](THIRD_PARTY_NOTICES.md), corresponding sources and checksums
accompany the downloads. `asset-preview licenses` prints the notices.

## Build and install

Requires Linux, CMake 3.22+, a C++20 compiler, Python 3, Qt 6.4+ with Widgets,
Network and OpenGL, F3D 3.5+ with native and Assimp readers, and development
packages for MD4C, libmpv, libavformat, libavcodec and libavutil. These libraries
must be discoverable through `pkg-config`.

```sh
./tools/build.sh
python tools/install.py
./bin/asset-preview
```

The installer adds a desktop entry, fish command/completion and the Codex skill.
Use `./bin/asset-preview` from other shells, or launch through `./play.sh`.
Installation points into this checkout; reinstall integration after moving it.

Crash reports persist in `~/.local/state/asset-preview/crash.log` (or the configured
`ASSET_PREVIEW_STATE_DIR`), with one rotated backup. Normal use writes no session
log; recent diagnostics stay in memory until a failure. Fatal reports retain
preview context and native fault details across restarts. An unreportable kill
or power loss is recorded as an unclean exit on the next launch.

Source launches use native Wayland (`QT_QPA_PLATFORM=wayland`) when the caller
provides `WAYLAND_DISPLAY`, and X11 (`xcb`) otherwise. Explicit platform
overrides are respected. `tools/test.sh wayland-gpu` checks rendering without
an X11 display in a private KWin compositor nested inside headless Gamescope.
That lane uses installed Spectacle to verify actual compositor pixels as well
as the source textures.
Published 0.0.2 packages retain their original X11/XWayland requirements.

## Commands

Review proposals with large check/X controls using a `.selection.json` file:
`asset-preview open /absolute/choices.selection.json`. Batch and one-at-a-time
views share saved, per-option decisions; see [Selection](specs/selection.md).

Examples use the installed fish command; otherwise use `./bin/asset-preview`.

```sh
asset-preview                                      # open the window
asset-preview chair.glb                            # open and select a file
asset-preview open texture.png "a song.wav"         # open several files; select the last
asset-preview add /path/to/chair.glb --id chair --label "Chair"
asset-preview add /path/to/texture.png --id texture
asset-preview add /path/to/turntable.mp4 --id turntable
asset-preview add /path/to/song.wav --id music       # MP3, FLAC, OGG, M4A and more
asset-preview open README.md                       # rendered Markdown
asset-preview add /path/to/source.cpp --id source   # read-only code, live saves
asset-preview diff before.cpp after.cpp --id changes --mode split
asset-preview add /path/to/changes.patch --id patch # multi-file unified diff
asset-preview settings music '{"paused":false}'     # start playing
asset-preview layout grid --size 3        # sizes: 2, 3, 4
asset-preview select chair
asset-preview status --json
asset-preview remove chair                         # preserves the file
asset-preview close-all                            # closes previews; keeps files/settings
```

The Fish function completes commands, file paths (including multiple files),
options and registered preview IDs. Press Tab after `asset-preview ` or
`asset-preview open `. Drag files from Dolphin into the window, onto an existing
preview, or onto its desktop launcher. **Open…** / **Ctrl+O** also accepts multiple
files. Opening selects the last file; reopening a path reuses its first registered
preview and keeps its generator, display settings and camera. Drops preserve the
original files. For filenames starting with `-`, use `asset-preview open -- FILE`.

Reusing an ID updates its registration. Files can be registered before they exist.
`start` starts the service without raising the window; `hide` suspends all views;
`stop` exits the service. Registrations, layout and all display/playback preferences
persist across restarts. New previews inherit the last-used image/audio/video/file/3D settings;
existing previews retain their own choices. **Close all** clears the previews while
keeping the window, files and remembered settings.

Documents support rendered Markdown, syntax-colored source, line numbers and
Ctrl+F / F3 search. Rendered Markdown supports `<br>` and sized local HTML images
alongside ordinary Markdown; code examples and escaped tags stay literal.
Use Ctrl+scroll or Ctrl++ / Ctrl+- to zoom Markdown.
Diffs offer inline or side by side views, changed-file
navigation, word highlights, change navigation and expandable context. Compare
two local versions with **File > Compare Files…** or `diff BEFORE AFTER`.
The CLI `diff` command registers in the background and watches both files.
Godot scripts/scenes/resources, C++, Python, shaders, JSON, YAML, TOML, CSV and
logs auto-detect. See [file support and limits](specs/files.md).

## Controls

**Settings > Theme > Auto / Dark / Light** changes the app's appearance immediately
and remembers it across restarts. Auto follows the system theme. Preview backgrounds
remain independent display settings.

| Preview | Controls |
|---|---|
| Files / Markdown / diffs | Rendered or source Markdown, inline or paired diffs; soft wrap on by default, Alt+Z toggles; Ctrl+F finds |
| Image | Wheel zoom toward pointer, drag pan, fit, AA on/off or automatic (off for raster sources up to 256 × 256), checker/dark/light background, T to [toggle linked images](specs/images.md) |
| Model | Drag orbit, right/middle/Shift drag pan, wheel zoom around the existing focal point, double-click/Fit to frame, Reset for default camera, E edges, M triangle count |
| 3D scene / modular assembly | Load the authoritative assembled GLB as a model; inspect modules or owner-generated proof views alongside it |
| 3D animation / rig | Clip selector, play/pause, loop, speed, scrub timeline and stepped playback with frame rate; comma/period step frames, Space pauses, Home restarts |
| Audio / music | Music-reactive stereo ribbons and onset glow; play/pause, restart, elapsed/total time, click/drag timeline, mute and loop; Space/double-click pause, Shift+Left/Right seek, Home restart, M mute |
| Video | Playback bar with play/pause, restart, elapsed/total time, click/drag timeline, mute, loop and fullscreen; Space pauses, double-click toggles fullscreen, Shift+Left/Right seeks, Home restarts, M mutes |

The [presentation inventory](specs/presentation.md) lists the information and
controls shown by every preview type and mode.

**F11** or **View > Full Screen** puts the current layout fullscreen.
Double-clicking a video or using its fullscreen button expands that preview;
**Esc** or **F11** exits and restores the layout. An exit-shortcut notice stays
for three seconds, then slides up out of view. Video controls and the cursor hide
after 700 ms of inactivity and immediately on entering fullscreen. Move the
pointer or use the keyboard to reveal them; hovering controls, seeking and
keyboard focus keep controls available. The video bar has a thin teal timeline,
round playback buttons and a time pill, with secondary actions at the right.

The **⋯** menu provides display options, including clay/textured shading,
lighting, grid, axes, projection and Y/Z-up. Videos loop and start muted. Audio starts paused and unmuted, so pressing Play
starts listening. Audio/video preferences are remembered separately; explicit
`add --settings JSON` overrides them for that preview.
Audio visuals follow measured loudness, stereo balance and spectral brightness
from the playing track. They work while muted, settle during silence and freeze
when paused. Hidden and off-page players unload completely.
3D camera angle, pan and zoom are saved per preview across switching and restarts.
For texture-heavy GLBs, choose **⋯ > Model texture detail > Fast GLB preview ·
1024 px**, or run `asset-preview settings ID '{"texture_quality":"preview"}'`.
This uses a bounded cache of smaller embedded textures; the first use prepares
the cache. **Original** remains the default. Parts/Cuts always use the original
model and full-resolution textures. Unsupported files fall back to Original.
Animated GLB/glTF models start paused. Toggle **Stepped** to hold poses at a
chosen frame rate (12 fps initially). Previous/Next frame or comma/period pause
and move one frame. Options also expose these controls in narrow cards.
Transport settings stay with that asset;
static models have no playback bar or animation timer. Skeletal, node-transform
and morph clips are supported through F3D. For example:

```sh
asset-preview add /path/to/character.glb --id character \
  --settings '{"paused":true,"animation":0,"animation_speed":1,"loop":true}'
asset-preview settings character '{"paused":false}'
asset-preview seek character 0.75
```

`status --json` reports available `metrics.animations` and the effective clip,
duration and position. Clip indices are zero-based. Source replacement restarts
playback with stored transport choices; off-page/hidden views unload.

Image zoom and pan are saved too. Point at a preview to target its shortcuts and
menus, with no selection highlight. Outside the previews, the last active one
stays the target. **Ctrl+W** closes it; **Ctrl+Shift+T** reopens the
last closed preview with its settings and view (within the current session).
Closing shows the preview on its left, or the next one when closing the first.
**Settings → After Closing a Preview** switches between **Left Preview** and
**Last Viewed Preview** and remembers the choice. The return target preloads
eligible images into the bounded cache; other files receive a bounded read to
warm the OS cache, without starting hidden renderers, players or generators.

**Overview** is the preferred starting view, showing all open assets as compact
thumbnails grouped by project. Saved layout choices still restore. **Esc** returns
from other views to Overview with all projects and grouping enabled; in fullscreen,
press Esc once to exit fullscreen, then again to return. You can also choose
Overview in the layout picker or press **Ctrl+0**. Click a tile or press Enter to
open it in Single view. Two image
workers and a separate content worker prepare thumbnails at low priority, with
visible images first. After a quiet period, nearby and other open assets warm
the bounded thumbnail cache, including while using Projects or Single view.
Loading ordinary previews pauses warming; hiding cancels it. Model/material jobs
render one finite offscreen frame. Markdown and code/text files show a first-page
preview; videos show a midpoint frame, falling back to the opening frame.
These also work before opening an asset. Other asset kinds reuse a frame from
their opened view. Thumbnail work starts no players or asset generators. Projects also
prefetches the next/previous member after settling on a visible project.

The folder button below **File** (or **Ctrl+B**) opens a resizable explorer with
one folder per category and each open preview listed underneath. Select a file
to open Single view; double-click a folder or press Enter to open its Gallery.
Toggle it off to collapse the entire pane. Its width and visibility are remembered.

Small decoded images and waveforms reuse a bounded cache: **32 MiB per payload,
128 MiB total**, including a **16 MiB thumbnail allowance**. Self-pruning disk
derivatives total **256 MiB**. View → Clear Image and Thumbnail Cache clears small
derivatives; hiding the app releases retained RAM. [Cache details](specs/cache-overview.md).

`asset-preview cache` shows cache churn suspects and the log location.
`asset-preview cache events --json` shows recent admissions, unused evictions,
reloads after removal, pruning and heap reclamation with reasons and timings.
Diagnostics use bounded history and two rotating 256 KiB files; they add no idle
polling and omit repeated thumbnail paint-hit log lines.

**View → Camera Views** offers front/right/top/isometric with **1 / 3 / 7 / 9**;
Ctrl+1/3/7 gives the opposite view. Numpad keys also work. **5** or **Numpad 5**
toggles perspective/orthographic, including in Parts/Cuts. **F** fits and
**Ctrl+Home** resets the camera. **View → Compare With** opens two 3D previews
side by side; **Sync camera** links orbit, pan and zoom around each model.

**File → Save Screenshot** (**Ctrl+Shift+S**) saves image pixels with transparency, or the current rendered frame, to
**Pictures/Asset Preview**. **Copy Image / Frame** (**Ctrl+Shift+C**) copies just the
image pixels (including transparency) or the current rendered frame. **Copy View
Screenshot** keeps the current framing and background. File can also save the
entire window.

**Codex ›** opens the originating conversation in a resizable right sidebar.
The default split is 9:7 (preview/Codex); resizing preserves it, and dragging the
divider saves your preferred split.
Connect the asset from that Codex thread first. **Reference** (**Ctrl+Shift+A**)
freezes the view for optional circles, highlighting, numbered dots, pen and text.
**Window** in the sidebar captures the actual Preview interface. **Attach to
Codex** saves the PNG under Pictures/Asset Preview and inserts it into the draft;
add an explanation and send or queue it normally. Native **Ctrl+V** pastes images,
and **PNG…** attaches other files. Collapse/reopen preserves the draft. The
conversation stays pinned during navigation; **Follow asset** explicitly switches
to another asset's originating thread. Closing Preview leaves Codex background
work and queued prompts running. **File → Quit** ends terminal clients: wait for
Tab-queued prompts to drain before quitting the service. The terminal requires
installed Codex with Unix
remote attachment and the optional KF6 Parts / Konsole component.

For 3D models and materials, **Preview Options > 4 View** (Ctrl+Alt+Q) shows
perspective, front, right and top together. Scroll zooms the pointed pane; drag
front/right/top to pan. The setting is remembered per asset.

**Preview Options > Edge topology > Quads / source faces** shows original OBJ/PLY
polygon boundaries; **Triangulated faces** includes tessellation diagonals. The
badge shows quad counts where present. GLB/glTF remains triangulated.

Left/Right navigates previews or pages, matching the toolbar arrows;
Alt+Left/Right also works. Live, waiting, building and error status stays visible.
Only the current page is loaded. Minimizing or closing releases views and stops
their generators; closing leaves the service running.

Use the top-right checkboxes or Ctrl+click to select several previews, then
**Close selected (N)** in the toolbar. Shift+click selects a range; Ctrl+Shift+click
adds a range. Selection works in live grids and Overview and survives navigation.
**File > Close Selected Previews** also closes the checked items; Ctrl+W continues
to close the active preview. Ctrl+Shift+T reopens closed previews.

## Inspect 3D and leave comments

For static GLB/glTF models, choose **View > Inspect 3D** or use the model's
options menu. Models opened from the shell, Open dialog or a file drop also show
a toolbar selector: **Preview**, **Part mode**, **Cut mode**. Part mode exposes
component selection, hide/show and isolate; Cut mode focuses on X/Y/Z sections.
Both modes show cut sliders above the part tools. Drag a slider to enable and
move that plane; use the numeric field for precise positioning and **Keep +/−**
to choose its retained side. Uncheck an axis to disable it, or **Reset cuts**
to clear all three.
Switching between Parts and Cuts keeps the same scene, selection and cuts.
Other model formats show the static GLB/glTF requirement in the selector tooltip.
Click a component to see its source name, then add a part comment
or surface pin. The native panel provides X/Y/Z cuts, hide/show, isolate, undo
and restore. **Return to Preview** restores the ordinary F3D view and layout.
Inspection uses its own light kit or authored lights, so shading can differ.

Comments are saved as readable JSON, bound to the displayed model and external
resource revisions. Changed targets are marked stale. Choose a project-owned
review file outside generated exports, or use the default persistent review path
shown in the panel.

```sh
asset-preview inspect open chair --review /absolute/project/reviews/chair.json
asset-preview annotations list /absolute/project/reviews/chair.json --json
asset-preview annotations export /absolute/project/reviews/chair.json --format markdown
```

Offline annotation list/export does not start the viewer. The inspection adapter
loads only for a visible inspected model, with no idle rendering or picking.
It requires installed VTK 9.7+ development files; source builds can disable it
with `-DASSET_PREVIEW_INSPECTION=OFF`. See [the inspection contract](specs/inspection.md)
and [protocol](docs/protocol.md). Automatic shell discovery, geometry editing,
animated rigs and synthetic cut caps are outside the current scope.

## Source-triggered generation

If the agent already builds the asset, register its output with `add`.
To rebuild on source saves, attach a finite command:

```sh
asset-preview add /absolute/project/exports/chair.glb --id chair \
  --watch /absolute/project/source/chair.json \
  --cwd /absolute/project \
  --exec /absolute/project/tools/build-chair.sh
```

Put `--exec` last. Arguments run directly without shell expansion. Repeat
`--watch` for input files; exclude generated outputs. Commands run only while
the view is visible, with serialized runs and a default 120-second timeout
(`--timeout` accepts 1–3600). Publish outputs atomically. Failed builds retain
the last good preview and show the error.

## Baked materials

```sh
asset-preview material /path/to/albedo.png --id finish \
  --normal /path/to/normal.png --orm /path/to/orm.png --shape sphere
```

Shapes: sphere, cube, plane. Albedo is sRGB; normal and ORM are linear.
ORM channels are occlusion, roughness and metallic in R/G/B. Engine shaders and
material graphs require an exported model, baked maps or an engine capture PNG.

## Limits and verification

Image/model/material files are limited to 256 MiB. Images decode to at most
4096 pixels per edge. Text inputs require UTF-8 and are limited to 2 MiB,
20,000 newlines and 16,384 characters per line. PDF and Office document rendering
is not included. Audio and videos stream from local files. Model imports run
on a native worker; an import already running cannot be interrupted. Exported 3D
animation requests frames only while visible and playing;
paused/static scenes remain idle. Format support depends on the
installed Qt image plugins, F3D readers and mpv codecs. Hardware video decoding
is requested with software fallback. Static and paused views have no idle render loop.

```sh
./tools/test.sh functional
./tools/test.sh gpu
./tools/test.sh video-controls
./tools/test.sh animation
./tools/test.sh animation-gpu
./tools/test.sh files
./tools/test.sh files-gpu
```

GPU checks use headless Gamescope; media encoding fixtures require FFmpeg.
Tests set `ASSET_PREVIEW_TEST_NULL_AUDIO=1` to route libmpv to a null output.

[Agent workflow](docs/agent-flow.md) · [Protocol and settings](docs/protocol.md) ·
[Specs](specs/_readme.md) · [Rendering](vendored/rendering.md) ·
[Verification evidence](evidence/verification.md)

## License

[MIT](LICENSE) for Asset Preview. Dependencies retain their own licenses; see
[third-party notices](THIRD_PARTY_NOTICES.md).

Browse a collection with compact thumbnails on the left and one full preview on
the right using **View > Image Gallery** (Ctrl+8), or
`bin/asset-preview layout gallery --project PROJECT`. The divider resizes,
project filtering/grouping selects the collection, and thumbnail clicks retain
the gallery while loading only the selected ordinary preview.
