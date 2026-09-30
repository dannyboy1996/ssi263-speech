"""Loads the bundled native library and declares its C API for ctypes.

Windows ships two files (ssi263.dll: the chip; bl.dll: the Blazie board, host and voice, which imports ssi263.dll),
Linux one (libssi263speech.so: all of it).  The chip's functions come from the first, the voice's from the second.
"""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIBS = os.path.join(HERE, "_libs")

_P, _I, _D, _S = ctypes.c_void_p, ctypes.c_int, ctypes.c_double, ctypes.c_char_p
_chip = _voice = None


def _open(name):
    path = os.path.join(LIBS, name)
    if not os.path.isfile(path):
        raise OSError("ssi263speech: %s is not in this installation (%s)" % (name, LIBS))
    return ctypes.CDLL(path)


def _declare_chip(lib):
    lib.ssi263_params_size.restype = _I
    lib.ssi263_default_params.argtypes = [_P]
    lib.ssi263_default_rom.restype = _P
    lib.ssi263_new.restype = _P
    lib.ssi263_new.argtypes = [_P, _P, _D]
    lib.ssi263_free.argtypes = [_P]
    lib.ssi263_write.argtypes = [_P, _I, _I]
    lib.ssi263_reg.argtypes = [_P, _I]
    lib.ssi263_reg.restype = _I
    lib.ssi263_request.argtypes = [_P]
    lib.ssi263_request.restype = _I
    lib.ssi263_time.argtypes = [_P]
    lib.ssi263_time.restype = _D
    for fn in (lib.ssi263_run, lib.ssi263_run_until_request):
        fn.argtypes = [_P, ctypes.c_long, ctypes.POINTER(_D)]
        fn.restype = ctypes.c_long
    lib.ssi_pcm16.argtypes = [ctypes.POINTER(_D), _I, _D, ctypes.POINTER(ctypes.c_short)]


def _declare_voice(lib):
    lib.blv_create.restype = _P
    lib.blv_create.argtypes = [_S, _S, _I, _D, _I, _I, _S, _I]
    lib.blv_destroy.argtypes = [_P]
    lib.blv_set.argtypes = [_P, _I, _I, _I, _I, _I]
    lib.blv_speak.argtypes = [_P, _S]
    lib.blv_speak.restype = _I
    lib.blv_render.argtypes = [_P, ctypes.POINTER(ctypes.POINTER(ctypes.c_short)), ctypes.POINTER(_I)]
    lib.blv_render.restype = _I
    lib.blv_cancel.argtypes = [_P]


def chip_lib():
    global _chip
    if _chip is None:
        _chip = _open("ssi263.dll" if sys.platform == "win32" else "libssi263speech.so")
        _declare_chip(_chip)
    return _chip


def voice_lib():
    global _voice
    if _voice is None:
        if sys.platform == "win32":
            chip_lib()                          # bl.dll imports ssi263.dll: this copy, loaded first
            _voice = _open("bl.dll")
        else:
            _voice = chip_lib()
        _declare_voice(_voice)
    return _voice
