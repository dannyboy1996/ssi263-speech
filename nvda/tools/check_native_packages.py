"""Check the native add-on payloads against source, firmware and the release DLLs.

    python nvda/tools/check_native_packages.py [addon.nvda-addon ...]

With no arguments, check all three packages named by their source manifests.
Reject stale Python hosts, old DLLs, missing notices and mismatched native files.
"""
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "nvda"))
from build_common import read_manifest  # noqa: E402

CORES = ("z180", "nec", "i86", "i8085")
DRIVERS = {"blazie": "blazie", "speakout": "speakout", "accent": "accentmini"}


def inputs(name):
    eng = "synthDrivers/_ssi263_%s/" % name
    files = {
        "manifest.ini": ROOT / "nvda" / name / "manifest.ini",
        "synthDrivers/%s.py" % DRIVERS[name]: ROOT / "nvda" / name / "synthDrivers" / (DRIVERS[name] + ".py"),
        eng + "ssi263_rates.py": ROOT / "nvda/shared/ssi263_rates.py",
        eng + "licenses/LICENSE-MIT.txt": ROOT / "LICENSE",
        eng + "licenses/LICENSE-Casso-MIT.txt": ROOT / "third_party/casso/LICENSE",
    }
    for arch in ("x64", "x86"):
        files[eng + "bin/%s/ssi263speech.dll" % arch] = ROOT / "build/win" / arch / "ssi263speech.dll"
    for core in CORES:
        src = ROOT / "src/csrc/cpu" / ("mame_" + core)
        files[eng + "licenses/LICENSE-%s-BSD-3-Clause.txt" % core] = src / "LICENSE-BSD-3-Clause.txt"
        files[eng + "licenses/MAME-%s-provenance.txt" % core] = src / "PINNED.txt"
    if name != "blazie":
        files[eng + "ssi263speech.py"] = ROOT / "nvda/shared/ssi263speech.py"
    if name == "speakout":
        files[eng + "SPEAKOUT.HEX"] = ROOT / "firmware/gw-micro-speakout/SPEAKOUT.HEX"
    elif name == "accent":
        files[eng + "SPKEMS.DVC"] = ROOT / "firmware/aicom-accent-mini/SPKEMS.DVC"
        files[eng + "AICOM.txt"] = ROOT / "firmware/AICOM.txt"
        for rom in ("u2.BIN", "u3.BIN", "u4.BIN"):
            files[eng + "accent-sa/" + rom] = ROOT / "firmware/aicom-accent-sa" / rom
    else:
        for rom in ("BL2ENG.BNS", "bl2_2003_warm.state"):
            files[eng + rom] = ROOT / "firmware/blazie" / rom
        for rom in ("BL2SPA.BNS", "bl2spa_fresh.state"):
            files[eng + rom] = ROOT / "firmware/blazie/spanish" / rom
    return eng, files


def check(path):
    name = next((n for n in DRIVERS if path.name.startswith(n + "-ssi263-")), None)
    if name is None:
        raise ValueError("unrecognized add-on filename: " + path.name)
    eng, files = inputs(name)
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        if len(names) != len(set(names)):
            raise ValueError("duplicate archive members")
        for n, src in files.items():
            # The builder writes the manifest as text; source checkout line endings may differ.
            actual, wanted = z.read(n), src.read_bytes()
            if n == "manifest.ini":
                actual, wanted = actual.replace(b"\r\n", b"\n"), wanted.replace(b"\r\n", b"\n")
            if actual != wanted:
                raise ValueError("member differs from release input: " + n)
        if eng + "__init__.py" not in names:
            raise ValueError("missing private package __init__.py")
        if name != "blazie":
            extra = set(names) - set(files) - {eng + "__init__.py"}
            if extra:
                raise ValueError("unexpected native package members: " + ", ".join(sorted(extra)))
        else:
            dlls = {n for n in names if n.endswith(".dll")}
            expected = {eng + "bin/%s/ssi263speech.dll" % a for a in ("x64", "x86")}
            expected |= {eng + "ssi263/_bin/%s/ssi263.dll" % a for a in ("x64", "x86")}
            if dlls != expected:
                raise ValueError("unexpected Blazie DLL inventory: " + str(sorted(dlls)))
        forbidden = [n for n in names if any(s in n.lower() for s in ("unicorn", "z180emu", "ucmini.py", "i8085.py"))]
        if forbidden:
            raise ValueError("legacy CPU payload: " + ", ".join(forbidden))
    print("PASS %s: native payload, firmware and all four MAME notices match" % path.name)


def main():
    paths = [Path(p) for p in sys.argv[1:]] or [ROOT / "nvda/dist" / ("%s-ssi263-%s.nvda-addon" %
             (n, read_manifest(str(ROOT / "nvda" / n))[1])) for n in DRIVERS]
    bad = 0
    for path in paths:
        try:
            check(path)
        except (OSError, ValueError, KeyError, zipfile.BadZipFile) as exc:
            print("FAIL %s: %s" % (path.name, exc))
            bad += 1
    return int(bool(bad))


if __name__ == "__main__":
    sys.exit(main())
