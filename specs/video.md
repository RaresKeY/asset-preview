# Video previews

Source ownership: `src/video_view.{h,cpp}`, `src/video_controls.{h,cpp}`,
`src/mpv_player.h`, fullscreen/window paths in `src/service.cpp`,
`src/fullscreen_notice.{h,cpp}` and video integration tests.

`asset-preview-mpv.so` is a lazy module linked to installed libmpv and Qt OpenGL.
Image/background-only sessions do not map libmpv. Visible videos own independent
mpv cores and native embedded QOpenGLWindows, with fitted aspect ratio. The module
remains resident after use, while every off-page/hidden player, decoder and render
context is destroyed. No subprocess player, CPU frame-copy loop or video polling.
The service restores `LC_NUMERIC=C` after Qt startup so desktop numeric locales
such as `en_US.UTF-8` do not make `mpv_create()` reject audio/video playback.

Playback defaults to unpaused, muted, looping. A native bottom overlay provides
play/pause, restart, elapsed/total time, a seekable timeline, mute, loop and fullscreen.
Video uses a full-width 3 px teal timeline with a round handle above 40 px dark
slate control pills. Play, mute and time sit at the left; restart, loop and
fullscreen sit at the right. Native masks leave the gap between groups clear
and available to the video surface. Their capsule outlines are rasterized at
pixel centers, preserving round buttons and time pills without coarse polygon
edges. The timeline keeps its full-height seek area with rounded ends.
Captures respect the same overlay masks.
Clicking the timeline seeks absolutely; dragging previews the target time and
commits one exact seek on release, retaining pause state. Observed mpv properties
update the controls without polling or requesting video paints. Unknown duration
or non-seekable media disable the timeline and restart; unloaded videos disable
all playback actions. Narrow cards hide time,
restart and loop as needed. The time pill also checks its measured text width,
including long durations and enlarged desktop fonts; font/layout changes update
the overlay height without clipping. A shorter replacement duration can reveal a
previously hidden time pill without resizing. Short cards use 28 px buttons with the timeline
above play plus mute/fullscreen when space permits;
the options menu keeps all playback actions available. Playing after EOF restarts.
Five-second Shift+Left/Right seeks and Home restart work on the surface and
playback controls. Space toggles pause on the focused surface;
Space on a focused button activates that button. Plain arrows navigate previews/pages.
Left-button double click on the video surface and its fullscreen button enter
fullscreen with only that preview visible, without reloading/seeking it or
changing its pause/mute/loop settings. Other previews unload under the existing
visible-only rule; leaving restores the configured layout. Navigation in this
temporary single view steps one preview at a time. F11 and View > Full Screen
toggle fullscreen for the current layout, including image/model/empty sessions.
Esc or F11 exits; double click and the video button also exit. Normal/maximized
window state is restored, and global menus/toolbar/status chrome are hidden in
fullscreen. Per-preview menus remain available. The temporary single-view choice
is not saved. Closing the window leaves fullscreen before saving its geometry.

Video overlays (title, actions, steady Live status and playback) and cursor hide
after 700 ms without input, including when paused. Entering fullscreen hides
them immediately, including when the pointer is over the fullscreen button.
Fullscreen entry ignores synthetic enter/focus, layout events and the remaining
mouse release from its activating double click. Fresh pointer movement or
explicit click/wheel/keyboard input reveals controls again. A hovered overlay,
held mouse button, open popup/dialog or keyboard-focused playback control defers
hiding until later input;
leaving controls or completing a seek re-arms the single-shot inactivity timer.
Tab/Shift+Tab from the native video surface reaches the playback controls in
visual order; mouse activity returns to the normal inactivity behavior.
Native mask/layout events under a stationary pointer preserve keyboard focus
inside the card, including a focused timeline after seeking.
Keys received by a native video window while a playback widget retains Qt focus
are forwarded to that widget, preserving timeline and button keyboard behavior.
Loading/waiting videos retain controls and busy/error status stays visible.
Audio controls remain visible. Playback property notifications never reset the
inactivity deadline. Off-page/hidden cards own no inactivity timer.

A native top-center notice identifies fullscreen and offers Exit full screen
(Esc), also documented as F11 in its tooltip. It waits three seconds, then slides
above the canvas over 220 ms and hides. A Qt style that disables widget animation
hides it immediately after the delay. Leaving fullscreen, hiding the canvas or
re-entering cancels the prior delay/animation. Only this finite animation requests
its own frames; no recurring control, notice or video render polling is added.
Entry `controls` and `overlays.playback` report transient playback geometry/state
for native input and capture checks. Top-level `presentation` reports fullscreen,
single-preview mode, chrome visibility and notice geometry/animation. Window
captures composite the bar and fullscreen notice after native video readback,
respecting their native masks so rounded corners remain clear.
Finite source-save generators can publish videos through the existing watcher.
The hardware input test covers waiting/loaded controls, click and held-drag seeks,
keyboard focus, EOF replay, grids and paused render/CPU inactivity.
The fullscreen/inactivity hardware input test covers native double click, the
video button, F11/Esc, grid restoration, unchanged playback, hover/drag protection,
notice dismissal and idle rendering, with captures under ASSET_PREVIEW_EVIDENCE_DIR.
`tools/test.sh video-controls` checks narrow and condensed layouts with short/long
durations at 10, 18 and 26 point fonts, plus fullscreen notice capture clipping.

The render API uses the owning Qt OpenGL context, supplies the X11 display for
interop, requests `hwdec=auto-safe` and permits codec/driver software fallback.
GPU renderer and decoder identity are reported separately. Frame/event callbacks
coalesce onto the GUI thread without calling mpv APIs inside callbacks. Core
commands/property writes use asynchronous APIs; event handling observes properties
without synchronous core reads. Advanced render control is disabled, and target
time blocking is disabled to avoid sleeping on the GUI thread. Static/paused views
have no continuing frame activity; active videos necessarily consume decode/render
resources. Decoder memory is not bounded by the demux queue limit.

Reload opens a silent candidate asynchronously, preserving the current player
until a decoded video frame can replace it. Successful swaps restart at time zero
and retain pause/loop/mute preferences. Unsupported/corrupt outputs retain the last
good player, with bounded retries. A 15-second candidate deadline prevents an
indefinite load. Load/revision counts advance only after successful publication.
Candidates and current render contexts are freed before their corresponding cores,
with the original OpenGL context current. Timers/callbacks are scoped to the view.

Local demux buffering is capped at 8 MiB with no backward queue/cache. Muted
playback disables the audio track; unmuting selects it again. mpv user
configs/scripts, online URL helpers, external media references, automatic audio
sidecars and subtitles are disabled. Common video extensions and animated GIF
route automatically; explicit `kind=video` handles other installed-codec formats.


Shared window, navigation and rendering lifetime: [Preview lifecycle](preview.md).
