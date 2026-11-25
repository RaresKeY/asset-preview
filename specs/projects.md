# Projects and grouped previews

Source ownership: `src/service.*`, `src/project_grid.*`, `src/project_identity.h`,
`src/overview_grid.*`, `bin/asset-preview`, Fish completion and the project skill.

## Project, group/gallery and preview hierarchy

Projects are the wider category. A project can contain separate named groups or
galleries, each retaining its own members and current preview. The existing
project registry represents both levels: optional string `parent` on a group
names a top-level project. Empty/absent parents retain legacy top-level behavior.
Parents must exist, differ from the group and have no parent themselves; a
project that contains groups cannot become a group. Missing parents, cycles,
deeper nesting and malformed saved relationships reject the operation/state.

`add … --project GAME --group props --group-label "Props"` registers a preview
inside that project's Props gallery. Without `--project`, the owning checkout
identity remains the parent. Group IDs combine the first 40 group-ID characters
with 16 SHA-256 hex digits of `PROJECT:GROUP`, so equal group names in different
projects stay separate. IDs use the existing 1..64 ASCII ID contract. The CLI
sends `parent_project`, optional `parent_project_label`, and `project_label`
alongside the existing group-valued `entry.project`. Parent/group creation,
reparenting and registration commit or roll back together. An unchanged asset
reconnect still saves and displays changed hierarchy metadata without reloading
its content. Existing callers without these fields retain their behavior.

`project create GROUP --parent PROJECT` places an existing or new gallery inside
an existing project; `--parent ''` detaches it. Socket project upserts use `parent`.
Right-click a group header in Overview or a folder in the explorer for **Move to
Project**, including **New Project…**, or **Move Out of Project**. New-project UI
creation and group assignment are separate validated saves; a failed assignment
can leave a session-only empty project. Parents cannot be removed while they
still have groups. Counts/limits include both levels.

Grouped Overview shows a project heading above its indented group headings.
Project filters, project-header Gallery and Close actions include all descendant
groups; group actions apply only to that gallery. Root and sibling creation order
determine traversal, with each project's direct previews before its groups and
Ungrouped last. Flat mode retains registration order while honoring descendant
filters. The filter picker indents group names; the explorer nests project →
group → preview and distinguishes folders from files explicitly. Both folder
levels open filtered Gallery; file selection opens Single. Projects layout omits
empty container-only panes and shows each group's own current preview.

Version-1 saved project rows retain optional `parent`. A parent with populated
groups persists even without a direct preview. Empty waiting groups remain
session-only; startup retains populated groups and their parents, pruning other
empty entries. Undo records include the parent label/order so Close all followed
by reopen restores the hierarchy. Legacy flat registries need no migration.
`list.projects` adds `parent`/`parent_label`; Overview headings report `nested`;
explorer folder rows report `parent` alongside their direct `files`.

The projects lane checks hierarchy ordering, descendant filters, restart/undo,
validation and save rollback. CLI checks cover owner-scoped group identities.
`GalleryIntegration.test_native_project_group_hierarchy` checks real header
actions, nested explorer input, narrow layout, parent close/undo and GPU identity.

## Registration and persistence

A project has a stable 1..64 ASCII letter/digit/underscore/hyphen ID, a nonblank
label of at most 256 characters and one current preview ID. There are at most
256 projects and 512 preview registrations. The saved registry is bounded to
8 MiB and uses compact JSON if indentation would exceed that bound. Project creation is independent of
outputs, so agents can create a session-only empty group before generation. `entry.project`
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
with automatic removal of legacy empty groups; ungrouped previews stay accessible. Saves include a
valid effective current ID: if a member is removed/moved, choose the last
registered surviving member. Closing or moving the last member automatically
removes the project and resets its filter. Close all clears every project,
including waiting groups. Newly created waiting groups remain visible for the
current session but are not saved until they contain a preview; their filter is
also session-only. Startup prunes legacy empty groups and their filters, and
atomically persists that cleanup after validating the full state. Undo recreates
a removed project with its original label and position when restoring a member.
After successful saves the effective fallback also replaces the in-memory current
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

