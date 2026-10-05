# Asset Preview

**Keep one preview window open while AI agents build your assets.** Agents send
images, 3D models and baked materials to the window as their work gets done;
connected files refresh automatically on save, so you can follow progress and
inspect results throughout an iteration.

Place the window beside your editor or agent conversation. Agents use the CLI
or the included [Codex skill](skills/asset-preview/SKILL.md) to register outputs
without raising the window or changing your selection. Stable IDs let them update
the same preview as they refine an asset. See [the agent workflow](docs/agent-flow.md).

## Compact grid showcase

### Follow several assets at once

A compact 3×3 page combines PC components, a material comparison and character
models from ongoing agent work. The same chef appears with authored materials
and in clay mode for geometry inspection.

![Compact 3×3 live preview grid with PC components and character models](docs/screenshots/compact-grid-showcase.png)

### Inspect a smaller set in detail

A compact 2×2 page gives individual components more room. Each view has its own
orbit camera and display options; saving a connected asset refreshes its preview.

![Compact 2×2 preview grid showing a PC case, motherboard, GPU and cooler](docs/screenshots/compact-grid-components.png)

These are direct application captures using hardware OpenGL rendering, with
camera angles and lighting adjusted in the viewer.

## Start a live preview session

Open it with **Super → Asset Preview**, `asset-preview`, or `./play.sh`.
The fish function and completion are installed by `python tools/install.py`.
The executable `play.sh` also makes this project eligible for Playpad discovery.

```sh
asset-preview start                 # start the socket server without raising a window
asset-preview                      # open / return to the same window
asset-preview add /path/to/model.glb --id chair --label "Chair"
asset-preview add /path/to/image.png --id texture
asset-preview layout grid
asset-preview layout grid --size 3 --compact
asset-preview select chair
asset-preview status
asset-preview hide                  # release every active preview and stop its generator
asset-preview stop                  # stop the server too
```

`add` connects quietly to the running app. It does not raise the window or
change the current selection. Reusing an ID replaces that registration.
Registrations, layout, grid size, compact mode and selection survive restart; closing the window suspends
all previews while keeping the server available. Window size and placement are
remembered on close, subject to the desktop's placement policy.

## Window

Drop files or use **Add files**. Choose **Single**, **2×2**, **3×3** or **4×4**
in the toolbar. Single loads one preview; grids load only the current page, up to
4, 9 or 16 previews. Left/right buttons and Alt+Left/Right switch previews
or pages. Every preview outside the current page is unloaded. Minimizing or
closing the window releases the visible previews and stops their generators.

Images have fit, wheel zoom, drag pan, checker/dark/light backgrounds and pixel
filtering. Models have drag orbit, right/middle/Shift drag pan, wheel zoom and
double click to fit. Orbit defaults to a locked horizon: Y is up and the XZ plane
is the floor, with pitch clamped before the poles. The **⋯** menu inside each
view can unlock orbit or choose Z-up / XY floor for Blender-oriented assets.
It also controls ground grid, axes, edges and orthographic projection.

**Show materials** switches authored shading to neutral clay; **Show textures**
independently removes texture maps while retaining material properties. Turning
them back on restores the imported appearance through a scene reload. Default
**Studio** lighting adds F3D's embedded HDRI environment to its normal lighting;
**F3D light kit** is available without the environment. Both use tone mapping,
with adjustable light intensity. Cameras stay in place across file refreshes and display-mode changes;
switching away releases the camera with the renderer.

Names, fit/options/remove actions and live status overlay the views. Long names
are elided with full name/path tooltips. **Compact** reduces gaps, moves fit/remove
into **⋯**, and hides steady Live footers; building, waiting and error status stays
visible. `layout ... --no-compact` restores normal density.

## Save a script, see its output

Connect a generated output with an explicit program and watched source files:

```sh
asset-preview add /tmp/asset-preview-demo/cube.obj --id demo \
  --label "Procedural cube" \
  --watch ~/workspace/asset-preview/examples/generate_cube.py \
  --cwd ~/workspace/asset-preview \
  --exec python examples/generate_cube.py /tmp/asset-preview-demo/cube.obj
asset-preview select demo
asset-preview
```

Edit `SIZE` or `COLOR` in the example and save. The app runs the command once
when the preview becomes visible, then on source saves. It serializes runs,
coalesces changes, displays a bounded stdout/stderr tail, and refreshes after
a successful exit. Valid intermediate outputs also refresh while it is running.
Failed runs retain the last good published preview and display the
failure. The default timeout is 120 seconds; `--timeout` accepts 1–3600.

Put `--exec` last: everything after it is the program and its arguments. Arguments
are passed directly, without shell expansion. Use an explicit script if your
generator requires a pipeline. Use the owning project's prescribed runtime or
container wrapper. Outputs must not be watched as generator inputs. Include each
source/recipe/dependency that should trigger regeneration with repeated `--watch`.
Watching is file based, not recursive directory scanning.

