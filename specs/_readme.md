# Asset Preview Specs Map

`specs/` is committed project memory for current intent and implementation. Read the relevant spec before changing its owning behavior, contract, boundary, source area, or verification flow, and update it with the implementation.

## Spec Map

| Spec | Owning sources | Scope | Read when |
|---|---|---|---|
| [Projects](projects.md) | `src/project_grid.*`, `src/project_identity.h`, project service/CLI and Overview routes | Persistent agent groups, current-preview publication, filters and interactive project panes | Changing project ownership, sends or project layouts |
| [Cache and overview](cache-overview.md) | `src/preview_cache.*`, `src/preview_preload.*`, `src/cache_diagnostics.*`, `src/overview_grid.*`, `src/thumbnail_queue.*`, `src/thumbnail_job.*`, service/image/audio cache routes | Bounded RAM/disk reuse, neighbor preload, churn/reclamation logs and all-open thumbnail navigation | Changing caching, preloading, diagnostics, pruning or overview behavior |
| [Material graphs](material-graphs.md) | `src/material_graph.*`, `src/material_sample.h`, graph service/CLI routes | Live recipe connections, right inspector, material-on-model previews and shared Atlas workbench module | Working with live material graphs |
| [Runtime and connections](runtime.md) | `src/main.cpp`, `src/crash_log.*`, `src/service.*`, `src/theme.h`, `bin/`, `fish/`, `skills/asset-preview/`, `tools/install.py`, `play.sh`, `icon.svg` | Singleton service, crash-only diagnostics, socket/state, app appearance, explicit generators, agent skill and desktop/shell integration | Changing lifecycle, commands, persistence or installation |
| [Preview lifecycle](preview.md) | `src/service.*`, `src/viewer.*`, `src/dependency_metadata.h`, `src/gl_framebuffer.h`, `src/fullscreen_notice.*`, `tools/wayland-session.py`, `CMakeLists.txt` | Shared visibility, publication, native surfaces, camera/navigation, triangle badges, fullscreen and capture | Changing cross-type lifecycle or window behavior |
| [Visual references and Codex](references.md) | `src/codex_panel.*`, `src/reference_editor.*`, window routes and CLI origin metadata | Right terminal, current-view captures, annotations, clipboard and conversation ownership | Changing references or Codex integration |
| [Images](images.md) | `ImageView` in `src/viewer.*`, image routes in `src/service.cpp` | Image/texture decode, zoom/pan, filtering and proof captures | Working with still images or texture inspection |
| [Audio and music](audio.md) | `src/audio_view.*`, `src/audio_visualizer.*`, `src/audio_waveform.*`, `src/seek_slider.h`, `src/mpv_player.h`, `src/video_controls.*` | Audio readiness, transport, measured music visualization and last-good streaming | Working with audio, songs or sound effects |
| [Video](video.md) | `src/video_view.*`, `src/video_controls.*`, `src/mpv_player.h` | Video frames, transport, responsive controls, inactivity and fullscreen | Working with video or rendered animation captures |
| [Models](models.md) | `src/model_view.*`, `src/model_surface.cpp`, `src/model_worker.h`, `src/f3d_compat.h`, `src/gltf_import.*`, `src/gltf_pixels.h`, `src/texture_preview.*`, `src/triangle_counts.h`, model routes | Exported mesh import, shading, bounded texture previews and ordinary 3D inspection | Working with a standalone exported model |
| [3D scenes](scenes.md) | `src/model_view.*`, `src/dependency_metadata.h` | Complete scene exports, hierarchy/instances, lighting and scene boundaries | Working with an assembled 3D scene |
| [Modular assets](modular.md) | `docs/agent-flow.md`, `examples/connect_surface_fixture.py`, registration/generators | Kit/host connections, proof views and geometry-owner boundaries | Working with modular parts, assemblies, fit or seams |
| [Materials](materials.md) | Material sample/options in `src/model_view.cpp`, material CLI | Baked map conventions, generated sample meshes and shader boundaries | Working with a baked material sample |
| [3D animations](animations.md) | `src/model_view.*`, `src/animation_controls.*`, `src/animation_timeline.h`, animation tests | Skeletal/node/morph clips, transport, frame scheduling and pose verification | Working with rigged or moving 3D exports |
| [3D inspection and reviews](inspection.md) | `src/inspection/`, inspection service/CLI routes, `tests/inspection_*` | Optional component selection, source identity, cuts, visibility, durable review JSON and agent access | Changing the inspection workspace, review format or lifecycle |
| [Distribution](distribution.md) | `packaging/`, `.github/workflows/release.yml`, `CMakeLists.txt`, `THIRD_PARTY_NOTICES.md` | Container, standalone bundle, dependency licensing and release gates | Building or publishing a release |

## Maintenance

- Keep specs compact, evidence-based, and current.
- Update this map when a spec is added, moved, split, or removed.
- Keep plans, TODOs, work logs, and merge handoffs outside `specs/`.
- Label planned behavior and open decisions; do not present them as implemented facts.

[Surface authoring and live proof views](../docs/agent-flow.md) and [the finite fixture connector](../examples/surface_fixture.md) route real host GLBs, separate owner diagnostics, exact publication checks and renderer evidence; viewer counts never certify topology.
