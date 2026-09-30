/* so_icu.c -- the V40's interrupt controller, reduced as the approved host reduces it (so_icu.h).  MIT. */
#include "so_icu.h"

void so_icu_reset(so_icu *c)
{
    c->imr = 0xFF;
    c->isr = 0;
    c->icw_step = 0;
    c->offered = -1;
}

uint8_t so_icu_in(so_icu *c, uint16_t port)
{
    return port == 9 ? c->imr : 0;
}

void so_icu_out(so_icu *c, uint16_t port, uint8_t value)
{
    if (port == 8) {
        if (value & 0x10)                          /* ICW1 */
            c->icw_step = 1;
        else if (value == 0x20 && c->isr)          /* non-specific EOI: the highest priority (lowest bit) */
            c->isr &= (uint8_t)(c->isr - 1);
    } else if (port == 9) {
        if (c->icw_step == 1)                      /* ICW2 (the vector base); single, no ICW4 */
            c->icw_step = 0;
        else                                       /* OCW1 */
            c->imr = value;
    }
}

int so_icu_offer(so_icu *c, int ir, int ie)
{
    if (c->offered >= 0)                           /* one offer at a time: the last one not yet acknowledged */
        return 0;
    if (!ie || (c->imr >> ir & 1) || (c->isr & ((2 << ir) - 1)))
        return 0;
    c->offered = ir;
    return 1;
}

int so_icu_ack(so_icu *c)
{
    int ir = c->offered;
    if (ir < 0)
        return -1;
    c->offered = -1;
    c->isr |= (uint8_t)(1 << ir);
    return 8 + ir;
}
