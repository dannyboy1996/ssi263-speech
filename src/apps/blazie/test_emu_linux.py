"""The Linux emulator (blazie_emu), headless: the whole program -- its key decoding, its units, its memory folder --
run with no sound card and no terminal (--null), keys typed from a script at their times, as a terminal would send
them.

    python3 src/apps/blazie/test_emu_linux.py BLAZIE_EMU FIRMWARE_DIR [--only NAME,NAME]

FIRMWARE_DIR holds BL2ENG.BNS and bl2_2003_warm.state, and the Type 'n Speak's TNSENG.TNS (in tns/ or beside them).
The checks:
  boot           the Braille Lite boots and speaks; F (dot 1) typed at 8 s is answered, and not without it
  clock-keys     o-chord t typed as keys (f s k space, then d s j k): the unit says the time, the host's
  clock-letters  the same in letters mode (O, then t: the BTSpeak's computer braille, a capital as its chord)
  tns            the Type 'n Speak from cold: it speaks, y y answered, then F4 says the host's time
  memory         the date and time set through the unit's own commands (typed as keys), the memory saved to the
                 program's folder (--autosave) and started from again: the unit still holds 2015-09-30
  held           an input device's keys: p-chord, l, i-chord held through the restart -- the cold reset's question
The control: BLAZIE_KEYS_BREAK=1 swaps dots 1 and 4 in every chord; clock-keys and clock-letters must then FAIL
(tools/linux_tests.sh judges it by its marks).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

failures = 0
ran = 0


def check(name, ok, detail):
    global failures, ran
    print("%-4s %-34s %s" % ("ok" if ok else "FAIL", name, detail))
    failures += not ok
    ran += 1


def run(exe, args, script=None, tmp=None):
    """blazie_emu's output; script: lines "T type TEXT" / "T down KEY" / "T up KEY"."""
    cmd = [exe] + args
    if script is not None:
        path = os.path.join(tmp, "script.txt")
        with open(path, "w") as f:
            f.write("\n".join(script) + "\n")
        cmd += ["--script", path]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, timeout=600)
    return p.stdout


def rms(out, frm, to):
    m = re.search(r"^rms %.2f-%.2f s: ([0-9.]+)$" % (frm, to), out, re.M)
    return float(m.group(1)) if m else -1.0


def said_time(out):
    m = re.search(r"^clock: (.*)$", out, re.M)
    return (m is not None and m.group(1).endswith(": yes")), (m.group(1) if m else "no clock line")


