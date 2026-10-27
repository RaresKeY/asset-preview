# Bounded preview cache and overview

[Projects](projects.md) owns persistent agent grouping, chosen current previews,
the interactive Projects grid and Overview grouping/filtering.

Owners: `src/preview_cache.*`, `src/preview_preload.*`, `src/overview_grid.*`, `src/thumbnail_job.*`, cache/layout routes in
`src/service.*`, image reuse in `src/viewer.*`, waveform reuse in
`src/audio_view.cpp`. Verification: `tests/preview_cache.cpp`,
`tests/overview_integration.py` and existing lifecycle lanes.

`src/cache_diagnostics.*` owns bounded cache/reclamation telemetry. The optional
3D/media adapters report through the service-owned `CacheStore` interface.

## RAM

One service-owned LRU retains successfully displayed standalone image pixels,
eligible return-target image preloads and completed audio waveform envelopes
when views unload. Admission uses actual
payload bytes: 32 MiB each, 128 MiB total, 256 entries. Taking a payload transfers
ownership/accounting to the visible view. Active views, temporary decodes,
Qt/driver allocations and loaded libraries are outside this retained-payload
budget. Engines, players, full PCM and generators are never retained.

Keys include normalized path, device/inode/mode/size, nanosecond mtime/ctime and
algorithm version. Images recheck identity after decode and before retention;
waveforms also check duration. Sidecar-dependent/unsupported image readers bypass
reuse. Explicit Reload bypasses payload reuse. Failed/raced replacements cannot
cache previous pixels under a new identity. Existing scalar metadata caches keep
their own bounds.

Thumbnails share the RAM pool with a 16 MiB subquota and 256 KiB item cap; actual
stored pixels are at most 160×100 RGBA8 (64 KiB), fitting all 256 possible
tiles inside the 16 MiB thumbnail budget. Legacy larger derivatives shrink once
on lookup. Thumbnail admission evicts inactive payloads before thumbnails when
the shared byte/entry limit is reached. The overview does not retain a widget,
pixmap or engine per registration. RAM clears on hide/minimize, Close all and
View > Clear Image and Thumbnail Cache. Linux MemAvailable is checked on cache
transitions, clearing/refusing retention below the greater of 256 MiB or 5% of
physical RAM. There is no memory polling timer.

## Return-target preload

The visible window prepares the single inactive preview that closing the current
selection would show. It follows Settings > After Closing a Preview: left
neighbor by default, or last viewed survivor; both fall back to the right neighbor
when there is no left neighbor/history. An already visible target needs no worker.
Overview, hidden/minimized windows, an empty registry and explicit cache clearing
cancel publication. Clear does not immediately refill the cache.

One service-owned worker runs at a time, with one replacement target and
cooperative cancellation between reads. It decodes supported standalone images
using the ordinary image identity, auto-transform and 4096-edge settings, only
for regular files at most 16 MiB and at most a 32 MiB estimated full raster.
Decoded payloads enter the existing 32 MiB/item, 128 MiB RAM pool; existing
payloads skip work. Other regular files warm at most their first 32 MiB in the
OS file cache using a 64 KiB scratch buffer. No raw model/media bytes are retained
in the service; no hidden engine, player, waveform generator or asset command runs.
Model/media opening still performs its ordinary visible import/player setup.
Memory pressure skips speculative work and existing cache admission rules apply.

The preload worker has low thread priority. For models the same aggregate 32 MiB
read budget includes discovered sidecars (up to 64 paths / 16 KiB of path text),
instead of warming only a small glTF manifest. Source and sidecar fingerprints
invalidate the attempt. `list.preload` also reports file count, completed bytes
and elapsed ms; it never creates a hidden renderer.

Source identities are rechecked before publishing; navigation, closing, hiding,
preference changes and clearing discard obsolete results. The target's source
and nearest existing parent join the event-driven watcher set so replacement or
later creation can trigger one fresh attempt. Unrelated directory events do not
restart an unchanged attempt. There is no preload polling or recurring timer.
`list.close_target` reports the close destination and `list.preload` reports the
speculative target/state, worker activity and byte limits. Finite `preload` cache
events describe completion/discard, bytes and elapsed microseconds. Functional
and native menu/keyboard checks live in `tests/overview_integration.py`.

## Disk

Qt's private application cache contains two partitions totaling 256 MiB:

| Partition | Total | Per output | Count |
|---|---:|---:|---:|
| Existing GLB derivatives | 224 MiB | 96 MiB | 2,048 files |
| Thumbnails/waveforms | 32 MiB | 1 MiB | 1,024 files |

