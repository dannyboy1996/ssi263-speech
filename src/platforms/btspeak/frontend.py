"""Blazie Mode host menus and direct keyboard capture for the firmware emulator."""

import argparse
import os
import select
import signal
import subprocess
import time
from dataclasses import replace
from gettext import gettext as _
from pathlib import Path
from types import ModuleType

from keymap import SPACE, ChordKeyboard
from display import BrailleOutput
from preferences import Preferences
from profiles import BY_KEY, PROFILES
from worker import Worker, WorkerError
from tns_keyboard import key_name as tns_key_name
from audio_menu import audio_menu
from audio_options import RATES
from runtime_paths import default_paths


def capture(worker: Worker, kb_client: ModuleType, display: BrailleOutput | None = None, *, tns: bool = False) -> str:
    """Return a host action after its chord is released. ACK before doing any work."""
    keys = {kb_client.BTB_DOT1 + dot: 1 << dot for dot in range(8)}
    keys[kb_client.BTB_SPACE] = SPACE
    bars = {kb_client.BTB_R3: 1, kb_client.BTB_R2: 2, kb_client.BTB_L3: 1, kb_client.BTB_L2: 2}
    bars_held: set[int] = set()
    keyboard = ChordKeyboard(keys)
    pending_display = False
    next_display = time.monotonic()
    with kb_client.connect(kb_client.FLAG_EXCLUSIVE | kb_client.FLAG_WANT_RAW) as conn:
        while True:
            ready, _writable, _failed = select.select([conn, worker.output], [], [], 0.02 if display else 0.1)
            if conn in ready:
                event = conn.read_key()
                conn.ack_consume()
                if event.key in bars:
                    if event.state:
                        bars_held.add(event.key)
                    else:
                        bars_held.discard(event.key)
                    bars_down = 0
                    for key in bars_held:
                        bars_down |= bars[key]
                    if not tns:
                        worker.send("BARS {down}".format(down=bars_down))
                    continue
                if event.key is not None:
                    action = keyboard.feed(event.key, event.state)
                    if action is not None:
                        if tns:
                            name = tns_key_name(action.chord)
                            if name:
                                worker.send("TNS " + name)
                        else:
                            worker.send("KEY {held} {chord}".format(held=action.held, chord=action.chord))
                        if action.menu:
                            return "menu"
                        if action.deep_escape:
                            return "deep_escape"
            if worker.output in ready:
                responses = worker.receive()  # Report failures; autosaves need no announcement.
                if "BRAILLE" in responses:
                    pending_display = False
                    if display is not None:
                        display.show(worker.braille)
            # One outstanding poll at most: neither a slow device nor a slow worker can build a frame backlog.
            if display is not None and not pending_display and time.monotonic() >= next_display:
                worker.send("BRAILLE")
                pending_display = True
                next_display = time.monotonic() + 0.05


