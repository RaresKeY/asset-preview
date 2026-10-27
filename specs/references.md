# Visual references and Codex sidebar

Source ownership: `src/codex_panel.*`, `src/reference_editor.*`, shared window
routes in `src/service.*`, origin propagation in `bin/asset-preview`.

The right Codex dock embeds the installed Konsole KPart when KF6 Parts was found
at build time. It runs `codex --remote unix://SOCKET resume THREAD --cd CWD`,
attaching to the existing server and exact originating conversation. It never
starts a second app server, forks the conversation, changes model/permissions,
or owns the background agent. The installed Codex must support Unix remote
attachment. Missing terminal support, executable, cwd or server is reported in
the dock; there is no fallback to a new conversation.
The embedded Konsole client starts through `env` with `TERM=xterm-256color` and
`COLORTERM=truecolor`, clearing inherited `NO_COLOR`, `FORCE_COLOR`, `CLICOLOR`
and `CLICOLOR_FORCE` overrides. Agent command runners can disable color in their
own output; those flags must not suppress the interactive terminal's colors.
The service and generator environments remain unchanged. Arguments are passed
directly without a shell, and `env` replaces itself with the same Codex client.

The standard-library client captures `CODEX_THREAD_ID` (or `CODEX_SESSION_ID`),
caller cwd, and the current Codex home's `app-server-control/app-server-control.sock`
for registrations. `ASSET_PREVIEW_CODEX_SOCKET` overrides that local socket for
another same-user server. Optional registration `codex` contains `thread_id`
(UUID), `cwd` and `socket` (absolute paths). Syntax/size/control characters are
validated before saving. Attachment additionally requires a live Unix socket
owned by this UID with no group/other permissions. A change only to this origin
saves without rebuilding an active preview; reconnects without it preserve the
previous origin. Explicit GUI/open requests can carry a transient fallback
origin; opening new paths records it. Read-only client requests never change it.
There is no history search or guessed most-recent-thread fallback. Existing
registrations without an origin should be reconnected from the intended thread.

The initial horizontal split is 9:7 (56.25% preview / 43.75% Codex), matching
the measured desktop composition. Window resizing maintains that fraction;
explicit splitter drags replace it. The fraction is saved in window.ini and
restored across service restarts, with native minimum widths taking precedence
on small windows. Collapse/reopen retains it. This is ordinary default layout,
without a separate compact-mode switch.

The dock stays pinned while navigating/hovering assets. Follow asset explicitly
retargets it. Switching parks the previous client with its draft and local queue;
returning reuses it. At most eight conversation clients are retained, and only
exited clients can be evicted automatically. Collapse, dock close and window close/hide retain the same terminal
client and its unsent draft; reopening reuses it. Quitting Preview destroys only
the attached terminal clients. Background turns and server-owned queued prompts
remain with Codex. The installed Codex's native Tab queue is client-local: normal
window close/collapse preserves and drains it, but File → Quit / `stop` discards
those local pending prompts and unsent drafts. Quit only after that queue drains
if those inputs must run; this is distinct from an already running server turn. Preview never sends interrupt, shutdown or Enter as part of
its lifecycle. The terminal retains its own input, clipboard and queue controls;
Preview's window shortcuts change scope while terminal widgets own focus.
When a terminal process exits, Konsole deletes its KPart. Preview observes that
destruction for both active and parked clients, using guarded widget pointers.
Exited clients are not reused or counted against the eight-client retention
limit; reopening attaches a fresh client to the same originating conversation.
If Konsole keeps a failed process's view open, attachment checks its process ID
and retires that exited view as well, without polling.

Reference freezes the current visible viewport, including image pan/zoom or the
current rendered video/model frame. Window captures composite the actual native
3D/video surfaces, overlays and Preview interface. Capturing with an owned
modal editor/dialog open captures that dialog. Annotation uses those frozen
pixels, independent of subsequent playback/navigation. Tools are circle, pen,
translucent highlight, ordered dots, text and undo. Marks are confined to the
image and rendered at its original captured pixel dimensions. The editor owns
the captured pixels for its modal session; it has no polling/render timer.
A reference includes asset ID/path, frame time when present, and captured view
state as editable text beside the attached image.

Save, Copy and Attach in the editor save a uniquely named PNG under the user's
Pictures/Asset Preview. Unchanged marks reuse that session's saved file. Attach
pastes the local image path with terminal bracketed-paste framing, which Codex
recognizes as an image attachment, followed by context. It never submits: wait
for the terminal composer to be ready, add an explanation, then use Codex's send
or queue controls. PNG… attaches arbitrary local PNG/JPEG/WebP files. Native
Ctrl+V image paste remains available in the embedded Codex terminal.

Copy Image / Frame (Ctrl+Shift+C) copies only the currently decoded image's
pixels, preserving its alpha and decode bounds, or a rendered non-image frame.
It publishes image MIME data without asset path/text/URLs and creates no file.
Copy View Screenshot retains viewport framing/background for reference use.
Save Screenshot uses those same decoded pixels for images, preserving alpha and
dimensions independently of viewport background, pan and zoom. Ordinary screenshot
saves and window screenshots also use Pictures/Asset Preview, a sibling of Screenshots.

Verification: `tools/test.sh references-gpu` performs opt-in real Codex terminal
attachment, native clipboard image paste, annotation, queued prompts and
background continuation after the Preview client exits. It uses only its own
throwaway test conversations and isolated preview registry, archiving its test
conversations on exit. Screenshots/reports go under Pictures/Asset Preview.

`tools/test.sh reference-media-gpu` checks frozen viewport pixels/dimensions for
zoomed images, paused video and an orbited model on the actual NVIDIA renderer,
plus text/freehand/undo. `tools/test.sh codex-origin` checks independent caller
metadata and read-only request boundaries.

`tools/test.sh compact-gpu` verifies the default split at 700, 1000, 1400
and 1920 pixels, native splitter dragging, resize/collapse/restart persistence,
standard overlays, unchanged image view state and hardware 3D status/counts.
`tools/test.sh terminal-gpu` injects an agent-style environment and a local
terminal fixture, verifies its PTY/color capabilities and rendered truecolor
blocks, checks that a generator retains the caller's original environment,
and verifies normal/abnormal active exits and a parked client's exit/reconnect.
It does not attach to a real Codex conversation or send prompts.
