/* emu_unit.h -- one emulated Blazie unit running in real time: the firmware, its board, the SSI-263, 16-bit PCM out.
 *
 * Portable (no platform calls): the shell pulls audio with emu_render at the pace of its sound card, and pushes key
 * chords with emu_key from its keyboard.  The two may come from different threads: emu_key only queues, under the
 * shell's own lock (see main_win.c), and the unit takes the chord at its next instruction boundary.
 *
 * Unlike the screen-reader drivers, the unit is NOT put into speech-box mode: it boots to its own main menu, as it
 * does when switched on, and everything it does -- speech, key echo, the channel left open until the firmware clicks
 * it off -- comes from the firmware.
 */
#ifndef BLAZIE_EMU_UNIT_H
#define BLAZIE_EMU_UNIT_H

typedef struct emu_unit emu_unit;

/* firmware: a .BNS update file; state: its saved RAM + file flash (bl2_2003_warm.state).  whine: 0 off, 1 hiss,
   2 whine (the idle channel noise).  NULL on failure, the reason in err. */
emu_unit *emu_create(const char *firmware, const char *state, double out_rate, int whine, char *err, int errlen);
void emu_destroy(emu_unit *u);

/* renders `n` samples of the unit running in real time into out (16-bit mono PCM at out_rate) */
void emu_render(emu_unit *u, short *out, int n);
/* a braille chord (chords.h bits); 0 if the unit's key queue is full */
int emu_key(emu_unit *u, int chord);
void emu_set_whine(emu_unit *u, int whine);
/* 0-100: the output gain (the unit's own volume keys still work on top of it) */
void emu_set_volume(emu_unit *u, int volume);

#endif
