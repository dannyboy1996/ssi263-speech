"""New speech that follows the open channel's idle audio WITHOUT a cancel: how long until it is heard?

With "keep the channel open" and the hiss or whine on, the Braille Lite driver keeps feeding the unit's idle audio
after speech (_idle_tail), paced to stay at most IDLE_AHEAD_S ahead of the listener.  New speech that NVDA queues
without a cancel (a notification, typed text with interruption off) plays after whatever idle audio is already fed,
so that lead is heard as delay.  Before 0.7 the tail counted "ahead" from its own start, not from what the listener had
heard: the speech's own unplayed audio (a device buffer's worth, or the whole utterance when feeding outruns playback)
was not counted, and the idle audio queued behind it grew by as much.

The real driver under stand-in NVDA.  The player is modelled as a real device: it starts at its first feed and plays
in real time, each fed block after the previous one (never before it was fed).  The tail's clock is the real one
(it is the thing measured).

  A  whine on, keep open: speak, wait until "done" + 0.4 s, speak again (no cancel): heard within LIMIT_MS, and
     within SAME_MS of B (the idle audio still queued is dropped once the last speech has played: Tomi heard the
     141 ms it took before, against 18 ms without the tail)
  B  whine off (the reference): the same
  C  whine on, cancel then speak: heard within LIMIT_MS

The device model plays at the driver's rate and calls onDone when it reaches the point fed (a real player's), so the
driver's "the last speech has played" is the device's, not the feed's.

    python tail_latency.py        TAIL_LATENCY_BREAK=1: the tail's pre-0.7 pacing put back -- A must fail its limit
                                  TAIL_LATENCY_BREAK=queued: the idle audio left queued -- A must fail against B
"""
import queue
import threading
import os
import sys
import time

sys.argv = [sys.argv[0], "blazie"]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
          encoding="utf-8").read().split("time.sleep(2.0)")[0])
time.sleep(2.0)

RATE = float(d._out_rate)     # the device plays at the driver's rate (22050 by default)
LIMIT_MS = 250.0          # render time on a loaded machine, with room
SAME_MS = 40.0            # A against B: the idle lead (IDLE_AHEAD_S + a block, ~90-150 ms) must not be heard
BREAK = os.environ.get("TAIL_LATENCY_BREAK") == "1"
if os.environ.get("TAIL_LATENCY_BREAK") in ("queued", "1"):     # "1": 0.6.0 whole (it also left the queue)
    drv_mod.TAIL_KEEP_QUEUED = True
if BREAK:
    # 0.6.0's pacing: the tail's own audio against the time since the tail started, the speech still playing ignored
    drv_mod.SynthDriver._tail_ahead = lambda self, fed, start: fed - (drv_mod._now() - start)

# the device model: every fed block starts playing at max(when fed, when the previous one ends)
timeline = []             # (play start, samples, chunk index)
_feed = d._player.feed


def feed(data, onDone=None):
    now = time.perf_counter()
    if data:
        n = len(data) // 2
        start = now
        if timeline:
            ps, pn, _ = timeline[-1]
            start = max(now, ps + pn / RATE)
        timeline.append((start, n, len(d._player.chunks)))
    if onDone is None:
        _feed(data, None)
        return
    _feed(data, lambda: None)             # the stand-in's "done" marker, where the done falls in the audio
    end = max((ps + pn / RATE for ps, pn, _ in timeline), default=now)
    callbacks.put((end, onDone))          # heard there, and in feed order, as a device calls them


def _call_in_order():
    while True:
        end, cb = callbacks.get()
        wait = end - time.perf_counter()
        if wait > 0:
            time.sleep(wait)
        try:
            cb()
        except Exception:
            pass
        finally:
            callbacks.task_done()


def idle():
    # a real player's idle() returns when what was fed has played, its callbacks called
    ends = [ps + pn / RATE for ps, pn, _ in timeline if pn]
    if ends and max(ends) > time.perf_counter():
        time.sleep(max(ends) - time.perf_counter())
    callbacks.join()


