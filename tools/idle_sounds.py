"""The Braille Lite's idle-channel sounds on Tomi's unit, measured, for the Blazie emulator's board sounds
(src/hosts/blazie_idle.py, src/csrc/blazie/bl_idle.c).

The data: the W03 grid (blite_sweep sessions W03_whine_grid, W03b_whine_grid_ac, W03c_redo_v14; every volume 0-15 x
tone 0-26, "Readme for Microsoft Windows." then 11 s of idle, 96 kHz, unit on battery).  Sweep sessions, not MASTER
lines: no hold-out applies.  Per cell:

  the click-off     the step when the firmware closes the channel (R3 = 00 and port A0 bit 1 off in the emulated
                    firmware), timed from the line's acoustic end (last 10 ms frame, 300 Hz high-passed, 15 dB over
                    the idle hiss), and its waveform from the edge (first sample 50 mFS off the level before it)
  the pop           the step when the next line opens the channel again (port A0 bit 1 on, ~0.26 s before its first
                    phoneme in the emulated firmware), its waveform, and the next line's acoustic onset after it
  the hiss          RMS of the idle 1-9 s after the line, 60 Hz high-passed and 10 kHz low-passed, in dBFS (absolute,
                    not re the speech), and the same after the click-off (the capture floor)
  the speech        the loud-vowel RMS (20 ms frames within 6 dB of the loudest, 60 Hz high-passed) from 0.2 s after the
                    pop -- whine_grid.py's reference took the pop's frame at low volumes, so its "dB re vowel" is not
                    the vowel there -- and the share of clipped samples (|x| > 0.99)
  the tick          the idle folded at the unit's 10 Hz timer period, phase from the click-off edge (the firmware
                    writes R3 = 00 in that timer's service), and the same after the click-off
  the comb lines    whine_grid.py's per-line measure (n x fc/64, Welch, lobe minus local floor), in dBFS

Cells from the W03 session's late part are left out: from ~78 min the volume-1 anchor cells show the unit changed
(the tick 10x, the hiss +12 dB) -- a drifting unit, not a volume effect (tools/idle_sounds.py drift).

    python tools/idle_sounds.py measure            per-volume table (and caches the per-cell numbers)
    python tools/idle_sounds.py drift              the anchor cells over the session
    python tools/idle_sounds.py fit                the model's numbers for src/hosts/blazie_idle.py
    python tools/idle_sounds.py excerpt V T OUT    a real-unit excerpt: volume V tone T, the line, its idle, the
                                                   click-off and the next line's pop (for listening)
"""
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np
import soundfile as sf
from scipy.optimize import curve_fit
from scipy.signal import butter, sosfilt, welch, resample_poly

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO)
from tools import repo_paths  # noqa: E402

SESSIONS = ("W03_whine_grid", "W03b_whine_grid_ac", "W03c_redo_v14")     # later sessions replace earlier cells
CACHE = os.path.join(repo_paths.INVESTIGATION_OUT, "idle_sounds_cells.npz")
EDGE = 0.05              # FS: a step's first sample this far off the level before it
TICK_P = 0.09998         # s: the timer period as captured (a fold's tick energy peaks here; 100.000 ms folds 3 dB less)
DRIFT_TICK = 2.0         # a cell whose tick is more than this x the grid median is from the drifted part
CLICK_S, POP_S, TICK_LO, TICK_HI = 1.2, 0.24, 0.010, 0.020
DEC = 4                  # the click and pop waveforms kept at 24 kHz
THIRDS = 10 ** np.arange(np.log10(25.0), np.log10(20000.0), 0.1)     # 1/3-octave centres for the noise spectrum


def db10(ms):
    return 10 * np.log10(ms + 1e-30)


def rows_of(s):
    with open(os.path.join(repo_paths.sweep_session(s), "utterances.jsonl")) as f:
        return [json.loads(ln) for ln in f]


def first_edge(x, a, b):
    base = np.median(x[max(0, a - 1920):a])
    i = np.nonzero(np.abs(x[a:b] - base) > EDGE)[0]
    return (a + int(i[0]), base) if len(i) else (None, base)


