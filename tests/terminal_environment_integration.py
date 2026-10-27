#!/usr/bin/env python3
"""Real Konsole PTY color checks with a local fixture and no Codex prompts."""
import json
import os
from pathlib import Path
import socket
import signal
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch

import integration as h

KEYS = ("TERM", "COLORTERM", "NO_COLOR", "FORCE_COLOR", "CLICOLOR", "CLICOLOR_FORCE")
AGENT = {"TERM": "dumb", "COLORTERM": "", "NO_COLOR": "1", "FORCE_COLOR": "0", "CLICOLOR": "0", "CLICOLOR_FORCE": "0"}
COLORS = ((230, 40, 50), (30, 210, 80), (40, 100, 240))


def main():
    assert os.environ.get("QT_QPA_PLATFORM") == "xcb", "Run with tools/test.sh terminal-gpu"
    graphics = subprocess.check_output(["glxinfo", "-B"], text=True)
    renderer = next(line.split(":", 1)[1].strip() for line in graphics.splitlines() if line.startswith("OpenGL renderer string:"))
    assert not any(word in renderer.lower() for word in ("llvmpipe", "softpipe", "software", "unknown")), renderer
    print("OpenGL renderer:", renderer, flush=True)
    with tempfile.TemporaryDirectory(prefix="preview-terminal-color-") as directory:
        root = Path(directory)
        (root / "bin").mkdir()
        fake = root / "bin/codex"
        fake.write_text(f"#!{sys.executable}\n" + '''import json, os, signal, sys, time
from pathlib import Path
keys = ("TERM", "COLORTERM", "NO_COLOR", "FORCE_COLOR", "CLICOLOR", "CLICOLOR_FORCE")
state = {key: os.environ.get(key) for key in keys}
state.update(pid=os.getpid(), tty=os.isatty(0) and os.isatty(1), args=sys.argv[1:])
signal.signal(signal.SIGUSR1, lambda *_: sys.exit(0))
output = Path(__file__).with_suffix(".json")
candidate = output.with_suffix(".pending")
candidate.write_text(json.dumps(state)); candidate.replace(output)
print("Embedded terminal color fixture", flush=True)
for _ in range(6):
    print("".join(f"\\033[48;2;{r};{g};{b}m          " for r,g,b in ((230,40,50),(30,210,80),(40,100,240))) + "\\033[0m", flush=True)
time.sleep(120)
''')
        fake.chmod(0o700)
        endpoint = socket.socket(socket.AF_UNIX)
        endpoint.bind(str(root / "codex.sock"))
        (root / "codex.sock").chmod(0o600)
        environment = dict(AGENT, ASSET_PREVIEW_RUNTIME_DIR=str(root / "runtime"),
                           ASSET_PREVIEW_STATE_DIR=str(root / "state"),
                           PATH=str(root / "bin") + os.pathsep + os.environ["PATH"], QT_XCB_NO_XI2="1",
                           CODEX_THREAD_ID="", CODEX_SESSION_ID="")
        input = None
        with patch.dict(os.environ, environment):
            try:
                h.client.start()
                asset = root / "image.png"
                h.png(asset)
                origin = {"thread_id": "00000000-0000-4000-8000-000000000001", "cwd": str(root), "socket": str(root / "codex.sock")}
                h.client.rpc({"method": "add", "entry": {"id": "image", "path": str(asset), "codex": origin}})
                h.client.rpc({"method": "show"})
                input = h.XInput()
                control = h.client.rpc({"method": "list"})["presentation"]["reference_controls"]["codexToggle"]
                input.click(control["x"] + control["width"] // 2, control["y"] + control["height"] // 2)
                output = fake.with_suffix(".json")
                h.eventually(output.exists)
                terminal = json.loads(output.read_text())
                assert terminal["tty"], terminal
                assert terminal["TERM"] == "xterm-256color", terminal
                assert terminal["COLORTERM"] == "truecolor", terminal
                assert all(terminal[key] is None for key in KEYS[2:]), terminal
                assert terminal["args"] == ["--remote", "unix://" + str(root / "codex.sock"), "resume", origin["thread_id"], "--cd", str(root)], terminal
                time.sleep(.25)
                snapshot = root / "terminal.png"
                h.client.rpc({"method": "capture", "path": str(snapshot)})
                bounds = h.client.rpc({"method": "list"})["presentation"]["codex"]
                points = [(x, y) for y in range(bounds["y"], min(bounds["y"] + bounds["height"], bounds["y"] + 230), 4)
                          for x in range(bounds["x"], min(bounds["x"] + bounds["width"], bounds["x"] + 400), 4)]
                pixels = set(h.png_pixels(snapshot, points))
                assert all(color in pixels for color in COLORS), "Terminal did not render the truecolor blocks"
                # A later generator must retain the original agent environment.
                generator_output = root / "generator.json"
                code = f"import json,os; from pathlib import Path; p=Path({str(generator_output)!r}); c=p.with_suffix('.pending'); c.write_text(json.dumps({{k:os.environ.get(k) for k in {KEYS!r}}})); c.replace(p)"
                h.client.rpc({"method": "add", "entry": {"id": "generator", "path": str(asset), "cwd": str(root),
                                                           "command": [sys.executable, "-c", code]}})
                h.client.rpc({"method": "select", "id": "generator"})
                h.eventually(generator_output.exists)
                assert json.loads(generator_output.read_text()) == AGENT
                terminal_state = lambda: h.client.rpc({"method": "list"})["presentation"]["codex"]
                def click(name):
                    control = h.client.rpc({"method": "list"})["presentation"]["reference_controls"][name]
                    input.click(control["x"] + control["width"] // 2, control["y"] + control["height"] // 2)
                # Normal exit of a parked client must not affect the active one.
                assert terminal_state()["client_pid"] == terminal["pid"]
                other = dict(origin, thread_id="00000000-0000-4000-8000-000000000002")
                h.client.rpc({"method": "add", "entry": {"id": "other", "path": str(asset), "codex": other}})
                h.client.rpc({"method": "select", "id": "other"})
                click("codexReconnect")
                h.eventually(lambda: terminal_state()["connected"] and terminal_state()["thread_id"] == other["thread_id"]
                             and json.loads(output.read_text())["pid"] != terminal["pid"])
                other_pid = terminal_state()["client_pid"]
                os.kill(terminal["pid"], signal.SIGUSR1)
                h.eventually(lambda: not Path(f'/proc/{terminal["pid"]}').exists())
                assert terminal_state()["client_pid"] == other_pid
                h.client.rpc({"method": "select", "id": "image"})
                click("codexReconnect")
                h.eventually(lambda: terminal_state()["connected"] and terminal_state()["thread_id"] == origin["thread_id"]
                             and json.loads(output.read_text())["pid"] not in (terminal["pid"], other_pid))
                assert terminal_state()["retained_clients"] == 1
                # Both abnormal and normal exits must reconnect to a fresh client.
                for exit_signal in (signal.SIGTERM, signal.SIGUSR1):
                    previous_pid = terminal_state()["client_pid"]
                    os.kill(previous_pid, exit_signal)
                    h.eventually(lambda: not terminal_state()["connected"])
                    for _ in range(2):
                        visible = terminal_state()["visible"]
                        click("codexToggle")
                        h.eventually(lambda: terminal_state()["visible"] != visible)
                    h.eventually(lambda: terminal_state()["connected"] and json.loads(output.read_text())["pid"] != previous_pid)
                    assert terminal_state()["retained_clients"] == 1
                print("PASS native Konsole PTY: truecolor, exact remote arguments, unchanged generator environment, active/parked exit and reconnect", flush=True)
                if os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR"):
                    evidence = Path(os.environ["ASSET_PREVIEW_EVIDENCE_DIR"])
                    evidence.mkdir(parents=True, exist_ok=True)
                    (evidence / "terminal.png").write_bytes(snapshot.read_bytes())
                    (evidence / "verification.json").write_text(json.dumps({"renderer": renderer, "terminal": terminal, "generator": AGENT, "truecolor_pixels": COLORS}, indent=2))
            finally:
                if os.environ.get("ASSET_PREVIEW_EVIDENCE_DIR"):
                    evidence = Path(os.environ["ASSET_PREVIEW_EVIDENCE_DIR"])
                    evidence.mkdir(parents=True, exist_ok=True)
                    log = root / "state/server.log"
                    if log.exists():
                        (evidence / "server.log").write_bytes(log.read_bytes())
                    if h.client.running():
                        final = evidence / "final.png"
                        final.unlink(missing_ok=True)
                        h.client.rpc({"method": "capture", "path": str(final)})
                if input:
                    input.close()
                if h.client.running():
                    h.client.rpc({"method": "quit"})
                    h.eventually(lambda: not h.client.running())
                endpoint.close()


if __name__ == "__main__":
    main()
