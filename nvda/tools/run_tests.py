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


def check(name, argv, env=None, cwd=HERE, ok=None, expect_fail=False, fail_marks=(), fail_codes=(1,)):
    """A must-fail control (expect_fail) names the failure it must show (after Astra, Reply 97): an exit code from
    fail_codes AND every regex in fail_marks found in its output (the intended diagnostic, and the completed
    inventory where it runs several cases).  A crash, a silent exit, an empty output or a timeout is not that."""
    assert not expect_fail or fail_marks, "a must-fail control needs fail_marks: %s" % name
    return dict(name=name, argv=argv, env=env or {}, cwd=cwd, ok=ok, expect_fail=expect_fail,
                fail_marks=tuple(fail_marks), fail_codes=tuple(fail_codes))


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
# complete_fuzz's completion is owned (Astra, Reply 104): an utterance is complete at ITS OWN index and the done after
# it, never at the previous utterance's done.  Each control breaks that one way and must end as that error (exit 2),
# neither a pass nor an incomplete count; utterance 7 is past the five references, 0 is the first reference.  stale
# done = the old rule put back, with a done forged after each speak (a previous utterance's, landing late); timeout =
# the worker stuck inside the unit.
for what, env, marks in (
        ("stale done", {"COMPLETE_FUZZ_STALE": "1"},
         [r"^reference #0 'Yes, it works\.': CompletionError: STALE DONE accepted for index 900000: the done at "
          r"notification \d+ follows no owned index$", r"^complete_fuzz: COMPLETION NOT IDENTIFIED at reference #0 "]),
        ("missing index", {"COMPLETE_FUZZ_NO_INDEX": "7"},
         [r"CompletionError: MISSING INDEX: index 9000\d\d never reached, though \d+ done\(s\) came",
          r"^complete_fuzz: COMPLETION NOT IDENTIFIED at step \d+, #\d+ .*\(MISSING INDEX\)"]),
        ("missing done", {"COMPLETE_FUZZ_NO_DONE": "7"},
         [r"CompletionError: MISSING DONE: index 9000\d\d reached, no done after it",
          r"^complete_fuzz: COMPLETION NOT IDENTIFIED at step \d+, #\d+ .*\(MISSING DONE\)"]),
        ("reference: missing done", {"COMPLETE_FUZZ_NO_DONE": "0"},
         [r"^reference #0 'Yes, it works\.': CompletionError: MISSING DONE: index 900000 reached",
          r"^complete_fuzz: COMPLETION NOT IDENTIFIED at reference #0 "]),
        ("timeout", {"COMPLETE_FUZZ_HANG": "7"},
         [r"CompletionError: TIMEOUT: neither index 9000\d\d nor any done in 5\.0 s",
          r"^complete_fuzz: COMPLETION NOT IDENTIFIED at step \d+, #\d+ .*\(TIMEOUT\)"]),
        ("worker death", {"COMPLETE_FUZZ_KILL_WORKER": "7"},
         [r"CompletionError: WORKER DIED: the driver's worker thread",
          r"^complete_fuzz: COMPLETION NOT IDENTIFIED at step \d+.*\(WORKER DIED\)"])):
    CHECKS.append(check("complete_fuzz CONTROL (%s, must error)" % what, [PY, "complete_fuzz.py", "150", "1"],
                        env=dict({"SIM_SPEED": "10", "COMPLETE_FUZZ_WAIT_S": "5"}, **env), expect_fail=True,
                        fail_marks=marks, fail_codes=(2,)))
# the late check, apart from the phonemes at completion: the unit told idle at once from utterance 5, the Braille
# Lite's open channel speaks the rest after the done -- it must be counted late (exit 1), not pass as complete
CHECKS.append(check("complete_fuzz CONTROL (early done, must count late)", [PY, "complete_fuzz.py", "150", "1"],
                    env={"SIM_SPEED": "10", "COMPLETE_FUZZ_EARLY_DONE": "5"}, expect_fail=True,
                    fail_marks=[r"^#\d+ '.*': LATE, \d+ phonemes loaded after its completion: ",
                                r"^\d+ finished utterances checked, \d+ incomplete, [1-9]\d* late$"]))
# ... and the positive half: the same forged dones under the owned rule change nothing
CHECKS.append(check("complete_fuzz, forged dones (owned rule)", [PY, "complete_fuzz.py", "150", "1"],
                    env={"SIM_SPEED": "10", "COMPLETE_FUZZ_FORGE_DONE": "1"}))
# driver_sim.py's own copy of the rule, the same way: the old rule and forged dones put back must fail (every scenario
# then ends at once, with no audio), not pass
CHECKS.append(check("driver_sim CONTROL (stale done, must fail)", [PY, "-S", "driver_sim.py", "speakout", NVDA, "rt"],
                    env={"DRIVER_SIM_STALE": "1"}, expect_fail=True,
                    fail_marks=[r"^sample rate: .*11025 0\.00 s rms 0.*: FAILED$", r"^speakout rt: .*all ok False"]))
# the Accent-mini on MAME's 8086 core (opt-in; not gated before Reply 104 because the old rule failed it on seed 3)
PC86_FUZZ = os.path.join(os.path.dirname(HERE), "dist", "blazie-lib", "x64", "pc86.dll")
if os.path.isfile(PC86_FUZZ):
    CHECKS.append(check("complete_fuzz accent on the MAME 8086 core", [PY, "complete_fuzz.py", "150", "3"],
                        env={"SIM_SPEED": "10", "COMPLETE_FUZZ_SYNTH": "accent", "SSI263_ACCENT_CORE": "mame",
                             "SSI263_PC86_DLL": PC86_FUZZ}))
