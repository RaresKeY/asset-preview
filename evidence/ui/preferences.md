# Remembered settings and Close all — October 6, 2026

The Release build completed without compiler warnings. Automated views stayed
inside Gamescope's headless backend with Qt xcb/XWayland and KDE Breeze Dark;
the model renderer reported NVIDIA GeForce RTX 2080 Ti/PCIe/SSE2.

The [700×500 grid](preferences-close-all.png) shows the Close all button fitting
the existing toolbar. All 11 captured scenes passed overlay bounds and overlap
checks. A native synthetic click on Close all cleared an isolated registry while
keeping the window visible and its preferences saved. The next hardware model
inherited the saved settings, including a hidden triangle badge. That click used
XSendEvent with `QT_XCB_NO_XI2=1`; physical mouse input was not tested.

- `./tools/test.sh functional`: 24 cases, 18 passed and 6 hardware-only skips.
- `./tools/test.sh gpu` with integration tests 10, 16, 21, 22, 23 and 24: all
  six passed. Covers hardware rendering and idle paints, appearance and camera
  retention, triangle overlays, remembered settings, failed-save rollback, and
  cancellation of generators and their descendants.
- Preference coverage includes all supported toggles, restart and new-preview
  inheritance, explicit registration overrides, same-kind reconnection,
  material shape inheritance through the CLI, and invalid-state preservation.
- Real fish completion offered `close-all`; the canonical asset-preview skill
  passed skill-creator's quick validation.

Visual replay: `ASSET_PREVIEW_TEST_WIDTH=1600 ASSET_PREVIEW_TEST_HEIGHT=1200 ./tools/offscreen.sh env QT_QPA_PLATFORMTHEME=kde QT_STYLE_OVERRIDE=breeze python tools/ui_scenes.py --phase after --output NEW_DIRECTORY`.
