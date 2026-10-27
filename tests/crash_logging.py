"""Crash the isolated native service; never touch the desktop singleton."""
import json
import os
from pathlib import Path
import resource
import runpy
import shlex
import signal
import socket
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def no_core():
    # Exercise real fatal signals without filling system coredump storage.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


class CrashLogging(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = tempfile.TemporaryDirectory(prefix="preview-native-fault-")
        root = Path(cls.fixture.name)
        source = root / "fault.cpp"
        source.write_text('''#include "crash_log.h"
#include <QByteArray>
#include <QDebug>
#include <stdexcept>
#include <sys/mman.h>
int main(int argc, char** argv) {
    preview::CrashLog log(QString::fromLocal8Bit(argv[1]));
    QString error;
    if (!log.beginSession(error)) return log.finish(1);
    if (argc > 2) {
        auto* address=static_cast<volatile int*>(mmap(nullptr,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        *address=42;
    }
    qWarning("old discarded diagnostic");
    for (int i=0; i<100; ++i) qWarning("bounded-message-%d %s",i,QByteArray(2048,'x').constData());
    throw std::runtime_error("crash logging exception fixture");
}
''')
        cls.native_fault = root / "fault"
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "Qt6Core"], text=True))
        subprocess.run(["c++", "-std=c++20", "-O2", "-fPIC", "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror",
                        '-DASSET_PREVIEW_VERSION="fixture"', "-I" + str(ROOT / "src"), "-I" + str(ROOT / "build/generated"),
                        str(source), str(ROOT / "src/crash_log.cpp"), "-o", str(cls.native_fault), *flags],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.fixture.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="preview-crash-test-")
        self.root = Path(self.temporary.name)
        self.runtime = self.root / "runtime"
        self.state = self.root / "state"
        self.runtime.mkdir(mode=0o700)
        self.state.mkdir(mode=0o700)
        self.socket = self.runtime / "preview.sock"
        self.log = self.state / "crash.log"
        self.marker = self.state / "session.running.json"
        self.processes = []
        self.environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", ASSET_PREVIEW_TEST_NULL_AUDIO="1",
                                ASSET_PREVIEW_RUNTIME_DIR=str(self.runtime), ASSET_PREVIEW_STATE_DIR=str(self.state))

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None: process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
        self.temporary.cleanup()

    def rpc(self, method, **arguments):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(3)
            client.connect(str(self.socket))
            client.sendall(json.dumps(dict(method=method, **arguments)).encode() + b"\n")
            data = b""
            while b"\n" not in data:
                part = client.recv(65536)
                if not part: raise RuntimeError("Service disconnected")
                data += part
        result = json.loads(data.split(b"\n")[0])
        self.assertTrue(result["ok"], result)
        return result

    def spawn(self, **environment):
        process = subprocess.Popen([str(ROOT / "build/asset-preview-server"), "--socket", str(self.socket),
                                    "--state", str(self.state)], env=dict(self.environment, **environment),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   preexec_fn=no_core)
        self.processes.append(process)
        return process

    def start(self):
        process = self.spawn()
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.assertIsNone(process.poll(), self.log.read_text() if self.log.exists() else "No crash log")
            try:
                if self.rpc("ping")["pid"] == process.pid: return process
            except (OSError, RuntimeError): pass
            time.sleep(.02)
        self.fail("Service startup timed out")

    def stop(self, process):
        self.rpc("quit")
        self.assertEqual(process.wait(timeout=5), 0)

    def test_normal_use_stop_and_signals_write_no_crash_records(self):
        for stop_signal in (None, signal.SIGTERM, signal.SIGINT):
            process = self.start()
            initial = self.log.stat()
            marker = self.marker.read_bytes()
            for _ in range(4): self.rpc("list")
            self.assertEqual(self.log.stat().st_mtime_ns, initial.st_mtime_ns)
            self.assertEqual(self.marker.read_bytes(), marker)
            if stop_signal: process.send_signal(stop_signal)
            else: self.rpc("quit")
            self.assertEqual(process.wait(timeout=5), 0)
            self.assertEqual(self.log.read_bytes(), b"")
            self.assertFalse(self.marker.exists())
            self.assertFalse((self.state / "server.log").exists())
        self.assertEqual(self.log.stat().st_mode & 0o777, 0o600)

    def test_fatal_signals_keep_context_and_survive_clean_restarts(self):
        for crash_signal in (signal.SIGSEGV, signal.SIGABRT, signal.SIGBUS, signal.SIGILL, signal.SIGFPE, signal.SIGSYS, signal.SIGTRAP):
            process = self.start()
            asset = self.root / "inspected texture.png"
            self.rpc("add", entry={"id": "crash-fixture", "path": str(asset), "kind": "image"})
            self.rpc("list") # Status probes must retain the last useful action.
            process.send_signal(crash_signal)
            self.assertEqual(process.wait(timeout=5), -crash_signal)
            report = self.log.read_text()
            self.assertIn("signal=" + crash_signal.name, report)
            self.assertIn('"selected":"crash-fixture"', report)
            self.assertIn(str(asset), report)
            self.assertIn('"method":"add"', report)
            self.assertIn("instruction_pointer=0x", report)
            self.assertIn("backend_abi=", report)
            self.assertIn("loaded_modules", report)
            self.assertFalse(self.marker.exists())
            restarted = self.start()
            self.stop(restarted)
            self.assertEqual(self.log.read_text(), report)
            self.assertNotIn("event=unclean_exit", report)

    def test_qt_fatal_during_startup_includes_recent_diagnostics(self):
        process = self.spawn(QT_QPA_PLATFORM="asset-preview-missing-platform")
        self.assertEqual(process.wait(timeout=5), -signal.SIGABRT)
        report = self.log.read_text()
        self.assertIn("signal=SIGABRT", report)
        self.assertIn("asset-preview-missing-platform", report)
        self.assertIn("recent_qt_diagnostics", report)

    def test_uncaught_exception_retains_its_message_and_bounded_diagnostics(self):
        result = subprocess.run([str(self.native_fault), str(self.state)], capture_output=True, preexec_fn=no_core)
        self.assertEqual(result.returncode, -signal.SIGABRT)
        report = self.log.read_text()
        self.assertIn("crash logging exception fixture", report)
        self.assertIn("bounded-message-99", report)
        self.assertNotIn("old discarded diagnostic", report)
        self.assertLess(self.log.stat().st_size, 192 * 1024)

    def test_real_memory_fault_retains_fault_address_and_native_frames(self):
        result = subprocess.run([str(self.native_fault), str(self.state), "fault"], capture_output=True, preexec_fn=no_core)
        self.assertEqual(result.returncode, -signal.SIGSEGV)
        report = self.log.read_text()
        self.assertIn("signal=SIGSEGV", report)
        self.assertIn("address=0x", report)
        frames = report.split("native_frames", 1)[1].split("loaded_modules", 1)[0]
        self.assertGreaterEqual(frames.count("0x"), 2)

    def test_source_cli_preserves_historical_logs_and_writes_no_session_log(self):
        old_log = self.state / "server.log"
        old_log.write_text("historical evidence")
        client = runpy.run_path(str(ROOT / "bin/asset-preview"))
        for _ in range(2):
            with patch.dict(os.environ, self.environment): client["start"]()
            process = client["_SPAWNED"][-1]
            self.processes.append(process)
            self.stop(process)
        self.assertEqual(self.log.read_bytes(), b"")
        self.assertEqual(old_log.read_text(), "historical evidence")

    def test_unclean_kill_is_reported_once_on_the_next_launch(self):
        process = self.start()
        process.kill()
        self.assertEqual(process.wait(timeout=5), -signal.SIGKILL)
        self.assertEqual(self.log.read_bytes(), b"")
        self.assertTrue(self.marker.exists())
        restarted = self.start()
        self.stop(restarted)
        report = self.log.read_text()
        self.assertIn("event=unclean_exit", report)
        self.assertIn('"pid":' + str(process.pid), report)
        self.assertIn("Signal/cause unavailable", report)
        again = self.start()
        self.stop(again)
        self.assertEqual(self.log.read_text(), report)

    def test_startup_failure_keeps_its_error_and_exit_code(self):
        (self.state / "previews.json").write_text("invalid fixture")
        process = self.spawn()
        self.assertEqual(process.wait(timeout=5), 1)
        report = self.log.read_text()
        self.assertIn("event=nonzero_exit", report)
        self.assertIn("exit_code=1", report)
        self.assertIn("Invalid preview state", report)
        self.assertFalse(self.marker.exists())

    def test_duplicate_start_does_not_change_the_live_session_or_log(self):
        owner = self.start()
        marker = self.marker.read_bytes()
        duplicate = self.spawn()
        self.assertEqual(duplicate.wait(timeout=5), 3)
        self.assertEqual(self.marker.read_bytes(), marker)
        self.assertEqual(self.log.read_bytes(), b"")
        self.stop(owner)

    def test_rotation_preserves_one_backup_and_never_truncates_on_start(self):
        previous = (b"older crash report\n" * 60000)[:1024 * 1024]
        self.log.write_bytes(previous)
        process = self.start()
        self.assertEqual(self.log.read_bytes(), previous)
        process.send_signal(signal.SIGABRT)
        self.assertEqual(process.wait(timeout=5), -signal.SIGABRT)
        self.assertEqual((self.state / "crash.log.1").read_bytes(), previous)
        self.assertLess(self.log.stat().st_size, 192 * 1024)
        report = self.log.read_bytes()
        restarted = self.start()
        self.stop(restarted)
        self.assertEqual(self.log.read_bytes(), report)
        self.assertEqual((self.state / "crash.log.1").read_bytes(), previous)

    def test_symlinked_log_or_marker_never_modifies_its_target(self):
        target = self.root / "unrelated"
        target.write_text("preserve me")
        for path in (self.log, self.marker):
            with self.subTest(path=path.name):
                if path.exists(): path.unlink()
                path.symlink_to(target)
                process = self.spawn()
                self.assertEqual(process.wait(timeout=5), 1)
                self.assertEqual(target.read_text(), "preserve me")
                self.assertTrue(path.is_symlink())
                path.unlink()


if __name__ == "__main__":
    unittest.main(verbosity=2)
