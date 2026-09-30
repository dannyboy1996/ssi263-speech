"""Speak-Out (1995, NEC V40) firmware driving the engine's SSI-263.

The firmware is NOT part of this repository: pass the path of your own
SPEAKOUT.HEX (Intel HEX, loaded 0000:0100).  What the host provides, all read
out of the firmware (see the Speak-Out memory note for offsets):

  * the V40's on-chip interrupt controller at ports 8-9 (ICW1 12h, vectors 8-F),
    reduced to a mask, an in-service set and non-specific EOI (out 8, 20h);
  * its serial unit at ports 0-3: text arrives as received bytes on IR1 (INT 9),
    status bit 1 = byte ready, port 0 = the byte;
  * the SSI-263 memory-mapped at F000:FE00-FE04 (R0-R4).  The firmware never
    reads the chip; A/R is its IR4 (INT 0Ch), whose handler writes the next
    frame (R0 = 00, R1, R2, R3, R4, then the phoneme byte into R0).

CPU time is coupled to chip time at `cpu_ips` instructions per chip second.

The CPU is Unicorn's.  SSI263_SPEAKOUT_CORE (opt-in, for comparison; not yet accepted) selects MAME's V40 core
instead, on the same board in C (src/csrc/speakout, through speakout_v40.py): "mame-steps" couples its steps at
`cpu_ips`, as Unicorn's instructions are -- the candidate for replacing Unicorn under this host; "mame" couples CPU
clocks to chip time at SSI263_SPEAKOUT_V40_HZ (default V40_HZ) -- experimental (nvda/tools/speakout_core_compare.py).
"""
import os
import sys

# Packaged (the NVDA add-ons: synthDrivers._ssi263_<name>), the engine is imported relatively,
# never from sys.path: both add-ons ship an `ssi263`, and NVDA keeps one module per name for
# the whole process (0.3.0: Braille Lite imported the older Speak-Out's copy and failed).
try:
    from .ucmini import (Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INSN, UC_HOOK_MEM_WRITE,
                         UC_X86_INS_IN, UC_X86_INS_OUT, UC_X86_REG_CS, UC_X86_REG_IP,
                         UC_X86_REG_SS, UC_X86_REG_SP, UC_X86_REG_EFLAGS)
    from .ssi263 import SSI263
except ImportError:                       # the research tree: src/ on sys.path
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from hosts.ucmini import (Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INSN,  # noqa: E402
                              UC_HOOK_MEM_WRITE, UC_X86_INS_IN, UC_X86_INS_OUT, UC_X86_REG_CS,
                              UC_X86_REG_IP, UC_X86_REG_SS, UC_X86_REG_SP, UC_X86_REG_EFLAGS)
    from ssi263 import SSI263  # noqa: E402


CHIP_BASE = 0xF0000 + 0xFE00
IRQ_SERIAL, IRQ_CHIP = 1, 4
V40_HZ = 8_000_000          # "mame" (experimental): the uPD70208-8's speed grade, NOT a measured clock (the unit's
                            # oscillator is not read, and the V40 may divide it)


def _core():
    """SSI263_SPEAKOUT_CORE: unicorn (the default), mame or mame-steps."""
    core = os.environ.get("SSI263_SPEAKOUT_CORE", "").strip().lower() or "unicorn"
    if core not in ("unicorn", "mame", "mame-steps"):
        raise ValueError("SSI263_SPEAKOUT_CORE=%s: unicorn, mame or mame-steps" % core)
    return core


def load_intel_hex(path):
    """Intel HEX with extended segment records -> {linear address: byte}."""
    mem, seg = {}, 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            b = bytes.fromhex(line[1:])
            n, addr, typ = b[0], (b[1] << 8) | b[2], b[3]
            data = b[4:4 + n]
            if typ == 0:
                for i, v in enumerate(data):
                    mem[(seg + addr + i) & 0xFFFFF] = v
            elif typ == 2:
                seg = ((data[0] << 8) | data[1]) << 4
            elif typ == 1:
                break
    return mem


