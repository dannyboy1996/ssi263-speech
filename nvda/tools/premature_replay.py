"""The premature completion, replayed deterministically (2026-09-29; investigation: complete_fuzz seed 4 caught with
SSI263_BLAZIE_RECORD; Astra, Reply 100).

The bug: after a cancel whose owed ^F echoed during its first cut, the host's count started the next utterance one
echo short, and 0.6.0's busy() ended "One, two, three, four." at its comma (W UH1 N).  golden/premature_history_seed4
.jsonl is the live session that showed it, on the z180emu core (0.6); on the MAME Z180 (0.7) its timing no longer
makes the miscount -- 0.6.0's busy() speaks it whole there, while bl_live_legacy.exe still cuts it -- so it is kept
as the 0.6 record only.  golden/premature_history_mame.jsonl is the same miscount on MAME (premature_record.py: the
fuzz's move, "One, two, three, four." cancelled 1.750 s in, as its last echo is due, then said again; found by a
sweep of cancel times, the window 1.745-1.755 s on both hosts).  Each case replays that history, then acts as a driver
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
    PREMATURE_REPLAY_NEVER=clean          # control: only the clean runs never done -- the latency comparison and the
                                          # two-line reference must be rejected, not used (Astra, Reply 101)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bl_replay                                    # noqa: E402
from write_spy import watch_writes                  # noqa: E402

HISTORY = os.path.join(HERE, "golden", "premature_history_mame.jsonl")    # the 0.7 core's (see above)
ENG = os.path.join(os.path.dirname(HERE), "dist", "blazie-build", "synthDrivers", "_ssi263_blazie")
OLD = os.environ.get("PREMATURE_REPLAY_OLD") == "1"
NEVER = os.environ.get("PREMATURE_REPLAY_NEVER") == "1"
NEVER_CLEAN = os.environ.get("PREMATURE_REPLAY_NEVER") == "clean"
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


def speak_until_done(unit, lines, first, block, said, never=False):
    """say(lines), then blocks until busy() is false.  (phonemes by then, phonemes in the second after, done?, the
    chip time at done -- kept as it was, not derived from the post-roll, which run() quantises)"""
    unit.say(lines)
    del said[:]
    t0, done, t_done = unit.chip.time, False, None
    if NEVER or never:
        unit.busy = lambda *a, **k: True
    elif OLD:
        unit.busy = old_busy(unit)
    if first:
        unit.run(first)
    while unit.chip.time - t0 < LIMIT_S:
        unit.run(block)
        if not unit.busy():
            done, t_done = True, unit.chip.time
            break
    at_done = list(said)
    unit.run(1.0)
    return at_done, said[len(at_done):], done, t_done


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
    at_done, after, done, _t = speak_until_done(unit, lines, first, block, said)
    unit.close()
    return at_done, after, done


def clean(host, lines):
    """The same lines spoken on a fresh unit (no cancel before them): (phonemes at done, or None if it never
    completed or spoke on after "done"; "done"'s chip time)."""
    unit = bl_replay.make_unit(history()[:2], host)
    said = spoken(unit)
    unit.run(0.5)
    at_done, after, done, t_done = speak_until_done(unit, lines, 0.0, 0.03, said, never=NEVER_CLEAN)
    unit.close()
    if not done or after:
        return None, None
    return at_done, t_done


bad = 0


def report(ok, what):
    global bad
    bad += not ok
    print("%-5s %s" % ("ok" if ok else "FAIL", what))


for host in ("native", "pipe"):
    ref_two = None
    if not (OLD or NEVER):                            # the clean-latency comparison, only between completed runs
        new_t = clean(host, ["Hello there, how are you today? Yes, it works."])[1]
        OLD = True
        old_t = clean(host, ["Hello there, how are you today? Yes, it works."])[1]
        OLD = False
        if new_t is None or old_t is None:
            report(False, "%-6s a clean utterance NEVER DONE (or spoke on after done): no latency comparison" % host)
        else:
            report(abs(new_t - old_t) < 1e-9, "%-6s a clean utterance done at %.4f s, 0.6.0's busy() at %.4f s" % (
                host, new_t, old_t))
        ref_two = clean(host, TWO_LINES)[0]
        if ref_two is None:
            report(False, "%-6s the two-line reference NEVER DONE (or spoke on after done): case skipped" % host)
    cases = [("30 ms blocks", ONE_LINE, 0.0, 0.03, WANT_ONE)]
    if not OLD:
        cases += [("15.5 ms, then 30 ms", ONE_LINE, 0.0155, 0.03, WANT_ONE),
                  ("0.5 ms polling", ONE_LINE, 0.0, 0.0005, WANT_ONE)]
        if ref_two is not None:
            cases.append(("two lines, 30 ms", TWO_LINES, 0.0, 0.03, ref_two))
    if NEVER_CLEAN:
        cases = []                                    # the clean-only control judges the clean runs alone
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
