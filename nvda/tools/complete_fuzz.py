"""Does every utterance finish?  A tester on 0.6.0's release night: after using the Braille Lite a while, speech
"cuts out and waits for the next utterance before finishing what was last said" -- the 0.5.0 bug (an utterance
ending at its first longer pause, the rest held in the unit), or something like it.

Random steps through the real driver (stand-in NVDA): speak a line with commas, sometimes after a cancel (which may
land mid-speech, or after the unit has already finished: the 0.5.0 trigger), sometimes queued, sometimes after
waiting.  Whenever the test waits for an utterance that nothing cancelled, the phonemes the unit loaded for it must
be the whole line (its reference, spoken once cleanly first).  slider_fuzz.py checks only how utterances BEGIN,
with one-word texts, so it could not see this.

    python complete_fuzz.py [steps] [seed]          SIM_SPEED=10 to run ten times faster
    COMPLETE_FUZZ_SYNTH=speakout|accent: the same check on the other add-ons
    COMPLETE_FUZZ_050=1: put 0.5.0's cancel back (every ^F counted as answered) -- the test must catch it
"""
import os
import random
import sys
import time

from write_spy import watch_writes

STEPS = int(sys.argv[1]) if len(sys.argv) > 1 else 200
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1
SYNTH = os.environ.get("COMPLETE_FUZZ_SYNTH", "blazie")      # blazie | speakout | accent
sys.argv = [sys.argv[0], SYNTH]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
          encoding="utf-8").read().split("time.sleep(2.0)")[0])
time.sleep(2.0)

TEXTS = ("Yes, it works.", "Custom number processing, check box, checked", "One, two, three, four.",
         "OK button", "Hello there, how are you today?")
loads = []           # (utterance index, phoneme)
cur = [-1]

while getattr(d, "_unit", None) is None and getattr(d, "_box", None) is None:
    time.sleep(0.05)
unit = getattr(d, "_unit", None) or d._box     # the Braille Lite's unit, the Speak-Out's or Accent's box
names = unit.chip.rom.names if hasattr(unit.chip, "rom") else None
if names is None:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "src"))
    from ssi263 import SSI263
    names = SSI263().rom.names


raw = []             # every write to R0 and R3 with its tag, for the failure report


DUMP = open(os.environ["COMPLETE_FUZZ_DUMP"], "w") if os.environ.get("COMPLETE_FUZZ_DUMP") else None


r3 = [0x7F]          # the chip's R3 as last written (amplitude in bits 3-0)
prep = []            # phonemes loaded at amplitude 0: preparation, not speech (below)


def write(t, reg, val):
    if DUMP:
        DUMP.write("#%s W %.5f R%d=%02X %s\n" % (cur[0], t, reg, val, names.get(val & 0x3F, "?") if reg == 0 else ""))
    if reg in (0, 3):
        raw.append((cur[0], reg, val, round(t, 4)))
    if reg == 3:
        r3[0] = val
    if reg == 0 and (val & 0x3F) and names.get(val & 0x3F) != "PA":
        if not (r3[0] & 0x0F):
            # The Braille Lite's HF handler emits a short E at amplitude 0 before HF, a two-pass preparation that
            # a flag (D603 bit 1) steers; a cancel between the passes leaves the flag set and the next Hello's
            # preparation is skipped (Astra, Reply 74).  Silent by the chip's amplitude: not a spoken phoneme.
            # Kept, and shown in a failure report, so a change in that state stays visible.
            prep.append((cur[0], names.get(val & 0x3F, "?")))
        else:
            loads.append((cur[0], names.get(val & 0x3F, "?")))


watch_writes(unit, write)
pending = []         # said indices given to the driver, not yet sent to the unit


def key(text):
    return "".join(c for c in text.lower() if c.isalnum())[:8]


_osay = unit.say


def say(lines, *a, **kw):
    # label loads by what the unit is actually SENT: a cancelled utterance's discarded flush stays with it
    text = lines if isinstance(lines, str) else " ".join(lines)
    k = next((i for i in pending if key(said[i][0]) == key(text)), None)
    if k is not None:
        cur[0] = k
        del pending[:pending.index(k) + 1]
    note("unit.say %r -> #%s" % (text[:24], k))
    if DUMP:
        DUMP.write("#%s SAY %r at chip %.5f sent %d echo %d owed %d\n" % (k, lines, unit.chip.time, unit.sent_f,
                                                                         unit.echo_f, unit.owed()))
    return _osay(lines, *a, **kw)


trace = []


def note(what):
    if hasattr(unit, "sent_f"):
        trace.append("%s [sent %d echo %d owed %d]" % (what, unit.sent_f, unit.echo_f, unit.owed()))
    else:
        trace.append(what)


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
import threading as _th


def worker_alive():
    return any(t.is_alive() for t in _th.enumerate() if t is not _th.main_thread())


for step in range(STEPS):
    if not worker_alive():
        print("the driver's worker thread died at step %d" % step)
        sys.exit(2)
    how = rng.choice(["cancel", "cancel", "queue", "wait", "wait"])
    note("step %d: %s" % (step, how))
    if how == "cancel":
        if said:
            said[-1][1] = True
        d.cancel()
        del pending[:]       # the driver drops every job not yet sent; an older same-text entry must not take a later line's phonemes
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
            # the symptom is a missing END: a line queued ahead of it (sent early, the unit speaks both in order)
            # may load after this one was sent, so only the tail is this utterance's own
            if got(k)[-len(ref[text]):] != ref[text]:
                bad += 1
                print("step %d: %r ended with %d of its %d phonemes (%s | ref %s)" % (
                    step, text, len(got(k)), len(ref[text]), " ".join(got(k)), " ".join(ref[text])))
                print("    previous #%d %r: %s" % (k - 1, said[k - 1][0], " ".join(got(k - 1))))
                print("    amplitude-0 preparation loads for #%d / #%d: %s / %s" % (
                    k - 1, k, " ".join(p for i, p in prep if i == k - 1) or "-",
                    " ".join(p for i, p in prep if i == k) or "-"))
                rw = [r for r in raw if r[0] in (k - 1, k)]
                print("    raw R0/R3 writes around the start of #%d: %s" % (k, " ".join(
                    "#%d:R%d=%02X@%.4f" % r for r in rw[max(0, next((i for i, r in enumerate(rw) if r[0] == k), len(rw)) - 6):][:14])))
                print("    sent to the unit: %s; driver queue empty: %s; cancel flag: %s" % (
                    k not in pending, d._queue.empty(), d._cancelFlag.is_set()))
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
