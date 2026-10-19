package com.emerald3ds.thor

import android.content.Context
import android.util.Log
import java.io.File

/**
 * Copies the engine's files (assets/romfs/, absent in the self-test build) to filesDir/romfs/ on first run
 * and whenever the APK changes. The marker is written last, so an interrupted copy is redone.
 */
object Assets {
    private const val ROOT = "romfs"
    private const val MARKER = "BUILD_ID"

    /** Blocking; call off the main thread. */
    fun extractIfNeeded(context: Context) {
        val assets = context.assets
        if (assets.list(ROOT).isNullOrEmpty()) return
        val buildId = runCatching { assets.open("$ROOT/$MARKER").bufferedReader().use { it.readText().trim() } }
            .getOrElse { context.packageManager.getPackageInfo(context.packageName, 0).lastUpdateTime.toString() }
        val target = File(context.filesDir, ROOT)
        val marker = File(target, MARKER)
        if (marker.exists() && marker.readText() == buildId) return
        Log.i(TAG, "extracting $ROOT (build $buildId)")
        target.deleteRecursively()
        copy(context, ROOT, target)
        marker.writeText(buildId)
    }

    private fun copy(context: Context, path: String, target: File) {
        val children = context.assets.list(path).orEmpty()
        if (children.isEmpty()) {
            target.parentFile?.mkdirs()
            context.assets.open(path).use { input -> target.outputStream().use { input.copyTo(it) } }
        } else {
            target.mkdirs()
            children.forEach { copy(context, "$path/$it", File(target, it)) }
        }
    }
}