Before small-cache admission, an older recognized texture cache is trimmed under
its importer lease to reserve the partition. Contention, unrecognized or unsafe
roots disable disk reuse without affecting previews. Tests isolate both roots
with `ASSET_PREVIEW_SMALL_CACHE` and `ASSET_PREVIEW_TEXTURE_CACHE`.

Small derivatives use versioned names, typed bounded payloads, SHA-256 integrity
and atomic writes. Raw pixels/envelopes avoid compressed-media decode. Private
roots, ownership markers, regular-file checks, nonblocking leases and exact
filenames constrain deletion. Initial-use/write pruning removes oldest uses,
entries unused for 30 days and excess bytes/count. No cleanup timer runs.
Corruption/unavailability is a miss. Original assets, reviews and registry files
are never cache contents. The clear action clears RAM and small derivatives;
GLB derivatives retain their separate pruning policy.

`list.cache` and `cache` (`action: stats` / `clear`) report bytes/limits,
thumbnail bytes, hits/misses, evictions and disk observations. Disk counters
describe the last bounded scan/publication, not a recurring scan.

## Churn and reclamation diagnostics

`asset-preview cache` summarizes retained bytes and likely churn sources;
`asset-preview cache events --limit 64 --after 0 --json` reads recent events.
These commands neither start nor raise the viewer. The socket `cache` action
`events` accepts integer `limit` 1..128 and nonnegative integer `after` sequence.
`list.cache.diagnostics` includes counters, the eight strongest recent suspects,
limits, dropped-history/event counts and logging errors. Reading it does not
generate cache events or scan files.

Events identify the source (last 256 characters), kind, exact cache identity,
UTC time, session sequence/PID, tier, operation, reason, payload bytes and work
microseconds where measured. RAM admissions/takes/hits/misses, replacements,
evictions/clears, disk hits/writes/identical-write attempts/prunes, image/waveform
decode, thumbnail generation/discard and glibc heap trimming are covered.
Removal reasons distinguish RAM bytes, entry count, thumbnail quota, disk quota,
expiry, memory pressure, hiding, Close all and explicit clearing. A heap trim's
boolean result is recorded; no reclaimed RSS/VRAM byte count is inferred.

Likely churn includes eviction/removal without a hit, a miss within 30 seconds
of removal, and re-admission within 30 seconds of eviction/pruning. Normal take
and return within two seconds has a separate counter, not a churn score. Per-key
history holds 128 tier/identity records, so older churn may fall outside that
bounded history; drops are visible. Metadata labels also have a 128-item bound.

The event ring holds at most 128 records / 64 KiB serialized bytes. Repeated hit
lookups increment counters without allocating event JSON or emitting records;
repeated misses for a key are sampled once per second except a new refault.
Prune scans report duration/files removed, including scans that free nothing.
Summary counters are lifetime totals for this service session; they are distinct
from the bounded event tail and per-key history.

`STATE/cache-churn-v1.jsonl` and one `.1` backup are each at most 256 KiB, separate
from asset-cache storage. A private 32 KiB pending batch flushes on one finite
250 ms timer after activity, with no fsync per event and no idle polling or
recurring flush. Overflow is counted. Shutdown flushes the final bounded batch.
Only recognized, private, same-user regular log files may be appended/rotated;
foreign/unsafe files are preserved and failures appear in diagnostics. Log lines
contain only metadata, never pixels, geometry or PCM. Cache policy is unchanged
by the observer. `tools/test.sh cache-diagnostics` covers classification, limits,
hit suppression, batching/rotation and ownership; integration checks the CLI,
reclamation records and absence of continuing idle logging.

## Overview

The layout picker, View > All Open Previews, Ctrl+0 and `layout overview` select
one compact scrollable view of all registrations, including missing outputs.
It persists separately from live 2×2/3×3/4×4 grids, activates zero ordinary
previews and stops their players/generators. Click, Enter or Space opens a tile
in single view through atomic `zoom`. Arrows/Home/End and scrolling navigate;
failed saves preserve selection/layout. Palette styling, elided labels and full
path tooltips follow the application.
Top-right checkboxes and Ctrl/Shift selection share the live cards' session-only
close selection described in [Preview lifecycle](preview.md). Modifier clicks
and checkbox clicks keep the overview open without activating any ordinary view.
Ctrl+Space toggles the keyboard target. Checked state survives scrolling and
layout changes; thumbnail refreshes do not reset it.

On departure, a successfully displayed current preview contributes one frame
before its visible engine/player is destroyed. GPU readback is finite and refused
above a 16 MiB RGBA surface estimate; images reuse decoded pixels. No idle copies.
Keys include source/dependency fingerprints and visual settings. Unknown-format
dependency discovery remains best effort; thumbnails are navigation aids.

