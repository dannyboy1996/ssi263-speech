// The import on the phone: a source's bytes (a file the system picker handed over, or a path from adb), judged by
// FirmwareImport with the native side's eyes, then brought in -- each unit's state made from its firmware on this
// phone (bl_state.c), checked, the unit made to speak once, and only then moved into place beside the voice.
package com.ssi263speech.tts

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.IOException
import java.io.InputStream

object SsiImport {
    /** The native side's judgement (bl_firmware.c through ssa_jni.c). */
    object NativeIdentify : FirmwareImport.Identify {
        override fun firmware(data: ByteArray, out: File): Pair<Int, String> {
            val r = SsiNative.nativeImportFirmware(data, out.absolutePath)
            return r to (if (r < 0) SsiNative.nativeImportError() else "")
        }

        override fun state(data: ByteArray): Int = SsiNative.nativeStateLanguage(data)
    }

    class Source(val name: String, val size: Long, val open: () -> InputStream)

    /** A file by path: the adb route. */
    fun source(file: File) = Source(file.name, file.length()) { file.inputStream() }

    /** A file the system picker handed over; its name and size come from the provider when it says them. */
    fun source(ctx: Context, uri: Uri): Source {
        var name = uri.lastPathSegment?.substringAfterLast('/') ?: "file"
        var size = -1L
        try {
            ctx.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE),
                null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    c.getColumnIndex(OpenableColumns.DISPLAY_NAME).takeIf { it >= 0 }
                        ?.let { i -> if (!c.isNull(i)) name = c.getString(i) }
                    c.getColumnIndex(OpenableColumns.SIZE).takeIf { it >= 0 }
                        ?.let { i -> if (!c.isNull(i)) size = c.getLong(i) }
                }
            }
        } catch (e: Exception) { /* the name is a courtesy */ }
        return Source(name, size) { ctx.contentResolver.openInputStream(uri) ?: throw IOException("cannot open $name") }
    }

    /** The whole source, which is small: firmware is a few hundred kilobytes, an add-on a few megabytes. */
    fun read(source: Source): ByteArray {
        if (source.size > FirmwareImport.MAX_SOURCE) throw IOException(tooBig(source))
        source.open().use { input ->
            val out = java.io.ByteArrayOutputStream()
            val buffer = ByteArray(1 shl 16)
            while (true) {
                val n = input.read(buffer)
                if (n < 0) break
                out.write(buffer, 0, n)
                if (out.size() > FirmwareImport.MAX_SOURCE) throw IOException(tooBig(source))
            }
            return out.toByteArray()
        }
    }

    private fun tooBig(source: Source) = "${source.name} is larger than 64 MB, so it is not Braille Lite firmware, " +
        "an update for it or the NVDA add-on."

    fun inspect(ctx: Context, source: Source): FirmwareImport.Plan =
        FirmwareImport.inspect(source.name, read(source), SsiData.staging(ctx), NativeIdentify)

    fun probeText(language: Int) = if (language == FirmwareImport.SPANISH) "Hola." else "Hello."

    class Cancelled : IOException("cancelled")

    /** An import in flight, held outside the screen so a screen rebuilt underneath it finds it again. */
    class Job(val plan: FirmwareImport.Plan, val choice: Int) {
        @Volatile var step = ""              // what is happening, in words
        @Volatile var cancelled = false
        @Volatile var result: Result<List<String>>? = null
        @Volatile var listener: ((Job) -> Unit)? = null

        /** 0..1 across the whole import (the states are nearly all of it). */
        fun progress(): Double = (made + SsiNative.nativeStateProgress() * making) / total
        @Volatile private var made = 0.0
        @Volatile private var making = 0.0
        private val total: Double = plan.found.sumOf { weight(language(it)).toDouble() }.coerceAtLeast(1.0)

        fun language(f: FirmwareImport.Found) = if (f.language == FirmwareImport.OTHER) choice else f.language

        /** English's state is 150 million instructions, Spanish's 1150 million. */
        private fun weight(language: Int) = if (language == FirmwareImport.SPANISH) 1150 else 150

        fun start(ctx: Context) = Thread({
            result = runCatching { commit(ctx.applicationContext) }
            listener?.invoke(this)
        }, "ssi263-import").start()

        fun cancel() {
            cancelled = true
            SsiNative.nativeStateCancel()
        }

        /** Each unit made ready in the staging folder, then all of them moved into place together. */
        private fun commit(ctx: Context): List<String> {
            val staging = SsiData.staging(ctx)
            val ready = File(staging, "ready")
            ready.deleteRecursively()
            ready.mkdirs()
            val labels = ArrayList<String>()
            try {
                for (f in plan.found) {
                    val language = language(f)
                    val files = FirmwareImport.FILES[language]
                    val bns = File(ready, files[0])
                    val state = File(ready, files[1])
                    if (!f.firmware.renameTo(bns)) f.firmware.copyTo(bns, overwrite = true)
                    val w = weight(language).toDouble()
                    if (f.state != null) {
                        if (!f.state.renameTo(state)) f.state.copyTo(state, overwrite = true)
                    } else {
                        step = "Making the ${FirmwareImport.languageName(language)} unit's state"
                        listener?.invoke(this)
                        making = w
                        val ok = SsiNative.nativeMakeState(bns.absolutePath, language, state.absolutePath) == 1
                        making = 0.0
                        if (cancelled) throw Cancelled()
                        if (!ok) throw IOException("The ${FirmwareImport.languageName(language)} unit's state " +
                            "could not be made: ${SsiNative.nativeImportError()}")
                        // A release the voice ships for makes the state it ships with, byte for byte.
                        if (f.language != FirmwareImport.OTHER && SsiNative.nativeStateLanguage(state.readBytes()) != language)
                            throw IOException("The ${FirmwareImport.languageName(language)} unit's state made on this " +
                                "phone is not the one the voice was tested with, so it was not imported.")
                    }
                    made += w
                    step = "Checking that the ${FirmwareImport.languageName(language)} unit speaks"
                    listener?.invoke(this)
                    val samples = SsiNative.nativeProbe(ready.absolutePath, language,
                        probeText(language).toByteArray(Charsets.UTF_8))
                    if (samples < 0) throw IOException("The ${FirmwareImport.languageName(language)} unit would not " +
                        "start: ${SsiNative.nativeImportError()}")
                    if (samples == 0L) throw IOException("The ${FirmwareImport.languageName(language)} unit started " +
                        "but stayed silent, so this firmware was not imported.")
                    File(ready, files[0] + SsiData.LABEL).writeText(
                        if (f.language == FirmwareImport.OTHER) "${f.label}, as ${FirmwareImport.languageName(language)}"
                        else f.label)
                    labels.add(File(ready, files[0] + SsiData.LABEL).readText())
                    if (cancelled) throw Cancelled()
                }
                step = "Moving the firmware into place"
                listener?.invoke(this)
                SsiData.install(ctx, ready)
                return labels
            } finally {
                staging.deleteRecursively()
            }
        }
    }

    /** One import at a time, whichever screen shows it. */
    @Volatile var job: Job? = null
}
