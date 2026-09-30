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
  D  the next speech while the last utterance's onDone is still due (an idle block fed behind it): heard within
     FOLLOW_MS of the old speech's end, the old speech whole, both completions in order (Astra, Reply 109)
  E  a cancel inside that wait, then speech: heard within CANCEL_MS of the cancel

The device model plays at the driver's rate, and calls onDone the way NVDA's WASAPI player does: when playback has
passed its point, only from feed() and idle(), on the feeding thread; stop() discards those not yet called.

    python tail_latency.py        TAIL_LATENCY_BREAK=1: the tail's pre-0.7 pacing put back -- A must fail its limit
                                  TAIL_LATENCY_BREAK=queued: the idle audio left queued -- A must fail against B
                                  TAIL_LATENCY_BREAK=plainwait: the last onDone awaited without servicing the
                                  player's callbacks (Reply 109) -- D and E must fail
"""
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
FOLLOW_MS = 60.0          # D: the new speech after the old one's end (a plain wait on the undelivered onDone: ~0.5 s)
CANCEL_MS = 120.0         # E: new speech after a cancel inside that wait (a plain wait: ~0.55 s)
BREAK = os.environ.get("TAIL_LATENCY_BREAK") == "1"
if os.environ.get("TAIL_LATENCY_BREAK") in ("queued", "1"):     # "1": 0.6.0 whole (it also left the queue)
    drv_mod.TAIL_KEEP_QUEUED = True
if BREAK:
    # 0.6.0's pacing: the tail's own audio against the time since the tail started, the speech still playing ignored
    drv_mod.SynthDriver._tail_ahead = lambda self, fed, start: fed - (drv_mod._now() - start)

# the device model -- NVDA's WasapiWavePlayer (nvdaHelper/local/wasapi.cpp, source/nvwave.py; Astra, Reply 109):
# every fed block starts playing at max(when fed, when the previous one ends); an onDone is due when playback passes
# its point, and is called ONLY from feed() (the due ones, before the new one is registered: maybeFireCallback) and
# idle() (all, once played: sync), on the calling thread, in feed order; stop() drops the audio not yet played AND the
# callbacks not yet called.  Nothing else calls them -- a model with its own callback thread hid the Reply 109 stall.
timeline = []             # (play start, samples, chunk index)
pending = []              # [due time, onDone], in feed order
stops = []                # when stop() was called
plock = threading.Lock()
_feed = d._player.feed


def _end():
    """When everything fed so far will have played (the last block's end: blocks play one after the other)."""
    if not timeline:
        return time.perf_counter()
    ps, pn, _ = timeline[-1]
    return ps + pn / RATE


def _fire_due(now):
    due = []
    with plock:
        while pending and pending[0][0] <= now:
            due.append(pending.pop(0)[1])
    for cb in due:
        try:
            cb()
        except Exception:
            pass


def feed(data, onDone=None):
    now = time.perf_counter()
    if data:
        timeline.append((max(now, _end()) if timeline else now, len(data) // 2, len(d._player.chunks)))
    _fire_due(time.perf_counter())
    if onDone is None:
        _feed(data, None)
        return
    _feed(data, lambda: None)             # the stand-in's "done" marker, where the done falls in the audio
    with plock:
        pending.append((_end(), onDone))


def idle():
    end = _end()
    if end > time.perf_counter():
        time.sleep(end - time.perf_counter())
    _fire_due(float("inf"))


def stop():
    now = time.perf_counter()
    stops.append(now)
    with plock:
        pending.clear()                   # nvwave.stop(): _doneCallbacks = {}
    if timeline:
        ps, pn, ci = timeline[-1]
        if ps + pn / RATE > now:
            timeline.append((now, 0, ci))   # a stopped device drops what it has not played: later feeds start fresh
    d._player.events.append(("stop", 0))


d._player.feed = feed
d._player.stop = stop
d._player.idle = idle
if os.environ.get("TAIL_LATENCY_BREAK") == "plainwait":
    drv_mod.TAIL_PLAIN_WAIT = True


def heard_after(t_speak, k0):
    """Seconds from t_speak until the first audible sample of what was fed at timeline index >= k0 is heard."""
    t_end = time.time() + 10
    while time.time() < t_end:
        for ps, n, ci in list(timeline[k0:]):
            if not n or ci >= len(d._player.chunks):      # its audio not stored yet (a feed in progress)
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


def speech_end(n_ev):
    """When the modelled device plays the first "done" marker fed after events[n_ev:] (an utterance's end)."""
    marks = [x for kind, x in d._player.events[n_ev:] if kind == "done"]
    if not marks:
        return None
    total = 0
    for ps, n, ci in list(timeline):
        total += n
        if n and total >= marks[0]:
            return ps + (n - (total - marks[0])) / RATE
    return None


def case_pending(cancel_in_wait):
    """D/E (Astra, Reply 109): the next utterance comes while the last one's onDone is still due -- after at least one
    idle block was fed behind it.  D: the new speech follows the old one's end promptly, the old speech is not cut,
    and the old utterance completes (its index, then its done) before the new one's.  E: a cancel 20 ms into that
    wait, then new speech: heard promptly after the cancel."""
    global mark
    d._set_whine("whine")
    d._set_keepOpen(True)
    n_ev = len(d._player.events)
    mark = len(notified)
    d.speak(["Settings dialog, press tab for more."])
    tok1 = owned[-1]
    t_end, ready = time.time() + 10, False
    while time.time() < t_end:
        marks = [x for kind, x in d._player.events[n_ev:] if kind == "done"]
        with plock:
            due = bool(pending)
        if marks and sum(n for _, n, _ in timeline) > marks[0] and due:
            ready = True
            break
        time.sleep(0.001)
    if not ready:
        return False, "the old utterance never reached a fed idle block with its onDone still due"
    end1 = speech_end(n_ev)
    k0, t = len(timeline), time.perf_counter()
    mark = len(notified)
    d.speak(["OK button"])
    tok2 = owned[-1]
    if cancel_in_wait:
        time.sleep(0.02)
        d.cancel()
        tc, k1 = time.perf_counter(), len(timeline)
        mark = len(notified)
        d.speak(["Yes."])
        dt = heard_after(tc, k1)
        must_complete()
        d.cancel()
        time.sleep(0.3)
        ms = None if dt is None else dt * 1e3
        return ms is not None and ms <= CANCEL_MS, "the new speech heard %s after the cancel (limit %.0f ms)" % (
            "never" if ms is None else "%.0f ms" % ms, CANCEL_MS)
    dt = heard_after(t, k0)
    try:
        wait_done(tok1)
        wait_done(tok2)
        order = True
    except CompletionError as e:
        order = str(e)
    d.cancel()
    time.sleep(0.3)
    if dt is None or end1 is None:
        return False, "no new speech heard, or no end found for the old one"
    after = (t + dt - end1) * 1e3
    cut = [x for x in stops if t <= x < end1 - 0.003]
    ok = after <= FOLLOW_MS and not cut and order is True
    return ok, "heard %.0f ms after the old speech's end (limit %.0f); old speech %s; completions %s" % (
        after, FOLLOW_MS, "cut by a stop %.0f ms before its end" % ((end1 - cut[0]) * 1e3) if cut else "whole",
        "in order" if order is True else order)


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
for name, cancel_in_wait in (("D next speech while the last onDone is due", False),
                             ("E cancel inside that wait", True)):
    ok, detail = case_pending(cancel_in_wait)
    failures += not ok
    print("%-4s %-40s %s" % ("ok" if ok else "FAIL", name, detail))
print("tail latency: %s" % ("all passed" if not failures else "%d FAILED" % failures))
d.terminate()
sys.exit(1 if failures else 0)
