"""Does every utterance finish?  A tester on 0.6.0's release night: after using the Braille Lite a while, speech
"cuts out and waits for the next utterance before finishing what was last said" -- the 0.5.0 bug (an utterance
ending at its first longer pause, the rest held in the unit), or something like it.

Random steps through the real driver (stand-in NVDA): speak a line with commas, sometimes after a cancel (which may
land mid-speech, or after the unit has already finished: the 0.5.0 trigger), sometimes queued, sometimes after
waiting.  Whenever the test waits for an utterance that nothing cancelled, the phonemes the unit loaded for it must
be the whole line (its reference, spoken once cleanly first).  slider_fuzz.py checks only how utterances BEGIN,
with one-word texts, so it could not see this.

Completion is owned (Astra, Reply 104): each utterance ends with an IndexCommand of its own, and it is complete
when THAT index has been reached and a done follows (fake_nvda_driver_test.wait_done).  "Any done since the speak"
took the previous utterance's done while this one was still being sent, and a half-second sleep after it papered
over that, most of the time.  Now there is no sleep: the phonemes are taken AT the matching done, and anything the
unit still loads for the utterance after its done is counted separately ("late").  A reference is valid only if its
own completion was identified, it has phonemes, and nothing came late.  Timeout, a missing index or done, a stale
done or the worker dying is an error (exit 2), never a pass.  Exit 1: an utterance incomplete or late.

    python complete_fuzz.py [steps] [seed]          SIM_SPEED=10 to run ten times faster
    COMPLETE_FUZZ_SYNTH=speakout|accent: the same check on the other add-ons
    COMPLETE_FUZZ_050=1: put 0.5.0's cancel back (every ^F counted as answered) -- the test must catch it
    COMPLETE_FUZZ_WAIT_S=20: how long one completion may take (wall seconds)
Must-fail controls of the completion rule itself; N = the utterance (0-4 are the references) from which it applies:
    COMPLETE_FUZZ_STALE=1          the old rule back (any done since the speak) and a done forged after each speak
                                   -> STALE DONE (=natural: the old rule alone, caught when the fuzz meets one)
    COMPLETE_FUZZ_FORGE_DONE=1     the forged dones alone, the owned rule kept: must still PASS
    COMPLETE_FUZZ_NO_INDEX=N       the driver's index notifications dropped -> MISSING INDEX
    COMPLETE_FUZZ_NO_DONE=N        the driver's done notifications dropped  -> MISSING DONE
    COMPLETE_FUZZ_HANG=N           the worker stuck in the unit's say()     -> TIMEOUT
    COMPLETE_FUZZ_KILL_WORKER=N    the worker told to stop (its queue's None) -> WORKER DIED
    COMPLETE_FUZZ_EARLY_DONE=N     (Braille Lite) the unit reports idle at once, its open channel speaks on after the
                                   done -> LATE (exit 1): the late check's own control
"""
import os
import random
import sys
import threading
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


# the chip's R3 as last written (amplitude in bits 3-0), tracked from the ORDERED writes: the in-process host reports
# its writes in a batch after each C call, so chip.regs inside the callback may already be a later R3 (Astra, Reply
# 76).  Starts from the chip's own R3 at attachment, when no batch is pending.
r3 = [unit.chip.regs[3]]
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
loads_at = {}        # position in notified -> len(loads) when that notification landed (on the driver's thread)
notify_hooks.append(lambda pos: loads_at.__setitem__(pos, len(loads)))


def breaks_from(name):
    """The utterance number from which control `name` applies, or None when it is off."""
    v = os.environ.get(name)
    return int(v) if v not in (None, "") else None


