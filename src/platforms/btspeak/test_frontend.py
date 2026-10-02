"""Host integration with simulated keyboard events; never touches device services."""

import contextlib
import socket
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType, SimpleNamespace
from unittest.mock import Mock, patch

from frontend import capture, deep_escape, host_menu, main
from keymap import DEEP_ESCAPE, MENU
from preferences import Preferences


class Connection:
    def __init__(self, events):
        self.reader, self.writer = socket.socketpair()
        self.events = iter(events)
        self.writer.sendall(b"x" * len(events))
        self.acks = 0
        self.reads = 0
        self.closed = False

    def fileno(self):
        return self.reader.fileno()

    def read_key(self):
        self.reader.recv(1)
        self.reads += 1
        return next(self.events)

    def ack_consume(self):
        self.acks += 1

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.closed = True
        self.reader.close()
        self.writer.close()


def keyboard(events):
    conn = Connection(events)
    kb = ModuleType("kb_client")
    kb.BTB_DOT1 = 497
    kb.BTB_SPACE = 57
    kb.BTB_R3 = 261
    kb.BTB_R2 = 260
    kb.BTB_L2 = 257
    kb.BTB_L3 = 258
    kb.FLAG_EXCLUSIVE = 1
    kb.FLAG_WANT_RAW = 2
    kb.connect = lambda flags: conn
    kb.server_available = lambda: True
    return kb, conn


def chord_events(bits):
    keys = [497 + dot for dot in range(8) if bits & (1 << dot)]
    if bits & 256:
        keys.append(57)
    return [SimpleNamespace(key=key, state=pressed) for pressed in (True, False) for key in keys]


