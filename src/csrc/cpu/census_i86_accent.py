"""Which x86 the Accent-mini's driver needs: an instruction census of SPKEMS.DVC under Unicorn (a full x86), every
distinct basic block it executes disassembled (capstone, not a repo dependency: pip install capstone).

    python src/csrc/cpu/census_i86_accent.py [--dvc PATH]

The scenario: INIT, boot, texts with numbers and punctuation, rate / pitch / volume / voice commands, a cancel in
mid-sentence.  Reported: the mnemonics, the prefixes, and every instruction whose opcode means something else on the
80186 and later (0Fh, 60h-6Fh, C0h/C1h, C8h/C9h, F1h) or needs a 386 (operand/address size, FS/GS); then the blocks
that use PUSHF/POPF or shift by CL, where an 8086 and a later CPU could part.  The standing check is the core's own
i86_aliased(), which pc86.py enforces on every run; this is the evidence it was chosen on (README.md, 2026-09-30).
Takes a few minutes.
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, "src"))
import capstone  # noqa: E402
from hosts.accent import Accent  # noqa: E402
from hosts.ucmini import UC_HOOK_BLOCK  # noqa: E402
from ssi263.native import SSI263C  # noqa: E402

DVC = sys.argv[sys.argv.index("--dvc") + 1] if "--dvc" in sys.argv else \
    os.path.join(REPO, "firmware", "aicom-accent-mini", "SPKEMS.DVC")
PREFIXES = (0x26, 0x2E, 0x36, 0x3E, 0xF0, 0xF2, 0xF3, 0x64, 0x65, 0x66, 0x67)
LATER = set([0x0F, 0xC0, 0xC1, 0xC8, 0xC9, 0xF1] + list(range(0x60, 0x70)))
TEXTS = ["Hello, this is the Accent.", "1234 dollars and 5.67 cents; 3rd of May 1999!", "\x1bR5\x1bP3\x1bV9\x1bM1",
         "What? Why not... (maybe) \"quoted\" -- e-mail: a@b.com",
         "The quick brown fox jumps over the lazy dog, 0123456789 times.", "\x1bR9",
         "Supercalifragilisticexpialidocious antidisestablishmentarianism.", "\x1bP9", "CAPITAL LETTERS AND A. B. C.",
         "\x1bR1\x1bP0\x1bV1\x1bM0", "slow and low", "\x1bR5\x1bP5\x1bV5"]


def main():
    blocks = {}

    def hook(uc, addr, size, _):
        if (addr, size) not in blocks:
            blocks[(addr, size)] = bytes(uc.mem_read(addr, size))    # code in the EMS frame changes: keep the bytes

    a = Accent(DVC, chip=SSI263C(), core="unicorn")
    a.uc.hook_add(UC_HOOK_BLOCK, hook)
    a.boot()
    for t in TEXTS:
        speech = not t.startswith("\x1b")
        a.say(t + ("\r" if speech else ""), speech=speech)
        for _ in range(400):
            if not a.busy():
                break
            a.skip(0.05)
    a.say("This sentence is cut off in the middle by a cancel, and it should stop.\r")
    a.skip(0.4)
    a.cancel()
    a.say("After the cancel.\r")
    for _ in range(200):
        if not a.busy():
            break
        a.skip(0.05)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
    mnemonics, prefixes, later, sensitive = collections.Counter(), collections.Counter(), [], []
    for (addr, _size), code in sorted(blocks.items()):
        insns = list(md.disasm(code, addr))
        for ins in insns:
            b = bytes(ins.bytes)
            i = 0
            while i < len(b) and b[i] in PREFIXES:
                prefixes[b[i]] += 1
                i += 1
            mnemonics[ins.mnemonic] += 1        # capstone names 98h "cwde" in 16-bit mode: it is CBW
            if (i < len(b) and b[i] in LATER) or any(p in b[:i] for p in (0x64, 0x65, 0x66, 0x67)):
                later.append("%05X %s %s (%s)" % (ins.address, ins.mnemonic, ins.op_str, b.hex()))
        if any(n.mnemonic in ("pushf", "popf") or (n.mnemonic in ("rcl", "rcr", "rol", "ror", "shl", "shr", "sar")
                                                    and "cl" in n.op_str.split(",")[-1]) for n in insns):
            sensitive.append("%05X: %s" % (addr, "; ".join("%s %s" % (n.mnemonic, n.op_str) for n in insns)))
    print("%d distinct blocks, %d chip writes, %d instructions asked of the CPU" % (len(blocks), len(a.writes), a.insns))
    print("%d mnemonics: %s" % (len(mnemonics), " ".join(sorted(mnemonics))))
    print("prefixes: %s" % ", ".join("%02Xh x%d" % kv for kv in sorted(prefixes.items())))
    print("80186-or-later forms: %s" % ("none" if not later else "\n  " + "\n  ".join(later)))
    print("blocks with PUSHF/POPF or a shift by CL (%d):" % len(sensitive))
    for s in sensitive:
        print("  " + s)


if __name__ == "__main__":
    main()
