package com.emerald3ds.thor

import android.content.Context
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * One screen's SurfaceView: hands its Surface to the engine and, for the bottom screen, its touches.
 * A replacement view can be created before the one it replaces is destroyed (the companion route
 * changes), so only the latest view of a screen may report its Surface as lost.
 */
class ScreenView(context: Context, private val screen: Int) : SurfaceView(context), SurfaceHolder.Callback {
    init { holder.addCallback(this) }

    override fun surfaceCreated(holder: SurfaceHolder) {
        owners[screen] = this
        NativeBridge.nativeSetSurface(screen, holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) =
        NativeBridge.nativeSurfaceSize(screen, width, height)

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        if (owners[screen] !== this) return
        owners[screen] = null
        NativeBridge.nativeSetSurface(screen, null)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (screen != NativeBridge.BOTTOM) return false
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE, MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL ->
                NativeBridge.nativeTouch(event.actionMasked, event.getX(0), event.getY(0), width, height)
        }
        return true
    }

    private companion object {
        val owners = arrayOfNulls<ScreenView>(2)
    }
}
