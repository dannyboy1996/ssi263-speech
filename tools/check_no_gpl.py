"""No z180emu (GPL) engine and no Unicorn engine in a shipping artifact: the Linux library and package, the APK, the
wheel (and the NVDA add-ons, the same way).

    python tools/check_no_gpl.py ARTIFACT...     each a file (.so .dll .exe .o .apk .whl .zip .tar.gz, anything) or a
                                                 folder (walked); archives are opened and searched member by member,
                                                 archives inside archives too (the APK's assets, a wheel in a zip)
    python tools/check_no_gpl.py --control       the must-fail control: a fake APK holding the genuine legacy payloads'
                                                 marks -- a library with z180emu's daisy chain and Unicorn's API,
                                                 unicorn.dll, ucmini.py, i8085.py and a GPL licence -- this run must FAIL
    python tools/check_no_gpl.py --clean-sample  the same fake APK with only what is NOT evidence (below): must pass

0.7 ships every unit on MAME's extracted cores (BSD-3-Clause, notices kept) and the project's own MIT code (Tomi,
2026-09-30): z180emu (GPL-2.0-or-later) remains only as a development reference (LEGACY=1 ./build_linux.sh), and
Unicorn not at all.  Only real evidence counts (Astra, Reply 127) -- a bare word in a comment, a development
selector's name or a MAME board's compatibility counter (`so_run_steps_unicorn`) is not:
- in a BINARY (ELF, PE, Mach-O, an object or a static archive; symbols and strings both, so a stripped library is
  caught by what stripping keeps): z180emu's engine -- its adapter (z180_legacy, z180_run_legacy), its core's entry
  points (cpu_create_z180, cpu_execute_z180, z80_daisy_chain_init), its daisy chain's source name and assert
  ("z80daisy.c", "daisy != NULL") and its Z80 SCC's register texts -- and Unicorn's engine: its API as whole
  identifiers (uc_open, uc_emu_start, uc_mem_map, uc_reg_read, uc_reg_write) and its error names (UC_ERR_...);
- in Python: an import of Unicorn (`import unicorn`, `from unicorn ...`);
- in the INVENTORY (every member's name): unicorn.dll / libunicorn.so* / a unicorn/ package, ucmini.py (the Unicorn
  host), i8085.py (the Python 8085 the C board replaced), anything named z180emu, a COPYING file;
- in a NOTICE (LICENSE*, COPYING*, NOTICE*, DISTRIBUTION*, a licenses/ folder, a wheel's METADATA, the app's dex
  where its licence text lives): the GPL ("General Public License", GPL-2.0, GPLv2).
An artifact with nothing in it to search fails too (a missing build never passes).  Exit 1 on any hit.
"""
import io
import os
import re
import sys
import tarfile
import tempfile
import zipfile

BINARY_MARKS = [
    ("z180emu engine", re.compile(rb"z180_legacy|z180_run_legacy|cpu_create_z180|cpu_execute_z180|"
                                  rb"z80_daisy_chain_init|z80daisy\.c|daisy != NULL|"
                                  rb"Sync Characters or SDLC Address Field|External/Status Interrupt Control")),
    ("Unicorn engine", re.compile(rb"(?<![A-Za-z0-9_])(?:uc_open|uc_emu_start|uc_mem_map|uc_reg_read|uc_reg_write|"
                                  rb"UC_ERR_[A-Z]+)(?![A-Za-z0-9_])")),
]
PY_MARK = ("Unicorn import", re.compile(rb"(?m)^[ \t]*(?:import[ \t]+unicorn\b|from[ \t]+unicorn\b)"))
NOTICE_MARK = ("GPL notice", re.compile(rb"(?i)General Public License|\bGPL-?2\.0|\bGPLv2"))
BAD_NAME = re.compile(r"(?i)(^|/)(lib)?unicorn(\.dll|\.so(\.[0-9.]+)?|\.dylib|/)|"
                      r"(^|/)(ucmini|i8085)(\.py|\.cpython[^/]*\.pyc)$|z180emu|(^|/)COPYING")
NOTICE_NAME = re.compile(r"(?i)(^|/)(LICEN[CS]E|COPYING|NOTICE|DISTRIBUTION)[^/]*$|(^|/)licen[cs]es/|"
                         r"\.dist-info/METADATA$|(^|/)classes[0-9]*\.dex$")
BINARY_MAGIC = (b"\x7fELF", b"MZ", b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa", b"!<arch>\n",
                b"\x4c\x01", b"\x64\x86")
ARCHIVE = (".zip", ".pyz", ".apk", ".whl", ".jar", ".aar", ".nvda-addon", ".tar.gz", ".tgz", ".tar")


def is_zip(data):
    # Executable Python zipapps have a shebang before the ZIP header, and often no extension.
    return data[:4] == b"PK\x03\x04" or (data.startswith(b"#!") and zipfile.is_zipfile(io.BytesIO(data)))


def is_archive(name, data):
    """By name or by content: a zip (an APK, a wheel, an NVDA add-on) or a gzip'd tar."""
    return name.lower().endswith(ARCHIVE) or is_zip(data) or data[:2] == b"\x1f\x8b"