The preferred home view and default for fresh or layout-less state is Overview
with all projects and project grouping enabled. Explicit saved layouts, grouping
and filters still restore. Esc returns from Single, grids, Projects, comparison,
inspection or a filtered/flat Overview to this home view in one atomic layout
request, preserving the selected preview. It is a no-op there. In fullscreen,
Esc first restores the window; another Esc returns home. Popups, dialogs and the
attached terminal retain their own Escape handling.

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
preview snapshots/overlays to the scroll viewport. Native ancestor creation stops
at the Projects branch, preserving the shared canvas/Overview raster backing
store under [Preview lifecycle](preview.md).
Panes receiving native cards establish their native window before card construction,
so overlay windows inherit the pane's clipping and stacking boundary before
the model surface exists. Raster-only panes retain normal backing-store input
routing. The projects GPU lane checks the native parent of real model chrome.
Switching a pane's member hides and detaches the departing card from the layout
without changing its QObject parent. The service then retires that card through
the ordinary lifecycle. Reparenting a native GL card during this transition can
hide the scroll content and prevent the next video from initializing. Only
deleting the pane reparents a surviving service-owned card before pane teardown.
Attaching a card clears the pane's loading placeholder state.

Hovered-pane navigation maps the pointer through the stable top-level widget
and widget-tree offsets. It does not use a native scroll child's global origin:
that origin can become zero after embedding GL, incorrectly accepting a pointer
over the menu bar as a pointer inside the first pane.

The window-owned thumbnail queue warms nearby members, then other open
registrations after a quiet period, using two image slots and one finite model
child. Active previews and their generators keep priority; loading pauses warming.
A single predictive preloader warms the next/previous member of the selected
visible project, following native arrow direction, without activating it.
It waits for 250 ms on one target, so sweeping across panes does not prefetch
every full image under the pointer.
See [Cache and overview](cache-overview.md) for budgets, cancellation and metrics.

Overview remains lightweight and activates no ordinary views. Grouping is a
toggle; project filters apply in Overview and Projects. Grouped headers preserve
project creation order, member registration order and put Ungrouped last. Flat
Overview retains registration order. Keyboard targets stay within filtered items;
existing tile activation and close selection continue to apply. Shift ranges and
keyboard navigation follow the filtered display order, excluding hidden groups. Filters do not
change agent publication or global selection. Agent sends preserve desktop focus,
visibility and layout while updating the addressed project's current pane.

Single-view Left/Right navigation, the preview chooser and its position counter follow
the Overview display order: left to right, then down, through each project in
creation order, with Ungrouped last. They honor the Overview grouping toggle
and project filter; flat mode follows registration order. Navigation clamps at
the first and last displayed preview.

Grouped navigation derives asset IDs directly from project/member order without
building the public project JSON. Project listings collect membership and fallback
current IDs in one registry pass, preserving valid explicit currents and empty
groups. Both paths derive fresh state without a retained grouping cache.

Stable marks share `projectIcon` across group headers, filters and live panes.
Version-1 mapping is FNV-1a over UTF-16 `project:<id>`, four native vector motifs
and eight palette choices; names remain authoritative because finite marks can
collide. Renaming/filtering does not change the mark; status uses separate text.

`list.projects` reports group identity/current/members; `presentation.projects`
reports pane geometry, responsive columns/rows and scroll state for verification.

Navigation reads the existing pane count without serializing pane geometry.

## Verification

`tools/test.sh projects` checks atomic registration/promotion, rollback, restart,
last-member cleanup on single/bulk close and membership moves, Close all,
project labels/order on undo, legacy empty-group migration, session-only waiting
groups, grouping/filtering and visible-only ownership. `projects-gpu` uses the
existing Gamescope headless harness, actual hardware-rendered models, image and
media panes, native chooser/reset controls, scroll suspension and desktop/narrow
captures. It also checks hovered pane/header arrows, native focus across image,
video and model switches, project/edge boundaries, atomic promotion/selection and failed-save rollback.
Evidence lives in `evidence/projects/`. CLI tests cover inferred/explicit
identity and flags; Workshop tests cover batch current selection and generator
argv. Existing overview and functional lanes guard legacy callers/lifecycle.
`tools/test.sh project-grid` exercises card retirement, replacement, pane deletion
and placeholder state independently. `project-grid-gpu` also checks pointer
mapping before and after native viewport creation in the private headless display.

## Gallery

