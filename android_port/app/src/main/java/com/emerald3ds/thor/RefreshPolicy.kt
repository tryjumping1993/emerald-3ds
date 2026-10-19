package com.emerald3ds.thor

import android.util.Log
import android.view.Display
import android.view.Window

/** Asks for the display mode (at the current resolution) nearest to `hz`: the game runs at 60. */
object RefreshPolicy {
    /** Firmware may reset the active mode on sleep while retaining the old window preference. */
    fun clear(window: Window) {
        val attributes = window.attributes
        if (attributes.preferredDisplayModeId == 0) return
        attributes.preferredDisplayModeId = 0
        window.attributes = attributes
    }

    fun request(window: Window, display: Display?, hz: Int) {
        display ?: return
        val current = display.mode
        val modes = display.supportedModes
            .filter { it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight }
        // 90/144 Hz panels need 60 Hz: choosing the mode nearest 120 would fight the native 60 Hz hint.
        val wanted = if (hz >= 100 && modes.any { kotlin.math.abs(it.refreshRate - 120) <= 2 }) 120 else 60
        val mode = modes.minByOrNull { kotlin.math.abs(it.refreshRate - wanted) } ?: return
        val attributes = window.attributes
        if (attributes.preferredDisplayModeId == mode.modeId) return
        attributes.preferredDisplayModeId = mode.modeId
        window.attributes = attributes
        Log.i(TAG, "refresh request display=${display.displayId} mode=${mode.modeId} ${mode.refreshRate}Hz (active ${display.refreshRate}Hz)")
    }
}
