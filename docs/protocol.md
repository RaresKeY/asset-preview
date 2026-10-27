# Local protocol, version 1

The optional model inspection route uses `method:"inspect"`, an existing model
`id` and `action`. `open` accepts `review` (absolute JSON path), `asset_id` and
`variant`; it selects that model for inspection without raising the window.
`close` returns to ordinary preview. `state` is read-only and reports loaded
state, exact model/dependency revisions, selected source target, cuts/visibility,
review path, document revision and exact-byte digest. Hidden views stay unloaded.
See [the inspection spec](../specs/inspection.md) for backend and identity limits.

| Inspection action | Fields | Behavior |
| --- | --- | --- |
| `parts` | `offset`, `limit` (1 to 100) | Paginated current source identities, requires visible inspection |
| `select` | `target` | Select exact opaque target ID from `parts` |
| `hide`, `show`, `isolate` | optional `target` | Change target/current selection visibility, no import |
| `restore` | `all:true` or `last:true` | Restore all visibility or undo the last operation |
| `cut` | `axis`: x/y/z, finite `position`, `keep`: positive/negative, boolean `enabled` | Update one model-space clipping plane |
| `apply` | `view` object | Restore a validated current-revision camera/selection/cuts/visibility snapshot |

`method:"annotations"` uses preview `id` and `action:list|add|update|remove|restore`.
`list` accepts bounded `offset`/`limit` and optional workflow `status`, returning
`items`, `total`, `has_more`, `document_revision` and `review_digest`. Each item
has separate live `binding` status. Metadata reads need no geometry load.
`add` takes `note:{text,status,tags?}` and binds the current source selection,
picked surface anchor and view. Explicit `targets:[]` chooses a model comment.
`update`/`remove` use `note_id`; update supplies a note patch. All mutations require
`expected_revision` and optionally `expected_digest`. A missing file is created
only by Add with revision zero. Normal responses return current compact state;
Add also returns the new note when it fits the response budget, otherwise its ID.
`restore` uses `note_id` and refuses stale/unresolved view attachments.

Inspection state and note pages are bounded below 256 KiB. Larger inventories
need pagination. Review collections are separate 16 MiB documents, not fields
inside the existing 8 KiB registration. Optional registration `inspection` holds
only `enabled`, `review`, `asset_id` and `variant`, never note text or geometry.
It is per-model, not a remembered default. Review-only file changes refresh
comments without running the finite model generator.
Use `inspect open`/`close` for mode transitions. Registration can retain
`enabled:true` only for the model already being inspected; new registrations
may provide review metadata with `enabled:false` or omit the flag.
Saved-view application rejects mismatched supplied model/dependency revisions
and malformed fields without changing the view or its undo history. Note
updates preserve creation model/dependency bindings.

The standard-library CLI exposes `inspect` subcommands and offline
`annotations list FILE` / `annotations export FILE --format markdown`.
Offline reads/export validate bounded JSON and do not start the service or GPU.

Unix stream socket, printed by `bin/asset-preview paths`. Socket and its parent
directory are private to the current user. One UTF-8 JSON request and one JSON
response, each followed by newline. Maximum request 1 MiB; incomplete connections
expire after 5 seconds. Responses contain `ok`; failures contain `error`.
CLI client timeout is 15 seconds. There are at most 64 connected clients, 256
registrations, 8 KiB per configuration, 128 extra input paths and 128 command args.

`open` validates every path and the registration limit before adding previews.
It preserves an existing registration rather than replacing its configuration.
Missing outputs are allowed, directories and relative paths are rejected. New
entries retain input order. Additions and final selection are saved in one
atomic registry publication before live views change. Save failure rolls back
the entire batch. Reopening an unchanged selection does not rewrite state. The CLI also
accepts bare file paths and expands relative filenames before sending this request.