# the premature completion replayed from a caught live session (complete_fuzz seed 4): the cut line must be spoken
# whole on both hosts; with 0.6.0's busy() put back it must end at its comma again
CHECKS.append(check("premature completion replay", [PY, "premature_replay.py"]))
CHECKS.append(check("premature completion replay CONTROL (0.6.0 busy, must be cut)", [PY, "premature_replay.py"],
                    env={"PREMATURE_REPLAY_OLD": "1"}, expect_fail=True,
                    fail_marks=[r"^FAIL +native +30 ms blocks +CUT 3 of 13 phonemes at done, 8 MORE after: W UH1 N$",
                                r"^FAIL +pipe +30 ms blocks +CUT 3 of 13 phonemes at done, 8 MORE after: W UH1 N$",
                                r"^premature replay: 2 FAILED$"]))
# ... and a busy() that is never false must fail every case, not pass on the phonemes it collected (Astra, Reply 100)
CHECKS.append(check("premature completion replay CONTROL (never done, must fail)", [PY, "premature_replay.py"],
                    env={"PREMATURE_REPLAY_NEVER": "1"}, expect_fail=True,
                    fail_marks=[r"^FAIL +native +30 ms blocks +NEVER DONE", r"^FAIL +pipe +0.5 ms polling +NEVER DONE",
                                r"^premature replay: 6 FAILED$"]))
# ... and clean runs that never complete must not feed the latency comparison or the two-line reference (Astra, 101)
CHECKS.append(check("premature completion replay CONTROL (clean never done, must fail)", [PY, "premature_replay.py"],
                    env={"PREMATURE_REPLAY_NEVER": "clean"}, expect_fail=True,
                    fail_marks=[r"^FAIL +native +a clean utterance NEVER DONE",
                                r"^FAIL +pipe +the two-line reference NEVER DONE", r"^premature replay: 4 FAILED$"]))
# and this runner's own must-fail judgement: silent exits, native crashes, tracebacks, missing marks never count
CHECKS.append(check("run_tests control judgement", [PY, "run_tests_guard.py"]))
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
    # its idle channel against Tomi's unit (src/csrc/blazie/bl_idle.c, src/hosts/blazie_idle.py): the noise's level at
    # every volume, keep open off/until/always, the pop after a click-off and none before, the click, the 10 Hz tick;
    # each control puts one bug back and must fail on exactly the checks that see it
    IDLE = [os.path.join(EMU, "test_idle.exe"), os.path.join(ENG, "BL2ENG.BNS"), os.path.join(ENG, "bl2_2003_warm.state")]
    CHECKS.append(check("Blazie emulator: the idle channel against the unit", IDLE))
    for brk, marks in (
            ("old-level", [r"^FAIL hiss level at volume 6 +-6\d\.\d dB", r"^idle sounds: 3 FAILED$"]),
            ("follow-volume", [r"^FAIL hiss the same at volume 1 and 15 ", r"^ok +hiss level at volume 6 ",
                               r"^idle sounds: 1 FAILED$"]),
            ("until", [r"^FAIL always: open 12 s after speech ", r"^FAIL off: silent after speech ",
                       r"^idle sounds: 2 FAILED$"]),
            ("no-pop", [r"^FAIL pop on reopening after the click-off ", r"^FAIL click at the click-off ",
                        r"^idle sounds: 2 FAILED$"]),
            ("pop-every-line", [r"^FAIL no pop on reopening before the click-off ", r"^idle sounds: 1 FAILED$"]),
            ("no-tick", [r"^FAIL the 10 Hz tick while open, none after +folded at 99\.98 ms: 0\.0 x",
                         r"^idle sounds: 1 FAILED$"]),
            ("tick-after-off", [r"^FAIL the 10 Hz tick while open, none after .*after the click-off -\d",
                                r"^idle sounds: 1 FAILED$"])):
        CHECKS.append(check("Blazie emulator: idle channel CONTROL (%s, must fail)" % brk, IDLE + ["--break=" + brk],
                            expect_fail=True, fail_marks=marks))
    # the Type 'n Speak from cold (firmware/blazie/tns/, when the builder has it): its reset to defaults, its
    # question heard, y answered, its memory kept
    TNS_DIR = os.path.join(os.path.dirname(os.path.dirname(HERE)), "firmware", "blazie", "tns")
    for name in ("TNSENG.TNS", "TNSSPA.TNS"):
        if os.path.isfile(os.path.join(TNS_DIR, name)):
            CHECKS.append(check("Blazie emulator: Type 'n Speak %s, headless" % name[3:6],
                                [os.path.join(EMU, "test_emu_unit.exe"), "tns", os.path.join(TNS_DIR, name), "-"]))
