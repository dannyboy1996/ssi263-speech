"""Braille Lite driver: dollar amounts with number processing on and off, as the phonemes
the chip was given.  A tester (0.5.0): with number processing on, "$25" lost its "dollars"
because the digits became words first and the firmware drops a "$" in front of words.

    py -3 nvda/tools/money_driver.py
"""
import os
import sys
import time

sys.argv = [sys.argv[0], "blazie"]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
          encoding="utf-8").read().split("time.sleep(2.0)")[0])
time.sleep(2.0)

TEXTS = ["It costs $25.", "It costs $1,234.56 today.", "$100,000,000,000,000.",
         "The file is 1,234,567.", "Pi is 3.14."]


def phonemes(text):
    global mark
    mark = len(notified)
    chip = d._unit.chip
    names = chip.rom.names
    seq, orig = [], chip.write

    def spy(reg, val):
        orig(reg, val)
        if reg == 0 and (val & 0x3F):
            seq.append(names.get(val & 0x3F, "?"))
    chip.write = spy
    try:
        d.speak([text])
        wait_idle()
    finally:
        chip.write = orig
    return " ".join(seq)


for on in (True, False):
    d._numbers = on
    print("number processing %s" % ("on" if on else "off"))
    for t in TEXTS:
        print("  %-28r %s" % (t, phonemes(t)))
d.terminate()
