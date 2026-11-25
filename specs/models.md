# Exported models

Source ownership: `src/model_view.{h,cpp}`, `src/model_surface.cpp`, `src/model_worker.h`, `src/model_gpu.h`, `src/f3d_compat.h`, `src/gltf_import.{h,cpp}`,
`src/gltf_pixels.h`, `src/topology_import.{h,cpp}`, `src/texture_preview.{h,cpp}`, model loader in `src/viewer.cpp`,
triangle/dependency metadata and model paths in `src/service.cpp`.
Shared camera, comparison, publication and resource lifetime: [Preview](preview.md).

## 4 View

Preview Options > 4 View (Ctrl+Alt+Q) splits one ordinary model or material
preview into Perspective (upper left), Front (upper right), Right (lower left)
and Top (lower right). The orbit pane retains the existing projection choice;
its label reads Orthographic when that option is enabled. The three axis panes
are always orthographic and follow the model's front/up axes. Mouse wheel zooms
the pointed pane; dragging an axis pane pans it, while the orbit pane keeps the
normal orbit and pan controls. Fit and Reset fit all panes. Axis pan/zoom survives
ordinary reloads and toggling the layout during the visible view's lifetime;
unloading the view resets those temporary axis controls.

The boolean `four_view` setting is validated for model/material registrations,
persists per asset, and is excluded from shared 3D defaults. Existing `quad_view`
registrations/patches are migrated to `four_view`; the old name never controls
mesh topology. Parts/Cuts keeps its
single inspection view; return to Preview before changing 4 View. The window's
asset grid and project layout remain independent of this per-asset choice.

One loaded scene/context evaluates each animation pose once, then renders four
cameras. Native rendering allocates one transient half-size framebuffer per draw,
reuses it across the four passes and blits on the GPU. It adds no display pixel
readbacks or idle timer. Explicit snapshots include all four panes and preserve
the orbit camera; window captures also include the native pane labels. Metrics
report `four_view`, the targeted `input_pane` (0 orbit, 1 front, 2 right, 3 top),
and the three axis `axis_panes` with zoom, pan and world span.

## Source quads and edge topology

Preview Options > Edge topology offers **Quads / source faces** and
**Triangulated faces**; selecting either also enables the existing edge overlay.
E still toggles edges. `edge_topology:source|triangles` is per registration and
excluded from shared defaults. The default is `source`. 4 View is independent
and displays the chosen topology in all four cameras. Parts/Cuts retains its
existing native edge controls; return to Preview to change edge topology.

The F3D4 native reader adapter preserves OBJ and PLY polygon arrays for source
edges, including genuine quads and n-gons. Triangle mode runs VTK's triangle
filter on those arrays and removes edge-suppression flags so diagonals appear.
OBJ retains its native material/texture importer and actor grouping. PLY retains
reader scalar colors, normals and UVs. There is one source import; no guessed
triangle pairing, remeshing, second source import, builder or timer is added.
Changing topology loads a candidate while retaining camera and last-good content.

`metrics.topology` records original face, quad, n-gon and tessellated-triangle
counts before the optional triangle filter, with the applied `edge_topology`.
The count badge adds quads when present (for example `6 quads · 12 tris`), with
exact counts in its tooltip. PLY also obtains the triangle badge from that import.
GLB/glTF surfaces expose triangle primitives and report zero original quads;
quad topology lost during export is not reconstructed. Other format readers
keep their existing source-edge behavior; explicit triangle mode supports only
OBJ, PLY, GLB/glTF and generated material samples.

The adapter uses the existing optional F3D4/VTK SDK plugin route, including
installed `vtkIOPLY`; it is available in the qualified host build. Builds without
that SDK route retain native source edges and existing triangle metadata, but
have no imported quad counts. Explicit OBJ/PLY triangle mode reports an error
there, retaining the last good scene. The packaged F3D3.5 route is not newly
claimed to support this adapter.

## Model loading and rendering

`kind:model` is the ordinary F3D-backed 3D registration. Unknown extensions route
here; actual support is determined by the installed native and Assimp readers.
GLB/glTF and OBJ are the preferred interchange outputs. The source cap is 256 MiB.
Loading a replacement creates a candidate engine first. Failure retains the
previous scene and does not advance successful loads/revisions.
Reconnecting an active ordinary model/material with the same configuration and
no generator command checks current publication fingerprints as usual. A fresh,
complete publication is retained unchanged; stale or incomplete coverage queues
a reload on the existing visible surface/worker. It keeps the last good scene,
camera and per-view counters while replacement content imports. It does not
retain any off-page engine or weaken the fresh-source/publication checks.
Tests distinguish immediate reload admission from completed publication, verify
same-size external geometry replacement before watcher debounce, incomplete
manifest discovery, failed replacement retention and successful recovery.

