"""Text to speech through an emulated Blazie Braille Lite: its own firmware turns the text into phonemes and drives
the SSI-263, exactly as in the NVDA add-on and the speech-dispatcher module (the same C, bl_voice).

The firmware is not included (it is Blazie's): pass the folder holding BL2ENG.BNS + bl2_2003_warm.state (English)
or BL2SPA.BNS + bl2spa_fresh.state (Spanish) -- for instance the NVDA add-on's engine folder.

    v = BrailleLite("path/to/firmware", rate=22050)
    pcm = v.speak("Hello there.")                    # 16-bit mono PCM (bytes)
"""
import ctypes
import os

from . import _native

FILES = {"en": ("BL2ENG.BNS", "bl2_2003_warm.state"), "es": ("BL2SPA.BNS", "bl2spa_fresh.state")}


class BrailleLite:
    """One Braille Lite unit booted as the screen-reader drivers boot it.  rate: the output sample rate (Hz)."""

    def __init__(self, firmware_dir, language="en", rate=22050, inflection=True, idle_noise=0):
        fw, st = FILES[language]
        paths = [os.path.join(firmware_dir, fw), os.path.join(firmware_dir, st)]
        for p in paths:
            if not os.path.isfile(p):
                raise FileNotFoundError("ssi263speech: %s not found (the firmware is not included)" % p)
        self._lib = _native.voice_lib()
        err = ctypes.create_string_buffer(256)
        enc = "mbcs" if os.name == "nt" else "utf-8"
        self._v = self._lib.blv_create(paths[0].encode(enc), paths[1].encode(enc), 1 if language == "es" else 0,
                                       float(rate), 1 if inflection else 0, int(idle_noise), err, 256)
        if not self._v:
            raise RuntimeError("ssi263speech: the unit did not boot: %s" % err.value.decode("latin-1", "replace"))
        self.rate = rate
        self.set()

    def close(self):
        if self._v:
            self._lib.blv_destroy(self._v)
            self._v = None

    def __del__(self):
        self.close()

    def set(self, rate=50, pitch=50, tone=7, volume=100, short_pauses=True):
        """NVDA's scales: rate, pitch and volume 0-100 (50, 50 = the unit's factory rate and pitch); tone 0-26."""
        self._lib.blv_set(self._v, int(rate), int(pitch), int(tone), int(volume), 1 if short_pauses else 0)

    def speak(self, text):
        """The whole utterance as 16-bit mono PCM (bytes)."""
        return b"".join(self.stream(text))

    def stream(self, text):
        """The utterance block by block, as the unit speaks it."""
        self._lib.blv_speak(self._v, text.encode("utf-8"))
        pcm, done = ctypes.POINTER(ctypes.c_short)(), ctypes.c_int(0)
        while not done.value:
            n = self._lib.blv_render(self._v, ctypes.byref(pcm), ctypes.byref(done))
            if n:
                yield ctypes.string_at(pcm, 2 * n)

    def cancel(self):
        self._lib.blv_cancel(self._v)
