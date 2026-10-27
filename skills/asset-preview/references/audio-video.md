# Audio and video connections

For rendered turntables/animation captures, connect the local finished encode with
`add /absolute/project/preview/turntable.mp4 --id project-turntable`; common video
extensions are automatic, other local video formats can use `--kind video`.
The same watched-output/finite-generator flow applies. Publish encodes atomically.
libmpv streams videos; a reload completes asynchronously, restarts at the beginning
and preserves paused/muted/loop preferences. Playback defaults to muted looping.
`settings ID '{"paused":true}'` pauses it; `seek ID 2.5` seeks a loaded visible
video. Check successful publication as described below. `metrics.hwdec` reports
the actual decoder separately from GPU renderer identity; software fallback works.

For music, generated songs or sound-effect listening, connect the local output:

```sh
"$preview" add /absolute/project/audio/song.wav --id project-music --label "Song"
"$preview" settings project-music '{"paused":false}'
"$preview" seek project-music 12.5
```

WAV, MP3, FLAC, OGG, Opus, M4A and other common audio extensions are automatic;
use `--kind audio` for another local format supported by installed libmpv codecs.
Register before generation if useful, then watch output saves; no generator is
needed when the agent already owns song generation. Publish completed audio
atomically. Audio has separate remembered defaults, initially paused, unmuted and
looping. Press Play to hear it, or set `paused:false` only when playback is wanted;
`settings ID '{"muted":true}'` silences a loaded track. Timeline and keyboard
controls match video. A hidden, minimized or off-page track unloads and stops
sound. Returning reloads from the beginning with stored playback preferences.
Audio publishes asynchronously after usable audio decoder parameters arrive;
it needs no video frame. Check loads/revisions and `metrics.loading`, duration,
sample rate and channels. Loaded status proves playback readiness, not a listening
verdict or music quality. Failed replacements keep the previous track playing and
do not count as successful new audio. Preserve other work's connections.
