/* so_scu.h -- the V40's on-chip serial unit's receiver as the Speak-Out firmware uses it, reduced exactly as
 * src/hosts/speakout.py reduces it: text arrives as whole bytes, with no baud timing, into a queue; one byte at a
 * time is "loaded" (the received byte the firmware sees).  Port 1, status: bit 0 (TxRDY) always, bit 1 (RxRDY) while
 * a byte is loaded.  Port 0, data: the loaded byte (0 without one), and reading it loads the next queued byte at once.
 * The board loads a byte, when none is, at a slice's start (so_board_offer), and offers IR1 for it.  The firmware's
 * writes to ports 1-3 (the SCU's command 35h, mode 4Eh, mask 02h) change nothing here.  MIT. */
#ifndef SO_SCU_H
#define SO_SCU_H

#include <stdint.h>

typedef struct {
    uint8_t *q;           /* queued bytes, q[head..tail) */
    int head, tail, cap;
    int loaded;           /* the received byte, -1: none */
} so_scu;

void so_scu_init(so_scu *s);
void so_scu_free(so_scu *s);
int so_scu_queue(so_scu *s, const uint8_t *bytes, int n);    /* 0 if out of memory */
void so_scu_drop(so_scu *s);                                 /* the queue and the loaded byte */
int so_scu_pending(const so_scu *s);                         /* a byte queued or loaded */
int so_scu_load(so_scu *s);                                  /* load the next if none is; 1 if one is loaded */
uint8_t so_scu_in(so_scu *s, uint16_t port);                 /* ports 0 and 1 */

#endif
