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

try:
    import numpy as np                       # noqa: E402
except ImportError:                          # NVDA 2021-2023 rigs (Python 3.7, 32-bit): array("f") gives the same float32 bytes
    np = None
from array import array as _array           # noqa: E402
from ssi263.native import SSI263C            # noqa: E402
from hosts.blazie import Blazie              # noqa: E402
if "--native" in sys.argv:
    from hosts.native_blazie import NativeBlazie as Blazie   # noqa: E402,F811

NATIVE = "--native" in sys.argv              # the in-process host: EXE is bl.dll (hosts/native_blazie.py)
SPANISH = "--es" in sys.argv                # the Spanish unit (BL2SPA.BNS, cp850) and Spanish lines
AGAINST = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--against=")), None)   # a golden file
ARGS = [a for a in sys.argv[1:] if a not in ("--es", "--native") and not a.startswith("--against=")]
EXE = os.path.abspath(ARGS[0])
OUT = ARGS[1] if len(ARGS) > 1 else None
ENG = os.path.join(REPO, "nvda", "dist", "blazie-build", "synthDrivers", "_ssi263_blazie")
if SPANISH:
    FW, ST = os.path.join(ENG, "BL2SPA.BNS"), os.path.join(ENG, "bl2spa_fresh.state")
else:
    FW, ST = os.path.join(ENG, "BL2ENG.BNS"), os.path.join(ENG, "bl2_2003_warm.state")

chip = SSI263C(out_rate=22050)
log = []
orig = chip.write


def spy(reg, val):
    orig(reg, val)
    log.append("W %.9f %d %02X" % (chip.time, reg, val))


chip.write = spy
t_start = time.perf_counter()
extra = {}
if NATIVE:                                  # the C host writes the chip directly: it reports its writes
    chip.write = orig
    extra["on_write"] = lambda t, reg, val: log.append("W %.9f %d %02X" % (t, reg, val))
u = Blazie(EXE, FW, ST, chip=chip, out_rate=22050, menu=("punct_none", "numbers_toggle"),
           key_start=3000000, key_gap=1500000, board_lowpass_hz=5000.0, status=("inflection_on",), **extra)
audio = hashlib.sha1()


def run(seconds):
    y = u.run(seconds)
    audio.update(np.asarray(y, dtype=np.float32).tobytes() if np is not None else _array("f", y).tobytes())


u.send(b"\x18")
u.send(b"\r\x06")
run(0.3)
u.send(b"\x056V\x0511E\x0516P\x057T")
run(0.1)
tabs = []
if SPANISH:
    u.encoding = "cp850"
    STEPS = (("Hola, ¿cómo estás??", 3.0, None),
             ("Procesamiento de números personalizado, casilla marcada", 0.4, "cancel"),
             ("Botón Aceptar", 1.5, None),
             ("Mañana a las 3,5 horas. ¿Está listo???", 5.0, None),
             ("Léame para Microsoft Windows.", 0.25, "cancel"),
             ("Sí, funciona.", 2.5, "cancel_idle"),
             ("Editar, varias líneas, en blanco", 2.5, None))
else:
    STEPS = (("Hello, how are you??", 3.0, None),
             ("Custom number processing check box checked", 0.4, "cancel"),
             ("OK button", 1.5, None),
             ("$12.50 and 1,234,567. Is it ready???", 5.0, None),
             ("Readme for Microsoft Windows.", 0.25, "cancel"),
             ("Yes, it works.", 2.5, "cancel_idle"),
             ("Edit, multi line, blank", 2.5, None))
# the host's whine layer too (a C port must reproduce it): one last line with the odd-volume whine on
STEPS += ((STEPS[2][0], 2.0, "whine"),)
for step, (text, secs, cut) in enumerate(STEPS):
    log.append("SAY %d %r" % (step, text))
    t0 = time.perf_counter()
    if cut == "whine":
        u.whine, cut = "whine", None
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
if AGAINST:
    gold = open(AGAINST, encoding="utf-8").read().splitlines()
    diff = next((k for k, (x, y) in enumerate(zip(gold, sig)) if x != y), None)
    if diff is None and len(gold) == len(sig):
        print("matches %s (%d lines)" % (os.path.basename(AGAINST), len(sig)))
        sys.exit(0)
    k = diff if diff is not None else min(len(gold), len(sig))
    print("DIFFERS from %s at line %d:\n  golden: %s\n  now:    %s" % (
        os.path.basename(AGAINST), k, gold[k] if k < len(gold) else "-", sig[k] if k < len(sig) else "-"))
    sys.exit(1)