# the emulator's serial port plugged in (src/apps/blazie/test_serial.c; Tomi: WinDisk to the emulated unit): the
# storage handshake WinDisk and PCDISK answer -- XON ENQ out at 19200 8N1, ACK answered with 'C' and NAK not, input
# paced at the baud rate, the directory command out -- on every unit; the Windows COM side (serial_win.c) end to end
# through a named pipe; and the controls, built with the receive path cut: "ACK answered" must FAIL
if os.path.isfile(os.path.join(EMU, "test_serial.exe")):
    SERIAL = os.path.join(EMU, "test_serial.exe")
    for label, fw, state in (("Braille Lite ENG", "BL2ENG.BNS", "bl2_2003_warm.state"),
                             ("Braille Lite SPA", "BL2SPA.BNS", "bl2spa_fresh.state")):
        CHECKS.append(check("Blazie emulator: serial port, %s" % label, [SERIAL, "bl", os.path.join(ENG, fw),
                                                                         os.path.join(ENG, state)]))
    for name in ("TNSENG.TNS", "TNSSPA.TNS"):
        TNS_FW = os.path.join(os.path.dirname(os.path.dirname(HERE)), "firmware", "blazie", "tns", name)
        if os.path.isfile(TNS_FW):
            CHECKS.append(check("Blazie emulator: serial port, Type 'n Speak %s" % name[3:6],
                                [SERIAL, "tns", TNS_FW, "-"]))
    BL_UNIT = [os.path.join(ENG, "BL2ENG.BNS"), os.path.join(ENG, "bl2_2003_warm.state")]
    CHECKS.append(check("Blazie emulator: serial port through Windows (a named pipe)",
                        [os.path.join(EMU, "test_serial_win.exe")] + BL_UNIT))
    CHECKS.append(check("Blazie emulator: serial port, receive path cut (control)",
                        [os.path.join(EMU, "test_serial_cut.exe"), "bl"] + BL_UNIT, expect_fail=True,
                        fail_marks=[r"^ok +storage: XON ENQ out +sent \[11 05\]$",
                                    r"^FAIL ACK answered +ACK -> nothing back$",
                                    r"^ok +NAK not answered \(control\)"]))
    CHECKS.append(check("Blazie emulator: serial port through Windows, receive path cut (control)",
                        [os.path.join(EMU, "test_serial_win_cut.exe")] + BL_UNIT, expect_fail=True,
                        fail_marks=[r"^ok +XON ENQ reach the far end", r"^FAIL ACK answered +ACK -> nothing back$"]))
# head-of-speech latency (Tomi, 2026-09-30: "it feels like a virtual machine"), in emulated time where it can be: the
# emulator's key-to-sound (inside the headless checks above: "key latency", and "quick key response", the opt-in), with
# its control (quick response never switched on must fail); and the Braille Lite driver's open channel, which must not
# queue the speech still playing as idle audio in front of new speech, with its control (0.6.0's pacing put back)
if os.path.isfile(os.path.join(EMU, "test_emu_unit.exe")):
    CHECKS.append(check("Blazie emulator: quick key response CONTROL (never on, must fail)",
                        [os.path.join(EMU, "test_emu_unit.exe"), "bl", os.path.join(ENG, "BL2ENG.BNS"),
                         os.path.join(ENG, "bl2_2003_warm.state")], env={"TEST_EMU_QUICK_BREAK": "1"}, expect_fail=True,
                        fail_marks=[r"^ok +key latency ", r"^FAIL quick key response +key to first sound", r"^FAILED$"]))
CHECKS.append(check("open channel: new speech not queued behind it", [PY, "tail_latency.py"]))
CHECKS.append(check("open channel CONTROL (0.6.0 pacing, must fail)", [PY, "tail_latency.py"],
                    env={"TAIL_LATENCY_BREAK": "1"}, expect_fail=True,
                    fail_marks=[r"^FAIL A whine on, keep open, no cancel ", r"^ok +B whine off",
                                r"^tail latency: [12] FAILED$"]))
# Tomi (0.7 test build): with keep open, a follow-up utterance paused longer -- the idle audio still queued (up to
# IDLE_AHEAD_S and a block) played before it; now dropped once the last speech has played (the player's onDone)
CHECKS.append(check("open channel CONTROL (idle audio left queued, must fail)", [PY, "tail_latency.py"],
                    env={"TAIL_LATENCY_BREAK": "queued"}, expect_fail=True,
                    fail_marks=[r"^FAIL A whine on, keep open, no cancel .*B \+ 40 ms", r"^ok +B whine off",
                                r"^tail latency: 1 FAILED$"]))
# ... and while the last speech's onDone is still due (Astra, Reply 109): NVDA's player calls it only from feed/sync on
# the feeding thread, so the wait must service it (an empty feed) and a cancel must end it; the plain wait put back
# stalled ~0.5 s, even after a cancel -- D and E must fail
CHECKS.append(check("open channel CONTROL (plain wait on the last onDone, must fail)", [PY, "tail_latency.py"],
                    env={"TAIL_LATENCY_BREAK": "plainwait"}, expect_fail=True,
                    fail_marks=[r"^ok +A whine on", r"^FAIL D next speech while the last onDone is due",
                                r"^FAIL E cancel inside that wait", r"^tail latency: 2 FAILED$"]))
