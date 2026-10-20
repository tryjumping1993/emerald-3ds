package com.emerald3ds.thor

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.Log
import android.view.Gravity
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import kotlin.concurrent.thread

/** Separate process: cancelling or an interrupted build cannot take down the game or its save. */
class RomImportActivity : Activity() {
    private val handler = Handler(Looper.getMainLooper())
    private lateinit var message: TextView
    private lateinit var bar: ProgressBar
    private lateinit var choose: Button
    private var building = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.immerse()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER; setPadding(40, 24, 40, 24)
        }
        layout.addView(TextView(this).apply { text = "Set up Emerald"; textSize = 26f })
        message = TextView(this).apply {
            text = "Choose your clean Emerald ROM. This device will prepare the game data. Your save and settings are kept."
            textSize = 18f; gravity = Gravity.CENTER; setPadding(0, 20, 0, 20)
        }
        layout.addView(message)
        bar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply { max = 100 }
        layout.addView(bar, LinearLayout.LayoutParams(-1, -2))
        choose = Button(this).apply { text = "Choose Emerald ROM"; setOnClickListener { pickRom() } }
        layout.addView(choose)
        layout.addView(Button(this).apply { text = "Cancel"; setOnClickListener { cancel() } })
        setContentView(layout)
        // Reuse a persisted document grant on app updates when the provider still offers it.
        val saved = getSharedPreferences("rom_import", MODE_PRIVATE).getString("uri", null)
        if (intent.getBooleanExtra("reuse", false) && saved != null) build(Uri.parse(saved))
    }

    @Suppress("DEPRECATION")
    private fun pickRom() {
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE); type = "*/*"
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
        }, 1)
    }

    @Deprecated("Uses the result API supported on all target versions")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 1 || resultCode != RESULT_OK || building) return
        val uri = data?.data ?: return
        runCatching { contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
        build(uri)
    }

    private fun build(uri: Uri) {
        building = true; choose.isEnabled = false
        thread(name = "RomBuild") {
            val failure = runCatching {
                RomBuilder.build(applicationContext, uri) { fraction, text ->
                    handler.post { bar.progress = (fraction * 100).toInt(); message.text = text }
                }
            }.exceptionOrNull()
            if (failure != null) Log.e(TAG, "ROM build failed", failure)
            handler.post {
                if (isFinishing || isDestroyed) return@post
                building = false; choose.isEnabled = true
                if (failure == null) {
                    getSharedPreferences("rom_import", MODE_PRIVATE).edit().putString("uri", uri.toString()).apply()
                    setResult(RESULT_OK); finish()
                } else {
                    message.text = if (failure is IllegalArgumentException || failure is IllegalStateException)
                        failure.message else "Game data could not be prepared. Please choose your ROM again."
                }
            }
        }
    }

    private fun cancel() {
        setResult(RESULT_CANCELED); finish()
        // Only the isolated builder process ends. The previous pack remains installed.
        if (building) Process.killProcess(Process.myPid())
    }

    @Deprecated("Back cancels setup")
    override fun onBackPressed() { cancel() }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing && !building) {
            // Release the Python heap after the result reaches the game process.
            handler.postDelayed({ Process.killProcess(Process.myPid()) }, 1500)
        }
    }
}
