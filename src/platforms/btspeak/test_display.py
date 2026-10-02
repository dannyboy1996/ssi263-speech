"""Raw output and the pipe protocol, without touching the physical display."""

import socket
import unittest
from types import SimpleNamespace
from unittest.mock import Mock

from display import BrailleOutput
from worker import Worker, WorkerError


class DisplayTests(unittest.TestCase):
    def test_pad_to_device_width_and_preserve_all_dots(self):
        cells = bytes([0x80, 0x40, 0xC0, 0xFF] + list(range(14)))
        for width in (20, 40):
            brl = SimpleNamespace(has_display=lambda: True, get_display_width=lambda: width, write_dots=Mock())
            output = BrailleOutput(brl)
            output.show(cells)
            brl.write_dots.assert_called_once_with(cells + bytes(width - 18))
            output.show(cells)
            self.assertEqual(brl.write_dots.call_count, 1)
            output.show(cells, force=True)  # A host menu has overwritten the device's cells.
            self.assertEqual(brl.write_dots.call_count, 2)
            output.show(None)
            brl.write_dots.assert_called_with(bytes(width))

    def test_no_display_and_reconnection(self):
        brl = SimpleNamespace(has_display=Mock(return_value=False), get_display_width=Mock(return_value=0), write_dots=Mock())
        output = BrailleOutput(brl)
        output.show(bytes(18))
        brl.write_dots.assert_not_called()
        brl.get_display_width.return_value = 20
        brl.has_display.return_value = True
        output.show(bytes(18))
        brl.write_dots.assert_called_once_with(bytes(20))

    def make_worker(self):
        reader, writer = socket.socketpair()
        self.addCleanup(reader.close)
        self.addCleanup(writer.close)
        worker = Worker.__new__(Worker)
        worker.buffer = bytearray()
        worker.braille = None
        worker.output = reader.makefile("rb", buffering=0)
        self.addCleanup(worker.output.close)
        return worker, writer

    def test_fragmented_frame_and_command_reply_in_same_read(self):
        worker, writer = self.make_worker()
        cells = bytes(range(18))
        wire = b"BRAILLE " + cells.hex().encode() + b"\nPAUSED\n"
        writer.sendall(wire[:13])
        self.assertEqual(worker.receive(), [])
        self.assertIsNone(worker.braille)
        writer.sendall(wire[13:])
        self.assertEqual(worker.receive(), ["BRAILLE", "PAUSED"])
        self.assertEqual(worker.braille, cells)
        writer.sendall(b"BRAILLE \n")
        self.assertEqual(worker.receive(), ["BRAILLE"])
        self.assertIsNone(worker.braille)

    def test_malformed_frame_is_reported(self):
        for wire in (b"BRAILLE gg\n", b"BRAILLE 00\n"):
            worker, writer = self.make_worker()
            writer.sendall(wire)
            with self.assertRaises(WorkerError):
                worker.receive()


if __name__ == "__main__":
    unittest.main()