class SpeakOut:
    def __new__(cls, *args, **kwargs):
        if cls is SpeakOut and _core() != "unicorn":
            cls = SpeakOutV40
        return object.__new__(cls)

    def __init__(self, hex_path, chip=None, out_rate=44100, cpu_ips=1_500_000):
        self.chip = chip or SSI263(out_rate=out_rate)
        self.cpu_ips = cpu_ips
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_16)
        uc.mem_map(0, 0x100000)
        mem = load_intel_hex(hex_path)
        img = bytearray(0x100000)
        for a, v in mem.items():
            img[a] = v
        uc.mem_write(0, bytes(img))
        self.imr = 0xFF            # interrupt mask (1 = masked)
        self.isr = 0               # in service
        self.icw_step = 0
        self.rx = []               # queued serial bytes
        self.rx_byte = None
        self.port_log = {}
        self.chip_writes = []      # (chip time, reg, value)
        self.keep_writes = True    # an NVDA driver turns this off: the list would grow forever
        self.last_speech = -1.0    # chip time of the last non-idle phoneme load
        self.preparing = False     # text with words sent, none of it spoken yet
        self.say_time = 0.0
        uc.hook_add(UC_HOOK_INSN, self._in, None, 1, 0, UC_X86_INS_IN)
        uc.hook_add(UC_HOOK_INSN, self._out, None, 1, 0, UC_X86_INS_OUT)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._mem_write, None, CHIP_BASE, CHIP_BASE + 7)
        uc.reg_write(UC_X86_REG_CS, 0)
        uc.reg_write(UC_X86_REG_IP, 0x100)
        self.insns = 0

    # ---- V40 peripherals ---------------------------------------------------------
    def _in(self, uc, port, size, _):
        if port == 1:                                   # serial status: TxRDY, RxRDY
            return 0x01 | (0x02 if self.rx_byte is not None else 0)
        if port == 0:                                   # serial data
            v = self.rx_byte if self.rx_byte is not None else 0
            self.rx_byte = self.rx.pop(0) if self.rx else None
            return v
        if port == 9:
            return self.imr
        self.port_log[("in", port)] = self.port_log.get(("in", port), 0) + 1
        return 0

    def _out(self, uc, port, size, value, _):
        if port == 8:
            if value & 0x10:                            # ICW1
                self.icw_step = 1
            elif value == 0x20 and self.isr:            # non-specific EOI
                self.isr &= self.isr - 1                # clear the highest priority (lowest bit)
        elif port == 9:
            if self.icw_step == 1:                      # ICW2 (vector base); single, no ICW4
                self.icw_step = 0
            else:                                       # OCW1
                self.imr = value & 0xFF
        else:
            self.port_log[("out", port)] = self.port_log.get(("out", port), 0) + 1

    def _mem_write(self, uc, access, addr, size, value, _):
        reg = addr - CHIP_BASE
        if 0 <= reg <= 4:
            self._chip_write(reg, value & 0xFF)

    def _chip_write(self, reg, v):
        self.chip.write(reg, v)
        if self.keep_writes:
            self.chip_writes.append((round(self.chip.time, 6), reg, v))
        # Idle = the start-of-utterance routine (0x4318): PA/3 with R2 forced to rate F.
        # Speech never uses rate F, and every frame first primes R0 with 00.
        if reg == 0 and v != 0x00 and not (v == 0xC0 and (self.chip.regs[2] >> 4) == 0xF):
            self.last_speech = self.chip.time
            self.preparing = False

    # ---- CPU ---------------------------------------------------------------------
    def _cpu(self, count):
        uc = self.uc
        cs = uc.reg_read(UC_X86_REG_CS)
        ip = uc.reg_read(UC_X86_REG_IP)
        uc.emu_start(cs * 16 + ip, 0, count=count)
        self.insns += count

    def _push(self, v):
        uc = self.uc
        sp = (uc.reg_read(UC_X86_REG_SP) - 2) & 0xFFFF
        uc.reg_write(UC_X86_REG_SP, sp)
        uc.mem_write(uc.reg_read(UC_X86_REG_SS) * 16 + sp, int(v & 0xFFFF).to_bytes(2, "little"))

    def _try_irq(self, irq):
        uc = self.uc
        fl = uc.reg_read(UC_X86_REG_EFLAGS)
        if not fl & 0x200 or self.imr >> irq & 1 or self.isr & ((2 << irq) - 1):
            return False
        self._push(fl)
        self._push(uc.reg_read(UC_X86_REG_CS))
        self._push(uc.reg_read(UC_X86_REG_IP))
        uc.reg_write(UC_X86_REG_EFLAGS, fl & ~0x300)
        vec = int.from_bytes(uc.mem_read((8 + irq) * 4, 4), "little")
        uc.reg_write(UC_X86_REG_IP, vec & 0xFFFF)
        uc.reg_write(UC_X86_REG_CS, vec >> 16)
        self.isr |= 1 << irq
        return True

    # ---- host --------------------------------------------------------------------
    def call(self, addr):
        """Near-call a firmware routine from wherever the CPU is (CS = 0)."""
        self._push(self.uc.reg_read(UC_X86_REG_IP))
        self.uc.reg_write(UC_X86_REG_IP, addr)

    def start_self_test(self, max_steps=200000):
        """The box's own self-test (0x4579): intro, bells, rates, pitches, volumes, tones.

        Entered from the main loop (0x414-0x471) with no interrupt in service, as the
        box's own menu would; entering from inside an ISR would leave it in service."""
        uc = self.uc
        for _ in range(max_steps):
            ip = uc.reg_read(UC_X86_REG_IP)
            if uc.reg_read(UC_X86_REG_CS) == 0 and 0x414 <= ip <= 0x471 and not self.isr:
                self.call(0x4579)
                return True
            self._cpu(1)
        raise RuntimeError("never reached the main loop")

    def say(self, text):
        """Queue text.  Anything with letters or digits is expected to speak: until its
        first phoneme plays the box counts as busy, because the rules can take well
        over 60 ms on a hard word ("ETI-Eloquence", measured) with every buffer empty."""
        body = "".join(ch for ch in text if ch >= " ")
        if any(ch.isalnum() for ch in body.replace("\x05", "")) and not text.startswith("\x05"):
            self.preparing = True
            self.say_time = self.chip.time
        self._queue(text.encode("latin-1", "replace"))

    # the serial input and the firmware's memory: overridden by SpeakOutV40, whose board holds them
    def _queue(self, data):
        self.rx.extend(data)

    def _drop_input(self):
        self.rx.clear()
        self.rx_byte = None

    def _input_queued(self):
        return bool(self.rx) or self.rx_byte is not None

    def _word(self, addr):
        return int.from_bytes(self.uc.mem_read(addr, 2), "little")

    def _offer(self):
        """A slice's start: the chip's request first, else a serial byte (loaded if none is)."""
        if self.chip.request:
            self._try_irq(IRQ_CHIP)
        elif (self.rx or self.rx_byte is not None):
            if self.rx_byte is None:
                self.rx_byte = self.rx.pop(0)
            self._try_irq(IRQ_SERIAL)

    def input_pending(self):
        """Text not yet taken in, or taken in and not yet spoken by the rules."""
        if self._input_queued():
            return True
        if self._word(0x1A32) != self._word(0x1A30):
            return True
        # the phoneme-frame ring the chip ISR consumes (0x1119: [0x243C] chases [0x243A])
        return self._word(0x243C) != self._word(0x243A)

    def busy(self, quiet=0.06, patience=1.5):
        """Still speaking: input pending, a line not yet started (up to `patience` s),
        or a speech phoneme loaded within `quiet` s."""
        if self.input_pending() or (self.chip.time - self.last_speech) < quiet:
            return True
        return self.preparing and (self.chip.time - self.say_time) < patience

    def boot(self, seconds=0.3):
        """Power on and silence the greeting (Ctrl-X), discarding the audio."""
        self.run(seconds)
        self.cancel()
        self.run(0.2)

    def cancel(self, limit=0.6):
        """Flush the box: drop queued input and send Ctrl-X, as a host would; then let any
        frames it had already queued play out SILENTLY (the chip keeps time, no sound),
        so none of the cut speech reaches the next utterance."""
        self._drop_input()
        self.preparing = False
        self.say("\x18")
        self.skip(0.02)
        t = 0.02
        while t < limit and self.busy():
            self.skip(0.02)
            t += 0.02

    def skip(self, seconds, step=0.002):
        """Run the firmware with the chip keeping time but making no sound."""
        t = 0.0
        while t < seconds - 1e-9:
            self._offer()
            before = self.chip.time
            self.chip.skip(min(step, seconds - t))
            dt = max(self.chip.time - before, 1e-5)
            self._cpu(max(200, int(self.cpu_ips * dt)))
            t += dt
        return t

    def run(self, seconds, step=0.0005):
        out, t = [], 0.0
        while t < seconds:
            self._offer()
            before = self.chip.time
            y = self.chip.run_until_request(step) if not self.chip.request else self.chip.run(step / 4)
            out.append(y)
            dt = max(self.chip.time - before, 1e-5)
            self._cpu(max(200, int(self.cpu_ips * dt)))
            t += dt
        return self.chip.dsp.concat(out)


