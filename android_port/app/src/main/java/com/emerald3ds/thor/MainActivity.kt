package com.emerald3ds.thor

import android.app.Activity
import android.app.ActivityManager
import android.app.ActivityOptions
import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.graphics.Color
import android.hardware.display.DisplayManager
import android.hardware.input.InputManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Display
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.math.abs
import kotlin.concurrent.thread

/**
 * The top screen, the owner of the engine's lifetime and of the companion (bottom screen) window.
 * Native pause/resume follow onStop/onStart, not onPause/onResume: on a dual-screen device a touch on
 * the other display can pause this Activity while it is still fully visible and must keep drawing.
 */
class MainActivity : Activity(), DisplayManager.DisplayListener, InputManager.InputDeviceListener {
    private val handler = Handler(Looper.getMainLooper())
    private lateinit var displays: DisplayManager
    private lateinit var stack: LinearLayout
    private lateinit var status: TextView
    private var presentation: CompanionPresentation? = null
    private var companionActivity: CompanionActivity? = null
    private var inlineView: ScreenView? = null
    private var started = false
    /** The companion Activity was sent to the background on its own (Home on the bottom display); see onWindowFocusChanged. */
    private var companionLost = false
    /** Debug builds only: `am start --es companion activity|inline` skips the earlier routes to test the fallbacks. */
    private var forcedRoute: String? = null

