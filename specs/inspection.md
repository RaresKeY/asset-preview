# Optional 3D inspection and reviews

Source ownership: `src/inspection/`, inspection branches in `src/service.*` and
`src/viewer.*`, `bin/asset-preview`, the canonical preview skill, and
`tests/inspection_*`. This spec describes the implemented initial scope.

## Entry and lifetime

View > Inspect 3D and the model options menu enter an optional single-model
workspace. Return to Preview restores ordinary layout and the F3D backend.
The main toolbar also offers Preview, Part mode and Cut mode for a selected
model, including ordinary file opens and drops. Both modes show X/Y/Z cut sliders
above the component tools. Parts also exposes component tools; Cuts hides those
tools to focus on sections. Both use the same inspection scene and
retain selection, hidden parts and cuts while switching, without a backend swap,
reimport or new generator. A pending cut gesture flushes before changing tools,
returning to Preview or selecting another asset. A failed flush reports the error
and keeps the current mode and selection. The panel Return button, toolbar, menus
and protocol transitions share this commit boundary.
The choice between Parts and Cuts is per-entry session state, defaulting to Parts;
it is separate from saved inspection enablement and review data. Returning to
Preview preserves the session inspection view for reentry. Unsupported formats
or an unavailable optional backend disable Part/Cut choices with an explanation.
An already inspected model keeps Preview available even if its adapter is missing,
so it can return to ordinary rendering.
Entering does not raise a hidden window. A hidden registration retains only
configuration and review metadata. Exactly the inspected model is active while
the workspace is open; ordinary preview grids keep their existing behavior.

`asset-preview-inspection.so` loads only when a visible inspected model needs it.
The Qt executable does not link VTK, and metadata-only CLI reads need neither
the service nor the adapter. F3D releases its scene when switching to inspection;
the two backends do not retain simultaneous steady-state geometry for one entry.
Switching preserves camera and the entry's finite generator connection without
starting another builder. Hide/minimize/off-page destroys the inspection window,
importer, actors, textures, picker and markers. Libraries remain resident after
first use, following the existing global-registration policy.
Selecting another asset leaves inspection and publishes the selection in one
registry transaction. Views change only after that save succeeds. Registration
may retain an already active inspection configuration; entering the mode uses
`inspect open`, rather than setting `inspection.enabled` on an ordinary entry.

No inspection render/picking/polling timer runs while idle. Native input,
exposure, resize, explicit state changes and successful watched publication
request paints. Cut gestures coalesce through a 16 ms single-shot UI timer;
state/parts/comment reads do not render. Captures explicitly read back a frame.

## Backend and identity

The adapter uses installed VTK 9.7+ public glTF import, external OpenGL window,
scene hierarchy, actor references, cell picking and mapper clipping. It does not
access F3D internals or implement a shading stack. CMake builds it optionally,
using existing matching VTK headers/shared libraries, with
`-DASSET_PREVIEW_INSPECTION=OFF` to disable it. Disabling or missing prerequisites
leaves ordinary preview available. No package installation is performed.

Initial supported assets are static GLB/glTF 2.x scenes. Skins, animations,
morph targets and unsupported required extensions are rejected. Ordinary F3D playback supports
animated, skinned and morphing exports through [Animations](animations.md).
Inspection resources must be embedded or local paths. The source GLB/glTF and declared external resources
are hashed and checked again after import. A rejected or changing candidate
preserves the last displayed scene and revision.

Source metadata is bounded separately from the single imported geometry copy.
The public hierarchy's explicit `flat_actor_id` joins actors to source structure,
primitive counts, labels and world transforms. One component can own multiple
material actors. Repeated mesh instances retain separate selectors. An
ambiguous or unavailable identity is reported as unavailable, never guessed
from collection order or fuzzy names. Source labels prefer
`source_manufactured_label`, then `semantic_id`, then exported name.
Importer records are snapshotted once. Label, primitive count and child count
buckets reject impossible candidates; memoized exact comparisons retain matrix
tolerance, recursive topology and mutual uniqueness. Child groups require a
distinct exact counterpart for every source/imported child, using bounded
augmenting paths. Repeated child multiplicities cannot reuse one counterpart
and make otherwise distinguishable parent groups ambiguous. The one-million-pair,
one-million matching-step and 256-depth limits remain bounded. Exhaustion leaves all identities unavailable,
since an incomplete uniqueness check cannot authorize a partial match.

