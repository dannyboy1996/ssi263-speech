# -*- coding: utf-8 -*-
"""The output sample rates the drivers offer (NVDA's "Sample rate" combo box), shared by all three.

22 kHz by default: the chip's filters are clocked at 1 MHz / (2 (32 - tone)), 20 kHz at tone 7 and 83 kHz at
tone 26, and 22050 keeps the voice to 9.9 kHz (at tone 26, Tomi: "I could hear something with 'synthesizer' in
the 22K one, I could not understand a word at the 11K one").  44 kHz keeps what lies above: the voice imaged
around the filter clock at low tones (measured on the unit's line out, W01) and the top tones' S/Z hiss (a
listener, Ray).  11 kHz: the sound of the unit's own speaker at low tones.  The chip model renders at the rate
directly: below 40 kHz its anti-alias low-pass sits at 90 % of the host Nyquist (ssi263/chip.py)."""

RATES = (11025, 22050, 44100)
DEFAULT = 22050
LABELS = {11025: "11 kHz", 22050: "22 kHz (default)", 44100: "44 kHz"}
SETTING_ID = "sampleRate"
SETTING_LABEL = "Sample &rate"


def parse(value):
    """An NVDA setting value ("22050") as a rate, or None if it is not one we offer."""
    try:
        rate = int(value)
    except (TypeError, ValueError):
        return None
    return rate if rate in RATES else None


def saved(driver_name):
    """The rate saved in NVDA's config for this driver, so the first boot already uses it
    (NVDA applies saved settings only after the driver has started its worker).  A 0.6.0 test build's
    "Higher sample rate" check box, unchecked, still means 11 kHz."""
    try:
        import config
        section = config.conf["speech"][driver_name]
    except Exception:
        return DEFAULT
    try:
        rate = parse(section[SETTING_ID])
        if rate:
            return rate
    except Exception:
        pass
    try:
        if str(section["higherSampleRate"]).strip().lower() in ("false", "0", "no", "off"):
            return 11025
    except Exception:
        pass
    return DEFAULT
