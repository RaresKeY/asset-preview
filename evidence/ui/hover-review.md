VERDICT: APPROVE

SCOPE

- Native Qt card titles, overlays, menus and mouse/keyboard targeting in mixed
  model/material/image/video grids and two-view 3D comparison.
- Matched window captures at 540×380, 1000×720 and 1600×1000; native mouse movement,
  keyboard shortcuts, popup interaction, drags and keyboard paging.

FINDINGS

1. `src/service.cpp`, PreviewCard: the title retained a selected border after
   pointing elsewhere. Removed the selected border/tint; accessible active-card
   names and ordinary control hover/focus states remain.
2. PreviewWindow event filters: shortcuts and menus required a selection click.
   Mouse entry/movement now targets visible cards, overlays and native windows
   through the existing selection path, without reload or rearrangement.
3. Native video focus: a previously focused video could receive another card's
   playback key. Hover moves surface focus along with the action target; M now
   follows the hovered video/model correctly.
4. Keyboard paging: new cards under a stationary pointer could replace the
   keyboard's target. Pointer position tracking preserves explicit selection
   until real movement, scroll or click. Popups and drags freeze hover targeting.

CHANGES

- Removed the selection marker, added event-based hover targeting and native
  focus handoff, retained the last target outside previews, and updated Help,
  README, protocol and specs.
- Preserved keyboard/CLI selection, recent-preview close behavior, inspection
  persistence, camera sync, visible-only resources and idle rendering behavior.

VERIFICATION

```sh
./tools/build.sh
./tools/test.sh gpu PreviewIntegration.test_19_plain_arrows_navigate_grid_pages_and_clamp_at_edges PreviewIntegration.test_40_hover_targets_shortcuts_menus_and_native_focus PreviewIntegration.test_37_active_grid_image_shortcuts_and_screenshots
./tools/test.sh functional
./tools/test.sh gpu
./tools/offscreen.sh python /tmp/asset-preview-hover-scenes.py before
./tools/offscreen.sh python /tmp/asset-preview-hover-scenes.py after
git diff --check
```

- Final hardware suite: all 37 pass. Functional suite: 24 pass, 13 hardware skips.
- Regression tests use real XTest motion without selection clicks, check E/M,
  camera presets, reload, image fit, native video playback and model/video key
  handoff, menu/drag ownership, failed-save recovery and outside-card fallback.
- The image test also verifies hovered screenshot saving/clipboard transfer and
  Ctrl+W/Ctrl+Shift+T with retained inspection state. Paging and idle-resource
  regressions pass.
- Matched captures and renderer reports are in
  `/tmp/asset-preview-hover-proof/before/` and `after/`: four hovered content
  types, minimum/large grids, comparison and the View menu. Final scene reports
  identify NVIDIA GeForce RTX 2080 Ti/PCIe/SSE2; card titles keep identical styling.
- Automated windows remained inside Gamescope headless. Generated screenshot
  fixtures were removed by exact filename; no release package was produced.
- The desktop shortcut's checkout build was gracefully restarted with all six
  registrations, selection, layout, comparison state and preferences retained.

UNVERIFIED

- Native Wayland, touch/controller, screen-reader and DPI-scale variants were not
  exercised in this pass. Existing native Wayland support limits still apply.