Ordinary models and material samples use one shared native render thread. Lazy
backend loading, F3D plugin initialization, import, metadata, GL upload, rendering
and scene destruction all run there. Each visible scene owns one OpenGL context.
Imports use a GUI-created offscreen surface; F3D's external-context API draws live
frames directly into an embedded native QWindow, and the worker swaps its buffers.
Pose evaluation and rendering use one native window context activation per frame.
Camera input and metadata publication do not activate the offscreen surface.
Imports, scene-changing settings, explicit seeks, captures and destruction own
their required context activation and release it when finished.
There is no display texture pool, shared display context, GUI blit or per-frame
CPU readback. Window exposure and destruction synchronize with drawing through
a surface lifetime lock; the context is released before the lock is dropped.
Imports never hold that lock. Native window events and surface creation/destruction
stay on the GUI thread; context and engine cleanup stay on the worker.
Native overlays retain mouse/keyboard access above the embedded window.
Drawing explicitly selects the drawable color buffer, including on EGL. Native
windows request opaque color buffers and clear only alpha after rendering because
F3D background alpha can be zero even on native EGL configurations.
The worker interface publishes render metadata without accepting image frames.
The headless Qt test route renders through F3D's offscreen context without a
pixel-transfer fallback (`frame_transport:offscreen`). Only explicit snapshots
and finite thumbnails read images back; they never invoke live frame/completion
callbacks or advance the animation pose. `capture_readbacks` counts backend
snapshot readbacks separately from live frame presentation.
Queued reloads check a
revision and lifetime token; obsolete imports cannot publish. An importer already
running is not interruptible: unloading cancels publication immediately and queues
scene destruction after that finite operation. No engine is retained for reuse.
The thread is idle when no exposed scene is playing or has requested an update.
Component inspection retains its separate VTK backend and lifecycle.

Pending initial loads and scene-changing settings show a status spinner. Existing
phase timings include render-queue delay (`load_timing.queue_ms`) and the first
render; readiness is reported after the first frame. Camera/input/settings and
seek commands enqueue native work, so clients observing their results wait for
corresponding updated metrics/frames. Screenshots render the current pose into a
temporary capture framebuffer on the worker; failed replacements retain the last
good scene. Explicit screenshots can wait for current worker work. Departure
thumbnails enqueue a finite render at at most 160×100 before scene destruction,
without blocking navigation behind another import. Publication checks the current
thumbnail identity, original registration and visible, nonminimized window. Cache
clearing and hiding invalidate queued captures, including across a later Show or
registration ID reuse. No capture framebuffer survives the request.
Metrics report `frame_transport:native_window`, zero application display buffers,
`presented_frames`, `presented_serial`, `display_readbacks` and readback bytes.
Presentation counts indicate worker buffer swaps, not physical scanout.

On F3D4 the model backend also loads the separately installed
`libf3d-plugin-trellispost.so` for `.post`, `.vxz` and `.ovoxel`. It searches
beside the application first, then `~/.local/lib/f3d/`. Missing/incompatible
readers report an import error. Ordinary formats retain their native readers;
packaged F3D3.5 does not claim compatibility with this F3D4 plugin.
The separately installed reader owns the decoder and bounded binary contracts;
this viewer does not modify those input formats.

O-Voxel View offers Extracted raw surface, Raw voxel cells, Occupied cell
centers and Decoded vertex positions. The validated
`ovoxel_mode:surface|cells|voxels|vertices` setting is per registration, persists
with it, and is excluded from shared 3D defaults. Setting changes load a new
candidate while retaining the camera. The reader option is set immediately
before each serialized import so simultaneous previews retain their own mode.
VXZ base colors use direct sample colors; geometry-only float fields use
diagnostic clay. Backfaces stay visible because FDG does not encode orientation.
Cell-center and decoded-vertex markers use per-point world-space radii of
0.3 and 0.18 cell respectively, with identity `rotation` and `scale` attributes
and F3D absolute splat sizing. They grow with zoom independently of occupied
bounds. Cells cover their exact grid extents and draw only exposed faces of
the occupied-cell union; shared interior faces are omitted. No occupancy is
added or sampled. Geometry-only cells have diagnostic checker shading; VXZ
base colors remain unchanged. The cell view rejects more than 8M exposed faces
before allocating its boundary geometry.
VXZ without geometry attributes falls back to occupied points. Reader imports
perform no hole filling, welding, remeshing, reduction, UVs or baking.
Existing `materials:false` still selects clay. Triangle metadata is unavailable
for O-Voxel rather than inventing a source triangle count.

