"""Blazie firmware (Braille Lite 2000, June 2003 BL2ENG) driving the engine's SSI-263.

The firmware runs in z180emu (`bns_live.exe --live`, GPLv2) as a child process; this
host runs the SSI-263 model and closes the loop: the unit's register writes arrive
as "W reg val" lines and are applied to the chip at the current chip time, and the
chip's A/R request goes back as "A 1" / "A 0", so the firmware's inflection tokens
and phoneme timing happen live instead of being replayed from a batch.

Neither the firmware nor the RAM snapshot (which contains it) is part of any
repository: pass your own paths.  Boot = the warm snapshot plus the speech-box key
chords (345, 123456, L, e), exactly as the MASTER harness did.
"""
import os
import subprocess
import sys

# Packaged (the NVDA add-ons: synthDrivers._ssi263_<name>), the engine is imported relatively,
# never from sys.path: both add-ons ship an `ssi263`, and NVDA keeps one module per name for
# the whole process (0.3.0: Braille Lite imported the older Speak-Out's copy and failed).
try:
    from .ssi263 import SSI263
except ImportError:                       # the research tree: src/ on sys.path
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from ssi263 import SSI263  # noqa: E402

CLOCK_HZ = 6144000.0
CHORDS = ["8000000=5C", "18000000=7F", "28000000=07", "38000000=51"]   # 345, 123456, L, e
BOOT_INSTR = 48000000
KEY_GAP = 10000000                 # instructions between boot keys (1.6 s of unit time)
# Speech-menu letters (BL2000 help, 345-chord menu), as braille key codes: bit n-1 = dot n.
# Punctuation t/m/s/z = total/most/some/none; n toggles digits / full numbers.
MENU = {"punct_total": 0x1E, "punct_most": 0x0D, "punct_some": 0x0E, "punct_none": 0x35,
        "numbers_toggle": 0x1D}
# Status-menu entries (BL2000 help, 34-chord menu: "Voice inflection: i y/n"): the letter that jumps to the
# entry, then y (dots 13456) or n (dots 1345).  The e-chord leaves the menu.
STATUS = {"inflection_on": (0x0A, 0x3D), "inflection_off": (0x0A, 0x1D)}
CREATE_NO_WINDOW = 0x08000000


def boot_keys(menu=(), start=None, gap=None, status=()):
    """The boot chords, with `menu` letters typed inside the 345-chord speech menu before
    the 123456-chord enters speech-box mode.  Returns (--key args, boot instructions).
    `start` (instructions before the first chord) and `gap` (between keys, and after the
    last) default to the MASTER harness's 8M and 10M."""
    if not menu and not status and start is None and gap is None:
        return list(CHORDS), BOOT_INSTR
    start = 8000000 if start is None else int(start)
    gap = KEY_GAP if gap is None else int(gap)
    codes = ([0x4C] + [c for st in status for c in STATUS[st]] + [0x51] if status else [])   # 34-chord .. e-chord
    codes += [0x5C] + [MENU[m] for m in menu] + [0x7F, 0x07, 0x51]
    keys = ["%d=%02X" % (start + i * gap, c) for i, c in enumerate(codes)]
    return keys, start + len(codes) * gap


