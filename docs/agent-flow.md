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

## Surface-owned models and proof views

For fixed flush windows, paint strips and thin panels, connect the **host's actual
exported GLB**. Their faces belong to host topology with named material regions;
joining a duplicate coplanar patch into the same object does not remove competing
faces. A moving door/window is a separate named leaf fitted to a real aperture
and its boundary. A raised turret/machinery seat is a seated component and needs
no decorative hull hole. Classify raised covers, slats and curved features before
choosing an operator.

The canonical `surface-ownership/v1` declaration and saved-readback helpers belong
to `game-dev-tools/tools/geometry-authoring/src/surface_ownership.py`; read that
owner's `specs/surface-ownership.md` through its
[spec map](../../game-dev-tools/specs/_readme.md). Asset Workshop's
`constructs/surface-ownership` is the intended pinned caller; read the
[workshop map](../../asset-workshop/specs/_readme.md) and construct's own contract.
Recheck those files, implementation/pin hashes and current tests before invocation.
This viewer loads their products and does not add a geometry repair engine.

Keep an asset's model and small owner-generated PNG proof views connected under
separate IDs: host/material and raking edge, leaf closed/open, and raised-seat
contact when present. Each PNG must depict the matching saved export, with
recorded pose/camera/light/export and receipt hashes. A generic material sphere,
painted ownership diagram or count overlay cannot show whether the underlying
host faces were removed or an opening exists. Inspect the full model with authored
materials and neutral clay as appropriate; exact shader appearance remains an
owning-engine capture.

Use one finite owner build to publish the validated model, receipt and proof
images. Watch its declaration/recipe, source geometry, material source and build
wrapper/pin files, excluding their generated outputs. Attach that generator to
the model view only; image views watch their published files. While the agent owns
the build, ordinary output watching is enough. Publish candidate files atomically
after saved readback passes, and publish a hash manifest last so a mixed set of
revisions cannot be accepted as one candidate. Retain the last good revision on
failure and report the rejected candidate.

Before accepting a surface edit, bind the receipt to exact source/export bytes
and independently reopen its geometry. Native face IDs and exported primitive-local
triangle IDs are different index spaces. Check actual host material membership,
absence of competing faces, aperture/rim and leaf fit through the intended sweep,
or raised-seat contact. Recheck affected UV/tangent/bake, LOD, collision, animation
and appearance after topology changes. Report saved triangles/mesh nodes/material
primitives separately from render passes and gameplay cost.

`status --json`, a Live label, successful loads, triangle counts and HUD overlays
prove viewer lifecycle/diagnostics only. They do not certify geometry, material
ownership, opening clearance or animation. A schema-valid receipt may still have
untested or failed checks. Keep execution, saved geometry, visual verdict and
runtime adoption as separate outcomes.
