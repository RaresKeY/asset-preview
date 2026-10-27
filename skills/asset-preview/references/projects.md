# Projects and current previews

Use one stable project per owning project or independently followed workstream.
Create it as work starts, or let the first send create it. Keep related model,
image, material and media views together; their preview IDs still identify
individual outputs. All commands below stay in the background.

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
filters the visible thumbnails. **Projects** (Ctrl+9) shows one fully controllable
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

Projects/current choices, grouping/filtering and grid density persist across
restarts. Removing a current preview falls back to the last registered surviving
member; removing the last member leaves an empty project. `project remove ID`
removes an empty project only. Close all removes registrations while retaining
project groups. Reconnecting without a socket-level `entry.project` retains an
existing registration's membership; an explicit empty string removes membership.
