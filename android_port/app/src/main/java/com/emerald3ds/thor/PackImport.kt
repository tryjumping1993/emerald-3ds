package com.emerald3ds.thor

import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.zip.CRC32

/** Replace the pack only after a complete, synced copy. Saves and settings are separate files. */
object PackImport {
    fun install(input: InputStream, folder: File, expectedAbi: Long? = null) {
        folder.mkdirs()
        val pending = File(folder, "emerald3ds.pak.partial")
        try {
            input.use { source ->
                val magic = ByteArray(8)
                var read = 0
                while (read < magic.size) {
                    val n = source.read(magic, read, magic.size - read)
                    require(n > 0) { "The selected file is incomplete." }
                    read += n
                }
                require(magic.contentEquals(byteArrayOf(69, 77, 51, 68, 80, 65, 75, 0))) {
                    "Choose emerald3ds.pak produced by the Thor builder."
                }
                FileOutputStream(pending).use { output ->
                    output.write(magic)
                    source.copyTo(output)
                    output.fd.sync()
                }
            }
            validate(pending, expectedAbi)
            Files.move(pending.toPath(), File(folder, "emerald3ds.pak").toPath(),
                StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
        } finally { pending.delete() }
    }

    private fun crc(bytes: ByteArray, length: Int = bytes.size): Long = CRC32().apply { update(bytes, 0, length) }.value

    /** Check the complete header, index, bounds and every payload CRC before replacing a working pack. */
    fun isCompatible(file: File, expectedAbi: Long): Boolean =
        runCatching { validate(file, expectedAbi) }.isSuccess

    private fun validate(file: File, expectedAbi: Long? = null) = RandomAccessFile(file, "r").use { source ->
        fun check(ok: Boolean) = require(ok) { "The data pack is incomplete or damaged. Build or copy it again." }
        val header = ByteArray(64)
        source.readFully(header)
        val head = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
        fun uint(offset: Int) = head.getInt(offset).toLong() and 0xffffffffL
        check(header.copyOfRange(0, 8).contentEquals(byteArrayOf(69, 77, 51, 68, 80, 65, 75, 0)) &&
            head.getInt(8) == 1 && crc(header, 60) == uint(60))
        require(expectedAbi == null || uint(12) == expectedAbi) {
            "This data pack belongs to another app version. Choose your Emerald ROM to build the matching data."
        }
        val count = uint(36)
        val indexOffset = head.getLong(40)
        val dataOffset = head.getLong(48)
        val indexSize = count * 40
        check(count in 1..100_000 && indexOffset >= 64 && dataOffset >= indexOffset + indexSize &&
            dataOffset <= source.length())
        val bytes = ByteArray(indexSize.toInt())
        source.seek(indexOffset)
        source.readFully(bytes)
        check(crc(bytes) == uint(56))
        val index = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        val buffer = ByteArray(65536)
        var previous = 0L
        repeat(count.toInt()) { i ->
            val at = i * 40
            val id = index.getLong(at)
            val offset = index.getLong(at + 16)
            val stored = index.getInt(at + 24).toLong() and 0xffffffffL
            val size = index.getInt(at + 28).toLong() and 0xffffffffL
            check((i == 0 || java.lang.Long.compareUnsigned(id, previous) > 0) &&
                index.getInt(at + 12) == 0 && stored == size && offset >= dataOffset &&
                offset <= source.length() && size <= source.length() - offset)
            previous = id
            source.seek(offset)
            var remaining = size
            val crc = CRC32()
            while (remaining > 0) {
                val n = source.read(buffer, 0, minOf(buffer.size.toLong(), remaining).toInt())
                check(n > 0)
                crc.update(buffer, 0, n)
                remaining -= n
            }
            check(crc.value == (index.getInt(at + 32).toLong() and 0xffffffffL))
        }
    }
}
