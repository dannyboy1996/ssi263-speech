"""Does the Braille Lite driver keep the unit's channel open after speaking, as the real unit does?

After speech the real unit's channel stays open -- its hiss or whine goes on -- until the firmware clicks it off
(R3 = 00, ~9.95 s after the last phoneme on Tomi's unit).  With "keep the channel open" and the hiss or whine on, the
driver keeps the emulated unit running in real time after "done" (listeners, 2026-09-29).  The real driver under
stand-in NVDA; the tail's clock is sped up SPEED times.

  A  hiss, keep open: 8.5-11 s of audible hiss after "done", ending with the firmware's click-off (R3 = 00)
  B  new speech 2 s into the tail: the tail stops (the idle audio before the new speech is ~2 s, not ~10)
  C  keep open OFF (the control): nothing after "done"
  D  hiss off, keep open on: nothing after "done"

    python keep_open_test.py            KEEP_OPEN_BREAK=1: case A runs with keep open off -- the test must fail
"""
import os
import sys
import time

sys.argv = [sys.argv[0], "blazie"]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
          encoding="utf-8").read().split("time.sleep(2.0)")[0])
time.sleep(2.0)

SPEED = 20.0
_t0 = time.perf_counter()
drv_mod._now = lambda: _t0 + (time.perf_counter() - _t0) * SPEED
BREAK = os.environ.get("KEEP_OPEN_BREAK") == "1"


def fed():
    return sum(len(c) for c in list(d._player.chunks))


done_at = []
_notify = sdh.synthDoneSpeaking.notify


def notify_done(**kw):
    done_at.append(fed())            # samples fed when "done" fired (in the tail case: the speech's own end)
    _notify(**kw)


sdh.synthDoneSpeaking.notify = notify_done


def utter(text, whine, keep_open, wait_tail=True):
    d._set_whine(whine)
    d._set_keepOpen(keep_open)
    n_done = len(done_at)
    d.speak([text])
    t = time.time()
    while len(done_at) == n_done and time.time() - t < 30:
        time.sleep(0.02)
    if not wait_tail:
        return done_at[-1]
    t = time.time()                  # the tail: until R3 = 00 and the feeding stops, or 1.5 s of real time
    last = -1
    while time.time() - t < 30:
        time.sleep(0.2)
        now = fed()
        if d._unit.chip.regs[3] == 0 and now == last:
            break
        if not (keep_open and whine != "off") and time.time() - t > 1.5:
            break
        last = now
    return done_at[-1]


rate = None
bad = 0


def check(name, ok, detail):
    global bad
    bad += not ok
    print("%-4s %s: %s" % ("ok" if ok else "FAIL", name, detail))


import numpy as np   # noqa: E402

# A: the tail
utter("Warming up.", "hiss", True)          # the first utterance after a setting change reboots the unit
rate = float(d._unit.chip.out_rate)
d0 = utter("Hello there.", "hiss", not BREAK)
tail = audio_since(0)[d0:] if False else None
with d._player.lock:
    y = np.concatenate(d._player.chunks)
tail = y[d0:]
tail_s = len(tail) / rate
rms = float(np.sqrt(np.mean(tail[:int(5 * rate)] ** 2))) if len(tail) else 0.0
check("A hiss, keep open", 8.5 <= tail_s <= 11.0 and d._unit.chip.regs[3] == 0 and rms > 1e-6,
      "%.2f s after done, R3 %02X at the end, rms %.2e over its first 5 s" % (tail_s, d._unit.chip.regs[3], rms))

# B: interrupted by new speech 2 s (tail time) in
d1 = utter("Hello there.", "hiss", True, wait_tail=False)
time.sleep(2.0 / SPEED)
n_speak = fed()
utter("OK.", "hiss", True, wait_tail=False)
d.cancel()                                  # end the second tail
with d._player.lock:
    y = np.concatenate(d._player.chunks)
seg = y[n_speak:]
first_loud = int(np.argmax(np.abs(seg) > 0.02)) if np.any(np.abs(seg) > 0.02) else len(seg)
# what matters is after the new speak(): the tail stops and the speech follows the idle already fed (at most
# IDLE_AHEAD_S, plus a block).  The tail fed before it depends on how much CPU the tail thread got (the full
# run_tests load gave 0.76 s where an idle machine gives ~1.5 s), so it is only required to have been running.
tail_before = (n_speak - d1) / rate
idle_after = first_loud / rate
check("B new speech stops the tail", tail_before > 0.1 and idle_after <= 0.4,
      "the tail had run %.2f s; %.3f s of idle after the new speak() before its speech" % (tail_before, idle_after))
time.sleep(0.5)

# C: the control -- keep open off
d2 = utter("Hello there.", "hiss", False)
check("C keep open off", fed() - d2 < int(0.05 * rate), "%.3f s after done" % ((fed() - d2) / rate))

# D: hiss off
utter("Warming up.", "off", True)
d3 = utter("Hello there.", "off", True)
check("D hiss off", fed() - d3 < int(0.05 * rate), "%.3f s after done" % ((fed() - d3) / rate))

# E: the first speech after the firmware clicked the channel off starts no later than with keep open off
#    (Tomi: keeping it open must not cost response time)
def first_sound_ms(text, keep_open, runs=5):
    """Median wall time from speak() to the first audible sample fed, over a few runs."""
    out = []
    for _ in range(runs):
        if keep_open == "open":
            utter("Warm.", "hiss", True, wait_tail=False)   # interrupt the open channel, as NVDA does:
            time.sleep(0.05)
            d.cancel()                                      # cancel, then speak
        elif keep_open:
            utter("Warm.", "hiss", True)                 # ... and wait out the tail to the click-off
            assert d._unit.chip.regs[3] == 0
        else:
            utter("Warm.", "hiss", False)
        n = fed()
        t = time.perf_counter()
        d.speak([text])
        while time.perf_counter() - t < 5:
            with d._player.lock:
                y = np.concatenate(d._player.chunks) if d._player.chunks else np.zeros(0)
            if np.any(np.abs(y[n:]) > 0.02):
                break
            time.sleep(0.002)
        out.append((time.perf_counter() - t) * 1e3)
        d.cancel()
        time.sleep(0.2)
    return sorted(out)[len(out) // 2]


after_off = first_sound_ms("Hello there.", True)
during = first_sound_ms("Hello there.", "open")
baseline = first_sound_ms("Hello there.", False)
check("E first speech after the click-off", after_off <= baseline + 15.0,
      "%.1f ms after the click-off, %.1f ms with keep open off (median of 5)" % (after_off, baseline))
check("F speech interrupting the open channel", during <= baseline + 15.0,
      "%.1f ms with the channel open (cancel, then speak), %.1f ms with keep open off" % (during, baseline))

print("keep open: %s" % ("PASS" if not bad else "%d FAILED" % bad))
sys.stdout.flush()
d.terminate()
os._exit(1 if bad else 0)
