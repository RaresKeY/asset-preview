# Rigged, scene and morph animation

Connect the owning export as an ordinary model. GLB/glTF is the verified path
for skeletal skinning, animated node transforms and morph targets. Include the
skeleton, weights and baked clips in the export. F3D plays that exported data;
it does not execute Blender control rigs, Godot scripts, constraints, physics,
IK or retargeting. Use an owning-engine PNG/video capture for exact runtime behavior.

```sh
preview="${ASSET_PREVIEW_PROJECT:-$HOME/workspace/asset-preview}/bin/asset-preview"
"$preview" add /absolute/project/exports/character.glb \
  --id project-character --label "Character · animation" \
  --settings '{"paused":true,"animation":0,"animation_speed":1,"loop":true}'
"$preview" status --json
"$preview" settings project-character '{"paused":false}'
"$preview" settings project-character '{"paused":true,"animation":1}'
"$preview" seek project-character 0.75
```

Clip indices are zero-based. Read the loaded model's `metrics.animations` before
choosing one. `metrics.animation` is the effective index; position and duration
are seconds relative to the selected clip. Speed accepts 0.1..4. Native controls
provide clip selection, pause/play, looping, speed and a seek timeline.
Stepped playback holds poses at `animation_fps` (integer 1..120, default 12);
set `animation_stepped:true` to enable it. Previous/Next frame and comma/period
pause and move to the adjacent sampling boundary. Sampling is viewer transport,
independent of source keyframe spacing. `metrics.sample_position` reports the
actual evaluated pose time alongside continuous `position`. These settings
persist per model and do not change defaults for unrelated assets. Space,
Home and Shift+Left/Right work on the model surface; E/M retain their 3D meanings.

Playback starts paused, looping at 1× when played. Set `paused:false` only when
motion is wanted. Transport options persist for this registration and do not
become animation defaults for unrelated assets. A clip change restarts that
clip through a fresh candidate engine to clear old clip transforms, preserving
the camera and last-good content. Pause, speed, loop and seek reuse the engine. Seek preserves pause state. Nonlooping playback
pauses at the final pose; Play then restarts. Successful source reload restarts
at the beginning with stored camera and playback preferences.

Use normal source-save watching and one finite generator, as for static models.
Do not re-register to advance time. Hidden/minimized/off-page scenes unload and
stop; paused/static scenes have no animation timer. Keep animation paused while
checking a fixed pose or comparing captures. Verify the expected clip names,
successful loads/revisions and actual rendered deformation, rather than treating
a changing time counter as proof of a valid rig or motion.

Full scene exports and modular host motion use this same path. Keep modules and
moving parts under their owning exporter; the viewer does not assemble or repair
rigs. The optional component inspector remains static-only and rejects animated,
skinned or morphing sources. Use ordinary Preview mode for animation playback.
The implementation contract is in `specs/animations.md` in the owning checkout.
