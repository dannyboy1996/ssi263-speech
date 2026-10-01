"""Faults are never silent (Astra, Reply 112 item 2), through bl.dll: its C API (ctypes, bh_*) and hosts/native_blazie.py.

A fault is something the unit did that the host lost for want of memory.  Each is injected as a failed allocation
would cause it (bl_host.c bh_set_int "fail_alloc_size" / "fail_event" / "fail_tx" / "fail_log"), on the unit booted as
the NVDA driver boots it:

  run-ahead script  the capture cannot keep a write mid-utterance (its store growing past 1024 writes), while input
                    waits for the utterance: a second say, or a raw send (a setting).  The error must be seen FIRST:
                    bh_busy -1 with that input held (it was 1: Astra's error_priority_probe.c), busy() raising
                    RunAheadError; the held input never delivered over it (a held say used to start the next capture,
                    and that reset the error); new input refused (-1, raising); then cancel() -- the explicit
                    recovery -- clears it, drops the held input, and the next say speaks to its done.
                    And Astra's probe itself: the capture failing at its start, a second say at once (before any run):
                    refused, bh_busy -1 -- it was held, and bh_busy 1.
  board event      a chip write or serial byte the board could not store (bl_board.c event()): a fault, busy() -1
                    and raising BlazieHostError, until cancel(); the lockstep (run ahead off).
  transmit record   a byte the unit sent that the host's record (bh_tx) could not keep: reading tx raises, the ^F
                    echo is still counted (the say reaches its done).
  write log         a chip write the test log (bh_writes, on_write) could not keep: the next drain raises.

    python run_ahead_fault.py
    RUN_AHEAD_FAULT_BREAK=sticky   run_ahead.h RA_BRK_STICKY: the error behind held input, delivered over (must fail)
    RUN_AHEAD_FAULT_BREAK=drop     run_ahead.h RA_BRK_DROP: the board's, the record's and the log's losses silent
                                   (must fail)
"""
import os
import sys
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [REPO, os.path.join(REPO, "src")]
ENG = os.path.join(REPO, "nvda", "dist", "blazie-build", "synthDrivers", "_ssi263_blazie")
DLL = os.path.join(REPO, "nvda", "dist", "blazie-lib", "x64" if sys.maxsize > 2 ** 32 else "x86", "bl.dll")
BREAK = {"": 0, "sticky": 8, "drop": 9}[os.environ.get("RUN_AHEAD_FAULT_BREAK", "")]   # run_ahead.h's RA_BRK_*
RATE = 22050
# long enough that the capture's write store grows past 1024 writes (ra_write: 16 bytes; 2048 of them = 32768)
LONG = ["The quick brown fox jumps over the lazy dog,", "and then it runs far away into the forest today.",
        "Error: the file could not be found. Try again later.", "Press enter to continue, or escape to cancel."]
GROW = 2048 * 16


def spoken(ws):
    r3, out = 0, []
    for _, r, v in ws:
        if r == 3:
            r3 = v
        if r == 0 and not r3 & 0x80 and v & 0x3F:
            out.append(v & 0x3F)
    return out


def unit(ahead, log):
    from ssi263.native import SSI263C
    from hosts.native_blazie import NativeBlazie
    chip = SSI263C(params={"closure_noise_lead_ms": 10.0}, out_rate=RATE)
    u = NativeBlazie(DLL, os.path.join(ENG, "BL2ENG.BNS"), os.path.join(ENG, "bl2_2003_warm.state"), chip=chip,
                     out_rate=RATE, menu=("punct_none", "numbers_toggle"), key_start=3000000, key_gap=1500000,
                     board_lowpass_hz=5000.0, on_write=lambda t, r, v: log.append((t, r, v)))
    u.send(b"\x18")                     # the driver's boot (synthDrivers/blazie.py _boot)
    u.send(b"\r\x06")
    u.run(0.3)
    u.send(b"\x056V")
    u.run(0.05)
    u.turbo_between_lines = True
    u.run_ahead = ahead
    u.run_ahead_break = BREAK
    return u


def seti(u, name, v):
    u._lib.bh_set_int(u._h, name.encode(), v)


def c_busy(u):
    return u._lib.bh_busy(u._h, 0.1, 3.0)


def until_done(u, limit=30.0):
    """(done, exception): busy() until false, raising, or the limit (chip seconds)"""
    t0 = u.chip.time
    while u.chip.time - t0 < limit:
        u.run(0.03)
        try:
            if not u.busy():
                return True, None
        except Exception as e:      # noqa: BLE001 -- the fault under test
            return False, e
    return False, None


