# Performance measurements

Measurements describe the development checkout on 2026-10-27 with Qt 6.12,
F3D 4 and NVIDIA GeForce RTX 2080 Ti, using headless Gamescope. They do not
describe the older downloadable release artifacts or every Linux/GPU setup.

The follow-up optimization removes a repeated 25 ms wait between successful
Overview thumbnails. Initial and changed viewports retain their 25 ms debounce;
settled jobs yield for 1 ms. Preparation remains serial and low priority, with
no ordinary views, players or generators activated by Overview.

| Registered assets | Visible thumbnails prepared | Median readiness before → after |
| --- | --- | --- |
| 96 images, 2048×2048 | 12 | 734.5 → 471.9 ms (36% shorter) |
| 256 images, 1024×1024 | 16 | 716.4 → 436.1 ms (39% shorter) |

Each binary ran twice per workload, in before/after/after/before order, using
fresh private application state and caches. Filesystem/shader caches and other
desktop load were retained. These small samples are observations, not a general
performance guarantee. Measurements precede the subsequent debounce correctness
fix; no additional speedup is claimed for that fix.

Denser preparation also increases socket ping p95 from 4.8 to 8.4 ms and from
9.3 to 16.5 ms respectively. The larger workload's median worst ping increases
from 122 to 164 ms. Socket round trips measure event-loop service, not display
cadence. This improves readiness without establishing better latency tails.

Project, startup, model and return-preload results vary between runs and workload
sizes; no further speedup is claimed for those paths. A project presentation
cache was tested and removed because completed-load results were inconsistent.
The retained direct project-count accessor removes unnecessary geometry JSON
construction without a standalone timing claim.

## Reproduce

Build with `tools/build.sh`, then use a fresh output directory:

```sh
tools/offscreen.sh python tools/profile_responsiveness.py \
  --output /tmp/asset-preview-profile --count 96 --edge 2048
```

Use `--count 256 --edge 1024` for the larger registry. `--binary` selects a saved
build; optional `--real-model` adds model first-load/hide/reopen measurements.
`--perf` collects attribution separately from clean timings. The harness records
completed-load timing, CPU, PSS/RSS and cache/preload counters, and cleans up its
owned service. Run only one measurement workload at a time.

Raw captures, profiles and reports stay local: they can contain absolute asset
paths and process details. Deterministic scheduling checks run with
`tools/test.sh overview-dispatch`; hardware Overview and Projects checks run with
`tools/test.sh overview-gpu` and `tools/test.sh projects-gpu`.

Model import and rendering already run in C++. Remaining native import/first
render costs and GUI layout work need further measurement; moving the Python
command client to C++ would not address those measured paths.
