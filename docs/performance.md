# Performance measurements

Measurements describe the development checkout on 2026-10-27/28 with Qt 6.12,
F3D 4 and NVIDIA GeForce RTX 2080 Ti, using headless Gamescope. They do not
describe the older downloadable release artifacts or every Linux/GPU setup.

The 2026-10-27 optimization removed a repeated 25 ms wait between successful
Overview thumbnails. Initial and changed viewports retain their 25 ms debounce;
settled jobs yield for 1 ms. Preparation in those measurements was serial and low priority, with
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

## Bounded parallel thumbnails (2026-10-28)

The window-owned scheduler uses two low-priority image slots and one independent
finite model child. Images lead within each priority tier; quiet warming covers
nearby and remaining open registrations. Ordinary loading takes priority.
Background deadlines cannot delay foreground continuations, image completion
avoids rebuilding watchers, and Projects neighbor preload waits for 250 ms on one
target so a pointer sweep does not decode every full neighbor.

| Workload | Before | After |
| --- | ---: | ---: |
| First image after a preceding model registration | 1836 ms | 174 ms |
| All eight images in that fixture | 1971 ms | 340 ms |
| Model thumbnail in that fixture | 1822 ms | 2505 ms |
| Mixed-workload ping p95 | 3.38 ms | 3.32 ms |
| Mixed-workload parent PSS peak | 53.1 MiB | 60.5 MiB |
| 96-image registry: visible Overview readiness | 264 ms | 200 ms |
| 96-image registry: return to Overview | 83 ms | 63 ms |
| Thumbnail files after the quiet background phase | 12 | 96 |

The mixed fixture uses one model registered before eight 1024px images, three
runs per binary in before/after/after/before/before/after order. The navigation
fixture uses 96 1024px PNGs and two runs per binary in before/after/after/before
order. Both use fresh private state/cache, retain OS/driver caches and desktop
load, and verify the actual RTX 2080 Ti renderer. Mixed cache bytes are identical
(384,000); parent PSS includes Qt and native allocations outside retained-cache accounting.
The extra allocation was not separately attributed; child/GPU peaks were not
separately measured.

The mixed case ran under substantial CPU contention (load around eight on eight
logical CPUs); model-thumbnail completion can be slower when images take
priority. Projects timings vary between runs, so no standalone Projects or model
speedup is claimed. Socket p95 is event-loop RTT, not display cadence. Raw
profiles, binary hashes, busy-host samples and review remain under
`.local/thumbnail-queue/` and `evidence/thumbnail-queue/`.

For the mixed comparison, keep a saved prepared server with matching backend
modules beside it, then run:

```sh
tools/offscreen.sh python tools/profile_thumbnails.py \
  --baseline /absolute/saved-build/asset-preview-server \
  --output /tmp/asset-preview-mixed-profile
```

## Mouse interaction (2026-10-28)

The first interaction pass used F3D on the render worker, with completed CPU
images published to Qt. At that point, the dispatcher yielded to a render/readback
between each queued mouse command. A busy renderer therefore replayed old
pointer positions long after the pointer stopped. Pending motion now runs in
ordered batches, capped at 256 commands or 4 ms and stopped by a same-view
non-motion command. Priority lookup also avoids repeated full-queue scans.

A matched before/after/after/before run on the registered angel GLB, RTX 2080 Ti
and private native Wayland compositor produced these two-run medians:

| Input | Catch-up after input, before → after | Rendered frames, before → after |
| --- | ---: | ---: |
| 96 queued moves | 1597 → 39 ms | 96 → 2 |
| 96 moves at 125 Hz | 375 → 17 ms | 96 → 53.5 |

Catch-up ends at Qt's repaint admission for the completed final image. It is
not physical mouse-to-photon latency. The imported scene/settings/viewport and
native backend are identical; final camera values and frame pixels match
exactly across all runs. Load averages were about 6–8 on eight logical CPUs;
there is no universal latency bound. Native render/readback, Qt/compositor
presentation and a currently executing non-interruptible import still have
their own costs. CPU/PSS/GPU peaks were not measured in this interaction run.
Raw results, hashes, captures and review are in `evidence/model-interaction/`;
the prepared baseline and matched runner are in `.local/model-interaction/`.

`tools/test.sh async-loading` also reproduces the defect using artificial slow
frames, mixed orbit/pan/wheel input, a Fit barrier and release persistence. The
old dispatcher fails by rendering intermediate positions; the patched dispatcher
renders three frames, preserving all 96 deltas and their exact command order.

For an opt-in hardware interaction profile of the current build, run:

```sh
tools/test.sh model-interaction-gpu /absolute/model.glb stream /tmp/final-frame.png
```

Use `burst` for queued motion. The helper verifies a hardware renderer, reports
the final camera/frame hash and checks that static rendering stops. Native
Wayland matched measurements use the existing private KWin harness with DISPLAY
removed and `QT_QPA_PLATFORM=wayland`, separately from the X11 input test lane.

## Cache indexing, watcher diffing and thumbnail queue lookup pass (2026-10-28)

High-cardinality registries (up to 256 open previews) previously incurred
repeated linear and quadratic scans across cache lookups, thumbnail scheduling,
overview tile updates and filesystem watcher synchronization:

- `PreviewCache` indexed its LRU `std::list` with a `QHash` key map for $O(1)$
  `peekThumbnail`, `thumbnail`, `hasImage`, `take` and replacement lookups, and
  skipped redundant `QImage::scaled` / `convertToFormat(Format_RGBA8888)` passes
  when worker-decoded thumbnails already matched target bounds and format.
