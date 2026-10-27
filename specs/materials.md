# Baked material samples

For saved recipe connections and live authoring progress, use the separate
[`graph` preview kind](material-graphs.md). It shares the shaded saved-map
renderer described here and does not execute procedural shaders.

Source ownership: generated sample meshes and options in `src/model_view.cpp`,
temporary GLB model assignment in `src/material_sample.h`, material CLI in
`bin/asset-preview`, registration/maps in `src/service.cpp`.
Shared native rendering/lifetime: [Preview](preview.md).

`kind:material` displays a baked albedo map on the selected sphere, cube or plane,
with optional normal and ORM maps. These generated meshes carry normals and
triangulated UV faces. Sphere caps omit collapsed pole triangles (2,208 real
triangles). Samples enter F3D as a temporary GLB with explicit UV-derived,
orthogonal tangents and handedness; `f3d::mesh_t` alone cannot carry tangents.
The former mesh-only path produced missing upper bands and jagged shading when
normal textures were enabled. Removing degenerate caps alone did not repair it.
The file is removed after import; the file-based route also supports the
packaged VTK 9.4 backend's lack of automatic glTF memory-stream detection.
The GLB authors the albedo as an explicitly opaque, double-sided material;
albedo is not supplied through F3D's color-texture override. F3D 3.5 forces any
RGBA override into its translucent pass regardless of actual alpha values,
disabling depth writes and allowing rear sphere triangles to overwrite the
front at cardinal camera angles. Tangents alone did not repair that failure.
These baked surface samples use opaque shading. Albedo is sRGB; tangent normal
and ORM are linear.
ORM channels are occlusion, roughness and metallic, with the latter multipliers
set to one when an ORM map is present. Scalar roughness/metallic options apply
to the no-ORM path. Maps participate in output watching and last-good publication.

The sample uses F3D's inspection environment. It does not execute Material Maker,
Godot or other native material graphs. Use the owning baker/exporter, then
connect saved baked maps. An engine capture is the appropriate reference when
the graph's exact shader behavior matters. Surface placement and modular host
fit belong to [Modular assets](modular.md), not the generic material sphere.

The graph inspector can supply an internal `sample_model` instead of a built-in
shape. `material_sample.h` assigns the saved PBR maps in an ephemeral GLB copy,
preserving the original binary data and hierarchy, and appends UV-derived
tangents where required. It accepts self-contained GLB 2, at most 256 MiB with
4 MiB metadata, and reports unsupported geometry/UVs explicitly. The nested
renderer reports the actual source model's triangle count and only publishes
`material_model` after a successful import. Ordinary material samples and model
registrations retain their existing defaults.

Materials retain the shared model/material camera and display preferences and
256 MiB file bounds. A generated sample has no animation clips or playback timer.

Verification: material-map save refresh, sample UV/normal appearance, shading
restoration and renderer/idle checks in `tools/test.sh gpu`.
`PreviewIntegration.test_material_sphere_flat_normal_preserves_surface` compares a grid
of surface pixels using opaque RGBA albedo and a flat normal against an RGB
unmapped reference in front, right, back, top and isometric views, then requires
a tilted normal to change shading. The RGBA fixture's path contains a space to
verify external image URI encoding. Load success and
triangle counts alone do not establish valid normal-mapped appearance.