HANG_FROM = breaks_from("COMPLETE_FUZZ_HANG")
_stuck = threading.Event()           # never set: the hang control's worker waits on it for good


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
    if HANG_FROM is not None and k is not None and k >= HANG_FROM:
        _stuck.wait()                # control: the worker stuck inside the unit, alive, never returning
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
    if hasattr(unit, "last_speech"):
        # and 0.5.0's busy(): done once the ^F count says so and nothing but PA played for `quiet` -- the host's
        # later check that the chip is not still playing a phoneme the firmware loaded makes a miscount harmless,
        # so without taking it out too the control could not show a cut-off at all (2026-09-29)
        def busy_050(quiet=0.1, patience=3.0):
            now = unit.chip.time
            if now - unit.last_speech < quiet:
                return True
            return unit.owed() > 0 and now - max(unit.say_time, unit.last_speech) < patience
        unit.busy = busy_050

said = []            # (text, cancelled after?)
spoke_time = []      # when each was spoken (monotonic), for the latency line only
WAIT_S = float(os.environ.get("COMPLETE_FUZZ_WAIT_S", "20"))
STALE = os.environ.get("COMPLETE_FUZZ_STALE") in ("1", "natural")
FORGE = os.environ.get("COMPLETE_FUZZ_STALE") == "1" or os.environ.get("COMPLETE_FUZZ_FORGE_DONE") == "1"
NO_INDEX_FROM = breaks_from("COMPLETE_FUZZ_NO_INDEX")
NO_DONE_FROM = breaks_from("COMPLETE_FUZZ_NO_DONE")
KILL_FROM = breaks_from("COMPLETE_FUZZ_KILL_WORKER")
EARLY_FROM = breaks_from("COMPLETE_FUZZ_EARLY_DONE")
if EARLY_FROM is not None:
    # control of the late check (the Braille Lite only): the unit reports idle at once from utterance N, so the driver
    # says done early, and with the channel kept open (hiss) its idle tail runs the unit on -- the rest of the line is
    # loaded AFTER the done, still tagged with it
    assert SYNTH == "blazie", "COMPLETE_FUZZ_EARLY_DONE needs the Braille Lite's idle tail"
    d._whine = d._want_whine = unit.whine = "hiss"
    _o_busy = unit.busy
    unit.busy = lambda *a, **k: False if cur[0] >= EARLY_FROM else _o_busy(*a, **k)

if STALE:
    # control: the rule before Reply 104 -- the queue empty and any done since the speak counts as this one's
    def find_completion(tok):
        if not d._queue.empty():
            return None
        return next((i for i in range(spoke_at[tok], len(notified)) if notified[i][0] == "done"), None)
if NO_INDEX_FROM is not None:
    _o_index = sdh.synthIndexReached.notify

    def _drop_index(**kw):       # control: this utterance's own index never reported
        i = kw.get("index")
        if isinstance(i, int) and i >= OWNED_BASE + NO_INDEX_FROM:
            return None
        return _o_index(**kw)
    sdh.synthIndexReached.notify = _drop_index
if NO_DONE_FROM is not None:
    _o_done = sdh.synthDoneSpeaking.notify

    def _drop_done(**kw):        # control: no done reported once utterance N has been spoken
        if len(owned) > NO_DONE_FROM:
            return None
        return _o_done(**kw)
    sdh.synthDoneSpeaking.notify = _drop_done


def speak(text):
    k = len(said)
    pending.append(k)
    said.append([text, False])
    spoke_time.append(time.monotonic())
    if KILL_FROM is not None and k == KILL_FROM:
        d._stopped = True        # control: the worker ends (after at most the job it has)
        d._queue.put(None)
    d.speak([text])
    assert owned[-1] == OWNED_BASE + k, "one owned index per utterance"
    if FORGE:
        sdh.synthDoneSpeaking.notify(synth=d)      # a done landing late, as a previous utterance's does
    return k


def got(k):
    return [p for i, p in loads if i == k]


def complete(k):
    """Utterance k's own completion: (its phonemes at that moment, len(loads) then).  Raises CompletionError."""
    q = wait_done(OWNED_BASE + k, WAIT_S)
    n = loads_at[q]
    return [p for i, p in loads[:n] if i == k], n


