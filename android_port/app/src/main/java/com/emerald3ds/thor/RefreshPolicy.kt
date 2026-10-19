package com.emerald3ds.thor

import android.util.Log
import android.view.Display
import android.view.Window

/** Asks for the display mode (at the current resolution) nearest to `hz`: the game runs at 60. */
object RefreshPolicy {
    fun request(window: Window, display: Display?, hz: Int) {
        display ?: return
        val current = display.mode
        val mode = display.supportedModes
            .filter { it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight }
            .minByOrNull { kotlin.math.abs(it.refreshRate - hz) } ?: return
        val attributes = window.attributes
        if (attributes.preferredDisplayModeId == mode.modeId) return
        attributes.preferredDisplayModeId = mode.modeId
        window.attributes = attributes
        Log.i(TAG, "refresh request display=${display.displayId} mode=${mode.modeId} ${mode.refreshRate}Hz (active ${display.refreshRate}Hz)")
    }
}
