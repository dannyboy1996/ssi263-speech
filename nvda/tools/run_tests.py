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
# its must-fail control: 0.5.0's cancel put back on four seeds in parallel; it passes when a seed catches it (one seed
# alone missed it under this suite's load about 1 run in 6)
CHECKS.append(check("complete_fuzz CONTROL (0.5.0 cancel, must be caught)", [PY, "complete_fuzz_control.py", "150"]))
# the control's own guard: fake children that crash, mis-summarise, under-cover or hang must never count as a catch
CHECKS.append(check("complete_fuzz CONTROL guard", [PY, "complete_fuzz_control_guard.py"]))
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
    # CONTRACT.md 3: the legacy path's own exceptions (src/csrc/cpu/test_z180_legacy.c)
    CHECKS.append(check("z180emu legacy path: its exceptions", [os.path.join(LIB, "test_z180_legacy.exe")]))
# the Blazie emulator app (src/apps/blazie): its chord logic, and the unit headless (boot greeting heard, a chord
# answered against the no-chord control, faster than real time)
EMU = os.path.join(os.path.dirname(HERE), "dist", "blazie-emu")
if os.path.isfile(os.path.join(EMU, "test_emu_unit.exe")):
    CHECKS.append(check("Blazie emulator: chords", [os.path.join(EMU, "test_chords.exe")]))
    CHECKS.append(check("Blazie emulator: the unit, headless", [os.path.join(EMU, "test_emu_unit.exe"), "bl",
                        os.path.join(ENG, "BL2ENG.BNS"), os.path.join(ENG, "bl2_2003_warm.state")]))
    CHECKS.append(check("Blazie emulator: the Spanish unit, headless", [os.path.join(EMU, "test_emu_unit.exe"), "bl",
                        os.path.join(ENG, "BL2SPA.BNS"), os.path.join(ENG, "bl2spa_fresh.state")]))
    # the Type 'n Speak from cold (firmware/blazie/tns/, when the builder has it): its reset to defaults, its
    # question heard, y answered, its memory kept
    TNS_DIR = os.path.join(os.path.dirname(os.path.dirname(HERE)), "firmware", "blazie", "tns")
    for name in ("TNSENG.TNS", "TNSSPA.TNS"):
        if os.path.isfile(os.path.join(TNS_DIR, name)):
            CHECKS.append(check("Blazie emulator: Type 'n Speak %s, headless" % name[3:6],
                                [os.path.join(EMU, "test_emu_unit.exe"), "tns", os.path.join(TNS_DIR, name), "-"]))
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
    # CONTRACT.md's clauses (src/csrc/cpu/test_z180_contract.c; its must-fail controls: cpu/contract_controls.py)
    CHECKS.append(check("MAME Z180 core: CPU contract tests", [os.path.join(LIB, "test_z180_contract.exe")]))
    CHECKS.append(check("MAME Z180 core: white-box tests", [os.path.join(LIB, "test_z180_whitebox.exe")]))
# the Android engine's native part (src/platforms/android): built for the desktop, it speaks as bl.dll does, byte
# for byte; its control (the request's rate dropped) must differ
ANDROID_TEST = os.path.join(os.path.dirname(os.path.dirname(HERE)), "src", "platforms", "android", "test",
                            "test_android_native.py")
if os.path.isfile(ANDROID_TEST):
    CHECKS.append(check("Android engine: native part as bl.dll", [PY, ANDROID_TEST]))
    CHECKS.append(check("Android engine CONTROL (rate dropped, must fail)", [PY, ANDROID_TEST],
                        env={"SSI263_ANDROID_TEST_BREAK": "1"}, expect_fail=True))
# MAME's 8085 core (the Accent SA's, src/csrc/cpu/i8085_mame.cpp; not yet in a board): CONTRACT.md's clauses
# (src/csrc/cpu/test_i8085_contract.c; its must-fail controls: cpu/i8085_controls.py)
if os.path.isfile(os.path.join(LIB, "test_i8085_contract.exe")):
    CHECKS.append(check("MAME 8085 core: CPU contract tests", [os.path.join(LIB, "test_i8085_contract.exe")]))
# the Python wheel (python/): built from this tree's libraries, installed into a fresh venv, the chip audible and
# deterministic and the Braille Lite byte for byte as bl.dll; its control (rate 70 asked for) must differ
WHEEL_TEST = os.path.join(os.path.dirname(os.path.dirname(HERE)), "python", "test_wheel.py")
if os.path.isfile(os.path.join(LIB, "x64", "bl.dll")):
    CHECKS.append(check("Python wheel (64-bit)", [PY, WHEEL_TEST, "--build", ENG]))
    CHECKS.append(check("Python wheel CONTROL (rate 70, must fail)", [PY, WHEEL_TEST, "--build", ENG],
                        env={"WHEEL_TEST_BREAK": "1"}, expect_fail=True))
CHECKS.append(check("stacked_q_symbols", [PY, "-S", "stacked_q_symbols.py", NVDA]))
# the Braille Lite driver keeps the unit's channel open after speech (hiss/whine until the firmware clicks off),
# at no cost to response time; the control runs it with keep open off and must fail
CHECKS.append(check("keep the channel open", [PY, "keep_open_test.py"]))
CHECKS.append(check("keep the channel open CONTROL (off, must fail)", [PY, "keep_open_test.py"],
                    env={"KEEP_OPEN_BREAK": "1"}, expect_fail=True))
# ... and with every fed sample silent, each audio check must reject it (Astra, Reply 95: missing sound passed)
CHECKS.append(check("keep the channel open CONTROL (mute, every audio check fails)", [PY, "keep_open_test.py"],
                    env={"KEEP_OPEN_BREAK": "mute"}))
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
        # a control must fail as a test fails, not by crashing: a traceback (a build that broke, a race on a shared
        # file) proves nothing about the bug it guards
        crashed = "Traceback (most recent call last)" in out
        passed, why = ((not passed and "TIMEOUT" not in why and not crashed),
                       "control CRASHED instead of failing" if crashed else ("" if not passed else "control did NOT fail"))
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
