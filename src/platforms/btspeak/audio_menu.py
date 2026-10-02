"""Accessible sound controls, applied while the firmware worker is paused."""

from dataclasses import replace
from gettext import gettext as _
from types import ModuleType

from audio_options import RATES
from preferences import Preferences
from profiles import BY_KEY
from worker import Worker, WorkerError


def audio_menu(worker: Worker, dialogs: ModuleType, preferences: Preferences) -> None:
    buffers = {"auto": _("Automatic (starts at 60 ms, increases if needed)"),
               "medium": _("Medium (100 ms)"), "long": _("Long (250 ms)")}
    idle = {"0": _("Silent"), "1": _("Hiss"), "2": _("Whine"), "3": _("Original unit behavior")}
    keep = {"0": _("During speech only"), "1": _("Until the firmware switches off"), "2": _("Always")}
    rates = {str(rate): _("{rate} Hz").format(rate=rate) for rate in RATES}
    while True:
        current = preferences.audio
        choices = {
            "b": _("Sound buffer: {value}").format(value=buffers[current.buffer]),
            "r": _("Sample rate: {rate} Hz").format(rate=current.rate),
        }
        if BY_KEY[preferences.unit].kind != "tns":
            choices.update({
                "i": _("Idle sound: {value}").format(value=idle[str(current.idle)]),
                "c": _("Keep channel open: {value}").format(value=keep[str(current.keep_open)]),
                "p": _("Startup pop and shutdown click: {value}").format(value=_("on") if current.pop_click else _("off")),
                "t": _("10 Hz channel tick: {value}").format(value=_("on") if current.tick else _("off")),
            })
        choices["h"] = _("Audio help")
        choices["x"] = _("Back")
        choice = dialogs.request_choice(choices, prompt=_("Audio settings"), default="x")
        if choice is None or choice.key == "x":
            return
        if choice.key == "h":
            dialogs.show_message(_(
                "A smaller sound buffer reduces the delay before speech. Automatic starts at 60 milliseconds\n"
                "and increases the buffer if playback runs dry. Medium is 100 milliseconds; Long is 250.\n"
                "Choose a longer buffer if speech breaks up. Quick key response is a separate setting.\n"
                "Changing sample rate saves memory and restarts the emulated unit, preserving its files and settings.\n"
                "Idle sound, channel behavior, pops, and ticks apply to Braille Lite and Braille 'n Speak only.\n"
                "Speech speed, pitch, inflection, and volume are controlled by the original firmware."
            ))
            continue
        selected = None
        updated = current
        if choice.key == "b":
            selected = dialogs.request_choice(buffers, prompt=_("Sound buffer"), default=current.buffer)
            if selected:
                updated = replace(current, buffer=selected.key)
        elif choice.key == "r":
            selected = dialogs.request_choice(rates, prompt=_("Sample rate (saves and restarts the unit)"), default=str(current.rate))
            if selected:
                updated = replace(current, rate=int(selected.key))
        elif choice.key == "i":
            selected = dialogs.request_choice(idle, prompt=_("Idle sound"), default=str(current.idle))
            if selected:
                updated = replace(current, idle=int(selected.key))
        elif choice.key == "c":
            selected = dialogs.request_choice(keep, prompt=_("Keep channel open"), default=str(current.keep_open))
            if selected:
                updated = replace(current, keep_open=int(selected.key))
        elif choice.key == "p":
            updated = replace(current, pop_click=not current.pop_click)
        elif choice.key == "t":
            updated = replace(current, tick=not current.tick)
        if updated == current:
            continue
        try:
            if updated.rate != current.rate:
                with dialogs.activity(_("Saving memory and changing sample rate")):
                    worker.request(updated.command(), "OK")
            else:
                worker.request(updated.command(), "OK")
        except WorkerError as exc:
            dialogs.show_message(_("Audio setting unchanged: {error}").format(error=exc))
            continue
        try:
            preferences.store(preferences.quick_keys, audio=updated)
        except OSError as exc:
            # The preference object still holds the old settings until its atomic write succeeds.
            worker.request(current.command(), "OK")
            dialogs.show_message(_("Could not save audio settings; restored the previous settings: {error}").format(error=exc))
