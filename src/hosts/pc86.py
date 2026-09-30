"""MAME's 8086 for the Accent-mini host, in the shape of the Unicorn calls accent.py makes (ucmini.Uc).

pc86.dll (src/csrc/pc86: the core of src/csrc/cpu/i86_mame.cpp with 1 MB of flat memory) runs the CPU; the host keeps
doing what DOS, the BIOS, the EMS manager and the PIC did, in Python, exactly as with Unicorn: its IN/OUT hooks become
the bus's I/O callbacks and its interrupt hook the core's INT seam (cpu_bus.intercept: every INT n, INT 3, INTO and
divide error is offered to the host, which services it -- as a Unicorn interrupt hook swallows the interrupt).

Opt-in: accent.py uses it when SSI263_ACCENT_CORE=mame.  The library is looked for in SSI263_PC86_DLL (a file), bin/<arch>/
beside this file, then the repository's nvda/dist/blazie-lib/<arch>/ (where src/csrc/blazie/build_board.py puts it).

Differences from Unicorn that a caller can see (src/csrc/cpu/README.md, "The MAME 8086 against Unicorn"):
  - a step is MAME's: a REP string instruction with CX = n > 0 is n steps (Unicorn counts n + 1, a last pass that
    only finds CX = 0); a LOCK prefix is its own step;
  - FLAGS bits 12-15 read 1 (the 8086), 0 on Unicorn (a 386 in real mode);
  - the undefined flags follow MAME's 8086, not QEMU's.
Any opcode whose meaning differs on the 80186 and later (i86_aliased) stops the run with an error: the driver is
8086 code, and one would mean it is not.
"""
import ctypes
import os
import struct
from ctypes import POINTER, byref, c_int, c_uint8, c_uint32, c_uint64, c_void_p

try:
    from .ucmini import (UC_HOOK_INSN, UC_HOOK_INTR, UC_X86_INS_IN, UC_X86_INS_OUT, UC_X86_REG_AX, UC_X86_REG_BX,
                         UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_SP,
                         UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_IP,
                         UC_X86_REG_EFLAGS)
except ImportError:
    from ucmini import (UC_HOOK_INSN, UC_HOOK_INTR, UC_X86_INS_IN, UC_X86_INS_OUT, UC_X86_REG_AX,  # noqa: F401
                        UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP,
                        UC_X86_REG_SP, UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_IP,
                        UC_X86_REG_EFLAGS)

MEM_SIZE = 0x100000
NO_UNTIL = 0xFFFFFFFF


class I86Regs(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint16) for n in ("ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
                                                "es", "cs", "ss", "ds", "ip", "flags")] + [("halted", c_uint8)]


FIELD = {UC_X86_REG_AX: "ax", UC_X86_REG_BX: "bx", UC_X86_REG_CX: "cx", UC_X86_REG_DX: "dx", UC_X86_REG_SI: "si",
         UC_X86_REG_DI: "di", UC_X86_REG_BP: "bp", UC_X86_REG_SP: "sp", UC_X86_REG_CS: "cs", UC_X86_REG_DS: "ds",
         UC_X86_REG_ES: "es", UC_X86_REG_SS: "ss", UC_X86_REG_IP: "ip", UC_X86_REG_EFLAGS: "flags"}

_IN = ctypes.CFUNCTYPE(c_int, c_void_p, c_int)
_OUT = ctypes.CFUNCTYPE(None, c_void_p, c_int, c_int)
_INT = ctypes.CFUNCTYPE(c_int, c_void_p, c_int, c_int)
_lib = None


class Pc86Error(Exception):
    pass


def _candidates():
    arch = "x64" if struct.calcsize("P") == 8 else "x86"
    name = "pc86.dll" if os.name == "nt" else "libpc86.so"
    env = os.environ.get("SSI263_PC86_DLL")
    if env:
        yield env
    here = os.path.dirname(os.path.abspath(__file__))
    yield os.path.join(here, "bin", arch, name)
    repo = os.path.dirname(os.path.dirname(here))
    yield os.path.join(repo, "nvda", "dist", "blazie-lib", arch, name)


def _load():
    global _lib
    if _lib is None:
        tried = []
        for path in _candidates():
            if os.path.isfile(path):
                lib = ctypes.CDLL(path)
                break
            tried.append(path)
        else:
            raise Pc86Error("pc86 library not found; tried %s" % tried)
        lib.pc86_create.restype = c_void_p
        lib.pc86_destroy.argtypes = [c_void_p]
        lib.pc86_mem.argtypes = [c_void_p]
        lib.pc86_mem.restype = c_void_p
        lib.pc86_set_hooks.argtypes = [c_void_p, _IN, _OUT, _INT, c_void_p]
        lib.pc86_run.argtypes = [c_void_p, c_uint64, c_uint32]
        lib.pc86_run.restype = c_uint64
        lib.pc86_stop.argtypes = [c_void_p]
        lib.pc86_regs_get.argtypes = [c_void_p, POINTER(I86Regs)]
        lib.pc86_regs_set.argtypes = [c_void_p, POINTER(I86Regs)]
        lib.pc86_cycles.argtypes = [c_void_p]
        lib.pc86_cycles.restype = c_uint64
        lib.pc86_steps.argtypes = [c_void_p]
        lib.pc86_steps.restype = c_uint64
        lib.pc86_aliased.argtypes = [c_void_p, POINTER(c_uint32), POINTER(c_uint8)]
        lib.pc86_aliased.restype = c_uint64
        _lib = lib
    return _lib