def main():
    exe, fw = sys.argv[1], sys.argv[2]
    only = sys.argv[sys.argv.index("--only") + 1].split(",") if "--only" in sys.argv else None
    tns = next((p for p in (os.path.join(fw, "tns", "TNSENG.TNS"), os.path.join(fw, "TNSENG.TNS"))
                if os.path.isfile(p)), None)
    tmp = tempfile.mkdtemp(prefix="blazie_emu_test.")
    base = ["--firmware", fw, "--null", "--rate", "22050"]
    try:
        def want(name):
            return only is None or name in only

        if want("boot"):
            a = run(exe, base + ["--unit", "bl-en", "--seconds", "10", "--rms", "0:6", "--rms", "8.2:10"],
                    ["8.0 type f"], tmp)
            b = run(exe, base + ["--unit", "bl-en", "--seconds", "10", "--rms", "8.2:10"], [], tmp)
            g, w, wo = rms(a, 0, 6), rms(a, 8.2, 10), rms(b, 8.2, 10)
            check("boot greeting", g > 0.01, "rms %.4f over the first 6 s" % g)
            check("a chord answered (F typed)", w > 0.01 and 0 <= wo < 0.002,
                  "8.2-10 s: rms %.4f with F typed at 8 s, %.4f without (the control)" % (w, wo))

        if want("clock-keys"):
            out = run(exe, base + ["--unit", "bl-en", "--seconds", "16", "--clock-check"],
                      ["12.0 type fsk ", "13.5 type dsjk"], tmp)
            ok, d = said_time(out)
            check("the clock, from the system time (keys)", ok, d)

        if want("clock-letters"):
            out = run(exe, base + ["--unit", "bl-en", "--input", "letters", "--seconds", "16", "--clock-check"],
                      ["12.0 type O", "13.5 type t"], tmp)
            ok, d = said_time(out)
            check("the clock, from the system time (letters)", ok, d)

        if want("tns"):
            if not tns:
                check("Type 'n Speak boot", False, "no TNSENG.TNS in %s (or its tns folder)" % fw)
            else:
                out = run(exe, base + ["--unit", "tns-en", "--flash-instant", "--seconds", "16", "--rms", "0:6",
                                       "--clock-check"],
                          ["3.0 type y", "6.0 type y", "12.0 type \\eOS"], tmp)
                g = rms(out, 0, 6)
                ok, d = said_time(out)
                check("Type 'n Speak boot (from cold)", g > 0.01 and "from cold" in out,
                      "rms %.4f over the first 6 s, %s" % (g, "a cold start" if "from cold" in out else "NOT cold"))
                check("Type 'n Speak: y y, then F4 the time", ok, d)

        if want("memory"):
            cfg = os.path.join(tmp, "config")
            # o-chord s d 0 9 3 0 1 5, o-chord s t 1 2 3 4 p -- test_clock.c's commands, typed as keys 0.4 s apart
            o, s, d, t = "fsk ", "dsj", "fjk", "dsjk"
            digits = {"0": "skl", "9": "sk", "3": "dk", "1": "d", "5": "dl", "2": "ds", "4": "dkl"}
            lines = []
            for at, word in ((18.0, [o, s, d]), (21.0, [digits[c] for c in "093015"]), (28.0, [o, s, t]),
                             (31.0, [digits[c] for c in "1234"]), (34.0, ["fdsj"])):
                lines += ["%.1f type %s" % (at + 0.4 * i, k) for i, k in enumerate(word)]
            one = run(exe, base + ["--config", cfg, "--autosave", "--unit", "bl-en", "--seconds", "38",
                                   "--clock-check"], lines, tmp)
            path = os.path.join(cfg, "english.state")
            saved = os.path.isfile(path) and os.path.getsize(path) >= 786432
            m = re.search(r"the unit's clock (\d{4}-\d\d-\d\d)", one)
            check("memory: set through the unit, saved", saved and m is not None and m.group(1) == "2015-09-30",
                  "the clock %s; %s %s" % (m.group(1) if m else "?", path if saved else "NOT saved",
                                           "(%d bytes)" % os.path.getsize(path) if saved else ""))
            two = run(exe, base + ["--config", cfg, "--autosave", "--unit", "bl-en", "--seconds", "4",
                                   "--rms", "0:4", "--clock-check"], [], tmp)
            m = re.search(r"the unit's clock (\d{4}-\d\d-\d\d)", two)
            started = "starting bl-en from %s" % path in two
            g = rms(two, 0, 4)
            check("memory: started again from it", started and m is not None and m.group(1) == "2015-09-30"
                  and g > 0.01, "%s; the clock %s; rms %.4f" % ("from the saved memory" if started else
                                                               "NOT from the saved memory", m.group(1) if m else "?", g))

        if want("held"):
            # an input device (evdev): p-chord, l, then i-chord (dots 2 4 and space) held while the unit restarts
            lines = []
            def chord(at, keys, hold=0.05):
                for i, k in enumerate(keys):
                    lines.append("%.3f down %s" % (at + 0.01 * i, k))
                for i, k in enumerate(keys):
                    lines.append("%.3f up %s" % (at + 0.01 * len(keys) + hold + 0.01 * i, k))
            chord(8.0, ["f", "d", "s", "j", "space"])
            chord(10.0, ["f", "d", "s"])
            chord(10.25, ["d", "j", "space"], hold=1.75)
            lines.sort(key=lambda x: float(x.split()[0]))
            out = run(exe, base + ["--unit", "bl-en", "--seconds", "14", "--ram-has", "initialize file system"],
                      lines, tmp)
            ok = 'ram has "initialize file system": yes' in out
            check("i-chord held through the restart", ok, "the unit asked \"initialize file system\": %s"
                  % ("yes" if ok else "no"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if failures:
        print("emulator: %d of %d FAILED" % (failures, ran))
    else:
        print("emulator: all %d passed" % ran)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
