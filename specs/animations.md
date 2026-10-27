# 3D animation playback

Source ownership: animation transport/render scheduling in `src/model_view.{h,cpp}`,
`src/animation_controls.{h,cpp}`, native frame ruler in `src/animation_timeline.h`,
playback callbacks in `src/viewer.h`, native input/settings/seek in `src/service.cpp`,
`tests/animation_fixture.py` and `tests/animation_integration.py`.

## Assets and settings

An animated mesh, rigged character, morphing asset or moving assembly remains a
`kind:model` registration. Clips are discovered through installed F3D readers;
GLB/glTF is the verified path for skeletal, node-transform and morph animation.
Exports must contain their skeleton, skin weights and baked clips as applicable.
The viewer plays exported data and does not author rigs, evaluate DCC control
rigs, solve IK, retarget motion or execute engine scenes/scripts.

The first clip is selected initially; playback starts paused at time zero,
looping at 1× when Play is pressed. Optional model settings are `animation`
(zero-based integer clip index 0..255), `animation_speed` (0.1..4), `paused`,
`loop`, `animation_stepped` (boolean, default false) and `animation_fps`
(integer 1..120, default 12). These transport choices persist per registration
and survive same-kind reconnection, but ordinary changes do not become defaults for unrelated assets.
Clip names exposed to the UI/protocol are capped at 256 clips and 128 characters.
A visible invalid clip request is rejected before saving. A requested clip no
longer present after source replacement falls back to an available clip.

## Controls and publication

Animated models expose a native playback overlay with play/pause, restart,
clip selection, speed, loop and a compact frame ruler. The ruler uses
native C++/Qt painting, adaptive numbered ticks, a highlighted playhead and
individual sample ticks when space permits. Its work is bounded by viewport
width, not clip length; it owns no thumbnails, decoded geometry or timers.
Frame numbers are zero-based on the selected sampling rate, not authored source
keyframes. A partial terminal frame is numbered with the rounded-up frame count.
The footer shows current/end frame; its tooltip retains elapsed/duration seconds.
The ruler sits above transport controls and spans the full preview width.
Text fields match the dark preview overlay under either desktop theme.
A single transport row below the slim ruler offers Previous/Next frame, Stepped
playback and frame rate alongside play, restart, clip, speed, loop and the frame
readout. Charcoal KDE-style surfaces and cyan focus/playhead accents match the
workstation. Secondary controls collapse according to their font-dependent size;
they remain available from the options menu. Stepped playback holds each sampled pose until the next frame boundary;
its sampling grid starts at clip-relative zero, independently of source key times.
The elapsed clock and playback speed remain continuous. Previous/Next frame
pause playback first, then use the fresh native elapsed position to seek to
the adjacent sampling boundary, clamped to clip ends. This applies to buttons,
comma/period and menu actions, including between low-rate render deadlines.
Comma/period perform the same steps on the model surface. Smooth mode remains
the default and still supports frame stepping. Changing sampling reuses the
engine, retains camera/position and immediately reevaluates the current pose.
These sampling controls also live in the options menu for narrow/condensed cards.
Narrow cards retain play, clip selection and seeking; secondary controls remain
in the model options menu. Static models expose no animation bar. Fit/orbit,
camera persistence, E edges and M triangle counts retain their normal 3D behavior.
On the model surface, Space toggles playback, Home restarts and Shift+Left/Right
seek five seconds. Plain arrows continue preview navigation. Focused controls
retain their native keyboard behavior.
Tab enters the first available playback control; Shift+Tab enters the last.
The focus order follows Play, Restart, clip, speed, loop, Previous, Next,
Stepped, frame rate and timeline, skipping hidden or disabled controls. Text controls follow desktop font sizes. External
speed and frame-rate updates reach focused controls while unfinished text edits
are preserved until committed. The ruler, readout and frame-step actions always
use the effective configured frame rate, independently of unfinished editor text.

