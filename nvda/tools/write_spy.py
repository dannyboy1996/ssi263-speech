"""Watch every SSI-263 write a unit makes, whichever host drives the chip.

The Python hosts (blazie.py, the Speak-Out's and Accent's boxes) write through chip.write, so a spy there sees
everything.  The in-process Braille Lite (native_blazie.NativeBlazie) writes the chip from C: a spy on chip.write
sees NOTHING and a test built on it passes vacuously.  It reports its writes through on_write instead.
"""


def watch_writes(unit, fn):
    """Call fn(t, reg, val) on every write (t = the chip time it was applied at).  Returns a function that stops it."""
    if hasattr(type(unit), "on_write"):          # NativeBlazie
        prev = unit.on_write

        def hook(t, reg, val):
            if prev:
                prev(t, reg, val)
            fn(t, reg, val)
        unit.on_write = hook
        return lambda: setattr(unit, "on_write", prev)
    chip = unit.chip
    orig = chip.write
    had_own = "write" in vars(chip)

    def write(reg, val):
        orig(reg, val)
        fn(chip.time, reg, val)
    chip.write = write

    def stop():
        if had_own:
            chip.write = orig
        else:
            del chip.write
    return stop