| Request | Fields | Behavior |
|---|---|---|
| `ping` | — | Protocol version, PID and actual Qt platform |
| `list` | — | Entries, live metrics, selection, layout, grid_size, preferences, theme, effective_theme, close_behavior, close_target, preload status, presentation, active count, watches, Qt platform |
| `theme` | optional `mode`: `auto`, `dark`, `light` | Read appearance choice or save/apply it; returns `theme` and `effective_theme` |
| `close_behavior` | optional `mode`: `left` / `last` | Read or atomically persist the close destination preference; returns `close_behavior` and `close_target`. Default `left`; `last` means last viewed survivor, with neighbor fallback |
| `add` | `entry` object; optional boolean `make_current` (default true), `project_label` | Add/replace and atomically choose the project current preview; no global focus/selection change |
| `project` | `action`: `upsert` (default), `list`, `remove`; `id`, optional `label` / `current` | Create/name group, choose existing member, list groups or remove an empty group |
| `open` | `paths`: array of 0 to 256 absolute file paths | Reuse the first registration for each normalized path or add a new auto-kind preview, select the last, and show the window; empty array only shows |
| `remove` | `id` | Stop/unload and unregister; selected removal shows the configured left/last-viewed survivor; preserve files |
| `remove_many` | `ids` | Atomically close 1..256 registered IDs (duplicates collapse); validate/save before unloading, preserve files, skip closed entries for the close destination |
| `close_all` | — | Close/unregister all previews and stop their generators; retain files, window, layout and preferences |
| `select` | `id`; optional boolean `make_current` (default false) | Change visible preview/page without raising window; `make_current:true` atomically promotes the grouped preview and selects it in one save |
| `layout` | optional `layout`: `single` / `grid` / `overview` / `projects`, `grid_size`: integer 2 / 3 / 4; `project_grid_size`: 0 (auto) / 2 / 3 / 4; `group_projects`: boolean; `project_filter`: empty / ID / `@ungrouped` | Persist layout; live grids activate at most 16 entries, thumbnail overview activates none; Projects activates current previews in intersecting panes |
| `zoom` | `id` | Atomically select the preview and switch to Single view; does not raise a hidden window |
| `cache` | optional `action`: `stats` / `events` / `clear`; `events` accepts integer `limit` 1..128 (default 64), nonnegative integer `after` (default 0) | Bounded cache/churn metrics, recent event tail or RAM/small-derivative clearing |
| `settings` | `id`, `settings` object | Validate, merge and persist entry options and remembered defaults |
| `reload` | `id` | Reload or rebuild only if currently visible |
| `animation_step` | `id`, numeric `direction` (-1 or 1) | Pause and step a visible animated model using fresh native state and the configured frame rate |
| `seek` | `id`, finite nonnegative numeric `seconds` | Seek visible audio/video, or an animated model within its current clip |
| `show` | — | Open/raise the single app window |
| `hide` | — | Suspend all entries and hide the window |
| `quit` | — | Stop owned generators, close socket and exit |
| `capture` | new absolute `path` | Save currently visible window as PNG; refuse overwrite |

Example registration:

```json
{"method":"add","entry":{"id":"chair","label":"Chair","kind":"model","path":"/absolute/chair.glb","watch":["/absolute/build_chair.py"],"cwd":"/absolute/project","command":["/absolute/project/build-chair.sh"],"timeout":120}}
```

Entry fields: `id` (optional, otherwise allocated), `label`, optional `project` ID, absolute `path`, `kind`
(`auto`, `image`, `audio`, `video`, `model`, `material`, `graph`), absolute file `watch` list, generator `cwd`,
argument-array `command`, timeout seconds and `settings`. Material entries also
have `maps` containing absolute `normal` / `orm` file paths. Files may be missing;
generator cwd must exist at registration. No shell, URI execution, TCP listener,
arbitrary plugin loading or command inference is implemented. Common audio extensions (WAV, MP3, FLAC, OGG, Opus, M4A, AAC, AIFF, WMA and APE)
and video extensions are detected automatically. OGG defaults to audio; use
`--kind video` for an OGG video. Explicit `audio` or `video` accepts other local
formats supported by the installed libmpv codecs. Audio/video streaming is exempt
from the 256 MiB image/model/material file limit.

An identical validated registration retains its hidden configuration or healthy
loaded view when watched inputs and the last successful output/dependency
publication are unchanged and complete resource coverage is established.
Incomplete discovery and formats with untracked external resources retain the
normal refresh path. A proven unchanged registration does not restart its
generator or rewrite state.
Changed files and failed candidates retain refresh/retry behavior. `reload`
explicitly forces refresh, including after a successful generator run. Ordinary
generator completion reuses an already displayed unchanged publication.