def comb_lines(idle, sr, tone):
    """whine_grid.py's line measure, absolute: {n: dBFS mean square}."""
    f, P = welch(idle, sr, nperseg=65536)
    df = f[1] - f[0]
    fc = 500000.0 / (32 - tone)
    out = {}
    for n in range(1, 65):
        fl = n * fc / 64
        if fl > min(45000, sr / 2 - 500):
            break
        i = int(round(fl / df))
        lobe = P[i - 3:i + 4].sum() * df
        loc = (np.abs(f - fl) < 150) & (np.abs(f - fl) > 5 * df)
        floor = np.median(P[loc]) * 7 * df
        if lobe >= floor * 10 ** 0.6:
            out[n] = float(db10(lobe - floor))
    return out


def cell(job):
    s, k = job
    rows = rows_of(s)
    r, nx = rows[k], rows[k + 1]
    path = os.path.join(repo_paths.sweep_session(s), "master.wav")
    sr = sf.info(path).samplerate
    o = r["frame_start"] - int(0.1 * sr)
    x, _ = sf.read(path, start=o, stop=nx["frame_start"] + int(1.0 * sr))
    L = lambda f: f - o
    lm, nxt = L(r["events"]["line_marker"]), L(nx["frame_start"])
    hp60 = sosfilt(butter(4, 60, "high", fs=sr, output="sos"), x)
    band = sosfilt(butter(4, 10000, "low", fs=sr, output="sos"), hp60)
    hp300 = sosfilt(butter(4, 300, "high", fs=sr, output="sos"), x)
    rms = lambda y: float(20 * np.log10(np.sqrt(np.mean(y ** 2)) + 1e-12))
    out = dict(session=s, index=k, trial=r["label"].get("trial"), volume=int(r["params"]["volume"]),
               tone=int(r["params"]["tone"]), minute=r["frame_start"] / sr / 60.0)
    a, b = lm + int(1.0 * sr), lm + int(9.0 * sr)
    out["hiss_db"], out["hiss_full_db"], hiss300 = rms(band[a:b]), rms(hp60[a:b]), rms(hp300[a:b])
    # this line's own pop (its channel opened after the previous idle) and the speech after it
    p0, _ = first_edge(x, L(r["frame_start"]), L(r["frame_start"]) + int(0.8 * sr))
    s0 = (p0 if p0 is not None else L(r["frame_start"]) + int(0.3 * sr)) + int(0.2 * sr)
    sp = hp60[s0:lm + int(0.1 * sr)]
    h = int(0.02 * sr)
    e = np.array([np.mean(sp[i:i + h] ** 2) for i in range(0, len(sp) - h, h)])
    out["vowel_db"] = float(db10(np.mean(e[e > e.max() * 10 ** -0.6])))
    out["clip"] = float(np.mean(np.abs(x[s0:lm]) > 0.99))
    # the acoustic end: the last 10 ms frame 15 dB over the hiss (300 Hz high-passed)
    w = int(0.01 * sr)
    seg = hp300[s0:lm + int(1.0 * sr)]
    fr = np.array([np.sqrt(np.mean(seg[i:i + w] ** 2)) for i in range(0, len(seg) - w, w)])
    loud = np.nonzero(fr > 10 ** ((hiss300 + 15) / 20))[0]
    end = s0 + (int(loud[-1]) + 1) * w if len(loud) else None
    # the click-off
    jc, base = first_edge(x, lm + int(9.5 * sr), nxt)
    if jc is None:
        return out
    out["click_after_end"] = (jc - end) / sr if end is not None else None     # volume 0 speaks silently
    out["click_after_marker"] = (jc - lm) / sr
    out["click_wave"] = (x[jc:jc + int(CLICK_S * sr)] - base)[::DEC].astype(np.float32)
    out["floor_db"] = rms(band[jc + int(0.4 * sr):nxt - int(0.02 * sr)])
    # the pop, and the next line's onset after it
    jp, base = first_edge(x, nxt, nxt + int(0.8 * sr))
    if jp is not None:
        out["pop_after_sent"] = (jp - nxt) / sr
        out["pop_wave"] = (x[jp - int(0.001 * sr):jp + int(POP_S * sr)] - base).astype(np.float32)
        seg = hp300[jp + int(0.15 * sr):jp + int(0.8 * sr)]
        fr = np.array([np.sqrt(np.mean(seg[i:i + w // 2] ** 2)) for i in range(0, len(seg) - w // 2, w // 2)])
        on = np.nonzero(fr > 10 ** ((hiss300 + 20) / 20))[0]
        out["onset_after_pop"] = 0.15 + int(on[0]) * w / 2 / sr if len(on) else None
    # the tick: fold the open idle before the click-off, and the closed idle after it, at the timer period
    y = sosfilt(butter(2, 20, "high", fs=sr, output="sos"), x)
    per, lo, hi = TICK_P * sr, int(TICK_LO * sr), int(TICK_HI * sr)
    for name, sign, first, count in (("tick", -1, 1, 75), ("tick_off", 1, 4, 6)):
        acc = np.zeros(lo + hi)
        for kk in range(first, first + count):
            c0 = jc + sign * int(round(kk * per)) - lo
            acc += y[c0:c0 + lo + hi]
        out[name] = (acc / count).astype(np.float32)
    out["lines"] = comb_lines(x[L(r["events"]["end_with_tail"]) + int(0.3 * sr):
                                min(L(r["events"]["end_with_tail"]) + int(9.0 * sr), nxt - int(0.05 * sr))], sr, out["tone"])
    # the broadband noise under the lines: 1/3-octave median densities (a band's median ignores its lines), open and
    # closed (after the click-off)
    for name, seg in (("psd_open", x[a:b]), ("psd_off", x[jc + int(0.5 * sr):nxt - int(0.02 * sr)])):
        f, P = welch(seg, sr, nperseg=16384)
        out[name] = np.array([np.median(P[(f >= c / 10 ** 0.05) & (f < c * 10 ** 0.05)]) for c in THIRDS], dtype=np.float32)
    return out


def measure_all():
    if os.path.isfile(CACHE):
        return list(np.load(CACHE, allow_pickle=True)["cells"])
    jobs = []
    for s in SESSIONS:
        rows = rows_of(s)
        for k, r in enumerate(rows[:-1]):
            gap = (rows[k + 1]["frame_start"] - r["events"]["line_marker"]) / 96000.0
            if r["label"].get("trial") in ("grid", "anchor") and r.get("marker", True) and gap > 10.6:
                jobs.append((s, k))
    with ProcessPoolExecutor() as ex:
        cells = list(ex.map(cell, jobs, chunksize=4))
    # a later session's cell replaces an earlier one (W03b/W03c redid W03's end)
    last = {}
    for c in cells:
        if c["trial"] == "grid":
            last[(c["volume"], c["tone"])] = c
    cells = [c for c in cells if c["trial"] != "grid" or last[(c["volume"], c["tone"])] is c]
    ticks = [float(np.min(c["tick"][int(TICK_LO * 96000) - 96:int(TICK_LO * 96000) + 480])) for c in cells if "tick" in c]
    med = np.median(ticks)
    for c in cells:
        c["drift"] = "tick" in c and float(np.min(c["tick"][int(TICK_LO * 96000) - 96:int(TICK_LO * 96000) + 480])) < DRIFT_TICK * med
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    np.savez_compressed(CACHE, cells=np.array(cells, dtype=object))
    return cells


def good(cells, trial="grid"):
    return [c for c in cells if c["trial"] == trial and not c.get("drift") and "click_wave" in c]


def med(cs, key):
    v = [c[key] for c in cs if c.get(key) is not None and np.isfinite(c[key])]
    return float(np.median(v)) if v else float("nan")


def cmd_measure():
    cells = measure_all()
    g = good(cells)
    print("cells: %d grid cells measured, %d left out as drifted (W03's late part), %d used" % (
        len([c for c in cells if c["trial"] == "grid"]), len([c for c in cells if c["trial"] == "grid" and c.get("drift")]), len(g)))
    print("vol   n | vowel dBFS clip%% | hiss dBFS (60 Hz-10 kHz) full | floor after click-off | hiss re vowel | "
          "click-off s after end | pop mFS  s after sent  onset s after pop | tick mFS")
    for v in range(16):
        cs = [c for c in g if c["volume"] == v]
        if not cs:
            continue
        tick = 1000 * np.min(np.mean([c["tick"] for c in cs], axis=0)[960 - 96:960 + 480])
        pop = 1000 * np.median([c["pop_wave"][96 + 96] for c in cs if "pop_wave" in c])      # 1 ms after the edge
        print("%2d  %3d | %6.1f %5.1f | %6.1f %6.1f | %6.1f | %6.1f | %6.3f | %6.0f %6.3f %6.3f | %5.2f" % (
            v, len(cs), med(cs, "vowel_db"), 100 * med(cs, "clip"), med(cs, "hiss_db"), med(cs, "hiss_full_db"),
            med(cs, "floor_db"), med(cs, "hiss_db") - med(cs, "vowel_db"), med(cs, "click_after_end"), pop,
            med(cs, "pop_after_sent"), med(cs, "onset_after_pop"), tick))
    ca = [c["click_after_end"] for c in g if c.get("click_after_end") is not None]
    print("click-off after the acoustic end: median %.3f s, IQR %.3f-%.3f, n %d; after the ^F marker: %.3f s" % (
        np.median(ca), np.percentile(ca, 25), np.percentile(ca, 75), len(ca), med(g, "click_after_marker")))
    odd = [c for c in g if c["volume"] % 2 and c["volume"] > 0]
    even = [c for c in g if not c["volume"] % 2 and c["volume"] > 0]
    print("hiss by class (60 Hz-10 kHz): odd volumes (whine) %.1f dBFS, even (hiss) %.1f dBFS; floor after click-off %.1f" % (
        med(odd, "hiss_db"), med(even, "hiss_db"), med(g, "floor_db")))
    open_t = np.mean([c["tick"] for c in g], axis=0)
    off_t = np.mean([c["tick_off"] for c in g], axis=0)
    lo = int(TICK_LO * 96000)
    print("the tick, folded over %d cells: open channel %.2f mFS (min), closed (after the click-off) %.3f mFS" % (
        len(g), 1000 * open_t[lo - 96:lo + 480].min(), 1000 * off_t[lo - 96:lo + 480].min()))


def cmd_drift():
    cells = measure_all()
    lo = int(TICK_LO * 96000)
    for c in sorted([c for c in cells if c["trial"] == "anchor" and "tick" in c], key=lambda c: (c["session"], c["minute"])):
        print("%-20s %5.1f min  volume 1 tone 7 anchor: tick %6.2f mFS  hiss %6.1f dBFS  %s" % (
            c["session"], c["minute"], 1000 * c["tick"][lo - 96:lo + 480].min(), c["hiss_db"], "DRIFTED" if c["drift"] else ""))


def two_hp(t, a, t1, t2):
    """a step of a through two first-order high-passes (time constants t1, t2)."""
    w1, w2 = 1 / t1, 1 / t2
    return a * (w1 * np.exp(-w1 * t) - w2 * np.exp(-w2 * t)) / (w1 - w2)


def fit_step(t, y, spike=False):
    big = 10 * np.max(np.abs(y))
    if spike:
        f = lambda t, a, t1, t2, s, ts: two_hp(t, a, t1, t2) + s * np.exp(-t / ts)
        p, _ = curve_fit(f, t, y, p0=(y[len(y) // 50], 0.06, 1.0, y[0] - y[len(y) // 50], 0.0002), maxfev=20000,
                         bounds=((-big, 0.005, 0.1, -big, 2e-5), (big, 0.5, 30.0, big, 0.005)))
    else:
        f = two_hp
        p, _ = curve_fit(f, t, y, p0=(y[0], 0.06, 1.0), maxfev=20000, bounds=((-big, 0.005, 0.1), (big, 0.5, 30.0)))
    err = np.sqrt(np.mean((f(t, *p) - y) ** 2)) / np.max(np.abs(y))
    return p, err


REF_TONES = (5, 6, 7, 8, 9)       # the reference vowel: volume 6 at tones around the host's 7 (R4 = E7)


def cmd_fit():
    """Every level re the unit's loud vowel at volume 6 (REF: its RMS, or its mean square for lines and densities),
    which the C host turns into chip units with its own volume-6 vowel (BL_WHINE_VOWEL_RMS)."""
    cells = measure_all()
    g = good(cells)
    sr = 96000
    ref_db = float(np.median([c["vowel_db"] for c in g if c["volume"] == 6 and c["tone"] in REF_TONES]))
    ref = 10 ** (ref_db / 20)
    print("REF: the volume-6 loud vowel, tones %s: %.2f dBFS (RMS %.4f)" % (REF_TONES, ref_db, ref))
    print("vowel re REF by volume:", " ".join("%d:%+.1f" % (v, med([c for c in g if c["volume"] == v], "vowel_db") - ref_db)
                                            for v in range(16)))
    print("hiss (60 Hz-10 kHz) re REF by volume:", " ".join(
        "%d:%+.1f" % (v, med([c for c in g if c["volume"] == v], "hiss_db") - ref_db) for v in range(16)))
    # the click-off: every cell (it does not depend on the volume)
    cw = np.median([c["click_wave"] for c in g], axis=0)
    t = np.arange(len(cw)) * DEC / sr
    (ca, c1, c2), cerr = fit_step(t, cw)
    print("click-off: step %.4f FS, tau %.4f s and %.3f s (rms error %.1f%% of the step)" % (ca, c1, c2, 100 * cerr))
    # the pop, 1 ms on (the first sample is the edge's own slope), up to 0.24 s (the next line's speech starts ~0.27 s)
    pw = np.median([c["pop_wave"] for c in g if "pop_wave" in c], axis=0)
    i0 = int(0.001 * sr) + 1
    t = np.arange(len(pw) - i0) / sr
    (pa, p1, p2, ps, pts), perr = fit_step(t, pw[i0:], spike=True)
    print("pop: step %.4f FS, tau %.4f s and %.3f s, spike %.4f FS decaying %.5f s (rms error %.1f%% of the peak)"
          % (pa, p1, p2, ps, pts, 100 * perr))
    by_vol = {}
    for c in g:
        if "pop_wave" in c:
            by_vol.setdefault(c["volume"], []).append(float(np.median(c["pop_wave"][i0 + 96:i0 + 192])))
    print("pop level 1-2 ms after the edge, by volume (mFS):",
          " ".join("%d:%.0f" % (v, 1000 * np.median(x)) for v, x in sorted(by_vol.items())))
    print("CLICK = (%.4f, %.5f, %.4f)" % (ca / ref, c1, c2))
    print("POP = (%.4f, %.5f, %.4f, %.4f, %.6f)" % (pa / ref, p1, p2, ps / ref, pts))
    # the tick: the open-channel fold over every good cell, 1 ms before its dip to 12 ms after, at 24 kHz
    tk = np.mean([c["tick"] for c in g], axis=0)
    tk = tk - np.mean(tk[:int(0.006 * sr)])
    lo = int(TICK_LO * sr)
    dip = lo - int(0.0015 * sr) + int(np.argmax(np.abs(tk[lo - int(0.0015 * sr):lo + int(0.001 * sr)]) > 0.1 * np.max(np.abs(tk))))
    seg = tk[dip - int(0.001 * sr):dip + int(0.012 * sr)]
    t24 = resample_poly(seg, 1, 4)
    print("tick: peak %.2f mFS; its first edge %.2f ms before the click-off edge's phase" % (
        1000 * np.min(tk), (lo - dip) / sr * 1000))
    print("TICK_LEAD_S = %.5f" % ((lo - dip) / sr + 0.001))
    print("TICK_24K = (" + ", ".join("%.4g" % (v / ref) for v in t24) + ")")
    # the broadband noise: open minus closed density, median over cells, fitted as a plateau through two poles
    d = np.nanmedian([np.maximum(c["psd_open"] - c["psd_off"], 1e-30) for c in g if "psd_open" in c], axis=0)
    use = (THIRDS < 4500) & (np.abs(THIRDS - 63) > 8) & np.isfinite(d) & (d > 1e-29)
    model = lambda f, d0, f1, f2: d0 - 10 * np.log10((1 + (f / f1) ** 2) * (1 + (f / f2) ** 2))
    (d0, f1, f2), _ = curve_fit(model, THIRDS[use], 10 * np.log10(d[use]), p0=(-80, 100, 3000))
    res = 10 * np.log10(d[use]) - model(THIRDS[use], d0, f1, f2)
    print("noise: plateau %.1f dBFS^2/Hz, poles %.0f Hz and %.0f Hz (fit residual rms %.1f dB, 25 Hz-4.5 kHz)" % (
        d0, f1, f2, np.sqrt(np.mean(res ** 2))))
    print("  per third:", " ".join("%.0f:%.1f/%.1f" % (f, 10 * np.log10(v), model(f, d0, f1, f2))
                                   for f, v in zip(THIRDS[use], d[use])))
    print("NOISE = (%.2f, %.1f, %.1f)" % (d0 - ref_db, f1, f2))
    # the comb lines (dB re REF's mean square): per class and tone, the median over the class's volumes 1-15
    table = {}
    for mode, odd in (("hiss", False), ("whine", True)):
        rows = []
        for tone in range(27):
            have = [c["lines"] for c in g if c["tone"] == tone and c["volume"] > 0 and (c["volume"] % 2 == 1) == odd]
            lines = {}
            for n in range(1, 65):
                vals = [h[n] for h in have if n in h]
                if have and len(vals) * 2 >= len(have):
                    lines[n] = float(np.median(vals)) - ref_db
            rows.append(" ".join("%d:%.1f" % (n, d) for n, d in sorted(lines.items())))
        table[mode] = rows
    print("LINES = {")
    for mode in ("hiss", "whine"):
        print('    "%s": (' % mode)
        for tone, r in enumerate(table[mode]):
            print('        "%s",   # tone %d' % (r, tone))
        print("    ),")
    print("}")


LINE = "Readme for Microsoft Windows."
EMU_RATE = 44100


def emulated_line(job):
    """The W03 line through the emulated unit (the NVDA driver's chip and board pole), at volume v and tone `tone`,
    with the host's hiss/whine `mode`: (audio before the driver's make-up gain, the SSI-263 writes, the say time)."""
    v, tone, mode, secs = job
    sys.path.insert(0, os.path.join(REPO, "src"))
    from hosts.native_blazie import NativeBlazie
    from ssi263.native import SSI263C
    params = {"closure_noise_lead_ms": 10.0}
    if mode:
        params["carrier_rel_db"] = -300.0
    chip = SSI263C(params=params, out_rate=EMU_RATE)
    log = []
    eng = os.path.join(REPO, "firmware", "blazie")
    u = NativeBlazie(os.path.join(repo_paths.DIST, "blazie-lib", "x64", "bl.dll"), os.path.join(eng, "BL2ENG.BNS"),
                     os.path.join(eng, "bl2_2003_warm.state"), chip=chip, out_rate=EMU_RATE, board_lowpass_hz=5000.0,
                     on_write=lambda t, r, val: log.append((t, r, val)))
    if mode:
        u.whine = mode
    u.turbo = 1.0                                   # the unit's own pace
    u.send(b"\x05%dV\x05%dT" % (v, tone))
    u.run(1.0)
    del log[:]
    u.say(LINE)
    t0 = u.chip.time
    # in 20 ms blocks, as the app renders (the host decides the idle sound per block, from the chip's state at its end)
    y = np.concatenate([np.asarray(u.run(0.02), dtype=float) for _ in range(int(round(secs / 0.02)))])
    u._drain()
    u.close()
    return y, [(t - t0, r, val) for t, r, val in log], t0


def vowel_db(y, sr):
    y = sosfilt(butter(4, 60, "high", fs=sr, output="sos"), y)
    h = int(0.02 * sr)
    e = np.array([np.mean(y[i:i + h] ** 2) for i in range(0, len(y) - h, h)])
    return float(db10(np.mean(e[e > e.max() * 10 ** -0.6])))


def cmd_emulated():
    """The emulated unit measured as the recordings are: the speech level by volume, the click-off time after the
    acoustic end, and the host's hiss level (absolute, and re the vowel) at volumes 1, 6 and 15."""
    jobs = [(v, 7, None, 4.0) for v in range(1, 16)] + [(v, 7, m, 13.0) for v in (1, 6, 15) for m in ("hiss", "whine")]
    jobs += [(6, t, None, 4.0) for t in REF_TONES if t != 7]
    with ProcessPoolExecutor() as ex:
        res = dict(zip(jobs, ex.map(emulated_line, jobs)))
    sr = EMU_RATE
    ref = [vowel_db(res[(6, t, None, 4.0)][0], sr) for t in REF_TONES]
    print("REF, emulated: the volume-6 loud vowel at tones %s: %s -> median %.2f dB (RMS %.5f)" % (
        REF_TONES, " ".join("%.2f" % r for r in ref), np.median(ref), 10 ** (np.median(ref) / 20)))
    vow = {}
    for v in range(1, 16):
        y, log, _ = res[(v, 7, None, 4.0)]
        vow[v] = vowel_db(y, sr)
        r3 = sorted(set(val for t, r, val in log if r == 3 and val))
        print("volume %2d: R3 %s  vowel %6.1f dB re chip full scale" % (v, " ".join("%02X" % x for x in r3), vow[v]))
    print("vowel re volume 6, emulated:", " ".join("%d:%+.1f" % (v, vow[v] - vow[6]) for v in range(1, 16)))
    band = lambda y: sosfilt(butter(4, 10000, "low", fs=sr, output="sos"), sosfilt(butter(4, 60, "high", fs=sr, output="sos"), y))
    for (v, tone, mode, secs), (y, log, _) in sorted(res.items(), key=lambda kv: (kv[0][2] or "", kv[0][0])):
        if not mode:
            continue
        speech = [t for t, r, val in log if r == 0 and (val & 0x3F)]
        off = [t for t, r, val in log if r == 3 and val == 0]
        # the acoustic end, as for the recordings (300 Hz high-passed 10 ms frames, 15 dB over the hiss)
        y300 = sosfilt(butter(4, 300, "high", fs=sr, output="sos"), y)
        w = int(0.01 * sr)
        idle = y300[int((speech[-1] + 1.0) * sr):int((speech[-1] + 9.0) * sr)]
        thr = np.sqrt(np.mean(idle ** 2)) * 10 ** (15 / 20)
        fr = np.array([np.sqrt(np.mean(y300[i:i + w] ** 2)) for i in range(0, int((speech[-1] + 1.0) * sr), w)])
        end = (np.nonzero(fr > thr)[0][-1] + 1) * w / sr
        hiss = band(y)[int((end + 1.0) * sr):int((end + 9.0) * sr)]
        hdb = 20 * np.log10(np.sqrt(np.mean(hiss ** 2)))
        print("%-5s volume %2d: hiss %6.1f dB (chip scale, 60 Hz-10 kHz), re the vowel %6.1f dB; click-off (R3 = 00) %s s "
              "after the acoustic end, %.3f after the last phoneme load" % (
                  mode, v, hdb, hdb - vow[v], "%.3f" % (off[0] - end) if off else "none in %.1f s" % secs,
                  (off[0] - speech[-1]) if off else float("nan")))


def cmd_excerpt(v, tone, out):
    """The line at volume v, tone `tone`, through its idle and click-off to the next line's pop and first words."""
    for s in reversed(SESSIONS):
        rows = rows_of(s)
        for k, r in enumerate(rows[:-1]):
            if (r["label"].get("trial") == "grid" and int(r["params"]["volume"]) == v and int(r["params"]["tone"]) == tone):
                path = os.path.join(repo_paths.sweep_session(s), "master.wav")
                sr = sf.info(path).samplerate
                x, sr = sf.read(path, start=r["frame_start"] - int(0.3 * sr), stop=rows[k + 1]["frame_start"] + int(2.0 * sr))
                sf.write(out, x, sr, subtype="PCM_24")
                print("wrote %s: %s line %d, %.1f s" % (out, s, k, len(x) / sr))
                return
    raise SystemExit("no cell volume %d tone %d" % (v, tone))


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "measure"
    if cmd == "measure":
        cmd_measure()
    elif cmd == "drift":
        cmd_drift()
    elif cmd == "fit":
        cmd_fit()
    elif cmd == "emulated":
        cmd_emulated()
    elif cmd == "excerpt":
        cmd_excerpt(int(sys.argv[2]), int(sys.argv[3]), sys.argv[4])
    else:
        raise SystemExit(__doc__)
