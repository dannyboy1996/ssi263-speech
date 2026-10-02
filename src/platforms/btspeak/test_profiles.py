"""Model discovery and preferences without external firmware or device access."""

import tempfile
import unittest
from pathlib import Path

from preferences import Preferences
from profiles import BY_KEY, PROFILES
from tns_keyboard import CELLS, key_name


class ProfileTests(unittest.TestCase):
    def test_all_existing_models_and_isolated_states(self):
        self.assertEqual(set(BY_KEY), {"bl-en", "bl-es", "tns-en", "tns-es", "bns-en", "bns-sk"})
        self.assertEqual(len({p.saved for p in PROFILES}), 6)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for profile in PROFILES:
                self.assertFalse(profile.available(root, root))
                firmware = root / profile.firmware[0]
                firmware.parent.mkdir(parents=True, exist_ok=True)
                firmware.touch()
                if profile.factory:
                    self.assertFalse(profile.available(root, root))
                    factory = root / profile.factory[0]
                    factory.parent.mkdir(parents=True, exist_ok=True)
                    factory.touch()
                self.assertTrue(profile.available(root, root))
                self.assertEqual(profile.paths(root, root)[0], firmware)

    def test_flat_layout_and_existing_state_without_factory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "BL2SPA.BNS").touch()
            (root / "spanish.state").touch()
            self.assertTrue(BY_KEY["bl-es"].available(root, root))
            self.assertEqual(BY_KEY["bl-es"].paths(root, root)[0], root / "BL2SPA.BNS")

    def test_old_preferences_and_unit_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "preferences.json"
            path.write_text('{"quick_keys": true}')
            prefs = Preferences.load(path)
            self.assertEqual(prefs.unit, "bl-en")
            prefs.store(True, unit="tns-es")
            self.assertEqual(Preferences.load(path).unit, "tns-es")
            prefs.store(False)
            self.assertEqual(Preferences.load(path).unit, "tns-es")
            path.write_text('{"unit": []}')
            with self.assertRaises(ValueError):
                Preferences.load(path)

    def test_tns_adapter_letters_and_controls(self):
        self.assertEqual(len(CELLS), 64)
        for char in "abcdefghijklmnopqrstuvwxyz":
            self.assertEqual(key_name(CELLS.index(char.upper())), char)
        for chord, expected in ((0x51, "enter"), (0x43, "backspace"), (0x4A, "tab"),
                                (0x64, "esc"), (0x40, "space"), (0x55, "ctrl-o"), (0x41, "up")):
            self.assertEqual(key_name(chord), expected)
        self.assertIsNone(key_name(0))


if __name__ == "__main__":
    unittest.main()
