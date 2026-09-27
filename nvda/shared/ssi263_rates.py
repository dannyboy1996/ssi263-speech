# -*- coding: utf-8 -*-
"""The output sample rate the drivers offer (NVDA's "Higher sample rate" check box), shared by all three.

11 kHz by default: what most people remember from the unit's own speaker.  Checked, 22 kHz: the chip's filters
are clocked at 1 MHz / (2 (32 - tone)), 20 kHz at tone 7, so its own band ends near 10 kHz and 22050 keeps it to
9.9 kHz; Tomi's A/B found no new highs at 44.1 kHz, so it is not offered.  The chip model renders at the rate
directly: below 40 kHz its anti-alias low-pass sits at 90 % of the host Nyquist (ssi263/chip.py)."""

LOW, HIGH = 11025, 22050
SETTING_ID = "higherSampleRate"
SETTING_LABEL = "Higher sample &rate (22 kHz instead of 11 kHz)"


def rate(higher):
    return HIGH if higher else LOW


def parse(value):
    """An NVDA setting value (a bool, or "True"/"False" from an older config) as a bool."""
    if isinstance(value, str):
        return value.strip().lower() in ("true", "1", "yes", "on")
    return bool(value)


def saved(driver_name):
    """The check box saved in NVDA's config for this driver, so the first boot already uses its rate
    (NVDA applies saved settings only after the driver has started its worker)."""
    try:
        import config
        return parse(config.conf["speech"][driver_name][SETTING_ID])
    except Exception:
        return False