Graph-only settings: `preview_model` is an absolute UV-mapped `.glb` path (empty
clears it); `preview_shape` is `sphere`, `cube` or `plane`. These control the
right sidebar's material target without changing its source model. Missing
model files remain waiting. Graph UI target changes use the same settings route.

Boolean settings: `grid`, `axes`, `edges`, `orthographic`, `nearest`, `materials`,
`textures`, `lock_horizon`, `triangles`, `paused`, `muted`, `loop`. Materials/textures/horizon/triangles
and loop default to true. Video defaults to muted and unpaused; audio defaults
to unmuted and paused. Remembered per-kind preferences override these defaults. Numeric settings:
`light` in 0–5, `roughness` / `metallic` in 0–1. `background` is `dark`, `light` or
`checker`; checker applies to images. Material `shape` is `sphere`, `cube` or `plane`.
Material roughness/metallic scalars apply when no ORM map is supplied.
`up_axis` is `y` (default, XZ floor) or `z` (XY floor). `lighting` is `studio`
(default, F3D embedded HDRI ambient + normal lights) or `lightkit` (normal lights).
Disabling materials uses opaque neutral clay and suppresses all texture maps;
the stored `textures` preference is retained. Display-mode changes reload the
scene to restore authored properties, preserving camera state.
`texture_quality` is `original` (default) or `preview`. Models can use a bounded
1024 px embedded-GLB texture derivative in ordinary Preview; material samples
ignore the shared preference. Unsupported sources/cache failures load the
original. Parts/Cuts and review identities always use the original. See
[Models](../specs/models.md) for eligibility and cache bounds. Model metrics
include `texture_preview` with actual status, source hash, cache hit,
preparation milliseconds and any fallback reason.
Model animation adds `animation` (integer clip index 0..255) and
`animation_speed` (0.1..4), `animation_stepped` (boolean, default false) and
`animation_fps` (integer 1..120, default 12), plus `paused` and `loop`.
Stepped playback evaluates poses on the chosen clip-relative sampling grid;
`metrics.sample_position` reports the evaluated time while `position` retains
elapsed time. Native Previous/Next buttons and comma/period pause and move one
frame boundary at that rate. Sampling changes retain position and camera. Default animation transport
is paused, looping, clip zero at 1×. `seek` accepts seconds relative to the
selected clip for a visible animated model, in addition to audio/video.
Visible unavailable clip selections are rejected; clips are discovered only
after the source loads. A shorter replacement clip list falls back to an
available index. Clip changes use a fresh candidate engine to reset old clip transforms, preserving
the camera and last-good content. Other transport changes reuse the engine. Nonlooping EOF
pauses at the final pose, and Play after EOF restarts.
These model transport settings persist per registration, but ordinary changes
do not become defaults for unrelated models/materials. All four `animation*` settings are per-model only and are rejected by `defaults`.
Audio and video support pause, mute, loop and dark/light background. Videos fit
automatically; audio shows measured green energy bars and a stereo waveform seek
timeline with a synchronized vertical playhead, using no video decoder or GL
renderer. Its visuals freeze on pause, update on paused seeks and keep responding
to source audio while muted.
Audio/video reload is asynchronous: Loading precedes successful `metrics.loads` and
Live status. Audio publishes after a loaded selected audio decoder reports
valid parameters, without requiring a video frame; rejected replacements retain the previous loaded player.
Reload restarts playback from the beginning and retains the stored playback options.

Version-1 registry files missing `grid_size` restore as 2. Legacy `compact`
fields are ignored on restore and omitted from saved state and responses.
The optional `theme` registry field defaults to `auto`. Invalid theme values are
rejected without replacing state. Theme changes save before applying the native
Qt palette; save failures retain the previous appearance. Auto clears app palette
overrides and follows system colors. Preview background settings are independent.
CLI equivalents include `layout grid --size 4` and `layout overview`.
Overview shows all registrations with zero ordinary active views; clicking a tile
uses `zoom` to atomically select Single view. `presentation.overview` reports its
visible tile geometry and finite worker state. Top-level `cache` describes the
bounded retained payloads and small derivatives; [Cache/overview](../specs/cache-overview.md)
owns admission, pruning, freshness and resource limits.
`cache.diagnostics` reports bounded churn history, likely problem sources and
rotating log status. `asset-preview cache [stats|events] --json` reads it without
starting/raising the service. `cache` action `events` returns `events`, `last_seq`,
`events_dropped` and the ordinary cache summary. Events distinguish normal reuse
from unused removals, recent refaults, repeated writes and empty prune scans;
`work_us` covers measured decode/prune/reclamation work. Per-session metadata logs
live at `STATE/cache-churn-v1.jsonl`, with one bounded backup.
All changed settings are remembered for future previews, grouped as `image`,
`audio`, `video` and `3d` (models/materials); `list.preferences` exposes these defaults.
Explicit `add.entry.settings` overrides them without changing remembered defaults.

