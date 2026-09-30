// The Braille Lite firmware a person brings, and what is in it.  The app carries none: the firmware is Blazie's, so
// each user imports their own copy, the way outspoken and Panthera take their engine data.  Nothing here touches
// Android, so the JVM tests (src/test) run it as it is; the bytes themselves are judged by the native side
// (bl_firmware.c) behind [Identify], by content and never by name.
package com.ssi263speech.tts

import java.io.ByteArrayInputStream
import java.io.File
import java.io.IOException
import java.util.zip.ZipInputStream

object FirmwareImport {
    const val ENGLISH = 0
    const val SPANISH = 1
    const val OTHER = 2                     // an image the voice can run, of no release known here
    const val NONE = -1                     // no Braille Lite firmware in the bytes
    const val REFUSED = -2                  // Blazie firmware, but not one the voice can run

    /** The two files each voice needs, by language: the firmware and the state made from it. */
    val FILES = listOf(listOf("BL2ENG.BNS", "bl2_2003_warm.state"), listOf("BL2SPA.BNS", "bl2spa_fresh.state"))

    /** The JVM tests' control (`gradlew testDebugUnitTest -Pssi263ImportBreak=1`; never set on a phone): only a
     * zip's top is looked at -- no folder down, no add-on layout -- so the layout tests must fail. */
    private val CONTROL = System.getProperty("ssi263.import.break") == "1"

    const val STATE_SIZE = 786432           // battery-backed RAM + file flash
    const val MAX_SOURCE = 64L shl 20       // an add-on is a few megabytes; firmware a few hundred kilobytes
    const val MAX_ENTRY = 4 shl 20          // an update program is half a megabyte

    const val NO_FIRMWARE_ZIP = "This zip does not contain Braille Lite firmware."
    const val NO_FIRMWARE_FILE = "This file does not contain Braille Lite firmware."
    const val WHAT_TO_CHOOSE = "Choose the Braille Lite 2000's firmware: its update program (such as blt2000.exe), " +
        "the BL2ENG.BNS or BL2SPA.BNS inside it, a zip holding them at its top or one folder down, or the NVDA " +
        "add-on (.nvda-addon), which carries both."

    /** What the native side says about some bytes: SsiImport's in the app, a fake in the tests. */
    interface Identify {
        /** Find the firmware image in `data` and write it to `out` as a .BNS: [ENGLISH], [SPANISH] or [OTHER]; or
         * [NONE] / [REFUSED], with the reason. */
        fun firmware(data: ByteArray, out: File): Pair<Int, String>

        /** [ENGLISH] or [SPANISH] when `data` is the state the voice ships with for that release, else -1. */
        fun state(data: ByteArray): Int
    }

    fun label(language: Int): String = when (language) {
        ENGLISH -> "Braille Lite English: the June 2003 release"
        SPANISH -> "Braille Lite Spanish: ONCE's release"
        else -> "Braille Lite firmware of a release this app does not know"
    }

    fun languageName(language: Int) = if (language == SPANISH) "Spanish" else "English"

    /** One firmware the import will bring in.  `language` is [OTHER] until the person says which unit it is. */
    class Found(
        val language: Int,
        val from: String,                   // where it was: the file's name, or its path in the zip
        val firmware: File,                 // the .BNS, written into the staging folder
        val state: File?,                   // the shipped state, when the source carried it (the add-on does)
    ) {
        val label: String get() = label(language)
    }

    /** Either `refusal` says why nothing can be imported, in words for the person holding the phone, or `found`
     * lists what will be.  `notes` are things worth saying either way. */
    class Plan(val found: List<Found>, val refusal: String?, val notes: List<String> = emptyList()) {
        val needsLanguage: Boolean get() = found.any { it.language == OTHER }
    }

    private class Candidate(val language: Int, val from: String, val file: File)

    /** Everything a source's bytes hold, judged into `staging` (emptied first). */
    fun inspect(name: String, data: ByteArray, staging: File, id: Identify): Plan {
        if (staging.exists()) staging.deleteRecursively()
        staging.mkdirs()
        val sink = Sink(staging, id)
        if (zipAt(data, 0)) {
            sink.zip(name, data, 0, nested = false)
            return sink.plan(zip = true)
        }
        val out = sink.next()
        val (language, reason) = id.firmware(data, out)
        when {
            language >= 0 -> sink.firmware.add(Candidate(language, name, out))
            language == REFUSED -> sink.refused.add(name to reason)
            data.size == STATE_SIZE && id.state(data) >= 0 -> sink.states[id.state(data)] = name to data
            else -> {
                // An update program that is a zip behind its own code (blt2000.exe): its first entry onwards.
                val at = zipStart(data)
                if (at > 0) try {
                    sink.zip(name, data, at, nested = false)
                    return sink.plan(zip = false)
                } catch (e: IOException) { /* bytes that only looked like a zip: no firmware, said below */ }
            }
        }
        return sink.plan(zip = false)
    }