# ... and the opt-in, EXPERIMENTAL "run ahead" mode (src/csrc/blazie/run_ahead.h; Astra, Reply 107).  Its contract on a
# synthetic board (test_run_ahead.exe: completion, limits, allocation failures, the 5 ms classification), each control
# putting one 0.7-draft behaviour back; then against today's lockstep through bl.dll (run_ahead_equiv.py): values
# whole-phrase identical (cancel-prefix matches counted apart), every say observed done, no audio or speech after done,
# the replay = the capture, head latency; each guard with its control
RA_TEST = os.path.join(LIB, "test_run_ahead.exe")
if os.path.isfile(RA_TEST):
    CHECKS.append(check("run ahead contract (synthetic board)", [RA_TEST]))
    for arg, marks in (
            ("old-completion", [r"^FAIL last load, no cleanup: final load seen 0, ended while the chip still sounded 1, "
                                r"0 samples played$", r"^FAIL final load never ends: ended as state 5 ",
                                r"^run ahead contract: 4 of 10 FAILED$"]),
            ("old-limit", [r"^FAIL owed, first load at 3\.1 s: ended as state 5 \(limit = 6\)",
                           r"^run ahead contract: 1 of 10 FAILED$"]),
            ("old-alloc", [r"^FAIL allocation fails \(acknowledgements\): ended as state 5 .* after 300 of 300 loads$",
                           r"^FAIL allocation fails \(writes\): ended as state 6 .* after 1024 of 1050 writes$",
                           r"^run ahead contract: 2 of 10 FAILED$"]),
            ("old-reading", [r"^FAIL 5 ms threshold .*first wrong: load 5 answered 0\.0\d\d ms after the request$",
                             r"^FAIL slow service \(50 ms answers in speech\): worst answer off by 49\.9\d\d ms",
                             r"^run ahead contract: 2 of 10 FAILED$"])):
        CHECKS.append(check("run ahead contract CONTROL (%s, must fail)" % arg, [RA_TEST, arg], expect_fail=True,
                            fail_marks=marks))
if os.path.isfile(os.path.join(LIB, "x64", "bl.dll")):
    CHECKS.append(check("run ahead = lockstep, English (values; done; after done; head latency)",
                        [PY, "run_ahead_equiv.py"]))
    CHECKS.append(check("run ahead = lockstep, Spanish (values; done; after done; head latency)",
                        [PY, "run_ahead_equiv.py", "--es"]))
    RA_SUM = r"^run ahead vs lockstep \(English\): \d+ utterances, .*, %d FAILED$"
    for what, env, args, marks in (
            ("a value flipped", {"RUN_AHEAD_EQUIV_FLIP": "1"}, ["--quick"],
             [r"^FAIL case 1 block 1: write values DIFFER at write \d+$",
              r"^FAIL 1\.1 Hello\. +REPLAY DIFFERS FROM CAPTURE", RA_SUM % 2]),
            ("left off, the head", {"RUN_AHEAD_EQUIV_OFF": "1"}, ["--quick"],
             [r"^FAIL 1\.3 Select synthesizer dialog +head 123\.0 -> 123\.0 ms", RA_SUM % 3]),
            ("busy never false", {"RUN_AHEAD_EQUIV_NEVER": "1", "RUN_AHEAD_EQUIV_SAY_LIMIT": "3"}, ["--cases=1"],
             [r"^FAIL 1\.1 Hello\. +NEVER DONE \(run ahead, 3 s\)$", RA_SUM % 3]),
            ("audio silenced", {"RUN_AHEAD_EQUIV_MUTE": "1"}, ["--cases=1"],
             [r"^FAIL 1\.1 Hello\. +NO FIRST AUDIO \(run ahead\)$", RA_SUM % 3]),
            ("last spoken load dropped", {"RUN_AHEAD_EQUIV_DROP_SPOKEN": "1"}, ["--cases=1"],
             [r"^FAIL case 1 block 1: a SPOKEN LOAD \(2B\) in the \d+-write suffix that only the lockstep has$",
              RA_SUM % 2]),
            ("0.7 draft end rule", {"RUN_AHEAD_EQUIV_OLD_SUFFIX": "1"}, ["--selftest"],
             [r"^FAIL the end rule: a missing final spoken load ACCEPTED \(Astra's case\)",
              r"^run ahead vs lockstep \(end rule\): 1 FAILED$"]),
            ("0.7 draft completion", {"RUN_AHEAD_EQUIV_BREAK": "completion"}, ["--cases=1"],
             [r"^FAIL 1\.1 Hello\. +AUDIO AFTER DONE \(run ahead\): \d+ samples", RA_SUM % 2]),
            ("a spoken load flipped where a setting came mid-line", {"RUN_AHEAD_EQUIV_FLIP": "send_mid"},
             ["--cases=9"], [r"^FAIL case 9 block 1: write values DIFFER at write \d+$", RA_SUM % 2])):
        CHECKS.append(check("run ahead CONTROL (%s, must fail)" % what, [PY, "run_ahead_equiv.py"] + args, env=env,
                            expect_fail=True, fail_marks=marks))
    # at 44.1 kHz: a block's rounding sliver once ran a lockstep slice in the middle of the script, and the unit
    # stalled (found by lane 2); its control puts that back
    CHECKS.append(check("run ahead = lockstep, English at 44.1 kHz", [PY, "run_ahead_equiv.py", "--rate=44100"]))
    CHECKS.append(check("run ahead CONTROL (the 0.7 draft's sliver slice at 44.1 kHz, must fail)",
                        [PY, "run_ahead_equiv.py", "--rate=44100", "--cases=1"],
                        env={"RUN_AHEAD_EQUIV_BREAK": "sliver", "RUN_AHEAD_EQUIV_SAY_LIMIT": "5"}, expect_fail=True,
                        fail_marks=[r"^FAIL 1\.3 Select synthesizer dialog +NEVER DONE \(run ahead, 5 s\)", RA_SUM % 2]))
    # the two lanes (run_ahead_lanes.py): lane 1, the lockstep's own schedule replayed from the capture -- times,
    # values, request boundaries and audio identical; lane 2, the deliberate retiming classified and bounded against
    # a finer and finer lockstep.  Their controls: the schedule shifted a sample, a boundary moved, answers misjudged
    for lang in ([], ["--es"]):
        CHECKS.append(check("run ahead lanes 1 and 2, %s" % ("Spanish" if lang else "English"),
                            [PY, "run_ahead_lanes.py"] + lang))
    LANES_SUM = r"^run ahead lanes \(English\): %d FAILED$"
    for what, brk, args, marks in (
            ("lane 1, schedule a sample late", "pace", ["--lane=1"],
             [r"^FAIL lane 1 case 1: TIME differs at write \d+: .*; AUDIO differs from sample \d+", LANES_SUM % 2]),
            ("lane 1, a boundary moved", "segment", ["--lane=1"],
             [r"^FAIL lane 1 case 1: SEGMENT boundary at write \d+ in the capture only$", LANES_SUM % 2]),
            ("lane 2, answers misjudged", "class", ["--lane=2"],
             [r"^FAIL lane 2 case 1 step 0\.5000 ms: UNCLASSIFIED 40 ", LANES_SUM % 4])):
        CHECKS.append(check("run ahead lanes CONTROL (%s, must fail)" % what, [PY, "run_ahead_lanes.py", "--quick"] + args,
                            env={"RUN_AHEAD_LANES_BREAK": brk}, expect_fail=True, fail_marks=marks))
    # the firmware and board state at semantic checkpoints (run_ahead_state.py), and the same continuation after them:
    # without a cancel every checkpoint must agree but for the listed timing differences; its control changes one RAM
    # byte.  With a cancel mid-utterance there is an OPEN finding (English: phonemes of the cancelled text at the head
    # of the next utterance); this tracker must keep showing it until it is fixed -- then flip it to a passing check
    for lang in ([], ["--es"]):
        CHECKS.append(check("run ahead state = lockstep at checkpoints, no cancel, %s" % ("Spanish" if lang else "English"),
                            [PY, "run_ahead_state.py", "--no-cancel"] + lang))
    CHECKS.append(check("run ahead state CONTROL (a RAM byte changed, must fail)",
                        [PY, "run_ahead_state.py", "--no-cancel"], env={"RUN_AHEAD_STATE_BREAK": "1"}, expect_fail=True,
                        fail_marks=[r"^FAIL setting .*FINDING: RAM 1 cells beyond timing's: FFF00 00/5A$",
                                    r"^run ahead state \(English\): 1 FAILED$"]))
    CHECKS.append(check("run ahead state OPEN FINDING tracker (cancel, then respeech: must still show)",
                        [PY, "run_ahead_state.py"], expect_fail=True,
                        fail_marks=[r"^FAIL respoken .*writes DIFFER at \d+ of \d+/\d+$",
                                    r"^run ahead state \(English\): \d+ FAILED$"]))
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
                        env={"BNS_EQUIV_FLIP": "1"}, expect_fail=True,
                        fail_marks=[r"^write values DIFFER from blazie_en\.txt at write \d+ of"]))
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
                        env={"SSI263_ANDROID_TEST_BREAK": "1"}, expect_fail=True,
                        fail_marks=[r"^FAILED: 4 case\(s\) differ$", r"^ok +desktop +spanish "]))
