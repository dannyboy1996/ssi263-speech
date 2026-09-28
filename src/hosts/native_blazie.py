"""The Braille Lite host in-process: hosts/blazie.py's Blazie, with the lockstep, the emulator and the board in C
(src/csrc/blazie: bl.dll), and the same constructor, methods and attributes.  The chip stays the SSI263C the
caller made (bl.dll imports ssi263.dll, so both use the one copy loaded).  nvda/tools/golden/blazie_*.txt gate it:
bns_equiv.py --native.

No pipe and no child process: every emulated step is a function call instead of a round trip.
"""
import ctypes
import os
import sys
from array import array

try:
    from .blazie import boot_keys
except ImportError:                       # the research tree: src/ on sys.path
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from hosts.blazie import boot_keys  # noqa: E402

_D, _I, _P, _S = ctypes.c_double, ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p
WHINES = {None: 0, "hiss": 1, "whine": 2}
_libs = {}


class _Write(ctypes.Structure):
    _fields_ = [("t", _D), ("reg", _I), ("val", _I)]


def _load(path):
    path = os.path.abspath(path)
    if path in _libs:
        return _libs[path]
    lib = ctypes.CDLL(path)
    lib.bh_create.restype = _P
    lib.bh_create.argtypes = [_S, _S, _P, _D, _D, ctypes.POINTER(ctypes.c_ulonglong), ctypes.POINTER(ctypes.c_ubyte),
                              _I, ctypes.c_ulonglong, _I, ctypes.c_char_p, _I]
    lib.bh_writes.argtypes = [_P, ctypes.POINTER(ctypes.POINTER(_Write))]
    lib.bh_writes.restype = _I
    lib.bh_clear_writes.argtypes = [_P]
    lib.bh_destroy.argtypes = [_P]
    for fn in (lib.bh_send, lib.bh_say):
        fn.argtypes = [_P, ctypes.c_char_p, _I]
    lib.bh_owed.argtypes = [_P]
    lib.bh_owed.restype = _I
    lib.bh_busy.argtypes = [_P, _D, _D]
    lib.bh_busy.restype = _I
    lib.bh_cancel.argtypes = [_P, _D, _D, _D]
    lib.bh_cancel.restype = _D
    lib.bh_skip.argtypes = [_P, _D]
    lib.bh_skip.restype = _D
    lib.bh_run.argtypes = [_P, _D, _D, ctypes.POINTER(ctypes.POINTER(_D))]
    lib.bh_run.restype = _I
    lib.bh_set_whine.argtypes = [_P, _I]
    lib.bh_get_whine.argtypes = [_P]
    lib.bh_get_whine.restype = _I
    lib.bh_get_int.argtypes = [_P, _S]
    lib.bh_get_int.restype = _I
    lib.bh_set_int.argtypes = [_P, _S, _I]
    lib.bh_get_double.argtypes = [_P, _S]
    lib.bh_get_double.restype = _D
    lib.bh_set_double.argtypes = [_P, _S, _D]
    lib.bh_tx.argtypes = [_P, ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
    lib.bh_tx.restype = _I
    _libs[path] = lib
    return lib


def _int_attr(name):
    def get(self):
        return self._lib.bh_get_int(self._h, name.encode())

    def put(self, v):
        self._lib.bh_set_int(self._h, name.encode(), int(v))
    return property(get, put)


def _float_attr(name, none_is_zero=False):
    def get(self):
        v = self._lib.bh_get_double(self._h, name.encode())
        return None if (none_is_zero and v == 0.0) else v

    def put(self, v):
        self._lib.bh_set_double(self._h, name.encode(), 0.0 if v is None else float(v))
    return property(get, put)


class NativeBlazie:
    def __init__(self, dll, firmware, state, chip=None, out_rate=44100, menu=(), key_start=None, key_gap=None,
                 board_lowpass_hz=None, status=(), on_write=None):
        """`on_write(t, reg, val)`: every SSI-263 write, with the chip time it was applied at (tests: the C host
        writes the chip directly, so a spy on chip.write sees nothing)."""
        self.on_write = on_write
        if chip is None:
            from ssi263.native import SSI263C
            chip = SSI263C(out_rate=out_rate)
        self.chip = chip
        self.encoding = "latin-1"
        self._lib = _load(dll)
        keys, boot_instr = boot_keys(menu, key_start, key_gap, status)
        at = (ctypes.c_ulonglong * max(1, len(keys)))(*[int(k.split("=")[0]) for k in keys])
        val = (ctypes.c_ubyte * max(1, len(keys)))(*[int(k.split("=")[1], 16) for k in keys])
        err = ctypes.create_string_buffer(256)
        self._h = self._lib.bh_create(firmware.encode("mbcs" if os.name == "nt" else "utf-8"),
                                      state.encode("mbcs" if os.name == "nt" else "utf-8"), chip._c,
                                      float(chip.out_rate), float(board_lowpass_hz or 0.0), at, val, len(keys),
                                      boot_instr, 1 if on_write else 0, err, 256)
        if not self._h:
            raise RuntimeError("bl.dll: %s" % err.value.decode("latin-1", "replace"))
        self._drain()

    def _drain(self):
        if self.on_write is None or not self._h:
            return
        p = ctypes.POINTER(_Write)()
        n = self._lib.bh_writes(self._h, ctypes.byref(p))
        for i in range(n):
            self.on_write(p[i].t, p[i].reg, p[i].val)
        self._lib.bh_clear_writes(self._h)

    def close(self):
        h = getattr(self, "_h", None)
        if h:
            self._tx_closed = self.tx          # blazie.py's tx outlives close()
            self._h = None
            self._lib.bh_destroy(h)

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    # ---- blazie.py's attributes ----------------------------------------------------------
    sent_f = _int_attr("sent_f")
    echo_f = _int_attr("echo_f")
    _stale_f = _int_attr("stale_f")
    turbo = _float_attr("turbo")
    prep_step = _float_attr("prep_step", none_is_zero=True)
    last_speech = _float_attr("last_speech")
    say_time = _float_attr("say_time")
    cancel_cut = _float_attr("cancel_cut")
    cancel_quiet = _float_attr("cancel_quiet")

    @property
    def preparing(self):
        return bool(self._lib.bh_get_int(self._h, b"preparing"))

    @preparing.setter
    def preparing(self, v):
        self._lib.bh_set_int(self._h, b"preparing", 1 if v else 0)

    @property
    def turbo_between_lines(self):
        return bool(self._lib.bh_get_int(self._h, b"turbo_between_lines"))

    @turbo_between_lines.setter
    def turbo_between_lines(self, v):
        self._lib.bh_set_int(self._h, b"turbo_between_lines", 1 if v else 0)

    @property
    def ar(self):
        v = self._lib.bh_get_int(self._h, b"ar")
        return None if v < 0 else bool(v)

    @property
    def whine(self):
        return {0: None, 1: "hiss", 2: "whine"}[self._lib.bh_get_whine(self._h)]

    @whine.setter
    def whine(self, mode):
        self._lib.bh_set_whine(self._h, WHINES[mode])

    @property
    def tx(self):
        if not getattr(self, "_h", None):
            return list(getattr(self, "_tx_closed", []))
        p = ctypes.POINTER(ctypes.c_ubyte)()
        n = self._lib.bh_tx(self._h, ctypes.byref(p))
        return list(p[:n]) if n else []

    # ---- blazie.py's methods ---------------------------------------------------------------
    def send(self, data):
        if isinstance(data, str):
            data = data.encode("latin-1", "replace")
        if data:
            self._lib.bh_send(self._h, data, len(data))
            self._drain()

    def say(self, text):
        lines = [text] if isinstance(text, str) else list(text)
        data = b"".join(ln.encode(self.encoding, "replace") + b"\r\x06" for ln in lines) + b"\r\x06"
        self._lib.bh_say(self._h, data, len(data))
        self._drain()

    def owed(self):
        return self._lib.bh_owed(self._h)

    def busy(self, quiet=0.1, patience=3.0):
        return bool(self._lib.bh_busy(self._h, quiet, patience))

    def cancel(self, limit=3.0, quiet=None, cut=None):
        t = self._lib.bh_cancel(self._h, limit, -1.0 if quiet is None else quiet, -1.0 if cut is None else cut)
        self._drain()
        return t

    def skip(self, seconds):
        t = self._lib.bh_skip(self._h, seconds)
        self._drain()
        return t

    def run(self, seconds, step=0.0005):
        p = ctypes.POINTER(_D)()
        n = self._lib.bh_run(self._h, seconds, step, ctypes.byref(p))
        out = array("d", bytes(8 * n))
        if n:
            ctypes.memmove(out.buffer_info()[0], p, 8 * n)
        self._drain()
        return out