For `.vxz`/`.ovoxel` models with the installed F3D4 TRELLIS reader,
`ovoxel_mode` accepts `surface` (default), `cells`, `voxels` or `vertices`.
These show the extracted raw surface, exact occupied cells, cell-center markers
or decoded vertex positions. Marker diameters are 0.6 and 0.36 grid cell and
scale with zoom; they do not use the overall model bounds for size. It is stored per preview,
excluded from shared defaults, and changes the candidate import while retaining
the camera. Occupied voxels use colored point sprites; geometry-only float fields
use diagnostic clay. No mesh repair or material baking runs during loading.
Same-kind ID replacements retain their existing settings unless explicitly overridden.
New material CLI registrations inherit the last sample shape unless `--shape` is set.
Preferences and layout survive Close all and app restarts; registered entries
keep their own saved settings across restarts. Legacy version-1 files without `preferences`
remain valid. Failed settings/Close all saves retain the prior registry and live views.
`asset-preview close-all` leaves a stopped service stopped.

`metrics.loads` counts successful loads during this view's lifetime;
`metrics.renders` counts paint calls. They reset on unloading/re-creating a view.
Image metrics include decoded bytes and original/decoded dimensions; model metrics
include `engine`, actual OpenGL `renderer`, lighting mode, scene light count and
`camera` position/focal/up vectors. VTK orthogonalizes the reported view-up vector;
it is not necessarily equal to the world-up vector even with the horizon locked.
Model `gltf_import` reports the optional native byte-fill reader's availability
and finite `array_views`, `byte_component_fills`, `filled_texels` counters.
`load_timing` reports elapsed milliseconds for engine/options, preparation,
import, metadata, animation, camera/publication, total load and first render.
`load_to_first_render_ms` begins at native load entry, excluding prior lazy-module
activation. These are CPU wall clocks with synchronization, not GPU timestamps;
use complete-service measurements for opening latency.
`metrics.triangles` counts source triangles for GLB/glTF scenes, triangulated OBJ
faces and material samples; it is null for unavailable formats/metadata. Mesh
instances and degenerate primitives are included. The `triangles` option controls
the per-view badge without reloading and defaults to true, in all layouts.
Active entry diagnostics expose `viewport`, `controls.options` geometry and
`status_visible`, plus `overlays` name/controls/status/triangles geometry/visibility;
top-level `options_open` reports an active popup. These support
isolated input checks and are not persisted. These are diagnostic counters,
not a guarantee of GPU-memory reclamation or perceptual quality.
Top-level `presentation` is empty without a window; otherwise it reports
`fullscreen`, temporary `single_preview`, window `width`/`height`, `menu_visible`,
`toolbar_visible`, and `notice` geometry/visibility plus `sliding`. These are
transient read-only UI diagnostics, independent of persisted layout/settings.
Playback `controls` also report `focused` for keyboard navigation checks;
video adds a `fullscreen` button alongside the existing playback controls.
Models expose a bounded `metrics.animations` clip-name array (up to 256 names,
128 characters each). Animated models additionally expose `animation`,
`duration`, `position`, `paused`, `eof`, `seekable` and diagnostic `animation_timer`.
`controls` reports their clip, speed, timeline and button geometry/state, and
`overlays.animation` describes the native animation bar. A static model's
animation bar stays hidden. [Animation contracts](../specs/animations.md) define
frame scheduling, source replacement and the static-only component inspector.
Audio/video metrics add `backend=libmpv`, `loading`, duration/position
(seconds), paused/muted/eof, seekable and codec. Video also reports width/height
and `hwdec`; audio reports `track` (filename), `sample_rate`, `channels` and
`audio_output` (possibly unavailable until output initializes). Audio also reports
`visualizer`: `available`, `updates`, two-channel `rms_db`, `peak_db`,
`centroid_hz`, smoothed `energy` and onset `pulse`. Measurements are finite, with
silence floored at -90 dB. Its bounded stereo energy history is not a frequency
spectrum or literal PCM waveform. The separate complete-clip PCM overview reports
`waveform`: `ready`, `loading`, `bins` (2,048 when ready), `channels` and `duration`.
Timeline control descriptions expose a `waveform` boolean and their existing
position/dragging fields. The finite background overview decoder has no polling
timer and is cancelled off-page. Audio has no OpenGL renderer metric. Decoder reports may be unavailable
until playback initializes; GPU renderer identity alone does not prove hardware
video decoding. `revisions` increases only after successful asynchronous audio/video
publication, not merely when a load command is accepted.


