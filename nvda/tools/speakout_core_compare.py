"""The Speak-Out on MAME's V40 core against today's Unicorn host: the same scenario, the same chip model, and every
SSI-263 write compared (src/hosts/speakout.py: SpeakOut and its opt-in SpeakOutV40; src/csrc/speakout).

    python speakout_core_compare.py [--clock HZ] [--cores unicorn,mame-steps,mame] [--dump DIR]

The scenario: power on and the greeting spoken to its end, then sentences, each spoken to its end, except one cut by
^X after 0.8 s.  Cores:
  unicorn     today's host (the reference)
  mame-steps  the MAME core coupled to chip time by steps at cpu_ips, counted as Unicorn counts its instructions
              (so_board.h): every write's value AND time must be identical -- a difference would be the instructions'.
              The migration candidate (Astra, Reply 103)
  mame        EXPERIMENTAL: the MAME core coupled by its clocks at --clock (default: speakout.py's V40_HZ, the
              uPD70208-8's speed grade, not a measured clock; the unit's oscillator is unread and may be divided, and
              no instructions-to-clocks ratio makes the two couplings equivalent).  Slower or faster rules
              change how many idle frames (PA at rate F: no phoneme ready when the chip asks) the firmware writes, so
              the SPEECH frames are compared: every utterance spoken to its end must have exactly Unicorn's (each
              frame's writes: the R0 prime, R1-R4, the phoneme); the cut one is compared up to the shorter.  The times
              are classified per utterance: where its first phoneme lands after the text was sent, idle frames inside
              it (a gap in the speech), and how its phonemes move against its first one.
Exit 1 if a check fails.  SPEAKOUT_COMPARE_FLIP=1 flips one value in each MAME stream: both checks must then fail
(run_tests' control).  A pass shows that this firmware's writes, in this scenario, do not depend on where the cores
differ; it does not establish the CPU's semantics, its interrupt timing or the host's scheduling.

The firmware is not in the repository: firmware/gw-micro-speakout/SPEAKOUT.HEX, or SSI263_SPEAKOUT_HEX.
"""
import argparse
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "src"))
os.environ.pop("SSI263_SPEAKOUT_CORE", None)          # each core is chosen here, never by the environment
from hosts import speakout  # noqa: E402

HEX = os.environ.get("SSI263_SPEAKOUT_HEX") or os.path.join(REPO, "firmware", "gw-micro-speakout", "SPEAKOUT.HEX")
TEXTS = [
    "Hello, world.",
    "The quick brown fox jumps over the lazy dog.",
    "It costs $12.50, or 1,234 points; call 555-0199 at 3:45 PM.",
    "Custom number processing, check box, checked.",
    ("CUT", "This long sentence is cut off by control X before it can finish speaking all of its words."),
    "Yes, it works.",
    "Mississippi, Worcestershire, and Llanfair are hard words.",
]


def scenario(box):
    """[(utterance index, chip time, reg, value)], and each utterance's send time; -1 = the greeting."""
    utt = [-1]
    log, sent = [], []
    box.keep_writes = False
    orig = box._chip_write

    def spy(reg, v):
        log.append((utt[0], round(box.chip.time, 6), reg, v))
        orig(reg, v)

    def to_end():
        t = 0.0
        while t < 15 and box.busy():
            box.run(0.05)
            t += 0.05

    box._chip_write = spy
    box.run(0.3)
    to_end()                                  # the greeting, whole
    box.skip(0.1)
    for i, text in enumerate(TEXTS):
        utt[0] = i
        sent.append(box.chip.time)
        if isinstance(text, tuple):
            box.say(text[1] + "\r")
            box.run(0.8)
            box.cancel()
        else:
            box.say(text + "\r")
            to_end()
        box.skip(0.1)
    return log, sent


def make(core, clock):
    if core == "unicorn":
        return speakout.SpeakOut(HEX)
    return speakout.SpeakOutV40(HEX, core=core, clock_hz=clock)


def frames(log):
    """{utterance: [(time, idle, writes)]}: a frame is the writes up to and including a phoneme load (R0 other than
    the 00 prime); idle = PA (C0h) at rate F, the start-of-utterance routine's filler (0x4318)."""
    out, cur, r2 = {}, [], 0
    for u, t, reg, v in log:
        cur.append((reg, v))
        if reg == 2:
            r2 = v
        if reg == 0 and v != 0x00:
            out.setdefault(u, []).append((t, v == 0xC0 and (r2 >> 4) == 0xF, tuple(cur)))
            cur = []
    return out


