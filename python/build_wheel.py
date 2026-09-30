#!/usr/bin/env python3
"""Build the ssi263speech wheel from already-built native libraries (after TGSpeechBox's python/build_wheel.py).

    python python/build_wheel.py --lib-dir nvda/dist/blazie-lib/x64 --chip-dir src/ssi263/_bin/x64 --plat win_amd64
    python python/build_wheel.py --lib-dir build/linux --plat manylinux_2_35_x86_64

Packs the package (python/ssi263speech) and the libraries -- Windows: ssi263.dll (the chip) + bl.dll (the Blazie
board, host and voice); Linux: libssi263speech.so -- into dist/ssi263speech-<version>-py3-none-<plat>.whl.  Standard
library only: a wheel is a zip with METADATA, WHEEL and RECORD.  The package loads the libraries with ctypes, so one
wheel serves every Python 3 on its platform.  No firmware goes in: the Braille Lite voice takes the user's own.

Licence: the chip is MIT; the board library carries z180emu (GPL-2.0-or-later), so a wheel holding it is GPL.
"""
import argparse
import base64
import hashlib
import os
import re
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(REPO, "python", "ssi263speech")
WINDOWS_LIBS = ("ssi263.dll", "bl.dll")
LINUX_LIBS = ("libssi263speech.so",)


def version():
    text = open(os.path.join(PKG, "_version.py"), encoding="utf-8").read()
    return re.search(r'__version__\s*=\s*"([^"]+)"', text).group(1)


def record_line(arcname, data):
    digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
    return "%s,sha256=%s,%d" % (arcname, digest, len(data))


def find_libs(a):
    names = WINDOWS_LIBS if a.plat.startswith("win") else LINUX_LIBS
    dirs = [d for d in (a.lib_dir, a.chip_dir) if d]
    found = []
    for n in names:
        hit = [os.path.join(d, n) for d in dirs if os.path.isfile(os.path.join(d, n))]
        if not hit:
            sys.exit("build_wheel: %s not found in %s" % (n, dirs))
        found.append(hit[0])
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lib-dir", required=True, help="the built board library (bl.dll or libssi263speech.so)")
    ap.add_argument("--chip-dir", help="Windows: where ssi263.dll is, if not in --lib-dir")
    ap.add_argument("--plat", required=True, help="win_amd64, win32, manylinux_2_35_x86_64, manylinux_2_39_aarch64")
    ap.add_argument("--out", default=os.path.join(REPO, "dist"))
    a = ap.parse_args()
    ver = version()
    files = []
    for root, dirs, names in os.walk(PKG):
        dirs[:] = [d for d in dirs if d not in ("__pycache__", "_libs")]
        for n in sorted(names):
            if n.endswith(".py"):
                p = os.path.join(root, n)
                files.append(("ssi263speech/" + os.path.relpath(p, PKG).replace(os.sep, "/"), open(p, "rb").read()))
    libs = find_libs(a)
    for p in libs:
        files.append(("ssi263speech/_libs/" + os.path.basename(p), open(p, "rb").read()))
    dist_info = "ssi263speech-%s.dist-info" % ver
    readme = open(os.path.join(REPO, "python", "README.md"), encoding="utf-8").read()
    metadata = ("Metadata-Version: 2.1\n"
                "Name: ssi263speech\n"
                "Version: %s\n"
                "Summary: The emulated Votrax SSI-263 speech chip, and a Blazie Braille Lite that drives it, "
                "from Python\n"
                "Home-page: https://github.com/tgeczy/ssi263-speech\n"
                "License: GPL-2.0-or-later (the chip itself: MIT)\n"
                "Requires-Python: >=3.7\n"
                "Classifier: License :: OSI Approved :: GNU General Public License v2 or later (GPLv2+)\n"
                "Classifier: Topic :: Multimedia :: Sound/Audio :: Speech\n"
                "Description-Content-Type: text/markdown\n"
                "\n" % ver) + readme
    wheel_meta = ("Wheel-Version: 1.0\n"
                  "Generator: ssi263speech build_wheel.py\n"
                  "Root-Is-Purelib: false\n"
                  "Tag: py3-none-%s\n" % a.plat)
    files += [(dist_info + "/METADATA", metadata.encode("utf-8")),
              (dist_info + "/WHEEL", wheel_meta.encode("utf-8")),
              (dist_info + "/LICENSE", open(os.path.join(REPO, "LICENSE"), "rb").read())]
    record = "\n".join(record_line(n, d) for n, d in files) + "\n%s/RECORD,,\n" % dist_info
    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, "ssi263speech-%s-py3-none-%s.whl" % (ver, a.plat))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if "/_libs/" in name else 0o644) << 16   # libraries stay executable
            z.writestr(info, data)
        z.writestr(dist_info + "/RECORD", record)
    print("built %s: %d libraries, %d files, %d KiB" % (os.path.basename(path), len(libs), len(files),
                                                         os.path.getsize(path) // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
