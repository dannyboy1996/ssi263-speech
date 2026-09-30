/* emu_unit.h -- one emulated Blazie unit running in real time: the firmware, its board, the SSI-263, 16-bit PCM out.
 *
 * Two kinds: the Braille Lite 2000 (bl_board, braille chords) and the Type 'n Speak (tns_board, QWERTY key events).
 * Portable (no platform calls): the shell pulls audio with emu_render at the pace of its sound card, and pushes keys
 * with emu_key from its keyboard.  The two may come from different threads: the shell serialises them with its own
 * lock (see main_win.c), and the unit takes each key at an instruction boundary.
 *
 * Unlike the screen-reader drivers, the unit is NOT put into speech-box mode: it boots to its own main menu, as it
 * does when switched on, and everything it does -- speech, key echo, the channel left open until the firmware clicks
 * it off -- comes from the firmware.
 */
#ifndef BLAZIE_EMU_UNIT_H
#define BLAZIE_EMU_UNIT_H

typedef struct emu_unit emu_unit;

enum { EMU_BRAILLE_LITE, EMU_TYPE_N_SPEAK };

/* firmware: a .BNS (Braille Lite) or .TNS (Type 'n Speak) update file; state: its saved memory (for the Braille
   Lite required -- bl2_2003_warm.state; for the Type 'n Speak NULL = a cold start, which asks to initialise the
   flash).  whine: 0 off, 1 hiss, 2 whine (the Braille Lite's idle channel noise).  NULL on failure, the reason in
   err. */
emu_unit *emu_create(int kind, const char *firmware, const char *state, double out_rate, int whine, char *err,
                     int errlen);
void emu_destroy(emu_unit *u);
int emu_kind(const emu_unit *u);

/* renders `n` samples of the unit running in real time into out (16-bit mono PCM at out_rate) */
void emu_render(emu_unit *u, short *out, int n);
/* a key: for the Braille Lite a chord (chords.h bits), for the Type 'n Speak a key event (tns_board.h: bit 7 =
   down); 0 if the unit's key queue is full */
int emu_key(emu_unit *u, int key);
/* saves what the unit keeps while switched off (its files and settings), in the state format emu_create reads;
   1 on success */
int emu_save(const emu_unit *u, const char *path);
void emu_set_whine(emu_unit *u, int whine);
/* 0-100: the output gain (the unit's own volume keys still work on top of it) */
void emu_set_volume(emu_unit *u, int volume);

/* Quick key response (off by default; not the real unit's pace).  After a key the firmware works for a while before
   it speaks: after any chord in the Braille Lite's main menu, ~240 ms of CPU time with the chip's request left
   unanswered, speech at 280 ms; after a Type 'n Speak key, speech at 240 ms (measured in emulated time: 6.144 MHz, the
   clock the unit's own serial divisor and 10 Hz timer give).  On, the CPU runs EMU_QUICK_TURBO times faster from a
   key until the firmware loads its first spoken phoneme (or EMU_QUICK_LIMIT_S of chip time passes): the words come
   sooner, the phonemes themselves play as before. */
#define EMU_QUICK_TURBO 8.0
#define EMU_QUICK_LIMIT_S 1.0
void emu_set_quick(emu_unit *u, int on);
/* chip time in seconds (tests) */
double emu_time(const emu_unit *u);

#endif
