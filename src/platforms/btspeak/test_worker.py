"""Real native worker, isolated saved states and silent pacing. Never takes the device keyboard."""

import signal
import os
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from worker import Worker, WorkerError

ROOT = Path(__file__).resolve().parents[3]
EXE = ROOT / "build/linux/blazie_bt"
FIRMWARE_DIR = Path(os.environ.get("BLAZIE_TEST_FIRMWARE", str(ROOT / "firmware/blazie"))).resolve()
FIRMWARE = FIRMWARE_DIR / "BL2ENG.BNS"
FACTORY = FIRMWARE_DIR / "bl2_2003_warm.state"


@unittest.skipUnless(EXE.exists() and FIRMWARE.exists() and FACTORY.exists(), "Build the worker and supply firmware")
class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.saved = Path(self.tmp.name) / "english.state"

    def start(self):
        worker = Worker(EXE, FIRMWARE, FACTORY, self.saved, "-")
        self.addCleanup(worker.abort)
        return worker

    def test_pause_save_resume_reload_and_lock(self):
        worker = self.start()
        with self.assertRaisesRegex(WorkerError, "busy"):
            self.start()
        worker.request("RESUME", "RUNNING")
        time.sleep(0.2)
        worker.send("KEY 1 0")
        worker.send("KEY 0 1")
        worker.request("PAUSE", "PAUSED")
        worker.request("SAVE", "SAVED")
        self.assertGreater(self.saved.stat().st_size, 786432)
        self.assertFalse(self.saved.with_suffix(".state.new").exists())
        worker.request("RESUME", "RUNNING")
        worker.close()
        restored = self.start()
        restored.close()

    def test_bad_saved_state_is_not_replaced_by_factory(self):
        original = b"damaged state"
        self.saved.write_bytes(original)
        with self.assertRaises(WorkerError):
            self.start()
        self.assertEqual(self.saved.read_bytes(), original)

    def test_signal_and_parent_eof_save(self):
        for reason in ("signal", "eof"):
            with self.subTest(reason=reason):
                worker = self.start()
                worker.request("RESUME", "RUNNING")
                if reason == "signal":
                    worker.process.send_signal(signal.SIGTERM)
                else:
                    worker.input.close()
                worker.wait_for("BYE")
                self.assertEqual(worker.process.wait(timeout=5), 0)
                self.assertGreater(self.saved.stat().st_size, 786432)
                worker.abort()

    def test_save_failure_is_reported(self):
        worker = self.start()
        self.saved.mkdir()  # Atomic replacement must fail, preserving the old destination.
        with self.assertRaisesRegex(WorkerError, "save"):
            worker.request("SAVE", "SAVED")
        self.assertTrue(self.saved.is_dir())
        self.saved.rmdir()
        worker.close()

    def test_audio_failure_leaves_worker_responsive(self):
        worker = Worker(EXE, FIRMWARE, FACTORY, self.saved, "blazie_nonexistent_audio_device")
        self.addCleanup(worker.abort)
        with self.assertRaises(WorkerError):
            worker.request("RESUME", "RUNNING")
        worker.request("PAUSE", "PAUSED")
        worker.close()

    def test_real_firmware_sees_keys_held_through_restart(self):
        worker = self.start()
        worker.request("QUICK 1", "OK")  # Fast keys must still allow the original reset hold timing.
        worker.request("RESUME", "RUNNING")
        time.sleep(8)
        worker.send("KEY 0 79")  # p-chord
        time.sleep(2)
        worker.send("KEY 0 7")  # l
        time.sleep(0.25)
        worker.send("KEY 74 0")  # hold i-chord through the restart
        time.sleep(2)
        worker.send("KEY 0 74")
        time.sleep(2)
        worker.close()
        result = subprocess.run([
            str(ROOT / "build/linux/blazie_emu"), "--null", "--firmware", str(FIRMWARE.parent),
            "--state", str(self.saved), "--seconds", "0.1", "--ram-has", "initialize file system",
        ], capture_output=True, text=True, timeout=30, check=True)
        self.assertIn('ram has "initialize file system": yes', result.stdout)

    def test_quick_response_can_be_changed_while_paused_or_running(self):
        worker = self.start()
        worker.request("QUICK 1", "OK")
        worker.request("RESUME", "RUNNING")
        worker.send("KEY 0 1")
        worker.request("QUICK 0", "OK")
        worker.request("PAUSE", "PAUSED")
        with self.assertRaisesRegex(WorkerError, "Invalid"):
            worker.request("QUICK 2", "OK")
        worker.close()

    def test_audio_controls_all_rates_and_buffers_keep_memory(self):
        from audio_options import AudioOptions, RATES
        from dataclasses import replace
        worker = self.start()
        worker.request("RESUME", "RUNNING")
        time.sleep(0.2)
        worker.request("PAUSE", "PAUSED")
        worker.request("SAVE", "SAVED")
        flash = self.saved.read_bytes()[262144:-64]
        options = AudioOptions()
        worker.request("AUDIO", options.command())
        worker.request("QUICK 1", "OK")
        for rate in RATES:
            options = replace(options, rate=rate)
            worker.request(options.command(), "OK")
            worker.request("AUDIO", options.command())
            worker.request("RESUME", "RUNNING")
            time.sleep(0.03)
            worker.request("PAUSE", "PAUSED")
        for mode in ("auto", "medium", "long"):
            for idle in range(4):
                options = replace(options, buffer=mode, idle=idle, keep_open=idle % 3, pop_click=bool(idle % 2), tick=False)
                worker.request(options.command(), "OK")
                worker.request("AUDIO", options.command())
        worker.request("SAVE", "SAVED")
        self.assertEqual(self.saved.read_bytes()[262144:-64], flash)
        for invalid in ("AUDIO 123 long 3 1 1 1", "AUDIO 22050 short 3 1 1 1", "AUDIO 22050 auto 4 1 1 1",
                        "AUDIO 22050 auto 3 3 1 1", "AUDIO 22050 auto 3 1 2 1", "AUDIO 22050 auto 3 1 1 2"):
            with self.assertRaises(WorkerError):
                worker.request(invalid, "OK")
            worker.request("AUDIO", options.command())
        worker.request("RESUME", "RUNNING")
        with self.assertRaisesRegex(WorkerError, "not paused"):
            worker.request(options.command(), "OK")
        worker.close()

    def test_sample_rate_save_failure_keeps_running_unit(self):
        worker = self.start()
        self.saved.mkdir()
        with self.assertRaisesRegex(WorkerError, "save"):
            worker.request("AUDIO 48000 auto 0 0 0 0", "OK")
        worker.request("AUDIO", "AUDIO 22050 long 3 1 1 1")
        worker.request("RESUME", "RUNNING")
        worker.request("PAUSE", "PAUSED")
        self.saved.rmdir()
        worker.close()

    def test_sample_rate_device_failure_keeps_old_settings(self):
        worker = Worker(EXE, FIRMWARE, FACTORY, self.saved, "blazie_nonexistent_audio_device")
        self.addCleanup(worker.abort)
        with self.assertRaises(WorkerError):
            worker.request("AUDIO 48000 auto 0 0 0 0", "OK")
        worker.request("AUDIO", "AUDIO 22050 long 3 1 1 1")
        worker.close()

    def test_sample_rate_reload_failure_keeps_old_unit(self):
        firmware = Path(self.tmp.name) / "copy.BNS"
        firmware.write_bytes(FIRMWARE.read_bytes())
        worker = Worker(EXE, firmware, FACTORY, self.saved, "-")
        self.addCleanup(worker.abort)
        firmware.unlink()  # The current unit has it in memory; recreating it must fail safely.
        with self.assertRaises(WorkerError):
            worker.request("AUDIO 48000 auto 0 0 0 0", "OK")
        worker.request("AUDIO", "AUDIO 22050 long 3 1 1 1")
        worker.request("RESUME", "RUNNING")
        worker.request("PAUSE", "PAUSED")
        worker.close()

    def test_firmware_braille_directory_and_help_file(self):
        worker = self.start()
        worker.request("BRAILLE", "BRAILLE")
        self.assertIsNone(worker.braille)  # No invented text before the firmware's first display latch.
        worker.request("RESUME", "RUNNING")
        time.sleep(3)
        worker.request("BRAILLE", "BRAILLE")
        self.assertEqual(len(worker.braille), 18)
        # The supplied factory state starts at file 0, help: number-sign, j (zero), space, h e l p.
        self.assertEqual(worker.braille[:7], bytes.fromhex("3c1a001311070f"))
        for chord in (0x55, 0x75, 0x51):  # Options, cancel, E-chord to open help.
            worker.send("KEY 0 {chord}".format(chord=chord))
            time.sleep(0.7)
        worker.request("BRAILLE", "BRAILLE")
        # The actual help text starts with 'braille lite', in the firmware's dot patterns.
        self.assertEqual(worker.braille[:12], bytes.fromhex("0317010a07071100070a1e11"))
        first = worker.braille
        # Short physical taps: press and release can arrive in the same pipe read.
        # The firmware must see its port B contacts, not a synthetic keyboard chord.
        for bars in (1, 2, 1, 2):
            worker.send("BARS {bars}".format(bars=bars))
            worker.send("BARS 0")
            time.sleep(0.7)
            worker.request("BRAILLE", "BRAILLE")
            if bars == 1:
                self.assertNotEqual(worker.braille, first)
            else:
                self.assertEqual(worker.braille, first)
        # Pausing releases a held bar and drops pending input before entering host menus.
        worker.send("BARS 1")
        time.sleep(0.7)
        worker.request("PAUSE", "PAUSED")
        worker.request("RESUME", "RUNNING")
        worker.send("BARS 2")
        worker.send("BARS 0")
        time.sleep(0.7)
        worker.request("BRAILLE", "BRAILLE")
        self.assertEqual(worker.braille, first)
        worker.send("BARS 1\nPAUSE")
        worker.wait_for("PAUSED")
        worker.request("RESUME", "RUNNING")
        time.sleep(0.7)
        worker.request("BRAILLE", "BRAILLE")
        self.assertEqual(worker.braille, first)
        worker.request("PAUSE", "PAUSED")
        frame = worker.braille
        time.sleep(0.1)
        worker.request("BRAILLE", "BRAILLE")
        self.assertEqual(worker.braille, frame)
        worker.close()

    def test_spanish_firmware_has_its_own_display_and_saved_memory(self):
        from profiles import BY_KEY
        paths = BY_KEY["bl-es"].paths(FIRMWARE.parent, Path(self.tmp.name))
        if not paths[0].exists() or not paths[1].exists():
            self.skipTest("Spanish firmware not supplied")
        english = self.start()
        english.request("SAVE", "SAVED")
        before = self.saved.read_bytes()
        spanish = Worker(EXE, *paths, "-")
        self.addCleanup(spanish.abort)
        spanish.request("RESUME", "RUNNING")
        time.sleep(3)
        spanish.request("BRAILLE", "BRAILLE")
        self.assertEqual(len(spanish.braille), 18)
        spanish.close()
        self.assertTrue(paths[2].is_file())
        self.assertEqual(self.saved.read_bytes(), before)
        english.close()

    def test_type_n_speak_cold_start_keyboard_and_save(self):
        from profiles import BY_KEY
        for key, yes in (("tns-en", "y"), ("tns-es", "s")):
            with self.subTest(key=key):
                paths = BY_KEY[key].paths(FIRMWARE.parent, Path(self.tmp.name))
                if not paths[0].exists():
                    self.skipTest("Type 'n Speak firmware not supplied")
                worker = Worker(EXE, *paths, "-", kind="tns")
                self.addCleanup(worker.abort)
                worker.request("RESUME", "RUNNING")
                time.sleep(1)
                worker.request("BRAILLE", "BRAILLE")
                self.assertIsNone(worker.braille)
                worker.request("TNS " + yes, "OK")
                time.sleep(1)
                worker.request("PAUSE", "PAUSED")
                with self.assertRaisesRegex(WorkerError, "Unknown"):
                    worker.request("TNS nonexistent", "OK")
                worker.close()
                self.assertTrue(paths[2].is_file())
                if key == "tns-en":
                    # The yes key reached the real firmware: it advanced to the confirmation question.
                    result = subprocess.run([
                        str(ROOT / "build/linux/blazie_emu"), "--null", "--unit", key,
                        "--firmware", str(FIRMWARE.parent), "--state", str(paths[2]),
                        "--seconds", "0.1", "--ram-has", "are you sure",
                    ], capture_output=True, text=True, timeout=30, check=True)
                    self.assertIn('ram has "are you sure": yes', result.stdout)


if __name__ == "__main__":
    unittest.main()
