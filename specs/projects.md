# Projects and grouped previews

Source ownership: `src/service.*`, `src/project_grid.*`, `src/project_identity.h`,
`src/overview_grid.*`, `bin/asset-preview`, Fish completion and the project skill.

## Registration and persistence

A project has a stable 1..64 ASCII letter/digit/underscore/hyphen ID, a nonblank
label of at most 256 characters and one current preview ID. There are at most
256 projects and 256 preview registrations. Project creation is independent of
outputs, so agents can create an empty group before generation. `entry.project`
is optional; absence retains old membership on reconnect, and an empty string
means ungrouped. Direct legacy clients remain valid.

`project` requests upsert (`id`, optional `label`/`current`), list, or remove an
empty group. Current IDs must belong to the group. `add` atomically creates a
missing group and promotes the sent preview, unless `make_current:false` keeps
an existing current member. The first member becomes current. `project_label`
changes the group's label. A metadata-only update or unchanged reconnect can
promote a preview without rebuilding it or changing generators/cameras/settings.
File changes refresh content without choosing a new current member.

Version-1 state adds optional `projects` (ordered array of id/label/current),
`project_grid_size` (0 auto, 2/3/4), `group_projects` (default true), and
`project_filter` (empty/all, project ID or `@ungrouped`). Existing state restores
without migration writes; ungrouped previews stay accessible. Saves include a
valid effective current ID: if a member is removed/moved, choose the last
registered surviving member or empty. Empty groups persist through Close all;
removing an empty filtered group resets the filter. Undo recreates a removed
project when restoring one of its old members. After successful saves the effective fallback also replaces the in-memory current
pointer, so later `--no-current` re-adds cannot resurrect a removed current ID.
Failed saves roll back metadata,
order, membership, current, filters and density before visible ownership changes.

The CLI creates projects on `project create [ID]` or a send. Its default identity
uses the nearest Git/specs root above generator cwd (caller cwd by default), a
48-character sanitized basename and the first ten SHA-256 hex digits of its
resolved absolute path. Explicit `--project` wins over
`ASSET_PREVIEW_PROJECT_ID`; `--project-label` overrides the environment/default
label. This path identity is local; use explicit IDs across checkouts/machines.
`--no-current` retains the chosen view; `--ungrouped` opts out. Workshop's connector
uses `workshop-<asset>` and promotes `model`/first or an explicitly named view.

## Presentation and ownership

`layout projects` / Ctrl+9 adds a scrollable native grid with one current preview
per group, including empty waiting panes and one ungrouped fallback pane. Each
pane contains the ordinary image/model/material/graph/audio/video card and its
controls. Project header dropdowns persist a chosen member without changing the
global selection/layout. Legacy ungrouped members are browsed in Overview.
Left/Right (also Alt+Left/Right) over a project pane switches its current preview
in member registration order and targets the new preview for keyboard controls.
The current member and active selection commit in one save; failure preserves
both and their visible panes. `select` with optional `make_current:true` exposes
the same atomic operation for a grouped preview, including an already-selected
member whose project is showing another preview. The default remains selection
without changing project publication.
Navigation clamps at the first/last member, stays within the hovered project and
also works over its header. Empty and ungrouped panes do not navigate.

Automatic density starts at ceil(sqrt(project count)); explicit 2/3/4 chooses
columns and desired visible rows. Columns adapt to at least 220 px per pane;
rows retain at least 170 px, with vertical scrolling for overflow. All project
slots exist, but only intersecting slots activate a preview. Scrolling, resizing,
filtering, promotion, hide and minimize reconcile ownership. Offscreen views
release players, renderers and owned generators using the ordinary lifecycle.
No recurring grid timer or speculative renderer is created. Native captures clip
preview snapshots/overlays to the scroll viewport.

Overview remains lightweight and activates no ordinary views. Grouping is a
toggle; project filters apply in Overview and Projects. Grouped headers preserve
project creation order, member registration order and put Ungrouped last. Flat
Overview retains registration order. Keyboard targets stay within filtered items;
existing tile activation and close selection continue to apply. Shift ranges and
keyboard navigation follow the filtered display order, excluding hidden groups. Filters do not
change agent publication or global selection. Agent sends preserve desktop focus,
visibility and layout while updating the addressed project's current pane.

Stable marks share `projectIcon` across group headers, filters and live panes.
Version-1 mapping is FNV-1a over UTF-16 `project:<id>`, four native vector motifs
and eight palette choices; names remain authoritative because finite marks can
collide. Renaming/filtering does not change the mark; status uses separate text.

`list.projects` reports group identity/current/members; `presentation.projects`
reports pane geometry, responsive columns/rows and scroll state for verification.

Navigation reads the existing pane count without serializing pane geometry.

## Verification

`tools/test.sh projects` checks atomic registration/promotion, rollback, restart,
removal, grouping/filtering and visible-only ownership. `projects-gpu` uses the
existing Gamescope headless harness, actual hardware-rendered models, image and
media panes, native chooser/reset controls, scroll suspension and desktop/narrow
captures. It also checks hovered pane/header arrows, native focus across image,
video and model switches, project/edge boundaries, atomic promotion/selection and failed-save rollback.
Evidence lives in `evidence/projects/`. CLI tests cover inferred/explicit
identity and flags; Workshop tests cover batch current selection and generator
argv. Existing overview and functional lanes guard legacy callers/lifecycle.
