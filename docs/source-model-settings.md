# Source-declared model settings

`add FILE --settings '{"up_axis":"y","materials":true,"textures":true}'`
passes explicit options atomically with the registration. These override remembered
preferences and a same-kind existing registration while preserving omitted options.
They do not modify remembered defaults. Put `--settings` before `--exec`.
The existing service validates allowed keys/types and persists the complete entry.

Use this for exported source facts: a Blender Z-up scene exported as a standard
Y-up GLB requires Y-up in the viewer. An authored-material preview should explicitly
enable materials and textures if it must remain independent of prior clay inspection.
Do not reset unrelated camera, lighting, grid, or other inspection choices.

`defaults 3d '{"up_axis":"y","materials":true,"textures":true}'` changes only
remembered options for future registrations; groups `image`, `audio` and `video` also work.
Existing entries keep their settings. By comparison, `settings ID JSON` updates an
entry and remembered defaults together. Reconnecting without explicit settings
retains same-kind entry settings; a new entry inherits remembered preferences.

Verification: `python3 tests/cli_source_settings.py` checks exact CLI requests.
`ASSET_PREVIEW_SOURCE_TEST=1 python3 tests/cli_source_settings.py` additionally
starts a private offscreen service, verifies inherited clay/Z-up, explicit same-ID
overrides, preserved preferences/selection/layout, then stops that exact service.
For a CLI worktree, set `ASSET_PREVIEW_BINARY_PROJECT` to the owning checkout with
its compiled binary. This functional lane has no visual or hardware-renderer claim.