Defaults can be edited without a registered preview: `{"method":"defaults","group":"3d","settings":{"edges":true}}`. Groups are `image`, `audio`, `video`, and `3d`; settings use the existing validation and persist for future registrations without modifying existing entries.

Model/material entries retain a per-preview `camera` object after camera input.
It contains three-number `position`, `focal` and `up` vectors, a view `angle` in
degrees, and a positive `parallel_span` for orthographic zoom. These values also
appear in `metrics.camera`. Same-kind registrations retain this state unless
an explicit camera is supplied; camera state does not become a shared default.
The card's Reset button restores the initial fitted camera and saves it.

Image entries retain `image_view`: `{"zoom":1.5,"pan":[0.1,-0.05]}`. Zoom is
bounded to 0.05–32; pan offsets are fractions of the viewport width and height.
`metrics.image_view` reports the current state. Same-kind reconnections retain it.

`reopen` restores the latest closed preview in this service session, including
its list position and inspection state. At most 32 configurations are retained;
an already re-registered ID is skipped. `list.closed_count` reports the count.
Entry `selected` identifies the shortcut/menu target independently of decoded
`active`. Hovering a visible preview updates this selection without clicking;
cards have no selection highlight. Outside previews, the last target is retained.

`{"method":"compare","ids":["before","after"],"camera_sync":true}` displays
two model/material previews with linked orbit, pan and zoom. Use `ids:[]` to
end comparison, or omit `ids` to toggle `camera_sync` on the current pair.
`list.comparison` and `list.camera_sync` report the saved comparison state.
`{"method":"camera_view","id":"before","view":"front"}` changes a visible
3D camera; supported views are front, back, left, right, top, bottom, isometric.

## Codex origins and visual references

Registrations optionally accept `codex:{thread_id,cwd,socket}`. The UUID and
absolute, bounded local paths are validated. `open` and `show` can also carry
this object as a transient fallback; newly opened paths retain it. Origin-only
reconnections save metadata without restarting views/generators. The CLI fills
this from the caller's Codex environment. `codex` requests with `action:open` or
`action:collapse` control the right dock in an already visible window. They do
not send a prompt or interrupt a turn. `presentation.codex` reports visibility,
client PID and attached thread ID; `reference_controls` reports current native
control/editor geometry. See [references](../specs/references.md) for ownership,
clipboard, captures, annotations and installed-component requirements.

`presentation.codex_fraction` reports the preferred right-dock share of the
window width (default 7/16). Native splitter drags update it; ordinary window
resizing preserves it. The preference is stored with window geometry, not in
the asset registry or remembered media defaults.

Project metadata, CLI inference, persistence/fallback and visible-pane ownership
are specified in [Projects](../specs/projects.md). Socket clients send
`entry.project`; omitted membership is preserved on reconnect, while empty string
means ungrouped. `list.projects` contains ordered `{id,label,current,entries}`
records. Creating/promoting a project never selects a global preview or raises
the window. `make_current:false` keeps an existing current member during batch
sends. Project labels and memberships are independent of Codex conversation IDs.

Native image/model loads publish asynchronously. `metrics.loading` remains true
until completion. Ordinary models expose `metrics.command_pending` while an
input/settings/seek command or its rendered frame is outstanding. Clients that
need completed pixels wait for both fields to clear. `list.performance` contains
finite startup and most recent layout/reconcile phase timings (ms), with no
sampling timer. Image, model and preload diagnostics include their own load times.
