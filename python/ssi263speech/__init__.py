"""ssi263speech: the emulated Votrax SSI-263 speech chip, and the Blazie Braille Lite that drives it, from Python.

    from ssi263speech import Chip, BrailleLite, write_wav
    write_wav("hello.wav", Chip().say_phonemes(["HF", "EH", "L", "OU"]), 22050)

The native library does the work (the same C the NVDA add-ons, the speech-dispatcher module and the Android engine
use); this package only calls it.
"""
import wave

from ._version import __version__
from .chip import PHONEMES, Chip
from .voice import BrailleLite

__all__ = ["__version__", "Chip", "BrailleLite", "PHONEMES", "write_wav"]


def write_wav(path, pcm, rate):
    """16-bit mono PCM (bytes) to a WAV file."""
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
