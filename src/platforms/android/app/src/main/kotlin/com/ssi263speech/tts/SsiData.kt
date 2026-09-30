// The unit's files: the firmware each user imports (SsiImport) and the state made from it, in device-protected
// storage, where the native side opens them by path -- and where they can be read before the phone is first
// unlocked, so the voice works on the lock screen after a restart.  A release APK carries no firmware; a developer
// build made with SSI263_ANDROID_BUNDLE_FIRMWARE=1 carries it as assets, copied in once per installed version.
package com.ssi263speech.tts

import android.content.Context
import android.util.Log
import java.io.File

object SsiData {
    private const val ASSETS = "firmware"
    private const val STAMP = ".bundled"
    const val LABEL = ".label"              // beside each .BNS: what was imported, in words

    /** The two files each voice needs, by SsiNative's voice index. */
    val FILES = FirmwareImport.FILES

    fun protectedContext(ctx: Context): Context =
        if (ctx.isDeviceProtectedStorage) ctx else ctx.createDeviceProtectedStorageContext()

    fun dir(ctx: Context): File = File(protectedContext(ctx).filesDir, "unit")

    /** Where an import is judged and made ready, beside the real folder; gone when the import is. */
    fun staging(ctx: Context): File = File(protectedContext(ctx).filesDir, "unit.importing")

    /** Both of the voice's files are here. */
    fun has(ctx: Context, voice: Int): Boolean {
        stageBundled(ctx)
        return FILES[voice].all { File(dir(ctx), it).isFile }
    }

    fun any(ctx: Context): Boolean = has(ctx, SsiNative.ENGLISH) || has(ctx, SsiNative.SPANISH)

    /** What was imported for the voice, in words; null when nothing was. */
    fun label(ctx: Context, voice: Int): String? {
        if (!has(ctx, voice)) return null
        return try { File(dir(ctx), FILES[voice][0] + LABEL).readText() }
            catch (e: Exception) { "Braille Lite ${FirmwareImport.languageName(voice)} (built into this app)" }
    }

    /** Move a finished import's files (`ready`: .BNS, .state, .label) into place, each replacing its namesake --
     * holding the engine, so no utterance boots a unit from half of them. */
    fun install(ctx: Context, ready: File) = SsiEngine.withEngine {
        SsiEngine.reload()
        val dir = dir(ctx)
        dir.mkdirs()
        for (f in ready.listFiles()?.sortedBy { it.name } ?: emptyList()) {
            val live = File(dir, f.name)
            if (live.exists()) live.delete()
            if (!f.renameTo(live)) f.copyTo(live, overwrite = true)
        }
    }

    /** Remove every imported unit (a developer build's bundled one stays away until the app is reinstalled). */
    fun remove(ctx: Context) = SsiEngine.withEngine {
        SsiEngine.reload()
        dir(ctx).deleteRecursively()
    }

    private fun stampFile(ctx: Context): File = File(protectedContext(ctx).filesDir, STAMP)

    /** The files a developer build carries. */
    private fun bundled(ctx: Context): Set<String> =
        try { ctx.assets.list(ASSETS).orEmpty().toSet() } catch (e: Exception) { emptySet() }

    /** A developer build's firmware, copied in once per installed version (a reinstall copies it again).  A
     * release build has none, and this does nothing. */
    @Volatile private var bundleChecked = false
    private val bundleLock = Any()

    private fun stageBundled(ctx: Context): Unit = synchronized(bundleLock) {
        if (bundleChecked) return
        bundleChecked = true
        val names = bundled(ctx)
        if (names.isEmpty()) return
        val stamp = stampFile(ctx)
        val version = installedStamp(ctx)
        if (try { stamp.readText() == version } catch (e: Exception) { false }) return
        val dir = dir(ctx)
        dir.mkdirs()
        for (name in names) {
            val tmp = File(dir, "$name.tmp")
            ctx.assets.open("$ASSETS/$name").use { input -> tmp.outputStream().use { input.copyTo(it) } }
            if (!tmp.renameTo(File(dir, name))) {
                File(dir, name).delete()
                tmp.renameTo(File(dir, name))
            }
        }
        stamp.writeText(version)
        Log.i("SsiData", "bundled unit files staged in $dir: ${names.sorted()}")
    }

    private fun installedStamp(ctx: Context): String = try {
        val info = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        @Suppress("DEPRECATION")
        val code = if (android.os.Build.VERSION.SDK_INT >= 28) info.longVersionCode else info.versionCode.toLong()
        "$code/${info.lastUpdateTime}"
    } catch (e: Exception) { "unknown" }
}
