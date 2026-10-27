# Runtime and connections

[Projects](projects.md) owns persistent agent grouping, chosen current previews,
the interactive Projects grid and Overview grouping/filtering.

Optional originating Codex conversation metadata, the attached terminal lifecycle
and image-reference flow are owned by [Visual references](references.md).

Material Maker `.ptex` / `.materialgraph.json` files auto-detect as `graph`;
`add --kind graph` is the explicit route. Its bounded dependency discovery,
saved-map presentation and shared Atlas widget are defined in
[Material graphs](material-graphs.md).

The optional `inspect` and `annotations` requests, persistent review references,
offline review reads and visible-only inspection ownership are defined in
[3D inspection and reviews](inspection.md). Review JSON is separate from the
preview registry and generator inputs; comments do not rebuild models.

Source ownership: `src/main.cpp`, `src/crash_log.{h,cpp}`, `src/service.{h,cpp}`, `src/theme.h`, `bin/asset-preview`,
`fish/`, `skills/asset-preview/`, `tools/install.py`, `play.sh`, `icon.svg`. See [protocol](../docs/protocol.md)
for concrete request/configuration fields and limits.
When the account already has the workstation's private
`~/.local/share/f3d-runtime/lib/libcudart.so.13` repair, the CLI prefixes that
directory to the viewer child's library search path, retaining caller paths.
It neither downloads a runtime nor changes the shell/system environment. This
lets the installed F3D/OpenSubdiv closure load its lazy backend. Direct-binary
test runners must supply the same recorded runtime path until the host package
dependency is repaired. Existing already-running viewers retain their loaded
backend until an ordinary app restart.
The CLI accepts audio/video registrations and seek for active media or model
animation. Media uses paused/muted/loop preferences. Model animation transport,
clip and speed are per-registration settings; see [Animations](animations.md).

A Qt service and optional single GUI window share one native process. The
numeric C locale is restored after QApplication initializes the desktop locale,
as required by libmpv. Qt's localized display formatting still uses QLocale.
A standard library client starts it once under a private startup flock, waits for socket
readiness and then sends newline JSON. The native server holds a QLockFile before
removing a stale socket. Unix socket permissions restrict access to the owning
user. SIGTERM/SIGINT wake Qt through a self-pipe; no signal polling timer is used.

Native `src/crash_log.*` owns crash-only diagnostics for every launcher. It is
installed before QApplication and remains active through service/Qt teardown.
`crash.log` in the state directory appends fatal SIGSEGV/ABRT/BUS/ILL/FPE/SYS/TRAP,
failed service startup and nonzero event-loop exits, with UTC time, PID/TID,
executable/build identity, preview IDs/paths/settings, uncaught C++ exception
messages, the last meaningful request and the last 32 Qt messages (at most
1 KiB each). Expected duplicate-start rejection stays quiet.
Context stays in bounded RAM during ordinary use;
there is no diagnostic polling or routine session-log writing. Source and
Flatpak bridge launches discard stdout/stderr instead of overwriting
`server.log`; direct/foreground runners may explicitly collect stderr.
Existing historical `server.log` files are left untouched. Cache telemetry is
separate and retains its [cache diagnostics](cache-overview.md) contract.

Fatal handlers use a preopened private file, fixed buffers, an alternate main
thread signal stack and syscall-only output, preserving the original fatal
signal and host core handling. Reports include fault registers, a bounded
best-effort frame-pointer chain and a bounded process mapping snapshot; optimized
third-party frames may be absent. No allocating native stack unwinder runs in a
signal handler. Reports rotate before appending when fewer than 192 KiB remain
in the 1 MiB log, keeping one `crash.log.1` backup. Both files are mode 0600;
symlink/nonregular/hardlinked log/marker targets are rejected.

After the singleton lock is acquired, one private `session.running.json` marker
records the session identity. Normal quit/SIGTERM/SIGINT and recorded failures
remove it. SIGKILL, `_Exit` or power loss cannot run a handler: a remaining marker
produces one `unclean_exit` report on the next launch, naming the previous
session without claiming an unavailable signal/cause. Normal restarts never
truncate crash history. `tools/test.sh crash-logging` uses isolated real native
processes to verify fatal signals, startup failures, quiet clean exits,
restart retention, kill recovery, rotation and private-file protections.

