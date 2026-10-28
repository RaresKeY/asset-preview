# Refresh, failures and captures

Read when verifying publication, diagnosing stale/failed output or making an automated capture. Use the owning checkout's `bin/asset-preview`.

Check `status --json`: confirm the expected path/ID; distinguish `active`,
`building`, waiting/failure status and successful revisions/loads. For audio/video
refreshes, compare before/after revisions or loads and wait for loading to finish;
accepting a load command is insufficient. `metrics.loads` resets when a view unloads or is
recreated. A registered or suspended view does not prove a build ran or looked
correct. Loaded 3D views report `metrics.renderer`; use that for renderer evidence.
`presentation.overview.visible_ready` distinguishes visible thumbnail readiness
from `worker_active`, which can remain true while other open assets warm in the
background. The shared queue has two image slots and one model slot, and remains
observable while using Projects/Single. Thumbnail completion does not activate
the ordinary preview or establish a generator revision. Do not wait for the
entire background queue before navigating or verifying an already-ready view.
On failure, inspect the bounded generator tail or entry status and correct the
source/runtime/path. Process crashes append `crash.log` in the configured state
directory (normally `~/.local/state/asset-preview`), keeping one `crash.log.1`
backup across restarts. Recent Qt diagnostics and preview context stay in RAM
until failure; there is no routine `server.log`. A forced kill or power loss is
reported as an unclean exit on the next launch, without inventing a signal/cause;
do not treat retained last-good content as the new output. Source launches use
native Wayland when the caller supplies WAYLAND_DISPLAY, and X11 (`xcb`)
otherwise; explicit platform overrides are preserved. Retain that default.
`tools/test.sh wayland-gpu` checks rendering without DISPLAY in private KWin
nested inside headless Gamescope; ordinary XTest input checks keep their X11
lane. If the executable is absent, read its AGENTS/specs and build using
existing dependencies; report an unavailable prerequisite rather than silently
substituting another renderer or installing packages.

For automated visual checks, use Gamescope headless through the owning project's
runtime and verify its actual renderer. `capture NEW.png` requires a mapped
window in its display session and refuses overwrite. Use the headless session
for automated captures; a capture or load-check requirement does not authorize
opening or raising a window on the user's screen. A background registration may
remain suspended until the user opens it; report that state without launching it.
