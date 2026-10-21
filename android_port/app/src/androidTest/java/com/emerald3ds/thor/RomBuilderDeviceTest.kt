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
    @Test fun usesPrebuiltSceneryOnUpdate() {
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
            assertTrue(messages.contains("Installing prebuilt scenery"))
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
        // Fresh setup must never invoke a scenery generator or reconstruct its inputs.
        if (!Python.isStarted()) Python.start(AndroidPlatform(context))
        Python.getInstance().getModule("builtins").callAttr("exec", """
            import emerald3ds_builder.build as core
            def unexpected_generation(*args, **kwargs):
                raise AssertionError('Fresh Android setup must use prebuilt scenery')
            core.run_generators = unexpected_generation
            core.build_tree = unexpected_generation
        """.trimIndent(), Python.getInstance().getModule("builtins").callAttr("dict"))
        var completed = false
        val messages = mutableListOf<String>()
        try {
            RomBuilder.build(context, Uri.fromFile(rom)) { fraction, message ->
                if (fraction == 1.0) completed = true
                messages.add(message)
            }
            assertTrue(completed)
            assertTrue(messages.contains("Installing prebuilt scenery"))
            assertTrue(PackImport.isCompatible(previous, RomBuilder.expectedAbi(context)!!))
            assertEquals("test progress", save.readText())
            assertEquals("test settings", settings.readText())
            assertFalse(context.cacheDir.resolve("rom-import").exists())
        } finally { rom.delete() }
    }
}