`tools/test.sh ovoxel` checks settings validation/isolation; `ovoxel-gpu` uses a
private real service for POST, VXZ/float-field rendering, source preservation,
mode changes, world-size marker zoom, failed replacements, and hidden-engine cleanup. Optional
`ASSET_PREVIEW_OVOXEL_MODEL` adds a saved real float field. The workstation's
direct binary test route needs the same private F3D runtime library path as the
CLI launcher when the installed OpenSubdiv dependency lacks its CUDA13 SONAME.

Host builds accept installed F3D 4.x or 3.5. The compatibility wrapper maps the
4.x animation/format-support APIs and FXAA option to the existing behavior;
packaged 3.5 builds retain their API path. With F3D 4 and matching public VTK
development files, the lazy F3D module also registers a glTF reader through the
installed plugin SDK ABI. It subclasses F3D's native glTF importer and shares each
decoded AOS unsigned-byte image buffer through a small array wrapper. The only
changed pixel operation is component filling: tuple count and stride are computed
once outside the loop. Values, other channels, source files, material declarations,
mesh/node identities and native scene/animation/shader handling are preserved.
Other scalar types retain the native array. The byte-fill wrapper itself adds no
fill-result memoization, extra decode, pixel-buffer copy, mesh grouping or cache;
the separate PNG preparation and static batching passes are described below. Missing plugin
prerequisites/loading failures retain the ordinary native reader.
`-DASSET_PREVIEW_GLTF_BYTE_FILLS=OFF` explicitly builds that reference/fallback
path for matched checks. It does not change texture quality or asset bytes.

The plugin and libraries stay resident with the existing lazy backend policy;
image wrappers/buffers belong only to the visible imported scene and unload with
its engine. Parts/Cuts continue importing the authoritative original independently.
`metrics.gltf_import` snapshots plugin availability, array views, byte component
fills and filled texels for the successful load. `metrics.load_timing` reports
finite wall-clock phases for engine/options, preparation, import, metadata,
animation, camera/publication, total load and first render. These are CPU-side
elapsed clocks, including synchronization; first render is not a GPU timestamp or
complete service-start latency. Neither diagnostic schedules timers or renders.

Orbit, pan, zoom, fit/reset, cardinal views, projection, axes, grid and source
up/front conventions use the shared camera contract. Shading supports authored
materials/textures or clay/textures-off inspection. Triangle counts describe
source primitive metadata, not GPU work or geometry correctness. Camera state is
per registration and survives same-kind reconnection and unloading.
Successful publication records the actual fitted/restored camera from the first
displayed frame into the ordinary registration, even before the first manual
camera operation. Reopening an untouched view therefore restores its displayed
framing rather than fitting again from a different constructor viewport. This
is publication metadata; it does not synchronize another comparison pane or
write the registry on every frame. Inspection keeps its separate camera state.

`texture_quality` defaults to `original`. The model menu's Fast GLB preview
option (`preview`) bounds embedded PNG/JPEG textures to 1024 px while preserving
aspect ratio. It is an explicit appearance tradeoff for faster ordinary preview;
Parts/Cuts always import the original model. Cameras, triangle metadata,
generators, watches, review paths and annotation revision identities continue to
refer to the original. Material samples ignore this shared 3D preference.

Eligible inputs are self-contained GLB 2 files up to 64 MiB, with one embedded
buffer, at most 64 PNG/JPEG images and supported extensions. Images must fit
4096 by 4096 pixels. External resources, unknown/texture extensions, unsupported
images, malformed data and cache failures fall back to the original loader.
Resized textures use 8-bit channels; high precision images selected for resizing
fall back to the original. RGB and alpha are filtered independently, preserving
RGB beneath transparent pixels for normal/ORM and opaque-color texture use.
Only image references and the buffer length change: new PNG buffer views are
appended after the entire original BIN chunk, preserving geometry, UVs, nodes,
instances, materials, samplers and animations. Sources are never rewritten.

