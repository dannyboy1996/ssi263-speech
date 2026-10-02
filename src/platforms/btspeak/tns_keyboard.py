"""Six-dot computer braille to the Type 'n Speak's QWERTY keys only.

The Braille Lite and Braille 'n Speak never pass through this adapter. Key names are
resolved by the original emulator's keys.c/tns_term.c, not copied hardware scancodes.
"""

# North American computer braille, matching bl_keys.c CELLS (dot 1 is bit 0).
CELLS = " A1B'K2L@CIF/MSP\"E3H9O6R^DJG>NTQ,*5<-U8V.%[$+X!&;:4\\0Z7(_?W]#Y)="
SPECIAL = {0x51: "enter", 0x43: "backspace", 0x64: "esc", 0x4A: "tab",
           0x41: "up", 0x48: "down", 0x42: "left", 0x50: "right"}


def key_name(chord: int) -> str | None:
    if chord in SPECIAL:
        return SPECIAL[chord]
    cell = chord & 63
    if not cell:
        return "space" if chord == 64 else None
    letter = CELLS[cell].lower()
    if chord & 64:
        return "ctrl-" + letter
    return letter
