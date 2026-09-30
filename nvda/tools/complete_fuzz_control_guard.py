"""The guard of complete_fuzz_control.py, tested with fake children (after Astra, Reply 95, whose fixtures made the
old wrapper pass on a crash and on an inconsistent summary).  Each case gives the wrapper's seeds a behaviour; the
wrapper must CATCH only when a seed caught the bug properly and every other child finished properly.

    python complete_fuzz_control_guard.py
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

FAKE = r'''
import os, sys, time
seed = sys.argv[2]
kind = dict(p.split(":") for p in os.environ["FAKE_SPEC"].split(",")).get(seed, "clean")
if kind == "caught":
    print("step 9: something ended with 3 of its 9 phonemes"); print("55 finished utterances checked, 1 incomplete"); sys.exit(1)
if kind == "clean":
    print("55 finished utterances checked, 0 incomplete"); sys.exit(0)
if kind == "crash":
    print("RuntimeError: incomplete"); sys.exit(1)
if kind == "inconsistent":
    print("10 finished utterances checked, 0 incomplete"); sys.exit(1)
if kind == "exit0_with_bad":
    print("55 finished utterances checked, 2 incomplete"); sys.exit(0)
if kind == "low_coverage":
    print("5 finished utterances checked, 1 incomplete"); sys.exit(1)
if kind == "silent42":
    sys.exit(42)
if kind == "hang":
    time.sleep(60)
'''

CASES = [   # (name, spec for the wrapper's seeds -- unlisted ones are clean --, the wrapper must catch?)
    ("one proper catch", "1:caught,5:clean,8:clean,10:clean", True),
    ("all clean", "1:clean,5:clean,8:clean,10:clean", False),
    ("a crash that says 'incomplete'", "1:crash,5:clean,8:clean,10:clean", False),
    ("exit 1 with 0 incomplete", "1:inconsistent,5:clean,8:clean,10:clean", False),
    ("exit 0 with 2 incomplete", "1:exit0_with_bad,5:clean,8:clean,10:clean", False),
    ("too little coverage", "1:low_coverage,5:clean,8:clean,10:clean", False),
    ("a catch beside a silent exit 42", "1:caught,5:silent42,8:clean,10:clean", False),
    ("a catch beside a hung child", "1:caught,5:hang,8:clean,10:clean", False),
]

bad = 0
with tempfile.TemporaryDirectory() as tmp:
    fake = os.path.join(tmp, "fake_child.py")
    open(fake, "w").write(FAKE)
    for name, spec, want in CASES:
        env = dict(os.environ, COMPLETE_FUZZ_CONTROL_CHILD=fake, FAKE_SPEC=spec, COMPLETE_FUZZ_CONTROL_DEADLINE="5",
                   PYTHON_COLORS="0")
        p = subprocess.run([sys.executable, os.path.join(HERE, "complete_fuzz_control.py"), "10"], env=env,
                           capture_output=True, text=True, timeout=60)
        got = p.returncode == 0 and "CAUGHT (" in p.stdout and "NOT CAUGHT" not in p.stdout
        ok = got == want
        bad += not ok
        print("%-4s %-34s -> %s" % ("ok" if ok else "FAIL", name, "caught" if got else "not caught (exit %d)" %
                                    p.returncode))
print("control guard: %s" % ("PASS" if not bad else "%d FAILED" % bad))
sys.exit(1 if bad else 0)
