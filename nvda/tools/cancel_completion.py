"""Completion after a cancel that timed out (Astra, Reply 74): the host must not call an utterance done while the
unit still has its phonemes queued.

golden/cancel_history_rec24.jsonl is a recorded fuzz session (SSI263_BLAZIE_RECORD) up to and including a cancel
that waited its full 1 s for a line the unit was still preparing -- one ^F echo still owed when it gave up.  The
test replays that history exactly, then acts as the NVDA driver does, adaptively: say "OK button", run 30 ms blocks
until busy() is false, then keep running.  Any phoneme loaded after "done" is premature completion (0.6.0 host:
"button" came after, and the next utterance's attribution began while it was still queued).  Variants: say at once,
a second cancel first, and one second idle first; on the in-process host and on the pipe host.

    python cancel_completion.py            # exit 1 on premature completion or a stalled "done"
    CANCEL_COMPLETION_OLD=1                # control: the 0.6.0 accounting (echo = sent after that cancel), must FAIL
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bl_replay                                    # noqa: E402
from write_spy import watch_writes                  # noqa: E402

HISTORY = os.path.join(HERE, "golden", "cancel_history_rec24.jsonl")
ENG = os.path.join(os.path.dirname(HERE), "dist", "blazie-build", "synthDrivers", "_ssi263_blazie")
OLD = os.environ.get("CANCEL_COMPLETION_OLD") == "1"
# two continuations: "OK button" (as recorded), and two lines with pauses -- a wrong fix can pass the short one by luck
# (its one word gap may fall before an early echo; Claude, section 93)
TEXTS = {"OK button": (["OK button"], 10),
         "two lines": (["OK button. Wait, there is more.", "Hello there, how are you today? Yes, it works, really."], 57)}
STALL = 0.5                                         # s from the last phoneme to "done" allowed


def trial(host, variant, lines):
    calls = bl_replay.sessions(HISTORY)[-1]
    init = calls[0][1]
    for k in ("firmware", "state"):
        init[k] = init[k].replace("{ENG}", ENG)
    unit = bl_replay.make_unit(calls, host)
    loads = []
    watch_writes(unit, lambda t, reg, val: loads.append(t) if reg == 0 and (val & 0x3F) else None)
    for c in calls:
        bl_replay.apply(unit, c, host)
    if OLD:
        unit.echo_f = unit.sent_f                   # what 0.6.0's cancel left after this history
    if variant == "cancel again":
        unit.cancel()
    elif variant == "idle 1 s":
        unit.run(1.0)
    unit.turbo_between_lines = True
    n0 = len(loads)
    unit.say(lines)
    t = 0.0
    while t < 15.0:
        unit.run(0.03)
        t += 0.03
        if not unit.busy():
            break
    t_done = unit.chip.time
    spoken = len(loads) - n0
    last = loads[-1] if len(loads) > n0 else t_done
    unit.run(1.5)
    late = sum(1 for x in loads[n0:] if x > t_done)
    unit.close()
    return spoken, late, t_done - last


bad = 0
n = 0
for host in ("native", "pipe"):
    for label, (lines, phonemes) in TEXTS.items():
        for variant in ("at once", "cancel again", "idle 1 s"):
            spoken, late, stall = trial(host, variant, lines)
            ok = late == 0 and spoken >= phonemes and stall <= STALL
            bad += not ok
            n += 1
            print("%-6s %-9s %-12s %2d phonemes before done, %2d after, done %.2f s after the last: %s"
                  % (host, label, variant, spoken, late, stall, "ok" if ok else "FAILED"))
print("%d of %d cases complete" % (n - bad, n))
sys.exit(1 if bad else 0)