    private class Sink(val staging: File, val id: Identify) {
        val firmware = ArrayList<Candidate>()
        val refused = ArrayList<Pair<String, String>>()
        val states = HashMap<Int, Pair<String, ByteArray>>()
        val deep = ArrayList<String>()
        private var n = 0

        fun next(): File = File(staging, "candidate${n++}.BNS")

        /** The zip's entries at its top, one folder down, and where the NVDA add-on keeps the unit's files. */
        fun zip(name: String, data: ByteArray, at: Int, nested: Boolean) {
            try {
                ZipInputStream(ByteArrayInputStream(data, at, data.size - at)).use { zip ->
                    while (true) {
                        val e = zip.nextEntry ?: break
                        if (e.isDirectory) continue
                        val path = e.name.replace('\\', '/').trimStart('/')
                        val parts = path.split('/').filter { it.isNotEmpty() }
                        if (parts.isEmpty() || parts.contains("__MACOSX")) continue
                        val addon = parts.size == 3 && parts[0].equals("synthDrivers", true) &&
                            parts[1].equals("_ssi263_blazie", true)
                        val tooDeep = if (CONTROL) parts.size > 1 else parts.size > 2 && !addon
                        if (tooDeep) {
                            if (parts.last().lowercase().let { it.endsWith(".bns") || it.endsWith(".exe") })
                                deep.add(path)
                            continue
                        }
                        val bytes = readUpTo(zip, MAX_ENTRY) ?: continue
                        val from = if (nested) "$name, $path" else path
                        if (bytes.size == STATE_SIZE) {
                            val language = id.state(bytes)
                            if (language >= 0) { states.putIfAbsent(language, from to bytes); continue }
                        }
                        val out = next()
                        val (language, reason) = id.firmware(bytes, out)
                        when {
                            language >= 0 -> firmware.add(Candidate(language, from, out))
                            language == REFUSED -> refused.add(from to reason)
                            !nested && zipStart(bytes) >= 0 -> zip(path, bytes, zipStart(bytes), nested = true)
                        }
                    }
                }
            } catch (e: IOException) {
                if (!nested) throw e
            }
        }

        fun plan(zip: Boolean): Plan {
            val notes = ArrayList<String>()
            val found = ArrayList<Found>()
            for (language in listOf(ENGLISH, SPANISH)) {
                val c = firmware.firstOrNull { it.language == language } ?: continue
                val state = states[language]?.let { (_, bytes) ->
                    File(staging, FILES[language][1]).also { it.writeBytes(bytes) }
                }
                found.add(Found(language, c.from, c.file, state))
            }
            val others = firmware.filter { it.language == OTHER }
            if (found.isEmpty() && others.size == 1)
                found.add(Found(OTHER, others[0].from, others[0].file, null))
            else if (found.isEmpty() && others.size > 1)
                return Plan(emptyList(), "This ${if (zip) "zip" else "file"} holds more than one Braille Lite " +
                    "firmware of a release this app does not know (${others.joinToString(", ") { it.from }}). " +
                    "Import them one at a time.")
            else for (o in others)
                notes.add("${o.from} is Braille Lite firmware of a release this app does not know; it is left out.")
            val kept = found.map { it.firmware }.toSet()
            for (c in firmware) if (c.file !in kept) c.file.delete()
            if (found.isNotEmpty()) return Plan(found, null, notes)

            val refusal = StringBuilder(if (zip) NO_FIRMWARE_ZIP else NO_FIRMWARE_FILE)
            for ((from, _) in refused)
                refusal.append(" $from is Blazie firmware, but not a Braille Lite 2000 release this voice can run.")
            if (states.isNotEmpty())
                refusal.append(" It holds the unit's saved state (${states.values.joinToString(", ") { it.first }}), " +
                    "which this app makes itself from the firmware.")
            if (deep.isNotEmpty())
                refusal.append(" Firmware is looked for only at the top of the zip and one folder down; " +
                    "${deep.first()} is deeper than that.")
            refusal.append(" ").append(WHAT_TO_CHOOSE)
            return Plan(emptyList(), refusal.toString())
        }
    }

    private fun readUpTo(zip: ZipInputStream, cap: Int): ByteArray? {
        val out = java.io.ByteArrayOutputStream()
        val buffer = ByteArray(1 shl 16)
        while (true) {
            val n = zip.read(buffer)
            if (n < 0) break
            out.write(buffer, 0, n)
            if (out.size() > cap) return null
        }
        return out.toByteArray()
    }

    private fun zipAt(data: ByteArray, at: Int) = at >= 0 && at + 4 <= data.size && data[at] == 'P'.code.toByte() &&
        data[at + 1] == 'K'.code.toByte() && data[at + 2] == 3.toByte() && data[at + 3] == 4.toByte()

    /** Where the first zip entry starts in `data`, or -1. */
    fun zipStart(data: ByteArray): Int {
        for (i in 0..data.size - 4) if (zipAt(data, i)) return i
        return -1
    }
}