def members(data):
    """(member name, bytes) of an archive held in memory."""
    if not is_zip(data):
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            for m in t.getmembers():
                if m.isfile():
                    yield m.name, t.extractfile(m).read()
    else:
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for n in z.namelist():
                if not n.endswith("/"):
                    yield n, z.read(n)


def kind_marks(name, data):
    """The marks that are evidence in this kind of file."""
    marks = []
    if data.startswith(BINARY_MAGIC) or name.lower().endswith((".so", ".dll", ".exe", ".o", ".obj", ".a", ".dylib")):
        marks += BINARY_MARKS
    if name.lower().endswith(".py"):
        marks.append(PY_MARK)
    if NOTICE_NAME.search(name):
        marks.append(NOTICE_MARK)
    return marks


def scan(name, data, hits, count):
    """Search one file (recursing into archives); returns the number of files searched."""
    inner = name.replace("\\", "/").split("!")[-1]
    if BAD_NAME.search(inner):
        hits.append("%s: a legacy payload by name" % name)
    if is_archive(name, data):
        try:
            for n, d in members(data):
                count = scan(name + "!" + n, d, hits, count)
            return count
        except (zipfile.BadZipFile, tarfile.TarError) as e:
            hits.append("%s: not a readable archive (%s)" % (name, e))
            return count
    for what, rx in kind_marks(inner, data):
        m = rx.search(data)
        if m:
            hits.append("%s: %s (%r)" % (name, what, m.group(0).decode("latin-1").strip()))
    return count + 1


def check(path):
    hits, count = [], 0
    if os.path.isdir(path):
        for root, dirs, names in os.walk(path):
            dirs.sort()
            for n in sorted(names):
                p = os.path.join(root, n)
                with open(p, "rb") as f:
                    count = scan(os.path.relpath(p, path).replace(os.sep, "/"), f.read(), hits, count)
    elif os.path.isfile(path):
        with open(path, "rb") as f:
            count = scan(os.path.basename(path), f.read(), hits, count)
    else:
        hits.append("missing")
    if count == 0 and not hits:
        hits.append("nothing to search")
    return hits, count


def clean_members():
    """What is NOT evidence (Astra, Reply 127): a MAME board's compatibility counter, our own comments and selector
    names, an MIT notice."""
    return [("lib/arm64-v8a/libspeakout_v40.so",
             b"\x7fELF" + b"\0" * 64 + b"so_run_steps_unicorn\0z180_mame.cpp\0" + b"\0" * 64),
            ("hosts/speakout_host.py", b"# 0.6 ran this on Unicorn; z180emu is a development reference only\n"
                                       b"CORES = ('mame', 'unicorn')  # the development selector's names\n"),
            ("assets/licenses/ssi263-speech-MIT.txt", b"MIT License\n\nCopyright (c) 2026 Tamas Geczy\n")]


def control_artifact(folder, clean=False):
    """A fake APK: the clean members, and (unless `clean`) the genuine legacy payloads' marks -- a stripped z180emu
    library keeps its daisy chain's assert text, Unicorn's library its API -- unicorn.dll, ucmini.py, i8085.py, the
    GPL's text among the licences, and an executable Python zipapp (a shebang before its zip, no extension, as the
    BT frontend's bin/blazie_emu_bt) with a module importing Unicorn: seen only if the zipapp is opened."""
    apk = os.path.join(folder, "clean.apk" if clean else "control.apk")
    zipapp = io.BytesIO()
    with zipfile.ZipFile(zipapp, "w") as z:
        z.writestr("frontend.py", "import unicorn\n")
    with zipfile.ZipFile(apk, "w") as z:
        for name, data in clean_members():
            z.writestr(name, data)
        if not clean:
            z.writestr("lib/arm64-v8a/libssi263speech.so", b"\x7fELF" + b"\0" * 64 + b"z80_daisy_chain_init\0"
                       b"daisy != NULL\0uc_open\0uc_emu_start\0" + b"\0" * 64)
            z.writestr("lib/x86/unicorn.dll", b"MZ" + b"\0" * 64)
            z.writestr("hosts/ucmini.py", "import unicorn\n")
            z.writestr("hosts/i8085.py", "# the Python 8085\n")
            z.writestr("hosts/__pycache__/ucmini.cpython-37.pyc", b"\x42\x0d\x0d\x0a")
            z.writestr("assets/licenses/third-party.txt", "GNU GENERAL PUBLIC LICENSE\nVersion 2, June 1991\n")
            z.writestr("bin/blazie_emu_bt", b"#!/usr/bin/env python3\n" + zipapp.getvalue())
    return apk


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    tmp = None
    if args in (["--control"], ["--clean-sample"]):
        tmp = tempfile.TemporaryDirectory()
        args = [control_artifact(tmp.name, clean=args == ["--clean-sample"])]
    bad = 0
    for a in args:
        label = os.path.basename(a.rstrip("/\\")) or a
        hits, count = check(a)
        if hits:
            bad += 1
            for h in hits[:20]:
                print("FAIL %s: %s" % (label, h))
            if len(hits) > 20:
                print("FAIL %s: ... %d more" % (label, len(hits) - 20))
        else:
            print("ok   %s: no z180emu or Unicorn engine, no GPL notice (%d files searched)" % (label, count))
    if tmp:
        tmp.cleanup()
    print("no-GPL audit: %s" % ("%d of %d FAILED" % (bad, len(args)) if bad else "%d clean" % len(args)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
