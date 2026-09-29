"""Every add-on check at once, across all cores, in under 3 minutes (Tomi: a test run takes 3 minutes at most, 5 is
pushing it).  Each check is its own process with a hard time limit; one line per check, then the total.

    python run_tests.py ["C:\\Program Files\\NVDA"]

A control check must FAIL: complete_fuzz with 0.5.0's cancel put back proves the fuzz can still see a cut-off
utterance (a fix claimed without a test that fails on the bug is how 0.6.0 shipped one).
"""
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

# a check's own output may carry characters the console lacks ("£", U+FFFD): never let the report crash the run
sys.stdout.reconfigure(errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
NVDA = sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files\NVDA"
PY = sys.executable
WIN7 = os.path.join(HERE, "win7")
PY37 = os.path.join(WIN7, "py37", "python.exe")
LIMIT = 240          # seconds, per check


def check(name, argv, env=None, cwd=HERE, ok=None, expect_fail=False):
    return dict(name=name, argv=argv, env=env or {}, cwd=cwd, ok=ok, expect_fail=expect_fail)


CHECKS = []
for w in ("speakout", "blazie", "accent", "accentsa"):
    CHECKS.append(check("driver_sim %s (NVDA 64-bit)" % w, [PY, "-S", "driver_sim.py", w, NVDA, "rt"]))
if os.path.isfile(PY37):
    for app in ("nvda2023app", "nvda2021app"):
        if os.path.isdir(os.path.join(WIN7, app)):
            for w in ("speakout", "blazie", "accent"):
                CHECKS.append(check("driver_sim %s (%s, 32-bit)" % (w, app), [PY37, "run37.py", "../driver_sim.py", w, app, "rt"],
                                    env={"NVDA_APP": app}, cwd=WIN7))
for seed in (1, 2, 3, 4):
    CHECKS.append(check("complete_fuzz seed %d" % seed, [PY, "complete_fuzz.py", "150", str(seed)], env={"SIM_SPEED": "10"}))
for synth in ("speakout", "accent"):
    CHECKS.append(check("complete_fuzz %s" % synth, [PY, "complete_fuzz.py", "150", "1"],
                        env={"SIM_SPEED": "10", "COMPLETE_FUZZ_SYNTH": synth}))
CHECKS.append(check("complete_fuzz CONTROL (0.5.0 cancel, must fail)", [PY, "complete_fuzz.py", "150", "3"],
                    env={"SIM_SPEED": "10", "COMPLETE_FUZZ_050": "1"}, expect_fail=True))
CHECKS.append(check("slider_fuzz", [PY, "slider_fuzz.py", "150", "7"], env={"SIM_SPEED": "10"}))
CHECKS.append(check("cut_test", [PY, "cut_test.py"], env={"CUTS": "0.1", "CUT_REPS": "2"},
                    ok=lambda out: re.search(r"tail bug in 0 of", out) is not None))
CHECKS.append(check("SAPI pipe server", [PY, os.path.join(os.path.dirname(os.path.dirname(HERE)), "sapi", "test_serve.py")]))
# the golden vectors: every SSI-263 write with its time, the serial output and the audio hash of a fixed scenario,
# as today's Braille Lite host and emulator make them (the gate for the native library, 0.7)
BNS = os.path.join(os.path.dirname(HERE), "dist", "blazie-build", "synthDrivers", "_ssi263_blazie", "bns_live.exe")
for lang in ("en", "es"):
    CHECKS.append(check("golden Braille Lite (%s)" % lang, [PY, "bns_equiv.py", BNS,
                        "--against=" + os.path.join(HERE, "golden", "blazie_%s.txt" % lang)] + (["--es"] if lang == "es" else [])))
# 0.7's library board (src/csrc/blazie): the same golden vectors through bl_live.exe, and two units in one process
LIB = os.path.join(os.path.dirname(HERE), "dist", "blazie-lib")
ENG = os.path.dirname(BNS)
if os.path.isfile(os.path.join(LIB, "bl_live.exe")):
    for lang in ("en", "es"):
        CHECKS.append(check("library board golden (%s)" % lang, [PY, "bns_equiv.py", os.path.join(LIB, "bl_live.exe"),
                            "--against=" + os.path.join(HERE, "golden", "blazie_%s.txt" % lang)] + (["--es"] if lang == "es" else [])))
    # the in-process host (bl.dll + hosts/native_blazie.py): the golden vectors bit for bit, 64-bit and 32-bit
    for arch, py in (("x64", PY), ("x86", PY37)):
        dll = os.path.join(LIB, arch, "bl.dll")
        if os.path.isfile(dll) and os.path.isfile(py):
            for lang in ("en", "es"):
                CHECKS.append(check("in-process host %s golden (%s)" % (arch, lang), [py, "bns_equiv.py", dll, "--native",
                                    "--against=" + os.path.join(HERE, "golden", "blazie_%s.txt" % lang)] + (["--es"] if lang == "es" else [])))
    CHECKS.append(check("library board: two units in one process", [os.path.join(LIB, "test_bl_board.exe"),
                        os.path.join(ENG, "BL2ENG.BNS"), os.path.join(ENG, "bl2_2003_warm.state"),
                        os.path.join(ENG, "BL2SPA.BNS"), os.path.join(ENG, "bl2spa_fresh.state")]))
# MAME's Z180 core (src/csrc/cpu/z180_mame.cpp, not yet accepted): the same spoken values as the goldens -- its
# timing legitimately differs (src/csrc/cpu/README.md) -- and two units in one process
MAME_LIVE = os.path.join(LIB, "bl_live_mame.exe")
if os.path.isfile(MAME_LIVE):
    for lang in ("en", "es"):
        CHECKS.append(check("MAME Z180 core: spoken values (%s)" % lang, [PY, "bns_equiv.py", MAME_LIVE, "--values-only",
                            "--against=" + os.path.join(HERE, "golden", "blazie_%s.txt" % lang)] + (["--es"] if lang == "es" else [])))
    CHECKS.append(check("MAME Z180 core: spoken values CONTROL (one value flipped, must fail)",
                        [PY, "bns_equiv.py", MAME_LIVE, "--values-only",
                         "--against=" + os.path.join(HERE, "golden", "blazie_en.txt")],
                        env={"BNS_EQUIV_FLIP": "1"}, expect_fail=True))
    CHECKS.append(check("MAME Z180 core: two units in one process", [os.path.join(LIB, "test_bl_board_mame.exe"),
                        os.path.join(ENG, "BL2ENG.BNS"), os.path.join(ENG, "bl2_2003_warm.state"),
                        os.path.join(ENG, "BL2SPA.BNS"), os.path.join(ENG, "bl2spa_fresh.state")]))
CHECKS.append(check("stacked_q_symbols", [PY, "-S", "stacked_q_symbols.py", NVDA]))
# the chip's defaults for front ends without Python (ssi263_defaults.h): still params.py's and the ROM's, as compiled
# bl_voice (the driver's front end in C, for speech-dispatcher and Android): the real driver's PCM, byte for byte;
# and its control (the C side with packing flipped) must fail
CHECKS.append(check("bl_voice = the NVDA driver, byte for byte", [PY, "voice_equiv.py"]))
CHECKS.append(check("bl_voice CONTROL (packing flipped, must fail)", [PY, "voice_equiv.py"],
                    env={"VOICE_EQUIV_BREAK": "1"}, expect_fail=True))
# bl_voice's text path (currencies, clean-up, lines, encoding) against the driver's, on random texts; and its control
CHECKS.append(check("bl_voice text = the driver's, 5000 random texts", [PY, "voice_text_equiv.py", "5000", "1"]))
CHECKS.append(check("bl_voice text CONTROL (no currencies, must fail)", [PY, "voice_text_equiv.py", "2000", "1"],
                    env={"VOICE_TEXT_BREAK": "1"}, expect_fail=True))
# other currencies than the dollar reach every unit as words (a listener: "£2.63" was read "2.63")
CHECKS.append(check("currency rule", [PY, "currency_test.py", "rules"]))
for w in ("blazie", "speakout", "accent"):
    CHECKS.append(check("currency %s: the unit is sent pounds and pence" % w, [PY, "currency_test.py", w]))
CHECKS.append(check("currency CONTROL (rule off, must fail)", [PY, "currency_test.py", "speakout"],
                    env={"CURRENCY_OFF": "1"}, expect_fail=True))
CHECKS.append(check("cp850 table", [PY, os.path.join(os.path.dirname(os.path.dirname(HERE)), "src", "csrc", "blazie",
                                                     "gen_cp850.py"), "--check"]))
GEN_DEFAULTS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "src", "csrc", "gen_chip_defaults.py")
CHECKS.append(check("chip defaults header", [PY, GEN_DEFAULTS, "--check"]))
# and on Python 3.7 (NVDA 2021-2023): the defaults must not depend on the Python version (3.12 changed float sum())
if os.path.isfile(PY37):
    CHECKS.append(check("chip defaults header (Python 3.7, 32-bit)", [PY37, GEN_DEFAULTS, "--check"]))


