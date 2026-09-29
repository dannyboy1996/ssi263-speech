"""No machine paths inside the built native binaries, and bl.dll reproducible from the add-on's source zip.

    python tools/check_binary_paths.py

1. Every built .dll / .exe / .so under nvda/dist (the blazie library and the add-on builds) is searched for a
   drive-letter path, a Windows or POSIX home folder, or a "\\git\\" checkout path.  z180emu's z180.c keeps
   __FILE__ in the core, so build_board.py and build_linux.sh map the checkout's path to "." (-fmacro-prefix-map).
2. bl.dll (x64) is rebuilt from the add-on's z180emu-source.zip exactly as its BUILD-bl.txt says, and must
   reproduce both golden vectors bit for bit: the zip is the complete corresponding source (GPL), shown by use.
   (It is not byte-identical to the shipped DLL; the two builds differ in their source paths.)
Exit 1 on either failure.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path[:0] = [REPO, os.path.join(REPO, "src", "csrc", "blazie")]
from tools import repo_paths            # noqa: E402
import build_board as B                 # noqa: E402

MACHINE = re.compile(rb"(?i)[a-z]:[\\/](?:[ -~]{0,80})|\\users\\|/home/|/Users/|\\git\\")
DIST = os.path.join(REPO, "nvda", "dist")
ENG = os.path.join(DIST, "blazie-build", "synthDrivers", "_ssi263_blazie")


def main():
    bad = 0
    files = [f for pat in ("blazie-lib/**/*.dll", "blazie-lib/*.exe", "*-build/**/*.dll", "*-build/**/*.exe",
                           "linux/**/*.so")
             for f in glob.glob(os.path.join(DIST, pat), recursive=True)]
    for f in sorted(set(files)):
        data = open(f, "rb").read()
        hits = sorted(set(m.group().decode("latin-1")[:90] for m in MACHINE.finditer(data)))
        # system DLL names and format strings are not paths into this machine; only report path-like hits
        hits = [h for h in hits if re.search(r"(?i)git|users|home|z180", h)]
        if hits:
            bad += 1
            print("MACHINE PATH in %s: %s" % (os.path.relpath(f, REPO), hits[:3]))
    print("%d native binaries searched, %d with a machine path" % (len(set(files)), bad))

    z = zipfile.ZipFile(os.path.join(ENG, "z180emu-source.zip"))
    d = tempfile.mkdtemp()
    try:
        z.extractall(d)
        shutil.copy(os.path.join(REPO, "src", "ssi263", "_bin", "x64", "ssi263.dll"), d)
        bindir = repo_paths.bin_dir("W64DEVKIT", path_fallback=True)
        env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
        r = subprocess.run([os.path.join(bindir, "gcc.exe")] + B.DLL_FLAGS
                           + ["-I.", "-Iz180", "-o", "bl.dll", "blazie/bl_unity.c", "blazie/bl_host.c",
                              "blazie/bl_voice.c", "ssi263.dll"], cwd=d, env=env, capture_output=True, text=True)
        ok = r.returncode == 0
        tools = os.path.join(REPO, "nvda", "tools")
        for lang in ("en", "es") if ok else ():
            g = subprocess.run([sys.executable, "bns_equiv.py", os.path.join(d, "bl.dll"), "--native",
                                "--against=" + os.path.join(tools, "golden", "blazie_%s.txt" % lang)]
                               + (["--es"] if lang == "es" else []), cwd=tools, capture_output=True, text=True,
                               env=dict(os.environ, PYTHON_COLORS="0"))
            ok = ok and g.returncode == 0
        print("bl.dll rebuilt from the add-on's source zip: %s" % (
            "reproduces both goldens bit for bit" if ok else "FAILED (%s)" % r.stderr[-200:]))
        bad += not ok
    finally:
        shutil.rmtree(d, ignore_errors=True)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
