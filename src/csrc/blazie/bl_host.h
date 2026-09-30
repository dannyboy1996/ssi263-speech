/* bl_host.h -- the Braille Lite host in C: src/hosts/blazie.py's lockstep, cancel and ^F bookkeeping, line for line,
 * around the library board (bl_board.h) and an SSI-263 made by ssi263.dll (the caller owns it).
 *
 * The unit's writes are applied to the chip at the chip's current time after each CPU run; the chip's A/R request
 * goes back before the next.  nvda/tools/golden/blazie_*.txt gates it through src/hosts/native_blazie.py.
 */
#ifndef BL_HOST_H
#define BL_HOST_H

#include "../ssi263.h"
#include "bl_serial.h"
#include "bl_idle.h"
#include "bl_board.h"

#if defined(_WIN32)
#define BL_API __declspec(dllexport)
#else
#define BL_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bl_host bl_host;

typedef struct {
    double t;                      /* the chip time the write was applied at */
    int reg, val;
} bh_write;

/* Boots the unit (keys at those instruction counts, then live mode at boot_instr).  board_hz <= 0: no board
   low-pass.  log_writes: keep every SSI-263 write (bh_writes) for tests -- drain it, or it grows.  NULL on failure,
   the reason in err. */
BL_API bl_host *bh_create(const char *firmware, const char *state, ssi263 *chip, double out_rate, double board_hz,
                          const unsigned long long *key_at, const unsigned char *key_val, int n_keys,
                          unsigned long long boot_instr, int log_writes, char *err, int errlen);
BL_API int bh_writes(const bl_host *h, const bh_write **writes);
BL_API void bh_clear_writes(bl_host *h);
BL_API void bh_destroy(bl_host *h);

/* bytes for the unit, counting ^F; and a say: the lines and flush, as say() builds them.  While a run-ahead utterance
   plays (below) both are held and delivered in order when it ends.  1, or 0 when out of memory (nothing taken). */
BL_API int bh_send(bl_host *h, const unsigned char *data, int n);
BL_API int bh_say(bl_host *h, const unsigned char *data, int n);
BL_API int bh_owed(const bl_host *h);
/* 1 while the unit is still speaking what it was given, 0 when done; -1 when a run-ahead utterance failed (out of
   memory: its script is incomplete; bh_cancel) */
BL_API int bh_busy(const bl_host *h, double quiet, double patience);
BL_API double bh_cancel(bl_host *h, double limit, double quiet, double cut);   /* quiet, cut < 0: the defaults */
BL_API double bh_skip(bl_host *h, double seconds);
/* Runs `seconds` of chip time in `step` lockstep (0.0005 s): the audio after the board pole and the whine, in an
   internal buffer valid until the next call. */
BL_API int bh_run(bl_host *h, double seconds, double step, const double **audio);

/* whine: 0 off, 1 hiss, 2 whine */
BL_API void bh_set_whine(bl_host *h, int mode);
BL_API int bh_get_whine(const bl_host *h);

/* The emulator's idle channel (bl_idle.h): the measured noise and lines at their absolute level, the pop when the
   firmware opens the channel, the click when it clicks it off, the 10 Hz tick, and when the channel is heard.  While
   set it replaces bh_set_whine's hiss/whine; NULL turns it off again (the default: the screen-reader drivers never set
   it).  0 if out of memory. */
BL_API int bh_set_idle(bl_host *h, const bl_idle_options *o);

/* the host's state, as blazie.py keeps it (tests and the Python wrapper read and some set these).
   "run_ahead" (int, 0 = off, the default; EXPERIMENTAL, opt-in): from the next bh_say, each utterance is captured
   with the unit run ahead of the chip -- a bounded streaming capture, up to RA_AHEAD segments ahead -- and played from
   the script (run_ahead.h).  Its end is inferred from bh_owed's ^F echo accounting and a quiet interval: a policy,
   not a proof.  bh_busy follows run_ahead.h's completion: busy until the capture has ended by that policy, every
   write has played and the final load has ended; a limit hands over to the lockstep's own judgement; an allocation
   failure is -1.  Input given meanwhile is held (bh_say).  The capture's other effects -- the unit's serial bytes,
   ^F echoes, XON/XOFF, its RAM -- happen at the capture's frontier, ahead of the listener: a cancel cannot take them
   back (nvda/tools/run_ahead_state.py compares them).  bh_cancel over a running capture first lets the unit, parked
   mid-routine at the frontier, run on to a wait for an interrupt (run_ahead.h ra_settle), and holds its A/R not
   requesting over the first ^X slice: before that, cancelled text could lead the next utterance
   (nvda/tools/run_ahead_cancel.py).  Timing is emulated Z180 time, not a chip-bus measurement.
   The pipe host has no such mode.  Read-only: "run_ahead_state" (RA_*), "run_ahead_end" (RA_END_*),
   "run_ahead_played" / "run_ahead_captured" (writes), "run_ahead_settled" / "run_ahead_dropped" (the last cancel's
   ra_settle), "held" (inputs waiting), "port_a0".  Tests only:
   "run_ahead_break" (RA_BRK_*), "log_ar" (the A/R edges given to the unit, reg 8, and the run-ahead segments'
   openings, reg 9 + how, in the write log). */
BL_API int bh_get_int(const bl_host *h, const char *name);
BL_API void bh_set_int(bl_host *h, const char *name, int v);
/* tests: the current (or last) run-ahead script, as run_ahead.h's ra_write array; its length */
BL_API int bh_script(const bl_host *h, const void **writes);
/* tests, lane 1: the next run-ahead utterance's writes at these chip times (copied); 0 when out of memory */
BL_API int bh_pace(bl_host *h, const double *t, int n);
/* tests: the unit's state (bl_board.h's bl_probe, bl_memory) for comparing two units at a checkpoint */
BL_API void bh_probe(const bl_host *h, bl_probe *p);
BL_API int bh_memory(const bl_host *h, int which, const unsigned char **bytes);
BL_API double bh_get_double(const bl_host *h, const char *name);
BL_API void bh_set_double(bl_host *h, const char *name, double v);
BL_API int bh_tx(const bl_host *h, const unsigned char **bytes);   /* every byte the unit sent back */
BL_API void bh_clear_tx(bl_host *h);        /* forget them (a program that never reads them: the emulator app) */
/* the serial port carried to a real port (bl_board.h's bl_serial_*); the drivers never attach it */
BL_API int bh_serial_attach(bl_host *h, int on);
BL_API int bh_serial_write(bl_host *h, const unsigned char *bytes, int n);
BL_API int bh_serial_space(const bl_host *h);
BL_API int bh_serial_read(bl_host *h, unsigned char *out, int cap, bl_serial_status *status);
BL_API int bh_key(bl_host *h, int chord);   /* a braille chord pressed live (bl_key) */
BL_API void bh_battery(bl_host *h, int level);   /* the battery gauge's reading (bl_battery) */
BL_API int bh_save_state(const bl_host *h, const char *path);   /* bl_save_state: 1 on success */

#ifdef __cplusplus
}
#endif
#endif
