package com.emerald3ds.thor

/**
 * Decides where the bottom screen lives: a Presentation on another display, else a second Activity launched
 * there, else inline below the top view (a phone, or a failed launch). Main thread only. Every route change
 * starts with stop(), which also bumps `generation` so callbacks from a window being torn down are ignored.
 */
class CompanionRouter(private val host: Host) {
    enum class Mode { NONE, PRESENTATION, ACTIVITY, INLINE }
    interface Host {
        fun close()
        fun presentation(displayId: Int): Boolean
        fun activity(displayId: Int, generation: Long): Boolean
        fun inline()
    }

    var mode = Mode.NONE; private set
    var generation = 0L; private set
    private var target: Int? = null

    /** `displays` are the usable secondary candidates; the lowest id other than `primary` wins. */
    fun reconcile(primary: Int?, displays: List<Int>) {
        val next = displays.filter { it != primary }.minOrNull()
        if (mode != Mode.NONE && next == target) return
        stop()
        target = next
        mode = when {
            next == null -> { host.inline(); Mode.INLINE }
            host.presentation(next) -> Mode.PRESENTATION
            host.activity(next, generation) -> Mode.ACTIVITY
            else -> { host.inline(); Mode.INLINE }
        }
    }

    fun acceptsActivity(displayId: Int, token: Long) = mode == Mode.ACTIVITY && target == displayId && generation == token

    /** The second Activity went away on its own: fall back to inline. */
    fun activityUnavailable(token: Long) {
        if (mode != Mode.ACTIVITY || generation != token) return
        stop()
        mode = Mode.INLINE
        host.inline()
    }

    fun stop() {
        generation++; mode = Mode.NONE; target = null
        host.close()
    }
}
