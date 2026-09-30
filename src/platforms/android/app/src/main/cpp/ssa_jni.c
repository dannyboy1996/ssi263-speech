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
#include "blazie/bl_firmware.h"
#include "blazie/bl_state.h"

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

/* The engine let go, so the next nativeOpen reads the unit's files afresh (after an import or a removal). */
JNIEXPORT void JNICALL FN(nativeClose)(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    ssa_free(g_engine);
    g_engine = NULL;
}

/* ---- the firmware import (FirmwareImport.kt, SsiImport.kt): its own error text, as it runs beside speech ------- */
static char g_import_error[256];
static volatile int g_state_cancel;
static volatile double g_state_done;

JNIEXPORT jstring JNICALL FN(nativeImportError)(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, g_import_error);
}

static unsigned char *bytes_of(JNIEnv *env, jbyteArray a, jsize *n)
{
    unsigned char *p;
    *n = (*env)->GetArrayLength(env, a);
    p = (unsigned char *)malloc((size_t)*n + 1);
    if (p) (*env)->GetByteArrayRegion(env, a, 0, *n, (jbyte *)p);
    return p;
}

/* bl_firmware.h's blv_import_firmware: BLV_FW_ENGLISH 0, _SPANISH 1, _OTHER 2; or negative, the reason in
   nativeImportError. */
JNIEXPORT jint JNICALL FN(nativeImportFirmware)(JNIEnv *env, jclass cls, jbyteArray jdata, jstring jout)
{
    jsize n;
    unsigned char *data;
    const char *out;
    int r;
    (void)cls;
    g_import_error[0] = 0;
    data = bytes_of(env, jdata, &n);
    if (!data) { strcpy(g_import_error, "out of memory"); return BLV_FW_WRITE; }
    out = (*env)->GetStringUTFChars(env, jout, NULL);
    r = out ? blv_import_firmware(data, (long)n, out, g_import_error, sizeof g_import_error) : BLV_FW_WRITE;
    if (out) (*env)->ReleaseStringUTFChars(env, jout, out);
    free(data);
    return r;
}

/* blv_state_language: 0 or 1 when the bytes are the state the voice ships with for that release, else -1. */
JNIEXPORT jint JNICALL FN(nativeStateLanguage)(JNIEnv *env, jclass cls, jbyteArray jdata)
{
    jsize n;
    unsigned char *data = bytes_of(env, jdata, &n);
    int r;
    (void)cls;
    if (!data) return -1;
    r = blv_state_language(data, (long)n);
    free(data);
    return r;
}

static int state_progress(void *ctx, double done)
{
    (void)ctx;
    g_state_done = done;
    return g_state_cancel;
}

/* blv_make_state: 1, or 0 with the reason in nativeImportError.  Long (seconds; Spanish most of a minute): call it
   off the main thread and read nativeStateProgress meanwhile; nativeStateCancel abandons it. */
JNIEXPORT jint JNICALL FN(nativeMakeState)(JNIEnv *env, jclass cls, jstring jfw, jint language, jstring jout)
{
    const char *fw, *out;
    int r = 0;
    (void)cls;
    g_import_error[0] = 0;
    g_state_cancel = 0;
    g_state_done = 0;
    fw = (*env)->GetStringUTFChars(env, jfw, NULL);
    out = (*env)->GetStringUTFChars(env, jout, NULL);
    if (fw && out)
        r = blv_make_state(fw, language, out, state_progress, NULL, g_import_error, sizeof g_import_error);
    if (fw) (*env)->ReleaseStringUTFChars(env, jfw, fw);
    if (out) (*env)->ReleaseStringUTFChars(env, jout, out);
    return r;
}

JNIEXPORT jdouble JNICALL FN(nativeStateProgress)(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return g_state_done;
}

JNIEXPORT void JNICALL FN(nativeStateCancel)(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    g_state_cancel = 1;
}

/* ssa_probe: the samples the unit in `dir` speaks for the text (0 when silent), or -1 with the reason. */
JNIEXPORT jlong JNICALL FN(nativeProbe)(JNIEnv *env, jclass cls, jstring jdir, jint voice, jbyteArray jutf8)
{
    jsize n;
    unsigned char *utf8 = bytes_of(env, jutf8, &n);
    const char *dir;
    long r = -1;
    (void)cls;
    g_import_error[0] = 0;
    if (!utf8) return -1;
    utf8[n] = 0;
    dir = (*env)->GetStringUTFChars(env, jdir, NULL);
    if (dir) r = ssa_probe(dir, voice, (const char *)utf8, NULL, g_import_error, sizeof g_import_error);
    if (dir) (*env)->ReleaseStringUTFChars(env, jdir, dir);
    free(utf8);
    return (jlong)r;
}
