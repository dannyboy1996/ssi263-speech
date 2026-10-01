"""Build Speak-Out with MAME's V40 and instruction-count compatibility timing.
Runtime code is MIT + BSD; firmware remains private and separately licensed.
"""
import os
import shutil
import sys
from build_common import read_manifest, check_native, copy_engine, copy_mame_notices, build_board, rm, zip_build

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST, VERSION = read_manifest(os.path.join(HERE, "speakout"))
REPO = os.path.dirname(HERE)
ENGINE = os.path.join(REPO, "src")
FIRMWARE = os.path.join(REPO, "firmware", "gw-micro-speakout", "SPEAKOUT.HEX")
BUILD = os.path.join(HERE, "dist", "speakout-build")
OUT = os.path.join(HERE, "dist", "speakout-ssi263-%s.nvda-addon" % VERSION)


def main():
    if not os.path.isfile(FIRMWARE):
        sys.exit("firmware not found: %s" % FIRMWARE)
    build_board("speakout")
    if os.path.isdir(BUILD):
        rm(BUILD)
    sd = os.path.join(BUILD, "synthDrivers")
    eng = os.path.join(sd, "_ssi263_speakout")
    os.makedirs(eng)
    shutil.copy2(os.path.join(HERE, "speakout", "synthDrivers", "speakout.py"), sd)
    copy_engine(ENGINE, eng)
    for src, dst in (("speakout.py", "speakout_host.py"), ("speakout_v40.py", "speakout_v40.py"),
                     ("x86_api.py", "x86_api.py")):
        shutil.copy2(os.path.join(ENGINE, "hosts", src), os.path.join(eng, dst))
    for name in ("ssi263_rates.py", "ssi263_numwords.py"):
        shutil.copy2(os.path.join(HERE, "shared", name), eng)
    for arch in ("x64", "x86"):
        dest = os.path.join(eng, "bin", arch)
        os.makedirs(dest)
        dll = os.path.join(HERE, "dist", "speakout-lib", arch, "speakout_v40.dll")
        check_native(dll, arch)
        shutil.copy2(dll, dest)
    shutil.copy2(FIRMWARE, eng)
    copy_mame_notices(eng, "nec")
    with open(os.path.join(BUILD, "manifest.ini"), "w", encoding="utf-8") as f:
        f.write(MANIFEST)
    zip_build(BUILD, OUT)


if __name__ == "__main__":
    main()
