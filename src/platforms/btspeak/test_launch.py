"""Build and relocation checks without importing BTSpeak or taking the device keyboard."""

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

from runtime_paths import default_paths

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("build_bt_frontend", ROOT / "tools/build_bt_frontend.py")
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)


class LauncherTests(unittest.TestCase):
    def test_existing_artifact_check_sees_inside_executable_zipapp(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "blazie_emu_bt"
            BUILDER.build(executable)
            command = [sys.executable, str(ROOT / "tools/check_no_gpl.py"), str(executable)]
            subprocess.run(command, capture_output=True, text=True, timeout=10, check=True)
            with zipfile.ZipFile(executable, "a") as archive:
                archive.writestr("ucmini.py", "# Test control: a forbidden legacy payload name.\n")
            result = subprocess.run(command, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("ucmini.py", result.stdout)

    def test_source_build_and_package_paths_ignore_working_directory(self):
        with tempfile.TemporaryDirectory(prefix="bt paths ") as tmp:
            root = Path(tmp)
            self.assertEqual(default_paths(root / "src/platforms/btspeak/blazie"),
                             (root / "build/linux/blazie_bt", root / "firmware/blazie"))
            self.assertEqual(default_paths(root / "build/linux/blazie_emu_bt"),
                             (root / "build/linux/blazie_bt", root / "firmware/blazie"))
            self.assertEqual(default_paths(root / "bin/blazie_emu_bt"),
                             (root / "bin/blazie_bt", root / "share/ssi263-speech"))
            (root / "bin").mkdir()
            (root / "shortcut").symlink_to(root / "bin/blazie_emu_bt")
            self.assertEqual(default_paths(root / "shortcut"), default_paths(root / "bin/blazie_emu_bt"))

    def test_archive_is_reproducible_and_runs_without_source_tree(self):
        with tempfile.TemporaryDirectory(prefix="bt package ") as tmp:
            root = Path(tmp)
            executable = root / "bin/blazie_emu_bt"
            BUILDER.build(executable)
            original = executable.read_bytes()
            BUILDER.build(executable)
            self.assertEqual(executable.read_bytes(), original)
            self.assertTrue(os.access(executable, os.X_OK))
            with zipfile.ZipFile(executable) as archive:
                self.assertEqual(set(archive.namelist()), set(BUILDER.MODULES) | {"__main__.py"})
                self.assertFalse(any(ROOT.as_posix().encode() in archive.read(name) for name in archive.namelist()))
            result = subprocess.run([str(executable), "--help"], cwd=root, capture_output=True, text=True, timeout=10, check=True)
            self.assertIn("Original Blazie firmware", result.stdout)
            self.assertIn("--unit", result.stdout)
            # Move it, just as a user unpacks a release somewhere else.
            moved = root / "moved/bin/blazie_emu_bt"
            moved.parent.mkdir(parents=True)
            executable.replace(moved)
            subprocess.run([str(moved), "--help"], cwd="/tmp", capture_output=True, timeout=10, check=True)


if __name__ == "__main__":
    unittest.main()
