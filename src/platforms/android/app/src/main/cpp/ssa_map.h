/* ssa_map.h -- Android's request scales onto the Braille Lite voice's own (bl_voice.h's blv_set: NVDA's 0-100, 50 =
 * the unit's factory rate 11 and pitch 16), the way the speech-dispatcher module maps SSIP's.
 *
 * sd_ssi263.c takes SSIP's -100..100 (0 = normal) to 0..100 with 0 -> 50 (its to100).  An Android request carries a
 * percentage instead (SynthesisRequest.getSpeechRate / getPitch: 100 = normal), so it is first put on SSIP's scale --
 * 50 per doubling, so 200% -> 50, 400% -> 100, 50% -> -50, 25% -> -100 -- then through the same to100, and the
 * difference from the middle is added to the app's own slider (50 by default): a request at 100% leaves the slider
 * as it is, as outspoken applies an app's rate on top of its slider.
 *
 * Volume is not mapped: Android applies the request's volume to its own audio track, so the engine's volume is the
 * app's slider alone.
 *
 * Plain C, no JNI: the library (ssa_jni.c) and the host-side test (src/platforms/android/test) compile this file.
 */
#ifndef SSA_MAP_H
#define SSA_MAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* An Android percentage (100 = normal; <= 0 = not given, normal) on SSIP's -100..100 scale. */
int ssa_ssip_from_percent(int percent);
/* sd_ssi263.c's to100: SSIP -100..100 -> 0..100, 0 -> 50. */
int ssa_to100(int ssip);
/* The app's slider (0..100) with the request's percentage on top, clamped to 0..100. */
int ssa_rate(int slider, int percent);
int ssa_pitch(int slider, int percent);

/* The test's control (test_android_native.c sets it; the app never does): nonzero breaks the rate mapping on
   purpose -- the request's rate is dropped -- so a request at any rate but 100% must sound different. */
extern int ssa_map_break;

#ifdef __cplusplus
}
#endif
#endif
