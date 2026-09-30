/* so_icu.h -- the V40's on-chip interrupt controller as the Speak-Out firmware uses it, reduced exactly as
 * src/hosts/speakout.py reduces it (the approved host): a mask (OCW1), an in-service set and the non-specific EOI, at
 * ports 8-9 (the firmware sets IULA = 08h).  ICW1 (any byte with bit 4 at port 8) makes the next port-9 byte ICW2,
 * the vector base, which is ignored: the vectors are 8 + IR (the firmware's ICW2 is 08h).  No ICW4, no IRR, no
 * priority rotation, no poll: the firmware uses none of them (measured: ICW1 12h, ICW2 08h, OCW1 41h, EOI 20h).
 *
 * An interrupt is OFFERED, as speakout.py's _try_irq does: taken only if the CPU has IE set, the IR is unmasked and
 * nothing of the same or higher priority is in service; otherwise the offer is dropped, not held.  A taken offer
 * asserts the core's INT line, and the acknowledge (the next step's A) puts the IR in service and gives its vector.
 * MIT. */
#ifndef SO_ICU_H
#define SO_ICU_H

#include <stdint.h>

typedef struct {
    uint8_t imr;          /* 1 = masked; FFh at power-on */
    uint8_t isr;          /* in service */
    uint8_t icw_step;     /* 1: the next port-9 write is ICW2 */
    int offered;          /* the IR whose offer awaits its acknowledge, -1: none */
} so_icu;

void so_icu_reset(so_icu *c);
uint8_t so_icu_in(so_icu *c, uint16_t port);                 /* port 9: the mask; port 8: 0 (as the host) */
void so_icu_out(so_icu *c, uint16_t port, uint8_t value);
/* 1 if the offer is taken (the caller asserts INT); ie: the CPU's IE flag now */
int so_icu_offer(so_icu *c, int ir, int ie);
int so_icu_ack(so_icu *c);                                   /* the vector number; -1 without an offer */

#endif
