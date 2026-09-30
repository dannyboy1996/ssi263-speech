/* ssa_engine.h -- the Android front end in plain C: the Braille Lite voices (English, and Spanish when its files are
 * there) around bl_voice.h, the way the speech-dispatcher module (src/platforms/speechd/sd_ssi263.c) drives them --
 * a unit booted on first use, settings sent before each utterance, the audio pulled block by block, a stop between
 * blocks, and a cancel so the next utterance starts clean.
 *
 * No JNI here (ssa_jni.c is the thin bridge), so the host-side test (src/platforms/android/test) runs this same code
 * on the desktop and over adb.  Not thread-safe: one caller at a time (the app holds a lock), except ssa_stop.
 */
#ifndef SSA_ENGINE_H
#define SSA_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ssa_engine ssa_engine;

#define SSA_ENGLISH 0                  /* BL2ENG.BNS + bl2_2003_warm.state */
#define SSA_SPANISH 1                  /* BL2SPA.BNS + bl2spa_fresh.state */
#define SSA_VOICES 2

/* The app's own settings, on the NVDA driver's scales (bl_voice.h's blv_set): rate, pitch 0-100 (50 = the unit's
   factory rate and pitch), tone 0-26 (7), volume 0-100, pack = short pauses. */
typedef struct {
    int rate, pitch, tone, volume, pack;
} ssa_settings;

/* datadir: the folder holding the firmware and state files. */
ssa_engine *ssa_new(const char *datadir);
void ssa_free(ssa_engine *e);

/* Both of the voice's files are in the data folder. */
int ssa_has_voice(const ssa_engine *e, int voice);

/* The settings a unit is booted with: sample rate (11025, 22050 or 44100; anything else is 22050, as sd_ssi263),
   inflection 0/1, whine 0 off / 1 hiss / 2 whine.  A change shuts the booted units down; the next use boots them
   again with the new settings. */
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

#ifdef __cplusplus
}
#endif
#endif
