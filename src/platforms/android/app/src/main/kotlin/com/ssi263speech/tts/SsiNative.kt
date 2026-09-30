// Raw JNI binding to libssi263speech.so (cpp/ssa_jni.c): the Braille Lite voices -- the unit's firmware on an
// emulated Z180 driving the SSI-263 model -- the same C the speech-dispatcher module and the NVDA add-on's library are
// built from.  Not thread-safe: SsiEngine serialises every call except nativeStop, which only sets a flag.
package com.ssi263speech.tts

object SsiNative {
    init {
        System.loadLibrary("ssi263speech")
    }

    const val ENGLISH = 0
    const val SPANISH = 1

    /** Open the engine on the folder holding the unit's files (once per process; later calls keep it). */
    external fun nativeOpen(dataDir: String): Boolean

    /** Both of the voice's files are in the data folder. */
    external fun nativeHasVoice(voice: Int): Boolean

    /** The settings a unit boots with: sample rate (11025, 22050, 44100), inflection 0/1, whine 0 off, 1 hiss,
     * 2 whine.  A change shuts the booted units down; the next use boots them again. */
    external fun nativeConfigure(sampleRate: Int, inflection: Int, whine: Int)

    external fun nativeSampleRate(): Int

    /** Boot the voice's unit now, if it is not yet.  0, or negative with the reason in [nativeError]. */
    external fun nativeLoad(voice: Int): Int

    external fun nativeError(): String

    /** Begin an utterance (UTF-8).  rate, pitch, tone, volume, pack: the app's settings on the NVDA driver's scales;
     * requestRate, requestPitch: the request's percentages (100 = normal), put on top by the C side (ssa_map.h).
     * 0 when there is audio to pull, 1 when there is nothing to say, negative on failure. */
    external fun nativeStart(voice: Int, utf8: ByteArray, rate: Int, pitch: Int, tone: Int, volume: Int, pack: Int,
                             requestRate: Int, requestPitch: Int): Int

    /** Fill `out` with 16-bit little-endian PCM: the byte count, 0 when the utterance is over, -2 when stopped. */
    external fun nativePull(out: ByteArray): Int

    /** Any thread: a pull in progress returns -2 before its next block. */
    external fun nativeStop()

    /** The synthesis thread, after a stop: the unit drops what it has not spoken, so the next utterance is clean. */
    external fun nativeCancel()

    /** Let the engine go, so the next [nativeOpen] reads the unit's files afresh (after an import or a removal). */
    external fun nativeClose()

    // ---- the firmware import (bl_firmware.h, bl_state.h, ssa_probe) ------------------------------------------------

    const val FW_NONE = -1
    const val FW_REFUSED = -2
    const val FW_UNKNOWN = -4

    /** Find the Braille Lite ROM image in these bytes by its content and, when it is a release on the list
     * (bl_firmware.c's), write it to `out` as a .BNS: [ENGLISH] or [SPANISH], the release's label in
     * [nativeImportError]; [FW_NONE] when there is no firmware in them, [FW_REFUSED] when it is another unit's,
     * [FW_UNKNOWN] when it is a Braille Lite 2000 release not on the list -- the reason in [nativeImportError]. */
    external fun nativeImportFirmware(data: ByteArray, out: String): Int

    /** The releases on the list, by label. */
    external fun nativeKnownFirmware(): Array<String>

    /** The state at `state` is the one [nativeMakeState] makes from the release at `bns`. */
    external fun nativeStateCheck(bns: String, state: String): Boolean

    /** Make the unit's state from its firmware, as the shipped one was made: 1, or 0 with the reason in
     * [nativeImportError].  Seconds long: never on the main thread. */
    external fun nativeMakeState(firmware: String, language: Int, out: String): Int

    /** How far [nativeMakeState] has come, 0..1; any thread. */
    external fun nativeStateProgress(): Double

    /** Any thread: [nativeMakeState] stops at its next report and fails with "cancelled". */
    external fun nativeStateCancel()

    /** Boot the voice's unit from the files in `dir` and speak the text: the samples (0 when it stays silent), or -1
     * with the reason in [nativeImportError]. */
    external fun nativeProbe(dir: String, voice: Int, utf8: ByteArray): Long

    external fun nativeImportError(): String
}
