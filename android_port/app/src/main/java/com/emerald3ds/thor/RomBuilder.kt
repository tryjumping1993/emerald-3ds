package com.emerald3ds.thor

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import com.chaquo.python.Python
import com.chaquo.python.android.AndroidPlatform
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Runs in :builder, away from the game's GL/audio threads and memory. No network is used. */
object RomBuilder {
    fun interface Progress { fun update(fraction: Double, message: String) }

    fun expectedAbi(context: Context): Long? = runCatching {
        val bytes = context.assets.open("romfs/engine/abi.bin").use { it.readBytes() }
        require(bytes.size == 4)
        ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).int.toLong() and 0xffffffffL
    }.getOrNull()

    fun dataFolder(context: Context): File =
        (context.getExternalFilesDir(null) ?: context.filesDir).resolve("emerald3ds")

    fun build(context: Context, uri: Uri, progress: Progress) {
        val abi = expectedAbi(context) ?: error("This APK does not contain the game engine.")
        // This is a fixed app-cache child: never delete a selected document or game data.
        val work = context.cacheDir.resolve("rom-import")
        work.deleteRecursively()
        check(work.mkdirs()) { "There is not enough storage to prepare the game data." }
        try {
            progress.update(0.0, "Reading your Emerald ROM")
            var name = "emerald.gba"
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
                if (it.moveToFirst()) name = it.getString(0) ?: name
            }
            val rom = work.resolve(if (name.endsWith(".zip", true)) "emerald.zip" else "emerald.gba")
            context.contentResolver.openInputStream(uri).use { input ->
                requireNotNull(input) { "The selected ROM could not be opened. Please choose it again." }
                FileOutputStream(rom).use { output ->
                    val buffer = ByteArray(65536)
                    var total = 0L
                    while (true) {
                        val n = input.read(buffer)
                        if (n < 0) break
                        total += n
                        require(total <= 32 * 1024 * 1024) { "Choose a clean Emerald .gba ROM or a ZIP containing it." }
                        output.write(buffer, 0, n)
                    }
                }
            }
            copyAssets(context, "rombuilder", work.resolve("builder"))
            val manifest = JSONObject(work.resolve("builder/manifest.json").readText())
            check(manifest.getLong("abi") == abi) { "This APK's ROM builder does not match its engine." }
            check(manifest.optInt("schema") == 2 && manifest.optBoolean("prebuilt_scenery")) {
                "This APK is missing its prebuilt scenery. Install the complete APK again."
            }
            if (!Python.isStarted()) Python.start(AndroidPlatform(context.applicationContext))
            val built = work.resolve("generated.pak")
            val result = JSONObject(Python.getInstance().getModule("android_builder").callAttr("build",
                rom.path, work.resolve("builder/payload").path, built.path, abi, progress,
                dataFolder(context).resolve("emerald3ds.pak").path).toString())
            require(result.getBoolean("ok")) {
                result.optString("message") + result.optString("hint").let { if (it.isEmpty()) "" else "\n\n$it" }
            }
            progress.update(0.98, "Installing verified game data")
            PackImport.install(built.inputStream(), dataFolder(context), abi)
            progress.update(1.0, "Game data ready")
        } finally {
            work.deleteRecursively()
        }
    }

    private fun copyAssets(context: Context, path: String, target: File) {
        val children = context.assets.list(path).orEmpty()
        if (children.isEmpty()) {
            target.parentFile?.mkdirs()
            context.assets.open(path).use { input -> target.outputStream().use { input.copyTo(it) } }
        } else {
            target.mkdirs()
            children.forEach { copyAssets(context, "$path/$it", target.resolve(it)) }
        }
    }
}
