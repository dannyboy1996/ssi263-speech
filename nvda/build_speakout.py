"""Build Speak-Out with MAME's V40 and instruction-count compatibility timing.
Runtime code is MIT + BSD; firmware remains private and separately licensed.
"""
import os
import shutil
import sys
from build_common import read_manifest, copy_native_binding, rm, zip_build

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST, VERSION = read_manifest(os.path.join(HERE, "speakout"))
REPO = os.path.dirname(HERE)
FIRMWARE = os.path.join(REPO, "firmware", "gw-micro-speakout", "SPEAKOUT.HEX")
BUILD = os.path.join(HERE, "dist", "speakout-build")
OUT = os.path.join(HERE, "dist", "speakout-ssi263-%s.nvda-addon" % VERSION)


def main():
    if not os.path.isfile(FIRMWARE):
        sys.exit("firmware not found: %s" % FIRMWARE)
    if os.path.isdir(BUILD):
        rm(BUILD)
    sd = os.path.join(BUILD, "synthDrivers")
    eng = os.path.join(sd, "_ssi263_speakout")
    os.makedirs(eng)
    shutil.copy2(os.path.join(HERE, "speakout", "synthDrivers", "speakout.py"), sd)
    copy_native_binding(eng)
    shutil.copy2(FIRMWARE, eng)
    with open(os.path.join(BUILD, "manifest.ini"), "w", encoding="utf-8") as f:
        f.write(MANIFEST)
    zip_build(BUILD, OUT)


if __name__ == "__main__":
    main()
