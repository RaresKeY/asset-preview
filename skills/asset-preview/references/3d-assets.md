# Models, scenes and modular assets

Use GLB/OBJ for exported geometry, PNG/images for textures/captures, and baked
sRGB albedo plus linear tangent normal/ORM for material samples. ORM packs
occlusion/roughness/metallic in R/G/B. F3D does not execute Godot `.gd`, `.tscn`,
`.gdshader` or Material Maker `.ptex`; use the owning exporter/baker, or a finite
Godot PNG capture for exact shader appearance. F3D lighting is an inspection
environment, not engine-shader equivalence.

Raw TRELLIS inspection also accepts `.post`, O-Voxel `.vxz` and the local
lossless float-field `.ovoxel` container when the matching F3D4 TRELLIS plugin
is installed. Register the original input as `kind:model`. Its per-preview
`ovoxel_mode` setting selects `surface` (default), `cells`, `voxels` or
`vertices`; the same choice
is under Preview Options > O-Voxel View. Cells show exact grid extents with diagnostic checker shading; cell centers
and decoded vertices use world-size markers that scale with zoom;
geometry-only fields use diagnostic clay. These imports do not fill holes,
remesh, reduce, UV-map or bake. A raw float snapshot needs the owner's
`ovoxel-pack SNAPSHOT ACTUAL_RESOLUTION OUTPUT.ovoxel` wrapper; an arbitrary
processed mesh cannot supply the earlier field. POST coordinates are Z-up;
O-Voxel XYZ and any explicit resolution are retained by its reader. An already
running older Preview needs a normal app restart to load the rebuilt backend.

For a flush window, paint strip or thin fixed panel, preview the actual host GLB
whose topology/material regions own that surface; joining coplanar duplicate
faces into one object is insufficient. A moving leaf stays separate and fits a
real aperture/rim. Raised seats retain purposeful contact without decorative
holes. Use the owner-produced host/material, edge and closed/open-leaf PNGs with
the exported model, rather than a generic material sample or ownership diagram.
The canonical `surface-ownership/v1` helper is in game-dev-tools' geometry-authoring
family; Asset Workshop's `constructs/surface-ownership` is the intended pinned caller.
Read their current specs, implementation/pin hashes and saved-readback tests before
claiming support. Details and proof-view requirements are in `docs/agent-flow.md`.
The viewer does not implement these geometry operations.

Watch declaration/recipe, source geometry, materials and the finite owner wrapper
or pin inputs; exclude generated models/receipts/images. Attach that build to one
model view and let the proof-image views watch its outputs. Publish validated
candidates atomically and their export/proof hash manifest last. Exact byte-bound
saved geometry/material/opening/leaf-fit checks are required alongside appearance;
native IDs, schema validity, Live/load status, triangle counts or UI overlays are
not geometry proof. Topology changes invalidate the affected UV/tangent/bake,
LOD, collision, motion and visual evidence until its owner rechecks them.

For a producer with a known export frame, use `add ... --settings
'{"up_axis":"y","materials":true,"textures":true}'` to declare its model view
atomically. Preserve other inspection choices. Source Z-up does not imply the
export is Z-up: Asset Workshop reference-modeling GLBs convert to Y-up/-Z-front.
Wrong remembered axes or clay mode can make a valid model appear tipped and
untextured. Inspect saved settings and source export conventions before blaming
geometry/material loss. `defaults 3d JSON` changes only future remembered defaults;
existing entries need their own requested correction. See
`docs/source-model-settings.md` for the public CLI and preservation test.