# ---- the optional idle whine, generated from the chip's own clock (Tomi's unit, blite_sweep W03, 2026-09-27) ----
# On the unit a faint whine rides under the speech and the pauses until the click-off.  Its lines sit at n * fc/64
# and obey one rule at every tone (404 of 406 measured lines): a mod-64 counter steps by N = 32 - tone on each
# filter-clock tick, and a fixed 64-value function g of its state leaks out.  g is fitted once (its DFT magnitude
# per component m, below) with a smooth output path (a 2nd-order roll-off at 4 kHz); then any tone's lines follow
# from the counter.  Blind test: fitted on half the odd tones, it predicts the other half's lines to a median 1.9 dB.
# Two classes, as Tomi hears them on the unit: EVEN volumes (6, the factory volume this host uses) give a slight hiss,
# ODD volumes the actual whine, about 20 dB louder.  Each is one fitted waveform plus an output path: the hiss from
# the volume-6 row (median of each component; 2nd order at 4 kHz; blind on half the odd tones: 1.9 dB median), the
# whine from the volume-7 row (75th percentile, since the loud lines are the ones heard; 2nd order at 7 kHz; blind on
# the audible lines, >= -70 dB: 2.2 dB median, 90 % within 10.6 dB).  g's PHASES were not measured: a pseudo-random
# set (WHINE_SEED) chosen to fit the tones where the counter visits only some states (0, 4, ... 24); blind on 2, 6,
# ... 26 the levels are within ~8 dB (blite_sweep analysis/whine_counter.py, whine_phases.py).  The chip's own
# two-sine carrier (carrier_rel_db) is switched off while this runs, since the model carries those lines.
# An empirical model of the recorded output, not a traced circuit.
WHINE_MODELS = {       # class: (|G(m)| dB re a loud vowel's mean square, m = 0..32; roll-off order; corner Hz)
    "hiss": ((None, -76.7, -72.5, -73.3, -74.8, -75.1, -78.1, -81.5, -62.6, -82.7, -68.5, None, None, None, -81.6,
              -82.6, -67.7, -85.1, -83.4, None, None, -58.8, -79.9, -81.6, -64.4, -80.3, -78.9, None, None, None,
              -77.7, -78.7, -65.9), 2, 4000.0),
    "whine": ((None, -75.9, -59.9, -72.1, -70.2, -63.7, -65.1, -71.8, -52.5, -72.0, -65.9, -74.0, -75.1, -72.0, -70.2,
               -77.1, -65.1, -68.4, -68.4, -67.8, -72.1, -72.8, -68.4, -73.9, -57.6, -75.1, -66.9, -73.6, -77.0, -74.4,
               -66.9, -74.9, -72.1), 2, 7000.0),
}
WHINE_VOWEL_RMS = 0.0426                 # this host's loud vowel at unit volume 6, after the board pole
WHINE_TABLE = 1024
WHINE_SEED = 392


def whine_wave(r4, out_rate, mode="whine", xck=1e6):
    """One 64-tick period of the hiss or whine at tone register r4, band-limited under 0.45 x out_rate: (fc, table)."""
    g_db, order, corner = WHINE_MODELS[mode]
    import cmath
    import math
    n_div = 256 - r4
    if n_div <= 0:
        return None, None
    fc = xck / (2.0 * n_div)
    seed, ph = WHINE_SEED, []
    for m in range(33):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        ph.append(2 * math.pi * seed / 0x7FFFFFFF)
    amp = [0.0 if (m == 0 or g is None) else math.sqrt(2.0) * 10 ** (g / 20.0) for m, g in enumerate(g_db)]
    g64 = [sum(amp[m] * math.cos(2 * math.pi * m * s / 64.0 + ph[m]) for m in range(1, 33)) for s in range(64)]
    x = [g64[(k * n_div) % 64] for k in range(64)]
    lines = []
    for n in range(1, 64):
        f = n * fc / 64.0
        if f >= 0.45 * out_rate:
            break
        X = sum(x[k] * cmath.exp(-2j * math.pi * n * k / 64.0) for k in range(64))
        a = abs(X) / 64.0 * (1.0 if n == 32 else 2.0)
        if a <= 0.0:
            continue
        h = (1.0 + (f / corner) ** 2) ** (-order / 2.0)
        lines.append((n, a * h * WHINE_VOWEL_RMS, cmath.phase(X)))
    tab = [sum(a * math.cos(2 * math.pi * n * j / WHINE_TABLE + p) for n, a, p in lines) for j in range(WHINE_TABLE)]
    return fc, tab


