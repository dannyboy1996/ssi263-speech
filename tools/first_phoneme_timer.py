"""Does the chip time a phoneme loaded after idle from a free-running divider, or from the load?  Dev lines only.

Why: Timothy hears the Braille Lite's flash-erase chirps (U1 then PA at rate F, each after ~1.7 s of idle) come at
different volumes on a real unit, and ours are alike.  If the duration counter ran off a free-running frame divider,
a phoneme loaded after idle would start at a random phase of it and lose 0..1 tick of its length; a chained phoneme
would not (A/R rises on a tick and the firmware answers at once).  MAME's die-traced SC-01 resets its duration
counters at the phoneme commit (phone_commit: "Only these two counters are reset on phone change, the rest is
free-running"), as does the SC-01 patent (US4433210A: the A/R LO "also serves to reset the phoneme timing counter").
The SSI-263's data sheet does not say.  So: measure it on the unit.

The MASTER lines start after idle with an audible phoneme written straight away (no PA first), so the first
phoneme of a line is the after-idle load.  For two repetitions A, B of an identical line (same text, settings and
emulated register stream), shift_on aligns their first phoneme's opening and shift_rest everything after its nominal
end (causal band envelopes, 10 ms, least squares; 0.5 ms search, then refined at the sample rate).
delta = shift_rest - shift_on is the difference in the first phoneme's length.

Predictions, written before the run:
  reset at the load (our model): delta ~ 0, detection noise only, at every rate;
  a free-running frame divider: delta = cut_A - cut_B, triangular over +-1 frame (median |delta| 0.29 frame:
    17 ms at rate 2, 7 ms at rate 10), scaling with 16 - R;
  a free-running 4096-XCK prescaler: the same over +-4.1 ms at every rate (rms 1.67 ms however small the noise).
The controls render every pair through our model with a random idle before the line (random pitch and noise
phase), plain or with the first phoneme cut short by a uniform phase of the grid, plus the unit's noise floor, and
go through the same measurement: the slope of delta on the known cut difference shows the method sees the mechanism.

Result (2026-10-01, 276 pairs): the unit's |delta| is the reset control's (median 0.50 against 0.49 ms; 0.012
frame against 0.247 for the free frame divider, Mann-Whitney p 4e-33), and its core rms (1.21 ms; 0.98 ms on the
voiceless-fricative openings, which no pitch pulse quantises) is under the 1.67 ms a free 4.1 ms prescaler alone
would add (p 6e-5 against that control).  The unit's few |delta| near one frame are alignment slips on fricative
pairs: their onset-to-voicing times, read directly, agree within a few ms.

One variant this cannot see: the duration counter AND the first audible change both waiting for the same free
divider.  That variant gives every chirp the same length too, so it cannot explain Timothy's report either.

Hold-out: holdout_lines.txt rows are dropped by script line before any audio is read.

    python tools/first_phoneme_timer.py            (about 3 minutes)
"""
import collections
import json
import os
import sys

import numpy as np
import soundfile as sf
from scipy.signal import butter, lfilter, sosfilt

ENGINE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src")
sys.path.insert(0, ENGINE)
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from tools import repo_paths             # noqa: E402
from ssi263 import Params, drivers       # noqa: E402
from ssi263.native import SSI263C        # noqa: E402

PRED = repo_paths.master_predictions()
MASTER = repo_paths.master_session()
SR = 44100
HOP = 0.0005
NAMES = ("PA E E1 Y YI AY IE I A AI EH EH1 AE AE1 AH AH1 AW O OU OO IU IU1 U U1 UH UH1 UH2 UH3 ER R R1 R2 L L1 LF W B "
         "D KV P T K HV HVC HF HFC HN Z S J SCH V F THV TH M N NG :A :OH :U :UH E2 LB").split()
FRIC = {"S", "SCH", "F", "TH", "HF"}
STOPS = {"B", "D", "KV", "P", "T", "K", "HVC", "HFC"}
HP = butter(2, 60, "highpass", fs=SR, output="sos")
BB = butter(4, [100, 9000], "bandpass", fs=SR, output="sos")
LP5K = butter(1, 5000, "lowpass", fs=SR, output="sos")     # the Braille Lite host's board roll-off
FLOOR_DB = -46.0                                          # the unit's floor re line peak (median, dev lines)
PRESCALER = 4096 / 1e6


def held_lines():
    held = set()
    with open(os.path.join(ENGINE, "holdout_lines.txt")) as f:
        for l in f:
            if l[:1].isdigit():
                held.add(int(l.split()[0]))
    return held


