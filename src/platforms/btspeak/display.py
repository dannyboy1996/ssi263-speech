"""Present the firmware's raw cells without translating text or adding a host cursor."""

from types import ModuleType


class BrailleOutput:
    def __init__(self, brl: ModuleType) -> None:
        self.brl = brl
        self.last: bytes | None = None

    def show(self, cells: bytes | None, *, force: bool = False) -> None:
        # This initializes the shared braille connection and refreshes cached device dimensions.
        if not self.brl.has_display():
            self.last = None
            return
        width = self.brl.get_display_width()
        if width <= 0:
            self.last = None
            return
        # The English BL2000 has 18 cells. Leave the rest of a BT20/BT40 blank.
        frame = (cells or b"")[:width].ljust(width, b"\0")
        if force or frame != self.last:
            self.brl.write_dots(frame)
            self.last = frame