class FrontendTests(unittest.TestCase):
    def test_type_n_speak_uses_qwerty_adapter_without_leaking_menu_chord(self):
        kb, conn = keyboard(chord_events(0x3D) + chord_events(256 | 0x11) + chord_events(MENU))
        output, peer = socket.socketpair()
        self.addCleanup(output.close)
        self.addCleanup(peer.close)
        commands = []
        capture(SimpleNamespace(output=output, send=commands.append), kb, tns=True)
        self.assertEqual(commands, ["TNS y", "TNS enter"])
        self.assertEqual(conn.reads, conn.acks)

    def test_panel_pairs_are_independent_of_chords_and_keep_overlapping_holds(self):
        presses = [(258, True), (261, True), (258, False), (261, False),
                   (257, True), (260, True), (257, False), (260, False),
                   (258, True), (260, True), (258, False), (260, False)]
        events = [SimpleNamespace(key=key, state=state) for key, state in presses]
        kb, conn = keyboard(events + chord_events(MENU))
        output, peer = socket.socketpair()
        self.addCleanup(output.close)
        self.addCleanup(peer.close)
        commands = []

        def send(command):
            self.assertEqual(conn.reads, conn.acks)
            commands.append(command)

        capture(SimpleNamespace(output=output, send=send), kb)
        self.assertEqual([c for c in commands if c.startswith("BARS")],
                         ["BARS " + str(n) for n in (1, 1, 1, 0, 2, 2, 2, 0, 1, 3, 2, 0)])
        self.assertFalse(any(c.startswith("KEY") and c.split()[-1] != "0" for c in commands))

    def test_firmware_menu_returns_selection_without_changing_running_unit(self):
        prefs = Preferences(Path("unused"))
        worker = Mock()
        dialogs = SimpleNamespace(request_choice=Mock(side_effect=[SimpleNamespace(key="f"),
                                                                   SimpleNamespace(key="bl-es")]))
        units = {"bl-en": "English", "bl-es": "Spanish"}
        self.assertEqual(host_menu(worker, dialogs, prefs, units), "unit:bl-es")
        self.assertEqual(prefs.unit, "bl-en")
        worker.assert_not_called()

    def test_switch_saves_old_unit_before_resuming_new_and_remembers_selection(self):
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                for name in ("backend", "BL2ENG.BNS", "bl2_2003_warm.state", "BL2SPA.BNS", "bl2spa_fresh.state"):
                    (root / name).touch()
                events = []

                def unit(name):
                    return SimpleNamespace(braille=bytes(18),
                        request=lambda cmd, reply: events.append((name, cmd)),
                        close=lambda: events.append((name, "close")),
                        abort=lambda: events.append((name, "abort")))

                english, spanish = unit("en"), unit("es")
                runtime = ModuleType("BTSpeak")
                runtime.kb_client = SimpleNamespace(server_available=lambda: True)
                runtime.brl = SimpleNamespace(has_display=lambda: False, write=lambda text: None)
                runtime.host = SimpleNamespace(push_self_voice=lambda value: None, pop_self_voice=lambda: None)
                runtime.dialogs = SimpleNamespace(activity=lambda text: contextlib.nullcontext(), show_message=Mock())
                runtime.script = SimpleNamespace(start_log=lambda: Mock())
                outcomes = [english, OSError("bad firmware") if fail else spanish]
                with (patch.dict(sys.modules, {"BTSpeak": runtime}), patch("frontend.Worker", side_effect=outcomes) as start,
                      patch("frontend.capture", return_value="menu"),
                      patch("frontend.host_menu", side_effect=["unit:bl-es", "quit"]),
                      patch.object(sys, "argv", ["blazie", "--backend", str(root / "backend"),
                                                "--firmware", tmp, "--state-dir", tmp])):
                    self.assertEqual(main(), 0)
                self.assertEqual(start.call_args_list[0].args[3], root / "english.state")
                self.assertEqual(start.call_args_list[1].args[3], root / "spanish.state")
                self.assertEqual(Preferences.load(root / "preferences.json").unit, "bl-en" if fail else "bl-es")
                if fail:
                    self.assertEqual(events.count(("en", "RESUME")), 2)
                    runtime.dialogs.show_message.assert_any_call("Could not switch firmware: bad firmware")
                else:
                    self.assertLess(events.index(("en", "close")), events.index(("es", "RESUME")))
                    self.assertIn(("es", "close"), events)

    def test_ack_before_forward_and_menu_does_not_leak(self):
        kb, conn = keyboard(chord_events(256 | 0x11) + chord_events(MENU))
        output, peer = socket.socketpair()
        self.addCleanup(output.close)
        self.addCleanup(peer.close)
        commands = []

        def send(command):
            self.assertEqual(conn.reads, conn.acks)
            commands.append(command)

        worker = SimpleNamespace(output=output, send=send)
        capture(worker, kb)
        self.assertTrue(conn.closed)
        self.assertEqual([c for c in commands if c.split()[-1] != "0"], ["KEY 0 81"])
        self.assertEqual(commands[-1], "KEY 0 0")

    def test_capture_releases_keyboard_on_worker_failure(self):
        kb, conn = keyboard(chord_events(1))
        output, peer = socket.socketpair()
        self.addCleanup(output.close)
        self.addCleanup(peer.close)

        def send(command):
            raise OSError("broken pipe")

        with self.assertRaises(OSError):
            capture(SimpleNamespace(output=output, send=send), kb)
        self.assertTrue(conn.closed)
        self.assertEqual(conn.reads, conn.acks)

    def test_main_pauses_and_releases_before_dialog_and_restores_on_exit(self):
        self.run_exit(MENU)

    def test_deep_escape_saves_and_restores_before_leaving_terminal(self):
        self.run_exit(DEEP_ESCAPE)

    def test_deep_escape_save_failure_does_not_close_terminal(self):
        self.run_exit(DEEP_ESCAPE, save_failure=True)

    def run_exit(self, chord, save_failure=False):
        kb, conn = keyboard(chord_events(chord))
        output, peer = socket.socketpair()
        self.addCleanup(output.close)
        self.addCleanup(peer.close)
        actions = []
        def close():
            actions.append("close")
            if save_failure:
                raise OSError("save failed")

        worker = SimpleNamespace(
            braille=bytes(range(18)),
            output=output, send=lambda command: actions.append(command),
            request=lambda command, response: actions.append(command),
            close=close,
            abort=lambda: actions.append("abort"),
        )

        def choice(*args, **kwargs):
            self.assertNotEqual(chord, DEEP_ESCAPE)
            self.assertTrue(conn.closed)
            self.assertEqual(actions[-1], "PAUSE")
            return SimpleNamespace(key="q")

        runtime = ModuleType("BTSpeak")
        runtime.kb_client = kb
        runtime.brl = SimpleNamespace(write=lambda text: actions.append(("braille", text)), get_display_width=lambda: 40,
                                      has_display=lambda: True, write_dots=lambda cells: actions.append(("dots", cells)))
        runtime.host = SimpleNamespace(push_self_voice=lambda value: actions.append("push"),
                                       pop_self_voice=lambda: actions.append("pop"))
        runtime.dialogs = SimpleNamespace(activity=lambda text: contextlib.nullcontext(),
                                          show_message=lambda text: None, request_choice=choice)
        runtime.script = SimpleNamespace(start_log=lambda: Mock())

        def leave(host):
            self.assertTrue(conn.closed)
            self.assertEqual(actions[-4:], ["close", "abort", ("braille", ""), "pop"])
            actions.append("deep_escape")

        with tempfile.TemporaryDirectory() as tmp:
            backend = Path(tmp) / "backend"
            backend.touch()
            for name in ("BL2ENG.BNS", "bl2_2003_warm.state"):
                (Path(tmp) / name).touch()
            with (patch.dict(sys.modules, {"BTSpeak": runtime}), patch("frontend.Worker", return_value=worker),
                  patch("frontend.deep_escape", side_effect=leave) as escape):
                with patch.object(sys, "argv", ["blazie", "--backend", str(backend), "--state-dir", tmp, "--firmware", tmp]):
                    self.assertEqual(main(), int(save_failure))
                self.assertEqual(escape.call_count, int(chord == DEEP_ESCAPE and not save_failure))
        self.assertEqual(actions[0], "push")
        self.assertIn("QUICK 0", actions)
        self.assertIn(("dots", bytes(range(18)) + bytes(22)), actions)

    def test_deep_escape_uses_detached_platform_helper(self):
        host = SimpleNamespace(repository_path=Path("/platform"))
        with patch("frontend.os.geteuid", return_value=1000), patch("frontend.subprocess.run") as run:
            deep_escape(host)
        self.assertEqual(run.call_args.args[0], ["sudo", "-n", "/platform/Tools/deep-escape"])
        self.assertTrue(run.call_args.kwargs["start_new_session"])

    def test_quick_menu_toggles_and_remembers_preference(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefs = Preferences.load(Path(tmp) / "preferences.json")
            worker = Mock()
            labels = []
            def choice(choices, **kwargs):
                labels.append(choices["k"])
                return SimpleNamespace(key="k" if len(labels) == 1 else "r")
            self.assertEqual(host_menu(worker, SimpleNamespace(request_choice=choice), prefs), "resume")
            worker.request.assert_called_once_with("QUICK 1", "OK")
            self.assertTrue(Preferences.load(prefs.path).quick_keys)
            self.assertEqual(labels, ["Quick key response: off", "Quick key response: on"])
            # The same menu switches back off and persists that choice too.
            labels.clear()
            host_menu(worker, SimpleNamespace(request_choice=choice), prefs)
            worker.request.assert_called_with("QUICK 0", "OK")
            self.assertFalse(Preferences.load(prefs.path).quick_keys)

    def test_preference_save_failure_rolls_back_quick_response(self):
        prefs = Preferences(Path("unused"))
        worker = Mock()
        with patch.object(prefs, "store", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                host_menu(worker, SimpleNamespace(request_choice=lambda *a, **k: SimpleNamespace(key="k")), prefs)
        self.assertEqual([call.args for call in worker.request.call_args_list], [("QUICK 1", "OK"), ("QUICK 0", "OK")])


if __name__ == "__main__":
    unittest.main()