`--version` reports the root `VERSION`; `licenses` prints dependency notices without
starting the service. The launcher defaults its child process to `QT_QPA_PLATFORM=xcb`, using XWayland
on KDE Wayland. Explicit platform overrides are retained. Native Wayland 3D
embedding is currently unsupported; ping/list expose the actual Qt platform.
On desktop startup without an explicit desktop identity or Qt platform theme,
the launcher fills missing theme metadata from the same user's systemd session
environment. This keeps agent shells consistent with the desktop's Qt style,
fonts and icons. The optional JSON query has a one-second timeout and bounded
accepted output; missing/unavailable session tooling leaves the caller unchanged.
Only desktop identity, KDE session hints, XDG config/data search paths and Qt
theme/style hints are copied. Explicit caller values win. Display endpoints,
plugin/library paths, credentials and preview runtime/state paths are never
imported. Offscreen/non-desktop platforms skip the query. The foreground packaged
supervisor uses the same environment policy. Existing-service clients and read-only
commands do not query the session or change the running viewer's style.
`tests/cli_source_settings.py` covers inheritance boundaries, explicit overrides,
unavailable/malformed session data and no-query paths. Matched hardware Qt probes
and identical AI/desktop window captures are recorded in
`evidence/theme/2026-10-18/desktop-launch/verification.json`.

`remove_many` atomically closes 1..256 known preview IDs, collapsing duplicates.
It validates the complete batch and persists the surviving order/active target
before unloading any content or stopping generators. Failure leaves live state
and undo history intact; success reconciles once without activating intermediate
closed neighbors. The window's checked selection is session-only and separate
from the persisted active preview; see [Preview lifecycle](preview.md).

Registration is independent of window focus and selected preview. IDs are stable,
replacement is explicit, files may not exist yet. Window creation is deliberate
through `gui` / `show` or explicit file opening. `open [FILE ...]` and bare file
paths open the window and select the last file. The read-only path lookup reuses
the first matching registration in display order, preserving generators, watches,
settings, inspection and camera. New paths use ordinary automatic-kind registrations;
missing outputs remain allowed. Paths are normalized without resolving symlinks.
Opening a batch validates all paths and merged defaults first, indexes existing
paths in display order, then atomically publishes all additions and the final
selection once. Save failure restores the entire batch and live views. Successful
publication reconciles the visible layout once; reused registrations retain
their configurations and running generators. Reopening an unchanged selection
does not rewrite the registry.
`open -- FILE` or `-- FILE` supports names starting with a dash. No arguments still
open the window. `add` retains its registration-only selection/focus contract.
A fully validated and merged identical registration is
idempotent while hidden, or while its healthy visible publication and watched
inputs are unchanged and complete resource coverage is established. Unknown or
incomplete model/texture discovery retains refresh behavior. A proven unchanged
registration does not save, rebuild the view or restart a running
generator. Changed/missing outputs, dependencies, inputs and failed candidates
retain the replacement/retry path. Explicit Reload remains a force refresh.
Closing hides the window and releases every visible entry;
Repeated GUI requests preserve maximized/fullscreen state and valid desktop placement.
F11 toggles fullscreen for the layout; a video's double click/fullscreen control
temporarily expands only that preview. Esc exits. The transient presentation and
exit-notice lifecycle are owned by [Preview lifecycle](preview.md).
Normal windows are fitted within one screen's available area (including frame
decorations), choosing the largest overlap or the primary screen for stale
offscreen positions; oversized windows shrink to fit. A deferred check handles
X11 decoration placement after mapping. Minimizing is cleared when opening.
`stop` / `quit` exits the server. Private version-1 state uses QSaveFile and retains
invalid/corrupt state with an error. Session diagnostics are separate from specs.
Failed selection saves retain the previous selection and active views; hover,
toolbar and keyboard navigation report the failure without terminating the service.
In the active window, hovering a visible preview updates the same saved selection
and recent-preview history used by clicks and keyboard navigation. No explicit
selection highlight is drawn. Popups/dialogs and ongoing drags keep their target;
leaving the cards retains the last target as the keyboard/menu fallback.
Reflow under a stationary mouse preserves explicit keyboard/CLI selection until
the next pointer movement, scroll or click.
Transient save/settings errors appear in the status bar.
Model/material registrations optionally retain a validated per-preview `camera`
object (position/focal/up vectors, angle and parallel_span). Camera state stays
separate from remembered defaults; legacy entries use the initial fitted view.
Images optionally retain validated `image_view` zoom and normalized pan, also
separate from defaults. Same-kind reconnections retain both inspection types.
Removing the selected preview defaults to its left neighbor, or its right
neighbor when closing the first entry. Settings > After Closing a Preview
offers Left Preview and Last Viewed Preview. The latter returns to the most
recently selected surviving preview in this service session, falling back to
the same neighbor rule without history. The optional version-1 `close_behavior`
field persists `left` (default for legacy state) or `last`; malformed values
reject restoration. The `close_behavior` request reads it or validates/saves
`mode` atomically. Failed preference saves preserve the choice and preload target.
Removing another preview preserves selection. History has one entry per surviving ID,
is cleared by Close all, and is not persisted. Failed removal saves preserve
the registry, selection, history and live views.
The session also retains at most 32 closed configurations for `reopen`, including
Close all removals. Reopening restores the original list position and selects
the preview only after a successful save; failure retains the closed record.
Records whose ID was registered again are skipped to preserve current work.
History is configuration only and does not survive service restarts.
Grid size (2/3/4) is an optional version-1 field with a default of 2; partial
layout requests merge and validate before saving. The additional `overview` layout persists compact all-open thumbnail
navigation with zero active views; [cache/overview](cache-overview.md) owns it.
The internal `asset-preview-server --thumbnail-job DIRECTORY` mode reads one
bounded request and writes one frame/receipt in its task-owned temporary directory.
It bypasses normal service startup, socket/state locks and crash-session files.
Its application cache name remains `asset-preview`, matching the ordinary service.
The Overview queue owns its finite lifetime and low-priority resource policy;
`src/thumbnail_job.*` loads the matching model backend without a desktop window.
Legacy compact fields are ignored on restore and omitted from saved state and list responses. Compact mode has no
UI, CLI or layout protocol option.
Optional `comparison` (empty or two distinct model/material IDs) and `camera_sync`
fields are also version-1 state. `compare` sets or ends the pair; existing layout
remains the return layout. Invalid IDs/flags are rejected. Selecting a preview
outside the pair, replacing it with another kind, or closing it ends comparison.
Display/media setting changes also update remembered defaults, persisted
in the optional version-1 `preferences` object. Image, audio, video and shared model/material
(`3d`) groups retain all setting keys independently. New registrations inherit the
appropriate group's last values; model animation transport remains per registration.
Explicit registration options override them without
changing defaults. Same-kind reconnections retain their existing inspection settings
before applying explicit overrides. Restored entries retain their saved configuration;
legacy registries without preferences start with no remembered defaults. Material CLI
registration supplies a shape only when `--shape` is explicitly passed.

