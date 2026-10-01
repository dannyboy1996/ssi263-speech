# ssi263speech

The emulated Votrax SSI-263 speech chip -- and a Blazie Braille Lite that drives it with its own firmware -- from
Python. The same native library as the NVDA add-ons, the Linux speech-dispatcher module and the Android engine; this
package only calls it (ctypes), so one wheel serves every Python 3 on its platform.

```python
from ssi263speech import Chip, BrailleLite, write_wav

# the chip alone: phonemes in, audio out
write_wav("hello.wav", Chip(rate=22050).say_phonemes(["HF", "EH", "L", "OU"]), 22050)

# text through the Braille Lite's firmware (not included: point it at your own copy,
# e.g. the NVDA add-on's engine folder with BL2ENG.BNS and bl2_2003_warm.state)
v = BrailleLite("path/to/firmware", rate=22050)
write_wav("hello.wav", v.speak("Hello there."), 22050)
```

Or from the command line:

    python -m ssi263speech --phonemes HF EH L OU --out hello.wav
    python -m ssi263speech --firmware path/to/firmware --say "Hello there." --out hello.wav

`Chip` writes the five registers as a host does (`write`, `reg`, `request` for the A/R line, `run`,
`run_until_request`); `BrailleLite` takes NVDA's scales for rate, pitch, volume and tone (`set`), and `speak` or
`stream`s an utterance as 16-bit mono PCM.

Wheels: Windows (64- and 32-bit) and Linux (x86_64, aarch64), attached to the GitHub releases. Licence: MIT
(with Casso's MIT notice, which the chip model draws on), except MAME's Z180 core inside the Braille Lite's board,
which keeps its BSD-3-Clause licence; all three notices are in the wheel.
