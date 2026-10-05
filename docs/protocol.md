# Local protocol, version 1

Unix stream socket, printed by `bin/asset-preview paths`. Socket and its parent
directory are private to the current user. One UTF-8 JSON request and one JSON
response, each followed by newline. Maximum request 1 MiB; incomplete connections
expire after 5 seconds. Responses contain `ok`; failures contain `error`.
CLI client timeout is 15 seconds. There are at most 64 connected clients, 256
registrations, 8 KiB per configuration, 128 extra input paths and 128 command args.

| Request | Fields | Behavior |
|---|---|---|
| `ping` | — | Protocol version, PID and actual Qt platform |
| `list` | — | Entries, live metrics, selection, layout, grid_size, compact, active count, watches, Qt platform |
| `add` | `entry` object | Add or replace stable ID; no focus/selection change |
| `remove` | `id` | Stop/unload and unregister; preserve files |
| `select` | `id` | Change visible preview/page without raising window |
| `layout` | optional `layout`: `single` / `grid`, `grid_size`: integer 2 / 3 / 4, `compact`: boolean | Merge/persist layout options; default grid 2×2, maximum 16 active entries |
| `settings` | `id`, `settings` object | Validate, merge and persist options |
| `reload` | `id` | Reload or rebuild only if currently visible |
| `seek` | `id`, nonnegative numeric `seconds` | Seek a loaded visible video to absolute seconds |
| `show` | — | Open/raise the single app window |
| `hide` | — | Suspend all entries and hide the window |
| `quit` | — | Stop owned generators, close socket and exit |
| `capture` | new absolute `path` | Save currently visible window as PNG; refuse overwrite |

Example registration:

```json
{"method":"add","entry":{"id":"chair","label":"Chair","kind":"model","path":"/absolute/chair.glb","watch":["/absolute/build_chair.py"],"cwd":"/absolute/project","command":["/absolute/project/build-chair.sh"],"timeout":120}}
```

Entry fields: `id` (optional, otherwise allocated), `label`, absolute `path`, `kind`
(`auto`, `image`, `video`, `model`, `material`), absolute file `watch` list, generator `cwd`,
argument-array `command`, timeout seconds and `settings`. Material entries also
have `maps` containing absolute `normal` / `orm` file paths. Files may be missing;
generator cwd must exist at registration. No shell, URI execution, TCP listener,
arbitrary plugin loading or command inference is implemented. Common video
extensions are detected automatically; explicit `video` accepts other local formats.

Boolean settings: `grid`, `axes`, `edges`, `orthographic`, `nearest`, `materials`,
`textures`, `lock_horizon`, `paused`, `muted`, `loop`. Materials/textures/horizon,
muted and loop default to true; paused defaults to false. Numeric settings:
`light` in 0–5, `roughness` / `metallic` in 0–1. `background` is `dark`, `light` or
`checker`; checker applies to images. Material `shape` is `sphere`, `cube` or `plane`.
Material roughness/metallic scalars apply when no ORM map is supplied.
`up_axis` is `y` (default, XZ floor) or `z` (XY floor). `lighting` is `studio`
(default, F3D embedded HDRI ambient + normal lights) or `lightkit` (normal lights).
Disabling materials uses opaque neutral clay and suppresses all texture maps;
the stored `textures` preference is retained. Display-mode changes reload the
scene to restore authored properties, preserving camera state.
Videos fit automatically and support pause, mute, loop and dark/light background.
Their reload is asynchronous: Loading precedes successful `metrics.loads` and
Live status; rejected replacements retain the previous loaded video. Reload
restarts playback from the beginning and retains the stored playback options.

Version-1 registry files missing `grid_size` and `compact` restore as 2 and false.
CLI equivalents include `layout grid --size 4 --compact` and `--no-compact`.

`metrics.loads` counts successful loads during this view's lifetime;
`metrics.renders` counts paint calls. They reset on unloading/re-creating a view.
Image metrics include decoded bytes and original/decoded dimensions; model metrics
include `engine`, actual OpenGL `renderer`, lighting mode, scene light count and
`camera` position/focal/up vectors. VTK orthogonalizes the reported view-up vector;
it is not necessarily equal to the world-up vector even with the horizon locked.
Active entry diagnostics expose `viewport`, `controls.options` geometry and
`status_visible`; top-level `options_open` reports an active popup. These support
isolated input checks and are not persisted. These are diagnostic counters,
not a guarantee of GPU-memory reclamation or perceptual quality.
Video metrics add `backend=libmpv`, `loading`, width/height, duration/position
(seconds), paused/muted/eof, codec and `hwdec`. Decoder reports may be unavailable
until playback initializes; GPU renderer identity alone does not prove hardware
video decoding. `revisions` increases only after successful asynchronous video
publication, not merely when a load command is accepted.
