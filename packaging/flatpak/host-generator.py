"""Run on the host via python3 -c; retire the exact generator child group."""
import contextlib
import os
import signal
import subprocess
import sys

cancelled = 0

def cancel(number, frame):
    global cancelled
    cancelled = number

for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
    signal.signal(number, cancel)

process = subprocess.Popen(sys.argv[1:], start_new_session=True)
try:
    while not cancelled:
        try:
            result = process.wait(timeout=0.1)
            break
        except subprocess.TimeoutExpired:
            pass
    else:
        result = 128 + cancelled
finally:
    # Flatpak watch-bus sends SIGINT; shell background jobs can ignore SIGINT.
    # Translate cancellation to TERM/KILL for the independently owned group.
    with contextlib.suppress(ProcessLookupError):
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=0.5)
    except subprocess.TimeoutExpired:
        pass
    with contextlib.suppress(ProcessLookupError):
        os.killpg(process.pid, signal.SIGKILL)
    process.wait()
sys.exit(result)
