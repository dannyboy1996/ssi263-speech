"""Does every utterance finish?  A tester on 0.6.0's release night: after using the Braille Lite a while, speech
"cuts out and waits for the next utterance before finishing what was last said" -- the 0.5.0 bug (an utterance
ending at its first longer pause, the rest held in the unit), or something like it.

Random steps through the real driver (stand-in NVDA): speak a line with commas, sometimes after a cancel (which may
land mid-speech, or after the unit has already finished: the 0.5.0 trigger), sometimes queued, sometimes after
waiting.  Whenever the test waits for an utterance that nothing cancelled, the phonemes the unit loaded for it must
be the whole line (its reference, spoken once cleanly first).  slider_fuzz.py checks only how utterances BEGIN,
with one-word texts, so it could not see this.

    python complete_fuzz.py [steps] [seed]          SIM_SPEED=10 to run ten times faster
    COMPLETE_FUZZ_050=1: put 0.5.0's cancel back (every ^F counted as answered) -- the test must catch it
"""
import os
import random
import sys
import time

STEPS = int(sys.argv[1]) if len(sys.argv) > 1 else 200
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1
sys.argv = [sys.argv[0], "blazie"]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
          encoding="utf-8").read().split("time.sleep(2.0)")[0])
time.sleep(2.0)

TEXTS = ("Yes, it works.", "Custom number processing, check box, checked", "One, two, three, four.",
         "OK button", "Hello there, how are you today?")
loads = []           # (utterance index, phoneme)
cur = [-1]

while d._unit is None:
    time.sleep(0.05)
unit = d._unit
names = unit.chip.rom.names if hasattr(unit.chip, "rom") else None
if names is None:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "src"))
    from ssi263 import SSI263
    names = SSI263().rom.names
ow = unit.chip.write


def write(reg, val):
    ow(reg, val)
    if reg == 0 and (val & 0x3F) and names.get(val & 0x3F) != "PA":
        loads.append((cur[0], names.get(val & 0x3F, "?")))


unit.chip.write = write
pending = []         # said indices given to the driver, not yet sent to the unit


def key(text):
    return "".join(c for c in text.lower() if c.isalnum())[:8]


_osay = unit.say


def say(lines):
    # label loads by what the unit is actually SENT: a cancelled utterance's discarded flush stays with it
    k = next((i for i in pending if key(said[i][0]) == key(" ".join(lines))), None)
    if k is not None:
        cur[0] = k
        del pending[:pending.index(k) + 1]
    note("unit.say %r -> #%s" % (" ".join(lines)[:24], k))
    return _osay(lines)


trace = []


def note(what):
    trace.append("%s [sent %d echo %d owed %d]" % (what, unit.sent_f, unit.echo_f, unit.owed()))


unit.say = say
_oc = unit.cancel


def ucancel(*a, **k):
    note("unit.cancel start")
    r = _oc(*a, **k)
    note("unit.cancel end")
    return r


unit.cancel = ucancel
if os.environ.get("COMPLETE_FUZZ_050"):
    _c = unit.cancel

    def cancel_050(*a, **k):
        r = _c(*a, **k)
        unit.echo_f = unit.sent_f
        return r
    unit.cancel = cancel_050

said = []            # (text, cancelled after?)


def speak(text):
    global mark
    pending.append(len(said))
    said.append([text, False])
    mark = len(notified)
    d.speak([text])


def got(k):
    return [p for i, p in loads if i == k]


ref = {}
for text in TEXTS:
    speak(text)
    wait_idle()
    ref[text] = got(len(said) - 1)
d._player.pace = True
rng = random.Random(SEED)
checked = bad = 0
for step in range(STEPS):
    how = rng.choice(["cancel", "cancel", "queue", "wait", "wait"])
    note("step %d: %s" % (step, how))
    if how == "cancel":
        if said:
            said[-1][1] = True
        d.cancel()
    elif how == "wait" and said:
        k = len(said) - 1
        t0 = time.time()
        while k in pending and time.time() - t0 < 10:       # the worker may still be busy with an earlier cancel
            time.sleep(0.02)
        wait_idle()
        time.sleep(0.5)      # a late done of a cancelled line must not pass for this one's; a held tail stays held
        text, cancelled = said[k]
        if not cancelled:
            checked += 1
            if got(k) != ref[text]:
                bad += 1
                print("step %d: %r ended with %d of its %d phonemes (%s | ref %s)" % (
                    step, text, len(got(k)), len(ref[text]), " ".join(got(k)), " ".join(ref[text])))
                print("    " + "\n    ".join(trace[-14:]))
    if how == "queue" and said:
        said[-1][1] = True         # another utterance queued behind it: its done may not be its own
    speak(rng.choice(TEXTS))
    time.sleep(rng.choice([0.0, 0.05, 0.2, 0.5, 1.0, 2.0]) / SIM_SPEED)
    if (step + 1) % 50 == 0:
        print("step %d of %d" % (step + 1, STEPS), flush=True)
print("%d finished utterances checked, %d incomplete" % (checked, bad))
d.terminate()
sys.exit(1 if bad else 0)
