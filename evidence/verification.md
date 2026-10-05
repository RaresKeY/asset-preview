# Verification — October 5, 2026

Release build on the owning CachyOS workstation, Qt 6.11.2 and F3D 3.5.0.
Automated visuals stayed inside Gamescope's headless backend. The loaded view
reported **NVIDIA GeForce RTX 2080 Ti/PCIe/SSE2**, using Qt `xcb` through XWayland.

`./tools/test.sh functional`: 14 tests, 12 passed and 2 hardware-only skips.
`./tools/test.sh gpu`: all 14 passed. Coverage includes concurrent startup, private
socket, inert registration, visible-only paging, atomic replacement, missing files
and directories, failed-save recovery, generator coalescing and descendant
cancellation, timeouts, persistence, read-only completion, corrupt-state preservation,
OBJ/MTL/material-map refresh, keyboard navigation, native model orbit, idle paint
counts and saved intermediate outputs during a running generator.

The fish function/completions and managed desktop entry are installed. Normal fish
autoload resolves `asset-preview`; layout completion returns `single` and `grid`.
The desktop entry passes `desktop-file-validate` and KDE's application cache was
refreshed. The real desktop session's background service reports `platform=xcb`,
zero active entries and a hidden window. No visible automated launch was used.

## Measured resources

Current [raw measurements](profile/performance.json) use one fresh process and the
same sequence of settled states. Each sample measures a 1.5-second idle interval
after a short settling period; `/proc` CPU ticks and view paint counters are sampled
without enabling a render loop. RSS/PSS are process memory, not dedicated GPU memory.
PSS apportions shared pages and depends on other running processes.

| State | Active | RSS MiB | PSS MiB | Idle CPU seconds | Additional paints |
|---|---:|---:|---:|---:|---:|
| Fresh background | 0 | 48.0 | 13.3 | 0.01 | 0 |
| 256px image | 1 | 65.8 | 20.5 | 0 | 0 |
| Textured sugar-cube GLB | 1 | 412.2 | 299.0 | 0 | 0 |
| Walnut material, three 2048px maps | 1 | 415.2 | 302.0 | 0.01 | 0 |
| Four-card grid | 4 | 493.1 | 379.9 | 0 | 0 |
| Hidden after using 3D | 0 | 327.2 | 212.7 | 0 | 0 |

The lazy backend keeps F3D/VTK out of fresh background and image-only sessions.
3D loading brings substantial library and GPU-driver memory. Hiding destroys all
engines/images/generators and trims free heap pages, but retains the backend and
driver caches. Stop/restart returns to the fresh-process baseline. These short
idle measurements establish event-driven behavior; they are not a benchmark of
large imports, interactive frame times, or every supported scene format.

Visuals: [four-card grid](profile/grid.png), [textured model](profile/model.png),
[baked sphere](profile/material.png), [image](profile/image.png).
The separate [hardware test snapshot](gpu/hardware.json) records renderer identity
and map reload counts; its temporary fixture paths have been cleaned up.

Replay the real-asset measurement from the checkout:

```sh
./tools/offscreen.sh python tools/profile.py \
  --model ../material-atlas/assets/models/sugar_cube/sugar_cube_textured.glb \
  --albedo ../material-atlas/assets/materials/smoked_walnut_albedo.png \
  --normal ../material-atlas/assets/materials/smoked_walnut_normal.png \
  --orm ../material-atlas/assets/materials/smoked_walnut_orm.png \
  --output evidence/profile
```

## Current boundaries

Native Wayland embedding failed the 3D check (daemon exited while creating the
embedded view). The supported launcher defaults to `xcb`/XWayland; explicit Qt
platform overrides remain caller-owned. No native Wayland support is claimed.

F3D previews exported assets and baked maps. Godot shaders and Material Maker graphs
require their owning exporter/baker or a finite PNG capture command. The inspected
material appearance is not evidence of an exact Godot shader match. Imported scenes
can block the GUI during loading and have no guaranteed resident-memory ceiling.
The agent skill is now versioned in `skills/asset-preview/`; the maintained connection contract is in
[agent-flow.md](../docs/agent-flow.md) and [protocol.md](../docs/protocol.md).
