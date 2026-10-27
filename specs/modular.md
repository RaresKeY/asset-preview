# Modular assets and host assemblies

Source ownership: registration/generators in `src/service.cpp`,
`docs/agent-flow.md`, `examples/connect_surface_fixture.py`, and the canonical
skill's 3D asset reference. Rendering and source semantics: [Models](models.md),
[Scenes](scenes.md), [Images](images.md), [Animations](animations.md).

Modular is an asset workflow using ordinary model/image registrations. It is
not a separate loader, geometry constructor or persistent scene editor. Connect
the authoritative assembled host GLB for context and add separate module/proof
views only when they answer distinct inspection questions. Use stable IDs and
one finite owner generator per source/build, not a duplicate generator per card.

Preview actual host geometry for flush strips, fixed panels, openings, moving
leaves and raised contact surfaces. Shared material samples or diagrams do not
prove surface ownership or fit. Keep moving parts independently transformable;
export their baked node/rig clips to inspect motion. Static component review can
use [Inspection](inspection.md) for compatible files.

Publish validated exports and owner-generated material/edge/closed/open PNGs
atomically, with their hash manifest last. Watch source/recipe/wrapper inputs,
excluding generated proof outputs. The geometry owner verifies saved topology,
material assignment, dimensions, clearance and fit. Viewer IDs, triangle counts,
Live status, valid receipts and an attractive screenshot are not substitutes.
Topology changes invalidate affected UV/bake, LOD, collision and motion evidence.

Verification: the finite connector flow and the ordinary multi-preview
publication/lifecycle tests. Construction-specific geometry proof stays with
the owning exporter and its existing test/runtime wrappers.
