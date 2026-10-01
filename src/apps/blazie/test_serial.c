/* test_serial.c -- the unit's serial port plugged in, headless: the storage handshake the firmware speaks to WinDisk,
 * PCDISK or the disk drive, answered from here as the far end would.
 *
 *   test_serial bl FIRMWARE STATE     the Braille Lite: s-chord (storage)
 *   test_serial tns FIRMWARE -        the Type 'n Speak from cold: F8 (storage)
 *
 * What the firmware does (measured, see ../../csrc/blazie/bl_serial.h): on the storage key it asks the disk drive's
 * port (ASCI1, not carried) with ENQ, then switches the serial port on at 19200 bit/s and sends XON ENQ.  A far end
 * that answers ACK gets 'C' back and the unit says "storage"; no answer, or NAK, and it says "storage device
 * missing" and sends nothing more.  So the checks: XON ENQ and nothing else leave, under 19200 8N1 with the port
 * powered; ACK is answered with 'C' (the firmware received it); NAK is not (the control); input is taken at the
 * programmed rate, in the unit's time; then the directory command (d, e-chord) goes out as ENQ "d" CR.
 * Built with -DBL_SERIAL_CUT_RX (test_serial_cut.exe) the receive path is cut and "ACK answered" must FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu_unit.h"

#define RATE 11025
#define BLOCK (RATE / 200)                 /* 5 ms of the unit per render */
#define LOG_MAX 4096

static int failures;
static emu_unit *u;
static long rendered;                      /* samples: the unit's time is rendered / RATE */
static unsigned char got[LOG_MAX];         /* every byte the unit sent, with the status it left under */
static bl_serial_status got_st[LOG_MAX];
static int n_got;
static bl_serial_status now;               /* the status last handed out (with 0 bytes: the status now) */

static void check(const char *name, int ok, const char *detail)
{
    printf("%-4s %-28s %s\n", ok ? "ok" : "FAIL", name, detail);
    failures += !ok;
}

static void drain(void)
{
    unsigned char b[256];
    bl_serial_status st;
    int n, i;
    while ((n = emu_serial_read(u, b, sizeof b, &st)) > 0)
        for (i = 0; i < n && n_got < LOG_MAX; i++) {
            got[n_got] = b[i];
            got_st[n_got++] = st;
        }
    now = st;
}

static void run(double seconds)
{
    static short buf[BLOCK];
    long end = rendered + (long)(seconds * RATE);
    while (rendered < end) {
        emu_render(u, buf, BLOCK);
        rendered += BLOCK;
        drain();
    }
}

/* runs until the unit has sent `byte` (at or after log index `from`) or `limit` s pass; its index, or -1 */
static int run_until_sent(unsigned char byte, int from, double limit)
{
    static short buf[BLOCK];
    long end = rendered + (long)(limit * RATE);
    int i;
    while (rendered < end) {
        for (i = from; i < n_got; i++)
            if (got[i] == byte)
                return i;
        emu_render(u, buf, BLOCK);
        rendered += BLOCK;
        drain();
    }
    for (i = from; i < n_got; i++)
        if (got[i] == byte)
            return i;
    return -1;
}

static void hexes(char *out, int cap, int from, int to)
{
    int i, k = 0;
    out[0] = 0;
    for (i = from; i < to && k + 4 < cap; i++)
        k += snprintf(out + k, cap - k, "%s%02X", i > from ? " " : "", got[i]);
}

static void key(int kind, int code)
{
    if (kind == EMU_TYPE_N_SPEAK) {
        emu_key(u, code | 0x80);
        emu_key(u, code & 0x7F);
    } else
        emu_key(u, code);
}

/* boot, press the storage key, wait for the unit's ENQ on the serial port, answer `reply`; 1 if the unit sent 'C'
   back within 0.5 s.  `report`: run the checks along the way */
