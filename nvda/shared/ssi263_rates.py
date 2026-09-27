# -*- coding: utf-8 -*-
"""The output sample rates the drivers offer (NVDA's "Sample rate" combo box), shared by all three.

The chip model renders at the chosen rate directly: below 40 kHz its anti-alias low-pass sits at
90 % of the host Nyquist (ssi263/chip.py), so 11 and 22 kHz lose only the top of the band."""

RATES = (11025, 22050, 44100)
# 22 kHz by default (Tomi): the chip's filters are clocked at 1 MHz / (2 (32 - tone)), 20 kHz at
# tone 7, so its own band ends near 10 kHz, and 22050 keeps it to 9.9 kHz.  11 kHz stops near the
# Blazie board's 5 kHz roll-off: duller than the unit, for those who want it.
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
    (NVDA applies saved settings only after the driver has started its worker)."""
    try:
        import config
        return parse(config.conf["speech"][driver_name][SETTING_ID]) or DEFAULT
    except Exception:
        return DEFAULT
