# Runtime and connections

Source ownership: `src/main.cpp`, `src/service.{h,cpp}`, `bin/asset-preview`,
`fish/`, `tools/install.py`, `play.sh`, `icon.svg`. See [protocol](../docs/protocol.md)
for concrete request/configuration fields and limits.

A Qt service and optional single GUI window share one native process. A standard
library client starts it once under a private startup flock, waits for socket
readiness and then sends newline JSON. The native server holds a QLockFile before
removing a stale socket. Unix socket permissions restrict access to the owning
user. SIGTERM/SIGINT wake Qt through a self-pipe; no signal polling timer is used.
The launcher defaults its child process to `QT_QPA_PLATFORM=xcb`, using XWayland
on KDE Wayland. Explicit platform overrides are retained. Native Wayland 3D
embedding is currently unsupported; ping/list expose the actual Qt platform.

Registration is independent of window focus and selected preview. IDs are stable,
replacement is explicit, files may not exist yet. Window creation is deliberate
through `gui` / `show`. Closing hides the window and releases every visible entry;
`stop` / `quit` exits the server. Private version-1 state uses QSaveFile and retains
invalid/corrupt state with an error. Session diagnostics are separate from specs.
Filesystem changes after registration (for example a removed generator cwd) do
not prevent restoring other registrations; the affected preview reports its own
load/generator failure when selected.

Finite explicit generator argument arrays run through QProcess in their declared
cwd and separate sessions/process groups. They never pass through a shell implicitly.
Source saves coalesce into at most one follow-up run; timeout, hide, remove and exit
stop the owned group. Generator logs retain only the latest 16 KiB. Project runtime,
container, lock and exporter policy remain caller-owned.

Installation preflights both fish symlinks and the managed desktop entry, refuses
unrelated destinations, validates the entry when a validator is installed and
refreshes KDE's application database. No PATH/global hotkey change. Completion is
read-only and Qt-free; it reads the running server or the persisted registry without
starting a server. `play.sh` delegates to the same GUI entrypoint for Playpad discovery.

Verification: `tools/test.sh functional` exercises the real daemon, concurrent
startup, permissions, schema failures, save replacement/recovery, bounded lifetime,
generator coalescing/cancellation/timeouts, completion and restart persistence.