def late(k, n):
    """What the unit loaded for utterance k after its completion (loads are tagged k until the next is sent)."""
    return [p for i, p in loads[n:] if i == k]


checked = bad = 0


def harness_error(where, e):
    """The run cannot say whether utterances finish: an error with its reason, never a pass or a verdict."""
    print("%s: CompletionError: %s" % (where, e))
    print("    " + "\n    ".join(trace[-10:]))
    print("complete_fuzz: COMPLETION NOT IDENTIFIED at %s (%s); %d utterances checked before it"
          % (where, str(e).split(":")[0], checked), flush=True)
    os._exit(2)          # the worker may be stuck for good: do not wait for it


# the references: each line spoken once, cleanly; valid only when its own completion was identified, it has
# phonemes, and nothing more was loaded for it after that
ref, ref_n = {}, {}
for text in TEXTS:
    k = speak(text)
    try:
        ref[text], ref_n[k] = complete(k)
    except CompletionError as e:
        harness_error("reference #%d %r" % (k, text), e)
    if not ref[text]:
        harness_error("reference #%d %r" % (k, text), "NO PHONEMES at its completion")
for k, text in enumerate(TEXTS):      # each one's tag ended when the next was sent; the last one's ends now
    if late(k, ref_n[k]):
        harness_error("reference #%d %r" % (k, text), "LATE: %d phonemes loaded after its completion: %s" % (
            len(late(k, ref_n[k])), " ".join(late(k, ref_n[k]))))
d._player.pace = True
rng = random.Random(SEED)
done_n = {}          # checked utterance -> len(loads) at its completion
latency = []         # speak to its own done, seconds: information only, not part of any verdict

for step in range(STEPS):
    if worker_dead():
        harness_error("step %d" % step, "WORKER DIED: the driver's worker thread has ended")
    how = rng.choice(["cancel", "cancel", "queue", "wait", "wait"])
    note("step %d: %s" % (step, how))
    if how == "cancel":
        if said:
            said[-1][1] = True
        d.cancel()
        del pending[:]       # the driver drops every job not yet sent; an older same-text entry must not take a later line's phonemes
    elif how == "wait" and said:
        k = len(said) - 1
        try:
            at_done, n = complete(k)
        except CompletionError as e:
            harness_error("step %d, #%d %r" % (step, k, said[k][0]), e)
        latency.append(time.monotonic() - spoke_time[k])
        text, cancelled = said[k]
        if not cancelled:
            checked += 1
            done_n[k] = n
            # the symptom is a missing END: a line queued ahead of it (sent early, the unit speaks both in order)
            # may load after this one was sent, so only the tail is this utterance's own
            if at_done[-len(ref[text]):] != ref[text]:
                bad += 1
                print("step %d: %r ended with %d of its %d phonemes at its completion (%s | ref %s)" % (
                    step, text, len(at_done), len(ref[text]), " ".join(at_done), " ".join(ref[text])))
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
        said[-1][1] = True         # another utterance queued behind it: never waited for, not checked
    speak(rng.choice(TEXTS))
    time.sleep(rng.choice([0.0, 0.05, 0.2, 0.5, 1.0, 2.0]) / SIM_SPEED)
    if (step + 1) % 50 == 0:
        print("step %d of %d" % (step + 1, STEPS), flush=True)
# separately from the phonemes at completion: anything the unit loaded for a checked utterance after its done
late_bad = 0
for k in sorted(done_n):
    after = late(k, done_n[k])
    if after:
        late_bad += 1
        print("#%d %r: LATE, %d phonemes loaded after its completion: %s" % (k, said[k][0], len(after), " ".join(after)))
if latency:
    lat = sorted(latency)
    print("speak to its own done (information, not a verdict): median %.0f ms, max %.0f ms, %d waits" % (
        lat[len(lat) // 2] * 1e3, lat[-1] * 1e3, len(lat)))
print("%d finished utterances checked, %d incomplete, %d late" % (checked, bad, late_bad))
d.terminate()
sys.exit(1 if bad or late_bad else 0)
