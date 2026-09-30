/* test_i8085_cpm.c -- runs a CP/M 8080/8085 test program on the cpu.h 8085 core (TST8080, 8080PRE, 8080EXM,
 * CPUTEST: GPL test inputs, not in this repository; pass the path).
 *
 * A minimal CP/M: the program at 0100h, BDOS at 0005h, functions 2 (print E) and 9 (print $-string at DE) answered
 * through three external OUTs of a stub at FF00h, and a HLT at 0000h for the warm boot (reached by JMP 0 or RST 0).
 * The stack starts under the stub, and 0006h holds its address (the top of the TPA, as the programs expect).
 * A line containing "ERROR" or "FAIL" counts as a failure.  The last line says where it halted.
 *
 *   build: gcc -O2 -I. -c test_i8085_cpm.c; g++ -O2 -std=c++17 -fno-exceptions -fno-rtti -I. -c i8085_mame.cpp;
 *          g++ -o cpm85 test_i8085_cpm.o i8085_mame.o
 *   run:   cpm85 8080EXM.COM
 * The 8080 exercisers' CRCs are an 8080's: on an 8085, groups whose results include the flags' undefined bits
 * (1, 3, 5: "X" in Intel's PUSH PSW listing; MAME keeps the undocumented V and K there) or the 8085's own AC rules
 * may report a different CRC.  Exit status 0 when the program reached its warm boot, whatever it printed.
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
        if (strstr(m->line, "ERROR") || strstr(m->line, "FAIL"))
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
        0xF5,                   /* PUSH PSW */
        0x79, 0xD3, 0x80,       /* MOV A,C ; OUT 80h */
        0x7B, 0xD3, 0x81,       /* MOV A,E ; OUT 81h */
        0x7A, 0xD3, 0x82,       /* MOV A,D ; OUT 82h */
        0xF1, 0xC9              /* POP PSW ; RET */
    };
    cpu_bus bus;
    i8085 *c;
    i8085_regs r;
    FILE *f;
    size_t n;
    unsigned long long total = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s program.com\n", argv[0]);
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
    /* from reset: LXI SP,FF00h; JMP 0100h at 0000h, replaced by the warm boot's HLT once the program runs */
    m.mem[0x0000] = 0x31; m.mem[0x0001] = 0x00; m.mem[0x0002] = 0xFF;
    m.mem[0x0003] = 0xC3; m.mem[0x0004] = 0x00; m.mem[0x0005] = 0x01;
    memcpy(m.mem + 0xFF00, bdos, sizeof bdos);

    memset(&bus, 0, sizeof bus);
    bus.ctx = &m;
    bus.read = rd;
    bus.write = wr;
    bus.in = in;
    bus.out = out;
    c = i8085_create(&bus, 3072000.0);
    if (!c)
        return 2;
    for (;;) {
        total += i8085_run(c, 100000);
        if (m.mem[0x0000] == 0x31 && i8085_pc(c) >= 0x0100) {
            m.mem[0x0000] = 0x76;                           /* warm boot: HLT */
            m.mem[0x0005] = 0xC3; m.mem[0x0006] = 0x00; m.mem[0x0007] = 0xFF;   /* JMP BDOS; (6) = top of TPA */
        }
        i8085_regs_get(c, &r);
        if (r.halted || total > 40000000000ULL)
            break;
    }
    printf("\n[cpm85] %llu T-states, %d failure line(s); halted at %04X%s\n", total, m.fails, r.pc,
           r.halted && r.pc == 0 ? ": the warm boot, done" : ": NOT the warm boot");
    i8085_destroy(c);
    return r.halted && r.pc == 0 ? 0 : 1;
}
