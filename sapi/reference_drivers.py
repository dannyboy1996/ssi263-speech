"""The SAPI tests' reference: sapi/ssi_serve.py over 0.7.0's Python drivers (Astra, Reply 141).

ssi_serve.py runs the NVDA add-ons' drivers from nvda/dist/*-build by default, and since 0.7.5 those are the native
drivers (ctypes over ssi263speech.dll, the library the native SAPI voices are), so the reference would be the thing it
is compared with.  test_serve.py, test_native.py and test_sapi_engine.py run it on nvda/tools/legacy_drivers.py's
0.7.0 folders instead (SSI263_SAPI_DRIVERS), with one environment, env(), for every reference server they start; and
before anything else guard() asks a server with that same environment where its drivers come from (ssi_serve.py
--drivers): the three driver modules and every synthDrivers module must lie in the legacy folder, and no DLL it
loaded from this repository may lie anywhere else or be ssi263speech.dll.  If not, the test stops there and fails.

    SSI263_SAPI_REF_BREAK=dist   control: the reference on nvda/dist's drivers (the native ones) -- the guard FAILS
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "nvda", "tools"))
import legacy_drivers  # noqa: E402

ADDONS = ("blazie", "speakout", "accent")
MODULES = ("synthDrivers.blazie", "synthDrivers.speakout", "synthDrivers.accentmini")
NATIVE = "ssi263speech.dll"


def env():
    """The reference servers' environment: 0.7.0's drivers (made once), or nvda/dist's for the control."""
    for a in ADDONS:
        legacy_drivers.synth_drivers(a)
    root = legacy_drivers.OUT
    if os.environ.get("SSI263_SAPI_REF_BREAK") == "dist":
        root = os.path.join(REPO, "nvda", "dist")
    return dict(os.environ, SSI263_SAPI_DRIVERS=root)


def _norm(p):
    return os.path.normcase(os.path.abspath(p))


def _under(path, root):
    return _norm(path).startswith(_norm(root) + os.sep)


def _rel(p):
    try:
        return os.path.relpath(p, REPO).replace(os.sep, "/")
    except ValueError:
        return p


def guard(py, serve, ref_env, summary):
    """ok, or FAIL and exit 1 ("<summary>: 1 FAILED"): the server ref_env starts runs 0.7.0's drivers."""
    legacy = legacy_drivers.OUT
    try:
        # stdin closed: a server without --drivers (a staged pre-0.7.5 copy) serves nothing and exits, and fails here
        r = subprocess.run([py, serve, "--drivers"], capture_output=True, env=ref_env, timeout=180,
                           stdin=subprocess.DEVNULL)
        out, code = r.stdout.decode("utf-8", "replace"), r.returncode
    except subprocess.TimeoutExpired:
        out, code = "", "a timeout"
    mods, dlls = {}, []
    for ln in out.splitlines():
        f = ln.split("\t")
        if f[0] == "module" and len(f) == 3:
            mods[f[1]] = f[2]
        elif f[0] == "dll" and len(f) == 2:
            dlls.append(f[1])
    wrong = ["%s from %s" % (m, _rel(p)) for m, p in sorted(mods.items()) if not _under(p, legacy)]
    missing = [m for m in MODULES if m not in mods]
    bad_dlls = [_rel(d) for d in dlls if os.path.basename(d).lower() == NATIVE or
                (_under(d, REPO) and not _under(d, legacy))]
    ok = code == 0 and not wrong and not missing and not bad_dlls and \
        any(os.path.basename(d).lower() == "bl.dll" for d in dlls)
    if ok:
        print("ok   reference: 0.7.0's Python drivers, %d synthDrivers modules from %s; DLLs %s; nothing from "
              "nvda/dist, no %s" % (len(mods), _rel(legacy), ", ".join(sorted({os.path.basename(d) for d in dlls})),
                                    NATIVE))
        return
    why = (["--drivers exited with %s" % code] if code else []) + \
        (["missing %s" % ", ".join(missing)] if missing else []) + \
        (["%d modules not from the legacy folder: %s" % (len(wrong), "; ".join(wrong[:3]))] if wrong else []) + \
        (["DLLs %s" % "; ".join(bad_dlls)] if bad_dlls else []) + \
        ([] if any(os.path.basename(d).lower() == "bl.dll" for d in dlls) else ["no bl.dll loaded"])
    print("FAIL reference: NOT 0.7.0's Python drivers: %s" % " | ".join(why))
    print("%s: 1 FAILED" % summary)
    sys.exit(1)
