package com.emerald3ds.thor

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import java.util.zip.CRC32

class PackImportTest {
    private fun pack(): ByteArray {
        val bytes = ByteArray(131)
        val b = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        "EM3DPAK\u0000".toByteArray().copyInto(bytes)
        b.putInt(8, 1); b.putInt(36, 1); b.putLong(40, 64); b.putLong(48, 128)
        b.putLong(64, 1); b.putInt(72, 1); b.putLong(80, 128); b.putInt(88, 3); b.putInt(92, 3)
        bytes[128] = 4; bytes[129] = 5; bytes[130] = 6
        fun crc(start: Int, size: Int) = CRC32().apply { update(bytes, start, size) }.value.toInt()
        b.putInt(96, crc(128, 3)); b.putInt(56, crc(64, 40)); b.putInt(60, crc(0, 60))
        return bytes
    }

    @Test fun importsAndKeepsSave() {
        val folder = Files.createTempDirectory("thor-import-").toFile()
        try {
            val save = folder.resolve("emerald3ds.sav").apply { writeText("save") }
            val bytes = pack()
            PackImport.install(bytes.inputStream(), folder)
            assertArrayEquals(bytes, folder.resolve("emerald3ds.pak").readBytes())
            assertEquals("save", save.readText())
            assertFalse(folder.resolve("emerald3ds.pak.partial").exists())
        } finally { folder.deleteRecursively() }
    }

    @Test fun damagedAndTruncatedPacksKeepPreviousCopy() {
        val folder = Files.createTempDirectory("thor-import-").toFile()
        try {
            val previous = folder.resolve("emerald3ds.pak").apply { writeText("previous") }
            val damaged = pack().apply { this[lastIndex] = 7 }
            for (bytes in listOf(byteArrayOf(0, 1), pack().copyOf(8), pack().copyOf(130), damaged)) {
                assertTrue(runCatching { PackImport.install(bytes.inputStream(), folder) }.isFailure)
                assertEquals("previous", previous.readText())
                assertFalse(folder.resolve("emerald3ds.pak.partial").exists())
            }
        } finally { folder.deleteRecursively() }
    }

    @Test fun wrongEngineKeepsPackSaveAndSettings() {
        val folder = Files.createTempDirectory("thor-import-").toFile()
        try {
            val previous = folder.resolve("emerald3ds.pak").apply { writeText("previous") }
            val save = folder.resolve("emerald3ds.sav").apply { writeText("progress") }
            val settings = folder.resolve("settings.txt").apply { writeText("settings") }
            assertTrue(runCatching { PackImport.install(pack().inputStream(), folder, 123) }.isFailure)
            assertEquals("previous", previous.readText())
            assertEquals("progress", save.readText())
            assertEquals("settings", settings.readText())
            PackImport.install(pack().inputStream(), folder, 0)
            assertTrue(PackImport.isCompatible(previous, 0))
            assertFalse(PackImport.isCompatible(previous, 123))
            previous.writeBytes(pack().apply { this[lastIndex] = 17 })
            assertFalse(PackImport.isCompatible(previous, 0))
        } finally { folder.deleteRecursively() }
    }
}