# ... its firmware import (src/csrc/blazie/bl_firmware.c, bl_state.c): files found and refused by content, only the
# releases on the list taken, and the states made from the firmware alone = the listed ones, byte for byte and in
# speech; one control holds the wrong chord at the English warm reset and must differ in the state and in every
# English case, the other drops the list and must take the unknown releases
IMPORT_TEST = os.path.join(os.path.dirname(ANDROID_TEST), "test_import_native.py")
if os.path.isfile(IMPORT_TEST):
    CHECKS.append(check("Android import: known firmware only, states made on the device", [PY, IMPORT_TEST]))
    CHECKS.append(check("Android import CONTROL (wrong warm-reset chord, must fail)", [PY, IMPORT_TEST],
                        env={"SSI263_IMPORT_TEST_BREAK": "1"}, expect_fail=True,
                        fail_marks=[r"^FAIL generated English state = the shipped", r"^import: 7 FAILED$"]))
    CHECKS.append(check("Android import CONTROL (the list dropped, must fail)", [PY, IMPORT_TEST],
                        env={"SSI263_IMPORT_TEST_BREAK": "hash"}, expect_fail=True,
                        fail_marks=[r"^FAIL a release not on the list", r"^import: 2 FAILED$"]))
# MAME's 8085 core (the Accent SA's, src/csrc/cpu/i8085_mame.cpp; not yet in a board): CONTRACT.md's clauses
# (src/csrc/cpu/test_i8085_contract.c; its must-fail controls: cpu/i8085_controls.py)
if os.path.isfile(os.path.join(LIB, "test_i8085_contract.exe")):
    CHECKS.append(check("MAME 8085 core: CPU contract tests", [os.path.join(LIB, "test_i8085_contract.exe")]))
