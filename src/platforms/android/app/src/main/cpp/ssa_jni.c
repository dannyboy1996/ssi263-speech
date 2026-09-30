/* ssa_jni.c -- the JNI bridge from Kotlin (SsiNative.kt) to the front end (ssa_engine.h).
 *
 * Thin: it marshals strings and the PCM buffer across the boundary and calls ssa_*, which calls bl_voice -- the same
 * C the speech-dispatcher module and the NVDA add-on's library are built from, linked into this one .so.  No IPC.
 *
 * C, not C++: everything below is C, and a C++ bridge would bring libc++ into the APK for nothing.
 *
 * One engine per process.  Every call is serialised on the Kotlin side (SsiEngine's lock) except nativeStop, which
 * only sets a flag.
 */
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "ssa_engine.h"

static ssa_engine *g_engine;
static char g_error[256];

#define FN(name) Java_com_ssi263speech_tts_SsiNative_##name

JNIEXPORT jboolean JNICALL FN(nativeOpen)(JNIEnv *env, jclass cls, jstring jdir)
{
    const char *dir;
    (void)cls;
    if (g_engine) return JNI_TRUE;
    dir = (*env)->GetStringUTFChars(env, jdir, NULL);
    if (!dir) return JNI_FALSE;
    g_engine = ssa_new(dir);
    (*env)->ReleaseStringUTFChars(env, jdir, dir);
    return g_engine ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL FN(nativeHasVoice)(JNIEnv *env, jclass cls, jint voice)
{
    (void)env; (void)cls;
    return g_engine && ssa_has_voice(g_engine, voice) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL FN(nativeConfigure)(JNIEnv *env, jclass cls, jint rate, jint inflection, jint whine)
{
    (void)env; (void)cls;
    if (g_engine) ssa_configure(g_engine, rate, inflection, whine);
}

JNIEXPORT jint JNICALL FN(nativeSampleRate)(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return g_engine ? ssa_sample_rate(g_engine) : 22050;
}

JNIEXPORT jint JNICALL FN(nativeLoad)(JNIEnv *env, jclass cls, jint voice)
{
    (void)env; (void)cls;
    g_error[0] = 0;
    if (!g_engine) { strcpy(g_error, "the engine is not open"); return -1; }
    return ssa_load(g_engine, voice, g_error, sizeof g_error);
}

JNIEXPORT jstring JNICALL FN(nativeError)(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, g_error);
}

JNIEXPORT jint JNICALL FN(nativeStart)(JNIEnv *env, jclass cls, jint voice, jbyteArray jutf8, jint rate, jint pitch,
                                       jint tone, jint volume, jint pack, jint request_rate, jint request_pitch)
{
    ssa_settings s;
    jsize n;
    char *utf8;
    int r;
    (void)cls;
    if (!g_engine || !jutf8) return -1;
    n = (*env)->GetArrayLength(env, jutf8);
    utf8 = (char *)malloc((size_t)n + 1);
    if (!utf8) return -1;
    (*env)->GetByteArrayRegion(env, jutf8, 0, n, (jbyte *)utf8);
    utf8[n] = 0;
    s.rate = rate; s.pitch = pitch; s.tone = tone; s.volume = volume; s.pack = pack;
    g_error[0] = 0;
    r = ssa_start(g_engine, voice, utf8, &s, request_rate, request_pitch);
    free(utf8);
    return r;
}

/* Up to out.length / 2 samples as 16-bit little-endian bytes (every Android ABI is little-endian): the byte count,
   0 when the utterance is over, -2 when stopped. */
JNIEXPORT jint JNICALL FN(nativePull)(JNIEnv *env, jclass cls, jbyteArray jout)
{
    static short pcm[8192];
    jsize cap = (*env)->GetArrayLength(env, jout) / 2;
    int n;
    (void)cls;
    if (!g_engine) return -1;
    if (cap > (jsize)(sizeof pcm / sizeof pcm[0])) cap = (jsize)(sizeof pcm / sizeof pcm[0]);
    n = ssa_pull(g_engine, pcm, (int)cap);
    if (n <= 0) return n;
    (*env)->SetByteArrayRegion(env, jout, 0, n * 2, (const jbyte *)pcm);
    return n * 2;
}

JNIEXPORT void JNICALL FN(nativeStop)(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (g_engine) ssa_stop(g_engine);
}

JNIEXPORT void JNICALL FN(nativeCancel)(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (g_engine) ssa_cancel(g_engine);
}
