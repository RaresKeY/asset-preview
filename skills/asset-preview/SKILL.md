---
name: asset-preview
description: Connect images, exported 3D assets, baked materials and source-save generator outputs to the workstation's live Asset Preview window during visual asset authoring or when live inspection is requested. Use for the existing viewer, not for implementing its application or for unrelated game performance profiling.
---

# Asset Preview

Use the existing native app at `${ASSET_PREVIEW_PROJECT:-$HOME/workspace/asset-preview}`.
Agents call its `bin/asset-preview` directly; fish setup is unnecessary. It owns
one window, saved registrations and a private Unix socket. Prefer the CLI over
reimplementing startup/socket logic. Read that checkout's `docs/agent-flow.md` or
`docs/protocol.md` only when more detail is needed.

Connect the authoritative exported output early, even if it does not exist yet.
Choose a stable project/asset/view ID (letters, digits, `_` or `-`, max 64), reuse
it during iteration, and keep other work's registrations intact. Asset Workshop
already defines `workshop-<asset>-<view>` IDs and a manifest-driven connector:
`./workshop preview <asset> [--view <name>]` from its root.

```sh
~/workspace/asset-preview/bin/asset-preview start
~/workspace/asset-preview/bin/asset-preview add /absolute/project/exports/chair.glb \
  --id project-chair-model --label "Chair · geometry"
~/workspace/asset-preview/bin/asset-preview material /absolute/project/exports/albedo.png \
  --id project-chair-finish --normal /absolute/project/exports/normal.png \
  --orm /absolute/project/exports/orm.png --shape sphere
```

Registration starts the service if needed but does not raise the window or change
selection. Preserve the user's placed window; do not call `gui` on every save.
For a requested visible launch, `gui` opens/returns to it. `select <id>` follows a
particular view without raising it; use when the user wants that asset shown.
`layout single|grid` controls one view or a page of up to four. Hidden/off-page
previews are unloaded. Automated visual checks use Gamescope headless through
the owning project's runtime, rather than opening desktop windows.

When the agent already runs the build, ordinary output watching is sufficient.
To rebuild on source saves, attach one explicit **finite** generator to one view:

```sh
~/workspace/asset-preview/bin/asset-preview add /absolute/project/exports/chair.obj \
  --id project-chair-model --watch /absolute/project/source/chair.json \
  --watch /absolute/project/scripts/build-chair.sh --cwd /absolute/project \
  --exec /absolute/project/scripts/build-chair.sh
```

Put `--exec` last; argv has no shell expansion. The wrapper owns its prescribed
runtime/container invocation, mounts and project/GPU locks. Watch source and
recipe dependencies, excluding generated outputs. Publish validated outputs
atomically. Do not run the same builder independently while this view owns it or
attach it to several views. Editors, games and long-lived servers are unsuitable
generators. Source-triggered generation runs only while the view is visible;
hide/minimize/remove/quit cancels its owned process group. Valid intermediate
output saves refresh during a run. The timeout defaults to 120 seconds; set
`--timeout` explicitly for a known finite job needing more (max 3600).

Use GLB/OBJ for exported geometry, PNG/images for textures/captures, and baked
sRGB albedo plus linear tangent normal/ORM for material samples. ORM packs
occlusion/roughness/metallic in R/G/B. F3D does not execute Godot `.gd`, `.tscn`,
`.gdshader` or Material Maker `.ptex`; use the owning exporter/baker, or a finite
Godot PNG capture for exact shader appearance. F3D lighting is an inspection
environment, not engine-shader equivalence.

Check `status --json`: confirm the expected path/ID; distinguish `active`,
`building`, waiting/failure status and successful revisions/loads. A registered
or suspended view does not prove a build ran or looked correct. Loaded 3D views
report `metrics.renderer`; use that for renderer evidence. On failure, inspect
the bounded generator tail or service log and correct the source/runtime/path;
do not treat retained last-good content as the new output. The supported desktop
path is XWayland (`xcb`); retain the launcher's default unless testing an explicit
alternative. If the executable is absent, read its AGENTS/specs and build using
existing dependencies; report an unavailable prerequisite rather than silently
substituting another renderer or installing packages.

Keep useful live connections during iteration. `remove <id>` unregisters only
that preview and preserves files. Avoid global `hide`/`stop` during task cleanup
because they affect other connected work.
