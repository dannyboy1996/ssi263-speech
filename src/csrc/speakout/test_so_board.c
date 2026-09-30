/* test_so_board.c -- the Speak-Out board's own rules (so_board.h), on small programs: no firmware needed.
 *   hex             Intel HEX as speakout.py loads it: extended segments, data, end of file, a malformed record
 *   power_on        the reset vector's JMP runs as the first step, to 0000:0100, and FFFF0h-FFFF4h are restored
 *   chip_window     writes to F000:FE00-FE04 are the chip's R0-R4, in order; FE05 is RAM only; the RAM keeps them
 *   icu_vectors     ICW1/ICW2/OCW1 at ports 8-9; IR4 -> vector 0Ch, IR1 -> vector 9; the mask reads back at port 9
 *   icu_priority    an IR in service blocks itself and lower ones, not higher; the EOI clears the highest in service
 *   icu_masked      a masked IR's offer and an offer with IE = 0 are dropped, not held
 *   scu_bytes       status bit 1 while a byte is loaded; reading port 0 loads the next at once; bytes in order
 *   offer_order     with the chip requesting, IR4 is offered and no serial byte is loaded (speakout.py's order)
 *   drop_input      dropping the input empties the queue and the loaded byte
 *   unicorn_counts  so_run_steps_unicorn counts REP forms as Unicorn does (measured): n + 1 when the count ends it
 *   two_boards      two boards interleaved write what each writes alone
 * Each line: "ok"/"FAIL", the test's name, the detail; the last line "all passed" or "FAILED"; exit status 0/1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "so_board.h"
#include "so_hex.h"

static int failures;

static void report(const char *name, int ok, const char *detail)
{
    printf("%-4s %-14s %s\n", ok ? "ok" : "FAIL", name, detail);
    if (!ok)
        failures++;
}

#define POKE(b, addr, ...) do { static const uint8_t p_[] = {__VA_ARGS__}; so_poke((b), (addr), p_, (int)sizeof p_); } while (0)

static uint8_t peek(so_board *b, uint32_t a)
{
    uint8_t v;
    so_read(b, a, &v, 1);
    return v;
}

static void vec(so_board *b, int n, uint16_t off)
{
    uint8_t v[4];
    v[0] = (uint8_t)off;
    v[1] = (uint8_t)(off >> 8);
    v[2] = v[3] = 0;
    so_poke(b, (uint32_t)n * 4, v, 4);
}

/* 0100: SP = 8000h; ICW1 12h, ICW2 08h, OCW1 `mask`; STI (or CLI); JMP $.
   IR4's handler (vector 0Ch, 0200h): INC [9000h]; IRET (no EOI).
   IR1's handler (vector 9, 0240h): status -> [9003h], data -> [9010h + count], status -> [9004h], INC [9002h], EOI,
   IRET. */
static so_board *board(uint8_t mask, int ie)
{
    so_board *b = so_create();
    POKE(b, 0x100, 0xBC, 0x00, 0x80,                        /* MOV SP,8000h */
         0xB0, 0x12, 0xE6, 0x08, 0xB0, 0x08, 0xE6, 0x09,     /* ICW1, ICW2 */
         0xB0, 0x00, 0xE6, 0x09,                             /* OCW1 (the mask, at 010Ch) */
         0xFB, 0xEB, 0xFE);                                  /* STI (or CLI, at 010Fh); JMP $ */
    so_poke(b, 0x10C, &mask, 1);
    if (!ie)
        POKE(b, 0x10F, 0xFA);
    vec(b, 0x0C, 0x200);
    vec(b, 0x09, 0x240);
    POKE(b, 0x200, 0xFE, 0x06, 0x00, 0x90, 0xCF);
    POKE(b, 0x240, 0xE4, 0x01, 0xA2, 0x03, 0x90,             /* IN AL,1; MOV [9003h],AL */
         0xE4, 0x00, 0x8A, 0x1E, 0x02, 0x90, 0xB7, 0x00,     /* IN AL,0; MOV BL,[9002h]; MOV BH,0 */
         0x88, 0x87, 0x10, 0x90,                             /* MOV [BX+9010h],AL */
         0xE4, 0x01, 0xA2, 0x04, 0x90,                       /* IN AL,1; MOV [9004h],AL */
         0xFE, 0x06, 0x02, 0x90,                             /* INC [9002h] */
         0xB0, 0x20, 0xE6, 0x08, 0xCF);                      /* EOI; IRET */
    so_power_on(b);
    so_run_steps(b, 20);
    return b;
}

