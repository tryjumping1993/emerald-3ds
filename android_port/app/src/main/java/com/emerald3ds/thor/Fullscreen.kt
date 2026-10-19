package com.emerald3ds.thor

import android.app.Activity
import android.os.Build
import android.view.Display
import android.view.View
import android.view.Window
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager

const val TAG = "Emerald3DS"

/** Immersive fullscreen for a game window (the Activities and the Presentation). */
fun Window.immerse() {
    addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    attributes = attributes.apply { layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES }
    if (Build.VERSION.SDK_INT >= 30) {
        setDecorFitsSystemWindows(false)
        decorView.post {
            decorView.windowInsetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        }
    } else {
        @Suppress("DEPRECATION")
        decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
    }
}

/** The display this Activity is on (Activity.getDisplay needs API 30; minSdk is 29). Valid from onCreate on. */
val Activity.activityDisplay: Display?
    @Suppress("DEPRECATION")
    get() = if (Build.VERSION.SDK_INT >= 30) display else windowManager.defaultDisplay
