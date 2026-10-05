# Runtime and connections

Source ownership: `src/main.cpp`, `src/service.{h,cpp}`, `bin/asset-preview`,
`fish/`, `skills/asset-preview/`, `tools/install.py`, `play.sh`, `icon.svg`. See [protocol](../docs/protocol.md)
for concrete request/configuration fields and limits.
The CLI accepts video registrations and absolute seek for active videos; playback
preferences are boolean paused/muted/loop settings in the existing registry.

A Qt service and optional single GUI window share one native process. A standard
library client starts it once under a private startup flock, waits for socket
readiness and then sends newline JSON. The native server holds a QLockFile before
removing a stale socket. Unix socket permissions restrict access to the owning
user. SIGTERM/SIGINT wake Qt through a self-pipe; no signal polling timer is used.
`--version` reports the root `VERSION`; `licenses` prints dependency notices without
starting the service. The launcher defaults its child process to `QT_QPA_PLATFORM=xcb`, using XWayland
on KDE Wayland. Explicit platform overrides are retained. Native Wayland 3D
embedding is currently unsupported; ping/list expose the actual Qt platform.

Registration is independent of window focus and selected preview. IDs are stable,
replacement is explicit, files may not exist yet. Window creation is deliberate
through `gui` / `show`. Closing hides the window and releases every visible entry;
`stop` / `quit` exits the server. Private version-1 state uses QSaveFile and retains
invalid/corrupt state with an error. Session diagnostics are separate from specs.
Failed selection saves retain the previous selection and active views; toolbar
and keyboard navigation report the failure without terminating the service.
Transient save/settings errors show the status bar even in compact mode.
Grid size (2/3/4) and compact mode are optional version-1 fields with backward
compatible defaults; partial layout requests merge and validate before saving.
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
refreshes KDE's application database. No PATH/global hotkey change. Completion is
read-only and Qt-free; it reads the running server or the persisted registry without
starting a server. `play.sh` delegates to the same GUI entrypoint for Playpad discovery.
The skill's canonical source/UI metadata is versioned in `skills/asset-preview/`;
installation links it into `${CODEX_HOME:-~/.codex}/skills/asset-preview` without
changing unrelated skills or invocation policy. Automatic skill selection is enabled.
The skill covers images, videos, exported models and baked materials. It uses one
stable registration per view and ordinary output watching during iteration;
reconnection replaces configuration, so agents retain explicit generator inputs
and user settings. It distinguishes visible-only generation and asynchronous
video publication from registration, and routes detailed options to the protocol.

Verification: `tools/test.sh functional` exercises the real daemon, concurrent
startup, permissions, schema failures, save replacement/recovery, bounded lifetime,
generator coalescing/cancellation/timeouts, completion and restart persistence.

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