def run(c):
    env = dict(os.environ, PYTHON_COLORS="0", **c["env"])
    t0 = time.perf_counter()
    try:
        p = subprocess.run(c["argv"], cwd=c["cwd"], env=env, capture_output=True, text=True, timeout=LIMIT,
                           encoding="utf-8", errors="replace")
        out, code = p.stdout + p.stderr, p.returncode
        passed = (c["ok"](out) if c["ok"] else code == 0)
        why = "" if passed else "exit %d" % code
    except subprocess.TimeoutExpired:
        out, passed, why = "", False, "TIMEOUT after %d s" % LIMIT
    if c["expect_fail"]:
        passed, why = (not passed and "TIMEOUT" not in why), ("" if not passed else "control did NOT fail")
    last = [ln for ln in out.splitlines() if ln.strip() and not ln.startswith("LOG")][-1:] or [""]
    first_bad = [ln.strip() for ln in out.splitlines() if re.search(r"ended with|died|Error|FAILED|began with", ln)][:1]
    if not passed and first_bad:
        why = (why + ": " if why else "") + first_bad[0]
    return c["name"], passed, time.perf_counter() - t0, why, last[0][:90]


t0 = time.perf_counter()
with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
    results = list(pool.map(run, CHECKS))
bad = 0
for name, passed, secs, why, last in results:
    bad += not passed
    print("%-4s %-52s %5.0f s  %s" % ("ok" if passed else "FAIL", name, secs, (why or last)[:200]))
print("%d of %d checks passed in %.0f s on %d cores" % (len(results) - bad, len(results), time.perf_counter() - t0,
                                                        os.cpu_count() or 0))
sys.exit(1 if bad else 0)