Opaque target IDs encode the source scene/node instance path and are exact only
within the accompanying revision. Notes retain readable labels and optional
semantic IDs for source lookup, without assuming that labels are unique or
semantic IDs exist on every part. L0/static detail is the intended review input;
batched LODs have only their actual exported component identities.

Lighting uses authored lights or VTK LightKit, with PBR importer limitations.
This is not F3D studio HDRI or engine-shader equivalence. The inspection options
omit unsupported ground-grid, axis-indicator and studio-lighting controls.
Materials/textures, edges, background, projection and camera controls remain.

## Interaction, cuts and peeling

A short left click selects the frontmost geometry and shows its exact source
name in the native panel. Drag orbit, pan, zoom, fit and cardinal views retain
their established input conventions. Picking observes hidden actors and cut
planes. Transparent surfaces are picked by their frontmost geometry. A mapped
surface hit supplies component-local position and normal for a pin; selecting a
tree item supplies a whole-part target. Model comments use an empty target list.

Hide, Show, Isolate, Undo and Restore All change visibility without reimporting.
Partial source mappings disable isolation that would pretend all geometry was
known. X/Y/Z controls move YZ/XZ/XY clipping planes in raw exported model
coordinates, with keep-positive/negative and enable/reset. Multiple cuts keep
the intersection. Each axis has a full-width slider paired with precise numeric
entry. Dragging or keyboard-adjusting a slider enables its plane and updates the
position live; numeric edits and side changes preserve its enabled state.
Focused sliders keep arrow, Home/End and Page Up/Down keys for position changes
instead of activating window navigation shortcuts.
Cuts are visual and open-edged; they do not modify geometry,
create caps, discover physical layers or invent modeled interiors.
Cut actions validate only their bounded plane state. Three retained axis planes
share one clipping collection, bound once per mapper and modified in place.
Moving a cut preserves camera, selection and hidden targets without reapplying
their state. Highlights update only the previous and new selected actors;
material changes still reset authored properties. Diagnostic metrics count
mapping comparisons, plane changes, mapper bindings and selection actor updates.

The part/comment lists are paginated and native Qt widgets. The panel preserves
unsaved text through selection, reload, unload and failed writes. Selecting a
comment displays its text; Restore view applies its saved camera, projection,
selection, hidden set and cuts only for an exact current binding. Unload/reentry
retains the session view. Revision changes clear unsafe selection/visibility/cuts
while retaining camera; old notes stay readable and are marked stale.
Backend switches use the current camera and projection. Hidden setting changes
override retained projection, and frame changes discard the old retained camera.
Applying a saved view validates supplied model/dependency revisions, field types
and numeric bounds before mutation. Applied cameras and projection persist per
entry; failed validation or registry saves preserve the scene and visibility
undo history. Active anchors and cuts retain only their supported fields.

## Durable review data

An explicit `inspection.review` absolute path selects a JSON document outside
generated model outputs and generator inputs. Without one, the default is
`reviews/<preview-id>.json` in the application's persistent state directory.
The path is exposed by inspect state and the panel. First Add creates it and
needed parent directories. Opening an existing review adopts its declared asset
ID/variant unless explicitly supplied; model path, variant and LOD must agree.
Live review paths currently bind absolute model paths. Offline reports can read
portable relative records, but relocating live reviews needs an explicit path
update. Remove/Close all preserve review files.

The canonical schema is `asset-preview-review/v1`: positive exact document
revision, asset identity/frame/model digest, annotations, and optional explicit
layer presets. Notes have generated UUIDs/UTC timestamps, author, text, status,
tags, targets, saved view and `created_against`. Viewer-created notes also bind
`dependency_revision` to external buffers/maps. Workflow status is separate from
live binding state: `exact`, `stale` or `unresolved`. Missing dependency identity
is unresolved. The viewer conservatively treats any changed model/dependency
digest as stale; it does not silently reproject a point onto rebuilt geometry.
Creation model and dependency bindings, including an absent dependency binding,
are immutable through note updates. A changed location needs a new verified note.
Canonical readers preserve non-NUL JSON control characters in source labels and
metadata; human-readable reports show controls as escaped data.

