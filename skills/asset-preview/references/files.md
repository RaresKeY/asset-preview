# Files and diffs

Register text outputs with stable IDs, as with other assets:

```sh
$preview add /absolute/project/README.md --id guide --project project-id
$preview add /absolute/project/player.gd --id player --project project-id
$preview diff /absolute/before.cpp /absolute/after.cpp --id changes --mode split --project project-id
$preview add /absolute/changes.patch --id patch --project project-id
```

These commands stay in the background. Use `open FILE` only when the user asks
to open it. Files can be connected before they exist. Comparisons watch both
versions, including atomic saves; no generator is required for ordinary saves.
Use `add AFTER --before BEFORE` for a comparison with the usual optional
generator configuration. Diff inputs are local files, not Git revisions.

Common Markdown/code/configuration/log/patch extensions auto-detect as `file`.
Godot `.gd`, `.tscn`, `.tres`, `.godot` and shader sources are supported. Use
`--kind file` for unknown text extensions or SVG source. Markdown has rendered
and source modes. HTML/XML are source; PDF/Office rendering is unavailable.
Text must be UTF-8, at most 2 MiB, 20,000 newlines and 16,384 characters per line.
Bad saves retain last-good content and report Waiting.

Inline is the default diff view. Soft wrap defaults on in source and both diff
modes; Alt+Z toggles it. File cards use one compact toolbar with no Fit action.
Rendered Markdown zooms with Ctrl+scroll or Ctrl++ / Ctrl+- (Ctrl+= also works);
zoom is local to the loaded view and reported in `metrics.file_zoom`.

The viewer is read-only: line numbers, basic syntax colors, search, inline or
paired diffs, file navigation, change counts and collapsed context. Saved patch
files contain only their supplied context. Large rewrites may use coarse exact
replacement blocks. Markdown does not fetch remote images or activate links.

`settings ID '{"file_mode":"split","file_context":true}'` updates a diff view.
Modes are `source`, `rendered`, `inline`, `split`; unsupported modes fall back.
`file_wrap` affects source and both diff modes; paired rows stay aligned with
wrapping enabled or disabled.
`defaults file JSON` changes remembered file defaults. Check actual loaded
`metrics.file_type`, `file_mode`, `files`, `added`, `removed` and `loads`, together
with revisions/status. Hidden previews unload and have no metrics.