- `ThumbnailQueue::work()` and `demand()` replaced $O(N^2)$ `QStringList::contains`
  membership checks and redundant `QMap` value copies with `QSet` tracking and
  direct iteration, and gated `attempted` key-intersection cleanup on slices
  that actually prepared keys.
- `OverviewGrid` replaced the $O(N^2)$ key-retention loop in `setItems()`,
  avoided full-grid `visibleIds()` rebuilds on single-tile thumbnail callbacks,
  and memoized `projectIcon()` in a bounded 256-entry cache.
- `Service::watchers()`, `Service::select()` and `PreviewWindow::prepareThumbnails()`
  deduplicated parent-directory `stat` walks, replaced $O(N^2)$ watcher list
  diffs with `QSet` lookups and batched `removePaths()`, skipped redundant
  watcher rebuilds on active-card hover when preload targets were unchanged,
  and grouped project members in a single pass.
- `ImageView::paintEvent()` replaced per-cell `fillRect` loops for checkerboard
  backgrounds with a static 40×40 tiled `QPixmap`.

Matched `before → after → after → before` runs under headless Gamescope on
RTX 2080 Ti (`tools/profile_responsiveness.py --count 256 --edge 1024`, two
runs per binary; `tools/profile_thumbnails.py`, three runs per binary):

| Workload / metric | Baseline median | Candidate median | Change |
| --- | ---: | ---: | ---: |
| 256-image `overview-all-open-warm` completed | 2342.78 ms | 2071.47 ms | −11.6% (−271.3 ms) |
| 256-image `overview-all-open-warm` CPU time | 2.595 s | 2.240 s | −13.7% (−0.355 s) |
| 256-image `overview-hover` ping max | 23.63 ms | 13.47 ms | −43.0% |
| 256-image `projects-hover` ping p95 | 9.53 ms | 6.74 ms | −29.3% |
| `reconcile` `watch_cache_trim_ms` (`projects-cold-16`) | 3.010 ms | 1.394 ms | −53.7% |
| `reconcile` `watch_cache_trim_ms` (`projects-warm-16`) | 3.271 ms | 1.624 ms | −50.4% |
| Mixed fixture `first_image_ms` | 102.03 ms | 96.52 ms | −5.4% |
| Mixed fixture `all_thumbnails_ms` | 1105.96 ms | 1066.71 ms | −3.5% |
| Mixed fixture CPU time | 0.25 s | 0.23 s | −8.0% |

## Direct GPU preview transport (2026-10-28)

Live F3D views now render to shared GPU textures and display through native
OpenGL windows. They do not read, flip, transfer and upload a CPU image for every
frame. At most three targets belong to each visible view; a replaceable mailbox
and GL fences preserve freshness and ownership. Captures read the completed
texture, and departure thumbnails downsample on the GPU before their small
readback. Full metadata publishes once per motion batch, drag camera updates
follow rendered frames, and gestures pause background thumbnail warming.

The registered textured angel was exercised under private native Wayland on the
RTX 2080 Ti. Two CPU-reference and two GPU runs used the same current F3D backend,
asset/settings, 1000×680 viewport and profiling driver in before/after/after/before
order. The reference uses the prior CPU adapter with corrected readiness
accounting. Final cameras and every captured RGB pixel match exactly.

| Observation | CPU reference | GPU transport |
| --- | ---: | ---: |
| Application pixel readback during live input | One image per rendered frame | Zero |
| Post-input catch-up, 96 queued moves | 29 ms | 18 ms |
| Post-input catch-up, 96 moves at 125 Hz | 4 ms | 6 ms |
| Departure thumbnail readback | Full image already on CPU | 150,400 bytes, without rerender |

These are two-run medians, not physical input-to-photon measurements. The CPU
endpoint is QWidget repaint admission; the native GPU endpoint is frameSwapped.
Compositor presentation, scanout and physical input latency remain unmeasured,
so the timings describe this finite harness rather than an identical display
clock or universal speedup. Shader/filesystem caches and other desktop load are
retained. The corrected run does not establish a sustained-input latency win;
the two GPU stream tails were 0 and 11 ms. GPU runs allocated two to three of
three allowed targets, with 5,440,000–8,160,000 bytes of color texture storage.
Depth/stencil, context/driver memory and CPU/PSS
peaks were not measured; there is an extra display GL context. No memory-saving
claim follows from removing the CPU image path.

The artificial slow-frame regression preserves 96 mixed orbit/pan/wheel deltas,
Fit ordering and release persistence with three frames and six metadata
publications. Native tests cover overlays, rapid modified double-click selection,
camera/keyboard focus and synchronized views. A stopped background model child is
cancelled by live input and retried after quiet. Raw captures, parity results,
profiles and review stay under `evidence/direct-gpu-*`; prepared binaries and
the matched driver are under `.local/direct-gpu/`.

The initial texture-only checks missed a native presentation regression:
the shared EGL display context selected `GL_NONE` for its draw buffer, and
F3D's background alpha exposed the underlying widget. Display painting now
selects the window color buffer and preserves opaque output alpha. The Wayland
lane compares actual compositor screenshots with source captures; texture
parity and `frameSwapped` are not evidence that a user can see the scene.
The table uses the rerun in `evidence/direct-gpu-profile-visible`, after the
display correction and compositor checks. Earlier GPU timings are superseded.

Video/libmpv and component inspection already display through native GL surfaces.
Their readbacks remain explicit captures. Images decode once and paint their
retained pixels, and audio/waveforms update from events. Heavy in-progress model
imports still occupy the shared render worker; this change does not make those
imports interruptible or remove that remaining source of cross-view delay.

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