`seek ID SECONDS` uses seconds relative to the current clip's start. Scrubbing
snaps to the nearest sample frame, commits one seek on release and preserves pause
state. The right endpoint retains the exact clip end, including partial frames.
The slider uses integer frame indices rather than a fixed million-position
range, so multi-hour clips retain single-frame steps. Clips exceeding Qt's signed
integer range use a bounded proportional slider; keyboard steps retain exact
seconds independently of that coarse slider position. Smooth poses keep their
exact time, so stepping uses the adjacent boundary rather than rounding first.
Focused ruler arrow keys take priority over preview navigation and move by one
frame; Home/End reach clip boundaries. A frame-rate, duration, clip or engine-load
change cancels an in-progress drag without seeking into the replacement.
Clip changes reset time and create a fresh candidate engine while preserving the
camera. The installed glTF importer leaves transforms from disabled clips in
place, so this reload restores the authored rest pose for disjoint clip channels.
It can advance the engine load counter. Speed/pause/loop changes and seeking
reuse the current engine. Nonlooping playback reaches the final pose and pauses;
Play after EOF restarts. Looping follows elapsed time rather than frame count.
Successful source reload restarts the selected clip and preserves transport
preferences and camera. Rejected output retains the last-good scene and clips.
Shading-driven engine replacement preserves the current position where possible.
Successful imports cancel older retries and acknowledge the loaded output
stamps, so a delayed watcher notification for that same save does not reset a
subsequently selected pose. Explicitly watched generator inputs remain pending
for their normal source-save handling.

## Lifetime and observability

Only a visible, exposed, playing animated scene schedules animation frames,
using a single-shot Qt timer and elapsed-time playback. Smooth playback retains
the 33 ms cadence. Stepped playback schedules the next sampling boundary (or clip
end), adjusted for playback speed and time already spent rendering. Missed
boundaries catch up to the current elapsed-time pose without queuing stale frames.
Unchanged samples skip pose evaluation, including incidental overlay/camera
repaints. F3D owns pose evaluation, skinning through `loadAnimationTime` and its
offscreen OpenGL context on the native render thread. Qt owns the GUI frame surface.
Static, paused, hidden, minimized and off-page scenes have no animation timer.
The shared visible-only rule unloads engines off-page/hidden. Returning reloads
from the start, applying that registration's transport preferences. Playback UI
notifications are capped at approximately 10 Hz without a polling timer; low-rate
stepped playback publishes at pose changes, rather than waking for clock text.

Loaded model metrics expose `animations` (clip names), and for animated files:
effective `animation`, clip-relative `position`, `duration`, `paused`, `eof`,
`seekable`, `sample_position` (the actual evaluated clip-relative pose time),
`animation_stepped`, `animation_fps` and diagnostic `animation_timer`. These supplement loads, camera,
triangles and actual renderer identity. A running counter alone is not animation
proof; verification compares captured geometry at distinct poses.

The optional [component inspector](inspection.md) remains a static-only backend
and rejects skins/animations/morphs under its own contract. Animation transport
is available in ordinary F3D Preview mode.

Verification: `tools/test.sh animation` validates schema, persistence and setting
scope. `tools/test.sh animation-gpu` adds actual skeletal/node/morph deformation,
native controls, held stepped geometry, frame stepping, playback/EOF/looping, paused render/CPU settling and off-page
ownership using deterministic standard-library GLB fixtures in Gamescope headless.
`tools/test.sh video-controls` also checks narrow/condensed animation widgets,
large and changing desktop fonts, speed/FPS edits, long-clip and fractional-pose
steps, partial terminal frames, stale drag cancellation, readout reflow and
keyboard focus order. Native timeline keys and stepping from elapsed position
between low-rate deadlines are covered by `animation-gpu`.

Frame-ruler redesign evidence: `evidence/animation-redo/` contains matched hardware
captures and three-second 2-fps fixture measurements. On the RTX 2080 Ti, draws
fell from 93 to 12 and process CPU ticks from 29 to 4. This is a small deterministic
rig fixture, not a throughput claim for production characters. Qt may repaint
again when a native overlay changes. The GPU cadence regression bounds those
extra draws and checks looping, speed changes and the final paused pose.

Native frame stepping runs as one serial worker operation after the pause is
persisted. The `animation_step` request, buttons, comma/period and menu actions
share this path; they never calculate from a stale GUI playback notification.
