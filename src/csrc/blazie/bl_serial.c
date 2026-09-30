/* bl_serial.c -- see bl_serial.h.  One producer and one consumer per direction, both on the caller's side of any
 * lock: the emulator's shell serialises every call on a unit (emu_unit.h), so there is no locking here.
 *
 * Built with -DBL_SERIAL_CUT_RX, the receive path is cut (the unit never sees a byte): the must-fail control of the
 * emulator's serial test (src/apps/blazie/test_serial.c) proves the test sees the firmware answer what it received.
 */
#include <stdlib.h>
#include <string.h>
#include "bl_serial.h"

#define RX_RING 128                            /* a power of two above BL_SERIAL_ROOM */

typedef struct {
    int pos;                                   /* the first byte sent under it */
    bl_serial_status st;
} mark;

struct bl_serial_line {
    unsigned char rx[RX_RING];
    unsigned rx_head, rx_tail;
    unsigned char *tx;                         /* the unit's bytes not yet taken, from tx_read */
    int n_tx, cap_tx, tx_read;
    mark *marks;                               /* status changes, in order; marks[0].pos <= tx_read */
    int n_marks, cap_marks;
    bl_serial_status now;
    int have_now;
};

void bl_serial_decode(const z180_asci_regs *r, double clock_hz, int powered, bl_serial_status *s)
{
    unsigned long brg, div;
    memset(s, 0, sizeof *s);
    /* bit time in T-states = the sampling divide (16, 64; 1 with ASEXT X1) x the prescale (10 or 30, PS) x 2^SS, or
       x (ASTC + 2) with ASEXT BRGM, as ../cpu/z180_asci.hpp models it */
    div = (r->asext & 0x10) ? 1 : ((r->cntlb & 0x08) ? 64 : 16);
    if ((r->cntlb & 0x07) == 0x07)
        brg = 0;                               /* an external clock (CKA0): none on these boards */
    else if (r->asext & 0x08)
        brg = (unsigned long)r->astc + 2;
    else
        brg = (1UL << (r->cntlb & 0x07)) * ((r->cntlb & 0x20) ? 30 : 10);
    s->baud = brg ? (long)(clock_hz / (double)(div * brg) + 0.5) : 0;
    s->data_bits = (r->cntla & 0x04) ? 8 : 7;
    s->parity = (r->cntla & 0x02) ? ((r->cntlb & 0x10) ? 'O' : 'E') : 'N';
    s->stop_bits = 1;
    s->powered = powered ? 1 : 0;
    s->rts = (r->cntla & 0x10) ? 0 : 1;
    s->rx_on = (r->cntla & 0x40) ? 1 : 0;
    s->tx_on = (r->cntla & 0x20) ? 1 : 0;
}

int bl_serial_same(const bl_serial_status *a, const bl_serial_status *b)
{
    return a->baud == b->baud && a->data_bits == b->data_bits && a->parity == b->parity
           && a->stop_bits == b->stop_bits && a->powered == b->powered && a->rts == b->rts
           && a->rx_on == b->rx_on && a->tx_on == b->tx_on;
}

bl_serial_line *bl_serial_new(void)
{
    return (bl_serial_line *)calloc(1, sizeof(bl_serial_line));
}

void bl_serial_free(bl_serial_line *l)
{
    if (!l)
        return;
    free(l->tx);
    free(l->marks);
    free(l);
}

void bl_serial_note(bl_serial_line *l, const bl_serial_status *now)
{
    if (l->have_now && bl_serial_same(&l->now, now))
        return;
    l->now = *now;
    l->have_now = 1;
    if (l->n_marks && l->marks[l->n_marks - 1].pos == l->n_tx) {
        l->marks[l->n_marks - 1].st = *now;    /* no byte left under the last one: it is replaced */
        return;
    }
    if (l->n_marks == l->cap_marks) {
        int cap = l->cap_marks ? l->cap_marks * 2 : 16;
        mark *m = (mark *)realloc(l->marks, (size_t)cap * sizeof(mark));
        if (!m)
            return;
        l->marks = m;
        l->cap_marks = cap;
    }
    l->marks[l->n_marks].pos = l->n_tx;
    l->marks[l->n_marks].st = *now;
    l->n_marks++;
}

void bl_serial_sent(bl_serial_line *l, unsigned char byte)
{
    if (l->n_tx == l->cap_tx) {
        int cap = l->cap_tx ? l->cap_tx * 2 : 256;
        unsigned char *t = (unsigned char *)realloc(l->tx, (size_t)cap);
        if (!t)
            return;
        l->tx = t;
        l->cap_tx = cap;
    }
    l->tx[l->n_tx++] = byte;
}

int bl_serial_next(bl_serial_line *l)
{
#ifdef BL_SERIAL_CUT_RX
    l->rx_tail = l->rx_head;                   /* the control: the line is cut */
    return -1;
#else
    if (l->rx_head == l->rx_tail)
        return -1;
    return l->rx[l->rx_tail++ & (RX_RING - 1)];
#endif
}

int bl_serial_room(const bl_serial_line *l)
{
    return BL_SERIAL_ROOM - (int)(l->rx_head - l->rx_tail);
}

int bl_serial_put(bl_serial_line *l, const unsigned char *bytes, int n)
{
    int room = bl_serial_room(l), i;
    if (n > room)
        n = room;
    for (i = 0; i < n; i++)
        l->rx[l->rx_head++ & (RX_RING - 1)] = bytes[i];
    return n < 0 ? 0 : n;
}

int bl_serial_take(bl_serial_line *l, unsigned char *out, int cap, bl_serial_status *status)
{
    int m = 0, end, n;
    while (m + 1 < l->n_marks && l->marks[m + 1].pos <= l->tx_read)
        m++;                                   /* the status the next byte left under */
    if (l->tx_read == l->n_tx) {               /* nothing waiting: the status now, and a fresh buffer */
        if (status)
            *status = l->now;
        l->n_tx = l->tx_read = 0;
        if (l->n_marks) {
            l->marks[0].pos = 0;
            l->marks[0].st = l->now;
            l->n_marks = 1;
        }
        return 0;
    }
    end = m + 1 < l->n_marks ? l->marks[m + 1].pos : l->n_tx;
    n = end - l->tx_read;
    if (n > cap)
        n = cap;
    memcpy(out, l->tx + l->tx_read, (size_t)n);
    l->tx_read += n;
    if (status)
        *status = l->n_marks ? l->marks[m].st : l->now;   /* (the boards note before every byte: always a mark) */
    return n;
}
