/* bl_host.h -- the Braille Lite host in C: src/hosts/blazie.py's lockstep, cancel and ^F bookkeeping, line for line,
 * around the library board (bl_board.h) and an SSI-263 made by ssi263.dll (the caller owns it).
 *
 * The unit's writes are applied to the chip at the chip's current time after each CPU run; the chip's A/R request
 * goes back before the next.  nvda/tools/golden/blazie_*.txt gates it through src/hosts/native_blazie.py.
 */
#ifndef BL_HOST_H
#define BL_HOST_H

#include "../ssi263.h"

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

BL_API void bh_send(bl_host *h, const unsigned char *data, int n);   /* bytes for the unit, counting ^F */
BL_API void bh_say(bl_host *h, const unsigned char *data, int n);    /* the lines and flush, as say() builds them */
BL_API int bh_owed(const bl_host *h);
BL_API int bh_busy(const bl_host *h, double quiet, double patience);
BL_API double bh_cancel(bl_host *h, double limit, double quiet, double cut);   /* quiet, cut < 0: the defaults */
BL_API double bh_skip(bl_host *h, double seconds);
/* Runs `seconds` of chip time in `step` lockstep (0.0005 s): the audio after the board pole and the whine, in an
   internal buffer valid until the next call. */
BL_API int bh_run(bl_host *h, double seconds, double step, const double **audio);

/* whine: 0 off, 1 hiss, 2 whine */
BL_API void bh_set_whine(bl_host *h, int mode);
BL_API int bh_get_whine(const bl_host *h);

/* the host's state, as blazie.py keeps it (tests and the Python wrapper read and some set these) */
BL_API int bh_get_int(const bl_host *h, const char *name);
BL_API void bh_set_int(bl_host *h, const char *name, int v);
BL_API double bh_get_double(const bl_host *h, const char *name);
BL_API void bh_set_double(bl_host *h, const char *name, double v);
BL_API int bh_tx(const bl_host *h, const unsigned char **bytes);   /* every byte the unit sent back */
BL_API int bh_key(bl_host *h, int chord);   /* a braille chord pressed live (bl_key) */
BL_API int bh_save_state(const bl_host *h, const char *path);   /* bl_save_state: 1 on success */

#ifdef __cplusplus
}
#endif
#endif