def pairs():
    """Repeated identical dev lines whose first phoneme sounds from its load and lasts 2 frames or more."""
    held = held_lines()
    preds, utt = {}, {}
    with open(PRED) as f:
        for l in f:
            x = json.loads(l)
            if x["script_line"] not in held:
                preds[x["script_line"]] = x
    with open(os.path.join(MASTER, "utterances.jsonl")) as f:
        for l in f:
            u = json.loads(l)
            if u["script_line"] not in held:
                utt[u["script_line"]] = u
    groups = collections.defaultdict(list)
    for sl, u in utt.items():
        x = preds.get(sl)
        if not x or not x.get("ok") or not u.get("marker") or u.get("lead_in") or u.get("audio_stalled"):
            continue
        key = (u["word"], json.dumps(u["params"], sort_keys=True), tuple(x["sends"]), tuple(x["r1_at_sends"]),
               tuple(x["r2"]), tuple(x["r3"]), tuple(x["r4"]))
        groups[key].append(sl)
    out = []
    for key, sls in sorted(groups.items(), key=lambda kv: min(kv[1])):
        d0 = int(key[2][0], 16)
        first = NAMES[d0 & 0x3F]
        if len(sls) < 2 or 4 - (d0 >> 6) < 2 or first in STOPS or first == "PA":
            continue
        out.append((key[0], json.loads(key[1])["rate"], first, 4 - (d0 >> 6), sorted(sls)[:2]))
    return out, utt, held


def env(y, hop=True):
    z = sosfilt(BB, sosfilt(HP, y)) ** 2
    n = int(0.010 * SR)
    e = 10 * np.log10(lfilter(np.ones(n) / n, [1.0], z) + 1e-20)      # causal throughout
    return e[:: int(HOP * SR)] if hop else e


def coarse_on(e):
    floor = np.percentile(e[: int(0.03 / HOP)], 50)
    pk = np.max(e)
    thr = max(floor + 15.0, pk - 40.0)
    above = e > thr
    n = int(0.015 / HOP)
    for i in np.flatnonzero(above):
        if above[i:i + n].mean() > 0.9:
            return i, max(floor + 3.0, pk - 50.0)
    return None, None


def best_shift(ea, eb, a0, a1, guess, search):
    seg = ea[a0:a1]
    best, bs = -np.inf, None
    for s in range(guess - search, guess + search + 1):
        if a0 + s < 0 or a1 + s > len(eb):
            continue
        c = -np.mean((seg - eb[a0 + s:a1 + s]) ** 2)
        if c > best:
            best, bs = c, s
    return bs


def pair_delta(ya, yb, frame, n_frames):
    ea, eb = env(ya), env(yb)
    ia, fa = coarse_on(ea)
    ib, fb = coarse_on(eb)
    if ia is None or ib is None:
        return None
    fl = max(fa, fb)
    ea, eb = np.maximum(ea, fl), np.maximum(eb, fl)
    w_on = int(min(0.5 * (n_frames - 1) * frame, 0.15) / HOP)
    a0 = max(0, ia - int(0.02 / HOP))
    s_on = best_shift(ea, eb, a0, ia + w_on, ib - ia, int(0.02 / HOP))
    r0 = ia + int((n_frames * frame + 0.02) / HOP)
    r1 = min(len(ea), len(eb) - int(0.1 / HOP)) - int(0.02 / HOP)
    if s_on is None or r1 - r0 < int(0.05 / HOP):
        return None
    s_rest = best_shift(ea, eb, r0, r1, s_on, int(1.1 * frame / HOP) + 2)
    if s_rest is None:
        return None
    k = int(HOP * SR)
    Ea, Eb = np.maximum(env(ya, False), fl), np.maximum(env(yb, False), fl)
    w = int(0.0015 * SR)
    f_on = best_shift(Ea, Eb, a0 * k, (ia + w_on) * k, s_on * k, w)
    f_rest = best_shift(Ea, Eb, r0 * k, r1 * k, s_rest * k, w)
    if f_on is None or f_rest is None:
        return (s_rest - s_on) * HOP
    return (f_rest - f_on) / SR


def read_real(wav, u):
    ev = u["events"]
    wav.seek(ev["text_sent"])
    y = wav.read(ev["end_with_tail"] - ev["text_sent"])
    return y if y.ndim == 1 else y[:, 0]