def compare_speech(ref, ref_sent, got, got_sent):
    """(failures, lines): the speech frames per utterance, and the times classified."""
    fa, fb = frames(ref), frames(got)
    bad, lines = 0, []
    for i in range(-1, len(TEXTS)):
        a, b = fa.get(i, []), fb.get(i, [])
        sa = [f for _, idle, f in a if not idle]
        sb = [f for _, idle, f in b if not idle]
        ta = [t for t, idle, _ in a if not idle]
        tb = [t for t, idle, _ in b if not idle]
        cut = i >= 0 and isinstance(TEXTS[i], tuple)
        name = "the greeting" if i < 0 else "utterance %d%s" % (i, " (cut)" if cut else "")
        n = min(len(sa), len(sb)) if cut else max(len(sa), len(sb))
        if not sa or sa[:n] != sb[:n] or (not cut and len(sa) != len(sb)):
            k = next((j for j, (x, y) in enumerate(zip(sa, sb)) if x != y), min(len(sa), len(sb)))
            bad += 1
            lines.append("  %s: speech frames DIFFER at frame %d of %d/%d" % (name, k, len(sa), len(sb)))
            continue
        gaps = [t for t, idle, _ in b if idle and tb[0] < t < tb[-1]]
        gaps_ref = [t for t, idle, _ in a if idle and ta[0] < t < ta[-1]]
        inner = [(y - tb[0]) - (x - ta[0]) for x, y in zip(ta, tb)]
        if i >= 0:
            start = "first phoneme %.1f ms after the send (Unicorn %.1f)" % ((tb[0] - got_sent[i]) * 1e3,
                                                                            (ta[0] - ref_sent[i]) * 1e3)
        else:
            start = "first phoneme at %.3f s (Unicorn %.3f)" % (tb[0], ta[0])
        lines.append("  %s: %d speech frames identical%s; %s; idle frames inside it %d (Unicorn %d); its phonemes "
                     "against its first within %+.2f ms" % (
                         name, n, " up to the cut (%d and %d spoken)" % (len(sa), len(sb)) if cut else "", start,
                         len(gaps), len(gaps_ref), max(inner, key=abs) * 1e3))
    return bad, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clock", type=float, default=speakout.V40_HZ)
    ap.add_argument("--cores", default="unicorn,mame-steps,mame")
    ap.add_argument("--dump", default=None)
    args = ap.parse_args()
    if not os.path.isfile(HEX):
        sys.exit("no firmware: %s (put SPEAKOUT.HEX there, or set SSI263_SPEAKOUT_HEX)" % HEX)
    cores = args.cores.split(",")
    runs = {}
    for core in cores:
        box = make(core, args.clock)
        log, sent = scenario(box)
        if core != "unicorn" and os.environ.get("SPEAKOUT_COMPARE_FLIP"):
            fr = frames(log)[1]                       # a speech phoneme of utterance 1: its value's low bit
            k = log.index(next(w for w in log if w[0] == 1 and w[1] == fr[len(fr) // 2][0] and w[2] == 0 and w[3]))
            u, t, reg, v = log[k]
            log[k] = (u, t, reg, v ^ 0x01)
        runs[core] = (log, sent, box)
        extra = ""
        if core != "unicorn":
            st = box.board.cpu_state()
            extra = ", %d clocks, %d undefined opcodes" % (box.board.cycles(), st["undefined"])
        n_speech = sum(1 for fs in frames(log).values() for _, idle, _ in fs if not idle)
        print("%-10s %5d writes, %4d speech frames, %9d instructions/steps%s" % (core, len(log), n_speech, box.insns,
                                                                               extra))
        if args.dump:
            os.makedirs(args.dump, exist_ok=True)
            with open(os.path.join(args.dump, core + ".txt"), "w") as f:
                for u, t, reg, v in log:
                    f.write("%d %.6f R%d=%02X\n" % (u, t, reg, v))
    bad = 0
    ref, ref_sent, _ = runs["unicorn"]
    for core in cores:
        if core == "unicorn":
            continue
        got, got_sent, box = runs[core]
        if core == "mame-steps":
            va = [(u, reg, v) for u, _, reg, v in ref]
            vb = [(u, reg, v) for u, _, reg, v in got]
            if va != vb:
                bad += 1
                k = next((i for i, (x, y) in enumerate(zip(va, vb)) if x != y), min(len(va), len(vb)))
                print("mame-steps: write values DIFFER from Unicorn's at write %d of %d/%d: Unicorn %s, MAME %s" % (
                    k, len(va), len(vb), va[k] if k < len(va) else "-", vb[k] if k < len(vb) else "-"))
                continue
            dt = max(abs(a[1] - b[1]) for a, b in zip(ref, got))
            if dt:
                bad += 1
                print("mame-steps: write values identical, times DIFFER from Unicorn's: up to %.3f ms" % (dt * 1e3))
            else:
                print("mame-steps: all %d writes identical to Unicorn's, values and times" % len(vb))
            continue
        f, lines = compare_speech(ref, ref_sent, got, got_sent)
        bad += f
        print("mame at %.3f MHz: %s; by utterance:" % (
            args.clock / 1e6, "speech frames DIFFER from Unicorn's" if f else "every speech frame identical to "
            "Unicorn's (the cut one up to the cut); times classified"))
        for ln in lines:
            print(ln)
    print("speakout cores: %s" % ("all as expected" if not bad else "%d FAILED" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
