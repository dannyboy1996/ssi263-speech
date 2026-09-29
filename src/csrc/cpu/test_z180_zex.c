/* test_z180_zex.c -- runs a CP/M instruction exerciser (zexdoc.com or zexall.com) on a cpu.h Z180 core.
 *
 * The exercisers (Frank Cringle's, GPL) are test inputs only and are not in this repository; pass the path.
 * A minimal CP/M: the program at 0100h, "JP BDOS" at 0005h, BDOS functions 2 (print E) and 9 (print $-string at
 * DE) answered through three external OUTs of a stub at FF00h, and a HALT at 0000h for the warm boot.  Memory is a
 * flat 64K: after reset the Z180's MMU maps logical to physical one to one.
 *
 * Expected on a Z180: the groups that use the Z80's undocumented IXH/IXL/IYH/IYL instructions fail (the Z180
 * treats DD/FD before an H or L instruction as an undefined opcode; MAME's core then runs the plain H/L form).
 * The acceptance for the MAME core is that every documented group passes and that the failures are the same as
 * the legacy core's.
 *
 *   build (MAME):   g++ -O2 -I. -I.. -x c test_z180_zex.c -x c++ z180_mame.cpp z180_asci.cpp -o zex_mame
 *   build (legacy): gcc -O2 -DZEX_LEGACY -I. -I.. -I<z180emu> -I<z180emu>/z180 test_z180_zex.c z180_legacy.c ...
 *   run:            zex_mame zexdoc.com
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

typedef struct {
    uint8_t mem[65536];
    uint8_t c, e, d;
    int fails;
    char line[256];
    int line_n;
} cpm;

static uint8_t rd(void *ctx, uint32_t a) { return ((cpm *)ctx)->mem[a & 0xFFFF]; }
static void wr(void *ctx, uint32_t a, uint8_t v) { ((cpm *)ctx)->mem[a & 0xFFFF] = v; }
static uint8_t in(void *ctx, uint16_t p) { (void)ctx; (void)p; return 0xFF; }

static void put(cpm *m, char ch)
{
    putchar(ch);
    if (ch == '\n' || m->line_n == (int)sizeof m->line - 1) {
        m->line[m->line_n] = 0;
        if (strstr(m->line, "ERROR"))
            m->fails++;
        m->line_n = 0;
    } else if (ch != '\r') {
        m->line[m->line_n++] = ch;
    }
}

static void out(void *ctx, uint16_t p, uint8_t v)
{
    cpm *m = (cpm *)ctx;
    switch (p & 0xFF) {
    case 0x80: m->c = v; break;
    case 0x81: m->e = v; break;
    case 0x82:
        m->d = v;
        if (m->c == 2) {
            put(m, (char)m->e);
        } else if (m->c == 9) {
            unsigned a = (unsigned)(m->d << 8 | m->e);
            while (m->mem[a & 0xFFFF] != '$')
                put(m, (char)m->mem[a++ & 0xFFFF]);
        }
        fflush(stdout);
        break;
    }
}

int main(int argc, char **argv)
{
    static cpm m;
    static const uint8_t bdos[] = {
        0x79, 0xD3, 0x80,       /* LD A,C ; OUT (80h),A */
        0x7B, 0xD3, 0x81,       /* LD A,E ; OUT (81h),A */
        0x7A, 0xD3, 0x82,       /* LD A,D ; OUT (82h),A */
        0xC9                    /* RET */
    };
    cpu_bus bus;
    z180 *c;
    z180_regs r;
    FILE *f;
    size_t n;
    unsigned long long total = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s zexdoc.com\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    n = fread(m.mem + 0x100, 1, 0xFE00 - 0x100, f);
    fclose(f);
    if (!n)
        return 2;
    m.mem[0x0000] = 0x76;                               /* warm boot: HALT */
    m.mem[0x0005] = 0xC3; m.mem[0x0006] = 0x00; m.mem[0x0007] = 0xFF;   /* JP FF00h; (6) = top of the TPA */
    memcpy(m.mem + 0xFF00, bdos, sizeof bdos);

    memset(&bus, 0, sizeof bus);
    bus.ctx = &m;
    bus.read = rd;
    bus.write = wr;
    bus.in = in;
    bus.out = out;
    c = z180_create(&bus, 6144000.0);
    if (!c)
        return 2;
    {                                                   /* start at 0100h: a JP there from the reset vector */
        m.mem[0x0000] = 0xC3; m.mem[0x0001] = 0x00; m.mem[0x0002] = 0x01;
    }
    for (;;) {
#ifdef ZEX_LEGACY
        total += z180_run_legacy(c, 1000000);
#else
        total += z180_run(c, 1000000);
#endif
        if (m.mem[0x0000] == 0xC3 && z180_pc(c) >= 0x0100)
            m.mem[0x0000] = 0x76, m.mem[0x0001] = 0, m.mem[0x0002] = 0;   /* once running: warm boot halts */
        z180_regs_get(c, &r);
        if (r.halted)
            break;
    }
    printf("\n[zex] %llu T-states, %d group(s) failed; halted at %04X (%02X %02X %02X)%s\n", total, m.fails,
           r.pc, m.mem[r.pc], m.mem[(r.pc + 1) & 0xFFFF], m.mem[(r.pc + 2) & 0xFFFF],
           r.pc == 0 ? ": the warm boot, done" : ": NOT the warm boot");
    z180_destroy(c);
    return 0;
}