# MAME's V40 core (the Speak-Out's, src/csrc/cpu/v40_mame.cpp) and the Speak-Out board on it (src/csrc/speakout): opt-in,
# not yet accepted (the add-on keeps Unicorn).  The contract's clauses, the board's own rules, and, with the firmware,
# the board against today's Unicorn host (speakout_core_compare.py: steps coupled as Unicorn's instructions, every write
# identical -- the migration candidate; clocks at 8 MHz, experimental: a speed grade, not a measured clock -- the
# speech frames identical, the times classified) with its must-fail control, and the
# driver on the MAME core: the candidate mame-steps gated, mame a smoke test (Astra, Reply 105).  Built by
# src/csrc/speakout/build_board.py.
SO_LIB = os.path.join(os.path.dirname(HERE), "dist", "speakout-lib")
if os.path.isfile(os.path.join(SO_LIB, "test_v40_contract.exe")):
    CHECKS.append(check("MAME V40 core: CPU contract tests", [os.path.join(SO_LIB, "test_v40_contract.exe")]))
    CHECKS.append(check("Speak-Out board (MAME V40): its rules", [os.path.join(SO_LIB, "test_so_board.exe")]))
    SO_HEX = os.path.join(os.path.dirname(os.path.dirname(HERE)), "firmware", "gw-micro-speakout", "SPEAKOUT.HEX")
    if os.path.isfile(SO_HEX) and os.path.isfile(os.path.join(SO_LIB, "x64", "speakout_v40.dll")):
        CHECKS.append(check("Speak-Out: MAME V40 core against Unicorn", [PY, "speakout_core_compare.py"]))
        CHECKS.append(check("Speak-Out: MAME V40 core CONTROL (a value flipped, must fail)",
                            [PY, "speakout_core_compare.py"], env={"SPEAKOUT_COMPARE_FLIP": "1"}, expect_fail=True,
                            fail_marks=[r"^mame-steps: write values DIFFER from Unicorn's at write \d+ of",
                                        r"^  utterance 1: speech frames DIFFER at frame \d+",
                                        r"^speakout cores: 2 FAILED$"]))
        # the gates run the migration candidate, mame-steps (Astra, Reply 105); mame (8 MHz clocks) is a labelled
        # experimental smoke test only -- passing it is not passing the candidate
        CHECKS.append(check("driver_sim speakout on the MAME V40 core (mame-steps, the candidate)",
                            [PY, "-S", "driver_sim.py", "speakout", NVDA, "rt"],
                            env={"SSI263_SPEAKOUT_CORE": "mame-steps"}))
        if os.path.isfile(PY37) and os.path.isdir(os.path.join(WIN7, "nvda2023app")):      # the x86 DLL
            CHECKS.append(check("driver_sim speakout on the MAME V40 core (mame-steps, nvda2023app, 32-bit)",
                                [PY37, "run37.py", "../driver_sim.py", "speakout", "nvda2023app", "rt"],
                                env={"NVDA_APP": "nvda2023app", "SSI263_SPEAKOUT_CORE": "mame-steps"}, cwd=WIN7))
        CHECKS.append(check("complete_fuzz speakout on the MAME V40 core (mame-steps, the candidate)",
                            [PY, "complete_fuzz.py", "150", "1"],
                            env={"SIM_SPEED": "10", "COMPLETE_FUZZ_SYNTH": "speakout",
                                 "SSI263_SPEAKOUT_CORE": "mame-steps"}))
        CHECKS.append(check("driver_sim speakout on the MAME V40 core (mame at 8 MHz: experimental smoke test)",
                            [PY, "-S", "driver_sim.py", "speakout", NVDA, "rt"], env={"SSI263_SPEAKOUT_CORE": "mame"}))

# MAME's 8086 core (the Accent-mini's PC, src/csrc/cpu/i86_mame.cpp; opt-in, Unicorn stays the default): CONTRACT.md's
# clauses (test_i86_contract.c; its must-fail controls: cpu/i86_controls.py); the Accent-mini's scripted scenarios on
# both CPUs, every promised invariant identical -- write values and times, audio, registers, FLAGS by its policy
# (Reply 106), host log, memory after INIT but for its allow-list, INIT snapshots (cpu/compare_i86_accent.py) -- with
# a must-fail control per invariant, each perturbing exactly one of them and showing its own FAIL line beside the
# others' ok (Astra, Reply 104); and the built add-on on it (SSI263_ACCENT_CORE=mame)
if os.path.isfile(os.path.join(LIB, "test_i86_contract.exe")):
    CHECKS.append(check("MAME 8086 core: CPU contract tests", [os.path.join(LIB, "test_i86_contract.exe")]))