def render(rows, rng, grid):
    """Our model on the line's emulated stream, after a random idle; grid None (reset at the load), 'frame' or a
    period in seconds: the first phoneme then ends a uniform phase of the grid early, the rest A/R-chained."""
    chip = SSI263C(Params(), out_rate=SR)
    out = [chip.run(0.02)]
    drivers.init(chip, rows[0])
    out.append(chip.run(0.2 + rng.uniform(0, 0.05)))
    frame = 4096.0 * (16 - (rows[0]["RE"] >> 4)) / 1e6
    dur0 = frame * (4 - (rows[0]["DP"] >> 6))
    g = frame if grid == "frame" else grid
    cut = 0.0 if g is None else rng.uniform(0, g)
    for i, row in enumerate(rows):
        if i == 1 and cut > 0:
            out.append(chip.run(dur0 - cut))
        elif i:
            out.append(chip.run_until_request(5.0))
        for name in drivers.ORDER_ATTR_FIRST:
            chip.write(drivers.REG[name], row[name])
    out.append(chip.run_until_request(5.0))
    out.append(chip.run(0.3))
    y = sosfilt(LP5K, np.concatenate([np.asarray(o, dtype=float) for o in out]))
    pk = np.max(np.abs(y)) or 1.0
    return y + rng.normal(0, pk * 10 ** (FLOOR_DB / 20.0), len(y)), cut


def main():
    todo, utt, held = pairs()
    print("dev pairs: %d (first phoneme sounds from its load, >= 2 frames); hold-out: %d script lines dropped before "
          "any audio was read" % (len(todo), len(held)))
    wav = sf.SoundFile(os.path.join(MASTER, "master.wav"))
    rng = np.random.default_rng(84)
    res = []
    for word, rate, first, n_frames, sls in todo:
        frame = 4096.0 * (16 - rate) / 1e6
        ya, yb = (read_real(wav, utt[s]) for s in sls)
        r = dict(word=word, rate=rate, first=first, sls=sls, unit=pair_delta(ya, yb, frame, n_frames))
        rows, _ = drivers.master_rows(PRED, sls[0])
        for name, grid in (("reset", None), ("frame", "frame"), ("presc", PRESCALER)):
            (sa, ca), (sb, cb) = (render(rows, rng, grid) for _ in range(2))
            r[name] = pair_delta(sa, sb, frame, n_frames)
            r[name + "_true"] = ca - cb
        res.append(r)
    report(res)


def report(res):
    cols = ("unit", "reset", "frame", "presc")
    for name in ("frame", "presc"):
        d = np.array([r[name] for r in res if r[name] is not None])
        t = np.array([r[name + "_true"] for r in res if r[name] is not None])
        print("control free-%s: delta on the true cut difference: slope %.2f, r %.3f (n %d)"
              % (name, np.polyfit(t, d, 1)[0], np.corrcoef(t, d)[0, 1], len(d)))
    print("\n|delta| per rate, ms: median / p90       (columns: unit, model reset, model free frame, model free 4.1 ms)")
    by = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in res:
        for c in cols:
            if r[c] is not None:
                by[r["rate"]][c].append(abs(r[c]) * 1e3)
    for rt in sorted(by):
        print("rate %2d (frame %4.1f ms) n=%3d  " % (rt, 4.096 * (16 - rt), len(by[rt]["unit"]))
              + "  ".join("%5.1f/%5.1f" % (np.median(by[rt][c]), np.percentile(by[rt][c], 90)) for c in cols))
    print()
    for c in cols:
        a = np.array([abs(r[c]) * 1e3 for r in res if r[c] is not None])
        f = np.array([abs(r[c]) * 1e3 / (4.096 * (16 - r["rate"])) for r in res if r[c] is not None])
        print("pooled %-5s n=%3d |delta| median %5.2f ms (%.3f frame), p90 %5.2f ms (%.3f frame)"
              % (c, len(a), np.median(a), np.median(f), np.percentile(a, 90), np.percentile(f, 90)))
    print("\nfine scale, pairs with |delta| < 8 ms: rms (a free 4.1 ms prescaler alone gives 1.67 ms)")
    for cls, keep in (("all", lambda r: True), ("voiceless fricative first", lambda r: r["first"] in FRIC)):
        print("  %-26s " % cls + "  ".join(
            "%s %.2f ms (n %d)" % (c, np.sqrt(np.mean(a ** 2)), len(a)) for c in cols
            for a in [np.array([r[c] * 1e3 for r in res if r[c] is not None and keep(r) and abs(r[c]) < 0.008])]))
    try:
        from scipy.stats import mannwhitneyu
        u = np.array([abs(r["unit"]) for r in res if r["unit"] is not None])
        for c in ("reset", "presc", "frame"):
            v = np.array([abs(r[c]) for r in res if r[c] is not None])
            print("Mann-Whitney, unit |delta| smaller than %s: p = %.2g" % (c, mannwhitneyu(u, v, alternative="less").pvalue))
    except ImportError:
        pass


if __name__ == "__main__":
    main()
