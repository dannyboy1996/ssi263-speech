#!/usr/bin/env python3
"""Bundle the BT frontend as an executable Python zipapp, with no firmware or device libraries.

    python3 tools/build_bt_frontend.py [output]

The native worker is built by build_linux.sh. The zipapp uses the device's Python 3.11+
and installed BTSpeak package; it can also show --help on ordinary development machines.
"""

import argparse
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULES = (
    "frontend.py", "worker.py", "keymap.py", "display.py", "preferences.py", "profiles.py",
    "tns_keyboard.py", "audio_options.py", "audio_menu.py", "runtime_paths.py",
)


def build(output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + ".", delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(b"#!/usr/bin/env python3\n# blazie-flags: self-voice\n")
            # Explicit runtime modules and fixed metadata keep the archive reproducible.
            with zipfile.ZipFile(stream, mode="w", compression=zipfile.ZIP_DEFLATED) as archive:
                files = {"__main__.py": b"from frontend import main\nraise SystemExit(main())\n"}
                files.update({name: (ROOT / "src/platforms/btspeak" / name).read_bytes() for name in MODULES})
                for name, data in files.items():
                    info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
                    info.compress_type = zipfile.ZIP_DEFLATED
                    info.external_attr = 0o644 << 16
                    archive.writestr(info, data)
            stream.flush()
            temporary.chmod(0o755)
            temporary.replace(output)
        finally:
            temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", type=Path, default=ROOT / "build/linux/blazie_emu_bt")
    build(parser.parse_args().output)
