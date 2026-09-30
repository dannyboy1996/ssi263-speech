/* bl_idle.h -- the Braille Lite's idle channel as Tomi's unit sounds, for the Blazie emulator.
 *
 * Measured on the W03 grid (tools/idle_sounds.py; the numbers and what they mean: src/hosts/blazie_idle.py):
 *   the open channel's noise and comb lines at their absolute level -- the same at every volume, as on the unit;
 *   the pop when the firmware opens the channel (port A0 bit 1 on, ~0.26 s before the first phoneme);
 *   the click when the firmware clicks it off (A0 bit 1 off with R3 = 00, ~10 s after the last phoneme);
 *   the tick of the firmware's 10 Hz timer while the channel is open.
 * bl_host.c feeds it the unit's chip writes and the channel's power, each at its sample in the block, and adds its
 * sound after the board pole (bh_set_idle).  Off unless a front end asks: the screen-reader drivers keep bh_set_whine's
 * hiss/whine and never hear the pop, the click or the tick.  An empirical model of the recorded output.
 */
#ifndef BL_IDLE_H
#define BL_IDLE_H

enum { BLI_SOUND_NONE, BLI_SOUND_HISS, BLI_SOUND_WHINE, BLI_SOUND_BY_VOLUME };
enum { BLI_OPEN_OFF, BLI_OPEN_UNTIL_CLICK, BLI_OPEN_ALWAYS };

typedef struct {
    int sound;       /* BLI_SOUND_*: the channel's noise and lines as the hiss, the whine, or as the unit chooses them
                        (the hiss at even volumes, the whine at odd); NONE: neither */
    int keep_open;   /* BLI_OPEN_*: when the channel is heard -- only under speech (and 0.3 s after); until the firmware
                        clicks it off (the unit); always */
    int pop_click;   /* the pop when the channel opens and the click when it is clicked off (BLI_OPEN_UNTIL_CLICK only:
                        the other two never let the channel be heard opening or closing) */
    int tick;        /* the 10 Hz tick, wherever the channel is heard */
} bl_idle_options;

typedef struct bl_idle bl_idle;

/* rate: the output rate; power, r3, r4: the channel as the unit has it now.  NULL if out of memory. */
bl_idle *bl_idle_new(double rate, const bl_idle_options *o, int power, int r3, int r4);
void bl_idle_free(bl_idle *s);
void bl_idle_set(bl_idle *s, const bl_idle_options *o);
void bl_idle_begin(bl_idle *s);                              /* a new block: what came since the last one is at its start */
void bl_idle_write(bl_idle *s, int pos, int reg, int val);   /* a chip write, landing at sample pos of the block */
void bl_idle_power(bl_idle *s, int pos, int on);             /* the channel's power (port A0 bit 1) at sample pos */
void bl_idle_render(bl_idle *s, double *y, int n);           /* adds the block's idle sound to y */

#endif
