"""Play a recorded Braille Lite session (SSI263_BLAZIE_RECORD=<file>, native_blazie.py) back offline.

  bl_replay.py <record> <writes-out> [native|pipe] [--session=N]

Writes every SSI-263 write as "t reg val" (t exact) to <writes-out>.  'native' replays into bl.dll (the fresh
build in nvda/dist/blazie-lib), 'pipe' into the Python host and bns_live.exe from the built add-on: the same calls
into both hosts show whether a difference is the C port's or the firmware's.  A record holds one session per unit
the driver started; --session picks one (default: the last).
"""
import json
import os
import platform
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "src"))
sys.path.insert(0, HERE)
from ssi263.native import SSI263C                       # noqa: E402
from ssi263.params import Params                        # noqa: E402
from write_spy import watch_writes                      # noqa: E402

ARCH = "x64" if platform.architecture()[0] == "64bit" else "x86"
DLL = os.path.join(REPO, "nvda", "dist", "blazie-lib", ARCH, "bl.dll")
EXE = os.path.join(REPO, "nvda", "dist", "blazie-build", "synthDrivers", "_ssi263_blazie", "bns_live.exe")
PY_NAMES = {"stale_f": "_stale_f"}


def sessions(path):
    out = []
    with open(path) as f:
        for ln in f:
            call = json.loads(ln)
            if call[0] == "init":
                out.append([])
            out[-1].append(call)
    return out


def make_unit(calls, host):
    init = calls[0][1]
    params, rate = next(c[1:] for c in calls if c[0] == "chip")
    chip = SSI263C(params=Params(params), out_rate=rate)
    kw = dict(chip=chip, out_rate=init["out_rate"], menu=tuple(init["menu"]), key_start=init["key_start"],
              key_gap=init["key_gap"], board_lowpass_hz=init["board_lowpass_hz"], status=tuple(init["status"]))
    if host == "native":
        from hosts.native_blazie import NativeBlazie
        unit = NativeBlazie(DLL, init["firmware"], init["state"], **kw)
    else:
        from hosts.blazie import Blazie
        unit = Blazie(EXE, init["firmware"], init["state"], **kw)
    unit.encoding = "latin-1"
    return unit


def apply(unit, call, host):
    op, args = call[0], call[1:]
    if op not in ("init", "chip", "owed"):
        if op == "send":
            unit.send(args[0].encode("latin-1"))
        elif op == "say":
            unit.say(args[0].split("\r\x06")[:-2])
        elif op == "run":
            unit.run(*args)
        elif op == "cancel":
            unit.cancel(*args)
        elif op == "skip":
            unit.skip(*args)
        elif op == "busy":
            unit.busy(*args)
        elif op == "whine":
            unit.whine = args[0]
        elif op in ("set", "setf"):
            name, v = args
            if host == "pipe":
                if name in ("preparing", "turbo_between_lines"):
                    v = bool(v)
                elif name == "prep_step" and v == 0.0:
                    v = None
                name = PY_NAMES.get(name, name)
            setattr(unit, name, v)
        else:
            sys.exit("unknown call %r" % (op,))


def replay(calls, host, on_write):
    unit = make_unit(calls, host)
    watch_writes(unit, on_write)
    for call in calls:
        apply(unit, call, host)
    unit.close()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--"))
    record, out = args[0], args[1]
    host = args[2] if len(args) > 2 else "native"
    ss = sessions(record)
    calls = ss[int(opts.get("session", len(ss) - 1))]
    with open(out, "w") as f:
        replay(calls, host, lambda t, reg, val: f.write("%r %d %02X\n" % (t, reg, val)))
    print("%s: %d calls replayed into %s, %d sessions in the record" % (host, len(calls), out, len(ss)))


if __name__ == "__main__":
    main()