PC86 = os.path.join(LIB, "x64", "pc86.dll")
if os.path.isfile(PC86):
    CMP86 = os.path.join(os.path.dirname(os.path.dirname(HERE)), "src", "csrc", "cpu", "compare_i86_accent.py")
    CHECKS.append(check("MAME 8086 core: Accent-mini writes = Unicorn's", [PY, CMP86, "--quick"],
                        env={"SSI263_PC86_DLL": PC86}))
    CHECKS.append(check("MAME 8086 core: Accent-mini writes CONTROL (one value flipped, must fail)",
                        [PY, CMP86, "--quick", "--control"], env={"SSI263_PC86_DLL": PC86}, expect_fail=True,
                        fail_marks=[r"^FAIL init +write values DIFFER at write 8 of 17/17",
                                    r"^FAIL state +write values DIFFER at write \d+ of", r"^compare_i86_accent: FAILED$"]))
    OK86 = {"values": r"^ok +%s +\d+ writes, values identical", "times": r"^ok +%s +times identical",
            "audio": r"^ok +%s +audio identical", "regs": r"^ok +%s +CPU registers identical",
            "flags": r"^ok +%s +FLAGS identical in bits 0-11 at \d+ samples .* 0000 / F000 \(unicorn / mame\) at every",
            "log": r"^ok +%s +host log identical", "mem": r"^ok +init +memory after INIT identical but for 4 allowed",
            "snapshot": r"^ok +state +INIT snapshots: .* identical$"}
    for what, scen, fails, mark in (
            ("time", "init", "times", r"^FAIL init +write times DIFFER: 1 of 17, the largest \+0\.000001 s at write 8"),
            ("pcm", "state", "audio", r"^FAIL state +audio DIFFERS \(\d+ / \d+ blocks\)"),
            ("reg", "init", "regs", r"^FAIL init +CPU registers DIFFER \(ax=0000 .* \| ax=10000 "),
            ("log", "init", "log", r"^FAIL init +host log DIFFERS at line \d+ of (\d+)/\d+: unicorn None, mame "
                                   r"'\(a line the control added\)'"),
            ("mem", "init", "mem", r"^FAIL init +memory after INIT DIFFERS in 1 byte\(s\) outside the allowed FLAGS "
                                   r"images \(00500: "),
            ("allowed", "init", "mem", r"^FAIL init +memory after INIT DIFFERS in 1 byte\(s\) outside the allowed "
                                       r"FLAGS images \(9FFB5: 02/F3\)"),
            ("snapshot", "state", "snapshot", r"^FAIL state +INIT snapshots: .* DIFFER in regs$"),
            # FLAGS (Astra, Reply 106): a required flag flipped (IF; CF, an arithmetic one), the masked bits no
            # longer the recorded values, the snapshot's FLAGS
            ("flags", "init", "flags", r"^FAIL init +FLAGS DIFFER at 1 of \d+ samples: first \d+ \(INT 67h\): "
                                       r"unicorn 0002, mame F202, bits 0200 \(IF\)$"),
            ("carry", "init", "flags", r"^FAIL init +FLAGS DIFFER at 1 of \d+ samples: first \d+ \(the end\): "
                                       r"unicorn 0002, mame F003, bits 0001 \(CF\)$"),
            ("flagsmask", "init", "flags", r"^FAIL init +FLAGS bits 12-15 not the recorded 0000 / F000 at 1 of \d+ "
                                           r"samples: first \d+ \(INT 67h\): unicorn 0002, mame E002$"),
            ("snapflags", "state", "snapshot", r"^FAIL state +INIT snapshots: .*\(FLAGS_POLICY: 0002 / F202\).* "
                                               r"DIFFER in eflags$")):
        others = [OK86[k] % scen if "%s" in OK86[k] else OK86[k] for k in OK86
                  if k != fails and (k != "mem" or scen == "init") and (k != "snapshot" or scen == "state")]
        CHECKS.append(check("MAME 8086 core: Accent-mini CONTROL (%s perturbed, must fail)" % what,
                            [PY, CMP86, "--quick", "--only", scen],
                            env={"SSI263_PC86_DLL": PC86, "I86_COMPARE_PERTURB": what}, expect_fail=True,
                            fail_marks=[mark] + others + [r"^compare_i86_accent: FAILED$"]))
    CHECKS.append(check("driver_sim accent on the MAME 8086 core (NVDA 64-bit)", [PY, "-S", "driver_sim.py", "accent",
                        NVDA, "rt"], env={"SSI263_ACCENT_CORE": "mame", "SSI263_PC86_DLL": PC86}))
# the Python wheel (python/): built from this tree's libraries, installed into a fresh venv, the chip audible and
# deterministic and the Braille Lite byte for byte as bl.dll; its control (rate 70 asked for) must differ
WHEEL_TEST = os.path.join(os.path.dirname(os.path.dirname(HERE)), "python", "test_wheel.py")
if os.path.isfile(os.path.join(LIB, "x64", "bl.dll")):
    CHECKS.append(check("Python wheel (64-bit)", [PY, WHEEL_TEST, "--build", ENG]))
    CHECKS.append(check("Python wheel CONTROL (rate 70, must fail)", [PY, WHEEL_TEST, "--build", ENG],
                        env={"WHEEL_TEST_BREAK": "1"}, expect_fail=True,
                        fail_marks=[r"^ok +the chip alone:", r"^FAIL +the Braille Lite:", r"^wheel: 1 FAILED$"]))
CHECKS.append(check("stacked_q_symbols", [PY, "-S", "stacked_q_symbols.py", NVDA]))
# the Braille Lite driver keeps the unit's channel open after speech (hiss/whine until the firmware clicks off),
# at no cost to response time; the control runs it with keep open off and must fail
CHECKS.append(check("keep the channel open", [PY, "keep_open_test.py"]))
CHECKS.append(check("keep the channel open CONTROL (off, must fail)", [PY, "keep_open_test.py"],
                    env={"KEEP_OPEN_BREAK": "1"}, expect_fail=True,
                    fail_marks=[r"^FAIL A hiss, keep open:", r"^FAIL A2 the click", r"^ok +F speech interrupting",
                                r"^keep open: 2 FAILED$"]))
# ... the click at the click-off (Tomi, 0.7: with keep open only): left out, only A2 must fail
CHECKS.append(check("keep the channel open CONTROL (no click, must fail)", [PY, "keep_open_test.py"],
                    env={"KEEP_OPEN_BREAK": "noclick"}, expect_fail=True,
                    fail_marks=[r"^ok +A hiss, keep open:", r"^FAIL A2 the click", r"^keep open: 1 FAILED$"]))
