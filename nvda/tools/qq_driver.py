"""Stacked question marks through the real Braille Lite driver (stand-in NVDA): the R1 the chip gets for '?', '??',
'???' as NVDA sends them (Tomi's log: 'hi, how are you??\\r\\n', the line break included), and a wav for listening
(D:\\downloads\\sbs\\stacked_questions.wav).

    python qq_driver.py
"""
import os
import sys
import time

sys.argv = [sys.argv[0], "blazie"]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
          encoding="utf-8").read().split("time.sleep(2.0)")[0])
time.sleep(1.5)
import soundfile as sf  # noqa: E402

chip = d._unit.chip
seq = []
orig = chip.write


def spy(reg, val):
    orig(reg, val)
    if reg == 1:
        seq.append(val)


chip.write = spy
audio = []
if len(sys.argv) > 2 or os.environ.get("QQ_RATE"):
    d._set_rate(int(os.environ.get("QQ_RATE", "50")))
CRLF = chr(13) + chr(10)
for text in ("hi, how are you?" + CRLF, "hi, how are you??" + CRLF, "hi, how are you???" + CRLF, "hi, how are you??"):
    del seq[:]
    mark = len(notified)
    n0 = len(d._player.chunks)
    d.speak([text])
    wait_idle()
    out = [v for k, v in enumerate(seq) if k == 0 or seq[k - 1] != v]
    print("%-26r R1: %s" % (text, " ".join("%02X" % v for v in out)))
    audio += list(d._player.chunks[n0:]) + [np.zeros(int(0.6 * d._out_rate))]
sf.write(os.environ.get("QQ_OUT", "stacked_questions_rate%s.wav" % os.environ.get("QQ_RATE", "50")), np.concatenate(audio), d._out_rate, subtype="PCM_16")
d.terminate()
