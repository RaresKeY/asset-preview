# 3D scenes and assemblies

Source ownership: F3D import and camera setup in `src/model_view.cpp`,
registration/dependency discovery in `src/service.cpp` and `src/dependency_metadata.h`.

A complete exported scene uses `kind:model`, with one authoritative export per
registration. Multiple meshes, node transforms, instances and imported materials
belong to that scene, rather than requiring a preview card per node. GLB/glTF is
the preferred scene interchange. Supported scene semantics remain reader-owned.
The viewer supplies its own fitted inspection camera and lighting choices;
it does not run a game scene, scripts, physics or engine-specific shader graphs.

Authored lights can participate in F3D lighting. Studio mode adds F3D's HDRI
ambient environment and its normal light handling; light-kit mode disables HDRI.
Use an owning-engine PNG/video capture for exact game shading or simulation.
Scene motion exported as clips follows [Animations](animations.md).

Each scene retains the [Model](models.md) source cap, candidate publication,
camera and visible-only engine rules. A scene counts as one grid entry even when
its geometry contains many instances. Dependency watching is bounded and best
effort where the reader can discover more resources than the shared manifest.

Verification: multi-node GLB fixtures, imported material/light checks, scene
triangle metadata and the static inspection integration tests.