Generators run only while their preview is visible. They should be finite jobs
that publish their outputs atomically; long-lived render servers and interactive
game launchers are not generator commands. Removing or hiding a preview stops its
owned process group. Only connect commands you intend to execute as your user.

## Baked materials

```sh
asset-preview material /path/to/albedo.png --id finish \
  --normal /path/to/normal.png --orm /path/to/orm.png --shape sphere
asset-preview select finish
asset-preview settings finish '{"shape":"cube","grid":false,"light":1.2}'
```

Material maps refresh together on a sphere, cube or plane. Albedo is sRGB;
normal and packed ORM are linear. ORM is occlusion in R, roughness in G, and
metallic in B. Normal mapping uses the renderer's glTF convention. Models retain
their authored materials, textures, hierarchy and transforms through F3D's readers.

For **game-dev-tools / CRS**, connect their exported OBJ/MTL, GLB or capture PNG.
For **material-atlas**, connect its baked albedo/normal/ORM maps or textured GLB.
For exact Godot procedural shaders, connect a PNG produced by the owning Godot
capture flow. A generator can call that project's shared Godot Podman wrapper.
F3D does not execute `.gd`, `.tscn`, `.gdshader` or Material Maker `.ptex` graphs;
those inputs require their owning exporter/baker/capture command. This viewer's
PBR lighting is an inspection environment, not proof of a Godot shader match.

## Refresh and resource boundaries

- Native filesystem notifications; 220 ms save debounce; no idle polling or render loop.
- Atomic replacement and missing parent directories are supported. Failed loads get
  three delayed retries while retaining the last successfully loaded content.
- OBJ material files and ordinary MTL texture paths, glTF/GLB external buffers and
  texture URIs, explicit dependencies and material maps are watched. Complex MTL
  options or several `mtllib` files on one line may need explicit `--watch` paths.
- Default grids activate at most four decoded images/renderers/generators; larger
  grids explicitly allow nine or sixteen and can use more memory. Only `native` and
  `assimp` F3D readers are loaded. F3D/VTK stays unloaded until a 3D preview is shown.
  The backend remains resident after first use; hiding frees views and trims free heap
  pages, while library/driver caches remain. PNG/JPEG and related images depend on Qt's installed
  image plugins; 3D formats depend on these two F3D readers.
- Preview files are capped at 256 MiB. Images decode to at most 4096 pixels per edge
  with a 64 MiB Qt allocation limit; oversized images may be refused. The UI exposes
  original and decoded dimensions through `status --json`. Models have no guaranteed
  resident-memory budget; imported scenes and the GPU driver can be substantially larger.
- Model loading uses the GUI's OpenGL thread. Very large imports can temporarily pause
  the UI. Animation is static; this is an asset inspection tool.

## Build, install, verify

```sh
./tools/build.sh
python tools/install.py
./tools/test.sh functional
./tools/test.sh gpu
```

For measured idle memory and real-project captures, use the replay command in
[verification evidence](evidence/verification.md).

The native app uses C++20, Qt 6 Widgets and libf3d. The command client and fish
completion use Python's standard library.

Dependencies are the workstation's CMake, C++ compiler, Qt 6 Core/Gui/Widgets/
Network/OpenGL and F3D 3.5+ library with native and Assimp readers. The build and
launcher never install dependencies. `build/` is the one current executable build,
not a release archive.

The launcher defaults this app to Qt's `xcb` platform, using XWayland on KDE
Wayland. Hardware rendering and embedded views are verified on that path. Native
Wayland embedding currently fails the 3D check and is unsupported. An explicitly
set `QT_QPA_PLATFORM` is respected; the launcher does not change your shell settings.

The installer uses owned fish and Codex skill symlinks plus a validated desktop
entry; it refuses to overwrite unrelated files and does not change global hotkeys or PATH. After
moving the checkout, remove the old owned fish/skill symlinks, set
`ASSET_PREVIEW_PROJECT` for fish and reinstall desktop integration. Use
`bin/asset-preview` directly from other shells.

`asset-preview paths` prints the private Unix socket and state directory. Defaults
are `$XDG_RUNTIME_DIR/asset-preview` and `$XDG_STATE_HOME/asset-preview`, with private
fallbacks under `/tmp` and `~/.local/state`. `ASSET_PREVIEW_RUNTIME_DIR` and
`ASSET_PREVIEW_STATE_DIR` support isolated sessions. Every socket request stays
inside the user's local permissions. State is written atomically and corrupt state
is preserved with an error. `server.log` holds the current daemon session; generator
tails are bounded to 16 KiB per preview.

See [agent connection examples](docs/agent-flow.md), [the socket protocol](docs/protocol.md),
[renderer decisions](vendored/rendering.md), [specs](specs/_readme.md) and
[verification evidence](evidence/verification.md). The installed
[`$asset-preview` skill](skills/asset-preview/SKILL.md) connects agent work through
this interface; its canonical source and UI metadata are maintained in this repo.
