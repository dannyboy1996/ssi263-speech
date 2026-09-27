"""A tester (0.5.0): stepping NVDA's rate between 94 and 95 %, "the last audio chunk from the
current utterance is cut off, then is joined to the next utterance".  Stepping a slider is
keypress (NVDA cancels speech), rate change, speak the new value.  This does exactly that
through the real driver, cancelling at a range of points into the first utterance, and
lists the phonemes the chip loads from the second say on: any phoneme of the first text
there is its tail, joined to the next utterance.

    python nvda/tools/cancel_tail.py
"""
import os
import sys
import time

sys.argv = [sys.argv[0], "blazie"]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
          encoding="utf-8").read().split("time.sleep(2.0)")[0])
time.sleep(2.0)

loads = []           # (tag, phoneme), tag = which say() the unit had last been given
tag = ["-"]


def install(unit):
    chip, names = unit.chip, unit.chip.rom.names
    orig_write, orig_say = chip.write, unit.say

    def write(reg, val):
        orig_write(reg, val)
        if reg == 0 and (val & 0x3F):
            loads.append((tag[0], names.get(val & 0x3F, "?")))

    def say(lines):
        tag[0] = "B" if tag[0] == "A" else "A"
        loads.append((tag[0], "<say %r>" % (lines,)))
        return orig_say(lines)
    chip.write, unit.say = write, say


while d._unit is None:
    time.sleep(0.05)
install(d._unit)
d._player.pace = True
bad = 0
for first, second in ((94, 95), (95, 94)):
    for delay in (0.15, 0.3, 0.45, 0.6, 0.8, 1.0, 1.3, 1.6):
        d._set_rate(first)
        tag[0] = "-"
        mark = len(notified)
        d.speak(["%d" % first])
        wait_idle()                               # the settings are in place; now the real test
        del loads[:]
        tag[0] = "-"
        d.speak(["%d" % first])
        time.sleep(delay)
        d.cancel()
        d._set_rate(second)
        mark = len(notified)
        d.speak(["%d" % second])
        wait_idle()
        after = [p for t, p in loads if t == "B"]
        text_a = " ".join(p for t, p in loads if t == "A" and not p.startswith("<"))
        text_b = " ".join(after[1:])
        print("rate %d -> %d, cancel at %.2f s" % (first, second, delay))
        print("   A = %s" % text_a)
        print("   B = %s" % text_b)
d.terminate()
