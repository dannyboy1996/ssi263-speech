"""Locate build-tree, unpacked-package, or installed BT runtime files."""

import sys
from pathlib import Path


def default_paths(entry: Path | None = None) -> tuple[Path, Path]:
    executable = (entry or Path(sys.argv[0])).resolve()
    directory = executable.parent
    if directory.name == "btspeak" and directory.parent.name == "platforms":
        root = directory.parents[2]  # The source-checkout convenience launcher.
        return root / "build/linux/blazie_bt", root / "firmware/blazie"
    # Built launcher and packaged worker live side by side, like the other Linux entry points.
    backend = directory / "blazie_bt"
    if directory.name == "linux" and directory.parent.name == "build":
        return backend, directory.parents[1] / "firmware/blazie"
    return backend, directory.parent / "share/ssi263-speech"
