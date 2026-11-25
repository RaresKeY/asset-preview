# Selection and decisions

Source ownership: `src/selection_document.*`, `src/selection_view.*`, registration/card routes in `src/service.cpp`, `bin/asset-preview` and `tests/selection_*`.

## Human input

The user requested a reusable selection view for batches or a single option: large square green check/red X controls on the right, subtle glass/transparency, clean sectioning, compact readable spacing and optional rendered Markdown. Use the default UI look references while preserving Asset Preview's project and lifecycle flow.

## Implemented behavior

`selection` is an ordinary preview kind. `.selection.json` auto-detects; `add --kind selection` handles other names. Registrations preserve desktop focus/layout. `open` explicitly opens/selects it. It works in Single, Grid, Gallery and Projects through the normal visible-only lifecycle. Hidden/off-page views release widgets/watchers; no player, renderer, generator or perpetual timer is created.

The attached header offers Batch or One at a time, All/Undecided/Accepted/Denied filters, text search and single-option navigation. Stable counts describe all options. Sections share a charcoal/light surface and thin separators. Each option has rendered Markdown on the left and two 56×56 square controls in an attached right column. Controls use drawn semantic glyphs and a restrained translucent gradient, native tooltips, accessible names, keyboard focus and Space activation. Clicking the selected decision again or Clear choice returns it to undecided. Recommendations are descriptive text and never preselect answers.

Set the optional root `presentation` field to `variant-rows` to lay out each section as one horizontally scrollable strip of adjacent cards. Each card retains independent image/model buttons and decision controls above its Markdown. Section header arrows page through overflowing strips. Only raster images that intersect both scroll viewports load. The default `list` presentation uses the original vertical options.

Markdown supports native headings, lists, emphasis, code, links and tables. It wraps within the outer scroll area. HTML, Markdown resource loading and link navigation are disabled. Options can declare local `image` and `asset` paths explicitly. Relative paths resolve beside the manifest. The row shows a bounded raster preview and an Image button. An image asset also supplies the inline image when `image` is omitted. A model asset adds a 3D model button; the ordinary interactive preview opens on click and reuses a matching registration in the selection's project. Inline images decode only as their rows enter the viewport and release their pixels offscreen. Supported raster formats are PNG, JPEG, WebP and BMP, subject to an 8 MiB file limit and a 16-megapixel decode limit. Model paths support GLB, glTF, OBJ, STL, PLY and FBX. Missing media leaves the decisions usable. Long titles wrap and empty/filter/error states retain the same controls. The outer 8 px neutral scrollbar has no arrows.

## Manifest

```json
{
  "schema": "asset-preview-selection/v1",
  "id": "release-packages",
  "title": "Release packages",
  "markdown": "Choose what to prepare.",
  "sections": [{
    "id": "platforms",
    "title": "Platforms",
    "items": [{
      "id": "windows",
      "title": "Windows portable ZIP",
      "recommendation": "Recommended",
      "markdown": "Application and runtime, revision 1.",
      "image": "references/windows.png",
      "asset": "models/windows.glb"
    }]
  }]
}
```

Root `items` is the shorter single/unsectioned form, mutually exclusive with `sections`. Zero items is a supported empty state. IDs use 1–96 letters, digits, underscores or hyphens; section and option IDs must be unique in their own scopes. Up to 32 sections and 256 options; titles/recommendations up to 256 characters and each Markdown field up to 32,768. Inputs share the file reader's 2 MiB, UTF-8/line/control-character bounds. Invalid saves retain the last displayed options and disable deciding until a valid reload. Original manifest bytes are never edited by choices.

## Durable decisions

`add --decisions /absolute/results.json` chooses a separate result path. Default: replace `.selection.json` with `.decisions.json`, or append `.decisions.json` to another filename. The existing parent directory is required. Registration rejects source/output identity collisions, including canonical aliases; document reads/writes additionally reject result symlinks, nonregular files and hard links.

Results use schema `asset-preview-decisions/v1`, `selection_id`, `manifest_digest`, and a `decisions` object keyed by option ID. Each record holds `decision` (`accepted`, `denied`, `pending`), option/context SHA-256 `digest`, UTC `updated_at` and `actor: user`. This records a local UI action, not a cryptographic identity or permission to execute/publish anything. Agents consume the JSON or live `selection_decisions` metrics; no protocol command fabricates a user answer.

An option digest includes its item content, section identity/title/Markdown and overall selection ID/title/Markdown. Image and asset paths and the root presentation setting preserve existing choices when added or changed. Put the reviewed artifact revision or hash in the option Markdown when changing what a choice applies to. Other changed option/context content resets its visible answer to undecided, retains old records for reference and shows a changed-options notice. Reordering is harmless. Renaming stable IDs creates new undecided options. Input changes are re-read before each save, preventing a stale view from approving changed text.

Writes use a nonblocking result-specific `QLockFile`, reread/merge existing results, then owner-private `QSaveFile` replacement without direct-write fallback. UI state changes only after successful commit. Corrupt, foreign-selection or oversized results are preserved and disable actions with an explanation. File/parent change notifications refresh results without rebuilding rows or touching the source. No result file is created just by opening/viewing a manifest. Presentation mode/filter/reading position are view-local; durable decisions survive unload/restart.

Metrics include `loads`, selection identity/title/mode, totals, accepted/denied/pending counts, exact current option digests/decisions, result path, writable flag and error. Counts exclude obsolete entries; historical unmatched records remain in the result JSON.

## Verification

`tools/test.sh selection` checks the document/decision contract and actual Qt controls. `selection-gpu` repeats under headless Gamescope and checks a hardware OpenGL renderer. It covers opposite choices, toggle/clear, keyboard Space/focus, merge/locking, changed-option invalidation, stale-view refusal, restoration, output aliases, corruption retention, duplicate IDs, single/empty/filter states and desktop/narrow dark/light captures. `tests/selection_integration.py` checks CLI detection, background registration, real service activation/watch/reload, hidden unload, output paths and restart persistence. Results belong in existing `evidence/verification.md`.

## AI Inferred

The JSON schema, digest invalidation, independent decision file, filters and safety bounds are implementation choices within the requested reusable selection workflow. Selecting a proposed release package does not clear its independent release gates.