# ... and with every fed sample silent, each audio check must reject it (Astra, Reply 95: missing sound passed)
CHECKS.append(check("keep the channel open CONTROL (mute, every audio check fails)", [PY, "keep_open_test.py"],
                    env={"KEEP_OPEN_BREAK": "mute"}))
# the chip's defaults for front ends without Python (ssi263_defaults.h): still params.py's and the ROM's, as compiled
# bl_voice (the driver's front end in C, for speech-dispatcher and Android): the real driver's PCM, byte for byte;
# and its control (the C side with packing flipped) must fail
CHECKS.append(check("bl_voice = the NVDA driver, byte for byte", [PY, "voice_equiv.py"]))
CHECKS.append(check("bl_voice CONTROL (packing flipped, must fail)", [PY, "voice_equiv.py"],
                    env={"VOICE_EQUIV_BREAK": "1"}, expect_fail=True,
                    fail_marks=[r"^DIFF ", r"^[0-9] of 10 utterances byte-identical to the NVDA driver$"]))
# bl_voice's text path (currencies, clean-up, lines, encoding) against the driver's, on random texts; and its control
CHECKS.append(check("bl_voice text = the driver's, 5000 random texts", [PY, "voice_text_equiv.py", "5000", "1"]))
CHECKS.append(check("bl_voice text CONTROL (no currencies, must fail)", [PY, "voice_text_equiv.py", "2000", "1"],
                    env={"VOICE_TEXT_BREAK": "1"}, expect_fail=True,
                    fail_marks=[r"^DIFF ", r"^(?!2000 )\d+ of 2000 texts give the unit the same bytes"]))
# other currencies than the dollar reach every unit as words (a listener: "£2.63" was read "2.63")
CHECKS.append(check("currency rule", [PY, "currency_test.py", "rules"]))
for w in ("blazie", "speakout", "accent"):
    CHECKS.append(check("currency %s: the unit is sent pounds and pence" % w, [PY, "currency_test.py", w]))
CHECKS.append(check("currency CONTROL (rule off, must fail)", [PY, "currency_test.py", "speakout"],
                    env={"CURRENCY_OFF": "1"}, expect_fail=True,
                    fail_marks=[r"^speakout: the unit was sent .*: FAILED$"]))
CHECKS.append(check("idle channel table (bl_idle_table.h = blazie_idle.py)",
                    [PY, os.path.join(os.path.dirname(os.path.dirname(HERE)), "src", "csrc", "blazie", "gen_idle.py"),
                     "--check"]))
CHECKS.append(check("cp850 table", [PY, os.path.join(os.path.dirname(os.path.dirname(HERE)), "src", "csrc", "blazie",
                                                     "gen_cp850.py"), "--check"]))
GEN_DEFAULTS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "src", "csrc", "gen_chip_defaults.py")
CHECKS.append(check("chip defaults header", [PY, GEN_DEFAULTS, "--check"]))
# and on Python 3.7 (NVDA 2021-2023): the defaults must not depend on the Python version (3.12 changed float sum())
if os.path.isfile(PY37):
    CHECKS.append(check("chip defaults header (Python 3.7, 32-bit)", [PY37, GEN_DEFAULTS, "--check"]))


def judge_control(c, code, out, timed_out):
    """(passed, why) for a must-fail control: it passes only by failing the way it names (check's docstring)."""
    if timed_out:
        return False, "control TIMED OUT"
    if "Traceback (most recent call last)" in out:
        return False, "control CRASHED instead of failing"
    if code not in c["fail_codes"]:
        return False, "control exit %d, not its expected %s" % (code, "/".join(map(str, c["fail_codes"])))
    missing = [m for m in c["fail_marks"] if not re.search(m, out, re.M)]
    if missing:
        return False, "control's own failure not shown (missing %s)" % missing[0]
    return True, ""


def run(c):
    env = dict(os.environ, PYTHON_COLORS="0", **c["env"])
    t0 = time.perf_counter()
    timed_out, code = False, None
    try:
        p = subprocess.run(c["argv"], cwd=c["cwd"], env=env, capture_output=True, text=True, timeout=LIMIT,
                           encoding="utf-8", errors="replace")
        out, code = p.stdout + p.stderr, p.returncode
        passed = (c["ok"](out) if c["ok"] else code == 0)
        why = "" if passed else "exit %d" % code
    except subprocess.TimeoutExpired:
        out, passed, why, timed_out = "", False, "TIMEOUT after %d s" % LIMIT, True
    if c["expect_fail"]:
        passed, why = judge_control(c, code, out, timed_out)
    last = [ln for ln in out.splitlines() if ln.strip() and not ln.startswith("LOG")][-1:] or [""]
    first_bad = [ln.strip() for ln in out.splitlines() if re.search(r"ended with|died|Error|FAILED|began with", ln)][:1]
    if not passed and first_bad:
        why = (why + ": " if why else "") + first_bad[0]
    return c["name"], passed, time.perf_counter() - t0, why, last[0][:90]


def main():
    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(run, CHECKS))
    bad = 0
    for name, passed, secs, why, last in results:
        bad += not passed
        print("%-4s %-52s %5.0f s  %s" % ("ok" if passed else "FAIL", name, secs, (why or last)[:200]))
    print("%d of %d checks passed in %.0f s on %d cores" % (len(results) - bad, len(results), time.perf_counter() - t0,
                                                            os.cpu_count() or 0))
    return 1 if bad else 0


if __name__ == "__main__":          # importable: run_tests_guard.py tests run() and judge_control()
    sys.exit(main())
