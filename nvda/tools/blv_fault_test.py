"""bl_voice (Android's, SAPI's and speech-dispatcher's Braille Lite front end) never hangs or goes silent on a host
fault (after Astra, Reply 112 item 2: bl_voice ignored bh_say's -1 and took bh_busy's -1 for busy).

A board event is lost on purpose (bl_host.c bh_set_int "fail_event": a fault, run ahead off): the utterance must end
within a few blocks with blv_fault set, and the next blv_speak must recover (a cancel) and speak to its done.

    python blv_fault_test.py           BLV_FAULT_TEST_BREAK=1: blv_break_fault (the draft's handling) -- must fail
"""
import ctypes
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ARCH = "x64" if sys.maxsize > 2 ** 32 else "x86"
DLL = os.path.join(REPO, "nvda", "dist", "blazie-lib", ARCH, "bl.dll")
CHIP = os.path.join(REPO, "src", "ssi263", "_bin", ARCH, "ssi263.dll")
FW = os.path.join(REPO, "firmware", "blazie")
MAX_BLOCKS = 400            # 12 s of 30 ms blocks: far beyond any of these utterances


def main():
    ctypes.CDLL(CHIP)
    lib = ctypes.CDLL(DLL)
    lib.blv_create.restype = ctypes.c_void_p
    lib.blv_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_double, ctypes.c_int,
                               ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    lib.blv_speak.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.blv_render.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.POINTER(ctypes.c_short)),
                               ctypes.POINTER(ctypes.c_int)]
    lib.blv_fault.argtypes = [ctypes.c_void_p]
    lib.blv_host.restype = ctypes.c_void_p
    lib.blv_host.argtypes = [ctypes.c_void_p]
    lib.bh_set_int.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    lib.blv_destroy.argtypes = [ctypes.c_void_p]
    if os.environ.get("BLV_FAULT_TEST_BREAK") == "1":
        ctypes.c_int.in_dll(lib, "blv_break_fault").value = 1
    with tempfile.TemporaryDirectory() as d:
        for n in ("BL2ENG.BNS", "bl2_2003_warm.state"):
            shutil.copy(os.path.join(FW, n), d)
        err = ctypes.create_string_buffer(256)
        v = lib.blv_create(os.path.join(d, "BL2ENG.BNS").encode(), os.path.join(d, "bl2_2003_warm.state").encode(),
                           0, 22050.0, 1, 0, err, 256)
        if not v:
            sys.exit("boot failed: %s" % err.value)

        def say(text):
            """(lines, blocks to done or None, audible samples, fault at the end)"""
            lines = lib.blv_speak(v, text.encode())
            pcm, done, blocks, loud = ctypes.POINTER(ctypes.c_short)(), ctypes.c_int(0), 0, 0
            while lines > 0 and not done.value and blocks < MAX_BLOCKS:
                n = lib.blv_render(v, ctypes.byref(pcm), ctypes.byref(done))
                loud += sum(1 for i in range(n) if abs(pcm[i]) > 300)
                blocks += 1
            return lines, (blocks if lines <= 0 or done.value else None), loud, lib.blv_fault(v)

        say("Warm up.")
        lib.bh_set_int(lib.blv_host(v), b"fail_event", 3)      # the third board event from now is lost
        lines, blocks, loud, fault = say("Hello there, this is a fault test.")
        bad = 0
        ok = blocks is not None and fault == 1
        bad += not ok
        print("%-4s the faulted utterance ends and says so: %d lines, %s, fault %d" % (
            "ok" if ok else "FAIL", lines, "done after %d blocks" % blocks if blocks is not None else
            "NOT DONE after %d blocks" % MAX_BLOCKS, fault))
        lines, blocks, loud, fault = say("OK button.")
        ok = lines > 0 and blocks is not None and loud > 1000 and fault == 0
        bad += not ok
        print("%-4s the next utterance recovers and speaks: %d lines, %s, %d audible samples, fault %d" % (
            "ok" if ok else "FAIL", lines, "done after %d blocks" % blocks if blocks is not None else "NOT DONE",
            loud, fault))
        lib.blv_destroy(v)
    print("blv fault: %s" % ("PASS" if not bad else "%d FAILED" % bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