Each project header in grouped Overview has a right-aligned **View in Gallery**
button on the same row as its identity icon and label. It opens Gallery filtered
to that group, including Ungrouped, through the atomic layout/filter operation.
An existing selection in the group is retained; otherwise the first member is
selected. A following X closes every registered preview in that group through
the existing atomic batch-close operation, preserving files and the normal
32-entry reopen history. This includes Ungrouped and hidden members. The actions
share the icon's 20 px height and 3 px top inset; the X has the same outer margin
as the icon, with an 8 px gap between actions. Buttons support mouse and keyboard focus, track scrolling/resizing,
and are hidden in the compact Gallery sidebar and flat Overview.

`layout gallery` / View > Image Gallery / Ctrl+8 keeps a compact scrollable
thumbnail grid on the left and one ordinary selected preview on the right. The
shared Overview widget, thumbnail queue/cache and project filter/order are reused;
there is no second queue, media player or full image decode per thumbnail. Click
selects in place; Left/Right and the picker follow the filtered project order.
Grouping remains available. A project-filter change chooses its first member if
the prior selection is outside the filtered set, atomically with the layout save.
Failed saves preserve selection, filters and the active preview.

The splitter is resizable; the initial sidebar is 380 px, narrowing to roughly a
third of the window below 760 px. Compact tiles use a 110 px minimum width and
116 px height. Existing grid multiselection and thumbnail tooltips remain
available. Small raster thumbnails retain their source pixels and enlarge with
nearest-neighbor sampling in the sidebar; larger images still shrink smoothly.
Aspect-ratio fit and tile geometry remain unchanged. The ordinary preview retains its image zoom, pan, filtering and
background controls. Gallery can also select the existing other preview kinds.
Only the selected filtered member is active; hide/minimize suspends it normally.
Inspection/comparison temporarily use their ordinary workspaces. Escape returns
to the existing all-project grouped Overview. Layout and selection persist; the
splitter width is session state. `presentation.gallery` reports visibility,
selection and sidebar width; `presentation.overview` describes the shared grid.

`tools/test.sh gallery` checks selection, save rollback, source refresh, hidden
generator ownership, hiding and restart. `gallery-gpu` adds real clicks, arrows,
scrolling, narrow layout, Escape, native 3D/raster switching and hardware identity.
Screenshots and results are retained under `evidence/gallery/`.

## Folder explorer

The first toolbar button, directly below File, toggles a native left folder
explorer (also View > Folder Explorer or Ctrl+B). Hiding removes the entire pane
and splitter handle; only the toolbar toggle remains. The horizontal splitter
resizes the pane. Visibility and width are remembered in `window.ini`; fullscreen
temporarily hides it and restores the prior choice on exit.
Below 760 px, an oversized explorer shrinks to roughly a third of the window,
retaining at least 170 px for the tree and room for the selected content.

The tree lists every project/category in grouped Overview order, with Ungrouped
last, regardless of the active filter or grouping toggle. Each registered preview
is one selectable child, using its label and a full-path tooltip. Folders expand
and collapse; double-click/Enter opens the category in Gallery. Selecting a child
opens Single. Tree Left/Right expand/collapse instead of switching previews.
Folders start collapsed, including the selected preview's folder on startup;
new folders introduced by background registrations also start collapsed.
Selection tracks the main preview; later navigation reveals its selected folder,
while metadata refresh preserves folders the user expanded or collapsed.
Expansion choices last for the current window session and reset on restart.
Add/remove/move/rename and reopen rebuild the lightweight tree as needed. The tree
creates no preview views, thumbnails, players or generators of its own.

Opening Single outside the active category clears that filter in the same saved
transaction as selection/layout; save failures restore the prior filter as well.
Entering Single from another layout applies the same rule when no filter was
supplied in that request. An explicitly supplied `project_filter` is preserved,
including when the prior selection belongs elsewhere; filtering alone retains
global selection, and a subsequent selection/navigation uses the requested set.
The picker always
includes an otherwise filtered-out selected entry, including restored states and
background metadata changes, so its name cannot become blank through index -1.
Its tooltip includes the complete label and source path. Arrow order still follows
the current Overview filter. `presentation.explorer` exposes pane/toggle geometry,
folders, expansion and file rows; `selection_control` reports the shown name/ID.

The Gallery integration lane checks the file/filter regression and failed-save
rollback. Its GPU lane also checks native tree clicks, keyboard expansion,
category Gallery, collapse/restore, fullscreen, category cleanup/reopen and
Overview ownership, with captures under `evidence/folder-explorer-native/`.