def run_ahead_case(waiting):
    """the capture fails mid-utterance with `waiting` ("say" or "send") held for the unit"""
    from hosts.native_blazie import RunAheadError
    log, problems, notes = [], [], []
    u = unit(1, log)
    seti(u, "fail_alloc_size", GROW)
    u.say(LONG)
    for _ in range(10):
        u.run(0.03)
    if waiting == "say":
        u.say(["Second say."])
    else:
        u.send(b"\x0514E")
    held = u.geti("held")
    if held != 1:
        problems.append("the %s was not held (held %d)" % (waiting, held))
    busy_c, exc, t0 = None, None, u.chip.time
    while u.chip.time - t0 < 30.0:
        u.run(0.03)
        if u.geti("run_ahead_state") == 7 or u.geti("fault"):
            busy_c = c_busy(u)               # the C API, the moment the error exists
            break
        if u.geti("held") == 0:
            break                            # delivered: the error (if any) is gone with it
    try:
        u.busy()
    except RunAheadError as e:
        exc = e
    n_err = len(log)
    for _ in range(20):                      # the host goes on: the held input must stay held
        u.run(0.03)
    state, held_after, captured = u.run_ahead_state, u.geti("held"), u.geti("run_ahead_captured")
    notes.append("C bh_busy %s with the %s held" % (busy_c, waiting))
    if busy_c != -1:
        problems.append("bh_busy = %s with the %s held over the error (C API; -1 wanted)" % (busy_c, waiting))
    if exc is None:
        problems.append("busy() did not raise RunAheadError")
    if held_after != 1 or state != "error":
        problems.append("the held %s was DELIVERED over the error (held %d, run ahead now %s, %d captured)"
                        % (waiting, held_after, state, captured))
    refused = []
    for what, call in (("say", lambda: u.say(["Refused."])), ("send", lambda: u.send(b"\x0513E"))):
        try:
            call()
        except RunAheadError:
            refused.append(what)
    if refused != ["say", "send"]:
        problems.append("new input taken over the error (refused: %s)" % (", ".join(refused) or "none"))
    u.cancel()                               # the explicit recovery
    after_cancel = (u.fault, u.geti("held"), u.run_ahead_state)
    if after_cancel[0] or after_cancel[1]:
        problems.append("cancel() left fault %d, held %d" % after_cancel[:2])
    u.run(0.3)
    n0 = len(log)
    u.say(["After the cancel."])
    done, e = until_done(u)
    if not done or not spoken(log[n0:]):
        problems.append("the next say after cancel(): done %s, %s, %d spoken loads" % (done, e, len(spoken(log[n0:]))))
    notes.append("%d writes to the error" % n_err)
    u.close()
    return "run-ahead script, a %s waiting" % waiting, problems, notes


def start_case():
    """Astra's error_priority_probe.c on the real host: the capture fails at its start (the 256-entry acknowledgement
    array), nothing run yet, and a second say comes.  Refused (C bh_say -1, raising), bh_busy -1 with nothing held; it
    was held, and bh_busy 1"""
    from hosts.native_blazie import RunAheadError
    log, problems = [], []
    u = unit(1, log)
    seti(u, "fail_alloc_size", 256 * 8)
    u.say(["First say."])
    data = b"Second say.\r\x06\r\x06"
    code = u._lib.bh_say(u._h, data, len(data))
    busy_c, held = c_busy(u), u.geti("held")
    notes = ["C bh_say %d, bh_busy %d, %d held" % (code, busy_c, held)]
    if code != -1 or busy_c != -1 or held:
        problems.append("RA_ERROR with the second say: C bh_say %d, bh_busy %d with %d input held (-1, -1, 0 wanted)"
                        % (code, busy_c, held))
    try:
        u.busy()
        problems.append("busy() did not raise RunAheadError")
    except RunAheadError:
        pass
    u.cancel()
    n0 = len(log)
    u.say(["After the cancel."])
    done, e = until_done(u)
    if not done or not spoken(log[n0:]):
        problems.append("the next say after cancel(): done %s, %s" % (done, e))
    u.close()
    return "run-ahead script failed at its start, a second say", problems, notes


def board_case():
    from hosts.native_blazie import BlazieHostError
    log, problems = [], []
    u = unit(0, log)
    u.say(["Board event lost."])
    for _ in range(5):
        u.run(0.03)
    seti(u, "fail_event", 3)                 # the third event from now: a chip write or a serial byte
    done, e = until_done(u, 10.0)
    if not isinstance(e, BlazieHostError) or u.fault != 2:
        problems.append("a lost board event passed SILENTLY: busy %s, fault %d" % (
            "raised %r" % e if e else "done" if done else "never done", u.fault))
    else:
        try:
            u.say(["Refused."])
            problems.append("a say was taken over the fault")
        except BlazieHostError:
            pass
        u.cancel()
        n0 = len(log)
        u.say(["After the cancel."])
        done, e = until_done(u)
        if not done or not spoken(log[n0:]):
            problems.append("the next say after cancel(): done %s, %s" % (done, e))
    u.close()
    return "board event lost (lockstep)", problems, []


def tx_case():
    from hosts.native_blazie import BlazieHostError
    log, problems = [], []
    u = unit(0, log)
    n_tx = len(u.tx)
    seti(u, "fail_tx", 1)                    # the next byte the unit transmits: a ^F echo
    u.say(["Transmit record."])
    done, e = until_done(u, 10.0)
    if not done:
        problems.append("the say never done (%s): the echo must still be counted" % e)
    try:
        tx = u.tx
        problems.append("a transmitted byte lost SILENTLY: %d bytes recorded for this say" % (len(tx) - n_tx))
    except BlazieHostError:
        pass
    u.close()
    return "transmit record", problems, []


def log_case():
    from hosts.native_blazie import BlazieHostError
    log, problems = [], []
    u = unit(0, log)
    seti(u, "fail_log", 10)
    raised = None
    try:
        u.say(["Write log."])
        until_done(u, 10.0)
        u._drain()
    except BlazieHostError as e:
        raised = e
    if raised is None:
        problems.append("a logged write lost SILENTLY (%d writes logged)" % len(log))
    u.close()
    return "write log", problems, []


CASES = [lambda: run_ahead_case("say"), lambda: run_ahead_case("send"), start_case, board_case, tx_case, log_case]


def one(k):
    return CASES[k]()


if __name__ == "__main__":
    with Pool(len(CASES)) as p:
        results = p.map(one, range(len(CASES)))
    failures = 0
    for name, problems, notes in results:
        failures += bool(problems)
        print("%-4s %s: %s" % ("FAIL" if problems else "ok", name,
                               "; ".join(problems) if problems else ", ".join(notes) or "raised, recovered by cancel()"))
    print("run ahead faults: %s" % ("all %d ok" % len(results) if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)
