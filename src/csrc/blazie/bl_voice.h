/* bl_voice.h -- the Braille Lite as a voice: nvda/blazie/synthDrivers/blazie.py's front end in C, line for line
 * (its boot, _clean, _lines, the unit's rate/pitch/tone commands, the speak loop with the lead trim and PCM, and
 * cancel), around the C host (bl_host.h) and a chip made from the built-in defaults (ssi263_default_params).
 *
 * For front ends without Python: the Linux speech-dispatcher module, Android.  nvda/tools/voice_equiv.py gates it:
 * the real NVDA driver's PCM and this one's, byte for byte, on the same text and settings.
 *
 * Not yet ported (v1): the driver's number words (ssi263_numwords.py); the firmware reads numbers itself up to
 * 999,999,999,999.  Pitch commands inside an utterance (capitals) are not part of this API.
 */
#ifndef BL_VOICE_H
#define BL_VOICE_H

#include "bl_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bl_voice bl_voice;

#define BLV_LATIN1 0               /* the English unit (BL2ENG.BNS): 7-bit text */
#define BLV_CP850 1                /* the Spanish unit (BL2SPA.BNS): DOS code page 850 */

/* Boots the unit as the driver does (punctuation none, full numbers, inflection on or off, volume 6).
   whine: 0 off, 1 hiss, 2 whine.  NULL on failure, the reason in err. */
BL_API bl_voice *blv_create(const char *firmware, const char *state, int encoding, double out_rate, int inflection,
                            int whine, char *err, int errlen);
BL_API void blv_destroy(bl_voice *v);

/* NVDA's scales: rate, pitch and volume 0-100 (50, 50, 100 = the unit's factory rate 11 and pitch 16, full volume);
   tone 0-26 (7 = factory); pack = the driver's "short pauses" (sentences packed onto one line from the second on). */
BL_API void blv_set(bl_voice *v, int rate, int pitch, int tone, int volume, int pack);

/* Starts one utterance (UTF-8).  Returns the number of lines sent to the unit (0: nothing to say), or -1 when the
   host refused it (a run-ahead fault); the next blv_speak recovers with a cancel first. */
BL_API int blv_speak(bl_voice *v, const char *utf8);
/* The next block of the utterance: 16-bit mono PCM at out_rate in an internal buffer valid until the next call
   (possibly 0 samples while the unit is still silent).  *done = 1 once the unit has finished, or on a host fault
   (blv_fault says which). */
BL_API int blv_render(bl_voice *v, const short **pcm, int *done);
/* Stops the utterance: the unit drops what it has not spoken (its audio is discarded); the next blv_speak starts
   clean. */
BL_API void blv_cancel(bl_voice *v);
/* Nonzero after a host fault ended or refused the last utterance (cleared by the next blv_speak's recovery). */
BL_API int blv_fault(const bl_voice *v);
/* A test's control (blv_fault_test.py sets it; nothing else does): nonzero puts back the 0.7 draft's handling --
   a refused say taken as said, a fault taken as busy -- which must fail that test. */
BL_API extern int blv_break_fault;

/* The bytes blv_speak would send the unit for this text (currencies, clean-up, lines, encoding), for tests: returns
   the length, and copies them into out when they fit in cap. */
BL_API int blv_say_bytes(const char *utf8, int encoding, int pack, unsigned char *out, int cap);

BL_API bl_host *blv_host(bl_voice *v);
BL_API ssi263 *blv_chip(bl_voice *v);

#ifdef __cplusplus
}
#endif
#endif
