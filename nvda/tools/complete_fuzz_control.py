"""complete_fuzz's must-fail control, made robust: 0.5.0's cancel (COMPLETE_FUZZ_050=1) put back, on several seeds at
once, in parallel.  The bug shows only when a cancel lands mid-line and a late ^F echo meets the next line: one seed's
random steps reach that nearly always, but under the full run_tests load one seed missed it in about 1 run in 6
(the control then passed when it had to fail).  The seeds are independent, so all of them missing together is rare.

Passes (exit 0) when at least one seed catches the bug (its complete_fuzz exits 1) and none crashed or timed out;
otherwise exits 1.  A directed, deterministic trigger was tried first (cancel after the unit finished; cancel a
counted number of phonemes into a line): neither reproduces the bug, whose interleaving (several cancels around a
late echo) the random steps find and a fixed script did not.

    python complete_fuzz_control.py [steps]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STEPS = sys.argv[1] if len(sys.argv) > 1 else "150"
SEEDS = (3, 5, 7, 11)

env = dict(os.environ, SIM_SPEED="10", COMPLETE_FUZZ_050="1", PYTHON_COLORS="0")
if os.environ.get("COMPLETE_FUZZ_CONTROL_SELFTEST") == "1":
    env.pop("COMPLETE_FUZZ_050")      # the wrapper's own check: with the fix in place it must report NOT CAUGHT
procs = [(s, subprocess.Popen([sys.executable, os.path.join(HERE, "complete_fuzz.py"), STEPS, str(s)], cwd=HERE,
                              env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
         for s in SEEDS]
caught, broken = [], []
for s, p in procs:
    try:
        out, _ = p.communicate(timeout=240)
    except subprocess.TimeoutExpired:
        p.kill()
        broken.append((s, "timed out"))
        continue
    last = out.strip().splitlines()[-1] if out.strip() else ""
    if p.returncode == 1 and "incomplete" in last:
        caught.append(s)
    elif p.returncode != 0 or "incomplete" not in last:
        broken.append((s, "exit %d: %s" % (p.returncode, last[:80])))
    print("seed %d: exit %d, %s" % (s, p.returncode, last[:80]))
ok = bool(caught) and not broken
print("0.5.0's cancel %s (seeds catching it: %s%s)" % ("CAUGHT" if ok else "NOT CAUGHT", caught or "none",
                                                       "; broken runs: %s" % broken if broken else ""))
sys.exit(0 if ok else 1)
