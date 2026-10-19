package com.emerald3ds.thor

import android.app.Activity
import android.app.Presentation
import android.os.Bundle
import android.util.Log
import android.view.Display
import android.view.KeyEvent
import android.view.MotionEvent

/**
 * The bottom screen as a Presentation on the second display. Its keys and controller motion go to MainActivity:
 * a touch here moves input focus to this window, but the buttons are still the game's.
 */
class CompanionPresentation(private val owner: MainActivity, display: Display) : Presentation(owner, display) {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window?.let { it.immerse(); RefreshPolicy.request(it, display, 60) }
        setContentView(ScreenView(context, NativeBridge.BOTTOM))
        Log.i(TAG, "companion route=presentation display=${display.displayId}")
    }

    override fun dispatchKeyEvent(event: KeyEvent) = owner.dispatchKeyEvent(event)
    override fun onGenericMotionEvent(event: MotionEvent) = owner.onGenericMotionEvent(event)
}

/** The bottom screen as a second Activity, launched on the other display when a Presentation is refused. */
class CompanionActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val owner = MainActivity.current
        // A stale launch (the route changed or the owner stopped since) must not leave a window behind.
        if (owner == null || !owner.attachCompanion(this, intent.getLongExtra(EXTRA_GENERATION, -1))) {
            finish()
            return
        }
        window.immerse()
        RefreshPolicy.request(window, activityDisplay, 60)
        setContentView(ScreenView(this, NativeBridge.BOTTOM))
    }

    // Home on the bottom display backgrounds this window while the game still runs: MainActivity brings it back.
    override fun onStop() {
        MainActivity.current?.companionStopped(this)
        super.onStop()
    }

    override fun onDestroy() {
        MainActivity.current?.detachCompanion(this)
        super.onDestroy()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean = MainActivity.current?.dispatchKeyEvent(event) ?: super.dispatchKeyEvent(event)
    override fun onGenericMotionEvent(event: MotionEvent): Boolean = MainActivity.current?.onGenericMotionEvent(event) ?: super.onGenericMotionEvent(event)

    companion object { const val EXTRA_GENERATION = "companionGeneration" }
}