Preparation runs synchronously only for a visible model requesting textures.
The private `texture-previews-v1` cache under Qt's application cache location is
content-addressed by complete source bytes, algorithm and Qt version. Atomic
files and hashed manifests detect partial/corrupt entries. A nonblocking cache
lease covers preparation and importer reads; contention uses the original.
Entries are capped at 96 MiB, total derivative/manifest storage at 224 MiB and 2,048 files,
with eviction only on explicit use. Space for both files is reserved before
publication. The bounded scan can recover previously overfull caches of small
revisions instead of permanently disabling hits. Cache roots/files reject symlinks and foreign ownership; exact
versioned filenames and an ownership marker limit deletion. Tests may override
the dedicated root with `ASSET_PREVIEW_TEXTURE_CACHE`. No decoded cache pixels,
engines, background producers or cleanup timers are retained by this derivative
cache. Separate small captured thumbnails follow [Cache/overview](cache-overview.md). Metrics expose
actual quality, cache hit, source hash, preparation cost and fallback reason.

A static file has no playback bar and no recurring rendering timer. Discovered
clips enable [Animation](animations.md) controls without requiring a new kind.
Complete scene exports follow [Scenes](scenes.md); kit/host proof workflows follow
[Modular assets](modular.md). The optional component [Inspector](inspection.md)
remains limited to supported static GLB/glTF scenes.

Verification: model import, camera, material restoration, last-good publication,
hover/navigation and idle tests in `tools/test.sh gpu`. `texture-preview` checks
derivative integrity, corruption, invalidation, quota and fallback. The
`optimization-gpu` lane checks original inspection identity and unload/reopen.
`gltf-import` compares native and optimized fills over independent buffers and
checks shared-buffer lifetime. The GPU lane includes a repeated textured model's
default full-resolution import, original Parts/Cuts identities, mode switching,
camera preservation and hidden release. `tools/profile_model.py` drives finite,
private-service matched city runs through `tools/offscreen.sh`; perf attribution
is kept separate from clean timings. Raw model benchmarks remain local because
they record asset paths and process details; compare exact binary/model hashes
and completed workloads when reproducing them.
`tools/test.sh wayland-gpu` additionally captures actual KWin compositor output
through installed Spectacle, compares scene pixels with explicit captures, and
checks camera changes, materials, Overview return, reopening and a mixed grid.
Texture captures and `frameSwapped` alone do not prove visible presentation.

The render dispatcher yields between finite jobs, prioritizing existing-view
input, then refreshes, then new imports. Each view retains its own FIFO order.
Already queued orbit/pan, wheel, release and synchronized-camera commands for
one view run in a batch before its pending frame callback. Each batch stops at
the next same-view non-motion command, 256 commands or 4 ms, preserving exact
motion order/deltas and giving other scenes and timers a turn. It does not
render and read back every obsolete intermediate pointer position. Priority
selection scans the queue linearly rather than rescanning it for each view.
Full metrics publish once per motion batch. Frame and motion publication do not trigger
playback widget updates; explicit commands and throttled playback notices do.
World-up orbit evaluates each pointer delta and pitch clamp in order in a pending
camera state. F3D's scene-wide clipping update runs once when that state is
committed, rather than once per obsolete intermediate pointer position. Camera
reads (including metrics and persistence), rendering/capture, motion transitions and other
camera operations commit pending orbit first. Explicit restore replaces it;
engine replacement and teardown clear it. Batch limits and FIFO barriers remain
unchanged. Consecutive pan events accumulate camera-axis translation while orientation,
distance and viewport height remain unchanged. Their F3D `pan` call runs once at
the next camera read/frame or motion barrier; each raw pan otherwise repeats a
scene-wide clipping update. Switching orbit/pan, scrolling, settings, resizing,
fit/views, restore and captures preserve command order and commit or replace the
pending state. Replacement/teardown discard it. Pan accumulation is fixed-size
view-owned state, not a cache or another timer. Free orbit retains its existing
F3D camera operations.
Non-persistent drag camera notifications publish with rendered frames. Release still saves the exact final
camera; synchronized comparisons follow those frame notifications.
Retiring a view removes its queued work and schedules prompt scene destruction.
An import already executing remains non-interruptible; it can delay another
scene's next frame, but does not block the GUI event loop. Thumbnail capture
also waits for pending commands, avoiding old pixels under a new visual key.
Delayed output-watch notifications retain an import already loading the same
complete output stamps; different outputs and changed generator inputs still
supersede it. Failed-import retry timers wait while another load is active.
`tools/test.sh model-reload-publication-gpu` holds a fixture import across both
events and verifies one recovery import and supersession by a newer output.
Coalesced frames and completion notices share one revision; a newer frame cannot
carry an older completion. Queued camera/playback requests discard superseded
revisions. Capture restores its live frame callback even when rendering fails.
GUI notifications retain their callback while it runs and recheck view lifetime
and revision after callbacks that may navigate away, delete or reload the view.
`tools/test.sh async-loading` uses an inert worker with deterministic barriers
to check these races and an artificial slow-frame motion burst across orbit,
pan, wheel, Fit and release; `model-snapshot-gpu` checks native capture exception
recovery and eager-versus-batched camera/pixel parity across pole clamps, long pan bursts and resizing,
zoom, Fit, named views, reset, restore and settings on Y/Z-up perspective and
orthographic cameras. An intentionally changed input must fail camera parity. The Overview GPU
departure test checks bounded capture before scene destruction without a new
thumbnail importer, and hidden cache release.

