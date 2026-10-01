"""0.7.0's Python Speak-Out and Accent drivers, the reference the native drivers (0.7.5) and the C voices are held
to: their add-on folders as nvda/build_speakout.py and nvda/build_accent.py made them at REV, the last commit with the
Python hosts in the add-ons, put together in nvda/tools/out/legacy-<rev>/ from that commit and this tree's native
libraries.

    python legacy_drivers.py [speakout|accent ...]      # print each one's synthDrivers folder (making it once)
    legacy_drivers.synth_drivers("speakout")            # the same, for a test

From REV (git show): the drivers, src/hosts, src/ssi263 and src/data, nvda/shared.  From this tree (not in git):
src/ssi263/_bin (the chip), nvda/dist/speakout-lib (speakout_v40.dll), nvda/dist/blazie-lib (pc86.dll),
nvda/dist/accentsa-lib (accent_sa.dll), and firmware/.  The Accent-mini's INIT snapshot (SPKEMS.state) is made as
build_accent.py made it, on MAME's 8086.  A test points fake_nvda_driver_test.py at a folder by
SSI263_SYNTH_DRIVERS.  Made once, into a temporary folder renamed into place, so parallel checks can share it.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
REV = "9bde0a76eba81be958d216c7b75edba2eb22c29f"      # main before 0.7.5's native drivers (the Python hosts' last)
OUT = os.path.join(HERE, "out", "legacy-%s" % REV[:7])
DIST = os.path.join(REPO, "nvda", "dist")
FW = os.path.join(REPO, "firmware")
ARCHES = ("x64", "x86")


def _git(*args):
    return subprocess.run(["git", "-C", REPO] + list(args), capture_output=True, check=True).stdout


def _put(rev_path, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "wb") as f:
        f.write(_git("show", "%s:%s" % (REV, rev_path)))


def _tree(rev_dir, dest, skip=("drivers.py",)):
    """every file of a folder at REV (its research-only drivers.py left out, as the build did)"""
    for path in _git("ls-tree", "-r", "--name-only", REV, rev_dir).decode().split():
        rel = os.path.relpath(path, rev_dir)
        if os.path.basename(rel) not in skip:
            _put(path, os.path.join(dest, rel))


def _copy(src, dest):
    if not os.path.isfile(src):
        sys.exit("legacy_drivers: %s is missing (build it first)" % src)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copy2(src, dest)


def _engine(eng):
    """build_common.copy_engine: the chip package with its C core, the data, the package's __init__"""
    _tree("src/ssi263", os.path.join(eng, "ssi263"))
    for arch in ARCHES:
        _copy(os.path.join(REPO, "src", "ssi263", "_bin", arch, "ssi263.dll"),
              os.path.join(eng, "ssi263", "_bin", arch, "ssi263.dll"))
    _tree("src/data", os.path.join(eng, "data"))
    with open(os.path.join(eng, "__init__.py"), "w") as f:
        f.write('"""0.7.0\'s engine package (nvda/tools/legacy_drivers.py)."""\n')
    for name in ("ssi263_rates.py", "ssi263_numwords.py"):
        _put("nvda/shared/" + name, os.path.join(eng, name))


def _speakout(build):
    sd = os.path.join(build, "synthDrivers")
    eng = os.path.join(sd, "_ssi263_speakout")
    _put("nvda/speakout/synthDrivers/speakout.py", os.path.join(sd, "speakout.py"))
    _engine(eng)
    for src, dst in (("speakout.py", "speakout_host.py"), ("speakout_v40.py", "speakout_v40.py"),
                     ("x86_api.py", "x86_api.py")):
        _put("src/hosts/" + src, os.path.join(eng, dst))
    for arch in ARCHES:
        _copy(os.path.join(DIST, "speakout-lib", arch, "speakout_v40.dll"), os.path.join(eng, "bin", arch, "speakout_v40.dll"))
    _copy(os.path.join(FW, "gw-micro-speakout", "SPEAKOUT.HEX"), os.path.join(eng, "SPEAKOUT.HEX"))


def _accent(build):
    sd = os.path.join(build, "synthDrivers")
    eng = os.path.join(sd, "_ssi263_accent")
    _put("nvda/accent/synthDrivers/accentmini.py", os.path.join(sd, "accentmini.py"))
    _engine(eng)
    for src, dst in (("accent.py", "accent_host.py"), ("accent_sa.py", "accent_sa_host.py"),
                     ("accent_sa_c.py", "accent_sa_c.py"), ("pc86.py", "pc86.py"), ("x86_api.py", "x86_api.py")):
        _put("src/hosts/" + src, os.path.join(eng, dst))
    for arch in ARCHES:
        _copy(os.path.join(DIST, "blazie-lib", arch, "pc86.dll"), os.path.join(eng, "bin", arch, "pc86.dll"))
        _copy(os.path.join(DIST, "accentsa-lib", arch, "accent_sa.dll"), os.path.join(eng, "bin", arch, "accent_sa.dll"))
    for name in ("u2.BIN", "u3.BIN", "u4.BIN"):
        _copy(os.path.join(FW, "aicom-accent-sa", name), os.path.join(eng, "accent-sa", name))
    _copy(os.path.join(FW, "aicom-accent-mini", "SPKEMS.DVC"), os.path.join(eng, "SPKEMS.DVC"))
    # build_accent.write_state: the driver's INIT on MAME's 8086, tagged with the driver's hash
    src = os.path.join(os.path.dirname(build), "src")
    code = ("import hashlib, pickle, sys; sys.path.insert(0, %r)\n"
            "from hosts.accent import Accent\nfrom ssi263.native import SSI263C\n"
            "dvc = %r\nstate = Accent(dvc, chip=SSI263C(), core='mame').init_state()\n"
            "state['dvc_sha256'] = hashlib.sha256(open(dvc, 'rb').read()).hexdigest()\n"
            "pickle.dump(state, open(%r, 'wb'), protocol=4)\n"
            % (src, os.path.join(eng, "SPKEMS.DVC"), os.path.join(eng, "SPKEMS.state")))
    env = dict(os.environ, SSI263_PC86_DLL=os.path.join(eng, "bin", "x64" if sys.maxsize > 2 ** 32 else "x86", "pc86.dll"))
    subprocess.run([sys.executable, "-c", code], check=True, env=env, capture_output=True)


def _src(dest):
    """REV's research tree (hosts, chip package), for the INIT snapshot"""
    _tree("src/hosts", os.path.join(dest, "hosts"))
    _engine(dest)


def synth_drivers(addon):
    """0.7.0's synthDrivers folder for "speakout" or "accent", made on first use"""
    final = os.path.join(OUT, "%s-build" % addon)
    if not os.path.isdir(final):
        os.makedirs(OUT, exist_ok=True)
        tmp = tempfile.mkdtemp(prefix="tmp-%s-" % addon, dir=OUT)
        try:
            build = os.path.join(tmp, "%s-build" % addon)
            if addon == "accent":
                _src(os.path.join(tmp, "src"))
            {"speakout": _speakout, "accent": _accent}[addon](build)
            try:
                os.rename(build, final)
            except OSError:
                if not os.path.isdir(final):        # another check made it meanwhile: use theirs
                    raise
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    return os.path.join(final, "synthDrivers")


if __name__ == "__main__":
    for a in sys.argv[1:] or ("speakout", "accent"):
        print(synth_drivers(a))
