# Triangle counts and compact overlays — October 6, 2026

Verified in isolated Gamescope headless sessions, Qt xcb/XWayland, KDE Breeze
Dark, F3D 3.5.0, NVIDIA GeForce RTX 2080 Ti/PCIe/SSE2. The desktop service was
not used for captures. The owning Release build completed without compiler warnings.

The matched [before](triangle-overlays-before.png) and
[after](triangle-overlays-after.png) captures show the same assets at 1000×720.
Titles and live status occupy their text width at top left; controls occupy top
right; model/material triangle badges occupy bottom left. Compact mode retains
the counts, hides steady Live status and keeps waiting/errors visible. The
[small 4×4 grid](triangle-overlays-small-grid.png) at 700×500 verifies long-title
elision and reachable options. All 11 captured scenes passed bounds/overlap checks,
including normal/compact grids, waiting, empty, clay and texture/light modes.

Checks:

- `./tools/test.sh metadata`: passed. Covers source instances/scene selection,
  OBJ triangulation/continuations, GLB JSON, malformed inputs and size limits.
- `./tools/test.sh functional`: 21 cases, 15 passed and 6 hardware-only skips.
- `./tools/test.sh gpu PreviewIntegration.test_10_f3d_hardware_model_material_and_idle PreviewIntegration.test_16_material_modes_restore_authored_appearance_and_camera PreviewIntegration.test_21_triangle_counts_overlay_layout_and_refresh`:
  all 3 passed, including idle rendering, save refresh, last-good count retention,
  shape changes, toggling without reload/camera movement, and off-page unloading.
- The actual native options popup opened and its count mnemonic toggled the badge
  off and on without reloading. This isolated check used XSendEvent with
  `QT_XCB_NO_XI2=1`; XTest emulation failed in the installed Gamescope/libei path.
  Physical keyboard/mouse input and the complete hardware suite were not rerun.

Visual replay: `ASSET_PREVIEW_TEST_WIDTH=1600 ASSET_PREVIEW_TEST_HEIGHT=1200 ./tools/offscreen.sh env QT_QPA_PLATFORMTHEME=kde QT_STYLE_OVERRIDE=breeze python tools/ui_scenes.py --phase after --output NEW_DIRECTORY`.
Counts cover GLB/glTF, OBJ and material samples; other formats show unavailable
metadata. They describe source triangle primitives, not per-frame GPU work.
