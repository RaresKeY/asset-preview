# Selection previews

Use this for author accept/deny decisions, release-package choices, design alternatives or a single proposal. Keep recommendations in content; leave every answer undecided.

Write a local manifest with schema `asset-preview-selection/v1`, stable `id`, `title`, optional `markdown`, and either root `items` or `sections`. Each section has `id`, `title`, optional `markdown` and `items`. Each item has `id`, `title`, optional `recommendation` and `markdown`. For visual review, add `"image":"references/item.png"` for an inline reference and `"asset":"models/item.glb"` for the full 3D preview, using local paths relative to the manifest or absolute paths. A raster `asset` can also supply the inline preview. Use the row buttons to open full image or 3D previews. Markdown is rendered/selectable, with HTML, resource fetching and link navigation disabled.

```sh
asset-preview add /absolute/choices.selection.json --id package-choices \
  --project release-review --label "Release packages" \
  --decisions /absolute/package-decisions.json
```

This registers in the background. `open /absolute/choices.selection.json` explicitly opens/selects the view; ordinary sends preserve the user's desktop. Use one stable project/preview ID and source file. Decisions live separately; default output is the same basename with `.decisions.json`. Its directory must exist. No results file is created until the user chooses.

For several variants per asset, use one section per asset and the root field `"presentation":"variant-rows"`. Each section shows its options side by side in a scrolling row. Switching presentation preserves existing decisions.

The user can review a batch or One at a time, search/filter and use green check/red X buttons on the right. Clicking an already chosen button or Clear choice clears it. Never manufacture answers by editing output JSON. Read the saved results or live `status --json` metrics for selected IDs. A decision records preparation intent, not blanket approval to publish, deploy, spend or contact others.

Changed item/explanation bytes invalidate only affected answers. Adding or changing the presentation paths `image` and `asset` preserves answers. Keep stable IDs and include the exact version, artifact digest and material conditions in the item Markdown so decisions apply to the intended candidate. Old unmatched records remain historical. Use only answers whose digests match the current view. Reordering options is safe. No autoaccept or bulk approval exists.

See [the owning schema and lifecycle](../../../specs/selection.md). Limits: 32 sections, 256 options, 2 MiB input and 32,768 characters per Markdown field. Use the ordinary file preview for large supporting research/illustrations and register it as a supporting view.
