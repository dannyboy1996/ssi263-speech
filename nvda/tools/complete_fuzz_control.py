"""complete_fuzz's must-fail control, made robust: 0.5.0's cancel (COMPLETE_FUZZ_050=1) put back, on several seeds at
once, in parallel.  The bug shows only when a cancel lands mid-line and a late ^F echo meets the next line: one seed's
random steps reach that often, but under the full run_tests load one seed missed it in about 1 run in 6 (the control
then passed when it had to fail).  Several seeds make a miss rarer; how much rarer is empirical (Astra, Reply 95: the
seeds share one machine's load, so their misses need not be independent).

Every child must finish properly (after Astra, Reply 95): its last line exactly "<n> finished utterances checked,
<m> incomplete[, <l> late]", at least MIN_CHECKED utterances checked, and its exit status consistent with m + l (1
when m + l > 0, 0 when both are 0); only m (short at the utterance's own completion) counts as a catch.  A child that
could not identify a completion (Reply 104: exit 2, "COMPLETION NOT IDENTIFIED") has no summary: broken, not a catch.  A crash, a malformed or inconsistent summary, too little coverage or a timeout makes the whole control
fail.  Each child's full output is kept in out/complete_fuzz_control/seed<N>.txt.

Passes (exit 0) when at least one seed catches the bug and every child finished properly; otherwise exits 1.

Caveat, kept visible: an incomplete utterance is the symptom 0.5.0's cancel causes, but the open premature-completion
bug (a line ending after a few phonemes, rarely, on the fixed driver too) shows the same symptom.  Astra's self-test
once saw two such utterances on seed 7 with the fix in place.  So "caught" means an incomplete utterance appeared, not
proof it came from the restored cancel; the kept outputs are there to tell them apart when it matters.

    python complete_fuzz_control.py [steps]
    COMPLETE_FUZZ_CONTROL_SELFTEST=1: the fix left in place -- must report NOT CAUGHT (and keeps the outputs)
"""
import os
import re
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
STEPS = sys.argv[1] if len(sys.argv) > 1 else "150"
SEEDS = (1, 5, 8, 10, 14, 16)   # the 6 of seeds 1-16 that caught it in 2 or 3 of 3 rounds (2026-09-30)
DEADLINE_S = float(os.environ.get("COMPLETE_FUZZ_CONTROL_DEADLINE", "240"))   # per child, from its launch
# the guard's own tests (complete_fuzz_control_guard.py) substitute a fake child here
CHILD = os.environ.get("COMPLETE_FUZZ_CONTROL_CHILD", os.path.join(HERE, "complete_fuzz.py"))
MIN_CHECKED = 40
# "..., <l> late" since Reply 104 (phonemes loaded after the utterance's own done, counted apart from incomplete)
SUMMARY = re.compile(r"^(\d+) finished utterances checked, (\d+) incomplete(?:, (\d+) late)?$")
OUT = os.path.join(HERE, "out", "complete_fuzz_control")

env = dict(os.environ, SIM_SPEED="10", COMPLETE_FUZZ_050="1", PYTHON_COLORS="0")
if os.environ.get("COMPLETE_FUZZ_CONTROL_SELFTEST") == "1":
    env.pop("COMPLETE_FUZZ_050")      # the wrapper's own check: with the fix in place it must report NOT CAUGHT


def kill_tree(p):
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    else:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except Exception:
            p.kill()


os.makedirs(OUT, exist_ok=True)
extra = {} if os.name == "nt" else {"start_new_session": True}
procs = []
for s in SEEDS:
    p = subprocess.Popen([sys.executable, CHILD, STEPS, str(s)], cwd=HERE, env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **extra)
    procs.append((s, p, time.monotonic() + DEADLINE_S))
caught, broken = [], []
for s, p, deadline in procs:
    timed_out = False
    try:
        out, _ = p.communicate(timeout=max(0.1, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        timed_out = True
        kill_tree(p)
        out, _ = p.communicate()              # drain and reap
    path = os.path.join(OUT, "seed%d.txt" % s)
    with open(path, "w", encoding="utf-8") as f:
        f.write(out or "")
    lines = [ln for ln in (out or "").splitlines() if ln.strip()]
    m = SUMMARY.match(lines[-1].strip()) if lines else None
    if timed_out:
        broken.append((s, "timed out after %.0f s" % DEADLINE_S))
    elif not m:
        broken.append((s, "no proper summary (exit %d): %r" % (p.returncode, lines[-1][:80] if lines else "")))
    else:
        checked, bad, late = int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)
        if checked < MIN_CHECKED:
            broken.append((s, "only %d utterances checked" % checked))
        elif (bad + late > 0 and p.returncode != 1) or (bad + late == 0 and p.returncode != 0):
            broken.append((s, "exit %d does not match %d incomplete, %d late" % (p.returncode, bad, late)))
        elif bad > 0:
            caught.append(s)
    print("seed %d: exit %s, %s  (full output: %s)" % (s, "timeout" if timed_out else p.returncode,
                                                      lines[-1][:80] if lines else "(none)", path))
ok = bool(caught) and not broken
print("0.5.0's cancel %s (seeds catching it: %s%s)" % ("CAUGHT" if ok else "NOT CAUGHT", caught or "none",
                                                       "; broken runs: %s" % broken if broken else ""))
if not ok:
    for s, _p, _d in procs:
        tail = open(os.path.join(OUT, "seed%d.txt" % s), encoding="utf-8").read().splitlines()[-12:]
        print("--- seed %d, last lines:\n    %s" % (s, "\n    ".join(tail)))
sys.exit(0 if ok else 1)
