// FirmwareImport's layout and wording on the JVM, with no phone and no firmware: the native side's judgement is
// played by a fake that knows a made-up image by the same signature (F3 C3 xx xx FF "COPYRIGHT") plus a letter for
// what it is.  The real judgement (bl_firmware.c) has its own test, test_import_native.py, on the real files.
//     gradlew testDebugUnitTest                              every case
//     gradlew testDebugUnitTest -Pssi263ImportBreak=1        the control: the layout rules off; the layout cases fail
package com.ssi263speech.tts

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class FirmwareImportTest {
    @get:Rule val temp = TemporaryFolder()

    /** A made-up image: the signature, then E (English), S (Spanish), O (another release) or T (Type 'n Speak). */
    private fun image(kind: Char) = byteArrayOf(0xF3.toByte(), 0xC3.toByte(), 0x32, 0x03, 0xFF.toByte()) +
        "COPYRIGHT".toByteArray() + kind.code.toByte() + ByteArray(1000) { (it * 7).toByte() }

    /** A .BNS: a loader, then the image at 3000h. */
    private fun bns(kind: Char) = ByteArray(0x3000) { 0x18 } + image(kind)

    /** A made-up state: the right size, W (English's) or F (Spanish's) first. */
    private fun state(kind: Char) = ByteArray(FirmwareImport.STATE_SIZE).also { it[0] = kind.code.toByte() }

    private object Fake : FirmwareImport.Identify {
        override fun firmware(data: ByteArray, out: File): Pair<Int, String> {
            val at = (0..data.size - 15).firstOrNull { i ->
                data[i] == 0xF3.toByte() && data[i + 1] == 0xC3.toByte() && data[i + 4] == 0xFF.toByte() &&
                    String(data, i + 5, 9, Charsets.ISO_8859_1) == "COPYRIGHT"
            } ?: return FirmwareImport.NONE to "no Braille Lite firmware in it"
            val language = when (data[at + 14].toInt().toChar()) {
                'E' -> FirmwareImport.ENGLISH
                'S' -> FirmwareImport.SPANISH
                'O' -> FirmwareImport.OTHER
                else -> return FirmwareImport.REFUSED to "not a release this voice can run"
            }
            out.writeBytes(data.copyOfRange(at, data.size))
            return language to ""
        }

        override fun state(data: ByteArray): Int = when {
            data.size != FirmwareImport.STATE_SIZE -> -1
            data[0] == 'W'.code.toByte() -> FirmwareImport.ENGLISH
            data[0] == 'F'.code.toByte() -> FirmwareImport.SPANISH
            else -> -1
        }
    }

    private fun zip(vararg entries: Pair<String, ByteArray>): ByteArray {
        val out = ByteArrayOutputStream()
        ZipOutputStream(out).use { z ->
            for ((name, bytes) in entries) { z.putNextEntry(ZipEntry(name)); z.write(bytes); z.closeEntry() }
        }
        return out.toByteArray()
    }

    private fun inspect(name: String, data: ByteArray) =
        FirmwareImport.inspect(name, data, temp.newFolder(), Fake)

    private fun languages(plan: FirmwareImport.Plan) = plan.found.map { it.language }

    @Test fun zipWithTheFirmwareAtItsTop() {
        val plan = inspect("update.zip", zip("BL2ENG.BNS" to bns('E'), "HOWTOUPD.TXT" to "text".toByteArray()))
        assertNull(plan.refusal)
        assertEquals(listOf(FirmwareImport.ENGLISH), languages(plan))
        assertEquals("BL2ENG.BNS", plan.found[0].from)
        assertTrue(plan.found[0].firmware.readBytes().contentEquals(image('E')))
        assertNull(plan.found[0].state)
    }

    @Test fun zipWithTheFirmwareOneFolderDown() {
        val plan = inspect("june2003.zip", zip("blt2000/BL2ENG.BNS" to bns('E'), "spanish/BL2SPA.BNS" to bns('S')))
        assertNull(plan.refusal)
        assertEquals(listOf(FirmwareImport.ENGLISH, FirmwareImport.SPANISH), languages(plan))
        assertEquals("spanish/BL2SPA.BNS", plan.found[1].from)
    }

    @Test fun theNvdaAddOnWithItsStates() {
        val d = "synthDrivers/_ssi263_blazie/"
        val plan = inspect("blazie.nvda-addon", zip("manifest.ini" to "name = blazie".toByteArray(),
            "synthDrivers/blazie.py" to "#".toByteArray(), d + "BL2ENG.BNS" to bns('E'),
            d + "bl2_2003_warm.state" to state('W'), d + "BL2SPA.BNS" to bns('S'),
            d + "bl2spa_fresh.state" to state('F'), d + "bl.dll" to ByteArray(5000)))
        assertNull(plan.refusal)
        assertEquals(listOf(FirmwareImport.ENGLISH, FirmwareImport.SPANISH), languages(plan))
        assertNotNull(plan.found[0].state)
        assertNotNull(plan.found[1].state)
        assertEquals('W'.code.toByte(), plan.found[0].state!!.readBytes()[0])
    }

    @Test fun aZipWithNoFirmwareSaysSo() {
        val plan = inspect("photos.zip", zip("a.txt" to "hello".toByteArray(), "b/c.bin" to ByteArray(300)))
        assertTrue(plan.found.isEmpty())
        assertTrue(plan.refusal!!, plan.refusal!!.startsWith("This zip does not contain Braille Lite firmware."))
    }

    @Test fun firmwareTwoFoldersDownIsNotLookedForButNamed() {
        val plan = inspect("deep.zip", zip("backup/blt2000/BL2ENG.BNS" to bns('E')))
        assertTrue(plan.found.isEmpty())
        assertTrue(plan.refusal!!.startsWith(FirmwareImport.NO_FIRMWARE_ZIP))
        assertTrue(plan.refusal!!, plan.refusal!!.contains("backup/blt2000/BL2ENG.BNS is deeper than that"))
    }

    @Test fun namesDoNotMatterContentDoes() {
        val plan = inspect("x.zip", zip("BL2ENG.BNS" to "not firmware".toByteArray(), "renamed.bin" to bns('S')))
        assertEquals(listOf(FirmwareImport.SPANISH), languages(plan))
        assertEquals("renamed.bin", plan.found[0].from)
    }

    @Test fun anUpdateProgramWithTheImageInside() {
        val exe = "MZ".toByteArray() + ByteArray(7000) { 1 } + bns('E') + ByteArray(3000) { 2 }
        val plan = inspect("BLT2000.EXE", exe)
        assertNull(plan.refusal)
        assertEquals(listOf(FirmwareImport.ENGLISH), languages(plan))
        assertEquals("BLT2000.EXE", plan.found[0].from)
    }

    @Test fun anUpdateProgramThatIsAZipBehindItsCode() {
        val exe = "MZ".toByteArray() + ByteArray(9000) { 1 } + zip("BL2ENG.BNS" to bns('E'), "SPELL.DIC" to ByteArray(99))
        val plan = inspect("blt2000.exe", exe)
        assertNull(plan.refusal)
        assertEquals(listOf(FirmwareImport.ENGLISH), languages(plan))
    }

    @Test fun anUpdateProgramInsideAZip() {
        val exe = "MZ".toByteArray() + ByteArray(9000) { 1 } + zip("BL2ENG.BNS" to bns('E'))
        val plan = inspect("fs.zip", zip("FS june2003/blt2000.exe" to exe))
        assertEquals(listOf(FirmwareImport.ENGLISH), languages(plan))
        assertEquals("FS june2003/blt2000.exe, BL2ENG.BNS", plan.found[0].from)
    }

    @Test fun aSingleBnsFile() {
        val plan = inspect("BL2SPA.BNS", bns('S'))
        assertEquals(listOf(FirmwareImport.SPANISH), languages(plan))
    }

    @Test fun anotherUnitsFirmwareIsRefused() {
        val plan = inspect("TNSENG-1.TNS", bns('T'))
        assertTrue(plan.found.isEmpty())
        assertTrue(plan.refusal!!.startsWith(FirmwareImport.NO_FIRMWARE_FILE))
        assertTrue(plan.refusal!!, plan.refusal!!.contains(
            "TNSENG-1.TNS is Blazie firmware, but not a Braille Lite 2000 release this voice can run."))
    }

    @Test fun aStateAloneIsNotFirmware() {
        val plan = inspect("bl2_2003_warm.state", state('W'))
        assertTrue(plan.found.isEmpty())
        assertTrue(plan.refusal!!, plan.refusal!!.contains("saved state (bl2_2003_warm.state)"))
    }

    @Test fun anUnknownReleaseAsksWhichUnit() {
        val plan = inspect("other.zip", zip("BL4ENG.BNS" to bns('O')))
        assertNull(plan.refusal)
        assertEquals(listOf(FirmwareImport.OTHER), languages(plan))
        assertTrue(plan.needsLanguage)
    }

    @Test fun twoUnknownReleasesAreRefused() {
        val plan = inspect("others.zip", zip("a/BL4ENG.BNS" to bns('O'), "b/BL2ENG.BNS" to bns('O')))
        assertTrue(plan.found.isEmpty())
        assertTrue(plan.refusal!!, plan.refusal!!.contains("more than one"))
    }

    @Test fun aKnownReleaseWinsOverAnUnknownOne() {
        val plan = inspect("mixed.zip", zip("old/BL2ENG.BNS" to bns('O'), "new/BL2ENG.BNS" to bns('E')))
        assertEquals(listOf(FirmwareImport.ENGLISH), languages(plan))
        assertEquals("new/BL2ENG.BNS", plan.found[0].from)
        assertTrue(plan.notes.single().startsWith("old/BL2ENG.BNS is Braille Lite firmware of a release"))
        assertTrue(!plan.needsLanguage)
    }

    @Test fun randomBytesAreNotFirmware() {
        val plan = inspect("noise.bin", ByteArray(300000) { (it * 31 + 7).toByte() })
        assertTrue(plan.found.isEmpty())
        assertTrue(plan.refusal!!.startsWith(FirmwareImport.NO_FIRMWARE_FILE))
    }
}
