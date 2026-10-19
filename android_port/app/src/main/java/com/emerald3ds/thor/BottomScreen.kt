package com.emerald3ds.thor

import android.content.Context
import android.graphics.Color
import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import android.view.View
import android.widget.FrameLayout

/** Dim a static companion canvas after 30 seconds. The first touch only wakes it. */
class BottomScreen(context: Context) : FrameLayout(context) {
    private val handler = Handler(Looper.getMainLooper())
    private val shade = View(context).apply { setBackgroundColor(Color.BLACK); alpha = 0f }
    private var dimmed = false
    private var waking = false
    private val dim = Runnable { dimmed = true; shade.alpha = 0.8f }

    init {
        addView(ScreenView(context, NativeBridge.BOTTOM), LayoutParams(-1, -1))
        addView(shade, LayoutParams(-1, -1))
    }

    private fun wake() {
        handler.removeCallbacks(dim)
        dimmed = false
        shade.alpha = 0f
        handler.postDelayed(dim, 30_000)
    }

    override fun onAttachedToWindow() { super.onAttachedToWindow(); wake() }
    override fun onDetachedFromWindow() { handler.removeCallbacks(dim); super.onDetachedFromWindow() }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_DOWN) waking = dimmed
        wake()
        if (waking) {
            if (event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_CANCEL) waking = false
            return true
        }
        return super.dispatchTouchEvent(event)
    }
}
