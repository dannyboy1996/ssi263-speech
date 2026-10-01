# -*- coding: utf-8 -*-
"""The SSI-263 add-ons' drivers behind a pipe, for the SAPI 5 engine (sapi/ssi263_sapi.cpp).

A fork of outspoken-nvda's sapi/osp_serve.py (itself Panthera's): the SAPI engine DLL launches this under the
embeddable Python installed beside it, and every decision about how speech sounds -- the firmware, the emulators,
the chip, number reading, cancel -- runs in the same driver files NVDA users run, byte for byte.  There is no port
to drift, because there is no port.  The three add-ons' synthDrivers folders sit beside this script (staged by
sapi/build.ps1 up to 0.7.0), or come from nvda/dist/*-build in the repository (since 0.7.5 the native drivers there).

Requests arrive on stdin, framed:

    'OSP4' | seq | rate | pitch | volume | namelen | textlen | name | text

and a cancel is 'OSPC' | seq.  The seq is the whole point of the cancel frame: pipes buffer, so a cancel sent for
one utterance can arrive after it finished and the next one started, and an untagged cancel then cuts the wrong
render.  A cancel only acts when its seq is the one rendering.

rate/pitch/volume are the drivers' own 0-100 integers; name is a voice id "<driver module>:<voice>", as --list
prints it.  The response is 'OSPR' | status, then PCM in chunks as the driver produces them -- u32 frame count, then
frames*2 bytes of 16-bit mono at the --rate (default 22050 Hz) -- and a zero frame count to finish.

`--list` prints one voice per line: "id<TAB>name<TAB>language".
`--drivers` makes every driver as a request would, then prints where they came from: "path<TAB>folder" for each
synthDrivers folder, "module<TAB>name<TAB>file" for every synthDrivers.* module loaded, "dll<TAB>file" for every DLL
loaded outside Windows and Python (sapi/reference_drivers.py checks the tests' reference is 0.7.0's drivers by it).
`--inflection 1|0` and `--whine off|hiss|whine`: the Braille Lite's voice inflection and hiss/whine (the dialog's).
`--rate 11025|22050|44100`: the output sample rate of every voice (the dialog's; the DLL declares the same rate).
`--accent-inflection 0..100`: the Accent's intonation, as its NVDA slider (five steps; 100, full, by default).
`--run-ahead 1|0`: the Braille Lite's "Run the unit ahead" (EXPERIMENTAL, off by default), as its NVDA setting: the
unit writes the whole utterance flat out and the chip plays the script (src/csrc/blazie/run_ahead.c).  Both Braille
Lite voices; the driver decides where it applies (short pauses on, the in-process unit, the voices it was tested on).

Test hooks, never set by the engine DLL: SSI263_SAPI_DRIVERS=<folder> takes the drivers from <folder>/<add-on>-build/
synthDrivers instead (the SAPI tests point it at nvda/tools/legacy_drivers.py's 0.7.0 drivers: since 0.7.5 nvda/dist
holds the native drivers, and the reference must not be what it is compared with); SSI263_SAPI_WRITE_LOG=<file> appends every Braille Lite unit's say, cancel
and SSI-263 write there (sapi/test_serve.py reads the phonemes from it), and with it SSI263_SAPI_NO_UNIT_CANCEL=1
never cancels the unit; SSI263_SAPI_IGNORE_RUN_AHEAD=1 drops the run-ahead setting on its way to the driver.  The two
1s are test_serve.py's must-fail controls (nvda/tools/run_tests.py).
"""
import os
import struct
import sys
import threading
import time
import types
import importlib

HERE = os.path.dirname(os.path.abspath(__file__))
RATE = 22050                # the add-ons' default rate (ssi263_rates.DEFAULT); --rate picks another of theirs
REQ = 0x4F535034            # 'OSP4'
RSP = 0x4F535052            # 'OSPR'
CANCEL = 0x4F535043         # 'OSPC'
DRIVERS = (("blazie", "blazie"), ("speakout", "speakout"), ("accent", "accentmini"))   # (add-on, module)


