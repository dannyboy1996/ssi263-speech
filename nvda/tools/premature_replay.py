"""The premature completion, replayed deterministically (2026-09-29; investigation: complete_fuzz seed 4 caught with
SSI263_BLAZIE_RECORD).

golden/premature_history_seed4.jsonl is that live session up to and including the say() that was cut: after a cancel
whose owed ^F echoed during its first cut, the host's count started the next utterance one echo short, and 0.6.0's
busy() -- done once the count says so and nothing but PA played for 0.1 s -- ended "One, two, three, four." at its
comma (W UH1 N), the rest held until the next utterance.  The test replays that history exactly, then acts as the
driver does: run 30 ms blocks until busy() is false.  Every phoneme of the line must have loaded by then.  On the
in-process host and on the pipe host.

    python premature_replay.py            # exit 1 if the line is cut
    PREMATURE_REPLAY_OLD=1                # control: 0.6.0's busy() (no check that the chip is still playing a
                                          # phoneme the firmware loaded) -- must FAIL with W UH1 N
Also: on a clean utterance, "done" comes at the same chip time as with 0.6.0's busy() (no added latency).
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
WANT = "W UH1 N T U U TH R E F OU OU ER".split()
PHON = ["PA", "E", "E1", "Y", "YI", "AY", "IE", "I", "A", "AI", "EH", "EH1", "AE", "AE1", "AH", "AH1",
        "AW", "O", "OU", "OO", "IU", "IU1", "U", "U1", "UH", "UH1", "UH2", "UH3", "ER", "R", "R1", "R2",
        "L", "L1", "LF", "W", "B", "D", "KV", "P", "T", "K", "HV", "HVC", "HF", "HFC", "HN", "Z",
        "S", "J", "SCH", "V", "F", "THV", "TH", "M", "N", "NG", ":A", ":OH", ":U", ":UH", "E2", "LB"]


def trial(host):
    calls = bl_replay.sessions(HISTORY)[-1]
    init = calls[0][1]
    for k in ("firmware", "state"):
        init[k] = init[k].replace("{ENG}", ENG)
    unit = bl_replay.make_unit(calls, host)
    said, r3 = [], [0x56]

    def write(t, reg, val):
        if reg == 3:
            r3[0] = val
        if reg == 0 and (val & 0x3F) and (r3[0] & 0x0F):     # spoken: not PA, not an amplitude-0 preparation load
            said.append(PHON[val & 0x3F])
    watch_writes(unit, write)
    for c in calls:
        if c[0] == "busy":
            unit.busy(*c[1:])
        elif c[0] == "cancel":
            unit.cancel(*c[1:])
        else:
            bl_replay.apply(unit, c, host)
    del said[:]                                     # from the last say() on
    if OLD:
        def busy_old(quiet=0.1, patience=3.0):
            now = unit.chip.time
            if now - unit.last_speech < quiet:
                return True
            return unit.owed() > 0 and now - max(unit.say_time, unit.last_speech) < patience
        unit.busy = busy_old
    for _ in range(400):                            # at most 12 s
        unit.run(0.03)
        if not unit.busy():
            break
    unit.close()
    return said


def old_busy(unit):
    def busy_old(quiet=0.1, patience=3.0):
        now = unit.chip.time
        if now - unit.last_speech < quiet:
            return True
        return unit.owed() > 0 and now - max(unit.say_time, unit.last_speech) < patience
    return busy_old


def clean_done(host, old):
    """A clean utterance (no cancel before it): the chip time "done" comes at.  The check that the chip is still
    playing must not delay it: the first version did, by the PA the firmware loads after the last echo (121 ms)."""
    calls = bl_replay.sessions(HISTORY)[-1]
    init = calls[0][1]
    for k in ("firmware", "state"):
        init[k] = init[k].replace("{ENG}", ENG)
    unit = bl_replay.make_unit(calls[:2], host)     # init + chip only
    if old:
        unit.busy = old_busy(unit)
    unit.run(0.5)
    unit.say(["Hello there, how are you today? Yes, it works."])
    for _ in range(400):
        unit.run(0.03)
        if not unit.busy():
            break
    t = unit.chip.time
    unit.close()
    return t


bad = 0
for host in ("native", "pipe"):
    new, old = clean_done(host, False), clean_done(host, True)
    ok = abs(new - old) < 1e-9
    bad += not ok
    print("%-6s %-4s a clean utterance done at %.4f s, 0.6.0's busy() at %.4f s" % (host, "ok" if ok else "LATE",
                                                                                   new, old))
for host in ("native", "pipe"):
    got = trial(host)
    ok = got[-len(WANT):] == WANT
    bad += not ok
    print("%-6s %-4s %2d of %d phonemes before done: %s" % (host, "ok" if ok else "CUT", len(got), len(WANT),
                                                          " ".join(got)))
print("premature replay: %s" % ("PASS" if not bad else "%d FAILED" % bad))
sys.exit(1 if bad else 0)
