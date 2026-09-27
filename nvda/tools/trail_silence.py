"""How much silence each utterance ends with, from the last audible sample to the end of what
the driver feeds before it calls idle().  NVDA 2024+ idle() = wait until the device clock
says the fed audio has played, then Stop and Reset the stream: whatever the clock counted
as played but the listener had not yet heard is lost with the Reset.  A synth that ends on
silence loses only silence.

    python nvda/tools/trail_silence.py speakout|blazie|accent
"""
import os
import sys
import time

WHICH = sys.argv[1]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_nvda_driver_test.py"),
          encoding="utf-8").read().split("time.sleep(2.0)")[0])
time.sleep(2.0)
THR = 10 ** (-40 / 20)
for rate in (50, 94, 95, 100):
    d._set_rate(rate)
    for text in ("ninety five", "Rate", "The quick brown fox jumps over the lazy dog."):
        mark = len(notified)
        n0 = len(d._player.chunks)
        d.speak([text])
        wait_idle()
        y = audio_since(n0)
        loud = np.nonzero(np.abs(y) > THR * np.max(np.abs(y)))[0] if len(y) else []
        tail = (len(y) - 1 - loud[-1]) / 44.1 if len(loud) else float("nan")
        print("rate %3d  %-46r %5.2f s audio, ends with %4.0f ms below -40 dB" % (rate, text, len(y) / 44100, tail))
d.terminate()
