"""An APK must carry no Braille Lite firmware -- the app is the one place it can NOT ship: no firmware or state by
name, no ROM image by content (F3 C3 xx xx FF "COPYRIGHT") and no unit state by content (the 786432 bytes every
state has: it holds what the firmware wrote) in any file, the source archive's members included.

    python check_apk_no_firmware.py <apk>...        prints what it looked at; exit 1 on any hit
    python check_apk_no_firmware.py --control <BL2ENG.BNS> <apk>
                                                    the control: the firmware added under a bland name; must FAIL
    python check_apk_no_firmware.py --control <bl2_2003_warm.state> <apk>
                                                    ... and a state under a bland name; must FAIL
"""
import io
import re
import sys
import tarfile
import zipfile

IMAGE = re.compile(rb"\xF3\xC3..\xFFCOPYRIGHT", re.S)
NAMES = re.compile(r"(?i)\.(bns|tns|state)$")
STATE_SIZE = 786432                         # bl_save_state's: battery-backed RAM + file flash


def scan(label, data, hits, depth=0):
    if IMAGE.search(data):
        hits.append("%s: a ROM image" % label)
    if depth > 0 and len(data) == STATE_SIZE:
        hits.append("%s: a unit's state, by its size" % label)
    if depth < 2 and data[:4] == b"PK\x03\x04":
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for n in z.namelist():
                if NAMES.search(n):
                    hits.append("%s!%s: a firmware or state file by name" % (label, n))
                scan("%s!%s" % (label, n), z.read(n), hits, depth + 1)
    elif depth < 2 and data[:2] == b"\x1f\x8b":
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
            for m in t.getmembers():
                if m.isfile():
                    if NAMES.search(m.name):
                        hits.append("%s!%s: a firmware or state file by name" % (label, m.name))
                    scan("%s!%s" % (label, m.name), t.extractfile(m).read(), hits, depth + 1)


def with_firmware(data, firmware):
    """The control: the APK's entries plus the firmware under a name that says nothing, as a bundled build had it."""
    out = io.BytesIO()
    with zipfile.ZipFile(io.BytesIO(data)) as src, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as dst:
        for n in src.namelist():
            dst.writestr(n, src.read(n))
        dst.writestr("assets/data/unit.bin", open(firmware, "rb").read())
    return out.getvalue()


def main():
    args = sys.argv[1:]
    control = None
    if args[:1] == ["--control"]:            # --control <firmware .BNS or .state> <apk>: this run must FAIL
        control, args = args[1], args[2:]
    bad = 0
    for apk in args:
        hits = []
        data = open(apk, "rb").read()
        if control:
            data = with_firmware(data, control)
        scan(apk.replace("\\", "/").split("/")[-1], data, hits)
        n = len(zipfile.ZipFile(io.BytesIO(data)).namelist())
        print("%s: %d entries, %s" % (apk.replace("\\", "/").split("/")[-1], n,
                                     "no firmware" if not hits else "FIRMWARE: " + "; ".join(hits[:5])))
        bad += bool(hits)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
