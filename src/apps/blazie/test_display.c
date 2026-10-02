/* Electrical display contract: shift order, wiring, padding, latch isolation and malformed transactions. */
#include <assert.h>
#include <stdio.h>
#include "../../csrc/blazie/bl_display.h"

static void pin(bld_display *d, unsigned char *port, int bit, int high)
{
    unsigned char before = *port;
    if (high) *port |= (unsigned char)(1 << bit);
    else *port &= (unsigned char)~(1 << bit);
    bld_port(d, before, *port, 0);
}

static void byte(bld_display *d, unsigned char *port, unsigned char value)
{
    int i;
    pin(d, port, 2, 0);
    for (i = 0; i < 8; i++) {
        pin(d, port, 0, (value >> i) & 1);
        pin(d, port, 1, 0);
        pin(d, port, 1, 1);
        pin(d, port, 1, 1);  /* Holding clock high is not an extra bit. */
        pin(d, port, 4, i & 1);  /* The clock controller's select must not affect the display. */
    }
}

static void latch(bld_display *d, unsigned char *port)
{
    pin(d, port, 2, 1);
    pin(d, port, 2, 0);
}

int main(void)
{
    bld_display d = {0}, other = {0};
    unsigned char port = 0, saved[40];
    /* Physical wire bit 0..7 corresponds to logical dots 7,3,2,1,8,6,5,4. */
    const unsigned char expected[8] = {64, 4, 2, 1, 128, 32, 16, 8};
    int i;
    for (i = 0; i < 8; i++) assert(bld_dots((unsigned char)(1 << i)) == expected[i]);
    assert(bld_dots(0xff) == 0xff && bld_dots(0) == 0);
    /* 18 actual cells, plus 2 unused registers per group of six. Padding deliberately nonzero. */
    for (i = 18; i > 0; i--) {
        if (i == 12 || i == 6) { byte(&d, &port, 0xff); byte(&d, &port, 0xff); }
        byte(&d, &port, (unsigned char)(1 << ((i - 1) % 8)));
    }
    byte(&d, &port, 0xff); byte(&d, &port, 0xff);
    assert(d.count == 0);  /* Nothing visible until the strobe. */
    latch(&d, &port);
    assert(d.count == 18);
    for (i = 0; i < 18; i++) assert(d.cells[i] == expected[i % 8]);
    for (i = 18; i < 40; i++) assert(d.cells[i] == 0);
    assert(other.count == 0);
    memcpy(saved, d.cells, sizeof saved);
    byte(&d, &port, 0); latch(&d, &port);  /* Incomplete frame is not a clear. */
    assert(d.count == 18 && !memcmp(saved, d.cells, sizeof saved));
    for (i = 0; i < 41; i++) byte(&d, &port, 0);
    latch(&d, &port);  /* Too many bits must not be mistaken for a 40-cell frame. */
    assert(d.count == 18 && !memcmp(saved, d.cells, sizeof saved));
    byte(&d, &port, 0xff);
    bld_port(&d, port, 0, 1); port = 0;  /* PPI mode reset discards the partial transaction. */
    assert(d.bits == 0 && !memcmp(saved, d.cells, sizeof saved));
    for (i = 40; i > 0; i--) byte(&d, &port, (unsigned char)(1 << ((i - 1) % 8)));
    latch(&d, &port);
    assert(d.count == 40);
    for (i = 0; i < 40; i++) assert(d.cells[i] == expected[i % 8]);
    for (i = 0; i < 40; i++) byte(&d, &port, 0);
    latch(&d, &port);
    for (i = 0; i < 40; i++) assert(d.cells[i] == 0);
    puts("display: dot wiring, 18/40 cells, padding, clock edges, latch and reset passed");
    return 0;
}
