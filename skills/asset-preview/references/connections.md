# Connections and display controls

Use `bin/asset-preview` from `${ASSET_PREVIEW_PROJECT:-$HOME/workspace/asset-preview}`. The checkout's `docs/agent-flow.md` covers exporter integration; `docs/protocol.md` covers additional commands/settings.

## Register and reconnect

See [projects](projects.md) for grouping, current-preview publication and the
interactive Projects grid. Sends automatically join the owning checkout project;
use `--project ID` for an explicit workstream and `--no-current` for supporting views.

Connect the authoritative exported output, including a not-yet-created file. IDs use letters, digits, `_` or `-`, at most 64 characters. Asset Workshop supplies `workshop-<asset>-<view>` through its manifest-driven connector.

```sh
preview="${ASSET_PREVIEW_PROJECT:-$HOME/workspace/asset-preview}/bin/asset-preview"
"$preview" add /absolute/project/exports/chair.glb --id project-chair-model --project chair --project-label "Chair" --label "Chair"
"$preview" material ALBEDO --id project-chair-finish --normal NORMAL --orm ORM --shape sphere
```

A changed registration replaces configuration and recreates the view. Preserve generator/watch configuration and same-kind inspection settings unless deliberately overridden. Identical healthy registrations may be idempotent under the runtime contract. Output saves normally refresh without re-registration.

New entries inherit remembered image/audio/video/3D preferences; `settings ID JSON` also changes remembered display/media defaults. Read `status --json` before changing them. Animation transport remains per registration.

## Window and visibility

`add`, `material` and `start` work in the background. `select ID` changes the shown asset/page without raising the window. `gui`, socket `show`, `open`, bare paths and the no-argument launcher are visible-launch routes, reserved for an explicit user request. Respect later closure/hiding; never bypass it with another desktop instance.

Hidden, minimized and off-page views unload renderers/media players and stop their generator's owned process group. Small decoded image/waveform payloads may enter the bounded service cache; hiding/minimizing clears retained RAM. A hidden registration does not start its generator. Preserve window placement, layout, selection and focus during updates.

## Display settings

`layout projects --size auto` shows each project’s agent-chosen current preview;
`--size 2` / `3` / `4` sets its density. Overview supports `--group-projects` /
`--no-group-projects` and `--project ID` (or `all` / `ungrouped`) filters.

`layout single` shows one view. `layout grid --size 3` shows a 3×3 page; sizes 2/3/4 show 4/9/16 views. More active views cost more memory. Controls remain inside each view.

`layout overview` / Ctrl+0 shows all open registrations as compact thumbnails,
with zero ordinary active views. Click or Enter opens one in Single view. Only
visible small image thumbnails decode lazily; model/media/graph thumbnails reuse
frames from already opened views. Payload RAM is capped at 32 MiB each / 128 MiB
total, thumbnails at 16 MiB within that total, and disk derivatives at 256 MiB.
See `specs/cache-overview.md`; `status --json` includes cache and overview metrics.

`asset-preview cache` reads churn suspects and the rotating log location;
`asset-preview cache events --limit 64 --json` reads the bounded recent event tail.
`--after SEQUENCE` filters newer records without polling or starting the service.
Unused removals, recent refaults, duplicate writes, prune scans and heap trims
include source identity, constraint reason and measured work time. Reading stats
does not raise or change the selected preview. A running older build requires a
normal app restart before its cache can provide the new diagnostics.

For 3D, default `lock_horizon:true` with `up_axis:"y"` keeps the XZ floor level; `up_axis:"z"` uses XY. `lighting:"studio"` combines F3D HDRI ambient and normal lights; `"lightkit"` uses normal lights. `materials:false` uses opaque neutral clay; `textures:false` hides maps while preserving material properties. Apply these JSON keys through `settings ID JSON`, for example `settings ID '{"textures":false}'`.

## Originating Codex conversation

The CLI automatically records the calling `CODEX_THREAD_ID`/`CODEX_SESSION_ID`,
cwd and local Codex server socket on registrations. This powers Preview’s optional
right Codex sidebar without starting another agent or changing the background
thread. Reconnect old registrations from the intended conversation to establish
that origin; do not guess the most recent thread. The sidebar stays pinned until
Follow asset is clicked. Reference captures/annotations attach PNGs to its draft;
all saved screenshots use the user’s Pictures/Screenshots directory.
