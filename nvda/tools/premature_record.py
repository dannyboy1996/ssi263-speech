"""How golden/premature_history_mame.jsonl was made: the premature completion's session on the MAME Z180 (0.7).

The 0.6 history (golden/premature_history_seed4.jsonl: complete_fuzz seed 4, caught live on the z180emu core) no
longer miscounts on MAME: its chip writes and ^F echoes land at other times, and 0.6.0's busy() speaks its last line
whole (the legacy z180emu still cuts it at W UH1 N: bl_live_legacy.exe, built with build_board.py --legacy-tests).
So the same miscount was searched for on MAME, in the fuzz's own move -- a line cancelled while it speaks, then a
line at once -- through bl.dll with the driver's boot and the driver's chip settings (taken from the seed-4 record):
each of complete_fuzz's five texts cancelled every 10 ms from 0.05 to 3.0 s (1,475 sessions), then "One, two, three,
four." under 0.6.0's busy().  Exactly one cut on the coarse grid: "One, two, three, four." itself cancelled 1.750 s in
-- as its last ^F echo is due -- and respoken: done at its comma, W UH1 N, 8 phonemes after.  On a 1 ms grid the
window is 1.745-1.755 s, on both hosts (the pipe host at 2 ms).  The record takes its middle.

    python premature_record.py <out.jsonl>      # then review and copy it to golden/premature_history_mame.jsonl
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [REPO, os.path.join(REPO, "src")]
ENG = os.path.join(REPO, "nvda", "dist", "blazie-build", "synthDrivers", "_ssi263_blazie")
DLL = os.path.join(REPO, "nvda", "dist", "blazie-lib", "x64" if sys.maxsize > 2 ** 32 else "x86", "bl.dll")
SEED4 = os.path.join(HERE, "golden", "premature_history_seed4.jsonl")
TEXT = "One, two, three, four."
CANCEL_AT = 1.750


def main():
    out = sys.argv[1]
    if os.path.exists(out):
        os.remove(out)
    with open(SEED4) as f:                     # the driver's unit and chip settings, as the live record has them
        init = json.loads(f.readline())[1]
        params, rate = json.loads(f.readline())[1:]
    os.environ["SSI263_BLAZIE_RECORD"] = out
    os.environ.pop("SSI263_BLAZIE_CANCEL_SETTLE", None)   # the shipping host, no override
    from ssi263.native import SSI263C
    from ssi263.params import Params
    from hosts.native_blazie import NativeBlazie
    chip = SSI263C(params=Params(params), out_rate=rate)
    u = NativeBlazie(DLL, os.path.join(ENG, "BL2ENG.BNS"), os.path.join(ENG, "bl2_2003_warm.state"), chip=chip,
                     out_rate=init["out_rate"], menu=tuple(init["menu"]), key_start=init["key_start"],
                     key_gap=init["key_gap"], board_lowpass_hz=init["board_lowpass_hz"], status=tuple(init["status"]))
    u.send(b"\x18")                            # the driver's boot (synthDrivers/blazie.py _boot)
    u.send(b"\r\x06")
    u.run(0.3)
    u.send(b"\x056V")
    u.run(0.05)
    u.turbo_between_lines = True
    u.say([TEXT])
    t0 = chip.time
    while chip.time - t0 < CANCEL_AT - 1e-9:   # the driver's blocks, busy() asked after each
        u.run(min(0.03, CANCEL_AT - (chip.time - t0)))
        u.busy()
    u.cancel(3.0, None, None)
    u.turbo_between_lines = True
    u.say([TEXT])                              # the cut say: the replay gives it itself, from the record's last call
    u.close()
    with open(out) as f:
        lines = f.read().replace(json.dumps(ENG.replace("\\", "/"))[1:-1], "{ENG}")
    first = json.loads(lines.splitlines()[0])[1]
    for k in ("firmware", "state"):
        first[k] = "{ENG}/" + os.path.basename(first[k])
    rest = lines.splitlines()[1:]
    with open(out, "w", newline="\n") as f:
        f.write(json.dumps(["init", first]) + "\n" + "\n".join(rest) + "\n")
    print("wrote %s: %d calls" % (out, 1 + len(rest)))


if __name__ == "__main__":
    main()