class Pc86:
    """ucmini.Uc's calls, on MAME's 8086: mem_map/mem_read/mem_write, reg_read/reg_write, emu_start/emu_stop,
    hook_add for the interrupt hook and the IN/OUT instruction hooks."""

    def __init__(self, arch=None, mode=None):
        lib = _load()
        self._p = c_void_p(lib.pc86_create())
        if not self._p:
            raise Pc86Error("pc86_create failed")
        self._mem = lib.pc86_mem(self._p)
        self._pending = None
        self._in_cb = self._out_cb = self._int_cb = None
        self._thunks = (_IN(self._on_in), _OUT(self._on_out), _INT(self._on_int))
        lib.pc86_set_hooks(self._p, self._thunks[0], self._thunks[1], self._thunks[2], None)

    def close(self):
        if self._p:
            _lib.pc86_destroy(self._p)
            self._p = c_void_p()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    # ---- memory: 1 MB, always there ----
    def mem_map(self, address, size, perms=None):
        if address != 0 or size > MEM_SIZE:
            raise Pc86Error("pc86 has 1 MB at 0 only")

    def mem_write(self, address, data):
        data = bytes(data)
        if address < 0 or address + len(data) > MEM_SIZE:
            raise Pc86Error("write outside 1 MB")
        ctypes.memmove(self._mem + address, data, len(data))

    def mem_read(self, address, size):
        if address < 0 or address + size > MEM_SIZE:
            raise Pc86Error("read outside 1 MB")
        return bytearray(ctypes.string_at(self._mem + address, size))

    # ---- registers ----
    def reg_read(self, reg):
        r = I86Regs()                      # per call: another thread may read registers (the driver's watch)
        _lib.pc86_regs_get(self._p, byref(r))
        return getattr(r, FIELD[reg])

    def reg_write(self, reg, value):
        r = I86Regs()                      # per call: another thread may read registers (the driver's watch)
        _lib.pc86_regs_get(self._p, byref(r))
        setattr(r, FIELD[reg], value & 0xFFFF)
        _lib.pc86_regs_set(self._p, byref(r))

    # ---- running ----
    def emu_start(self, begin, until, timeout=0, count=0):
        """As uc_emu_start: begin sets IP (CS stays), until stops before the instruction at that linear address,
        count caps the steps (0: none)."""
        r = I86Regs()                      # per call: another thread may read registers (the driver's watch)
        _lib.pc86_regs_get(self._p, byref(r))
        ip = (begin - r.cs * 16) & 0xFFFF
        if ip != r.ip:
            r.ip = ip
            _lib.pc86_regs_set(self._p, byref(r))
        self._pending = None
        _lib.pc86_run(self._p, count if count else (1 << 62), until & 0xFFFFF if until is not None else NO_UNTIL)
        if self._pending is not None:
            e, self._pending = self._pending, None
            raise e
        addr, op = c_uint32(), c_uint8()
        if _lib.pc86_aliased(self._p, byref(addr), byref(op)):
            raise Pc86Error("opcode %02Xh at %05X: its meaning differs on the 80186 and later; the 8086 core "
                            "is not the CPU this program needs" % (op.value, addr.value))

    def emu_stop(self):
        _lib.pc86_stop(self._p)

    @property
    def cycles(self):
        return _lib.pc86_cycles(self._p)

    @property
    def steps(self):
        return _lib.pc86_steps(self._p)

    @property
    def aliased(self):
        return _lib.pc86_aliased(self._p, None, None)

    def hook_add(self, htype, callback, user_data=None, begin=1, end=0, arg1=0):
        if htype == UC_HOOK_INTR:
            self._int_cb = (callback, user_data)
        elif htype == UC_HOOK_INSN and arg1 == UC_X86_INS_IN:
            self._in_cb = (callback, user_data)
        elif htype == UC_HOOK_INSN and arg1 == UC_X86_INS_OUT:
            self._out_cb = (callback, user_data)
        else:
            raise Pc86Error("pc86 supports the interrupt hook and the IN/OUT hooks only")
        return 0

    # ---- the callbacks: an exception stops the run and is raised from emu_start, as ucmini does ----
    def _fail(self, e):
        if self._pending is None:
            self._pending = e
        _lib.pc86_stop(self._p)

    def _on_in(self, _user, port):
        try:
            if self._in_cb is None:
                return 0xFF
            cb, ud = self._in_cb
            v = cb(self, port, 1, ud)
            return 0xFF if v is None else v & 0xFF
        except Exception as e:
            self._fail(e)
            return 0xFF

    def _on_out(self, _user, port, value):
        try:
            if self._out_cb is not None:
                cb, ud = self._out_cb
                cb(self, port, 1, value, ud)
        except Exception as e:
            self._fail(e)

    def _on_int(self, _user, vector, kind):
        try:
            if self._int_cb is None:
                return 0
            cb, ud = self._int_cb
            cb(self, vector, ud)
            return 1                       # a Unicorn interrupt hook swallows the interrupt
        except Exception as e:
            self._fail(e)
            return 1