static int handshake(int kind, const char *fw, const char *st, unsigned char reply, int report)
{
    char err[256], d[300], hx[100];
    int enq, from, c, i;
    u = emu_create(kind, fw, st, RATE, 0, err, sizeof err);
    if (!u) {
        printf("FAIL create: %s\n", err);
        exit(1);
    }
    emu_set_flash_timed(u, 0);              /* the cold start's flash erase done at once (its 32 s and chirps are
                                               test_flash.c's): the storage handshake follows within seconds */
    rendered = 0;
    n_got = 0;
    if (!emu_serial_attach(u, 1)) {
        printf("FAIL attach\n");
        exit(1);
    }
    if (kind == EMU_TYPE_N_SPEAK) {         /* from cold: "initialize flash system?" y, "are you sure?" y (the
                                               Spanish unit's yes is s) */
        int yes = strstr(fw, "SPA") || strstr(fw, "spa") ? 0x2C : 0x3D;
        run(3.0);
        key(kind, yes);
        run(3.0);
        key(kind, yes);
        run(10.0);
    } else
        run(8.0);                           /* the greeting is over */
    drain();
    if (report) {
        snprintf(d, sizeof d, "%ld bit/s, %d%c%d, port %s; %d bytes sent", now.baud, now.data_bits, now.parity,
                 now.stop_bits, now.powered ? "on" : "off", n_got);
        check("idle: 9600 8N1, port off", now.baud == 9600 && now.data_bits == 8 && now.parity == 'N'
              && !now.powered && n_got == 0, d);
    }
    from = n_got;
    key(kind, kind == EMU_TYPE_N_SPEAK ? 0x3E : 0x4E);   /* F8; s-chord */
    enq = run_until_sent(0x05, from, 3.0);
    if (report) {
        hexes(hx, sizeof hx, from, enq < 0 ? n_got : enq + 1);
        snprintf(d, sizeof d, "sent [%s]", hx);
        check("storage: XON ENQ out", enq == from + 1 && got[from] == 0x11, d);
        if (enq >= 0) {
            bl_serial_status *s = &got_st[enq];
            snprintf(d, sizeof d, "ENQ left at %ld bit/s, %d%c%d, port %s, RTS %s", s->baud, s->data_bits,
                     s->parity, s->stop_bits, s->powered ? "on" : "off", s->rts ? "on" : "off");
            check("storage: 19200 8N1, port on", s->baud == 19200 && s->data_bits == 8 && s->parity == 'N'
                  && s->powered && s->rts && got_st[from].baud == 19200, d);
        }
    }
    if (enq < 0)
        return 0;
    run(0.02);                              /* the far end's turnaround */
    from = n_got;
    emu_serial_write(u, &reply, 1);
    c = run_until_sent(0x43, from, 0.5);
    if (report && c >= 0) {
        /* pacing: the port is on at 19200 again after the handshake; 64 bytes offered, 20 ms of the unit takes one
           frame each (8N1 is 10 bit times; this core's frame is 11, see bl_serial.h) */
        static const unsigned char junk[64] = {0};
        int before, after, taken;
        run(0.5);
        emu_serial_write(u, junk, 64);
        before = emu_serial_space(u);
        run(0.02);
        after = emu_serial_space(u);
        taken = after - before;
        snprintf(d, sizeof d, "%d bytes taken in 20 ms at %ld bit/s (want %d-%d)", taken, now.baud,
                 (int)(19200 / 12 * 0.02), (int)(19200 / 10 * 0.02 + 1));
        check("input paced at the baud rate", now.baud == 19200 && taken >= (int)(19200 / 12 * 0.02)
              && taken <= (int)(19200 / 10 * 0.02 + 1), d);
        run(0.5);                           /* the rest drains */
        /* the directory command: d, then e (the Type 'n Speak: d, Enter) */
        from = n_got;
        key(kind, kind == EMU_TYPE_N_SPEAK ? 0x34 : 0x19);
        run(3.0);
        key(kind, kind == EMU_TYPE_N_SPEAK ? 0x5B : 0x51);
        run(1.0);
        for (i = from; i + 2 < n_got; i++)
            if (got[i] == 0x05 && got[i + 1] == 'd' && got[i + 2] == 0x0D)
                break;
        hexes(hx, sizeof hx, from, n_got);
        snprintf(d, sizeof d, "sent [%s]", hx);
        check("directory command out", i + 2 < n_got, d);
    }
    emu_destroy(u);
    return c >= 0;
}

int main(int argc, char **argv)
{
    int kind, ack, nak;
    const char *fw, *st;
    char d[200];
    if (argc < 4) {
        printf("usage: test_serial bl|tns FIRMWARE STATE|-\n");
        return 2;
    }
    kind = !strcmp(argv[1], "tns") ? EMU_TYPE_N_SPEAK : EMU_BRAILLE_LITE;
    fw = argv[2];
    st = strcmp(argv[3], "-") ? argv[3] : NULL;
    ack = handshake(kind, fw, st, 0x06, 1);
    snprintf(d, sizeof d, "ACK -> %s", ack ? "'C' back: the firmware received it" : "nothing back");
    check("ACK answered", ack, d);
    if (kind == EMU_BRAILLE_LITE) {         /* the control: the unit answers ACK, not any byte */
        nak = handshake(kind, fw, st, 0x15, 0);
        snprintf(d, sizeof d, "NAK -> %s", nak ? "'C' back" : "nothing back (storage device missing)");
        check("NAK not answered (control)", !nak, d);
    }
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