The toolbar's Close all button and CLI `close-all` (`close_all` request) persist an
empty registry before unloading all views/stopping their owned generator groups.
The window and service remain available; files, layout and preferences are retained.
Failed saves restore selection/order/settings/defaults and leave live views intact.
Close all is disabled for an empty registry; the CLI does not start a stopped service.
Filesystem changes after registration (for example a removed generator cwd) do
not prevent restoring other registrations; the affected preview reports its own
load/generator failure when selected.

Finite explicit generator argument arrays run through QProcess in their declared
cwd and separate sessions/process groups. They never pass through a shell implicitly.
Source saves coalesce into at most one follow-up run; timeout, hide, remove and exit
stop the owned group. Generator logs retain only the latest 16 KiB. Project runtime,
container, lock and exporter policy remain caller-owned. Packaged launchers preserve
the caller's library/Qt/Python path settings; generator children restore those
settings instead of inheriting the viewer bundle's paths.

Installation preflights both fish symlinks, the owned Codex skill symlink and the
managed desktop entry, refuses unrelated destinations, validates the entry when a validator is installed and
refreshes KDE's application database. No PATH/global hotkey change. Completion (`--complete-ids` / `--complete-projects`) is
read-only and Qt-free; it reads the running server or the persisted registry without
starting a server. Fish completes bare paths and multiple `open` paths as well as
commands/options and registered IDs. Desktop entries accept multiple local files
through `open %F`; launching without files still opens the window.
Completion context comes only from the first command token; filenames and option
values named `select`, `layout` or another command cannot change it.
`play.sh` delegates to the same GUI entrypoint for Playpad discovery.
The skill's canonical source/UI metadata is versioned in `skills/asset-preview/`;
installation links it into `${CODEX_HOME:-~/.codex}/skills/asset-preview` without
changing unrelated skills or invocation policy. Automatic skill selection is enabled.
The skill covers images, audio/music, videos, exported models/scenes, modular
assets, rigged animation and baked materials. It uses one
stable registration per view and ordinary output watching during iteration;
reconnection replaces configuration, so agents retain explicit generator inputs
and user settings. It distinguishes visible-only generation and asynchronous
audio/video publication from registration, and routes detailed options to the protocol.
The compact skill entry keeps the connect/watch/verify flow and desktop-state
rules. Its connection, generator and verification references retain detailed
commands, settings, lifecycle and failure checks; five asset-specific references
cover media/materials, 3D assets, animation and optional component inspection.
This documentation split changes no runtime behavior or invocation policy.
Agent sends and updates use background registration by default and preserve the
user's selection, layout, visibility and focus. Desktop opening or raising requires
an explicit user request. Later closure/hiding is respected during updates;
capture or load verification does not authorize reopening or a second desktop
instance. Automated visual work uses hardware Gamescope headless, while ordinary
background registrations may remain suspended until opened by the user.

