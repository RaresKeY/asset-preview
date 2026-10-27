# Exported models

Source ownership: `src/model_view.{h,cpp}`, `src/model_surface.cpp`, `src/model_worker.h`, `src/f3d_compat.h`, `src/gltf_import.{h,cpp}`,
`src/gltf_pixels.h`, `src/texture_preview.{h,cpp}`, model loader in `src/viewer.cpp`,
triangle/dependency metadata and model paths in `src/service.cpp`.
Shared camera, comparison, publication and resource lifetime: [Preview](preview.md).

`kind:model` is the ordinary F3D-backed 3D registration. Unknown extensions route
here; actual support is determined by the installed native and Assimp readers.
GLB/glTF and OBJ are the preferred interchange outputs. The source cap is 256 MiB.
Loading a replacement creates a candidate engine first. Failure retains the
previous scene and does not advance successful loads/revisions.

Ordinary models and material samples use one shared native render thread. Lazy
backend loading, F3D plugin initialization, import, metadata, GL upload, rendering
and scene destruction all run there. On Linux/F3D4 each scene prefers EGL, avoiding hidden X11 windows, and falls
back to the installed offscreen context route if unavailable. Each scene owns
its offscreen GL context;
only completed image frames and scalar state cross to the QWidget surface.
Each visible surface has one replaceable frame mailbox. Queued reloads check a
revision and lifetime token; obsolete imports cannot publish. An importer already
running is not interruptible: unloading cancels publication immediately and queues
scene destruction after that finite operation. No engine is retained for reuse.
The thread is idle when no exposed scene is playing or has requested an update.
Component inspection retains its separate VTK backend and lifecycle.

Pending initial loads and scene-changing settings show a status spinner. Existing
phase timings include render-queue delay (`load_timing.queue_ms`) and the first
render; readiness is reported after the first frame. Camera/input/settings and
seek commands enqueue native work, so clients observing their results wait for
corresponding updated metrics/frames. Screenshots use the latest completed frame,
retaining the last good scene during a failed replacement.

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
Other scalar types retain the native array. There is no fill-result memoization,
extra decode, pixel-buffer copy, mesh grouping or new cache. Missing plugin
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

The render dispatcher yields between finite jobs, prioritizing existing-view
input, then refreshes, then new imports. Each view retains its own FIFO order.
Retiring a view removes its queued work and schedules prompt scene destruction.
An import already executing remains non-interruptible; it can delay another
scene's next frame, but does not block the GUI event loop. Thumbnail capture
also waits for pending commands, avoiding old pixels under a new visual key.
Coalesced frames and completion notices share one revision; a newer frame cannot
carry an older completion. Queued camera/playback requests discard superseded
revisions. Capture restores its live frame callback even when rendering fails.
`tools/test.sh async-loading` uses an inert worker with deterministic barriers
to check these races; `model-snapshot-gpu` checks native capture exception recovery.