class SpeakOutV40(SpeakOut):
    """The same host on MAME's V40 core (opt-in: SSI263_SPEAKOUT_CORE=mame or mame-steps; SpeakOut() returns one).

    Only the CPU changes: the board in C (src/csrc/speakout) has the same memory, the same chip window and the ICU and
    SCU reduced as above; this class keeps the chip-time loop, the chip and its bookkeeping.  "mame-steps" couples its
    steps as Unicorn's instructions are, counted as Unicorn counts them (a REP ended by its count is one more:
    so_board.h): the migration candidate, comparable with Unicorn step for step.  "mame" (experimental) couples the
    core's CLOCKS to chip time at clock_hz (a slice of n instructions above is n * clock_hz / cpu_ips clocks); no
    clock_hz is established for the unit.
    The chip's writes are applied after each slice, in order: nothing reads the chip during one."""

    def __init__(self, hex_path, chip=None, out_rate=44100, cpu_ips=1_500_000, core=None, clock_hz=None):
        try:
            from .speakout_v40 import Board
        except ImportError:                   # the research tree
            from hosts.speakout_v40 import Board  # noqa: E402
        self.chip = chip or SSI263(out_rate=out_rate)
        self.cpu_ips = cpu_ips
        self.core = core or _core()
        self.clock_hz = float(clock_hz or os.environ.get("SSI263_SPEAKOUT_V40_HZ") or V40_HZ)
        self.board = Board(hex_path)
        self.uc = None
        self.port_log = {}
        self.chip_writes = []
        self.keep_writes = True
        self.last_speech = -1.0
        self.preparing = False
        self.say_time = 0.0
        self.insns = 0

    def _cpu(self, count):
        b = self.board
        s0 = b.steps()
        if self.core == "mame-steps":
            b.run_steps_unicorn(count)
        else:
            b.run_cycles(int(count * self.clock_hz / self.cpu_ips))
        for reg, v in b.take_writes():
            self._chip_write(reg, v)
        self.insns += b.steps() - s0

    def _offer(self):
        self.board.offer(self.chip.request)

    def _queue(self, data):
        self.board.send(data)

    def _drop_input(self):
        self.board.drop_input()

    def _input_queued(self):
        return self.board.input_queued()

    def _word(self, addr):
        return int.from_bytes(self.board.read(addr, 2), "little")

    def call(self, addr):
        raise NotImplementedError("the MAME core's board has no register access (start_self_test: Unicorn only)")

    def start_self_test(self, max_steps=200000):
        raise NotImplementedError("start_self_test: Unicorn only")

    @property
    def isr(self):
        return self.board.icu_state()["isr"]

    @property
    def imr(self):
        return self.board.icu_state()["imr"]