    private val router: CompanionRouter = CompanionRouter(object : CompanionRouter.Host {
        override fun close() {
            presentation?.let { it.setOnDismissListener(null); it.dismiss() }
            presentation = null
            companionActivity?.finish()
            companionActivity = null
            inlineView?.let { stack.removeView(it) }
            inlineView = null
        }

        override fun presentation(displayId: Int): Boolean {
            if (forcedRoute != null) return false
            val display = displays.getDisplay(displayId) ?: return false
            return try {
                val shown = CompanionPresentation(this@MainActivity, display)
                shown.show()
                shown.setOnDismissListener { handler.post { if (presentation === shown) { router.stop(); refreshDisplays() } } }
                presentation = shown
                true
            } catch (e: WindowManager.InvalidDisplayException) {
                Log.w(TAG, "presentation rejected on display $displayId", e); false
            } catch (e: SecurityException) {
                Log.w(TAG, "presentation denied on display $displayId", e); false
            }
        }

        override fun activity(displayId: Int, generation: Long): Boolean {
            val intent = Intent(this@MainActivity, CompanionActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_MULTIPLE_TASK)
                .putExtra(CompanionActivity.EXTRA_GENERATION, generation)
            if (forcedRoute == "inline") return false
            return try {
                val manager = getSystemService(ActivityManager::class.java)
                if (!manager.isActivityStartAllowedOnDisplay(this@MainActivity, displayId, intent)) return false
                startActivity(intent, ActivityOptions.makeBasic().setLaunchDisplayId(displayId).toBundle())
                Log.i(TAG, "companion route=activity display=$displayId")
                // The system can drop a launch right after an app switch without telling us.
                handler.postDelayed({
                    if (companionActivity == null) router.activityUnavailable(generation)
                }, 2000)
                true
            } catch (e: SecurityException) {
                Log.w(TAG, "companion launch denied", e); false
            } catch (e: ActivityNotFoundException) {
                Log.w(TAG, "companion activity missing", e); false
            } catch (e: IllegalArgumentException) {
                Log.w(TAG, "companion display vanished during launch", e); false
            }
        }

        override fun inline() {
            val view = ScreenView(this@MainActivity, NativeBridge.BOTTOM)
            inlineView = view
            stack.addView(view, LinearLayout.LayoutParams(-1, 0, 1f))
            Log.i(TAG, "companion route=inline")
        }
    })

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        current = this
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) forcedRoute = intent.getStringExtra("companion")
        window.immerse()
        stack = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        stack.addView(ScreenView(this, NativeBridge.TOP), LinearLayout.LayoutParams(-1, 0, 1f))
        status = TextView(this).apply {
            setTextColor(Color.WHITE); textSize = 14f; setPadding(16, 10, 16, 10)
            setBackgroundColor(0xB0000000.toInt()); visibility = View.GONE
        }
        setContentView(FrameLayout(this).apply {
            addView(stack, FrameLayout.LayoutParams(-1, -1))
            addView(status, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.START))
        })
        NativeBridge.onMessage = { text -> status.text = text; status.visibility = if (text.isEmpty()) View.GONE else View.VISIBLE }
        NativeBridge.onGameExit = { finishAndRemoveTask() }

        displays = getSystemService(DisplayManager::class.java)
        displays.registerDisplayListener(this, handler)
        getSystemService(InputManager::class.java).registerInputDeviceListener(this, handler)
        startEngine()
    }

    /** Extracts the engine files off the main thread (the status line says so), then starts the engine thread. */
    private fun startEngine() {
        NativeBridge.showMessage("Preparing game files...")
        thread(name = "AssetExtract") {
            Assets.extractIfNeeded(applicationContext)
            handler.post {
                if (isFinishing || isDestroyed) return@post
                NativeBridge.showMessage("")
                val external = (getExternalFilesDir(null) ?: filesDir).resolve("emerald3ds").apply { mkdirs() }
                if (!NativeBridge.nativeStart(filesDir.path + "/", external.path + "/")) NativeBridge.showMessage("The engine could not start.")
            }
        }
    }

    override fun onStart() {
        super.onStart()
        started = true
        companionLost = false
        NativeBridge.nativeResume()
        refreshDisplays()
    }

    override fun onStop() {
        started = false
        NativeBridge.nativePause()
        NativeBridge.nativeResetInput()
        router.stop() // closes the bottom window; its Surface is reported lost before the engine resumes
        super.onStop()
    }

    override fun onDestroy() {
        displays.unregisterDisplayListener(this)
        getSystemService(InputManager::class.java).unregisterInputDeviceListener(this)
        NativeBridge.onMessage = null
        NativeBridge.onGameExit = null
        if (current === this) current = null
        if (isFinishing) NativeBridge.nativeStop()
        super.onDestroy()
    }

    /** Re-evaluates which display shows the bottom screen (start, hot-plug, a lost Presentation). */
    private fun refreshDisplays() {
        if (!started || companionLost) return
        val own = activityDisplay
        RefreshPolicy.request(window, own, 60)
        presentation?.let { RefreshPolicy.request(it.window!!, it.display, 60) }
        companionActivity?.let { RefreshPolicy.request(it.window, it.activityDisplay, 60) }
        val usable = displays.displays
            .filter { it.isValid && it.state == Display.STATE_ON && it.flags and Display.FLAG_PRIVATE == 0 }
            .map { it.displayId }
        router.reconcile(own?.displayId, usable)
        Log.i(TAG, "displays top=${own?.displayId} ${own?.mode?.physicalWidth}x${own?.mode?.physicalHeight} usable=$usable route=${router.mode}")
    }

    internal fun attachCompanion(activity: CompanionActivity, generation: Long): Boolean {
        if (!started || !router.acceptsActivity(activity.activityDisplay?.displayId ?: -1, generation)) return false
        companionActivity?.takeIf { it !== activity }?.finish()
        companionActivity = activity
        return true
    }

    /** The bottom display went to its launcher. The system rejects an immediate relaunch (no app switch yet), so wait
     * until the player comes back to this window. */
    internal fun companionStopped(activity: CompanionActivity) {
        handler.post {
            if (companionActivity !== activity || !started) return@post
            router.stop()
            companionLost = true
        }
    }

    private fun restoreCompanion() {
        if (!companionLost) return
        companionLost = false
        refreshDisplays()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) restoreCompanion()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        restoreCompanion()
    }

    internal fun detachCompanion(activity: CompanionActivity) {
        if (companionActivity !== activity) return
        companionActivity = null
        router.activityUnavailable(router.generation)
    }

    override fun onDisplayAdded(displayId: Int) = refreshDisplays()
    override fun onDisplayRemoved(displayId: Int) = refreshDisplays()
    override fun onDisplayChanged(displayId: Int) = refreshDisplays()

    override fun onInputDeviceAdded(deviceId: Int) { Log.i(TAG, "input device added $deviceId") }
    override fun onInputDeviceChanged(deviceId: Int) {}
    override fun onInputDeviceRemoved(deviceId: Int) {
        Log.i(TAG, "input device removed $deviceId")
        NativeBridge.nativeResetInput() // a button held on a pulled controller would stay down
    }

    // The companion window forwards its keys here: a touch on the other display moves focus to it.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action != KeyEvent.ACTION_MULTIPLE && NativeBridge.nativeKey(event.keyCode, event.action == KeyEvent.ACTION_DOWN)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_JOYSTICK != InputDevice.SOURCE_JOYSTICK || event.actionMasked != MotionEvent.ACTION_MOVE)
            return super.onGenericMotionEvent(event)
        fun axis(a: Int) = event.getAxisValue(a)
        fun larger(a: Float, b: Float) = if (abs(a) >= abs(b)) a else b
        NativeBridge.nativeMotion(
            axis(MotionEvent.AXIS_X), axis(MotionEvent.AXIS_Y),
            larger(axis(MotionEvent.AXIS_Z), axis(MotionEvent.AXIS_RX)), larger(axis(MotionEvent.AXIS_RZ), axis(MotionEvent.AXIS_RY)),
            axis(MotionEvent.AXIS_HAT_X), axis(MotionEvent.AXIS_HAT_Y),
            maxOf(axis(MotionEvent.AXIS_LTRIGGER), axis(MotionEvent.AXIS_BRAKE)), maxOf(axis(MotionEvent.AXIS_RTRIGGER), axis(MotionEvent.AXIS_GAS)),
        )
        return true
    }

    companion object {
        /** The live Activity, for the companion Activity to find its owner. */
        @Volatile var current: MainActivity? = null; private set
    }
}
