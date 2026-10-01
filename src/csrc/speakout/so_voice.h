/* so_voice.h -- the GW Micro Speak-Out as a voice: nvda/speakout/synthDrivers/speakout.py's front end in C (its boot,
 * the box's settings commands, currencies and _clean, a capital's pitch with snap_pitch, the speak loop with the lead
 * trim and PCM, cancel with the pitch said again, the sample-rate switch), around the C host (so_host.h) and a chip
 * made from the built-in defaults (ssi263_default_params) -- as bl_voice.h is the Braille Lite's and as_voice.h the
 * Accent SA's.  For front ends without Python: the Linux speech-dispatcher module, Android.
 *
 * nvda/tools/so_voice_equiv.py gates it: the real NVDA driver (under the stand-in NVDA) and this voice speak the same
 * texts with the same settings, and every PCM byte must be equal.
 *
 * Usage: sov_create (the firmware, the sample rate) -> sov_set (settings, any time between utterances) ->
 * sov_speak (one utterance) -> sov_render until *done (each call one 30 ms block of chip time; it may return 0
 * samples while the box is still reading) -> the next sov_speak; sov_cancel at any point stops the utterance (a
 * sov_speak while one is still active cancels it first).  One text per utterance: NVDA's multi-item sequences
 * (several texts, index commands, the driver's _joined) are the caller's, so `join` reaches only the box's word delay.
 *
 * The firmware (SPEAKOUT.HEX, GW Micro's) comes from the caller and is never built in.  Not thread-safe per voice.
 * Link: speakout/so_voice.c so_host.c so_board.c so_icu.c so_scu.c so_hex.c, cpu/v40_mame.cpp (C++17, no exceptions
 * or RTTI), numwords.c, ssi263.c, ssi263dsp.c; include dirs src/csrc, src/csrc/cpu, src/csrc/speakout.  MIT.
 */
#ifndef SO_VOICE_H
#define SO_VOICE_H

#include <stddef.h>
#include "so_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct so_voice so_voice;

/* Boots the box as the driver does (power-on, its greeting flushed with Ctrl-X, punctuation none: ^E Mn), at out_rate
   (the driver offers 11025, 22050 and 44100).  sov_create takes the firmware's path, sov_create_hex its Intel HEX
   text in memory (Android's asset).  NULL on failure, the reason in err. */
SO_API so_voice *sov_create(const char *firmware, double out_rate, char *err, int errlen);
SO_API so_voice *sov_create_hex(const char *hex, size_t len, double out_rate, char *err, int errlen);
SO_API void sov_destroy(so_voice *v);

/* The driver's settings, on NVDA's scales: rate, pitch and volume 0-100 (50, 50, 100 = the box's rate 5 and pitch 3,
   the gain 1; volume may go to 200, louder and clipped at full scale, as asv_set's: Android's slider); tone 0-25 = the box's tones A-Z (the driver's variant; 8 = I, the box's default); join = "Join
   phrases" (word delay 0, else the box's factory 1), short_pauses = "Shorten pauses between sentences" (sentence
   delay 0, else 1).  Both default on, as in the driver.  Sent before the next text, only when something changed. */
SO_API void sov_set(so_voice *v, int rate, int pitch, int tone, int volume, int join, int short_pauses);

/* The driver's sample rate (_switch_rate): a different rate reboots the box at it (the chip renders at the host
   rate), and the next utterance sends every setting again.  Returns 0, or -1 if the new box could not be made (the
   old one is kept). */
SO_API int sov_set_sample_rate(so_voice *v, double out_rate);
SO_API double sov_sample_rate(const so_voice *v);

/* Starts one utterance (UTF-8).  pitch_offset != 0 is NVDA's PitchCommand(offset) before the text -- a capital: the
   pitch jumps (snap_pitch) to the settings' pitch plus the offset for this text, and is restored (snapped) once its
   audio is out.  Returns 1 when text was sent, 0 when there is nothing to say (render then reports done at once). */
SO_API int sov_speak(so_voice *v, const char *utf8, int pitch_offset);
/* The next 30 ms block: 16-bit mono PCM at the sample rate in an internal buffer valid until the next call (0 samples
   while the box is still silent: the driver's lead trim).  *done = 1 once the box has finished (or on a CPU fault:
   sov_fault). */
SO_API int sov_render(so_voice *v, const short **pcm, int *done);
/* The driver's cancel: input dropped, Ctrl-X, what was queued played out silently; then the user's pitch said again
   if a pitch command may have been dropped.  The next sov_speak starts clean. */
SO_API void sov_cancel(so_voice *v);
/* Nonzero once the V40 has faulted (an undefined opcode or a halt the firmware never makes): the box is rebooted by
   the next sov_speak, as the driver reboots it when speech fails. */
SO_API int sov_fault(const so_voice *v);

/* A job: NVDA's whole speech sequence as the NVDA driver (nvda/speakout/synthDrivers/speakout.py, since 0.7.5 on
   this library) speaks it -- several texts, PitchCommands and IndexCommands in one utterance, which sov_speak cannot
   say byte for byte: the lead trim is armed once per job (the second text's head is not trimmed), and a capital's
   pitch stays until the next PitchCommand or the job's end.
     sov_begin            _speakJob's start: a faulted box rebooted, the settings (sov_set) sent if they changed, the
                          lead trim armed.
     sov_pitch(offset)    a PitchCommand item (offset 0 included: back to the user's pitch); the pitch is sov_set's
                          at the call, the restore at sov_end the one sov_begin sent.
     sov_text(bytes, n)   a text item, as the box is sent it: sov_say_bytes' bytes, the carriage return included;
                          then sov_render until *done before the next item.  0 when n is 0.
     sov_end              _speakJob's finally: the user's pitch said again (snapped) if a capital left it changed.
     sov_flush            after sov_end when the job was cancelled (_run's box.cancel() and _resend_pitch); called
                          whether or not a text was still sounding, as the driver does.
   IndexCommands are the caller's (the driver's player callbacks).  sov_speak and sov_cancel are a one-text job. */
SO_API void sov_begin(so_voice *v);
SO_API void sov_pitch(so_voice *v, int pitch_offset);
SO_API int sov_text(so_voice *v, const unsigned char *bytes, int n);
SO_API void sov_end(so_voice *v);
SO_API void sov_flush(so_voice *v);

/* The driver's _box_pitch: NVDA's pitch 0-100 on the box's 0-9 (50 -> 3). */
SO_API int sov_pitch_step(int pitch);
/* The bytes the driver would send for this utterance's text (currencies, _clean, strip, Latin-1, the carriage
   return included), for tests: returns the length, and copies them into out when they fit in cap. */
SO_API int sov_say_bytes(const char *utf8, unsigned char *out, int cap);

SO_API so_host *sov_host(so_voice *v);
SO_API ssi263 *sov_chip(so_voice *v);

#ifdef __cplusplus
}
#endif
#endif