Visible missing thumbnails prepare lazily, one job at a time at low priority.
Self-contained raster inputs are limited to 16 MiB files and a 32 MiB decoded
raster estimate, with 160×100 cached output. Unvisited models and material samples use
one task-owned `--thumbnail-job` child of the prepared server binary, sharing the
ordinary F3D importer/plugins, visual settings and camera conventions. It renders
one offscreen 256×160 frame without opening a desktop window, playing animations,
activating registrations or executing asset commands. The child runs at nice 19
and idle I/O priority with one-thread hints, a 20/25-second CPU limit and a
30-second parent deadline; inputs retain the ordinary 256 MiB model-file bound.
Each job releases its engine on exit; successful frames enter the existing
bounded RAM/disk cache. There is no retained speculative renderer.
The child has a parent-death kill signal; failed-attempt bookkeeping is bounded
to one current identity per registration.
Job scratch lives in private `thumbnail-jobs/job-XXXXXX` directories beside the
service socket, under its lifetime `server.lock`. Children use their job directory
as TMPDIR so cancelled material imports leave no loose temporary GLBs. Normal
completion/cancellation removes the complete job; the next locked service startup
reclaims up to 256 recognized private orphan jobs after an abrupt service exit.
The child's application cache identity matches the service, so texture derivatives
reuse the same bounded partition rather than creating a separate cache tree.

Scrolling or resizing a tile away, filtering/removing/replacing its source, leaving Overview,
hiding or minimizing cancels the exact owned child group and rejects stale output.
Results recheck source/dependency/settings identity before publication. A failed or
timed-out identity is attempted once until invalidation; cancellation can retry
when visible again. Unvisited video/audio/graph and unsupported assets retain a
kind placeholder until an ordinary view supplies a frame. No thumbnail starts a
player or asset generator. Workers and single-shot dispatches settle without idle
rendering/polling. `presentation.overview` reports the current thumbnail child,
serial worker limit, completed renders, renderer identity and last render error.

Only visible thumbnail sources/dependencies and nearest parent directories are
watched; no recursive registry-wide scanning. `presentation.overview` exposes
tile geometry, count and worker state for native verification.

Initial/invalidated viewport work retains a 25 ms debounce. Successfully completed
jobs dispatch the next visible thumbnail after a 1 ms yield, keeping serial
low-priority work responsive without repeating the viewport debounce per tile.
Cancellation, rejected results and failures retain the ordinary debounce.
Viewport activity replaces any pending fast continuation and records its quiet
period even during a worker job; completion respects a still-pending debounce.
Hiding or minimized-window invalidation stops queued dispatch immediately.
`tools/test.sh overview-dispatch` checks refresh during decoding and between jobs,
hide/reentry and minimize/restore.

Run `tools/test.sh preview-cache`, `overview`, `overview-gpu`, `functional` and
the existing GPU lifecycle/texture lanes. Evidence goes under
`evidence/cache-overview/`; verify the actual hardware OpenGL renderer.

## Responsiveness measurements

Overview painting uses memoized visual keys and RAM-only thumbnail lookups. File
fingerprinting, disk lookup and diagnostic events run on finite refresh/work
transitions, never on hover repaint. Image unloads reuse an existing identical
thumbnail instead of rescaling and rewriting it. Only the active thumbnail tile
animates a small spinner; the timer stops when work finishes or Overview hides.
Image/model cards use ordinary Qt overlays; native overlays are reserved for
embedded media and component inspection surfaces. Project cards
are created directly in their owning pane. Image workers prepare 160×100 RGBA
thumbnails during decoding, so unload does not scale full-resolution pixels on
the UI thread. Cache-hit images reuse existing thumbnails or let Overview prepare
a missing one asynchronously.

`tools/profile_responsiveness.py` runs a private 96-registration workload under
`tools/offscreen.sh`: cold/warm 16-project loads, Overview hover/return, left-return
preloads and optional real-model first/reopen. It records completed-load wall
and CPU time, PSS/RSS, cache/preload counters and concurrent socket ping latency.
Ping RTT measures event-loop service latency, not display frame time. `--perf`
adds attributed samples; those runs are separate from clean timing comparisons.
The script owns its state/cache/process and never invokes asset generators.

View admission yields between batches of at most two new cards / approximately
8 ms of construction work. Each finite callback recomputes visibility, so
navigation/hide cancels undisplayed work. Project panes show loading feedback
while waiting for their card. Hover redraw touches only changed tiles; finite
thumbnail work memoizes keys until a source/layout/viewport refresh.

[Performance notes](../docs/performance.md) summarize
matched 96/256-registration readiness, preparation latency tails and resource
bounds. The rejected project presentation cache is not part of the implementation.
