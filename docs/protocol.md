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
| `list` | — | Entries, live metrics, selection, layout, active count, watches, Qt platform |
| `add` | `entry` object | Add or replace stable ID; no focus/selection change |
| `remove` | `id` | Stop/unload and unregister; preserve files |
| `select` | `id` | Change visible preview/page without raising window |
| `layout` | `layout`: `single` or `grid` | Change display, with at most four active entries |
| `settings` | `id`, `settings` object | Validate, merge and persist options |
| `reload` | `id` | Reload or rebuild only if currently visible |
| `show` | — | Open/raise the single app window |
| `hide` | — | Suspend all entries and hide the window |
| `quit` | — | Stop owned generators, close socket and exit |
| `capture` | new absolute `path` | Save currently visible window as PNG; refuse overwrite |

Example registration:

```json
{"method":"add","entry":{"id":"chair","label":"Chair","kind":"model","path":"/absolute/chair.glb","watch":["/absolute/build_chair.py"],"cwd":"/absolute/project","command":["/absolute/project/build-chair.sh"],"timeout":120}}
```

Entry fields: `id` (optional, otherwise allocated), `label`, absolute `path`, `kind`
(`auto`, `image`, `model`, `material`), absolute file `watch` list, generator `cwd`,
argument-array `command`, timeout seconds and `settings`. Material entries also
have `maps` containing absolute `normal` / `orm` file paths. Files may be missing;
generator cwd must exist at registration. No shell, URI execution, TCP listener,
arbitrary plugin loading or command inference is implemented.

Boolean settings: `grid`, `axes`, `edges`, `orthographic`, `nearest`. Numeric settings:
`light` in 0–5, `roughness` / `metallic` in 0–1. `background` is `dark`, `light` or
`checker`; checker applies to images. Material `shape` is `sphere`, `cube` or `plane`.
Material roughness/metallic scalars apply when no ORM map is supplied.

`metrics.loads` counts successful loads during this view's lifetime;
`metrics.renders` counts paint calls. They reset on unloading/re-creating a view.
Image metrics include decoded bytes and original/decoded dimensions; model metrics
include `engine` and actual OpenGL `renderer`. These are diagnostic counters,
not a guarantee of GPU-memory reclamation or perceptual quality.
