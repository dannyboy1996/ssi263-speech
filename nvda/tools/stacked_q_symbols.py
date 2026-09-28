"""Stacked question marks through NVDA's own symbol processing (characterProcessing from an installed NVDA's
library.zip), with and without the Braille Lite add-on's symbol dictionary (nvda/blazie/locale/*/symbols-stackedq.dic).

NVDA only sends a "?" that ends a sentence, so "how are you??" reached the driver as "how are you" at every
symbol level and the unit never raised its pitch for the marks (Tomi, 0.6.0 draft).  The driver never sees the
text before this step, so fake_nvda_driver_test.py and driver_sim.py can't catch it; this can.

    python -S stacked_q_symbols.py "C:\\Program Files\\NVDA"
"""
import builtins
import os
import sys
import types

NVDA_DIR = sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files\NVDA"
ADDON_LOCALE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "blazie", "locale")
sys.path[:] = [os.path.join(NVDA_DIR, "library.zip"), NVDA_DIR]


def stub(name, **kw):
    m = types.ModuleType(name)
    m.__dict__.update(kw)
    sys.modules[name] = m


class _Log:
    def __getattr__(self, n):
        return lambda *a, **k: None


builtins._ = lambda s: s
builtins.pgettext = lambda c, s: s
stub("logHandler", log=_Log())
stub("globalVars", appDir=NVDA_DIR, appArgs=types.SimpleNamespace(configPath="", secure=False))
stub("config", conf={"speech": {"symbolDictionaries": ["cldr"]}})
import characterProcessing as cp  # noqa: E402

LINES = {"how are you?": "?", "how are you??": "??", "how are you???": "???"}
bad = 0
for loc in ("en", "es"):
    base = cp.SpeechSymbols()
    base.load(os.path.join(NVDA_DIR, "locale", loc, "symbols.dic"))
    addon = cp.SpeechSymbols()
    addon.load(os.path.join(ADDON_LOCALE, loc, "symbols-stackedq.dic"), allowComplexSymbols=False)
    for label, sources in (("NVDA alone", (base, cp.SpeechSymbols())), ("with the add-on", (base, addon, cp.SpeechSymbols()))):

        class Fetch:
            def fetchLocaleData(self, locale, fallback=True, s=sources):
                return s
        cp.SpeechSymbolProcessor.localeSymbols = Fetch()
        proc = cp.SpeechSymbolProcessor(loc)
        for level in (cp.SymbolLevel.NONE, cp.SymbolLevel.SOME, cp.SymbolLevel.MOST, cp.SymbolLevel.ALL):
            kept = [proc.processText(t, level).count("?") == len(marks) for t, marks in LINES.items()]
            if label == "with the add-on" and not all(kept):
                bad += 1
        print("%s %-16s stacked '?' reach the driver at every level: %s" % (
            loc, label, all(proc.processText(t, lv).count("?") == len(m) for t, m in LINES.items() for lv in (
                cp.SymbolLevel.NONE, cp.SymbolLevel.SOME, cp.SymbolLevel.MOST, cp.SymbolLevel.ALL))))
print("ok" if not bad else "FAILED")
sys.exit(1 if bad else 0)
