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
#include "../../csrc/blazie/bl_clock.h"

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
/* The Braille Lite's keys physically down now (chords.h bits; 0 when none), as they go down and come up, beside
   emu_key's chord when the last one comes up: the unit reads keys held while it starts (power-on, or its own restart
   after p-chord l): i-chord held is the cold reset, and so on (bl_board.h bl_keys_down).  No effect on the Type 'n
   Speak, whose keys go down and up through emu_key. */
void emu_keys_down(emu_unit *u, int bits);
/* saves what the unit keeps while switched off (its files and settings, its clock), in the state format emu_create
   reads; 1 on success */
int emu_save(emu_unit *u, const char *path);
/* The unit's clock controller (../../csrc/blazie/bl_clock.h): emu_create switches it on, going on from the clock
   saved in the state (plus the time the unit was switched off: the controller keeps time on its battery) or, with
   none saved, starting at the host's local time -- the year as the unit can hold it (1989-2020: a later year becomes
   the latest one with the same calendar).  The time then passes in the unit's own time.  emu_clock_time: the
   clock's (alarm 0) or the alarm's (1) fields. */
int emu_clock_time(const emu_unit *u, int alarm, blc_time *t);
/* tests: the unit's RAM as its 1 MB address space (the Braille Lite's 00000-3FFFF unused: the ROM); its size */
int emu_memory(const emu_unit *u, const unsigned char **ram);
/* tests: the host's time emu_create and emu_save use, in seconds since 1970 read as local time (-1: the real one) */
void emu_fake_host_time(long long seconds);
void emu_set_whine(emu_unit *u, int whine);
/* The Braille Lite's idle channel as the unit sounds (../../csrc/blazie/bl_idle.h), in place of emu_set_whine's:
   sound 0 none, 1 hiss, 2 whine, 3 as the unit (the hiss at even volumes, the whine at odd), at the measured level,
   the same at every volume; keep_open 0 only under speech, 1 until the unit clicks off, 2 always; pop_click: the pop
   when the channel opens and the click when it is clicked off; tick: the 10 Hz tick while the channel is heard.
   0 if out of memory.  No effect on the Type 'n Speak. */
int emu_set_idle(emu_unit *u, int sound, int keep_open, int pop_click, int tick);
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
