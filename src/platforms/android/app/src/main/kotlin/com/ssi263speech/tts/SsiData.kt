// The unit's files: the firmware and saved states the APK carries as assets (build_android.sh stages them), copied
// once per installed version into device-protected storage, where the native side opens them by path -- and where
// they can be read before the phone is first unlocked, so the voice works on the lock screen after a restart.
package com.ssi263speech.tts

import android.content.Context
import android.util.Log
import java.io.File

object SsiData {
    private const val ASSETS = "firmware"
    private const val STAMP = ".installed"

    /** The two files each voice needs, by SsiNative's voice index. */
    val FILES = listOf(listOf("BL2ENG.BNS", "bl2_2003_warm.state"), listOf("BL2SPA.BNS", "bl2spa_fresh.state"))

    fun protectedContext(ctx: Context): Context =
        if (ctx.isDeviceProtectedStorage) ctx else ctx.createDeviceProtectedStorageContext()

    fun dir(ctx: Context): File = File(protectedContext(ctx).filesDir, "unit")

    /** The files the APK carries. */
    fun shipped(ctx: Context): Set<String> =
        try { ctx.assets.list(ASSETS).orEmpty().toSet() } catch (e: Exception) { emptySet() }

    fun ships(ctx: Context, voice: Int): Boolean = shipped(ctx).containsAll(FILES[voice])

    /** Copy the unit's files out of the APK if this version has not yet.  Idempotent; true when English is there. */
    @Synchronized fun stage(ctx: Context): Boolean {
        val dir = dir(ctx)
        val version = installedStamp(ctx)
        val stamp = File(dir, STAMP)
        val current = try { stamp.readText() == version } catch (e: Exception) { false }
        if (!current || FILES[0].any { !File(dir, it).isFile }) {
            dir.mkdirs()
            for (name in shipped(ctx)) {
                val tmp = File(dir, "$name.tmp")
                ctx.assets.open("$ASSETS/$name").use { input -> tmp.outputStream().use { input.copyTo(it) } }
                if (!tmp.renameTo(File(dir, name))) {
                    File(dir, name).delete()
                    tmp.renameTo(File(dir, name))
                }
            }
            stamp.writeText(version)
            Log.i("SsiData", "unit files staged in $dir: ${shipped(ctx).sorted()}")
        }
        return FILES[0].all { File(dir, it).isFile }
    }

    /** Which build put the files there: a reinstall or an update copies them again. */
    private fun installedStamp(ctx: Context): String = try {
        val info = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        "${info.longVersionCode}/${info.lastUpdateTime}"
    } catch (e: Exception) { "unknown" }
}
