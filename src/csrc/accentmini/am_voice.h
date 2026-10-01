/* am_voice.h -- the Accent-mini as a voice: nvda/accent/synthDrivers/accentmini.py's front end for its "mini" voice,
 * in C (its boot, its settings commands, currencies, _clean and _numbers, a capital's pitch with snap_pitch, the speak
 * loop with the lead trim and PCM, and cancel), around the C host (am_host.h: Aicom's SPKEMS.DVC on MAME's 8086,
 * src/hosts/accent.py's host) and a chip made from the built-in defaults (ssi263_default_params) -- as bl_voice.h is
 * the Braille Lite's and as_voice.h the Accent SA's.  The driver's text rules are shared with as_voice
 * (../accent_front.h).  For front ends without Python: the Linux speech-dispatcher module, Android.
 *
 * nvda/tools/am_voice_equiv.py gates it: the real NVDA driver's PCM and this one's, byte for byte, on the same texts
 * and settings (every sample rate, the settings' extremes, a capital, a cancel mid-utterance), and the text it sends
 * on random texts.  Not modelled: NVDA's index commands (a caller marks its own places) and more than one pitch
 * command per utterance (the capital's restore is this API's, after the text, as the driver's finally).
 *
 * The driver (SPKEMS.DVC, firmware/aicom-accent-mini) comes from the caller, as a path or in memory; never built in.
 * Not thread-safe per voice; different voices may be used from different threads.  MIT.
 *
 * Sources to build it: am_voice.c, am_host.c, ../accent_front.c, ../numwords.c, ../pc86/pc86.c, ../cpu/i86_mame.cpp
 * (C++17), and the chip (../ssi263.c, ../ssi263dsp.c); include folders ../ (src/csrc), ../cpu, ../pc86.
 */
#ifndef AM_VOICE_H
#define AM_VOICE_H

#include <stddef.h>
#include "am_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct am_voice am_voice;

/* Loads the driver (a path, or its bytes), runs its INIT as DOS would, and boots it as the NVDA driver does (ESC =F,
   ESC =M, Ctrl-X: the greeting flushed); its settings are then the Accent's power-up ones: rate 5, pitch 5, voice 5,
   full intonation.  out_rate: the sample rate (the NVDA driver's 11025, 22050 or 44100; any works).  The driver's
   INIT runs here (the NVDA add-on restores a snapshot of it instead: the same machine, am_voice_equiv.py shows it).
   NULL on failure, the reason in err. */
AM_API am_voice *amv_create(const char *dvc_path, double out_rate, char *err, int errlen);
AM_API am_voice *amv_create_mem(const unsigned char *dvc, size_t n, double out_rate, char *err, int errlen);
AM_API void amv_destroy(am_voice *v);

/* The driver's settings, on NVDA's scales: rate and pitch 0-100 (50 = the Accent's 5; rate 100 = H), inflection 0-100
   (100 = full intonation, ESC M0; 0 = monotone, M1; 25/50/75 = M2/M3/M4, the nearest taken), volume in percent (100 =
   the gain 1; above it is louder, clipped at full scale, as as_voice -- the NVDA driver stops at 100), numbers = the
   driver's custom number processing (its
   default: on), voice = the NVDA variant, the Accent's voice characteristic ESC V0-9 (5 = its default).  Sent before
   the next text, only what changed (the driver's _sent). */
AM_API void amv_set(am_voice *v, int rate, int pitch, int inflection, int volume, int numbers, int voice);

/* Starts one utterance (UTF-8; the driver's joined text).  pitch_offset != 0 is NVDA's PitchCommand(offset) before
   the text -- a capital: the pitch jumps (snap_pitch) to the settings' pitch plus the offset for this text, and is
   restored (snapped) once its audio is out.  Returns 1 when text was sent, 0 when there is nothing to say, -1 when the
   host failed (amv_fault; the next amv_speak boots a new card first, as the driver restarts it). */
AM_API int amv_speak(am_voice *v, const char *utf8, int pitch_offset);
/* The next 30 ms block: 16-bit mono PCM at out_rate in an internal buffer valid until the next call (0 samples while
   the unit is still silent: the driver's lead trim).  *done = 1 once the unit has finished, or on a host fault. */
AM_API int amv_render(am_voice *v, const short **pcm, int *done);
/* The driver's cancel: the Accent's flush (Ctrl-X; a text the driver is still taking is cut after its current
   character), what it had sent let play out silently, then the pitch said again if a capital's restore was pending.
   Once the utterance has finished (amv_render's done) it does nothing, as the driver's cancel between utterances. */
AM_API void amv_cancel(am_voice *v);
/* Nonzero after a host fault ended or refused the last utterance (cleared by the next amv_speak's reboot). */
AM_API int amv_fault(const am_voice *v);

/* The driver's _accent_pitch: NVDA's pitch 0-100 on the Accent's ESC P 0-9 (50 -> 5). */
AM_API int amv_pitch_step(int pitch);
/* The text the driver would send for this utterance (currencies, _clean, strip, _numbers; the carriage return not
   included), for tests: returns the length, and copies it into out (NUL-terminated) when it fits in cap. */
AM_API int amv_say_bytes(const char *utf8, int numbers, char *out, int cap);

AM_API am_host *amv_host(am_voice *v);

#ifdef __cplusplus
}
#endif
#endif
