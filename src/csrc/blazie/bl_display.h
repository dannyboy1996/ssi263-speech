/* Braille Lite shift-register display. Header-only so all existing board builds share it.
 * PPI C0=data, C1=clock (rising edge), C2=latch (rising edge). Bytes arrive LSB first,
 * rightmost cell first. The 18-cell board has two unused registers per six-cell group.
 */
#ifndef BL_DISPLAY_H
#define BL_DISPLAY_H

#include <string.h>

#define BLD_MAX_CELLS 40
typedef struct {
    unsigned char wire[BLD_MAX_CELLS], cells[BLD_MAX_CELLS];
    int bits, count;
} bld_display;

static unsigned char bld_dots(unsigned char wire)
{
    static const unsigned char dots[8] = {0x40, 0x04, 0x02, 0x01, 0x80, 0x20, 0x10, 0x08};
    unsigned char out = 0;
    int i;
    for (i = 0; i < 8; i++)
        if (wire & (1 << i)) out |= dots[i];
    return out;
}

static void bld_port(bld_display *d, unsigned char before, unsigned char after, int mode_reset)
{
    int n, i;
    if (mode_reset) {
        d->bits = 0;  /* Reset the serial transaction, without inventing a display latch. */
        return;
    }
    if (!(before & 2) && (after & 2)) {
        if (d->bits < BLD_MAX_CELLS * 8) {
            int byte = d->bits / 8, bit = d->bits % 8;
            if (!bit) d->wire[byte] = 0;
            if (after & 1) d->wire[byte] |= (unsigned char)(1 << bit);
        }
        if (d->bits <= BLD_MAX_CELLS * 8) d->bits++;  /* Saturate invalid/overlong frames. */
    }
    if (!(before & 4) && (after & 4)) {
        n = d->bits / 8;
        if (d->bits == 24 * 8 || d->bits == 40 * 8) {
            d->count = n == 24 ? 18 : 40;
            for (i = 0; i < d->count; i++) {
                int position = n == 24 ? 2 + (i / 6) * 8 + i % 6 : i;
                d->cells[i] = bld_dots(d->wire[n - 1 - position]);
            }
            memset(d->cells + d->count, 0, BLD_MAX_CELLS - d->count);
        }
        d->bits = 0;
    }
}

#endif
