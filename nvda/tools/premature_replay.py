"""The premature completion, replayed deterministically (2026-09-29; investigation: complete_fuzz seed 4 caught with
SSI263_BLAZIE_RECORD; Astra, Reply 100).

golden/premature_history_seed4.jsonl is that live session up to and including the say() that was cut: after a cancel
whose owed ^F echoed during its first cut, the host's count started the next utterance one echo short, and 0.6.0's
busy() ended "One, two, three, four." at its comma (W UH1 N).  Each case replays that history, then acts as a driver
does -- run blocks, ask busy() after each -- and requires, on both hosts:
  - an explicit "done" (busy() false) before the limit: running out is a failure, not a pass;
  - the EXACT phonemes of the line(s) loaded by then;
  - nothing more loaded in the second after "done" (no speech left pending).
Cases: the driver's 30 ms blocks; a 15.5 ms first block then 30 ms (Astra's shifted phase: the chip's request raised
at the end of a run() call, not yet given to the firmware, passed for an end); 0.5 ms polling; and a two-line
continuation (checked against the same lines spoken clean).  Also: on a clean utterance "done" comes at the same
chip time as with 0.6.0's busy() (no added latency).

    python premature_replay.py            # exit 1 on any failure
    PREMATURE_REPLAY_OLD=1                # control: 0.6.0's busy() -- the 30 ms case must be CUT at W UH1 N
    PREMATURE_REPLAY_NEVER=1              # control: busy() never false -- every case must report NEVER DONE
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bl_replay                                    # noqa: E402
from write_spy import watch_writes                  # noqa: E402

HISTORY = os.path.join(HERE, "golden", "premature_history_seed4.jsonl")
ENG = os.path.join(os.path.dirname(HERE), "dist", "blazie-build", "synthDrivers", "_ssi263_blazie")
OLD = os.environ.get("PREMATURE_REPLAY_OLD") == "1"
NEVER = os.environ.get("PREMATURE_REPLAY_NEVER") == "1"
ONE_LINE = ["One, two, three, four."]
TWO_LINES = ["One, two, three, four.", "Hello there, how are you today? Yes, it works."]
WANT_ONE = "W UH1 N T U U TH R E F OU OU ER".split()
LIMIT_S = 12.0
PHON = ["PA", "E", "E1", "Y", "YI", "AY", "IE", "I", "A", "AI", "EH", "EH1", "AE", "AE1", "AH", "AH1",
        "AW", "O", "OU", "OO", "IU", "IU1", "U", "U1", "UH", "UH1", "UH2", "UH3", "ER", "R", "R1", "R2",
        "L", "L1", "LF", "W", "B", "D", "KV", "P", "T", "K", "HV", "HVC", "HF", "HFC", "HN", "Z",
        "S", "J", "SCH", "V", "F", "THV", "TH", "M", "N", "NG", ":A", ":OH", ":U", ":UH", "E2", "LB"]


def history():
    calls = bl_replay.sessions(HISTORY)[-1]
    for k in ("firmware", "state"):
        calls[0][1][k] = calls[0][1][k].replace("{ENG}", ENG)
    return calls


def old_busy(unit):
    def busy_old(quiet=0.1, patience=3.0):
        now = unit.chip.time
        if now - unit.last_speech < quiet:
            return True
        return unit.owed() > 0 and now - max(unit.say_time, unit.last_speech) < patience
    return busy_old


def spoken(unit):
    """The phonemes the unit loads from now on (not PA, not an amplitude-0 preparation load)."""
    said, r3 = [], [unit.chip.regs[3]]

    def write(t, reg, val):
        if reg == 3:
            r3[0] = val
        if reg == 0 and (val & 0x3F) and (r3[0] & 0x0F):
            said.append(PHON[val & 0x3F])
    watch_writes(unit, write)
    return said


def speak_until_done(unit, lines, first, block, said):
    """say(lines), then blocks until busy() is false.  (phonemes by then, phonemes in the second after, done?)"""
    unit.say(lines)
    del said[:]
    t0, done = unit.chip.time, False
    if NEVER:
        unit.busy = lambda *a, **k: True
    elif OLD:
        unit.busy = old_busy(unit)
    if first:
        unit.run(first)
    while unit.chip.time - t0 < LIMIT_S:
        unit.run(block)
        if not unit.busy():
            done = True
            break
    at_done = list(said)
    unit.run(1.0)
    return at_done, said[len(at_done):], done


def with_history(host, lines, first, block):
    calls = history()
    unit = bl_replay.make_unit(calls, host)
    said = spoken(unit)
    for c in calls[:-1]:                              # everything up to the cut say(), exactly as recorded
        if c[0] == "busy":
            unit.busy(*c[1:])
        elif c[0] == "cancel":
            unit.cancel(*c[1:])
        else:
            bl_replay.apply(unit, c, host)
    r = speak_until_done(unit, lines, first, block, said)
    unit.close()
    return r


def clean(host, lines):
    """The same lines spoken on a fresh unit (no cancel before them): the reference, and "done"'s chip time."""
    unit = bl_replay.make_unit(history()[:2], host)
    said = spoken(unit)
    unit.run(0.5)
    at_done, after, done = speak_until_done(unit, lines, 0.0, 0.03, said)
    t = unit.chip.time - 1.0
    unit.close()
    return at_done, after, done, t


bad = 0


def report(ok, what):
    global bad
    bad += not ok
    print("%-5s %s" % ("ok" if ok else "FAIL", what))


for host in ("native", "pipe"):
    if not (OLD or NEVER):                            # the clean-latency comparison
        new_t = clean(host, ["Hello there, how are you today? Yes, it works."])[3]
        save = OLD
        OLD = True
        old_t = clean(host, ["Hello there, how are you today? Yes, it works."])[3]
        OLD = save
        report(abs(new_t - old_t) < 1e-9, "%-6s a clean utterance done at %.4f s, 0.6.0's busy() at %.4f s" % (
            host, new_t, old_t))
    ref_two = clean(host, TWO_LINES)[0] if not (OLD or NEVER) else None
    cases = [("30 ms blocks", ONE_LINE, 0.0, 0.03, WANT_ONE)]
    if not OLD:
        cases += [("15.5 ms, then 30 ms", ONE_LINE, 0.0155, 0.03, WANT_ONE),
                  ("0.5 ms polling", ONE_LINE, 0.0, 0.0005, WANT_ONE)]
        if ref_two is not None:
            cases.append(("two lines, 30 ms", TWO_LINES, 0.0, 0.03, ref_two))
    for name, lines, first, block, want in cases:
        at_done, after, done = with_history(host, lines, first, block)
        if not done:
            report(False, "%-6s %-20s NEVER DONE in %.0f s (%d phonemes)" % (host, name, LIMIT_S, len(at_done)))
            continue
        ok = at_done == want and not after
        report(ok, "%-6s %-20s %s %d of %d phonemes at done%s: %s" % (
            host, name, "whole" if ok else "CUT", len(at_done), len(want),
            ", %d MORE after" % len(after) if after else "", " ".join(at_done[:14])))
print("premature replay: %s" % ("PASS" if not bad else "%d FAILED" % bad))
sys.exit(1 if bad else 0)
