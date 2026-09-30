"""The SSI-263 alone: write its five registers as a host does, pull its audio.

    chip = Chip(rate=22050)
    chip.write(3, 0x80)          # CTL on
    chip.write(0, 0xC0)          # mode: phoneme timing, A/R on
    chip.write(3, 0x70)          # CTL off, articulation 7, amplitude 0 ... set R1-R4, then phonemes into R0
    pcm = chip.run(0.5)          # 16-bit mono PCM (bytes)

`say_phonemes` does the whole sequence for a list of phoneme codes (0-63), waiting on the chip's A/R request between
them as a real host does.
"""
import ctypes

from . import _native

# the SSI-263's 64 phonemes by code
PHONEMES = ["PA", "E", "E1", "Y", "YI", "AY", "IE", "I", "A", "AI", "EH", "EH1", "AE", "AE1", "AH", "AH1",
            "AW", "O", "OU", "OO", "IU", "IU1", "U", "U1", "UH", "UH1", "UH2", "UH3", "ER", "R", "R1", "R2",
            "L", "L1", "LF", "W", "B", "D", "KV", "P", "T", "K", "HV", "HVC", "HF", "HFC", "HN", "Z",
            "S", "J", "SCH", "V", "F", "THV", "TH", "M", "N", "NG", ":A", ":OH", ":U", ":UH", "E2", "LB"]


class Chip:
    """One emulated SSI-263 with its default parameters, rendering at `rate` Hz."""

    def __init__(self, rate=22050, gain=2.0):
        self._lib = _native.chip_lib()
        params = ctypes.create_string_buffer(self._lib.ssi263_params_size())
        self._lib.ssi263_default_params(params)
        self._c = self._lib.ssi263_new(params, self._lib.ssi263_default_rom(), float(rate))
        if not self._c:
            raise RuntimeError("ssi263speech: could not create the chip")
        self.rate = rate
        self.gain = gain

    def close(self):
        if self._c:
            self._lib.ssi263_free(self._c)
            self._c = None

    def __del__(self):
        self.close()

    def write(self, reg, value):
        """Writes register 0-4 (the bus's RS2..RS0: 0-3 select R0-R3, 4-7 select R4)."""
        self._lib.ssi263_write(self._c, int(reg), int(value) & 0xFF)

    def reg(self, i):
        return self._lib.ssi263_reg(self._c, int(i))

    @property
    def request(self):
        """The A/R request: the chip wants its next phoneme."""
        return bool(self._lib.ssi263_request(self._c))

    @property
    def time(self):
        return self._lib.ssi263_time(self._c)

    def _pcm(self, buf, n):
        out = (ctypes.c_short * max(1, n))()
        if n:
            self._lib.ssi_pcm16(buf, n, self.gain, out)
        return bytes(out)[:2 * n]

    def run(self, seconds):
        """`seconds` of audio as 16-bit mono PCM (bytes)."""
        n = int(seconds * self.rate)
        buf = (ctypes.c_double * max(1, n))()
        got = self._lib.ssi263_run(self._c, n, buf)
        return self._pcm(buf, got)

    def run_until_request(self, limit):
        """Audio until the chip asks for its next phoneme, at most `limit` seconds."""
        n = int(limit * self.rate)
        buf = (ctypes.c_double * max(1, n))()
        got = self._lib.ssi263_run_until_request(self._c, n, buf)
        return self._pcm(buf, got)

    def say_phonemes(self, codes, duration=0, inflection=0x40, rate=0xA8, articulation=7, amplitude=0x0C,
                     filter_freq=0xE7, tail=0.3):
        """Speaks phoneme codes (0-63, or names from PHONEMES) and returns the PCM.  Register values: R0's duration
        bits (0-3), R1 inflection, R2 rate/inflection, R3's articulation (0-7) and amplitude (0-15), R4 filter."""
        out = []
        self.write(3, 0x80)                        # CTL: the next R0 write sets the mode
        self.write(0, 0xC0)                        # phoneme timing, transitioned inflection, A/R on
        self.write(1, inflection)
        self.write(2, rate)
        self.write(4, filter_freq)
        self.write(3, (articulation & 7) << 4 | (amplitude & 15))
        for c in codes:
            code = PHONEMES.index(c) if isinstance(c, str) else int(c)
            self.write(0, (duration & 3) << 6 | (code & 0x3F))
            out.append(self.run(0.001))
            out.append(self.run_until_request(2.0))
        self.write(0, 0x00)                        # PA
        out.append(self.run(tail))
        return b"".join(out)