def host_menu(worker: Worker, dialogs: ModuleType, preferences: Preferences,
              units: dict[str, str] | None = None) -> str:
    """The worker is paused and the keyboard is back with BRLTTY."""
    while True:
        choices = {
            "r": _("Resume emulator"),
            "s": _("Save memory"),
            "a": _("Press advance bar and resume"),
            "k": _("Quick key response: {state}").format(state=_("on") if preferences.quick_keys else _("off")),
            "o": _("Audio settings"),
            "h": _("Keyboard help"),
            "q": _("Save and exit"),
        }
        if units:
            choices["f"] = _("Firmware: {name}").format(name=units[preferences.unit])
        if not preferences.unit.startswith("bl-"):
            choices.pop("a")
        if BY_KEY[preferences.unit].kind == "tns":
            choices["t"] = _("Send Type 'n Speak key")
        choice = dialogs.request_choice(choices, prompt=_("Blazie emulator"), default="r")
        if choice is None or choice.key == "r":
            return "resume"
        if choice.key == "q":
            return "quit"
        if choice.key == "o":
            audio_menu(worker, dialogs, preferences)
            continue
        if choice.key == "t":
            name = dialogs.request_input(_("Key name, for example ctrl-o, alt-x, or f1"))
            if name:
                try:
                    if not name.isascii() or any(char.isspace() for char in name):
                        raise ValueError(_("Use a single key name, with hyphens for modifiers."))
                    worker.request("TNS " + name, "OK")
                except (ValueError, WorkerError) as exc:
                    dialogs.show_message(str(exc))
                    continue
                return "resume"
            continue
        if choice.key == "a":
            return "advance"
        if choice.key == "f" and units:
            selected = dialogs.request_choice(units, prompt=_("Choose firmware"), default=preferences.unit)
            if selected is not None and selected.key != preferences.unit:
                return "unit:" + selected.key
            continue
        if choice.key == "k":
            quick = not preferences.quick_keys
            worker.request("QUICK {value}".format(value=int(quick)), "OK")
            try:
                preferences.store(quick)
            except OSError:
                worker.request("QUICK {value}".format(value=int(preferences.quick_keys)), "OK")
                raise
            continue
        if choice.key == "s":
            with dialogs.activity(_("Saving memory")):
                worker.request("SAVE", "SAVED")
            dialogs.show_message(_("Memory saved."))
        elif choice.key == "h":
            if BY_KEY[preferences.unit].kind == "tns":
                dialogs.show_message(_(
                    "Type 'n Speak has a QWERTY keyboard and no braille display.\n"
                    "Use six-dot computer braille to type. E-chord is Enter, B-chord is Backspace,\n"
                    "dots 3-6 chord is Escape, I-chord is Tab, dots 1/4 chords are Up/Down,\n"
                    "dots 2/5 chords are Left/Right. Other space chords send Control plus the character.\n"
                    "Use Send Type 'n Speak key for function keys, Shift, Alt, and other combinations.\n"
                    "M-chord with Dot 7 opens this menu; Z-chord with Dot 7 saves and closes the terminal."
                ))
                continue
            display_help = (_("On BT Braille, L3 or R3 advances braille; L2 or R2 moves it back. Routing keys are unused.\n"
                              "On BT Speak, the advance bar is available from the host menu.\n")
                            if preferences.unit.startswith("bl-") else
                            _("Braille 'n Speak has no braille display or advance bars.\n"))
            dialogs.show_message(_(
                "The original firmware handles dots 1 through 6 and Space.\n"
                "Use E-chord for Enter and B-chord for Backspace.\n"
                "M-chord with Dot 7 opens this host menu: Space plus dots 1, 3, 4, and 7.\n"
                "Z-chord with Dot 7 saves and exits to the host editor, closing the terminal too.\n"
                "Other combinations with dots 7 or 8 do nothing in the emulator.\n"
                "{display_help}"
                "Memory is saved every minute and on exit.\n"
                "Quick key response reduces the delay before speech by speeding up firmware processing.\n"
                "It is optional, remembered between runs, and off by default for original timing.\n"
                "Choose Firmware to switch units; each keeps its own saved memory.\n"
                "Braille follows the selected firmware; unused cells are blank."
            ).format(display_help=display_help))


def deep_escape(host: ModuleType) -> None:
    """Use the platform's normal deep escape, after saving and releasing all app resources.

    Detached from this terminal so the helper survives its own terminal cleanup. No inherited
    pipes: the platform may terminate this frontend before its helper finishes switching VTs.
    """
    command = [str(host.repository_path / "Tools" / "deep-escape")]
    if os.geteuid() != 0:
        command = ["sudo", "-n", *command]
    subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   start_new_session=True, check=True, timeout=15)


