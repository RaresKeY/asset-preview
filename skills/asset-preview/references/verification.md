# Refresh, failures and captures

Read when verifying publication, diagnosing stale/failed output or making an automated capture. Use the owning checkout's `bin/asset-preview`.

Check `status --json`: confirm the expected path/ID; distinguish `active`,
`building`, waiting/failure status and successful revisions/loads. For audio/video
refreshes, compare before/after revisions or loads and wait for loading to finish;
accepting a load command is insufficient. `metrics.loads` resets when a view unloads or is
recreated. A registered or suspended view does not prove a build ran or looked
correct. Loaded 3D views report `metrics.renderer`; use that for renderer evidence.
On failure, inspect the bounded generator tail or entry status and correct the
source/runtime/path. Process crashes append `crash.log` in the configured state
directory (normally `~/.local/state/asset-preview`), keeping one `crash.log.1`
backup across restarts. Recent Qt diagnostics and preview context stay in RAM
until failure; there is no routine `server.log`. A forced kill or power loss is
reported as an unclean exit on the next launch, without inventing a signal/cause;
do not treat retained last-good content as the new output. The supported desktop
path is XWayland (`xcb`); retain the launcher's default unless testing an explicit
alternative. If the executable is absent, read its AGENTS/specs and build using
existing dependencies; report an unavailable prerequisite rather than silently
substituting another renderer or installing packages.

For automated visual checks, use Gamescope headless through the owning project's
runtime and verify its actual renderer. `capture NEW.png` requires a mapped
window in its display session and refuses overwrite. Use the headless session
for automated captures; a capture or load-check requirement does not authorize
opening or raising a window on the user's screen. A background registration may
remain suspended until the user opens it; report that state without launching it.
