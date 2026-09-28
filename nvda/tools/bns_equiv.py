"""bns_live.exe speed-ups must not change a single write: a fixed scenario through the Blazie host (boot, lines,
cancels mid-word and when idle, stacked '?', money and numbers), recording every SSI-263 write with its emulated
time, the unit's serial output and a hash of the audio.  Run it with two builds and compare the signatures; it
also reports the wall time of the scenario and of a cancel + next line.

    python bns_equiv.py <bns_live.exe> [signature_out.txt]
    python bns_equiv.py --compare a.txt b.txt

The firmware and state come from the built add-on (nvda/dist/blazie-build), as the add-on runs them.
"""
import hashlib
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [os.path.join(REPO, "src")]

if sys.argv[1:2] == ["--compare"]:
    a, b = (open(p, encoding="utf-8").read().splitlines() for p in sys.argv[2:4])
    diff = next((k for k, (x, y) in enumerate(zip(a, b)) if x != y), None)
    if diff is None and len(a) == len(b):
        print("identical: %d lines" % len(a))
        sys.exit(0)
    k = diff if diff is not None else min(len(a), len(b))
    print("DIFFER at line %d of %d/%d:\n  a: %s\n  b: %s" % (k, len(a), len(b), a[k] if k < len(a) else "-",
                                                          b[k] if k < len(b) else "-"))
    sys.exit(1)

import numpy as np                           # noqa: E402
from ssi263.native import SSI263C            # noqa: E402
from hosts.blazie import Blazie              # noqa: E402

EXE = os.path.abspath(sys.argv[1])
OUT = sys.argv[2] if len(sys.argv) > 2 else None
ENG = os.path.join(REPO, "nvda", "dist", "blazie-build", "synthDrivers", "_ssi263_blazie")
FW, ST = os.path.join(ENG, "BL2ENG.BNS"), os.path.join(ENG, "bl2_2003_warm.state")

chip = SSI263C(out_rate=22050)
log = []
orig = chip.write


def spy(reg, val):
    orig(reg, val)
    log.append("W %.9f %d %02X" % (chip.time, reg, val))


chip.write = spy
t_start = time.perf_counter()
u = Blazie(EXE, FW, ST, chip=chip, out_rate=22050, menu=("punct_none", "numbers_toggle"),
           key_start=3000000, key_gap=1500000, board_lowpass_hz=5000.0, status=("inflection_on",))
audio = hashlib.sha1()


def run(seconds):
    y = u.run(seconds)
    audio.update(np.asarray(y, dtype=np.float32).tobytes())


u.send(b"\x18")
u.send(b"\r\x06")
run(0.3)
u.send(b"\x056V\x0511E\x0516P\x057T")
run(0.1)
tabs = []
for step, (text, secs, cut) in enumerate((
        ("Hello, how are you??", 3.0, None),
        ("Custom number processing check box checked", 0.4, "cancel"),
        ("OK button", 1.5, None),
        ("$12.50 and 1,234,567. Is it ready???", 5.0, None),
        ("Readme for Microsoft Windows.", 0.25, "cancel"),
        ("Yes, it works.", 2.5, "cancel_idle"),
        ("Edit, multi line, blank", 2.5, None))):
    log.append("SAY %d %r" % (step, text))
    t0 = time.perf_counter()
    u.say([text])
    run(secs)
    if cut:
        c0 = time.perf_counter()
        log.append("CANCEL %.6f" % u.cancel())
        tabs.append((time.perf_counter() - c0) * 1e3)
    log.append("STEP %d wall %.1f ms" % (step, (time.perf_counter() - t0) * 1e3))
u.close()
wall = time.perf_counter() - t_start
sig = [ln for ln in log if not ln.startswith("STEP")]
sig.append("TX %s" % " ".join("%02X" % b for b in u.tx))
sig.append("AUDIO %s" % audio.hexdigest())
print("%s: %d writes, audio %s, scenario %.2f s wall, cancels %s ms" % (
    os.path.basename(EXE), sum(1 for ln in log if ln.startswith("W")), audio.hexdigest()[:12], wall,
    " / ".join("%.1f" % t for t in tabs)))
if OUT:
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(sig) + "\n")
