# Asset Preview Specs Map

`specs/` is committed project memory for current intent and implementation. Read the relevant spec before changing its owning behavior, contract, boundary, source area, or verification flow, and update it with the implementation.

## Spec Map

| Spec | Owning sources | Scope | Read when |
|---|---|---|---|
| [Runtime and connections](runtime.md) | `src/main.cpp`, `src/service.*`, `bin/`, `fish/`, `skills/asset-preview/`, `tools/install.py`, `play.sh`, `icon.svg` | Singleton service, socket/state, explicit generators, agent skill and desktop/shell integration | Changing lifecycle, commands, persistence or installation |
| [Preview lifecycle](preview.md) | `CMakeLists.txt`, `src/viewer.*`, `src/model_view.*`, `src/triangle_counts.h`, `src/video_view.*`, window/watch paths in `src/service.cpp`, `tests/`, `examples/`, `tools/profile.py`, `tools/ui_scenes.py` | Visible-only loading, lazy native backends, video playback, event refresh, controls, imported/material semantics and verification | Changing rendering, navigation, dependencies, performance or refresh behavior |
| [Distribution](distribution.md) | `packaging/`, `.github/workflows/release.yml`, `CMakeLists.txt`, `THIRD_PARTY_NOTICES.md` | Container, standalone bundle, dependency licensing and release gates | Building or publishing a release |

## Maintenance

- Keep specs compact, evidence-based, and current.
- Update this map when a spec is added, moved, split, or removed.
- Keep plans, TODOs, work logs, and merge handoffs outside `specs/`.
- Label planned behavior and open decisions; do not present them as implemented facts.
