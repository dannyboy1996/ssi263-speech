"""Audio choices, persistence, and rollback without using a physical sound device."""

import contextlib
import json
import tempfile
import unittest
from dataclasses import asdict, replace
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from audio_menu import audio_menu
from audio_options import AudioOptions, RATES
from preferences import Preferences
from worker import WorkerError


class AudioMenuTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.prefs = Preferences(Path(tmp.name) / 'preferences.json')
        self.worker = Mock()

    def dialogs(self, choices):
        return SimpleNamespace(request_choice=Mock(side_effect=[None if key is None else SimpleNamespace(key=key) for key in choices]),
                               show_message=Mock(), activity=lambda text: contextlib.nullcontext())

    def test_all_controls_persist_without_losing_unit_or_quick_response(self):
        self.prefs.store(True, unit='bl-es')
        dialogs = self.dialogs(['b', 'auto', 'r', '16000', 'i', '0', 'c', '2', 'p', 't', 'x'])
        audio_menu(self.worker, dialogs, self.prefs)
        expected = AudioOptions(16000, 'auto', 0, 2, False, False)
        self.assertEqual(self.prefs.audio, expected)
        self.worker.request.assert_called_with(expected.command(), 'OK')
        loaded = Preferences.load(self.prefs.path)
        self.assertEqual(loaded.audio, expected)
        self.assertTrue(loaded.quick_keys)
        self.assertEqual(loaded.unit, 'bl-es')
        loaded.store(False, unit='tns-en')
        self.assertEqual(Preferences.load(loaded.path).audio, expected)

    def test_cancel_and_unchanged_choices_do_not_apply_or_save(self):
        dialogs = self.dialogs(['b', None, 'r', '22050', None])
        audio_menu(self.worker, dialogs, self.prefs)
        self.worker.request.assert_not_called()
        self.assertFalse(self.prefs.path.exists())

    def test_worker_failure_keeps_previous_preferences_and_menu_open(self):
        self.worker.request.side_effect = WorkerError('device rejected sample rate')
        dialogs = self.dialogs(['r', '48000', 'x'])
        audio_menu(self.worker, dialogs, self.prefs)
        self.assertEqual(self.prefs.audio, AudioOptions())
        self.assertFalse(self.prefs.path.exists())
        dialogs.show_message.assert_called_once()

    def test_save_failure_restores_previous_worker_settings(self):
        dialogs = self.dialogs(['b', 'auto', 'x'])
        with patch.object(self.prefs, 'store', side_effect=OSError('disk full')):
            audio_menu(self.worker, dialogs, self.prefs)
        self.assertEqual([c.args[0] for c in self.worker.request.call_args_list],
                         [replace(AudioOptions(), buffer='auto').command(), AudioOptions().command()])
        self.assertEqual(self.prefs.audio, AudioOptions())
        dialogs.show_message.assert_called_once()

    def test_type_n_speak_hides_unsupported_idle_controls(self):
        self.prefs.unit = 'tns-en'
        dialogs = self.dialogs(['x'])
        audio_menu(self.worker, dialogs, self.prefs)
        self.assertEqual(set(dialogs.request_choice.call_args.args[0]), {'b', 'r', 'h', 'x'})

    def test_migration_and_validation(self):
        self.prefs.path.write_text('{"quick_keys": true, "unit": "bl-es"}')
        self.assertEqual(Preferences.load(self.prefs.path).audio, AudioOptions())
        for rate in RATES:
            self.assertEqual(AudioOptions.load({'rate': rate}).rate, rate)
        invalid = [None, [], {'rate': True}, {'rate': 123}, {'buffer': 'short'}, {'buffer': []},
                   {'idle': 4}, {'idle': False}, {'keep_open': 3}, {'pop_click': 1}, {'tick': 'off'}, {'unknown': 0}]
        for data in invalid:
            with self.subTest(data=data), self.assertRaises(ValueError):
                AudioOptions.load(data)
        self.prefs.store(True, audio=AudioOptions(buffer='auto'))
        self.assertEqual(json.loads(self.prefs.path.read_text())['audio'], asdict(self.prefs.audio))


if __name__ == '__main__':
    unittest.main()