def _driver_dirs():
    """The synthDrivers folders: SSI263_SAPI_DRIVERS's (a test hook: a folder of <add-on>-build/synthDrivers, as
    nvda/tools/legacy_drivers.py makes 0.7.0's), staged beside this script, or the repository's built add-ons."""
    root = os.environ.get("SSI263_SAPI_DRIVERS")
    staged = os.path.join(HERE, "synthDrivers")
    if not root and os.path.isdir(staged):
        return [staged]
    dist = root or os.path.join(os.path.dirname(HERE), "nvda", "dist")
    return [os.path.join(dist, "%s-build" % a, "synthDrivers") for a, _m in DRIVERS
            if os.path.isdir(os.path.join(dist, "%s-build" % a, "synthDrivers"))]


def _loaded_dlls():
    """Every DLL in this process outside Windows and this Python's own folders (the --drivers report)."""
    if os.name != "nt":
        return []
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.GetCurrentProcess.restype = wintypes.HANDLE
    k32.K32EnumProcessModules.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD,
                                          ctypes.POINTER(wintypes.DWORD)]
    k32.GetModuleFileNameW.argtypes = [wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
    mods, need = (wintypes.HMODULE * 4096)(), wintypes.DWORD()
    if not k32.K32EnumProcessModules(k32.GetCurrentProcess(), mods, ctypes.sizeof(mods), ctypes.byref(need)):
        return ["?"]
    skip = [os.path.normcase(os.path.abspath(p)) + os.sep
            for p in (os.environ.get("SystemRoot") or os.environ.get("windir"), sys.base_prefix, sys.prefix) if p]
    out = []
    for h in mods[:min(len(mods), need.value // ctypes.sizeof(wintypes.HMODULE))]:
        buf = ctypes.create_unicode_buffer(32768)
        k32.GetModuleFileNameW(h, buf, len(buf))
        path = os.path.normcase(os.path.abspath(buf.value))
        if buf.value and not any(path.startswith(s) for s in skip):
            out.append(buf.value)
    return out


class _StreamPlayer(object):
    """An nvwave.WavePlayer whose feed() is the wire.  The drivers end every utterance with an empty feed that
    carries onDone (NVDA 2021-2023's buffered player needs it); here it is simply called."""
    out = None
    lock = threading.Lock()
    last_feed = [0.0]

    def __init__(self, *a, **k):
        pass

    def feed(self, data, onDone=None):
        if data:
            with _StreamPlayer.lock:
                if _StreamPlayer.out is not None:
                    _StreamPlayer.out.write(struct.pack("<I", len(data) // 2) + bytes(data))
                    _StreamPlayer.out.flush()
                    _StreamPlayer.last_feed[0] = time.monotonic()
        if onDone:
            onDone()

    def stop(self):
        pass

    def idle(self):
        pass

    def pause(self, switch):
        pass

    def close(self):
        pass


class _Done(object):
    """synthDoneSpeaking: remembers which driver finished."""
    def __init__(self):
        self.event = threading.Event()
        self.synth = None

    def notify(self, synth=None, **k):
        self.synth = synth
        self.event.set()


def _install_fakes():
    """Enough of NVDA for the three drivers (the same stand-ins as nvda/tools/fake_nvda_driver_test.py)."""
    nvwave = types.ModuleType("nvwave")
    nvwave.WavePlayer = _StreamPlayer
    nvwave.AudioPurpose = type("AudioPurpose", (), {"SPEECH": 1})
    sys.modules["nvwave"] = nvwave

    cfg = types.ModuleType("config")
    cfg.conf = {"audio": {"outputDevice": "default"}, "speech": {"outputDevice": "default"}}
    sys.modules["config"] = cfg

    logh = types.ModuleType("logHandler")

    class _Log(object):
        def _drop(self, *a, **k):
            pass
        info = debug = warning = error = exception = _drop

        def isEnabledFor(self, level):
            return False
    logh.log = _Log()
    sys.modules["logHandler"] = logh

    speech = types.ModuleType("speech")
    cmds = types.ModuleType("speech.commands")

    class IndexCommand(object):
        def __init__(self, index=0):
            self.index = index

    class PitchCommand(object):
        def __init__(self, offset=0):
            self.offset = offset

    class LangChangeCommand(object):
        def __init__(self, lang=None):
            self.lang = lang
    cmds.IndexCommand, cmds.PitchCommand, cmds.LangChangeCommand = IndexCommand, PitchCommand, LangChangeCommand
    speech.commands = cmds
    sys.modules["speech"] = speech
    sys.modules["speech.commands"] = cmds

    sdh = types.ModuleType("synthDriverHandler")

    class VoiceInfo(object):
        def __init__(self, id, name, language=None):
            self.id, self.name, self.language = id, name, language

    class SynthDriver(object):
        VoiceSetting = RateSetting = PitchSetting = VolumeSetting = VariantSetting = InflectionSetting = \
            staticmethod(lambda *a, **k: None)

        def __init__(self):
            pass
    sdh.SynthDriver = SynthDriver
    sdh.VoiceInfo = VoiceInfo
    sdh.synthDoneSpeaking = _Done()
    sdh.synthIndexReached = type("_Idx", (), {"notify": lambda self, **k: None})()
    sys.modules["synthDriverHandler"] = sdh

    asu = types.ModuleType("autoSettingsUtils")
    asu_utils = types.ModuleType("autoSettingsUtils.utils")
    asu_utils.StringParameterInfo = lambda *a, **k: a
    ds = types.ModuleType("autoSettingsUtils.driverSetting")
    ds.DriverSetting = ds.BooleanDriverSetting = ds.NumericDriverSetting = lambda *a, **k: None
    asu.utils, asu.driverSetting = asu_utils, ds
    sys.modules["autoSettingsUtils"] = asu
    sys.modules["autoSettingsUtils.utils"] = asu_utils
    sys.modules["autoSettingsUtils.driverSetting"] = ds

    import builtins
    if not hasattr(builtins, "_"):
        builtins._ = lambda s: s

    pkg = types.ModuleType("synthDrivers")      # NVDA's package, with the add-ons' folders on it
    pkg.__path__ = _driver_dirs()
    sys.modules["synthDrivers"] = pkg
    return sdh.synthDoneSpeaking


_drivers = {}
# The settings no SAPI request carries (the settings dialog, sapi/settings.ps1): the engine DLL passes them on the
# command line and replaces this server when they change.
OPTIONS = {"inflection": True, "whine": "off", "rate": RATE, "accent_inflection": 100, "run_ahead": False}


def _log_writes(cls, path):
    """The test hook (SSI263_SAPI_WRITE_LOG): every unit the Braille Lite driver boots logs its says (with the unit's
    run_ahead as it speaks), cancels and SSI-263 writes, one per line, as nvda/tools/run_ahead_driver.py observes
    them in-process.  Only the worker thread talks to a unit, so the lines come in its order."""
    out = open(path, "a", encoding="utf-8")
    boot = cls._boot

    def logged_boot(self, *a, **k):
        unit = boot(self, *a, **k)
        say, cancel = unit.say, unit.cancel

        def logged_say(text, *sa, **sk):
            out.write("say %s %s %r\n" % (getattr(unit, "voice", "?"), getattr(unit, "run_ahead", "-"), text))
            out.flush()
            return say(text, *sa, **sk)

        def logged_cancel(*ca, **ck):
            out.write("cancel\n")
            out.flush()
            if os.environ.get("SSI263_SAPI_NO_UNIT_CANCEL") == "1":      # a control: the unit runs on
                return 0.0
            return cancel(*ca, **ck)
        unit.say, unit.cancel = logged_say, logged_cancel
        if hasattr(type(unit), "on_write"):
            unit.on_write = lambda t, reg, val: (out.write("w %d %d\n" % (reg, val)), out.flush())
        return unit
    cls._boot = logged_boot


def driver(module):
    """One resident driver per add-on, made on first use, with the dialog's settings."""
    if module not in _drivers:
        mod = importlib.import_module("synthDrivers." + module)
        if module == "blazie" and os.environ.get("SSI263_SAPI_WRITE_LOG"):
            _log_writes(mod.SynthDriver, os.environ["SSI263_SAPI_WRITE_LOG"])
        d = mod.SynthDriver()
        d._set_sampleRate(str(OPTIONS["rate"]))       # every driver: the rate the DLL declared to SAPI
        if module == "accentmini":
            d._set_inflection(OPTIONS["accent_inflection"])     # its NVDA slider's 0-100 (five steps)
        if module == "blazie":
            d._set_voiceInflection(OPTIONS["inflection"])
            d._set_whine(OPTIONS["whine"])
            if hasattr(d, "_set_runAhead") and os.environ.get("SSI263_SAPI_IGNORE_RUN_AHEAD") != "1":   # 1: a control
                d._set_runAhead(OPTIONS["run_ahead"])     # applied by the driver at each utterance, for both voices
            # never the open channel after speech: SAPI gives the engine its next text only after this utterance's
            # stream ends, so the idle hiss would hold every queued utterance back by up to ~10 s (the driver's
            # tail would feed on and this server would wait for it: a response ends 0.12 s after the last feed)
            if hasattr(d, "_set_keepOpen") and os.environ.get("SSI263_SAPI_KEEP_OPEN") != "1":   # 1: a test control
                d._set_keepOpen(False)
        _drivers[module] = d
    return _drivers[module]


def modules_present():
    return [m for _a, m in DRIVERS
            if any(os.path.isfile(os.path.join(p, m + ".py")) for p in sys.modules["synthDrivers"].__path__)]


def _exact(stream, n):
    buf = b""
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def _claim_stdout():
    """The protocol keeps the pipe; stdout stops being it (a stray print must never land in the audio stream)."""
    fd = os.dup(1)
    try:
        import msvcrt
        msvcrt.setmode(fd, os.O_BINARY)
    except ImportError:
        pass
    proto = os.fdopen(fd, "wb")
    try:
        sys.stdout.flush()
    except Exception:
        pass
    try:
        os.dup2(2, 1)
    except OSError:
        nul = os.open(os.devnull, os.O_WRONLY)
        os.dup2(nul, 1)
        os.close(nul)
    sys.stdout = sys.stderr
    return proto


def main():
    args = sys.argv[1:]
    for k in range(len(args) - 1):
        if args[k] == "--inflection":
            OPTIONS["inflection"] = args[k + 1] not in ("0", "off", "false")
        elif args[k] == "--whine" and args[k + 1] in ("off", "hiss", "whine"):
            OPTIONS["whine"] = args[k + 1]
        elif args[k] == "--rate" and args[k + 1] in ("11025", "22050", "44100"):
            OPTIONS["rate"] = int(args[k + 1])
        elif args[k] == "--accent-inflection" and args[k + 1].isdigit():
            OPTIONS["accent_inflection"] = max(0, min(100, int(args[k + 1])))
        elif args[k] == "--run-ahead":
            OPTIONS["run_ahead"] = args[k + 1] in ("1", "on", "true")
    done_evt = _install_fakes()
    if "--drivers" in args:
        ds = [driver(m) for m in modules_present()]      # each made as a request makes it (its unit booted)
        try:
            for p in sys.modules["synthDrivers"].__path__:
                print("path\t%s" % p)
            for name, mod in sorted(sys.modules.items()):
                if name.startswith("synthDrivers.") and getattr(mod, "__file__", None):
                    print("module\t%s\t%s" % (name, mod.__file__))
            for p in _loaded_dlls():
                print("dll\t%s" % p)
        finally:
            for d in ds:
                d.terminate()
        return 0
    if "--list" in args:
        try:
            sys.stdout.reconfigure(encoding="utf-8")       # "español" must reach the token registration intact
        except Exception:
            pass
        for m in modules_present():
            d = driver(m)
            try:
                for vid, info in d._get_availableVoices().items():
                    print("%s:%s\t%s\t%s" % (m, vid, info.name, info.language or "en"))
            finally:
                d.terminate()
        return 0

    stdin = sys.stdin.buffer
    stdout = _claim_stdout()
    inbox, inbox_ready, eof = [], threading.Event(), threading.Event()
    cancel_now = threading.Event()
    current = [0, None]          # seq rendering, its driver
    cancelled_seqs = set()

    def reader():
        while True:
            magic_bytes = _exact(stdin, 4)
            if magic_bytes is None:
                break
            magic = struct.unpack("<I", magic_bytes)[0]
            if magic == CANCEL:
                seq_bytes = _exact(stdin, 4)
                if seq_bytes is None:
                    break
                seq = struct.unpack("<I", seq_bytes)[0]
                cancelled_seqs.add(seq)
                if seq == current[0] and current[1] is not None:
                    cancel_now.set()
                    try:
                        current[1].cancel()
                    except Exception:
                        pass
                continue
            if magic != REQ:
                break
            rest = _exact(stdin, 24)
            if rest is None:
                break
            seq, rate, pitch, volume, nv, nt = struct.unpack("<IiiiII", rest)
            name, text = _exact(stdin, nv), _exact(stdin, nt)
            if name is None or text is None:
                break
            inbox.append((seq, rate, pitch, volume, name, text))
            inbox_ready.set()
        eof.set()
        inbox_ready.set()

    threading.Thread(target=reader, daemon=True).start()
    try:
        while True:
            while not inbox:
                if eof.is_set():
                    return 0
                inbox_ready.wait(0.5)
                inbox_ready.clear()
            seq, rate, pitch, volume, name, text = inbox.pop(0)
            if seq in cancelled_seqs:
                cancelled_seqs.discard(seq)
                stdout.write(struct.pack("<Ii", RSP, 0) + struct.pack("<I", 0))
                stdout.flush()
                continue
            cancel_now.clear()
            status, d = 0, None
            try:
                module, _sep, vid = name.decode("utf-8").partition(":")
                if module not in modules_present():
                    module = modules_present()[0]
                d = driver(module)
                if vid and vid in d._get_availableVoices() and d._get_voice() != vid:
                    d._set_voice(vid)
                d._set_rate(max(0, min(100, rate)))
                d._set_pitch(max(0, min(100, pitch)))
                d._set_volume(max(0, min(100, volume)))
            except Exception:
                status = 1
            stdout.write(struct.pack("<Ii", RSP, status))
            stdout.flush()
            if status:
                continue
            current[0], current[1] = seq, d
            done_evt.event.clear()
            _StreamPlayer.out = stdout
            _StreamPlayer.last_feed[0] = time.monotonic()
            try:
                d.speak([text.decode("utf-8", "replace")])
                deadline = time.monotonic() + 120.0
                done = False
                # the end of the audio: the driver's done for THIS driver, then no feed for a settle window
                while not (cancel_now.is_set() or eof.is_set()):
                    if not done:
                        if done_evt.event.wait(0.03):
                            if done_evt.synth is d:
                                done = True
                            else:
                                done_evt.event.clear()     # another driver's done: not this utterance's end
                    else:
                        time.sleep(0.03)
                    if time.monotonic() > deadline:
                        d.cancel()
                        break
                    if done and time.monotonic() - _StreamPlayer.last_feed[0] > 0.12:
                        break
            finally:
                with _StreamPlayer.lock:
                    _StreamPlayer.out = None
            current[0], current[1] = 0, None
            cancelled_seqs.discard(seq)
            stdout.write(struct.pack("<I", 0))
            stdout.flush()
    finally:
        for d in _drivers.values():
            try:
                d.terminate()
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
