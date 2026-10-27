# Audio and music previews

Source ownership: `src/audio_view.{h,cpp}`, `src/mpv_player.h`, shared playback
controls in `src/video_controls.{h,cpp}`, `src/audio_visualizer.{h,cpp}`, and audio
integration tests. `src/audio_waveform.{h,cpp}` owns the finite PCM overview;
`src/seek_slider.h` paints it in the shared audio timeline.

The same lazy `asset-preview-mpv.so` module exports a dedicated `AudioView`
QWidget. It shares bounded local-only player setup with video through
`src/mpv_player.h`, without a video decoder, OpenGL context or render API.
A painted card identifies the loaded filename; existing playback overlays provide
play/pause, restart, elapsed/duration, seek, mute and loop. Keyboard playback
shortcuts match video; audio retains its persistent framed controls
and condensed layout. A simple row of rounded green bars sits centered near the
bottom of the preview, above the transport, with the filename and status above it.
Bars rise from one shared bottom baseline and show recent mean stereo energy from
left (oldest) to right (newest). They have no glow, ribbons, central badge or
frequency labels. Measurements come from metadata-only FFmpeg `astats` (channel
RMS and overall peak) and `aspectralstats` (spectral centroid) filters in the same
libmpv decoder path. The sound samples are unchanged. The bars show energy history,
not frequency bins or a literal PCM waveform; spectral and onset measurements
remain available in metrics. Muting leaves source analysis active. Silence settles
to quiet baseline dots. When the Qt style disables widget animations, the bars show
current levels without traveling history. Pausing freezes and dims the bars.

Only libmpv property notifications advance the bounded 40-sample history, with
attack/release smoothing and a maximum 29 updates per media second. There is no
visualization timer, subprocess, video surface or GL context. The separate finite
waveform decoder described below runs only while generating the overview.
Paused audio freezes its history and has no continuing paint or polling timer.
Position notifications still update controls independently. Seeking clears the
visual history; successful replacement starts a new one, while a failed candidate
preserves the last-good visualization and transport. Hidden/off-page/minimized
views destroy both current and candidate cores, including analysis/history.

The audio seek timeline shows the complete clip as two cyan/green min/max PCM
envelopes with a white vertical playhead. Mono is duplicated; multichannel files
show their first two channels. A shared peak scale preserves relative channel
levels and exposes quieter details. Elapsed audio is brighter. The playhead uses
the same libmpv position/duration and held slider position as the transport,
including paused seeks, restart, loop and drag previews. A drag commits one seek
on release. Keyboard seeking and slider accessibility remain native. Normal
timelines are 64 px tall; condensed cards use 28 px. The energy bars remain above
the waveform, with space reserved for both waveform and transport.

After publication, one cancellable worker decodes local audio using the existing
FFmpeg libraries directly. It retains 2,048 min/max bins (32 KiB), never full PCM.
Completed envelopes may enter the bounded RAM/disk
[preview cache](cache-overview.md). Frame timestamps relative to stream start place samples on the
same timeline as playback. Each decode uses one codec thread, a 1 MiB probe,
2-second stream-analysis allowance, a 20-second wall deadline and a six-hour
duration cap. Only regular local files and an explicit audio-container allowlist
are accepted, only the file protocol is allowed, and MOV external references
remain disabled. Completion queues one control update; there is no polling or
idle rendering. Unknown durations, unsupported files or failed/bounded decodes
retain a usable plain seek bar. Source device/inode/size and nanosecond mtime/ctime
are checked around decoding; raced results are discarded. The worker is stopped and joined on successful
replacement or view destruction. Failed candidates keep the current waveform.
Immutable bins are shared only between the visible view and its controls, never
serialized through position notifications or persisted. `waveform` metrics expose
ready/loading, bin count, channels and duration; timeline controls expose a
`waveform` boolean.

Audio defaults to paused, unmuted and looping. Its remembered `audio` preference
group is independent of video's muted autoplay defaults. Candidate loads always
stay paused and muted, with the audio decoder selected. A loaded file plus valid
audio codec/sample-rate parameters authorizes publication; no video frame is
required. Replacements retain the last-good player until successful publication,
then restart from the beginning and apply stored playback settings. Failed loads
retain last-good duration/controls, use the existing bounded retries and a
15-second candidate deadline, and do not advance successful loads/revisions.
The selected audio decoder remains enabled while muted so progress, seek and
readiness continue working. No external media references, scripts, user mpv
configuration or online helpers are enabled.

Common local WAV, MP3, FLAC, OGG, Opus, M4A, AAC, AIFF, WMA and APE extensions
route to audio. Explicit `kind:audio` uses other formats supported by installed
libmpv. OGG video can explicitly choose video. Metrics report codec, sample rate,
channels, duration/position, pause/mute/EOF/seekable, loaded filename and current
audio output, with no GL renderer claim. `visualizer` reports measurement
availability, an update count, finite `rms_db` (two channels), `peak_db`,
`centroid_hz`, smoothed `energy` and onset `pulse`; dB values floor at -90.
`ASSET_PREVIEW_TEST_NULL_AUDIO=1` selects
libmpv's null audio output for quiet automated playback across both media kinds.

Functional tests cover registration/CLI validation, separate default persistence,
WAV and five compressed formats, late output appearance, progress, paused seek,
unmute state, loop/EOF replay, replacement/failure/recovery and visible-only
unload. A deterministic stereo fixture verifies silence, loud/soft bass, treble,
channel isolation, source analysis while muted and frozen paints/history on pause.
The packaged FFmpeg filter allowlist includes both analysis filters.
`tools/test.sh audio-waveform` verifies PCM and float samples, stereo timing and
channel isolation, mono duplication, five compressed formats, malformed input,
duration bounds and cancellation. Audio integration covers waveform readiness,
failed replacement retention, timeline input and off-page unload.
The headless Gamescope lane exercises native buttons, drag/click seeks,
keyboard playback, mixed audio/model/image grids, real GPU renderer
identity on the model, and off-page cleanup. Existing video tests cover its
frame publication, paused idle rendering and input regressions.

Shared window, navigation and rendering lifetime: [Preview lifecycle](preview.md).
