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

#include "../../csrc/blazie/bl_serial.h"

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

/* The unit's serial port (its RS-232 port: WinDisk, PCDISK, a terminal or a screen reader on the far end;
   ../../csrc/blazie/bl_serial.h says what was measured).  Unplugged by default.  emu_serial_attach(u, 1) plugs it
   into the shell's port: emu_serial_write gives the unit the bytes that arrived there (returns how many it took --
   at most emu_serial_space; keep the rest for later), emu_serial_read hands out what the unit sent, one run of bytes
   per status (call until it returns 0; set the port to *status before sending the bytes).  The bytes move as the
   unit runs (emu_render), at the rate the firmware programmed, in the unit's time.  0 from attach: out of memory. */
int emu_serial_attach(emu_unit *u, int on);
int emu_serial_space(const emu_unit *u);
int emu_serial_write(emu_unit *u, const unsigned char *bytes, int n);
int emu_serial_read(emu_unit *u, unsigned char *out, int cap, bl_serial_status *status);

#endif
