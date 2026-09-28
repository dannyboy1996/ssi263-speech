"""Where a Braille Lite tab's time goes: the host calls between d.cancel()+d.speak() and the first audible block,
timed through the real driver (stand-in NVDA).  Wraps the host's cancel / send / run / say and counts pipe commands.

    python tab_profile.py
"""
import collections
import os
import sys
import time

sys.argv = [sys.argv[0], "blazie"]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
          encoding="utf-8").read().split("time.sleep(2.0)")[0])
time.sleep(1.5)
mark = len(notified)
d.speak(["Warm up."])
wait_idle()

stats = collections.defaultdict(lambda: [0, 0.0])
active = [False]


def wrap(obj, name):
    orig = getattr(obj, name)

    def w(*a, **k):
        t = time.perf_counter()
        try:
            return orig(*a, **k)
        finally:
            if active[0]:
                s = stats[name]
                s[0] += 1
                s[1] += time.perf_counter() - t
    setattr(obj, name, w)


unit = d._unit
for name in ("cancel", "send", "run", "say", "_cmd", "busy"):
    wrap(unit, name)

walls = []
for rep in range(12):
    d.speak(["Custom number processing", "check box", "checked"])
    time.sleep(0.15)
    active[0] = True
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
    active[0] = False
    mark = len(notified)
    wait_idle()
print("tab to first audible: %.0f ms median over %d" % (np.median(walls) * 1e3, len(walls)))
for name, (n, t) in sorted(stats.items(), key=lambda kv: -kv[1][1]):
    print("  %-7s %5d calls  %6.1f ms total  (%.2f ms each) per tab: %.1f calls, %.1f ms"
          % (name, n, t * 1e3, t * 1e3 / max(1, n), n / len(walls), t * 1e3 / len(walls)))
d.terminate()
