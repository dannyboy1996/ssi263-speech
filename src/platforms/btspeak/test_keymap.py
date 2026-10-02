"""Firmware fidelity and host-menu isolation, without a BT runtime or keyboard grab."""

import itertools
import unittest

from keymap import ADVANCE, DEEP_ESCAPE, DOT7, DOT8, MENU, SPACE, ChordKeyboard

KEYS = {bit: bit for bit in [1 << n for n in range(10)]}


def gesture(keyboard: ChordKeyboard, bits: int, reverse: bool = False) -> list:
    keys = [bit for bit in KEYS if bits & bit]
    if reverse:
        keys.reverse()
    return [keyboard.feed(key, pressed) for pressed in (True, False) for key in keys]


class KeyboardTests(unittest.TestCase):
    def test_all_original_chords_are_unchanged(self):
        for firmware in range(1, 256):
            physical = firmware & 63
            physical |= SPACE if firmware & 64 else 0
            physical |= ADVANCE if firmware & 128 else 0
            for reverse in (False, True):
                with self.subTest(firmware=firmware, reverse=reverse):
                    actions = gesture(ChordKeyboard(KEYS), physical, reverse)
                    self.assertEqual([a.chord for a in actions if a.chord], [firmware])
                    self.assertFalse(any(a.menu for a in actions))
                    self.assertFalse(any(a.deep_escape for a in actions))
                    self.assertEqual(actions[-1].held, 0)

    def test_enter_and_backspace_belong_to_firmware(self):
        for bits, expected in ((SPACE | 0x11, 0x51), (SPACE | 0x03, 0x43)):
            self.assertEqual(gesture(ChordKeyboard(KEYS), bits)[-1].chord, expected)
        for bits in (DOT7, DOT8, DOT7 | SPACE, DOT8 | SPACE):
            self.assertFalse(any(a.chord or a.menu for a in gesture(ChordKeyboard(KEYS), bits)))

    def test_menu_in_every_press_and_release_order(self):
        keys = [bit for bit in KEYS if MENU & bit]
        for presses in itertools.permutations(keys):
            for releases in (keys, list(reversed(keys)), list(presses)):
                keyboard = ChordKeyboard(KEYS)
                actions = [keyboard.feed(key, True) for key in presses]
                actions += [keyboard.feed(key, False) for key in releases]
                self.assertEqual(sum(a.menu for a in actions), 1)
                self.assertTrue(actions[-1].menu)
                self.assertFalse(any(a.chord for a in actions))

    def test_extra_dots_do_not_become_space_or_advance(self):
        for dots in range(64):
            for extra in (DOT7, DOT8, DOT7 | DOT8):
                for space in (0, SPACE):
                    bits = dots | extra | space
                    actions = gesture(ChordKeyboard(KEYS), bits)
                    self.assertFalse(any(a.chord for a in actions))
                    self.assertEqual(any(a.menu for a in actions), bits == MENU)
                    self.assertEqual(any(a.deep_escape for a in actions), bits == DEEP_ESCAPE)

    def test_deep_escape_in_every_press_order(self):
        keys = [bit for bit in KEYS if DEEP_ESCAPE & bit]
        for presses in itertools.permutations(keys):
            keyboard = ChordKeyboard(KEYS)
            actions = [keyboard.feed(key, True) for key in presses]
            actions += [keyboard.feed(key, False) for key in reversed(presses)]
            self.assertFalse(any(a.chord or a.menu for a in actions))
            self.assertEqual(sum(a.deep_escape for a in actions), 1)
            self.assertTrue(actions[-1].deep_escape)

    def test_repeat_unmatched_release_and_unassigned_keys(self):
        keyboard = ChordKeyboard(KEYS)
        self.assertIsNone(keyboard.feed(1, False))
        self.assertEqual(keyboard.feed(1, True).held, 1)
        self.assertIsNone(keyboard.feed(1, True))
        self.assertIsNone(keyboard.feed(9999, True))
        self.assertIsNone(keyboard.feed(9999, False))
        self.assertEqual(keyboard.feed(1, False).chord, 1)

    def test_rolled_chord_and_held_reset(self):
        keyboard = ChordKeyboard(KEYS)
        keyboard.feed(2, True)
        self.assertEqual(keyboard.feed(8, True).held, 10)
        self.assertEqual(keyboard.feed(SPACE, True).held, 0x4A)
        self.assertEqual(keyboard.feed(2, False).held, 0x48)
        self.assertEqual(keyboard.feed(8, False).chord, 0)
        self.assertEqual(keyboard.feed(SPACE, False).chord, 0x4A)


if __name__ == "__main__":
    unittest.main()
