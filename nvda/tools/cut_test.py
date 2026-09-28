"""The cancel cut interval (Blazie host cancel_cut): tab latency against the tail bug, per interval.

For each cut: (1) cancel_tail's slider case -- speak "94", cancel at 0.15-1.6 s, speak "95" -- where the second
utterance must load exactly the phonemes a clean "95" loads (anything else is the first one's tail); (2) the
tab_latency case, speak() to the first audible block after a cancel.

    python cut_test.py
"""
import os
import sys
import time

from write_spy import watch_writes

sys.argv = [sys.argv[0], "blazie"]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
          encoding="utf-8").read().split("time.sleep(2.0)")[0])
time.sleep(1.5)
names = d._unit.chip.rom.names if hasattr(d._unit.chip, "rom") else None
if names is None:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "src"))
    from ssi263 import SSI263
    names = SSI263().rom.names
loads, tag = [], ["-"]


def install(unit):
    osay = unit.say

    def write(t, reg, val):
        if reg == 0 and (val & 0x3F) and names.get(val & 0x3F) != "PA":
            loads.append((tag[0], names.get(val & 0x3F, "?")))

    def say(lines):
        # label by what is actually SENT: loads during the worker's cancel flush (audio discarded) stay with the
        # first utterance; only what comes after the second text reaches the unit counts as its own
        tag[0] = "B" if any("95" in ln for ln in lines) else "A"
        return osay(lines)
    watch_writes(unit, write)
    unit.say = say


install(d._unit)


def spoken(text):
    global mark
    del loads[:]
    tag[0] = "B"
    mark = len(notified)
    d.speak([text])
    wait_idle()
    return [p for t, p in loads if t == "B"]


ref95 = spoken("95")
import os as _os
REPS = int(_os.environ.get("CUT_REPS", "1"))
for cut in tuple(float(x) for x in os.environ.get("CUTS", "0.02,0.05,0.1").split(",")):
    d._unit.cancel_cut = cut
    d._player.pace = True
    bad = 0
    for delay in [x for x in (0.15, 0.3, 0.45, 0.6, 0.8, 1.0, 1.3, 1.6) for _ in range(REPS)]:
        mark = len(notified)
        d.speak(["94"])
        wait_idle()
        del loads[:]
        tag[0] = "A"
        d.speak(["94"])
        time.sleep(delay)
        d.cancel()
        mark = len(notified)
        d.speak(["95"])
        wait_idle()
        got = [p for t, p in loads if t == "B"]
        if got != ref95:
            bad += 1
            print("   cut %.0f ms, cancel at %.2f s: B = %s | ref = %s | A = %s" % (cut * 1000, delay, " ".join(got), " ".join(ref95), " ".join(p for t, p in loads if t == "A")))
    d._player.pace = False
    walls = []
    for rep in range(12):
        d.speak(["Custom number processing", "check box", "checked"])
        time.sleep(0.15)
        t0 = time.perf_counter()
        d.cancel()
        n0 = len(d._player.chunks)
        d.speak(["OK", "button"])
        while time.perf_counter() - t0 < 5:
            with d._player.lock:
                ch = d._player.chunks[n0:]
            if any(np.any(np.abs(c) > 0.01) for c in ch):
                break
            time.sleep(0.0005)
        walls.append(time.perf_counter() - t0)
        mark = len(notified)
        wait_idle()
    print("cut %.0f ms: tail bug in %d of %d cancel points; tab to first audible %.0f ms median" % (cut * 1000, bad, 8 * REPS, np.median(walls) * 1e3))
d.terminate()
