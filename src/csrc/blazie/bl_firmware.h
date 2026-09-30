/* bl_firmware.h -- the Braille Lite firmware a user brings, found by its content and written the way bl_create reads
 * it: a .BNS, the ROM image from file offset 3000h.
 *
 * The image starts F3 C3 xx xx FF "COPYRIGHT" (di; jp ...; then Blazie's notice).  A .BNS update file carries it at
 * 3000h (the 1998 ones at 4000h or 5000h); an update program may carry it anywhere, so the whole file is scanned.
 * Blazie's other units (Braille 'n Speak, Type 'n Speak) carry the same notice: bl_create's firmware sites decide
 * whether the voice can run an image.  The two releases the voice ships for are known by the sha256 of their image;
 * only hashes are here, never bytes.
 */
#ifndef BL_FIRMWARE_H
#define BL_FIRMWARE_H

#include "bl_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLV_FW_ENGLISH 0           /* the June 2003 English release (BL2ENG.BNS) */
#define BLV_FW_SPANISH 1           /* ONCE's Spanish release (BL2SPA.BNS) */
#define BLV_FW_OTHER 2             /* an image the voice's sites were found in, of no release known here */
#define BLV_FW_NONE (-1)           /* no Braille Lite firmware in these bytes */
#define BLV_FW_REFUSED (-2)        /* an image, but not one the voice can run (the sites are missing) */
#define BLV_FW_WRITE (-3)          /* out_bns could not be written */

/* The offset of the first ROM image in data, or -1. */
BL_API long blv_find_image(const unsigned char *data, long n);

/* Finds the image in data (a .BNS, an update program, any file), writes it to out_bns as a .BNS -- a .BNS with the
   image at 3000h exactly as it came, anything else behind 3000h zero bytes -- and checks that the voice can run it.
   Returns BLV_FW_ENGLISH, _SPANISH or _OTHER; or BLV_FW_NONE, _REFUSED, _WRITE with the reason in err and out_bns
   removed. */
BL_API int blv_import_firmware(const unsigned char *data, long n, const char *out_bns, char *err, int errlen);

/* BLV_FW_ENGLISH or _SPANISH when data is the state the voice ships with for that release (bl2_2003_warm.state,
   bl2spa_fresh.state: blv_make_state's result), else -1. */
BL_API int blv_state_language(const unsigned char *data, long n);

/* sha256 of n bytes into out[32]; exported for the tests. */
BL_API void blv_sha256(const unsigned char *data, long n, unsigned char *out);

#ifdef __cplusplus
}
#endif
#endif
