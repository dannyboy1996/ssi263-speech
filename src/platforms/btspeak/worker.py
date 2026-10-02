"""Bounded pipe communication with the native worker; no BT UI imports."""

import os
import select
import subprocess
import tempfile
import time
from gettext import gettext as _
from pathlib import Path


class WorkerError(RuntimeError):
    pass


class Worker:
    def __init__(self, executable: Path, firmware: Path, factory: Path | None, saved: Path,
                 device: str = "default", rate: int = 22050, kind: str = "bl") -> None:
        self.buffer = bytearray()
        self.braille: bytes | None = None
        self.errors = tempfile.TemporaryFile()
        try:
            self.process = subprocess.Popen(
                [str(executable), str(firmware), str(factory) if factory else "-", str(saved), device, str(rate), kind],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.errors, bufsize=0,
                start_new_session=True,  # The frontend owns Ctrl+C and requests a checked final save.
            )
        except BaseException:
            self.errors.close()
            raise
        assert self.process.stdin is not None and self.process.stdout is not None
        self.input = self.process.stdin
        self.output = self.process.stdout
        os.set_blocking(self.input.fileno(), False)
        try:
            self.wait_for("READY", timeout=30)
        except BaseException:
            self.abort()
            raise

    def send(self, command: str) -> None:
        data = (command + "\n").encode("ascii")
        try:
            if self.process.poll() is not None:
                raise WorkerError(_("The emulator stopped unexpectedly."))
            if os.write(self.input.fileno(), data) != len(data):
                raise WorkerError(_("The emulator could not accept the keyboard event."))
        except (BrokenPipeError, OSError) as exc:
            raise WorkerError(_("Lost the connection to the emulator.")) from exc

    def receive(self, timeout: float = 0) -> list[str]:
        if not select.select([self.output], [], [], timeout)[0]:
            return []
        data = os.read(self.output.fileno(), 4096)
        if not data:
            raise WorkerError(_("The emulator closed its connection."))
        self.buffer.extend(data)
        lines = []
        while b"\n" in self.buffer:
            line, remaining = self.buffer.split(b"\n", 1)
            self.buffer = bytearray(remaining)
            text = line.decode("utf-8", errors="replace")
            if text.startswith("ERROR "):
                raise WorkerError(text[6:])
            if text.startswith("BRAILLE "):
                try:
                    cells = bytes.fromhex(text[8:])
                except ValueError as exc:
                    raise WorkerError(_("Invalid braille display response.")) from exc
                if len(cells) not in (0, 18, 40):
                    raise WorkerError(_("Invalid braille display length."))
                self.braille = cells or None
                text = "BRAILLE"
            lines.append(text)
        return lines

    def wait_for(self, response: str, timeout: float = 5) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if response in self.receive(max(0, deadline - time.monotonic())):
                return
        raise WorkerError(_("The emulator did not respond in time."))

    def request(self, command: str, response: str) -> None:
        self.send(command)
        self.wait_for(response)

    def close(self) -> None:
        """Save and shut down; propagate save failures to the caller."""
        try:
            self.request("QUIT", "BYE")
            if self.process.wait(timeout=5):
                raise WorkerError(_("The emulator could not save its memory on exit."))
        finally:
            self.abort()

    def abort(self) -> None:
        """SIGTERM also asks the native worker to save. Kill only if it is stuck."""
        try:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
        finally:
            self.input.close()
            self.output.close()
            self.errors.close()
