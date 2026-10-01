/* ssa_engine.h -- the Android front end in plain C: the voices, their settings, start, pull, stop and cancel.
 *
 * The Aicom Accent SA (the built-in voice, Tomi 2026-09-30: Aicom's ROMs ship in the APK, handed over in memory) runs
 * through as_voice.h, the NVDA Accent driver's front end in C.  Each utterance gets a unit of its own, booted as the
 * driver boots one and let go afterwards: so an utterance sounds the same whatever came before it (a capital's raised
 * pitch cannot linger, and a stop needs no flush), and every pitch -- the app's slider and the request's together --
 * is said the way the driver says a capital's, with snap_pitch, so it jumps rather than glides from the unit's
 * power-up pitch.  The next unit is booted as soon as one is let go.
 *
 * The Braille Lite voices (English, and Spanish when its files are there; the user imports their firmware) run through
 * bl_voice.h, the way the speech-dispatcher module (src/platforms/speechd/sd_ssi263.c) drives them -- a unit booted on
 * first use, settings sent before each utterance, the audio pulled block by block, a stop between blocks, and a cancel
 * so the next utterance starts clean.
 *
 * No JNI here (ssa_jni.c is the thin bridge), so the host-side test (src/platforms/android/test) runs this same code
 * on the desktop and over adb.  Not thread-safe: one caller at a time (the app holds a lock), except ssa_stop.
 */
#ifndef SSA_ENGINE_H
#define SSA_ENGINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ssa_engine ssa_engine;

#define SSA_ENGLISH 0                  /* BL2ENG.BNS + bl2_2003_warm.state */
#define SSA_SPANISH 1                  /* BL2SPA.BNS + bl2spa_fresh.state */
#define SSA_ACCENT_SA 2                /* Aicom's u2, u3, u4, from ssa_set_accent_roms */
#define SSA_VOICES 3

/* The Accent SA's level at the app's volume 100, in the driver's percent (as_voice.h's asv_set): the engine volume
   times this over 100.  test_volume_headroom.py measures it against the Braille Lite's. */
#define SSA_ACCENT_LEVEL 100

/* The app's own settings, on the NVDA driver's scales (bl_voice.h's blv_set): rate, pitch 0-100 (50 = the unit's
   factory rate and pitch), tone 0-26 (7), volume 0-200 (100 = the desktop voices' level), pack = short pauses.  The
   Accent SA takes rate, pitch and volume; tone and pack are the Braille Lite's. */
typedef struct {
    int rate, pitch, tone, volume, pack;
} ssa_settings;

/* datadir: the folder holding the firmware and state files. */
ssa_engine *ssa_new(const char *datadir);
void ssa_free(ssa_engine *e);

/* The Accent SA's ROMs (u2 64 KB, u3 and u4 32 KB each), copied.  1, or 0 (wrong sizes, out of memory). */
int ssa_set_accent_roms(ssa_engine *e, const unsigned char *u2, size_t n2, const unsigned char *u3, size_t n3,
                        const unsigned char *u4, size_t n4);

/* The voice can speak: both of a Braille Lite voice's files are in the data folder; the Accent SA's ROMs are set. */
int ssa_has_voice(const ssa_engine *e, int voice);

/* The settings a unit is booted with: sample rate (11025, 22050 or 44100; anything else is 22050, as sd_ssi263),
   inflection 0/1 (the Accent SA: full intonation or monotone, ESC M0 / M1), whine 0 off / 1 hiss / 2 whine (the
   Braille Lite's).  A change shuts the booted units down; the next use boots them again with the new settings. */
void ssa_configure(ssa_engine *e, int sample_rate, int inflection, int whine);
int ssa_sample_rate(const ssa_engine *e);

/* Boots the voice's unit now if it is not yet (the first utterance otherwise waits for it).  0, or -1 with the
   reason in err. */
int ssa_load(ssa_engine *e, int voice, char *err, int errlen);

/* Starts an utterance (UTF-8): the app's settings with the request's rate and pitch percentages on top (ssa_map.h).
   Cancels an utterance still running.  0: there is audio to pull; 1: nothing to say; -1: the unit could not boot. */
int ssa_start(ssa_engine *e, int voice, const char *utf8, const ssa_settings *s, int request_rate,
              int request_pitch);

/* Up to cap 16-bit samples of the utterance into out: the count, 0 once it has finished, -2 when stopped. */
int ssa_pull(ssa_engine *e, short *out, int cap);

/* Any thread: makes a pull in progress return -2 before its next block. */
void ssa_stop(ssa_engine *e);

/* The synthesis thread, after a stop or an abandoned utterance: the unit drops what it has not spoken. */
void ssa_cancel(ssa_engine *e);

/* Non-empty blocks the unit has rendered for the current utterance (the test's stop point). */
int ssa_blocks(const ssa_engine *e);

/* The firmware import's last check, on a folder of its own: boots the voice's unit from the files in datadir (the
   defaults: 22050 Hz, inflection on, no idle sound), speaks utf8 at the app's default settings and returns the
   samples it made -- 0 when the unit stays silent -- with the FNV-1a 64 of their little-endian bytes in *fnv when
   fnv is not NULL; or -1 with the reason in err. */
long ssa_probe(const char *datadir, int voice, const char *utf8, unsigned long long *fnv, char *err, int errlen);

/* The tests' controls (test_android_native.c sets them; the app never does): each puts back one bug the Accent SA's
   cases must catch.  1: the request's pitch dropped; 2: the pitch sent as a setting, so it glides instead of jumping
   (no snap_pitch); 3: one unit kept from utterance to utterance, so what came before is heard in what follows. */
extern int ssa_accent_break;

#ifdef __cplusplus
}
#endif
#endif
