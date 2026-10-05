VERDICT: APPROVE

## SCOPE

- Native Qt single window, image/model/material cards, navigation, per-view
  options, compact layout and current-page resource lifecycle.
- Matched 2×2 scenes at 1000×720 and 700×500, plus compact 3×3 at 1000×720 and
  4×4 at 1600×1000. Long labels, empty and missing-output states inspected.
- Mouse orbit/clicks and keyboard navigation/menu activation exercised through
  XTest inside Gamescope headless. Actual renderer: NVIDIA GeForce RTX 2080 Ti.

## FINDINGS

1. `src/model_view.cpp`: unrestricted incremental elevation could roll the floor.
   Default turntable orbit now enforces world-up yaw and bounded pitch, so the
   ground remains level through repeated steep drags. Y-up and Z-up supported.
2. `src/model_view.cpp`: imported models lacked the material preview's environment
   lighting. Studio mode applies F3D's embedded HDRI with normal lighting and tone
   mapping to all 3D views; the plain F3D light kit remains selectable.
3. `src/service.cpp`: inspecting geometry required retaining authored shading.
   Materials/textures toggles now offer neutral clay and texture-free inspection;
   re-enabling restores appearance through a camera-preserving scene reload.
4. `src/service.cpp`: two toolbar rows and external card chrome consumed preview
   area. One global toolbar and small native overlays retain discoverable actions,
   elided names/tooltips, and status inside each view. Compact hides steady Live
   status while retaining waiting/building/failure information.
5. `src/service.cpp`: only four previews could be compared. Opt-in 3×3 and 4×4
   grids page 9/16 entries and retain off-page unloading. Layout/compact settings
   persist and existing version-1 registries keep their previous defaults.

## CHANGES

- Added horizon/up-axis, lighting, materials and textures settings plus per-view
  menu controls. Camera state survives refresh and display-mode changes.
- Added grid size/compact controls, CLI flags, fish completion and persistence.
- Added isolated UI replay and meaningful regression checks; updated protocol,
  specs, renderer rationale, README and installed skill's canonical instructions.
- Preserved one native window, explicit finite generators, file-save watching,
  last-good content, visible-only activation and event-driven rendering.

## VERIFICATION

```sh
./tools/build.sh
python tests/integration.py
./tools/offscreen.sh env ASSET_PREVIEW_GPU_TEST=1 \
  ASSET_PREVIEW_EVIDENCE_DIR=evidence/gpu python tests/integration.py
ASSET_PREVIEW_TEST_WIDTH=1600 ASSET_PREVIEW_TEST_HEIGHT=1000 \
  ./tools/offscreen.sh python tools/ui_scenes.py --phase after --output evidence/ui
fish -c 'source fish/completions/asset-preview.fish; complete -C "asset-preview layout grid --size "'
git diff --check
```

- Functional: 13 passed, 3 hardware-only skips. Hardware: all 16 passed.
- Orbit tests verify zero horizon roll, constant radius, pole clamping,
  camera retention and Z-up. A real mouse click opens the native overlay menu;
  keyboard activation toggles textures, proving controls receive input over GL.
- Pixel checks verify different texture/clay appearances and restored authored
  color, with unchanged camera. Settled image and 3D views have no idle paints.
- Paging verifies 4/9/16 limits, off-page unload, compact status visibility,
  validation and restart persistence. Generator lifecycle tests remain passing.
- Matched evidence: [before normal](before-normal-2x2.png) /
  [after normal](after-normal-2x2.png), [before small](before-small-2x2.png) /
  [after small](after-small-2x2.png). Full model surfaces at 1000×720 changed from
  456×211 to 487×319; overlays occupy part of those surfaces.
- Additional evidence: [3×3](after-compact-3x3.png), [4×4](after-compact-4x4.png),
  [clay](after-clay.png), [missing output](after-compact-waiting.png),
  [empty](after-empty.png). Scene inputs are hashed in [before](before.json) and
  [after](after.json); generated fixtures were removed after the runs.
- Applied the build to the already visible desktop service after a graceful
  window close saved geometry. Current registrations, selection and layout
  retained; no active generator was interrupted. Automated scenes stayed offscreen.

## UNVERIFIED

- Native Wayland embedding remains unsupported. Touch, controller, screen-reader,
  localization expansion and every OS/theme/scale combination were not tested.
- Authored vertex-color neutralization, all imported shader/material variants and
  authored-light scenes are not covered by the clay/material pixel fixture.
- Sixteen simultaneous complex 3D scenes are not a memory/frame-time benchmark;
  larger grids allow more active resources. Import remains on the GUI thread.
- No exact Godot shader match or universal minimum resource footprint is claimed.
