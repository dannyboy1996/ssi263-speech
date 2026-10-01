"""Build Accent Mini/SA with MAME's 8086/8085 (MIT + BSD; Aicom's firmware notice retained)."""
import os
import shutil
import sys
from build_common import read_manifest, check_native, copy_engine, copy_mame_notices, build_board, rm, zip_build

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST, VERSION = read_manifest(os.path.join(HERE, "accent"))
REPO = os.path.dirname(HERE)
ENGINE = os.path.join(REPO, "src")
DRIVER = os.path.join(REPO, "firmware", "aicom-accent-mini", "SPKEMS.DVC")
SA_ROMS = os.path.join(REPO, "firmware", "aicom-accent-sa")
BUILD = os.path.join(HERE, "dist", "accent-build")
OUT = os.path.join(HERE, "dist", "accent-ssi263-%s.nvda-addon" % VERSION)


def write_state(path):
    """Generate INIT state with the shipping CPU, tagged by the DOS driver's hash."""
    import hashlib
    import pickle
    sys.path.insert(0, ENGINE)
    from hosts.accent import Accent
    from ssi263.native import SSI263C
    state = Accent(DRIVER, chip=SSI263C(), core="mame").init_state()
    state["dvc_sha256"] = hashlib.sha256(open(DRIVER, "rb").read()).hexdigest()
    with open(path, "wb") as f:
        pickle.dump(state, f, protocol=4)


def main():
    if not os.path.isfile(DRIVER):
        sys.exit("driver not found: %s" % DRIVER)
    build_board("blazie")  # also builds pc86.dll
    build_board("accentsa")
    if os.path.isdir(BUILD):
        rm(BUILD)
    sd = os.path.join(BUILD, "synthDrivers")
    eng = os.path.join(sd, "_ssi263_accent")
    os.makedirs(eng)
    shutil.copy2(os.path.join(HERE, "accent", "synthDrivers", "accentmini.py"), sd)
    copy_engine(ENGINE, eng)
    for src, dst in (("accent.py", "accent_host.py"), ("accent_sa.py", "accent_sa_host.py"),
                     ("accent_sa_c.py", "accent_sa_c.py"), ("pc86.py", "pc86.py"), ("x86_api.py", "x86_api.py")):
        shutil.copy2(os.path.join(ENGINE, "hosts", src), os.path.join(eng, dst))
    for arch in ("x64", "x86"):
        dest = os.path.join(eng, "bin", arch)
        os.makedirs(dest)
        for folder, name, imports in (("blazie-lib", "pc86.dll", ()), ("accentsa-lib", "accent_sa.dll", ("ssi263.dll",))):
            dll = os.path.join(HERE, "dist", folder, arch, name)
            check_native(dll, arch, imports=imports)
            shutil.copy2(dll, dest)
    os.makedirs(os.path.join(eng, "accent-sa"))
    for name in ("u2.BIN", "u3.BIN", "u4.BIN"):
        shutil.copy2(os.path.join(SA_ROMS, name), os.path.join(eng, "accent-sa", name))
    shutil.copy2(os.path.join(REPO, "firmware", "AICOM.txt"), eng)
    for name in ("ssi263_numwords.py", "ssi263_rates.py"):
        shutil.copy2(os.path.join(HERE, "shared", name), eng)
    shutil.copy2(DRIVER, eng)
    write_state(os.path.join(eng, "SPKEMS.state"))
    copy_mame_notices(eng, "i86", "i8085")
    with open(os.path.join(BUILD, "manifest.ini"), "w", encoding="utf-8") as f:
        f.write(MANIFEST)
    zip_build(BUILD, OUT)


if __name__ == "__main__":
    main()
