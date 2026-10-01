/* as_voice.h -- the Accent SA as a voice: nvda/accent/synthDrivers/accentmini.py's front end for its "sa" voice, in C
 * (its boot, its settings commands, currencies, _clean and _numbers, a capital's pitch with snap_pitch, the speak loop
 * with the lead trim and PCM, and cancel), around the C host (as_host.h) and a chip made from the built-in defaults
 * (ssi263_default_params) -- as bl_voice.h is the Braille Lite's.  For front ends without Python: Android first.
 *
 * src/platforms/android/test/test_android_native.py holds it to the NVDA driver itself (under the stand-in NVDA, on the
 * desktop's C host), byte for byte, on the texts and settings of its Accent SA cases.
 *
 * The ROMs come from the caller (as ash_create): Aicom's u2/u3/u4, never built in.  Not thread-safe per voice.  MIT.
 */
#ifndef AS_VOICE_H
#define AS_VOICE_H

#include <stddef.h>
#include "as_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct as_voice as_voice;

/* Creates the unit from the ROMs and boots it as the driver does (ash_boot: the greeting flushed; its settings are
   then the Accent's power-up ones: rate 5, pitch 5, voice 5, full intonation).  NULL on failure, the reason in err. */
AS_API as_voice *asv_create(const unsigned char *u2, size_t n2, const unsigned char *u3, size_t n3,
                            const unsigned char *u4, size_t n4, double out_rate, char *err, int errlen);
AS_API void asv_destroy(as_voice *v);

/* The driver's settings, on NVDA's scales: rate and pitch 0-100 (50 = the Accent's 5), inflection 0-100 (100 = full
   intonation, ESC M0; 0 = monotone, M1), volume in percent (100 = the driver's full volume, the gain 1; above it is
   louder, clipped at full scale), numbers = the driver's custom number processing.  Sent before the next text, only
   what changed (the driver's _sent). */
AS_API void asv_set(as_voice *v, int rate, int pitch, int inflection, int volume, int numbers);

/* Starts one utterance (UTF-8).  pitch_offset != 0 is NVDA's PitchCommand(offset) before the text -- a capital: the
   pitch jumps (snap_pitch) to the settings' pitch plus the offset for this text, and is restored (snapped) once its
   audio is out.  Returns 1 when text was sent, 0 when there is nothing to say. */
AS_API int asv_speak(as_voice *v, const char *utf8, int pitch_offset);
/* The next 30 ms block: 16-bit mono PCM at out_rate in an internal buffer valid until the next call (0 samples while
   the unit is still silent: the driver's lead trim).  *done = 1 once the unit has finished. */
AS_API int asv_render(as_voice *v, const short **pcm, int *done);
/* The driver's cancel: the Accent's flush (Ctrl-X), then the pitch said again if a capital's restore was pending. */
AS_API void asv_cancel(as_voice *v);

/* The text the driver would send for this utterance (currencies, _clean, strip, _numbers; the carriage return not
   included), for tests: returns the length, and copies it into out (NUL-terminated) when it fits in cap. */
AS_API int asv_say_bytes(const char *utf8, int numbers, char *out, int cap);

AS_API as_host *asv_host(as_voice *v);

#ifdef __cplusplus
}
#endif
#endif
