"""Currencies other than the dollar reach the units as words (numwords.currencies; a listener, 2026-09-28: "2 pounds
63 pence ... is read out as 2.63").

    python currency_test.py rules              # the rule's own cases
    python currency_test.py blazie|speakout|accent
        the real driver under the stand-in NVDA speaks "It costs £2.63." and the unit must be SENT "... pounds ... pence" (digits or words)
    CURRENCY_OFF=1                             # control: the rule stubbed out in the driver -- must FAIL
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WHICH = sys.argv[1] if len(sys.argv) > 1 else "rules"

CASES = [("£2.63", "2 pounds 63 pence"), ("It costs £1.01 today.", "It costs 1 pound 1 penny today."),
         ("£0.63", "63 pence"), ("£5, then", "5 pounds, then"),
         ("£1,234.56.", "1,234 pounds 56 pence."), ("€2.50 and 5 €", "2 euros 50 cents and 5 euros"),
         ("10€", "10 euros"), ("¥100", "100 yen"), ("50¢ and 1¢", "50 cents and 1 cent"),
         ("£1.00", "1 pound"), ("£.5", "50 pence"), ("£2.635", "2.635 pounds"),
         ("price: £ 12", "price: 12 pounds"), ("just £", "just £"), ("US$5 stays", "US$5 stays"),
         ("a£3", "a 3 pounds")]

if WHICH == "rules":
    sys.path.insert(0, os.path.join(os.path.dirname(HERE), "shared"))
    import ssi263_numwords as numwords
    bad = [(t, want, numwords.currencies(t)) for t, want in CASES if numwords.currencies(t) != want]
    for t, want, got in bad:
        print("WRONG %r -> %r, want %r" % (t, got, want))
    same = numwords.currencies("£2.63", lang="es") == "£2.63"
    print("%d of %d currency cases right; Spanish left alone: %s" % (len(CASES) - len(bad), len(CASES), same))
    sys.exit(1 if bad or not same else 0)

sys.argv = [sys.argv[0], WHICH]
src = open(os.path.join(HERE, "fake_nvda_driver_test.py"), encoding="utf-8").read()
exec(src.split("time.sleep(2.0)")[0])
if os.environ.get("CURRENCY_OFF") == "1":
    drv_mod.numwords.currencies = lambda text, *a, **k: text
while (getattr(d, "_unit", None) or getattr(d, "_box", None)) is None:
    time.sleep(0.05)
unit = getattr(d, "_unit", None) or d._box
sent = []
orig_say = unit.say


def say(text, *a, **k):
    sent.append(text if isinstance(text, str) else " ".join(text))
    return orig_say(text, *a, **k)


unit.say = say
mark = len(notified)
d.speak(["It costs £2.63."])
wait_idle(30)
d.terminate()
got = " ".join(sent)
ok = " pounds " in got and got.rstrip(" .\r").endswith("pence")   # "2 pounds 63 pence", or in words
print("%s: the unit was sent %r: %s" % (WHICH, got.strip(), "ok" if ok else "FAILED"))
sys.exit(0 if ok else 1)