def main() -> int:
    backend_path, firmware_path = default_paths()
    parser = argparse.ArgumentParser(description=_("Original Blazie firmware in Blazie Mode."))
    parser.add_argument("--firmware", type=Path, default=firmware_path,
                        help=_("Folder containing firmware and factory states"))
    parser.add_argument("--unit", choices=BY_KEY, help=_("Firmware to run; defaults to the last selected unit"))
    parser.add_argument("--state-dir", type=Path, help=_("Separate folder for the emulated unit's memory"))
    parser.add_argument("--backend", type=Path, default=backend_path)
    parser.add_argument("--device", default="default", help=_("ALSA output device"))
    parser.add_argument("--rate", type=int, choices=RATES, help=_("Sample rate; defaults to the saved audio setting"))
    args = parser.parse_args()

    # Deferred so --help works even on a development machine without the BT runtime.
    from BTSpeak import brl, dialogs, host, kb_client, script

    log = script.start_log()
    worker = None
    result = 0
    exit_to_editor = False

    def interrupted(signum: int, frame: object) -> None:
        raise KeyboardInterrupt

    old_handlers = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGTERM, signal.SIGHUP)}
    host.push_self_voice(True)
    try:
        if not args.backend.is_file():
            raise WorkerError(_("Build the emulator first by running ./build_linux.sh in the repository."))
        if not kb_client.server_available():
            raise WorkerError(_("The device keyboard service is unavailable."))
        state_dir = args.state_dir or Path(script.getUserSubdirectory("blazie-emulator"))
        state_dir.mkdir(parents=True, exist_ok=True)
        preferences = Preferences.load(state_dir / "preferences.json")
        if args.rate is not None:
            preferences.audio = replace(preferences.audio, rate=args.rate)
        selected = args.unit or preferences.unit
        units = {profile.key: profile.label for profile in PROFILES if profile.available(args.firmware, state_dir)}
        if selected not in units:
            raise WorkerError(_("Firmware or factory state is missing for {name}.").format(name=BY_KEY[selected].label))

        def start_unit(key: str) -> Worker:
            profile = BY_KEY[key]
            paths = profile.paths(args.firmware, state_dir)
            candidate = Worker(args.backend.resolve(), *paths, args.device, preferences.audio.rate, kind=profile.kind)
            try:
                candidate.request("QUICK {value}".format(value=int(preferences.quick_keys)), "OK")
                candidate.request(preferences.audio.command(), "OK")
            except BaseException:
                candidate.abort()
                raise
            return candidate

        def first_start_notice(key: str) -> None:
            profile = BY_KEY[key]
            if profile.kind == "tns" and not (state_dir / profile.saved).exists():
                dialogs.show_message(_(
                    "New Type 'n Speak. Answer y to each setup question, or s for Spanish, seven times in all.\n"
                    "Initialize file system, are you sure, initialize flash, are you sure; then about 45 seconds\n"
                    "of clicks. Initialize folders, delete all data in file area, are you sure; then about\n"
                    "35 seconds of silence. Wait for each question. This initializes this new unit's memory."
                ))

        with dialogs.activity(_("Starting Blazie emulator")):
            worker = start_unit(selected)
            preferences.store(preferences.quick_keys, unit=selected)
        first_start_notice(selected)
        dialogs.show_message(_(
            "Use the original six-dot chords. M-chord with Dot 7 opens the emulator menu."
        ))
        action = "resume"
        display = BrailleOutput(brl)
        while True:
            worker.request("RESUME", "RUNNING")
            worker.request("BRAILLE", "BRAILLE")
            display.show(worker.braille, force=True)
            if action == "advance":
                worker.send("BARS 1")
                worker.send("BARS 0")
            try:
                action = capture(worker, kb_client, display, tns=BY_KEY[preferences.unit].kind == "tns")
            finally:
                # capture has already closed its exclusive connection; dialogs may now acquire it.
                worker.request("PAUSE", "PAUSED")
            if action == "deep_escape":
                exit_to_editor = True
                break
            action = host_menu(worker, dialogs, preferences, units)
            if action == "quit":
                break
            if action.startswith("unit:"):
                selected = action[5:]
                try:
                    with dialogs.activity(_("Switching firmware")):
                        candidate = start_unit(selected)
                except (OSError, WorkerError) as exc:
                    dialogs.show_message(_("Could not switch firmware: {error}").format(error=exc))
                    continue  # The previous unit is still paused, with its memory intact.
                try:
                    worker.close()
                except BaseException:
                    candidate.abort()
                    raise
                worker = candidate
                preferences.store(preferences.quick_keys, unit=selected)
                first_start_notice(selected)
    except KeyboardInterrupt:
        pass
    except (OSError, EOFError, ValueError, WorkerError) as exc:
        log.exception("Blazie emulator failed")
        dialogs.show_message(_("Emulator error: {error}").format(error=exc))
        result = 1
    finally:
        try:
            if worker is not None:
                try:
                    with dialogs.activity(_("Saving emulator memory")):
                        worker.close()
                except (OSError, WorkerError) as exc:
                    log.exception("Blazie emulator could not close cleanly")
                    dialogs.show_message(_("Could not finish saving: {error}").format(error=exc))
                    result = 1
                finally:
                    worker.abort()
        finally:
            try:
                brl.write("")
            finally:
                try:
                    host.pop_self_voice()
                finally:
                    for sig, handler in old_handlers.items():
                        signal.signal(sig, handler)
    if exit_to_editor and result == 0:
        try:
            deep_escape(host)
        except (OSError, subprocess.SubprocessError) as exc:
            log.exception("Host deep escape failed")
            dialogs.show_message(_("Emulator saved, but could not return to the editor: {error}").format(error=exc))
            result = 1
    return result
