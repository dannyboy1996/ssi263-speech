"""Physical BT keys to the original six-dot Blazie keyboard, without translation."""

from dataclasses import dataclass

DOT7 = 1 << 6
DOT8 = 1 << 7
SPACE = 1 << 8
ADVANCE = 1 << 9
MENU = SPACE | DOT7 | 0x0D  # M-chord with dot 7: dots 1, 3, 4, 7 and space.
DEEP_ESCAPE = SPACE | DOT7 | 0x35  # Z-chord with dot 7: return to the host editor.


@dataclass(frozen=True)
class KeyAction:
    held: int = 0
    chord: int = 0
    menu: bool = False
    deep_escape: bool = False


def firmware_bits(bits: int) -> int:
    """The unit's bits 6 and 7 are space and advance, never dots 7 and 8."""
    return (bits & 0x3F) | (0x40 if bits & SPACE else 0) | (0x80 if bits & ADVANCE else 0)


class ChordKeyboard:
    def __init__(self, keys: dict[int, int]) -> None:
        self.keys = keys
        self.down = 0
        self.seen = 0

    def feed(self, code: int, pressed: bool) -> KeyAction | None:
        bit = self.keys.get(code)
        if bit is None:
            return None  # Unassigned panel and routing keys have no effect.
        if pressed:
            if self.down & bit:
                return None  # Auto-repeat is not another chord.
            self.down |= bit
            self.seen |= bit
        else:
            if not self.down & bit:
                return None  # A release from before capture began.
            self.down &= ~bit
        if self.down:
            # Once an extra dot is involved, suppress the entire gesture until release.
            # In particular, the M prefix must never become an m-chord in the firmware.
            return KeyAction(held=0 if self.seen & (DOT7 | DOT8) else firmware_bits(self.down))
        chord, self.seen = self.seen, 0
        if chord == MENU:
            return KeyAction(menu=True)
        if chord == DEEP_ESCAPE:
            return KeyAction(deep_escape=True)
        if chord & (DOT7 | DOT8):
            return KeyAction()
        return KeyAction(chord=firmware_bits(chord))