class Blazie:
    def __init__(self, exe, firmware, state, chip=None, out_rate=44100, menu=(), key_start=None, key_gap=None,
                 board_lowpass_hz=None, status=()):
        """`board_lowpass_hz`: a roll-off after the chip that matches the unit's line out (None: off).
        The unit's line out has ~3 dB less at 4-8 kHz and ~12 dB less at 8-16 kHz than the chip
        model; a first-order 5 kHz low-pass matches it (octave error 4.6 -> 1.2 dB on Reclaim's
        first sentence; Tomi's ear, 2026-09-27).  Fitted, not traced: it may compensate for the
        board's output stage, the headphone socket, the recording chain or chip-model error (the Artic sample
        schematic's AO network corners near 224 Hz, so it is no support).  Not the chip model."""
        self.chip = chip or SSI263(out_rate=out_rate)
        self.board = self.chip.dsp.onepole(board_lowpass_hz, self.chip.out_rate) if board_lowpass_hz else None
        self.whine = None            # None, "hiss" or "whine" (whine_wave); the driver sets it
        self.encoding = "latin-1"    # how say() sends text: the Spanish firmware reads DOS code page 850
        # cancel(): the emulated time between ^X cuts.  Each cut costs pipe round trips: 20 ms cuts (0.5.0) made
        # the cancel ~32 ms of a ~44 ms tab-to-speech; 100 ms gives ~20 ms, no tail left in 24 cancel points
        # (nvda/tools/cut_test.py; 150 and 200 ms are slower again)
        self.cancel_cut = 0.1
        self._whine_key, self._whine_fc, self._whine_tab, self._whine_ph = None, None, None, 0.0
        args = [exe, firmware, "--live", "--state-in", state, "--phon-ms", "5"]
        keys, boot_instr = boot_keys(menu, key_start, key_gap, status)
        for c in keys:
            args += ["--key", c]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                     creationflags=CREATE_NO_WINDOW if os.name == "nt" else 0,
                                     cwd=os.path.dirname(os.path.abspath(exe)))
        self.ar = None
        self.tx = []               # bytes the unit sent back (XON/XOFF, ^F markers)
        # ^F bookkeeping.  The unit echoes each ^F in speaking order, but it holds the
        # last line it received (an empty flush line, CR ^F) until the next arrives, so
        # when everything sent has been spoken, exactly one ^F is still owed.
        self.sent_f = 0
        self.echo_f = 0
        self.say_time = 0.0
        self.preparing = False     # a line sent and no phoneme of it spoken yet
        self.turbo = 4.0           # CPU speed-up while preparing (the unit reads a whole line first)
        # also re-arm the turbo while the unit reads each NEXT line of a multi-line send.
        # Off = the unit's own pace: CPU speed is audible in its timing (a line at x4 is 10%
        # shorter, measured), so this is a "shorter pauses" option, not a transparent one.
        self.turbo_between_lines = False
        self.prep_step = None      # a coarser lockstep while preparing (None: the run() step)
        self._stale_f = 0          # ^F echoes owed by earlier say()s (the held flush line)
        self.last_speech = -1.0
        self.feed_chip = False     # during boot, writes set chip state but make no audio
        self._cmd("B %d" % boot_instr)
        self._cmd("LIVE")
        self.feed_chip = True

    # ---- protocol ------------------------------------------------------------------
    def _cmd(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        while True:
            r = self.proc.stdout.readline()
            if not r:
                raise RuntimeError("z180emu exited")
            if r.startswith("OK"):
                return
            if r.startswith("W "):
                _, reg, val = r.split()
                reg, val = int(reg), int(val, 16)
                self.chip.write(reg, val)
                if reg == 0 and not (self.chip.regs[3] & 0x80) and (val & 0x3F):
                    self.last_speech = self.chip.time        # PA (code 00) is not speech
                    self.preparing = False
            elif r.startswith("T "):
                b = int(r.split()[1], 16)
                self.tx.append(b)
                if b == 0x06:
                    self.echo_f += 1
                    self.say_time = self.chip.time           # progress: patience counts from here
                    if self._stale_f > 0:
                        # the held flush line of an EARLIER say(), answered now that new text
                        # came -- often after this line's first phoneme, so it must not re-arm
                        self._stale_f -= 1
                    elif self.turbo_between_lines and self.owed() > 0:
                        self.preparing = True                # a line is done; the next is being read
            elif r.startswith("DROPPED "):
                self.sent_f -= int(r.split()[1])
            # anything else is the emulator core's own chatter

    def close(self):
        try:
            self._cmd("Q")
        except Exception:
            pass
        try:
            self.proc.kill()
        except Exception:
            pass

    # ---- host ----------------------------------------------------------------------
    def send(self, data):
        if isinstance(data, str):
            data = data.encode("latin-1", "replace")
        if data:
            self.sent_f += data.count(0x06)
            self._cmd("S " + data.hex())

    def say(self, text):
        """A line (or a list of lines, sent together so the unit moves from one to the next
        by itself), each ended CR ^F, then the flush the unit needs: it speaks a line when
        the next arrives.  Each ^F echo comes once its line's last phoneme is sent; the
        flush line's only when a later transmission arrives, so that one is not waited for."""
        lines = [text] if isinstance(text, str) else list(text)
        self.say_time = self.chip.time
        self.preparing = True
        self._stale_f = max(0, self.sent_f - self.echo_f)     # echoes still due from earlier sends
        self.send(b"".join(ln.encode(self.encoding, "replace") + b"\r\x06" for ln in lines) + b"\r\x06")

    def owed(self):
        """^F echoes still to come beyond the one the unit holds (after a cancel it
        holds none until the next line, so this can read -1: nothing owed)."""
        return self.sent_f - 1 - self.echo_f

    def busy(self, quiet=0.1, patience=3.0):
        """Still speaking: a phoneme within `quiet` s, or a line's ^F echo still owed.
        `patience` bounds only SILENT waiting for that echo, counted from the last speech
        or echo.  Counted from the last echo alone (0.3.4 and before), one line longer
        than it -- 8.4 s, a spelled-out Mastodon handle -- ended the utterance just before
        its echo, and the unit's remaining lines came out only with the next utterance."""
        if (self.chip.time - self.last_speech) < quiet:
            return True
        return self.owed() > 0 and (self.chip.time - max(self.say_time, self.last_speech)) < patience

    def cancel(self, limit=3.0, quiet=0.15, cut=None):
        """Silence and flush.  ^X cuts the word being spoken and flushes the unit's
        input, held flush line and its ^F included; the unit may still move on to a
        word it had already prepared, so keep cutting (every `cancel_cut` s), audio discarded,
        until nothing has loaded for `quiet` s.  Afterwards nothing is held, so every
        ^F sent counts as answered -- unless every line had already echoed: then the unit
        holds its flush line, ^X leaves it held (measured), and its ^F comes with the next
        text.  Counted as answered (0.5.0), that ^F was taken for the next line's own
        echo, and the next utterance ended at its first word gap longer than `quiet`,
        the rest held until the one after (a tester: at NVDA's slower rates, "the last
        syllable is cut off, then joined to the next utterance").  Returns the emulated
        seconds it took."""
        holding = self.owed() == 0              # every line echoed: the flush line is held
        self._cmd("D")                          # drop what the unit has not taken yet
        self.preparing = False
        t = 0.0
        cut = self.cancel_cut if cut is None else cut
        while t < limit:
            self._cmd("U 18")
            t += self.skip(cut)
            # a line still being prepared ignores ^X and speaks afterwards: until its ^F is
            # back (or ^X flushed it), keep cutting for up to 1 s, the longest measured wait
            if (t >= 0.04 and self.chip.time - self.last_speech > quiet
                    and (self.owed() <= 0 or t >= 1.0)):
                break
        self.echo_f = self.sent_f - 1 if holding else self.sent_f
        return t

    def skip(self, seconds):
        """Fast-forward with no sound: the chip only keeps time and A/R."""
        t = 0.0
        while t < seconds - 1e-9:
            if self.chip.request != self.ar:
                self.ar = self.chip.request
                self._cmd("A %d" % (1 if self.ar else 0))
            before = self.chip.time
            self.chip.skip(seconds - t)
            dt = max(self.chip.time - before, 1e-5)
            self._cmd("R %d" % max(1, int(CLOCK_HZ * dt)))
            t += dt
        return t

    def run(self, seconds, step=0.0005):
        """`step` is the lockstep grain: chip and CPU trade A/R and writes every step.  While
        the unit silently reads a line (`preparing`), `prep_step` may be coarser -- each step
        is one pipe round trip, and at a high turbo those dominate (tools/latency_check.py)."""
        out, t = [], 0.0
        while t < seconds:
            if self.chip.request != self.ar:
                self.ar = self.chip.request
                self._cmd("A %d" % (1 if self.ar else 0))
            before = self.chip.time
            st = self.prep_step if (self.preparing and self.prep_step) else step
            y = self.chip.run_until_request(st) if not self.chip.request else self.chip.run(st / 4)
            out.append(y)
            dt = max(self.chip.time - before, 1e-5)
            speed = self.turbo if self.preparing else 1.0
            self._cmd("R %d" % max(1, int(CLOCK_HZ * dt * speed)))
            t += dt
        y = self.chip.dsp.concat(out)
        y = self.board.process(y) if self.board else y
        return self._add_whine(y) if self.whine else y

    def _add_whine(self, y):
        """The whine under this block, phase-continuous; silent while the chip is powered down or clicked off
        (R3 = 00: on the unit everything, whine included, stops there)."""
        regs = self.chip.regs
        rate = self.chip.out_rate
        key = (regs[4], rate, self.whine)
        if key != self._whine_key:
            self._whine_key = key
            self._whine_fc, self._whine_tab = whine_wave(regs[4], rate, self.whine)
        if self._whine_tab is None or not len(y):
            return y
        inc = self._whine_fc / 64.0 / rate
        ph = self._whine_ph
        if (regs[3] & 0x80) or not (regs[3] & 0x70):
            self._whine_ph = (ph + inc * len(y)) % 1.0
            return y
        tab, size = self._whine_tab, WHINE_TABLE
        out = self.chip.dsp.concat([y])
        for i in range(len(out)):
            pos = ph * size
            j = int(pos)
            fr = pos - j
            out[i] += tab[j % size] * (1.0 - fr) + tab[(j + 1) % size] * fr
            ph += inc
            if ph >= 1.0:
                ph -= 1.0
        self._whine_ph = ph
        return out