static void t_hex(void)
{
    static const char text[] = ":020000020010EC\r\n:03000400AABBCC00\n  :0100000077FF\n:00000001FF\n:010000005500\n";
    static const char bad[] = ":0300000011\n";
    uint8_t *mem = (uint8_t *)calloc(1, 0x100000);
    char d[200];
    long n = so_hex_parse(text, sizeof text - 1, mem);
    long m = so_hex_parse(bad, sizeof bad - 1, mem);
    sprintf(d, "stored %ld (want 4); 00104h-00106h %02X %02X %02X (want AA BB CC), 00100h %02X (want 77; the record after "
            "the end not read); malformed -> %ld (want -1)", n, mem[0x104], mem[0x105], mem[0x106], mem[0x100], m);
    report("hex", n == 4 && mem[0x104] == 0xAA && mem[0x105] == 0xBB && mem[0x106] == 0xCC && mem[0x100] == 0x77
                  && m == -1, d);
    free(mem);
}

static void t_power_on(void)
{
    so_board *b = so_create();
    uint32_t s[7];
    char d[160];
    POKE(b, 0xFFFF0, 0x11, 0x22, 0x33, 0x44, 0x55);
    POKE(b, 0x100, 0xEB, 0xFE);
    so_power_on(b);
    so_cpu_state(b, s);
    sprintf(d, "PS:IP %04X:%04X (want 0000:0100), steps %llu (want 1), FFFF0h.. %02X %02X %02X %02X %02X (want 11 .. 55)",
            s[0], s[1], (unsigned long long)so_steps(b), peek(b, 0xFFFF0), peek(b, 0xFFFF1), peek(b, 0xFFFF2),
            peek(b, 0xFFFF3), peek(b, 0xFFFF4));
    report("power_on", s[0] == 0 && s[1] == 0x100 && so_steps(b) == 1 && peek(b, 0xFFFF0) == 0x11
                       && peek(b, 0xFFFF4) == 0x55, d);
    so_destroy(b);
}

static so_board *chip_program(void)
{
    so_board *b = so_create();
    POKE(b, 0x100, 0xB8, 0x00, 0xF0, 0x8E, 0xD8,             /* MOV AW,F000h; MOV DS,AW */
         0xC6, 0x06, 0x00, 0xFE, 0x00, 0xC6, 0x06, 0x01, 0xFE, 0x51, 0xC6, 0x06, 0x02, 0xFE, 0xD8,
         0xC6, 0x06, 0x03, 0xFE, 0x5C, 0xC6, 0x06, 0x04, 0xFE, 0xE4, 0xC6, 0x06, 0x05, 0xFE, 0x77,
         0xC6, 0x06, 0x00, 0xFE, 0xC3, 0xEB, 0xDB);          /* ... R0 = C3; JMP to the MOV at 0105h */
    so_power_on(b);
    return b;
}

static void t_chip_window(void)
{
    so_board *b = chip_program();
    const so_write *w;
    int n, ok;
    char d[200];
    so_run_steps(b, 9);
    n = so_writes(b, &w);
    ok = n == 6 && w[0].reg == 0 && w[0].val == 0x00 && w[1].reg == 1 && w[1].val == 0x51 && w[2].reg == 2
         && w[3].reg == 3 && w[4].reg == 4 && w[4].val == 0xE4 && w[5].reg == 0 && w[5].val == 0xC3;
    sprintf(d, "%d writes (want 6: R0 00, R1 51, R2 D8, R3 5C, R4 E4, R0 C3); FFE05h = %02X (want 77, RAM only); "
            "FFE00h = %02X (want C3)", n, peek(b, 0xFFE05), peek(b, 0xFFE00));
    report("chip_window", ok && peek(b, 0xFFE05) == 0x77 && peek(b, 0xFFE00) == 0xC3, d);
    so_destroy(b);
}

