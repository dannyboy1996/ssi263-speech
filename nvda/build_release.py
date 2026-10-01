"""Build all three NVDA packages together, sharing native builds and chip checks."""
import os
import subprocess
import sys

import build_blazie
import build_accent
import build_speakout
from build_common import build_native_voices


def main():
    builders = (build_blazie, build_accent, build_speakout)
    versions = {b.VERSION for b in builders}
    if len(versions) != 1:
        raise SystemExit("Release manifests disagree: %s" % versions)
    build_native_voices()
    for builder in builders:
        builder.main()
    subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "tools", "check_native_packages.py")],
                   check=True)


if __name__ == "__main__":
    main()
