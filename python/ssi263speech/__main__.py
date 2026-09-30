"""python -m ssi263speech --phonemes HF EH L OU --out hello.wav
   python -m ssi263speech --firmware FOLDER --say "Hello there." --out hello.wav [--spanish]
"""
import argparse
import sys

from . import BrailleLite, Chip, write_wav, __version__


def main():
    ap = argparse.ArgumentParser(prog="python -m ssi263speech", description="The emulated SSI-263 speech chip.")
    ap.add_argument("--phonemes", nargs="+", help="phoneme names or codes, spoken by the chip alone")
    ap.add_argument("--say", help="text, spoken by an emulated Braille Lite (needs --firmware)")
    ap.add_argument("--firmware", help="the folder with the Braille Lite's firmware and state (not included)")
    ap.add_argument("--spanish", action="store_true")
    ap.add_argument("--rate", type=int, default=22050, help="sample rate (Hz)")
    ap.add_argument("--out", required=True, help="the WAV file to write")
    ap.add_argument("--version", action="version", version=__version__)
    a = ap.parse_args()
    if a.say:
        if not a.firmware:
            ap.error("--say needs --firmware (the firmware is not included)")
        pcm = BrailleLite(a.firmware, "es" if a.spanish else "en", rate=a.rate).speak(a.say)
    elif a.phonemes:
        pcm = Chip(rate=a.rate).say_phonemes([int(p) if p.isdigit() else p for p in a.phonemes])
    else:
        ap.error("give --phonemes or --say")
    write_wav(a.out, pcm, a.rate)
    print("%s: %.2f s" % (a.out, len(pcm) / 2 / a.rate))
    return 0


if __name__ == "__main__":
    sys.exit(main())
