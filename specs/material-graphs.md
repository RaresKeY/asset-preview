# Material graph previews

Owning sources: `src/material_graph.{h,cpp}`, graph routes in `src/service.cpp`,
`bin/asset-preview`. The standalone Material Atlas workbench builds the same
native sources; no terminal or authoring tool is added to this service.

`add FILE --kind graph` registers a Material Maker `.ptex` or compatible JSON
graph (`nodes`, `connections`). Auto kind recognizes `.ptex` and
`.materialgraph.json`. Registration preserves existing window, selection,
generators and background behavior. Ordinary preview visibility ownership
applies: only active visible entries decode thumbnails or own an F3D engine.

Nodes use actual name/id, type, parameters, image paths, edges and socket indices.
Layout is generated from the DAG; nodes cannot be dragged. Output sockets
connected to `material` nodes are represented as channel preview cards using
saved output maps. ORM channel cards show R occlusion, G roughness, B metallic.
Shader internals are opaque. Click/Enter/Space inspects a node; double click,
`+` or E expands it. Ctrl+wheel zooms; Fit graph fits all nodes. Refresh keeps
selection, expansion and canvas transform. Hover text identifies the node,
state and description. A resizable right sidebar splits vertically: the upper
pane holds the shaded material or selected saved map, and the lower pane holds
the selected node's thumbnail and parameters. Compact charcoal surfaces, small
corner radii and dense controls apply to the shared graph presentation.
Compact cards hide the sidebar and release its shaded renderer; graph
thumbnails, hover information and expandable nodes stay available. Embedded
graphs reserve room for the owning card's title/close controls.

The preview target selector offers sphere, cube, plane and `Open model…` for a
UV-mapped GLB. Saved maps are assigned across all of the selected model's
primitives in a temporary GLB. The original file, topology, UVs, transforms and
existing binary data remain unchanged.
Original vertex colors are excluded from the temporary assignment so they do
not tint the replacement finish. Missing tangents are generated for
normal-mapped triangle meshes with ordinary float positions, normals and UVs.
Unsupported/missing UVs, compressed geometry and malformed inputs fail visibly.
This material inspection does not evaluate engine-specific material graphs.
The chosen model is watched for saves; there is no persistent export or poller.

Graph-only settings `preview_model` (absolute `.glb` path, or empty to clear)
and `preview_shape` (`sphere`, `cube`, `plane`) allow CLI registration and persist
operator target changes in the existing registry. A missing model remains
waiting until its file is created. UI choice and sidebar split survive graph
refreshes; the Atlas workbench also saves them across minimize/reopen. Metrics
include `preview_target`, `inspector_side`, `inspector_visible`, and the nested
renderer's successfully loaded `material_model` path.

`FILE.live.json` is optional agent progress: status, maps, node overrides and
at most 500 operations. The material-owning project's publisher is responsible
for durable progress and atomic saves. Relative paths resolve beside the graph;
`%PROJECT_PATH%` also expands to that directory. Atlas material conventions are
inferred from its nearest `project.godot` and `assets/materials/`; explicit
sidecar maps override inference. Dependencies include graph, sidecar, input
previews and output candidates, including missing files so creation can refresh.

The parser bounds JSON files at 4 MiB, original nodes at 256 and connections at
1024. Invalid JSON, duplicate IDs, bad edges or cycles fail visibly while the
last valid graph remains. Missing images clear old thumbnails. Reading graphs
never evaluates shader code or recorded commands. Thumbnail decoding is bounded
and images are cached only while the graph view is alive. Static graphs do not
poll or continuously repaint. Metrics expose node/edge count, graph revisions,
selected and expanded nodes, decoded previews and the nested material renderer.

Verification: `tools/offscreen.sh python3 tests/material_graph_integration.py`
runs a private service and checks background registration, auto detection,
watched progress/map saves, GPU material preview, capture, compact-card renderer
release/restoration and suspension when hidden. It never touches the normal
preview registry. Current native module capture: `evidence/material-graph/`.
It also exercises graph model settings, source preservation and watched model
replacement. `tools/test.sh material-model` verifies the temporary assignment's
geometry/transform preservation, normal tangents, URI encoding and rejection
of invalid or unmapped models without loading OpenGL.
