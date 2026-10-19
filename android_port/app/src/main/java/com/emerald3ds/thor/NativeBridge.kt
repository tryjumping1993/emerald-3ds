package com.emerald3ds.thor

import android.os.Handler
import android.os.Looper
import android.view.Surface

/** The JNI surface of libemerald3ds.so; the contract is documented in include/android_host.h. */
object NativeBridge {
    init { System.loadLibrary("emerald3ds") }

    const val TOP = 0
    const val BOTTOM = 1

    @JvmStatic external fun nativeStart(internalDir: String, externalDir: String): Boolean
    @JvmStatic external fun nativeSetSurface(screen: Int, surface: Surface?)
    @JvmStatic external fun nativeSurfaceSize(screen: Int, width: Int, height: Int)
    @JvmStatic external fun nativeDisplayRate(hz: Float)
    @JvmStatic external fun nativePause()
    @JvmStatic external fun nativeResume()
    @JvmStatic external fun nativeStop()
    @JvmStatic external fun nativeKey(keyCode: Int, down: Boolean): Boolean
    @JvmStatic external fun nativeResetInput()
    @JvmStatic external fun nativeMotion(lx: Float, ly: Float, rx: Float, ry: Float, hatX: Float, hatY: Float, lt: Float, rt: Float)
    @JvmStatic external fun nativeTouch(action: Int, x: Float, y: Float, viewW: Int, viewH: Int)

    private val main = Handler(Looper.getMainLooper())

    /** Set by the Activity that shows the status overlay and owns the game's lifetime. */
    @Volatile var onMessage: ((String) -> Unit)? = null
    @Volatile var onGameExit: (() -> Unit)? = null
    @Volatile var onRefresh: ((Int) -> Unit)? = null

    /** Called by native code, from any thread. */
    @JvmStatic fun showMessage(text: String) { main.post { onMessage?.invoke(text) } }
    @JvmStatic fun gameExited() { main.post { onGameExit?.invoke() } }
    @JvmStatic fun requestRefresh(hz: String) { main.post { onRefresh?.invoke(hz.toInt()) } }
}
