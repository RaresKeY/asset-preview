# Video verification — October 5, 2026

The existing workstation supplied mpv 0.41.0 / libmpv client API 2.5.0,
Qt 6.11.2 and FFmpeg 9.0.2. No dependencies were installed. Automated visuals
used Gamescope headless with Qt xcb, NVIDIA GeForce RTX 2080 Ti OpenGL rendering
and **NVDEC hardware decoding**, reported separately by libmpv.

```sh
./tools/build.sh
python tests/integration.py
./tools/offscreen.sh env ASSET_PREVIEW_GPU_TEST=1 \
  ASSET_PREVIEW_EVIDENCE_DIR=evidence/video python tests/integration.py
# Fresh video-only resource snapshot and extended multi-player/page check:
./tools/offscreen.sh env ASSET_PREVIEW_GPU_TEST=1 \
  ASSET_PREVIEW_EVIDENCE_DIR=evidence/video python tests/integration.py \
  PreviewIntegration.test_18_video_playback_pause_seek_reload_failure_and_unload
python tools/install.py
```

Functional: 14 passed, 4 hardware-only skips. Full hardware lane: all 18 passed.
The final video-only replay also passed after extending it with concurrent players
and off-page unloading. Existing model/material rendering, orbit, native controls,
generators, file watching and state tests remained passing.

Video checks cover automatic types, inactive registration, streamed files above
the static asset cap, playback, muted defaults, actual decoder identity, pause
idle, seeking/validation, native keyboard focus and global navigation, atomic
replacement, malformed-save last-good retention/recovery, loop-off EOF, sound
preference changes while paused, capture pixels, two concurrent video cards in a
compact grid, off-page release and hidden idle. User mpv config/scripts are disabled.
The four-second 320×180 H.264/AAC fixture was generated with FFmpeg in a private
temporary directory; all files and its exact service were cleaned up.

Evidence: [replacement frame](video-red.png), [compact video/image grid](video.png),
[decoder/playback metrics](video.json), [replacement metrics](video-red.json),
[raw resources](resources.json). Reports omit temporary asset paths and logs.

Fresh video-only process samples (RSS/PSS, not dedicated GPU memory):

| State | RSS MiB | PSS MiB | Extra idle paints | CPU ticks |
|---|---:|---:|---:|---:|
| Before video | 48.2 | 14.1 | — | — |
| One playing video | 340.1 | 220.7 | — | — |
| Paused, 0.9-second sample | 340.1 | 220.7 | 0 | 0 |
| Hidden after multi-video use, 0.8-second sample | 303.0 | 179.6 | — | 0 |

Library/driver residency remains after freeing players; this does not imply an
active decoder or frame loop. Stop/restart removes that residency. Active decode,
multiple videos, larger resolutions and codecs have additional costs. These short
samples are not a benchmark of 4K playback, every codec or sixteen complex videos.
No frame-by-frame CPU copy is used during ordinary rendering; PNG captures perform
an explicit readback. Tight audiovisual synchronization, audible output quality,
native Wayland and other GPU/platform interop paths were not verified.

The desktop service was gracefully restarted to save geometry and apply the build,
preserving its eight current registrations, selection and layout. Automated clips
were never added to that desktop registry. Fish video completion, installed desktop
metadata and the updated discoverable skill were verified. Asset Workshop accepts
explicit `kind: "video"` manifests through its ordinary preview command.
