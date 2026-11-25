---
name: asset-preview
description: Connect live previews for images, media, 3D assets, materials, documents and file diffs during authoring or inspection; exclude viewer implementation and game profiling.
---

# Asset Preview

Use `bin/asset-preview` in `${ASSET_PREVIEW_PROJECT:-$HOME/workspace/asset-preview}`. The CLI owns service startup and socket communication; no fish setup is needed. Examples use `$preview` for this full executable path. The checkout's `specs/_readme.md` routes implementation contracts.

For a requested collection browser, `layout gallery --project ID` shows compact
left thumbnails and one full selected preview; see [projects](references/projects.md).

The user's preferred home view is **Overview, all projects, grouped by project**
(`layout overview --group-projects --project all`). Esc returns there from other
views; in fullscreen, Esc exits fullscreen first. Prefer this view when asked to
choose a layout; ordinary registrations still preserve the user's current view.

## Core flow

1. **Create the project and connect early.** Use one stable project/workstream and choose its current preview; read [projects](references/projects.md) for automatic IDs, early creation and supporting views. Register the authoritative exported output with `add` or `material`, even before the file exists. Use stable project/asset/view IDs and preserve other registrations. Read [connections](references/connections.md) for syntax, ID limits and reconnect behavior. Asset Workshop's `./workshop preview ASSET [--view NAME]` already supplies manifest-driven connections.
   Keep the owning project as the wider category; use `--group PART` for each separate part/gallery within it. Overview and the folder explorer show project → group → preview.
2. **Preserve the desktop.** Sends/updates stay in the background, preserving selection, layout, visibility and focus. Connecting an asset does not request a window. Open/raise only on an explicit user request; respect later closure/hiding. Never create another desktop instance or reopen for captures/load checks. Hidden or off-page registrations may remain suspended.
3. **Watch output saves.** Register once; ordinary watching is enough while the agent owns the build. For source-triggered exports, read [generators](references/generators.md) and attach one finite owner wrapper to one view. Preserve its runtime and locks; avoid duplicate builders and generated-output watch loops.
4. **Verify refresh.** Check `status --json` for the expected ID/path, actual active/build/loading state and successful revision. Registration, a Live label or last-good content does not prove a new build or visual correctness. Read [verification](references/verification.md) for async media, renderer evidence, failures and headless captures.
5. **Clean up narrowly.** Keep useful connections during iteration. `remove ID` unregisters that preview without deleting files. Avoid global hide/stop/close-all, which affects other work. Preserve user inspection and playback choices.

## Read for the asset

| Asset or task | Reference |
|---|---|
| Images, textures or baked material samples | [Images and materials](references/images-materials.md) |
| Markdown, code, configuration, logs or file comparisons | [Files and diffs](references/files.md) |
| Batch or single accept/deny choices with Markdown and saved decisions | [Selection](references/selection.md) |
| Music/SFX, video or rendered turntables | [Audio and video](references/audio-video.md) |
| Models, scenes, modular kits or assembled hosts | [3D assets](references/3d-assets.md) |
| Rigged, node or morph animation | [3D animation](references/animations.md) |
| Exact component targets, comments, pins or planar cuts | [3D inspection](references/3d-inspection.md); opt in on an existing model registration. Offline review reads need no viewer. |

Scenes, modules and animated GLBs use the existing `model` kind. The viewer inspects exported products; geometry, engine-specific shading and gameplay acceptance remain with their owning workflows.
