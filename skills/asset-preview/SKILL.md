---
name: asset-preview
description: Connect live image, video, model and baked-material previews during asset authoring or inspection. Use the existing Asset Preview app and its source-save generators; exclude viewer implementation and game profiling.
---

# Asset Preview

Use the existing native app at `${ASSET_PREVIEW_PROJECT:-$HOME/workspace/asset-preview}`.
Call its `bin/asset-preview` directly; fish setup is unnecessary. It owns one
window, saved registrations and a private Unix socket. The CLI handles startup
and socket communication. Read that checkout's `docs/agent-flow.md` for exporter
integration or `docs/protocol.md` for additional commands/settings.

Connect the authoritative exported output early, even if it does not exist yet.
Choose a stable project/asset/view ID (letters, digits, `_` or `-`, max 64), and
keep other work's registrations intact. Asset Workshop already defines
`workshop-<asset>-<view>` IDs and a manifest-driven connector:
`./workshop preview <asset> [--view <name>]` from its root.

```sh
preview="${ASSET_PREVIEW_PROJECT:-$HOME/workspace/asset-preview}/bin/asset-preview"
"$preview" add /absolute/project/exports/chair.glb \
  --id project-chair-model --label "Chair · geometry"
"$preview" material /absolute/project/exports/albedo.png \
  --id project-chair-finish --normal /absolute/project/exports/normal.png \
  --orm /absolute/project/exports/orm.png --shape sphere
```

Register once and let output saves refresh the view. Re-registering an ID replaces
its entire configuration and recreates the view; when changing a connection,
retain its explicit generator/watch configuration and reapply the user's settings
from `status --json`. Use `settings ID JSON` for partial option changes.

Registration starts the service if needed without raising the window or changing
selection. `start` starts it in the background; `gui` opens/raises it for a
requested visible launch. Preserve the user's placed window during iteration.
`select ID` changes the shown asset/page without raising it. Hidden, minimized
and off-page previews unload their images, renderers and video players; each
generator's owned process group stops. Registration does not start a hidden
generator.

When the agent already runs the build, ordinary output watching is sufficient.
To rebuild on source saves, attach one explicit **finite** generator to one view:

```sh
"$preview" add /absolute/project/exports/chair.obj \
  --id project-chair-model --watch /absolute/project/source/chair.json \
  --watch /absolute/project/scripts/build-chair.sh --cwd /absolute/project \
  --exec /absolute/project/scripts/build-chair.sh
```

`--watch` alone never executes a script. Put `--exec` last; argv has no shell
expansion. The wrapper owns its prescribed runtime/container invocation, mounts
and project/GPU locks. Watch source and recipe dependencies, excluding generated
outputs. Publish validated outputs atomically. Do not run the same builder
independently while this view owns it or
attach it to several views. Editors, games and long-lived servers are unsuitable
generators. Source-triggered generation runs only while the view is visible.
Valid intermediate output saves refresh during a run. The timeout defaults to
120 seconds; set `--timeout` explicitly for a known finite job needing more
(max 3600).

Use GLB/OBJ for exported geometry, PNG/images for textures/captures, and baked
sRGB albedo plus linear tangent normal/ORM for material samples. ORM packs
occlusion/roughness/metallic in R/G/B. F3D does not execute Godot `.gd`, `.tscn`,
`.gdshader` or Material Maker `.ptex`; use the owning exporter/baker, or a finite
Godot PNG capture for exact shader appearance. F3D lighting is an inspection
environment, not engine-shader equivalence.

For rendered turntables/animation captures, connect the local finished encode with
`add /absolute/project/preview/turntable.mp4 --id project-turntable`; common video
extensions are automatic, other local video formats can use `--kind video`.
The same watched-output/finite-generator flow applies. Publish encodes atomically.
libmpv streams videos; a reload completes asynchronously, restarts at the beginning
and preserves paused/muted/loop preferences. Playback defaults to muted looping.
`settings ID '{"paused":true}'` pauses it; `seek ID 2.5` seeks a loaded visible
video. Check successful publication as described below. `metrics.hwdec` reports
the actual decoder separately from GPU renderer identity; software fallback works.

Preserve inspection choices unless the task calls for a change. `layout single`
shows one view; `layout grid --size 3 --compact` shows a compact 3×3 page.
Grid sizes 2/3/4 show 4/9/16 views, with more active views costing more memory.
`--compact` tightens spacing and `--no-compact` restores normal density; per-view
controls live inside each view. For 3D, default `lock_horizon:true` and
`up_axis:"y"` keep the XZ floor level; `up_axis:"z"`
uses an XY floor. `lighting:"studio"` uses F3D's HDRI ambient plus normal lights;
`"lightkit"` uses normal lights. `materials:false` shows opaque neutral clay;
`textures:false` hides maps while retaining material properties. These are JSON
keys for `settings`, for example `settings ID '{"textures":false}'`.

Check `status --json`: confirm the expected path/ID; distinguish `active`,
`building`, waiting/failure status and successful revisions/loads. For video
refreshes, compare before/after revisions or loads and wait for loading to finish;
accepting a load command is insufficient. `metrics.loads` resets when a view unloads or is
recreated. A registered or suspended view does not prove a build ran or looked
correct. Loaded 3D views report `metrics.renderer`; use that for renderer evidence.
On failure, inspect the bounded generator tail or service log and correct the
source/runtime/path;
do not treat retained last-good content as the new output. The supported desktop
path is XWayland (`xcb`); retain the launcher's default unless testing an explicit
alternative. If the executable is absent, read its AGENTS/specs and build using
existing dependencies; report an unavailable prerequisite rather than silently
substituting another renderer or installing packages.

For automated visual checks, use Gamescope headless through the owning project's
runtime and verify its actual renderer. Capture the preview with `capture NEW.png`
when visual evidence is needed; it requires a visible window and refuses overwrite.

Keep useful live connections during iteration. `remove <id>` unregisters only
that preview and preserves files. Avoid global `hide`/`stop` during task cleanup
because they affect other connected work.
