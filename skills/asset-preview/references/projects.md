# Projects and current previews

Use one stable project for the wider owning project, with separate groups/galleries
for its individual parts. Create it as work starts, or let the first send create
it. Keep related model, image, material and media views within their part's group;
preview IDs still identify individual outputs. All registrations stay in the background.

```sh
preview="${ASSET_PREVIEW_PROJECT:-$HOME/workspace/asset-preview}/bin/asset-preview"
"$preview" add /absolute/game/exports/chair.glb --id chair-model --project my-game --project-label "My Game" --group furniture --group-label "Furniture"
"$preview" add /absolute/game/exports/hero.png --id hero --project my-game --group characters --group-label "Characters"
# Or put an existing gallery under its wider project:
"$preview" project create chair --parent my-game --label "Chair"
```

`--group` defaults its parent to the owning checkout when `--project` is omitted.
Group IDs are scoped to their project, so two projects can both have a `props`
gallery. `project list --json` reports their effective IDs and `parent` relationship.
Without `--group`, existing `--project` usage still registers directly in that
project. The following remains useful for one small standalone asset:

```sh
preview="${ASSET_PREVIEW_PROJECT:-$HOME/workspace/asset-preview}/bin/asset-preview"
"$preview" project create chair --label "Chair"
"$preview" add /absolute/project/exports/chair.glb --id chair-model --project chair
"$preview" add /absolute/project/proof/contact.png --id chair-contact --project chair --no-current
"$preview" project current chair chair-model
"$preview" project list --json
```

`add` and `material` normally choose the sent view as the project's current
preview, including an unchanged reconnection. Use `--no-current` for supporting
views so a batch of diagnostics does not replace the intended main output.
The first member of an empty project becomes its current preview. Ordinary
output watching refreshes the chosen view; file timestamps do not choose a
different view. `project current PROJECT PREVIEW` promotes an existing member
without replacing its generator, settings or camera.

Without `--project`, the CLI derives a stable ID from the nearest Git/specs root
above `--cwd` (the caller's cwd by default): sanitized basename plus a path hash.
Use an explicit ID for a workstream, shared project across checkouts, or output
sent from another tool's directory. `--project-label` names it. Environment
`ASSET_PREVIEW_PROJECT_ID` / `ASSET_PREVIEW_PROJECT_LABEL` provide helper defaults;
`ASSET_PREVIEW_PROJECT` remains the viewer checkout path. `--ungrouped` opts out.
The importable standard-library CLI exposes `project_identity(directory)` for
connectors that use socket requests directly.

Workshop / model-flow use `workshop-<asset>` as the default project and retain
`workshop-<asset>-<view>` preview IDs. A batch makes `model` current when present,
otherwise its first view; `--view NAME` deliberately chooses that view. Environment
overrides can join several assets into an owning project's group. Keep a chosen
finite generator attached to its intended view; choosing a different current view
may suspend it when its old pane becomes hidden.

In **Overview**, **Group by project** toggles grouping and the project selector
filters the visible thumbnails. Projects have wider headers with indented group
headers below. Choosing a project includes all its groups; choosing a group shows
only that gallery. The folder explorer nests project → group → preview.
Right-click a group heading or explorer folder for **Move to Project** (including
**New Project…**) or **Move Out of Project**. **Projects** (Ctrl+9) shows one fully controllable
current preview per project, with an in-pane dropdown for switching members.
Auto grid grows with the project count; 2×2, 3×3 and 4×4 provide explicit density.
Narrow windows reduce columns, and overflow scrolls. Only panes intersecting the
viewport load media/renderers or run generators. Empty projects have waiting panes.
Legacy ungrouped entries remain accessible through Overview and share a fallback
pane in Projects.

The user's layout/filter is separate from agent publication. Do not send layout,
select, zoom or gui commands merely to publish a current preview. If already
watching Projects, the user sees that project's pane update; other project panes,
window placement and focus remain in place. Hiding/minimizing suspends all panes.
Read `status --json`: `projects[].current`, membership, active flags and successful
revisions establish what was chosen and what actually loaded.

Populated projects/current choices, grouping/filtering and grid density persist
across restarts. Empty waiting groups and their filters last for the creating
session. Removing a current preview falls back to the last registered surviving
member; closing or moving the last member automatically removes the group.
`project remove ID` removes a waiting group only. Close all clears registrations
and groups; undo restores a preview's project name and position. Legacy empty
groups are pruned at startup. Reconnecting without a socket-level `entry.project` retains an
existing registration's membership; an explicit empty string removes membership.

For a collection browser with one full preview, use
`layout gallery --project PROJECT` (Ctrl+8 / View > Image Gallery). It shows a
compact left thumbnail grid and one selected asset on the right, with a resizable
divider. Click thumbnails to select in place; arrows follow filtered order.
Organize unique assets and method comparisons into separate named groups beneath their owning project;
register one representative per subject in the former and comparison images in
the latter. The project selector switches collections. Only the selected ordinary
preview loads; thumbnails reuse the bounded cache. As with other layout commands,
choose Gallery only when organizing/opening is requested. Verify `layout:gallery`,
`presentation.gallery.visible`, one active selected member and successful loads.
