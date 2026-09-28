"""Stepping NVDA's rate slider, at random: each step cancels (keypress), sets the rate and says
the new value; sometimes the value is queued behind the last one instead, sometimes the step
lands before the last value is finished.  Every utterance's phonemes must start with its own
text: anything else at its head is a previous utterance's tail joined to it.

    python nvda/tools/slider_fuzz.py [steps] [seed]

SIM_SPEED=10 runs it ten times faster (player and pauses scaled alike); progress every 25 steps.
"""
import os
import random
import sys
import time

STEPS = int(sys.argv[1]) if len(sys.argv) > 1 else 150
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1
sys.argv = [sys.argv[0], "blazie"]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
          encoding="utf-8").read().split("time.sleep(2.0)")[0])
time.sleep(2.0)

HEAD = {"ninety four": ["N", "AH", "E", "N"], "ninety five": ["N", "AH", "E", "N"],
        "rate": ["R"], "percent": ["P"]}
says = []            # [text, phonemes loaded after this say, events before it]
events = []


def install(unit):
    chip, names = unit.chip, unit.chip.rom.names
    orig_write, orig_say, orig_cancel = chip.write, unit.say, unit.cancel

    def write(reg, val):
        orig_write(reg, val)
        if reg == 0 and (val & 0x3F) and says:
            says[-1][1].append(names.get(val & 0x3F, "?"))

    def say(lines):
        events.append("say: sent %d echo %d stale %d" % (unit.sent_f, unit.echo_f, unit._stale_f))
        says.append([" ".join(lines), [], list(events)])
        del events[:]
        r = orig_say(lines)
        says[-1][2].append("after say: owed %d stale %d" % (unit.owed(), unit._stale_f))
        return r

    def cancel(*a, **k):
        n = len(says[-1][1]) if says else 0
        r = orig_cancel(*a, **k)
        events.append("unit.cancel(%d loads after; sent %d echo %d)" % (len(says[-1][1]) - n if says else 0, unit.sent_f, unit.echo_f))
        return r
    chip.write, unit.say, unit.cancel = write, say, cancel


while d._unit is None:
    time.sleep(0.05)
install(d._unit)
d._player.pace = True
rng = random.Random(SEED)
rate = 94
for step in range(STEPS):
    rate = 95 if rate == 94 else 94
    how = rng.choice(["cancel", "cancel", "cancel", "queue", "wait"])
    events.append("%s/%d" % (how, rate if rate == 94 else 95))
    if how == "cancel":
        d.cancel()
    elif how == "wait":
        wait_idle()          # the last say's own done (mark is taken at each say: if it already came, no wait)
    d._set_rate(rate)
    text = rng.choice(["ninety four" if rate == 94 else "ninety five", "rate", "percent"])
    mark = len(notified)
    d.speak([text])
    pause = rng.choice([0.0, 0.05, 0.1, 0.2, 0.3, 0.5, 0.8, 1.2])
    events.append("sleep %.2f" % pause)
    time.sleep(pause / SIM_SPEED)
    if (step + 1) % 25 == 0:
        print("step %d of %d" % (step + 1, STEPS), flush=True)
wait_idle()
bad = 0
for k, (text, ph, ev) in enumerate(says):
    head = HEAD[text]
    n = min(len(head), len(ph))
    if ph[:n] != head[:n]:
        bad += 1
        print("step %d %r began with %s (previous: %r)" % (k, text, " ".join(ph[:8]), says[k - 1][0] if k else None))
        print("    previous said: %s" % " ".join(says[k - 1][1]) if k else "")
        print("    before the previous: %s" % "; ".join(says[k - 1][2]) if k else "")
        print("    between: %s" % "; ".join(ev))
print("%d utterances given to the unit, %d began with something else" % (len(says), bad))
d.terminate()
sys.exit(1 if bad else 0)