static void t_icu_vectors(void)
{
    so_board *b = board(0xED, 1);                            /* IR1 and IR4 unmasked */
    int icu[2], r4, r1;
    char d[200];
    so_icu_state(b, icu);
    r4 = so_offer(b, 1);
    so_run_steps(b, 4);
    so_send(b, (const uint8_t *)"Z", 1);
    r1 = so_offer(b, 0);
    so_run_steps(b, 20);
    sprintf(d, "mask %02X (want ED); IR4 offer -> %d, handler at 0200h ran %d (want 4, 1); IR1 offer -> %d, handler "
            "at 0240h read '%c' (want 1, Z)", icu[0], r4, peek(b, 0x9000), r1, peek(b, 0x9010));
    report("icu_vectors", icu[0] == 0xED && r4 == 4 && peek(b, 0x9000) == 1 && r1 == 1 && peek(b, 0x9010) == 'Z', d);
    so_destroy(b);
}

static void t_icu_priority(void)
{
    so_board *b = board(0xED, 1);
    int a, c, icu1[2], icu2[2];
    char d[220];
    a = so_offer(b, 1);                                      /* IR4 taken; its handler sends no EOI */
    so_run_steps(b, 4);
    c = so_offer(b, 1);                                      /* IR4 again: blocked by itself */
    so_icu_state(b, icu1);
    so_send(b, (const uint8_t *)"Q", 1);
    so_offer(b, 0);                                          /* IR1: higher, taken; its EOI clears IR1, not IR4 */
    so_run_steps(b, 20);
    so_icu_state(b, icu2);
    sprintf(d, "IR4 %d, again %d (want 4, -1); in service %02X then %02X (want 10, 10: IR1's EOI cleared IR1); IR1's "
            "handler ran %d", a, c, icu1[1], icu2[1], peek(b, 0x9002));
    report("icu_priority", a == 4 && c == -1 && icu1[1] == 0x10 && icu2[1] == 0x10 && peek(b, 0x9002) == 1, d);
    so_destroy(b);
}

static void t_icu_masked(void)
{
    so_board *b = board(0xFD, 1);                            /* IR4 masked */
    so_board *c = board(0xED, 0);                            /* IE off */
    int x, y, z;
    char d[200];
    x = so_offer(b, 1);
    so_run_steps(b, 20);
    y = so_offer(c, 1);
    so_run_steps(c, 20);
    POKE(c, 0x10F, 0xFB);                                    /* the program's CLI becomes STI ... */
    POKE(c, 0x110, 0xEB, 0xFD);                              /* ... and loops to it: IE on from now */
    so_run_steps(c, 20);
    z = peek(c, 0x9000);
    sprintf(d, "masked IR4 -> %d, handler %d (want -1, 0); IE off -> %d, handler %d after IE is set, no new offer "
            "(want -1, 0)", x, peek(b, 0x9000), y, z);
    report("icu_masked", x == -1 && peek(b, 0x9000) == 0 && y == -1 && z == 0, d);
    so_destroy(b);
    so_destroy(c);
}

static void t_scu_bytes(void)
{
    so_board *b = board(0xED, 1);
    char d[200];
    uint8_t s1, s2, s3;
    so_send(b, (const uint8_t *)"AB", 2);
    so_offer(b, 0);
    so_run_steps(b, 30);
    s1 = peek(b, 0x9003);
    s2 = peek(b, 0x9004);
    so_offer(b, 0);                                          /* B is already loaded */
    so_run_steps(b, 30);
    s3 = peek(b, 0x9004);
    sprintf(d, "status %02X, after reading A %02X (B loaded at once), after B %02X (want 03 03 01); bytes %c%c, %d "
            "read; queued %d", s1, s2, s3, peek(b, 0x9010), peek(b, 0x9011), peek(b, 0x9002), so_input_queued(b));
    report("scu_bytes", s1 == 3 && s2 == 3 && s3 == 1 && peek(b, 0x9010) == 'A' && peek(b, 0x9011) == 'B'
                        && peek(b, 0x9002) == 2
                        && so_input_queued(b) == 0, d);
    so_destroy(b);
}

