package com.emerald3ds.thor

import android.app.Activity
import android.app.AlertDialog
import android.view.KeyEvent

/** Physical Android button -> game button. Stored separately from the game's settings and save. */
class Controls(private val owner: Activity) {
    private val prefs = owner.getSharedPreferences("controller", 0)
    private val actions = linkedMapOf(
        KeyEvent.KEYCODE_BUTTON_A to "A", KeyEvent.KEYCODE_BUTTON_B to "B",
        KeyEvent.KEYCODE_BUTTON_X to "X", KeyEvent.KEYCODE_BUTTON_Y to "Y",
        KeyEvent.KEYCODE_DPAD_UP to "Up", KeyEvent.KEYCODE_DPAD_DOWN to "Down",
        KeyEvent.KEYCODE_DPAD_LEFT to "Left", KeyEvent.KEYCODE_DPAD_RIGHT to "Right",
        KeyEvent.KEYCODE_BUTTON_L1 to "L", KeyEvent.KEYCODE_BUTTON_R1 to "R",
        KeyEvent.KEYCODE_BUTTON_L2 to "ZL", KeyEvent.KEYCODE_BUTTON_R2 to "ZR",
        KeyEvent.KEYCODE_BUTTON_START to "Start", KeyEvent.KEYCODE_BUTTON_SELECT to "Select",
    )
    private val held = mutableMapOf<Int, Int>()
    private val keys = mutableSetOf<Int>()
    private val axes = mutableSetOf<Int>()
    private var dialog: AlertDialog? = null

    fun key(code: Int, down: Boolean): Boolean {
        if (down) keys.add(code) else keys.remove(code)
        return update(code)
    }

    private fun update(code: Int): Boolean {
        val down = code in keys || code in axes
        val mapped = if (down) held.getOrPut(code) { prefs.getInt(code.toString(), code) }
                     else held.remove(code) ?: prefs.getInt(code.toString(), code)
        // More than one physical input can resolve to the same logical button.
        return NativeBridge.nativeKey(mapped, down || mapped in held.values)
    }

    /** Hats and analogue triggers use the same mappings as their key-event equivalents. */
    fun motion(hx: Float, hy: Float, lt: Float, rt: Float) {
        val values = mapOf(KeyEvent.KEYCODE_DPAD_LEFT to (hx < -0.5f), KeyEvent.KEYCODE_DPAD_RIGHT to (hx > 0.5f),
            KeyEvent.KEYCODE_DPAD_UP to (hy < -0.5f), KeyEvent.KEYCODE_DPAD_DOWN to (hy > 0.5f),
            KeyEvent.KEYCODE_BUTTON_L2 to (lt > 0.5f), KeyEvent.KEYCODE_BUTTON_R2 to (rt > 0.5f))
        for ((code, down) in values) {
            if (down == (code in axes)) continue
            if (down) axes.add(code) else axes.remove(code)
            update(code)
        }
    }

    fun reset() { held.clear(); keys.clear(); axes.clear(); NativeBridge.nativeResetInput() }
    fun close() { dialog?.dismiss(); dialog = null; reset() }

    fun show() {
        if (dialog?.isShowing == true) return
        reset()
        val codes = actions.keys.toList()
        val rows = codes.map { code ->
            val physical = (actions.keys + prefs.all.keys.mapNotNull { it.toIntOrNull() })
                .distinct().filter { prefs.getInt(it.toString(), it) == code }
            "${actions[code]}: ${physical.joinToString { KeyEvent.keyCodeToString(it).removePrefix("KEYCODE_") }}"
        }.toTypedArray()
        dialog = AlertDialog.Builder(owner).setTitle("Controller buttons")
            .setItems(rows) { _, i -> capture(codes[i]) }
            .setNeutralButton("Reset defaults") { _, _ -> prefs.edit().clear().apply(); showAgain() }
            .setNegativeButton("Done", null)
            .create().also { it.show() }
    }

    private fun showAgain() { dialog = null; show() }

    private fun capture(action: Int) {
        val prompt = AlertDialog.Builder(owner).setTitle("Map ${actions[action]}")
            .setMessage("Press a controller button. Back cancels. Sticks keep their movement and camera axes.")
            .setNegativeButton("Cancel", null).create()
        dialog = prompt
        prompt.setOnKeyListener { _, code, event ->
            if (code == KeyEvent.KEYCODE_BACK) false
            else if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0 &&
                (KeyEvent.isGamepadButton(code) || code in actions)) {
                // Swap the displaced action onto the previous physical button to keep every action reachable.
                val previous = (actions.keys + prefs.all.keys.mapNotNull { it.toIntOrNull() })
                    .distinct().firstOrNull { prefs.getInt(it.toString(), it) == action }
                val displaced = prefs.getInt(code.toString(), code)
                prefs.edit().apply {
                    putInt(code.toString(), action)
                    if (previous != null && previous != code) putInt(previous.toString(), displaced)
                }.apply()
                prompt.dismiss()
                owner.window.decorView.post { showAgain() }
                true
            } else true
        }
        prompt.show()
    }
}
