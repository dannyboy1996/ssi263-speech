"""The emulator's disk images opened by another program, 7-Zip (Tomi: "a disk image 7-Zip and other tools open").

  1. blazie_files pack makes an image from a folder of awkward names (spaces, capitals, a long name, a non-ASCII
     one, a sub-folder); 7-Zip extracts it, and every name and byte must come back.
  2. That image goes into a Braille Lite's saved state (blazie_files import), out again (export); 7-Zip's extraction
     of the export must equal blazie_files extract of the same state: the unit's files, named and filled alike.

  python files_7zip.py BLAZIE_FILES_EXE BL_STATE [SEVEN_ZIP_EXE]

Control: TEST_FILES_FAT_BREAK=1 in the environment (blazie_files writes every long name with a wrong checksum) must
fail: 7-Zip then shows the 8.3 aliases.
"""
import os
import shutil
import subprocess
import sys
import tempfile

FAILED = []


def check(name, ok, detail):
    print("%-4s %-50s %s" % ("ok" if ok else "FAIL", name, detail))
    if not ok:
        FAILED.append(name)


def tree(root):
    """{relative path: bytes} of every file under root"""
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            with open(p, "rb") as h:
                out[os.path.relpath(p, root).replace(os.sep, "/")] = h.read()
    return out


def run(argv):
    r = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return r.returncode, r.stdout.decode("utf-8", "replace")


def diff(a, b):
    missing = sorted(set(a) - set(b))
    extra = sorted(set(b) - set(a))
    changed = sorted(k for k in set(a) & set(b) if a[k] != b[k])
    parts = []
    if missing:
        parts.append("missing " + ", ".join(missing[:4]))
    if extra:
        parts.append("extra " + ", ".join(extra[:4]))
    if changed:
        parts.append("bytes differ " + ", ".join(changed[:4]))
    return "; ".join(parts)


def main():
    tool, state = sys.argv[1], sys.argv[2]
    seven = sys.argv[3] if len(sys.argv) > 3 else shutil.which("7z") or os.path.join(os.environ.get("ProgramFiles", ""), "7-Zip", "7z.exe")
    work = tempfile.mkdtemp(prefix="files_7zip_")
    try:
        src = os.path.join(work, "src")
        files = {
            "ram startup/Report Final.txt": b"first line\r\nsecond line\r\n",
            "ram startup/notes": b"braille text\r",
            "flash startup/A Rather Long Book Name.brf": bytes(range(256)) * 9,
            "work/\u00fcbung.txt": "caf\u00e9 \u00f1and\u00fa\r".encode("cp850"),
            "top level.TXT": b"",
        }
        for rel, data in files.items():
            p = os.path.join(src, *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as h:
                h.write(data)
        img = os.path.join(work, "packed.img")
        rc, out = run([tool, "pack", src, img])
        x1 = os.path.join(work, "x1")
        rc7, out7 = run([seven, "x", img, "-o" + x1, "-y", "-bso0", "-bsp0"])
        d = diff(files, tree(x1)) if rc == 0 and rc7 == 0 else "pack %d, 7-Zip %d: %s" % (rc, rc7, (out + out7)[-200:])
        check("7-Zip extracts a packed image exactly", not d, d or "%d files, names and bytes" % len(files))

        unit = os.path.join(work, "unit.state")
        shutil.copyfile(state, unit)
        rc, out = run([tool, "import", unit, img])
        exp = os.path.join(work, "export.img")
        rc2, out2 = run([tool, "export", unit, exp])
        x2, x3 = os.path.join(work, "x2"), os.path.join(work, "x3")
        rc7, out7 = run([seven, "x", exp, "-o" + x2, "-y", "-bso0", "-bsp0"])
        rc3, out3 = run([tool, "extract", unit, x3])
        ok = rc == 0 and rc2 == 0 and rc7 == 0 and rc3 == 0
        a, b = tree(x2), tree(x3)
        d = diff(b, a) if ok else "import %d, export %d, 7-Zip %d, extract %d: %s" % (
            rc, rc2, rc7, rc3, (out + out2 + out7 + out3)[-300:])
        check("7-Zip reads a unit's export as the unit holds it", ok and not d and len(a) >= 5,
              d or "%d files, names and bytes" % len(a))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("FAILED" if FAILED else "all passed")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
