# Connecting agent work

The user opens and places the Asset Preview window. Connect work through
`~/workspace/asset-preview/bin/asset-preview`; an agent does not need fish loaded.
Use `start` if the socket is absent. Registration does not raise the window;
`gui` is the deliberate visible-window action.

Use a stable, project-specific ID and connect the authoritative exported file
before generation starts. Missing files are valid registrations:

```sh
~/workspace/asset-preview/bin/asset-preview add /absolute/project/assets/generated/chair.glb \
  --id project-chair --label "Chair · geometry"
```

The user's current selection stays in place. `select project-chair` changes it
without moving or raising the window; use it when the user wants to follow that
asset. Later registration with the same ID updates its paths and command. Same-kind
reconnections retain inspection settings; explicit settings override them. New previews
inherit the app's remembered image/video/3D preferences. A `settings` request also
updates those defaults, so preserve the user's inspection choices when changing them.

When the agent already runs a generator, use file watching alone. The exported
output will refresh at each successful save. When saving source should trigger a
fresh export, pass an explicit finite generator and its input files:

```sh
~/workspace/asset-preview/bin/asset-preview add /absolute/project/preview/chair.obj \
  --id project-chair --label "Chair · procedural" \
  --watch /absolute/project/tools/build_chair.py \
  --watch /absolute/project/design/chair.json \
  --cwd /absolute/project \
  --exec /absolute/project/tools/run-build-chair.sh
```

That wrapper owns runtime/container invocation, output selection and atomic
publication. Reuse existing game-dev-tools, CRS, Material Maker or Godot capture
flows. Keep their project locks and hardware-renderer rules. The viewer neither
replaces those flows nor implies permission to publish, install packages or run
unrelated project launchers. A generator that writes a watched input can produce a
feedback loop; watch source files only.

For texture finishing, use `add image.png`. For baked Material Maker surfaces,
use `material albedo.png --normal normal.png --orm orm.png`. For authored model
materials, prefer a self-contained GLB. For Godot-only materials, use a finite
offscreen capture command and connect its PNG; an ordinary game `play.sh` is an
interactive process and is unsuitable as this generator.

For rendered turntables or animation captures, connect the local video output:
`add /absolute/project/preview/turntable.mp4 --id project-turntable`. It uses the
same finite-generator/source-save flow; publish finished encodes atomically.
Playback defaults to muted looping. Preserve the user's paused/muted/loop choices;
`settings ID '{"paused":true}'` and `seek ID 2.5` support inspection. Loading is
asynchronous; wait for Live and `metrics.loads`, and read `metrics.hwdec` separately
from renderer identity when reporting hardware decoding.

Inspect `status --json` after connecting: `active` distinguishes a visible entry
from a suspended one, `building` distinguishes an in-flight command, `status`
reports waiting/failed/live state, and `metrics.renderer` identifies the actual
GPU for a loaded 3D preview. File registration alone is not evidence of a successful
export or correct material appearance. Remove only the registrations you own when
finished; removal does not delete source or asset files.

The [`$asset-preview` skill](../skills/asset-preview/SKILL.md) is installed by
`python tools/install.py` as an owned symlink in Codex's skills directory. Its
instructions apply during visual asset authoring and live inspection. Asset
Workshop's `AGENTS.md` requires that connection throughout visual iteration;
its manifest helper uses stable `workshop-<asset>-<view>` IDs.

The newline JSON socket is available when another tool needs a direct connection;
the CLI implements startup serialization and validation, so prefer it for ordinary
agent work. See [protocol.md](protocol.md). The skill follows this maintained contract.