Comments bind the displayed last-good revision while a replacement is building
or rejected. Current exact open-note surface pins are bounded and drawn in the
inspection scene. A point remains review data, not a decal or topology change.

QtCore document mutations validate schema and limits, canonicalize the document
path for locking, reject file symlinks, hold QLockFile and compare expected
revision/optional exact-byte SHA-256 before atomic QSaveFile publication. Existing
identity and creation metadata survive patches. Corrupt files and failed saves
are preserved; established missing/corrupt reviews report an error with the last
known revision rather than masquerading as a new empty review. External edits
trigger review refresh separately from model generation. Saves do not rebuild or
reimport the model. Review text and linked data never infer executable commands.
Every review read rejects file symlinks, including cache hits. A successfully
reconnected registration with a different review path starts that path's own
metadata history; failed reconnections retain the previous path and revision.

Native mutations also return the validated published object with its exact-byte
digest. Cache installation still rereads bounded current bytes and hashes them;
only an exact match to an unmodified native snapshot skips repeated parsing and
schema validation. Cache stamps are captured before reading, and include device,
inode, size, mode, and nanosecond mtime/ctime. Unchanged stamps retain the existing
fast path and symlink checks; changed bytes receive full validation. External
same-revision edits, including preserved mtime, invalidate metadata and CAS.
Annotation lists compute binding only for returned page rows, preserving source
order, status-filter counts/offsets, exact binding and the response byte bound.

## Bounds and verification

GLB/glTF metadata: 16 MiB, 65,536 nodes, depth 64, 8,192 rendered parts,
4,096-character instance selector and 16 MiB serialized source inventory.
Individual model/resources retain the 256 MiB cap; decoded importer/texture
memory is not bounded by that file cap. No separate picker index is built.
Hidden sets allow at most 8,192 IDs/128 KiB. Parts pages allow 100 rows/240 KiB;
state and annotation responses stay below 256 KiB. Viewport markers allow 200
anchors. Review documents allow 16 MiB, 4,096 notes, 8 KiB UTF-8 text per note,
16 targets per note and bounded nested optional metadata. The service caches at
most the visible review and one explicit metadata query, rather than every
registration's complete comments. Existing private IPC and generator limits hold.

`tools/test.sh inspection-document` runs document corruption, CAS, concurrency,
atomic-write failure and bounds checks. `inspection-cli` checks offline no-start
reads, schema/output bounds and command mapping. `inspection-matching` compares bounded child matching with exhaustive
permutation references; the GPU optimization lane covers repeated coincident
child groups and preserves genuinely ambiguous child identities. `inspection-panel` checks
unresolved bindings and cancellation of queued cuts after a dependency change
without loading a renderer, plus slider bounds, numeric synchronization, automatic
enablement, tool-group visibility and pending-cut flushing when switching.
The ordinary GPU lane opens a fixture through the file-opening
route and uses native toolbar input to switch Preview/Cuts/Parts, checking actual
hardware rendering, selection/cut retention and no Parts/Cuts reimport.
`tools/inspection-check.sh`
provides functional and headless Gamescope GPU acceptance. The GPU fixture checks
source identities, clipping/visibility-aware picks, transformed local anchors,
native click versus orbit, captures, idle paints, camera-preserving backend
switches, last-good reload, stale notes, generators and hidden unloading.
The [integration checks](../tests/inspection_integration.py) record the actual
renderer and enforce these limits. Raw captures and runtime logs remain local.
Latency percentiles and a comparative memory benchmark are not claimed.

Automatic shell discovery, arbitrary plane gizmos, caps, animated mechanism
control, freehand marks and multi-model annotation are deferred. The durable
format can preserve explicit layer presets; initial UI peeling uses selected
parts and saved comment views rather than a layer-authoring editor.
