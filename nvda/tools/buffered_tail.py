"""NVDA 2021-2023's buffered player (WinMM, buffered=True) keeps fed audio until it holds more
than 300 ms, unless a feed carries onDone; only idle() or such a feed lets the rest out.  The
drivers call idle() only when nothing is queued, so with the next utterance already waiting
the last blocks sat in that buffer and came out at the head of the next one (a tester,
0.5.0: "the last audio chunk ... is cut off, then is joined to the next utterance").
This speaks two queued utterances through a player with those rules and checks that
everything of the first is let out before the second is given to the unit.

    python nvda/tools/buffered_tail.py speakout|blazie|accent
"""
import os
import sys
import time

WHICH = sys.argv[1]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
          encoding="utf-8").read().split("time.sleep(2.0)")[0])
time.sleep(2.0)

MIN_BUFFER = int(44100 * 2 * 0.3)          # bytes: nvwave.WinmmWavePlayer.MIN_BUFFER_MS = 300


class Buffered:
    """Just the buffering rules of NVDA 2023.3's WinmmWavePlayer.feed and idle."""

    def __init__(self):
        self.held, self.fed, self.out = b"", 0, 0

    def feed(self, data, onDone=None):
        self.fed += len(data)
        self.held += data
        if onDone or len(self.held) > MIN_BUFFER:
            self.out += len(self.held)
            self.held = b""
            if onDone:
                onDone()

    def idle(self):
        self.out += len(self.held)
        self.held = b""

    def stop(self):
        self.held = b""

    def pause(self, s):
        pass

    def close(self):
        pass


while getattr(d, "_unit", None) is None and getattr(d, "_box", None) is None and getattr(d, "_card", None) is None:
    time.sleep(0.05)
unit = next(u for u in (getattr(d, n, None) for n in ("_unit", "_box", "_card")) if u is not None)
d._player = player = Buffered()
held_at_say = []
orig_say = unit.say


def say(*a, **k):
    held_at_say.append(player.fed - player.out)
    return orig_say(*a, **k)


unit.say = say
mark = len(notified)
d.speak(["The first sentence ends here."])
d.speak(["And the second one follows."])
t = time.time()
while sum(1 for n in notified[mark:] if n[0] == "done") < 2 and time.time() - t < 30:
    time.sleep(0.05)
print("audio still held in the player when each utterance was given to the unit:",
      ", ".join("%.0f ms" % (b / 2 / 44.1) for b in held_at_say))
ok = len(held_at_say) >= 2 and held_at_say[1] == 0
print("PASS" if ok else "FAIL: the first utterance's tail waits for the second")
d.terminate()
sys.exit(0 if ok else 1)
