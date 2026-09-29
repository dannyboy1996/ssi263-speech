/* mc6850.h -- clean-room MC6850 ACIA model, MIDI subset.
 *
 * Covers what the Robovox patent's MIDI interface (Fig. 1, "mainly an
 * ACIA 6850") needs: master reset, /1-/16-/64 divide select, 8N1 word
 * format, RDRF/TDRE status, overrun + framing-error flags, and the
 * receive-data IRQ into the 6502. Transmit side is modeled (TDRE clears
 * on write, sets on tx_done) for a future MIDI THRU; DCD/CTS modem pins
 * are tied inactive, as on a MIDI input.
 *
 * No MAME code is used here (see ../third_party/README.md for why).
 * Behavior is pinned by tests/test_6850_midi.c.
 */
#ifndef SSINGER_MC6850_H
#define SSINGER_MC6850_H

#include <stdint.h>

/* Status register bits (read) */
#define M6850_SR_RDRF 0x01u  /* receive data ready */
#define M6850_SR_TDRE 0x02u  /* transmit data empty */
#define M6850_SR_DCD  0x04u  /* tied inactive: always 0 */
#define M6850_SR_CTS  0x08u  /* tied inactive: always 0 */
#define M6850_SR_FE   0x10u  /* framing error on last received byte */
#define M6850_SR_OVRN 0x20u  /* overrun: byte lost while RDRF held */
#define M6850_SR_PE   0x40u  /* parity error (unused in 8N1; always 0) */
#define M6850_SR_IRQ  0x80u  /* interrupt request pending */

/* Control register fields (write) */
#define M6850_CR_DIV_MASK 0x03u  /* CR1-0: 0=/1 1=/16 2=/64 3=master reset */
#define M6850_CR_DIV_RESET 0x03u
#define M6850_CR_WORD_MASK 0x1Cu /* CR4-2 word select; 8N1 = 0x14 */
#define M6850_CR_WORD_8N1 0x14u
#define M6850_CR_TX_MASK 0x60u   /* CR6-5 transmit control */
#define M6850_CR_TX_IRQ 0x20u    /* bit5: transmit IRQ enable */
#define M6850_CR_RX_IRQ 0x80u    /* CR7: receive IRQ enable */

typedef struct mc6850 {
    uint8_t ctrl;      /* last control value (0x00 until programmed) */
    uint8_t status;    /* live status bits */
    uint8_t rdr;       /* receive holding register */
    uint8_t tdr;       /* transmit holding register */
    int in_reset;      /* latched until a non-reset control is written */
    int rx_irq_en;     /* decoded CR7 */
    int tx_irq_en;     /* decoded CR5 */
    void (*irq_cb)(int level, void *ctx);  /* edge-triggered on change */
    void *irq_ctx;
    int irq_level;     /* last signaled level */
} mc6850_t;

static void mc6850_raise(mc6850_t *m)
{
    int lvl = (m->status & M6850_SR_IRQ) ? 1 : 0;
    if (lvl != m->irq_level) {
        m->irq_level = lvl;
        if (m->irq_cb)
            m->irq_cb(lvl, m->irq_ctx);
    }
}

static void mc6850_recalc_irq(mc6850_t *m)
{
    uint8_t irq = 0;
    if ((m->status & M6850_SR_RDRF) && m->rx_irq_en)
        irq = M6850_SR_IRQ;
    /* Transmit IRQ fires while TDRE is set (holding register empty). */
    if (m->tx_irq_en && (m->status & M6850_SR_TDRE))
        irq = M6850_SR_IRQ;
    if (irq)
        m->status |= M6850_SR_IRQ;
    else
        m->status &= (uint8_t)~M6850_SR_IRQ;
    mc6850_raise(m);
}

/* Power-on state: TDRE set (transmitter empty), everything else clear,
 * control register unprogrammed, IRQs masked. Real silicon powers up
 * undefined; firmware must issue a master reset first. */
static void mc6850_init(mc6850_t *m)
{
    m->ctrl = 0x00;
    m->status = M6850_SR_TDRE;
    m->rdr = 0x00;
    m->tdr = 0x00;
    m->in_reset = 1;
    m->rx_irq_en = 0;
    m->tx_irq_en = 0;
    m->irq_cb = 0;
    m->irq_ctx = 0;
    m->irq_level = 0;
}

static void mc6850_write_ctrl(mc6850_t *m, uint8_t v)
{
    m->ctrl = v;
    if ((v & M6850_CR_DIV_MASK) == M6850_CR_DIV_RESET) {
        /* Master reset: status keeps TDRE, clears the rest; RDR/TDR
         * holding registers are flushed, IRQs masked until reprogrammed. */
        m->status = M6850_SR_TDRE;
        m->rdr = 0x00;
        m->in_reset = 1;
        m->rx_irq_en = 0;
        m->tx_irq_en = 0;
        mc6850_raise(m);
        return;
    }
    m->in_reset = 0;
    m->rx_irq_en = (v & M6850_CR_RX_IRQ) ? 1 : 0;
    m->tx_irq_en = (v & M6850_CR_TX_IRQ) ? 1 : 0;
    mc6850_recalc_irq(m);
}

static uint8_t mc6850_read_status(const mc6850_t *m) { return m->status; }

/* CPU reads the received byte: clears RDRF (and latched FE/OVRN, per the
 * datasheet: OVRN clears after RDRF data is read). */
static uint8_t mc6850_read_data(mc6850_t *m)
{
    uint8_t v = m->rdr;
    m->status &= (uint8_t)~(M6850_SR_RDRF | M6850_SR_FE | M6850_SR_OVRN);
    mc6850_recalc_irq(m);
    return v;
}

/* CPU writes a byte to transmit: TDRE clears until the (instant, for our
 * purposes) shift completes via mc6850_tx_done. */
static void mc6850_write_data(mc6850_t *m, uint8_t v)
{
    m->tdr = v;
    m->status &= (uint8_t)~M6850_SR_TDRE;
    mc6850_recalc_irq(m);
}

/* Shift register emptied (baud clock domain). Restores TDRE. */
static void mc6850_tx_done(mc6850_t *m)
{
    m->status |= M6850_SR_TDRE;
    mc6850_recalc_irq(m);
}

/* A byte arrives from the MIDI wire (31250 baud 8N1 → 10-bit frames;
 * framing_error = 1 when the stop bit read low). If RDRF is still held
 * from the previous byte, that byte is kept and the new one is lost
 * with OVRN set — the 6850 has no FIFO beyond the holding register. */
static void mc6850_receive_byte(mc6850_t *m, uint8_t v, int framing_error)
{
    if (m->in_reset)
        return;
    if (m->status & M6850_SR_RDRF) {
        m->status |= M6850_SR_OVRN;
        /* RDR keeps the older byte; IRQ stays asserted. */
        mc6850_recalc_irq(m);
        return;
    }
    m->rdr = v;
    m->status |= M6850_SR_RDRF;
    if (framing_error)
        m->status |= M6850_SR_FE;
    mc6850_recalc_irq(m);
}

static int mc6850_irq_level(const mc6850_t *m) { return m->irq_level; }

#endif /* SSINGER_MC6850_H */
