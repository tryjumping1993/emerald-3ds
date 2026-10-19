#ifndef ANDROID_HOST_H
#define ANDROID_HOST_H
/*
 * The boundary between the Android host (the Java shell, JNI and EGL: android_host.c) and
 * the engine's backend units (android_platform.c, android_video.c, android_input.c, ...).
 *
 * The host owns everything Android: the two displays' Surfaces, one EGL context made
 * current on the ENGINE thread, the controller and touch queues, the app lifecycle. The
 * backend never includes an Android or JNI header; it sees only this file.
 *
 * Threads: the Java shell calls the AndroidHost_Jni* entry points from the main thread.
 * AndroidHost_Start creates the engine thread, which runs AndroidEngine_Main (defined by
 * the backend, or by the host self-test). Every other AndroidHost_* function below is
 * called on the engine thread only.
 */
#include <stdbool.h>
#include <stdint.h>

/* ── Backend, called by the host ─────────────────────────────────────────── */
/* The engine thread's body. Returns when the game exits; the host then reports it to Java. */
int AndroidEngine_Main(void);

/* ── Host, called by the backend on the engine thread ────────────────────── */

/* Directories the shell gave at start: internal files (the extracted engine files live in
 * <internal>/romfs/) and the external app files directory (the data pack, saves, logs).
 * Both end with '/' and exist. */
const char *AndroidHost_InternalDir(void);
const char *AndroidHost_ExternalDir(void);

/* True once the shell asked the app to stop (Activity finishing, or Stop). Poll once a frame. */
bool AndroidHost_ExitRequested(void);
/* The app is in the background: the engine should stop drawing and mixing. Blocks the
 * calling thread while paused (returns false if an exit was requested meanwhile). */
bool AndroidHost_WaitWhilePaused(void);
/* The game ended by itself (its own exit path): tells the shell to finish the Activity. */
void AndroidHost_NotifyGameExit(void);
/* Shows UTF-8 text in the shell's status overlay ("no game data", a fatal error); "" hides it.
 * Safe from any thread. */
void AndroidHost_ShowMessage(const char *text);

/*
 * Frames. Both screens take tightly packed RGBA8888 (byte order R, G, B, A in memory; the
 * alpha is ignored), row-major, first row at the TOP. The host uploads it to a texture and
 * draws it into the screen's surface, letterboxed (aspect kept, centred, black bars) with
 * nearest filtering. Nothing is drawn, and nothing blocks, while that screen has no surface.
 *
 * PresentTop swaps the top surface (vsync paced: the Thor's panel runs 120 Hz, the game
 * 60, so the host asks for 60 where the platform allows it, or swaps every second vsync).
 * PresentBottom swaps the bottom surface only when called (the engine calls it when the
 * canvas changed), with swap interval 0 so it never waits for its own vsync.
 *
 * Both may be called with a new size at any time (the soft renderer's 400x240 today, a
 * larger frame later); the host recreates its texture on a change. A NULL pixels pointer
 * with a size just redraws the last frame (a surface that came back).
 */
void AndroidHost_PresentTop(const uint32_t *rgba, int width, int height);
void AndroidHost_PresentBottom(const uint32_t *rgba, int width, int height);
/* The size of the surface a screen has now, in pixels; 0 x 0 without one. */
void AndroidHost_TopSurfaceSize(int *width, int *height);
void AndroidHost_BottomSurfaceSize(int *width, int *height);

/*
 * Input, as Android reports it. The host keeps the state; the backend samples it once a
 * frame and maps it to the game's keys (android_input.c).
 */
enum
{
    ANDROID_BTN_A = 1 << 0, ANDROID_BTN_B = 1 << 1, ANDROID_BTN_X = 1 << 2, ANDROID_BTN_Y = 1 << 3,
    ANDROID_BTN_DPAD_UP = 1 << 4, ANDROID_BTN_DPAD_DOWN = 1 << 5,
    ANDROID_BTN_DPAD_LEFT = 1 << 6, ANDROID_BTN_DPAD_RIGHT = 1 << 7,
    ANDROID_BTN_L1 = 1 << 8, ANDROID_BTN_R1 = 1 << 9, ANDROID_BTN_L2 = 1 << 10, ANDROID_BTN_R2 = 1 << 11,
    ANDROID_BTN_START = 1 << 12, ANDROID_BTN_SELECT = 1 << 13,
    ANDROID_BTN_THUMBL = 1 << 14, ANDROID_BTN_THUMBR = 1 << 15,
    ANDROID_BTN_MODE = 1 << 16,   /* the Home / mode button, if the device reports it */
    ANDROID_BTN_BACK = 1 << 17    /* the system Back key (KEYCODE_BACK), not a controller button */
};

typedef struct
{
    uint32_t buttons;            /* ANDROID_BTN_*; the hat (AXIS_HAT_X/Y) is folded into DPAD */
    int16_t leftX, leftY;        /* left stick, -32767..32767, +Y down */
    int16_t rightX, rightY;      /* right stick */
    uint8_t leftTrigger, rightTrigger; /* analog triggers, 0..255 (L2/R2 also set their button past 50%) */
    /* The bottom screen's touch, already mapped through its letterbox to the bottom canvas
     * of the last PresentBottom (0..width-1, 0..height-1). Touches outside the picture are
     * clamped to its edge. */
    bool touchActive;
    int16_t touchX, touchY;
    /* Counts up with every key, motion or touch event, so the engine can tell input happened. */
    uint32_t serial;
} AndroidInputState;

void AndroidHost_GetInput(AndroidInputState *state);

/* ── Host, called by the Java shell through JNI (main thread) ─────────────── */
/* android_host.c exports the JNI names for the Kotlin object NativeBridge; the C names are
 * documented here so the Kotlin side and the C side agree. */
/*
 *  nativeStart(internalDir: String, externalDir: String): Boolean   starts the engine thread (once)
 *  nativeSetSurface(screen: Int, surface: Surface?)                  screen 0 = top, 1 = bottom; null = lost
 *  nativeSurfaceSize(screen: Int, width: Int, height: Int)           the surface changed size
 *  nativePause() / nativeResume()                                    the app left / came back to the foreground
 *  nativeStop()                                                      the Activity is finishing: asks the engine to exit, joins it
 *  nativeKey(keyCode: Int, down: Boolean): Boolean                   android.view.KeyEvent codes; true if the key is one the host maps
 *                                                                    (the shell then consumes the event)
 *  nativeResetInput()                                                releases every button, stick and touch (controller unplugged, window lost)
 *  nativeMotion(lx, ly, rx, ry, hatX, hatY, lt, rt: Float)           controller axes (AXIS_X/Y/Z/RZ/HAT_X/HAT_Y/LTRIGGER/RTRIGGER)
 *  nativeTouch(action: Int, x: Float, y: Float, viewW: Int, viewH: Int)   MotionEvent action (DOWN/MOVE/UP/CANCEL) on the bottom view
 */

#endif