static void t_offer_order(void)
{
    so_board *b = board(0xED, 1);
    int r;
    char d[160];
    so_send(b, (const uint8_t *)"X", 1);
    r = so_offer(b, 1);
    so_run_steps(b, 4);
    sprintf(d, "offer with the chip requesting and a byte queued -> %d (want 4); IR4's handler ran %d, IR1's %d (want "
            "1, 0); input still queued %d", r, peek(b, 0x9000), peek(b, 0x9002), so_input_queued(b));
    report("offer_order", r == 4 && peek(b, 0x9000) == 1 && peek(b, 0x9002) == 0 && so_input_queued(b), d);
    so_destroy(b);
}

static void t_drop_input(void)
{
    so_board *b = board(0xED, 0);                            /* IE off: the byte is loaded, never read */
    char d[120];
    int before;
    so_send(b, (const uint8_t *)"XYZ", 3);
    so_offer(b, 0);
    before = so_input_queued(b);
    so_drop_input(b);
    sprintf(d, "queued before %d, after %d (want 1, 0)", before, so_input_queued(b));
    report("drop_input", before == 1 && so_input_queued(b) == 0, d);
    so_destroy(b);
}

/* MOV IY,2000h; MOV AL,55h; CLD; MOV CW,n; the REP form; NOP (0100h..) -- counted by so_run_steps_unicorn one count
   at a time until the NOP has run, so a count carried past the REP's own is counted too: the counts the REP took */
static int unicorn_counts(uint8_t n, uint8_t rep, uint8_t op)
{
    so_board *b = so_create();
    uint8_t code[12] = {0xBF, 0x00, 0x20, 0xB0, 0x55, 0xFC, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x90};
    static const uint8_t data[8] = {1, 2, 3, 0x55, 5, 6, 7, 8};
    uint32_t s[7];
    int k = 0;
    code[7] = n;
    code[9] = rep;
    code[10] = op;
    so_poke(b, 0x100, code, 12);
    so_poke(b, 0x2000, data, 8);
    so_power_on(b);
    for (;;) {
        so_cpu_state(b, s);
        if (s[1] >= 0x10C || k > 50)             /* the NOP has run */
            break;
        so_run_steps_unicorn(b, 1);
        k++;
    }
    so_destroy(b);
    return k - 5;                                /* MOV IY, MOV AL, CLD, MOV CW: four counts; the NOP: one */
}

static void t_unicorn_counts(void)
{
    int a = unicorn_counts(4, 0xF3, 0xAA), c = unicorn_counts(8, 0xF2, 0xAE), e = unicorn_counts(3, 0xF2, 0xAE);
    int f = unicorn_counts(4, 0xF2, 0xAE), g = unicorn_counts(8, 0xF3, 0xAE), h = unicorn_counts(0, 0xF3, 0xAA);
    char d[240];
    sprintf(d, "REP STOSB of 4: %d; REPNE SCASB found at 4 of 8: %d, not found in 3: %d, found at 4 of 4: %d; REPE SCASB "
            "differing at 1: %d; CW = 0: %d (want 5 4 4 4 1 1, as Unicorn counts them)", a, c, e, f, g, h);
    report("unicorn_counts", a == 5 && c == 4 && e == 4 && f == 4 && g == 1 && h == 1, d);
}

static void t_two_boards(void)
{
    enum { N = 400 };
    so_board *a = chip_program(), *b = chip_program(), *c = chip_program();
    const so_write *wa, *wb, *wc;
    int i, na, nb, nc, same;
    char d[120];
    so_run_steps(a, N);
    for (i = 0; i < N; i++) {
        so_run_steps(b, 1);
        so_run_steps(c, 1);
    }
    na = so_writes(a, &wa);
    nb = so_writes(b, &wb);
    nc = so_writes(c, &wc);
    same = na == nb && na == nc && !memcmp(wa, wb, (size_t)na * sizeof *wa) && !memcmp(wa, wc, (size_t)na * sizeof *wa)
           && so_cycles(a) == so_cycles(b);
    sprintf(d, "%d writes alone, %d and %d interleaved: %s", na, nb, nc, same ? "identical" : "DIFFERENT");
    report("two_boards", same && na > 100, d);
    so_destroy(a);
    so_destroy(b);
    so_destroy(c);
}

int main(void)
{
    t_hex();
    t_power_on();
    t_chip_window();
    t_icu_vectors();
    t_icu_priority();
    t_icu_masked();
    t_scu_bytes();
    t_offer_order();
    t_drop_input();
    t_unicorn_counts();
    t_two_boards();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
