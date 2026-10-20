package com.emerald3ds.thor

import android.net.Uri
import com.chaquo.python.Python
import com.chaquo.python.android.AndroidPlatform
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.*
import org.junit.Test
import org.junit.Assume.assumeTrue
import org.junit.runner.RunWith

/** Local-only test: the maintainer stages their ROM through run-as, never in an APK. */
@RunWith(AndroidJUnit4::class)
class RomBuilderDeviceTest {
    @Test fun reusesVerifiedSceneryOnUpdate() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val rom = context.filesDir.resolve("test-rom.gba")
        val folder = RomBuilder.dataFolder(context)
        val seed = context.filesDir.resolve("test-pack.pak")
        if (seed.isFile) {
            folder.mkdirs()
            PackImport.install(seed.inputStream(), folder, RomBuilder.expectedAbi(context)!!)
            seed.delete()
            folder.resolve("emerald3ds.sav").writeText("test progress")
            folder.resolve("settings.txt").writeText("test settings")
        }
        assumeTrue(rom.isFile && PackImport.isCompatible(folder.resolve("emerald3ds.pak"), RomBuilder.expectedAbi(context)!!))
        val messages = mutableListOf<String>()
        try {
            RomBuilder.build(context, Uri.fromFile(rom)) { _, message -> messages.add(message) }
            assertTrue(messages.contains("Reusing verified scenery"))
            assertFalse(messages.any { it.startsWith("Generating the 3D scenery") })
            assertEquals("test progress", folder.resolve("emerald3ds.sav").readText())
            assertEquals("test settings", folder.resolve("settings.txt").readText())
        } finally { rom.delete() }
    }

    @Test fun buildsOnAndroidAndKeepsProgress() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val rom = context.filesDir.resolve("test-rom.gba")
        assertTrue("Stage test-rom.gba using adb run-as before this local test", rom.isFile)
        val folder = RomBuilder.dataFolder(context).apply { mkdirs() }
        val save = folder.resolve("emerald3ds.sav").apply { writeText("test progress") }
        val settings = folder.resolve("settings.txt").apply { writeText("test settings") }
        val previous = folder.resolve("emerald3ds.pak").apply { writeText("previous pack") }
        // Preserve only a local diagnostic on generator failure. Nothing is bundled.
        if (!Python.isStarted()) Python.start(AndroidPlatform(context))
        Python.getInstance().getModule("builtins").callAttr("exec", """
            import pathlib, shutil
            import emerald3ds_builder.build as core
            original_generators = core.run_generators
            def capture_failure(tree, *args, **kwargs):
                try:
                    return original_generators(tree, *args, **kwargs)
                except Exception:
                    source = tree / '3ds_port/romfs/voxel/relief.bin'
                    if source.exists(): shutil.copyfile(source, ${org.json.JSONObject.quote(context.filesDir.resolve("test-relief.bin").path)})
                    raise
            core.run_generators = capture_failure
        """.trimIndent(), Python.getInstance().getModule("builtins").callAttr("dict"))
        var completed = false
        try {
            RomBuilder.build(context, Uri.fromFile(rom)) { fraction, _ ->
                if (fraction == 1.0) completed = true
            }
            assertTrue(completed)
            assertTrue(PackImport.isCompatible(previous, RomBuilder.expectedAbi(context)!!))
            assertEquals("test progress", save.readText())
            assertEquals("test settings", settings.readText())
            assertFalse(context.cacheDir.resolve("rom-import").exists())
        } finally { rom.delete() }
    }
}