callbacks = queue.Queue()
threading.Thread(target=_call_in_order, daemon=True).start()


def stop():
    now = time.perf_counter()
    # a stopped device drops what it has not played: later feeds start fresh
    if timeline:
        ps, pn, ci = timeline[-1]
        if ps + pn / RATE > now:
            timeline.append((now, 0, ci))
    d._player.events.append(("stop", 0))


d._player.feed = feed
d._player.stop = stop
d._player.idle = idle


def heard_after(t_speak, k0):
    """Seconds from t_speak until the first audible sample of what was fed at timeline index >= k0 is heard."""
    t_end = time.time() + 10
    while time.time() < t_end:
        for ps, n, ci in list(timeline[k0:]):
            if not n:
                continue
            y = d._player.chunks[ci]
            k = np.nonzero(np.abs(y) > 0.01)[0]
            if len(k):
                return ps + k[0] / RATE - t_speak
        time.sleep(0.005)
    return None


def case(whine, cancel):
    global mark
    d._set_whine(whine)
    d._set_keepOpen(True)
    mark = len(notified)
    n_ev = len(d._player.events)
    d.speak(["Settings dialog, press tab for more."])
    must_complete()
    # "done" comes when the speech has PLAYED (NVDA calls onDone, or idle() returns, then): wait for the modelled
    # device to reach the speech's end -- the tail's "done" marker, or without a tail everything fed
    marks = [s for kind, s in d._player.events[n_ev:] if kind == "done"]
    done_samples = marks[-1] if marks and whine != "off" else sum(n for _, n, _ in timeline)
    total, played_end = 0, None
    for ps, n, ci in list(timeline):
        total += n
        if n and total >= done_samples:
            played_end = ps + (n - (total - done_samples)) / RATE
            break
    while played_end is not None and time.perf_counter() < played_end + 0.4:
        time.sleep(0.01)
    if cancel:
        d.cancel()
    k0 = len(timeline)
    t = time.perf_counter()
    mark = len(notified)
    d.speak(["OK button"])
    dt = heard_after(t, k0)
    must_complete()
    d.cancel()
    time.sleep(0.3)
    return None if dt is None else dt * 1e3


def must_complete():
    """Each utterance's own completion, or the test fails: a wait that runs out would let the channel click off
    and the idle audio this test measures be gone, so a timeout must never pass (it did, under run_tests' load)."""
    if not wait_idle():
        print("FAIL an utterance's completion was not identified: %s" % last_wait_error[0])
        print("tail latency: COMPLETION NOT IDENTIFIED")
        sys.stdout.flush()
        os._exit(2)


failures = 0
d.speak(["Warm up."])
mark = len(notified)
must_complete()
results = {}
for key, name, whine, cancel in (("A", "A whine on, keep open, no cancel", "whine", False),
                                 ("B", "B whine off, no cancel (reference)", "off", False),
                                 ("C", "C whine on, keep open, cancel first", "whine", True)):
    results[key] = (name, case(whine, cancel))
for key in ("A", "B", "C"):
    name, ms = results[key]
    ok = ms is not None and ms <= LIMIT_MS
    note = "limit %.0f ms" % LIMIT_MS
    if key == "A" and ok and results["B"][1] is not None:
        ok = ms <= results["B"][1] + SAME_MS
        note += "; B + %.0f ms = %.0f" % (SAME_MS, results["B"][1] + SAME_MS)
    failures += not ok
    print("%-4s %-40s heard %s after speak() (%s)" % ("ok" if ok else "FAIL", name,
          "never" if ms is None else "%.0f ms" % ms, note))
print("tail latency: %s" % ("all passed" if not failures else "%d FAILED" % failures))
d.terminate()
sys.exit(1 if failures else 0)
