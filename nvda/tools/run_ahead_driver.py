"""Run ahead through the REAL driver (the built add-on under stand-in NVDA modules): Tomi's stress test, Tab held down
in Windows' Run dialog.  At key-repeat rate NVDA cancels and speaks each control's label in turn ("Open, combo box",
"OK, button", "Cancel, button", "Browse..., button").  Every SSI-263 write is logged per utterance: each utterance's
spoken phonemes, up to the next cancel, must be a prefix of its own label's (the label said alone first) -- nothing of
the previous label at its head, no leftover of a word cut mid-way.

A device-paced player (each block blocks for its own duration), wall-clock intervals: the cancel lands wherever the
worker is.  Default settings, "run ahead" on.  The labels are single lines: they never stage a next line, the window
of the cancel leak run_ahead_cancel.py tracks, so this is the listener's scenario, not that leak's regression.

    python run_ahead_driver.py [--n=40] [--ms=30,50] [--off]
    RUN_AHEAD_DRIVER_BREAK=nocancel   the unit is never cancelled: the previous label runs on (must fail)
"""
import importlib
import os
import sys
import threading
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, REPO)
from tools import repo_paths             # noqa: E402

BUILD = repo_paths.synth_drivers("blazie")
AHEAD = "--off" not in sys.argv
N = next((int(a[4:]) for a in sys.argv if a.startswith("--n=")), 40)
MS = next(([float(x) for x in a[5:].split(",")] for a in sys.argv if a.startswith("--ms=")), [30.0, 50.0])
BREAK = os.environ.get("RUN_AHEAD_DRIVER_BREAK", "")
LABELS = ["Open, combo box", "OK, button", "Cancel, button", "Browse..., button"]


# ---- stand-ins ---------------------------------------------------------------------------------------------------
class Player:
    def __init__(self, *a, samplesPerSec=22050, **k):
        self.rate = samplesPerSec

    def feed(self, data, onDone=None):
        if data:
            time.sleep(len(data) / 2.0 / self.rate)    # a device: a block blocks for its own duration
        if onDone:
            onDone()

    def stop(self):
        pass

    def idle(self):
        pass

    def pause(self, s):
        pass

    def close(self):
        pass


def module(name, **attrs):
    m = types.ModuleType(name)
    m.__dict__.update(attrs)
    sys.modules[name] = m
    return m


notified = []
_lock = threading.Lock()


class Notifier:
    def __init__(self, name):
        self.name = name

    def notify(self, **kw):
        with _lock:
            notified.append((self.name, kw.get("index")))


class Base:
    VoiceSetting = RateSetting = PitchSetting = VolumeSetting = VariantSetting = InflectionSetting = \
        staticmethod(lambda: None)

    def __init__(self):
        pass


class Log:
    def isEnabledFor(self, level):
        return False

    def debug(self, msg):
        pass

    def warning(self, msg, exc_info=False):
        print("LOG WARNING:", msg)

    def error(self, msg, exc_info=False):
        print("LOG ERROR:", msg)


class IndexCommand:
    def __init__(self, index):
        self.index = index


class PitchCommand:
    def __init__(self, offset=0):
        self.offset = offset


class LangChangeCommand:
    def __init__(self, lang=None):
        self.lang = lang


module("nvwave", WavePlayer=Player)
module("config", conf={"audio": {"outputDevice": "default"}, "speech": {"outputDevice": "default"}})
module("synthDriverHandler", SynthDriver=Base, VoiceInfo=lambda *a: a, synthIndexReached=Notifier("index"),
       synthDoneSpeaking=Notifier("done"))
module("autoSettingsUtils")
module("autoSettingsUtils.utils", StringParameterInfo=lambda *a: a)
module("autoSettingsUtils.driverSetting", BooleanDriverSetting=lambda *a, **k: None, DriverSetting=lambda *a, **k: None)
module("logHandler", log=Log())
cmds = module("speech.commands", IndexCommand=IndexCommand, PitchCommand=PitchCommand,
              LangChangeCommand=LangChangeCommand)
module("speech", commands=cmds)
module("synthDrivers", __path__=[BUILD])
drv = importlib.import_module("synthDrivers.blazie")
NB = drv.NativeBlazie
if NB is None:
    sys.exit("the built add-on has no in-process unit (bl.dll)")

# ---- every write, say and cancel of the unit, in order -----------------------------------------------------------
LOG = []                 # ("w", t, reg, val) | ("say", t, lines) | ("cancel", t)
_say, _cancel = NB.say, NB.cancel


def say(self, text):
    LOG.append(("say", self.chip.time, text))
    return _say(self, text)


def cancel(self, *a, **k):
    LOG.append(("cancel", self.chip.time))
    if BREAK == "nocancel":
        return 0.0
    return _cancel(self, *a, **k)


NB.say, NB.cancel = say, cancel
_boot = drv.SynthDriver._boot


def boot(self, voice="blazie"):
    u = _boot(self, voice)
    u.on_write = lambda t, r, v: LOG.append(("w", t, r, v))
    return u


drv.SynthDriver._boot = boot


def utterances(log):
    """[(label, spoken phonemes applied between its say and the next cancel)]"""
    out, cur = [], None
    for e in log:
        if e[0] == "say":
            lines = e[2] if isinstance(e[2], list) else [e[2]]
            cur = (" ".join(lines), [], [0])
            out.append(cur)
        elif e[0] == "cancel":
            cur = None
        elif e[0] == "w" and cur is not None:
            if e[2] == 3:
                cur[2][0] = e[3]
            if e[2] == 0 and not cur[2][0] & 0x80 and e[3] & 0x3F:
                cur[1].append(e[3] & 0x3F)
    return [(lab, sp) for lab, sp, _ in out]


def main():
    d = drv.SynthDriver()
    d._set_runAhead(AHEAD)
    t0 = time.monotonic()
    while d._unit is None and time.monotonic() - t0 < 20:
        time.sleep(0.05)
    ref = {}
    for j, lab in enumerate(LABELS):             # each label said alone, to its end
        n0 = len(LOG)
        d.speak([lab, IndexCommand(5000 + j)])
        t0 = time.monotonic()
        while ("index", 5000 + j) not in notified or notified[-1][0] != "done":
            if time.monotonic() - t0 > 20:
                sys.exit("FAIL %r never done" % lab)
            time.sleep(0.01)
        time.sleep(0.2)
        said = utterances(LOG[n0:])
        ref[said[-1][0]] = said[-1][1]
    failures = 0
    for ms in MS:
        n0 = len(LOG)
        for i in range(N):
            d.cancel()
            d.speak([LABELS[i % len(LABELS)], IndexCommand(i)])
            time.sleep(ms / 1000.0)
        d.cancel()
        time.sleep(0.3)
        us = utterances(LOG[n0:])
        heard, bad = 0, []
        for k, (lab, sp) in enumerate(us):
            want = ref.get(lab)
            heard += bool(sp)
            if want is None or sp != want[:len(sp)]:
                bad.append("%d %r: %s, its own %s" % (k, lab, sp[:6], (want or [])[:6]))
        failures += bool(bad)
        print("%-4s Tab every %.0f ms: %d utterances (NVDA drops the ones cancelled before they start), %d with "
              "speech before the next cancel, %d led by anything but their own label%s" % (
                  "FAIL" if bad else "ok", ms, len(us), heard, len(bad), "".join("; " + b for b in bad[:3])))
    d.terminate()
    print("run ahead through the driver, Run dialog (%s): %s" % ("run ahead" if AHEAD else "lockstep",
                                                                 "all ok" if not failures else "%d FAILED" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