Verification: `tools/test.sh functional` exercises the real daemon, concurrent
startup, permissions, schema failures, save replacement/recovery, bounded lifetime,
generator coalescing/cancellation/timeouts, completion and restart persistence.
`tests/cli_source_settings.py` checks shorthand parsing and read-only Fish file
completion. Functional opening tests cover reuse, selection, generator preservation
and batch validation; the GPU lane sends native XDND file drops from a separate
process onto 3D and image surfaces, verifies copy semantics and duplicate prevention,
and rejects sources that offer only Move.

Lifecycle probes for existing-service commands and startup allow up to 15 seconds
to avoid launching a second daemon during a slow first GPU render. Fast startup
polling still uses a short probe while watching the exact spawned child.

Flatpak packages keep the server in a foreground sandbox process; their host CLI
bridge serializes startup and uses a separate private `asset-preview-flatpak`
socket and app data directory. Generators run via `flatpak-spawn --host --watch-bus`
in the specified host cwd. Flatpak owns the host supervisor session; local helper cancellation closes its
bus. Watch-bus SIGINT reaches the host Python supervisor, which retires the
generator’s separate child group with TERM/KILL, including SIGINT-ignoring
background jobs and descendants left behind after normal generator completion.
Packaged desktop launchers set the canonical reverse-DNS desktop identity;
source-checkout launchers keep their existing desktop entry identity.
The packaged foreground supervisor shares the CLI's argument parser for GUI and
file opening, including bare paths and `--`. The Flatpak launcher sends every
invocation through this supervisor; read-only/control actions delegate to the
ordinary client without creating another native child. Explicit `start` retains
foreground ownership when needed but only pings the server, preserving hidden
state until a GUI/file activation. GUI/file/start activations take
the startup flock, reuse a running server, or retain and supervise the exact child
they create. A second activation sends its open request and exits while the first
supervisor remains alive. Signals retire only the owned child.
`tools/test.sh packaged` exercises actual private-service lifetime, concurrent
file activations, reuse, shutdown and read-only no-start commands without creating
a release package.

The `defaults` request accepts `group` (`image`, `audio`, `video`, `3d`) and a validated `settings` patch. It saves remembered values atomically without changing existing entries; failed saves restore preferences. The Settings menu uses this path independently of View > Preview Options.

Settings > Theme offers mutually exclusive Auto, Dark and Light choices. The
optional version-1 registry field `theme` accepts only `auto`, `dark` or `light`;
legacy registries default to Auto. Invalid saved choices fail startup while
preserving the file. A `theme` request without `mode` reads the current choice;
with a valid `mode` it saves atomically before applying the palette. Failed saves
retain both the previous choice and appearance, and menu errors remain visible.
Theme responses and `list` expose `theme` and `effective_theme` (`dark` or `light`).
The theme survives Close all, empty registries and service restarts. It is
independent of per-preview backgrounds and remembered display defaults.

Dark and Light use application palette roles with the existing native Qt widget
style, including selection, tooltips, placeholders and disabled foregrounds.
Auto clears application palette overrides so Qt uses the current platform colors
and continues following system changes. Qt's separate tooltip palette is synced
on a theme choice and before showing a forced-theme tooltip, without polling.
Theme changes repaint widgets without
reconciling previews, importing models, restarting generators or creating timers.
`tools/test.sh theme` verifies role contrast, Auto reset, legacy state, persistence,
validation and save rollback. `tools/test.sh theme-gpu` adds native menu input,
ordinary F3D reload/camera/idle checks and inspection panel captures in both themes.

## Playpad native tool entry

The root `play.sh` resolves its own root and switches to that cwd before the
existing `gui` command. `--check` verifies the prepared executable without
starting the service. Missing native builds require explicit `tools/build.sh`;
normal launch never builds, syncs dependencies, generates fixtures or creates
a second runtime. This is the existing C++/Qt preview application, without an
invented separate lab chooser. The same private same-user singleton and
visibility/close lifecycle above apply to Playpad launches. Keep the current
root SVG and source artwork unchanged.

## Playpad runtime acceptance, 2026-10-17

Two root no-argument launches from an unrelated cwd with detached stdin showed the prepared native viewer in 2.013 and 0.538 seconds. An isolated synthetic GLB exercised Ctrl+W removal, actual window-close handling, hidden/unloaded state and reopening. The viewer reported NVIDIA GeForce RTX 2080 Ti/PCIe/SSE2. The owned service and socket were removed afterward. Screenshots and runtime-results.json are local compliance evidence. No dependency setup ran.

Startup and layout diagnostics are finite: `list.performance` reports state
restore/service-start ms and the latest reconciliation phase durations. Native
backend initialization is deferred until the GUI has completed initial metadata
and plugin discovery, keeping the dynamic loader's global lock out of that path.
The source launcher retries only duplicate-lock exit 3 within its existing
startup deadline when a quitting service has closed its socket but is still
releasing renderer work. All other startup failures retain their error path.
