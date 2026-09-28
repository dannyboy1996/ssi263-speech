"""Rapid tabbing, through the real driver (stand-in NVDA): speak an item, cancel it 150 ms in, speak the next one at
once -- as NVDA does on every Tab -- and time the new speak() to its first audible sample.  Also split out the leading
silence fed before that sample (audio the listener hears as delay too).

    python tab_latency.py blazie|speakout|accent
"""
import os
import sys
import time

WHICH = sys.argv[1]
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
           encoding="utf-8").read()
exec(src.split("time.sleep(2.0)")[0])
time.sleep(1.5)

LONG = ["The quick brown fox jumps over the lazy dog, and then it runs far away into the forest. "
        "Meanwhile the farmer counts his chickens, one by one, and wonders where the fox has gone. "
        "It is a long story, and it goes on for quite a while before anything happens at all."]
ITEMS = [["Select synthesizer", "dialog"], ["OK", "button"], ["Cancel", "button"], ["Voice", "combo box", "Braille Lite"],
         ["Rate", "slider", "50"], ["Custom number processing", "check box", "checked"]]


def first_audible(n0, t0, rate):
    while time.perf_counter() - t0 < 10:
        with d._player.lock:
            chunks = d._player.chunks[n0:]
        fed = 0
        for c in chunks:
            a = np.asarray(c, float)             # the stand-in player keeps floats (int16 / 32767)
            k = np.where(np.abs(a) > 0.01)[0]
            if len(k):
                return time.perf_counter() - t0, (fed + k[0]) / rate
            fed += len(a)
        time.sleep(0.0005)
    return None, None


for arg in sys.argv[2:]:                     # e.g. whine=whine sampleRate=44100
    k, _, v = arg.partition("=")
    getattr(d, "_set_" + k)(v)
mark = len(notified)
d.speak(["Settle."])
wait_idle()
rate = getattr(d, "_out_rate", 44100)
mark = len(notified)
d.speak(["Warm up."])
wait_idle()
rows = []
for rep in range(3):
    for k, item in enumerate(ITEMS):
        prev = LONG if os.environ.get("TAB_LONG") else ITEMS[k - 1]
        d.speak(prev)                          # the previous item (or a long paragraph), interrupted 150 ms in
        time.sleep(0.15)
        d.cancel()
        n0 = len(d._player.chunks)
        t0 = time.perf_counter()
        d.speak(item)
        wall, lead = first_audible(n0, t0, rate)
        if wall is not None:
            rows.append((wall * 1e3, lead * 1e3))
mark = len(notified)
wait_idle()
w = np.array([r[0] for r in rows])
lead = np.array([r[1] for r in rows])
print("%-8s tab after cancel, %d items: first audible fed %.0f ms median (%.0f-%.0f), of which leading silence in the "
      "audio %.0f ms median; heard at ~%.0f ms median" % (WHICH, len(rows), np.median(w), w.min(), w.max(),
                                                          np.median(lead), np.median(w)))
d.terminate()