## Embedded PNG import

On VTK 9.7+, the SDK reader's document-loader preparation hook decodes eligible
embedded PNGs with the same native `vtkPNGReader`, using at most four finite
workers (also bounded by hardware concurrency and unique-image count). Eligibility
requires 16–2048 images, all embedded 8-bit PNGs, valid buffer ranges, dimensions
up to 4096 per axis and at most 2 GiB of aggregate RGBA8-equivalent pixels. Other
inputs retain native serial loading, including JPEG, external images and 16-bit
PNG. Older VTK keeps its native loader.

SHA-256 buckets plus exact encoded-byte comparisons identify duplicates within
one import. Each unique PNG is decoded once; duplicate image slots get deep
copies before material setup because native ORM/alpha processing can mutate
pixels. No image slot, texture index, sampler, material, geometry or source file
is rewritten. No texture resizing, GPU texture sharing or retained decode cache
is introduced. Readers and threads belong only to preparation; all workers join
before publication. A decode failure leaves all model images untouched and
rejects the candidate through the existing last-good-scene boundary; it does
not retry a malformed stream through native loading. Hidden teardown is unchanged.

`gltf_import.image_decode` reports images, unique decodes, worker count and
preparation/total elapsed milliseconds, or an empty object on the native path.
`ASSET_PREVIEW_DISABLE_GLTF_IMAGE_DECODE=1` selects that reference path for matched
fresh-process checks. `tools/test.sh gltf-image-decode-gpu` compares exact native
captures for repeated mutable-role textures, distinct images, external sources,
16-bit PNG and small-input fallback, with source preservation and hidden release.
A truncated PNG also verifies rejection while retaining the last-good scene.
The native integration tests compare local fixture pixels. Representative
readiness timings and measurement boundaries are in [performance](../docs/performance.md).

## Static glTF material batches

The F3D4 SDK reader groups eligible static ordinary-preview actors by authored
material and exact point/cell attribute schema, including active attribute roles.
At 32 or more imported actors, multiple eligible actors become one native
`vtkAppendPolyData`/mapper per group. Points, normals, tangents, UVs, colors and
cells remain distinct; there is no welding, simplification, UV change, texture
resize or file rewrite. Native material/texture properties come from the group's
original actor. The original source path, source hash and triangle metadata remain
authoritative. Parts/Cuts use their separate original-file VTK importer and do
not use these display batches.

Eligibility requires identity world transforms, triangle primitives, float
positions, matching named attribute schemas and opaque source materials.
Transparent/masked materials, morph targets, other primitive types, other point
precision and transformed actors stay native. Any source animation or skin
excludes the whole file, including paused animations: VTK's internal animation
list is incomplete during ImportActors, so this check reads the source JSON
header separately (16 MiB cap, conservative fallback). Small files and missing
SDK dependencies retain the native importer. Stream-only inputs without a
readable filename also stay native.

Preparation occurs once during native import and adds concatenated geometry
owned by that scene. It adds no retained scene cache or recurring timer. The
original native scene graph remains intake metadata; ordinary rendering is not
a per-part editing API. Supported structural selection, source identities and
cuts remain owned by the independent inspection adapter.

Metrics `gltf_import.source_actors` and `render_actors` report native collection
counts before/after preparation, not GPU draw calls. Internal diagnostic env
`ASSET_PREVIEW_DISABLE_GLTF_BATCHING=1` retains the original actors for fresh
process A/B checks. `gltf-batching-gpu` checks native-vs-batched exact pixels,
triangle/source preservation and ten eligibility/fallback cases. It includes an
animated file whose native animation metadata is not ready at ImportActors time.
Real convoy comparisons additionally allow tiny edge/depth tie changes caused by
opaque draw ordering; camera equality and pixel error are recorded separately.
