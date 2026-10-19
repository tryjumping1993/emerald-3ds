/*
 * The Android host: JNI entry points for the Java shell, the engine thread, one EGL context
 * with a window surface per screen, the frame draw and the controller/touch state.
 * Contract: include/android_host.h.
 *
 * Threading rules that shape the file:
 *  - The main thread only ever takes g.lock for a few instructions and never touches EGL or GL:
 *    a Surface from the shell lands in a pending slot, and the engine thread applies it at its
 *    next Present / TopSurfaceSize / WaitWhilePaused. A surface can therefore arrive late, twice,
 *    or be replaced before it was ever used; the slot just keeps the newest.
 *  - The ANativeWindow reference keeps the buffer queue memory valid, so a surface the shell
 *    destroyed while a frame is in flight fails its swap (and is dropped) instead of crashing.
 *  - The engine thread owns the EGLContext, the EGLSurfaces and every GL object.
 */
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "android_host.h"

#define TAG "Emerald3DS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

enum { SCREEN_TOP, SCREEN_BOTTOM, SCREEN_COUNT };
static const char *const kScreenName[SCREEN_COUNT] = { "top", "bottom" };

/* ── State shared with the main thread ───────────────────────────────────── */

static struct
{
    JavaVM *vm;
    jclass bridge;
    jmethodID showMessage, gameExited;

    pthread_mutex_t lock;
    pthread_cond_t cond; /* wakes the engine (resume, exit, new surface) and nativeStop (engine done) */
    struct { ANativeWindow *window; bool dirty; } pending[SCREEN_COUNT];
    bool started, finished, paused, exitRequested, gameExitReported;
    pthread_t thread;
    char internalDir[1024], externalDir[1024];
} g = { .lock = PTHREAD_MUTEX_INITIALIZER };

static struct
{
    pthread_mutex_t lock;
    uint32_t keys;               /* ANDROID_BTN_* from key events */
    /* Pressed and released between two samples (a quick tap, `adb shell input`): the engine polls once a
     * frame and must still see it once. Cleared by GetInput. */
    uint32_t latchedKeys;
    bool latchedTouch;
    int8_t hatX, hatY;
    uint8_t leftTrigger, rightTrigger;
    int16_t leftX, leftY, rightX, rightY;
    bool touchActive;
    int16_t touchX, touchY;
    uint32_t serial;
    /* The letterbox the last bottom draw used, in surface pixels, to invert it for touches. */
    struct { bool valid; int surfaceW, surfaceH, x, y, w, h, canvasW, canvasH; } letterbox;
} in = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ── Java callbacks ──────────────────────────────────────────────────────── */

static void callBridge(jmethodID method, const char *text)
{
    JNIEnv *env;
    bool attached = false;
    if ((*g.vm)->GetEnv(g.vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
    {
        if ((*g.vm)->AttachCurrentThread(g.vm, &env, NULL) != JNI_OK)
            return;
        attached = true;
    }
    if (text != NULL)
    {
        jstring s = (*env)->NewStringUTF(env, text);
        (*env)->CallStaticVoidMethod(env, g.bridge, method, s);
        (*env)->DeleteLocalRef(env, s);
    }
    else
        (*env)->CallStaticVoidMethod(env, g.bridge, method);
    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    if (attached)
        (*g.vm)->DetachCurrentThread(g.vm);
}

void AndroidHost_ShowMessage(const char *text)
{
    callBridge(g.showMessage, text != NULL ? text : "");
}

void AndroidHost_NotifyGameExit(void)
{
    pthread_mutex_lock(&g.lock);
    bool report = !g.gameExitReported;
    g.gameExitReported = true;
    pthread_mutex_unlock(&g.lock);
    if (report)
        callBridge(g.gameExited, NULL);
}

/* ── GL / EGL, engine thread only ────────────────────────────────────────── */

typedef int32_t (*SetFrameRateFn)(ANativeWindow *, float, int8_t);

typedef struct
{
    ANativeWindow *window; /* reference held while we own it */
    EGLSurface surface;
    GLuint texture;
    int texW, texH;
    int surfaceW, surfaceH;
} Screen;

static Screen s_screen[SCREEN_COUNT];
static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLConfig s_config;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_idleSurface = EGL_NO_SURFACE; /* keeps the context current while no screen has a surface */
static GLuint s_program;
static SetFrameRateFn s_setFrameRate;
static int s_refreshHint = 120;
static bool s_forceSixty;
static unsigned s_presentsPerTick = 1;
static bool s_rebuilding;
/* The bottom canvas is only presented when it changes, so the host keeps the last one to
 * redraw it onto a surface that arrives later or after the context was lost. The top frame is
 * re-presented every frame by the engine and is not kept. */
static uint32_t *s_bottomCache;
static int s_bottomCacheW, s_bottomCacheH;

static const char kVertexShader[] =
    "#version 300 es\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n" /* triangle strip over the viewport */
    "    uv = vec2(p.x, 1.0 - p.y);\n"                                      /* row 0 of the upload is the top */
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char kFragmentShader[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "void main() { color = vec4(texture(tex, uv).rgb, 1.0); }\n"; /* the frames' alpha is ignored */

static GLuint compileShader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool buildProgram(void)
{
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vs == 0 || fs == 0)
        return false;
    s_program = glCreateProgram();
    glAttachShader(s_program, vs);
    glAttachShader(s_program, fs);
    glLinkProgram(s_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(s_program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        LOGE("program link failed");
        return false;
    }
    glUseProgram(s_program);
    glUniform1i(glGetUniformLocation(s_program, "tex"), 0);
    return true;
}

static bool createContext(void)
{
    static const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    static const EGLint pbufferAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    s_context = eglCreateContext(s_display, s_config, EGL_NO_CONTEXT, contextAttribs);
    s_idleSurface = eglCreatePbufferSurface(s_display, s_config, pbufferAttribs);
    if (s_context == EGL_NO_CONTEXT || s_idleSurface == EGL_NO_SURFACE
        || !eglMakeCurrent(s_display, s_idleSurface, s_idleSurface, s_context))
    {
        LOGE("EGL context creation failed (0x%x)", eglGetError());
        return false;
    }
    return buildProgram();
}

static bool initEgl(void)
{
    static const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint count = 0;
    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(s_display, NULL, NULL) || !eglChooseConfig(s_display, configAttribs, &s_config, 1, &count) || count < 1)
    {
        LOGE("EGL init failed (0x%x)", eglGetError());
        return false;
    }
    /* API 30+: lets the 120 Hz top panel run the 60 fps game at 60 Hz. Looked up at run time because
     * the app's minSdk is 29 (RTLD_DEFAULT does not see libandroid). Without it the top surface just
     * follows the panel's refresh rate; noteSwap times it and the engine presents twice a tick on 120 Hz. */
    void *libandroid = dlopen("libandroid.so", RTLD_NOW);
    s_setFrameRate = libandroid != NULL ? (SetFrameRateFn)dlsym(libandroid, "ANativeWindow_setFrameRate") : NULL;
    if (!createContext())
        return false;
    LOGI("GL: %s | %s | setFrameRate %s", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION),
         s_setFrameRate != NULL ? "available" : "missing");
    return true;
}

static void shutdownEgl(void)
{
    if (s_display == EGL_NO_DISPLAY)
        return;
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    for (int i = 0; i < SCREEN_COUNT; i++)
    {
        if (s_screen[i].surface != EGL_NO_SURFACE)
            eglDestroySurface(s_display, s_screen[i].surface);
        if (s_screen[i].window != NULL)
            ANativeWindow_release(s_screen[i].window);
        memset(&s_screen[i], 0, sizeof s_screen[i]);
    }
    if (s_idleSurface != EGL_NO_SURFACE)
        eglDestroySurface(s_display, s_idleSurface);
    if (s_context != EGL_NO_CONTEXT)
        eglDestroyContext(s_display, s_context);
    eglTerminate(s_display);
    s_display = EGL_NO_DISPLAY;
    s_context = EGL_NO_CONTEXT;
    s_idleSurface = EGL_NO_SURFACE;
    free(s_bottomCache);
    s_bottomCache = NULL;
    s_bottomCacheW = s_bottomCacheH = 0;
}

static void releaseSurface(Screen *s)
{
    if (s->surface != EGL_NO_SURFACE)
    {
        /* Not current when destroyed, or EGL defers the destruction and the window stays connected. */
        eglMakeCurrent(s_display, s_idleSurface, s_idleSurface, s_context);
        eglDestroySurface(s_display, s->surface);
        s->surface = EGL_NO_SURFACE;
    }
    if (s->window != NULL)
    {
        ANativeWindow_release(s->window);
        s->window = NULL;
    }
    s->surfaceW = s->surfaceH = 0;
}

static void uploadFrame(Screen *s, const uint32_t *rgba, int w, int h)
{
    if (s->texture == 0)
    {
        glGenTextures(1, &s->texture);
        glBindTexture(GL_TEXTURE_2D, s->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, s->texture);
    if (w != s->texW || h != s->texH)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        s->texW = w;
        s->texH = h;
    }
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

static void rebuildContext(void);

/* A swap or make-current failed: the context was lost (power management, driver reset) or the
 * window is gone (the shell destroyed it; its replacement arrives through the pending slot). */
static void surfaceFailed(int index, EGLint error)
{
    if (error == EGL_CONTEXT_LOST && !s_rebuilding)
        rebuildContext();
    else
    {
        LOGW("%s surface dropped (EGL 0x%x)", kScreenName[index], error);
        releaseSurface(&s_screen[index]);
    }
}

/* Draws the screen's texture letterboxed, uploading `rgba` first when given. */
static void drawScreen(int index, const uint32_t *rgba, int w, int h)
{
    Screen *s = &s_screen[index];
    if (!eglMakeCurrent(s_display, s->surface, s->surface, s_context))
    {
        surfaceFailed(index, eglGetError());
        return;
    }
    if (rgba != NULL)
        uploadFrame(s, rgba, w, h);
    EGLint sw = 0, sh = 0;
    eglQuerySurface(s_display, s->surface, EGL_WIDTH, &sw);
    eglQuerySurface(s_display, s->surface, EGL_HEIGHT, &sh);
    s->surfaceW = sw;
    s->surfaceH = sh;

    glViewport(0, 0, sw, sh);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (s->texture != 0 && sw > 0 && sh > 0)
    {
        int dw, dh;
        if ((int64_t)sw * s->texH <= (int64_t)sh * s->texW)
        {
            dw = sw;
            dh = (int)((int64_t)s->texH * sw / s->texW);
        }
        else
        {
            dh = sh;
            dw = (int)((int64_t)s->texW * sh / s->texH);
        }
        int dx = (sw - dw) / 2, dy = (sh - dh) / 2;
        glViewport(dx, sh - dy - dh, dw, dh); /* GL's origin is bottom-left */
        glUseProgram(s_program);
        glBindTexture(GL_TEXTURE_2D, s->texture);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (index == SCREEN_BOTTOM)
        {
            pthread_mutex_lock(&in.lock);
            in.letterbox.valid = true;
            in.letterbox.surfaceW = sw;
            in.letterbox.surfaceH = sh;
            in.letterbox.x = dx;
            in.letterbox.y = dy;
            in.letterbox.w = dw;
            in.letterbox.h = dh;
            in.letterbox.canvasW = s->texW;
            in.letterbox.canvasH = s->texH;
            pthread_mutex_unlock(&in.lock);
        }
    }
    if (!eglSwapBuffers(s_display, s->surface))
        surfaceFailed(index, eglGetError());
}

static void attachSurface(int index)
{
    Screen *s = &s_screen[index];
    s->surface = eglCreateWindowSurface(s_display, s_config, s->window, NULL);
    if (s->surface == EGL_NO_SURFACE)
    {
        LOGE("%s: eglCreateWindowSurface failed (0x%x)", kScreenName[index], eglGetError());
        releaseSurface(s);
        return;
    }
    /* Top follows vsync; bottom is presented only on change and must never wait for its own vsync. */
    eglMakeCurrent(s_display, s->surface, s->surface, s_context);
    eglSwapInterval(s_display, index == SCREEN_TOP ? 1 : 0);
    if (index == SCREEN_TOP && s_setFrameRate != NULL)
        s_setFrameRate(s->window, s_forceSixty ? 60.f : (float)s_refreshHint, 0 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT */);
    EGLint w = 0, h = 0;
    eglQuerySurface(s_display, s->surface, EGL_WIDTH, &w);
    eglQuerySurface(s_display, s->surface, EGL_HEIGHT, &h);
    s->surfaceW = w;
    s->surfaceH = h;
    LOGI("%s surface created %dx%d", kScreenName[index], w, h);
}

static void redrawBottomFromCache(void)
{
    if (s_screen[SCREEN_BOTTOM].surface != EGL_NO_SURFACE && s_bottomCache != NULL)
        drawScreen(SCREEN_BOTTOM, s_bottomCache, s_bottomCacheW, s_bottomCacheH);
}

/* All GL objects died with the context: make new ones and put the surfaces back. */
static void rebuildContext(void)
{
    LOGW("EGL context lost, rebuilding");
    s_rebuilding = true;
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    for (int i = 0; i < SCREEN_COUNT; i++)
    {
        if (s_screen[i].surface != EGL_NO_SURFACE)
            eglDestroySurface(s_display, s_screen[i].surface);
        s_screen[i].surface = EGL_NO_SURFACE;
        s_screen[i].texture = 0;
        s_screen[i].texW = s_screen[i].texH = 0;
    }
    eglDestroySurface(s_display, s_idleSurface);
    eglDestroyContext(s_display, s_context);
    s_idleSurface = EGL_NO_SURFACE;
    s_context = EGL_NO_CONTEXT;
    if (createContext())
    {
        for (int i = 0; i < SCREEN_COUNT; i++)
            if (s_screen[i].window != NULL)
                attachSurface(i);
        redrawBottomFromCache();
    }
    else
        for (int i = 0; i < SCREEN_COUNT; i++)
            if (s_screen[i].window != NULL)
            {
                ANativeWindow_release(s_screen[i].window);
                s_screen[i].window = NULL;
            }
    s_rebuilding = false;
}

/* Takes the surfaces the shell posted since the last call. */
static void applyPending(void)
{
    ANativeWindow *incoming[SCREEN_COUNT];
    bool changed[SCREEN_COUNT];
    pthread_mutex_lock(&g.lock);
    for (int i = 0; i < SCREEN_COUNT; i++)
    {
        changed[i] = g.pending[i].dirty;
        incoming[i] = g.pending[i].window;
        g.pending[i].dirty = false;
        g.pending[i].window = NULL;
    }
    pthread_mutex_unlock(&g.lock);

    for (int i = 0; i < SCREEN_COUNT; i++)
    {
        if (!changed[i])
            continue;
        if (incoming[i] != NULL && incoming[i] == s_screen[i].window)
        {
            ANativeWindow_release(incoming[i]); /* the same window announced twice */
            continue;
        }
        if (s_context == EGL_NO_CONTEXT) /* the context could not be rebuilt: nothing can be shown */
        {
            if (incoming[i] != NULL)
                ANativeWindow_release(incoming[i]);
            continue;
        }
        if (s_screen[i].window != NULL)
            LOGI("%s surface released", kScreenName[i]);
        releaseSurface(&s_screen[i]);
        s_screen[i].window = incoming[i];
        if (incoming[i] != NULL)
        {
            attachSurface(i);
            if (i == SCREEN_BOTTOM)
                redrawBottomFromCache();
        }
    }
}

static void present(int index, const uint32_t *rgba, int w, int h)
{
    applyPending();
    if (rgba != NULL && (w <= 0 || h <= 0))
        return;
    if (index == SCREEN_BOTTOM && rgba != NULL)
    {
        size_t bytes = (size_t)w * h * 4;
        if (w != s_bottomCacheW || h != s_bottomCacheH)
        {
            free(s_bottomCache);
            s_bottomCache = malloc(bytes);
            s_bottomCacheW = s_bottomCache != NULL ? w : 0;
            s_bottomCacheH = s_bottomCache != NULL ? h : 0;
        }
        if (s_bottomCache != NULL)
            memcpy(s_bottomCache, rgba, bytes);
    }
    if (s_screen[index].surface == EGL_NO_SURFACE)
        return;
    drawScreen(index, rgba, w, h);
}

void AndroidHost_GlMakeCurrent(void)
{
    applyPending();
    Screen *s = &s_screen[SCREEN_TOP];
    if (s->surface != EGL_NO_SURFACE && eglMakeCurrent(s_display, s->surface, s->surface, s_context))
        return;
    eglMakeCurrent(s_display, s_idleSurface, s_idleSurface, s_context);
}

bool AndroidHost_TopBegin(int *width, int *height)
{
    applyPending();
    Screen *s = &s_screen[SCREEN_TOP];
    *width = *height = 0;
    if (s->surface == EGL_NO_SURFACE)
    {
        eglMakeCurrent(s_display, s_idleSurface, s_idleSurface, s_context);
        return false;
    }
    if (!eglMakeCurrent(s_display, s->surface, s->surface, s_context))
    {
        surfaceFailed(SCREEN_TOP, eglGetError());
        return false;
    }
    EGLint w = 0, h = 0;
    eglQuerySurface(s_display, s->surface, EGL_WIDTH, &w);
    eglQuerySurface(s_display, s->surface, EGL_HEIGHT, &h);
    s->surfaceW = w;
    s->surfaceH = h;
    *width = w;
    *height = h;
    return w > 0 && h > 0;
}

static void applyHint(void);

/*
 * Each swap of the top surface waits for a vsync, so the time between two says the panel's period. The
 * least of the last few is taken for it (a late frame lengthens one, never shortens): under 11.5 ms the panel
 * is at 120 Hz and each tick wants two pictures; over, one.
 */
static void noteSwap(void)
{
    enum { WINDOW = 24 };
    static double last, intervals[WINDOW];
    static unsigned count;
    struct timespec now;
    double ms;

    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
    if (last != 0.0)
    {
        static double sum, worst;
        static unsigned seen, late;
        double d = ms - last;

        sum += d;
        if (d > worst) worst = d;
        if (d > 12.0) ++late;
        if (++seen == 600)
        {
            LOGI("top swaps: mean %.2f ms, worst %.2f ms, %u over 12 ms, %u picture(s) per tick", sum / seen, worst, late, s_presentsPerTick);
            sum = worst = 0.0; seen = late = 0;
        }
        /* The first swaps fill the buffer queue and wait for nothing: no panel is faster than 5 ms. */
        if (d >= 5.0)
            intervals[count++ % WINDOW] = d;
        if (count >= WINDOW && count % 12 == 0)
        {
            /* The lower quartile of the last swaps is the panel's period: a swap that waited for nothing but the vsync is
             * as quick as the panel allows, and a stall (a map loading) only adds slow ones, never fast. */
            double sorted[WINDOW], median;
            unsigned wanted, class;
            static unsigned agreed, lastClass;

            memcpy(sorted, intervals, sizeof(sorted));
            for (unsigned i = 1; i < WINDOW; ++i)
                for (unsigned j = i; j > 0 && sorted[j - 1] > sorted[j]; --j)
                {
                    double t = sorted[j];

                    sorted[j] = sorted[j - 1];
                    sorted[j - 1] = t;
                }
            median = sorted[WINDOW / 4];
            /* 120 Hz is two pictures a tick, 60 Hz one; any other rate (a 90 or 144 Hz panel is no multiple of the
             * game's tick) is held to 60 by asking the system for it. Three readings in a row before it changes. */
            class = median > 7.4 && median < 9.5 ? 2u : median > 15.0 && median < 18.5 ? 1u : 3u;
            agreed = class == lastClass ? agreed + 1 : 1;
            lastClass = class;
            wanted = class == 2u ? 2u : 1u;
            if (agreed >= 3 && wanted != s_presentsPerTick)
            {
                s_presentsPerTick = wanted;
                LOGI("top swaps every %.2f ms: %u picture(s) per tick", median, wanted);
            }
            if (agreed >= 3 && class == 3u && !s_forceSixty)
            {
                s_forceSixty = true;
                applyHint();
                LOGI("a %.2f ms panel is no multiple of the game's tick: refresh asked at 60 Hz", median);
            }
        }
    }
    last = ms;
}

unsigned AndroidHost_PresentsPerTick(void) { return s_presentsPerTick; }

/* The refresh rate asked of the system for the top surface, now and for every surface made after. */
static void applyHint(void)
{
    Screen *s = &s_screen[SCREEN_TOP];

    if (s->window != NULL && s_setFrameRate != NULL)
        s_setFrameRate(s->window, s_forceSixty ? 60.f : (float)s_refreshHint, 0);
}

void AndroidHost_SetRefreshHint(int hz)
{
    hz = hz >= 100 ? 120 : 60;
    if (hz == s_refreshHint)
        return;
    s_refreshHint = hz;
    applyHint();
}

void AndroidHost_TopEnd(void)
{
    Screen *s = &s_screen[SCREEN_TOP];
    if (s->surface != EGL_NO_SURFACE && !eglSwapBuffers(s_display, s->surface))
        surfaceFailed(SCREEN_TOP, eglGetError());
    else
        noteSwap();
}

void AndroidHost_PresentTop(const uint32_t *rgba, int width, int height) { present(SCREEN_TOP, rgba, width, height); }
void AndroidHost_PresentBottom(const uint32_t *rgba, int width, int height) { present(SCREEN_BOTTOM, rgba, width, height); }

static void surfaceSize(int index, int *width, int *height)
{
    applyPending();
    Screen *s = &s_screen[index];
    EGLint w = 0, h = 0;
    if (s->surface != EGL_NO_SURFACE && eglQuerySurface(s_display, s->surface, EGL_WIDTH, &w))
        eglQuerySurface(s_display, s->surface, EGL_HEIGHT, &h);
    *width = w;
    *height = h;
}

void AndroidHost_TopSurfaceSize(int *width, int *height) { surfaceSize(SCREEN_TOP, width, height); }
void AndroidHost_BottomSurfaceSize(int *width, int *height) { surfaceSize(SCREEN_BOTTOM, width, height); }

/* ── Lifecycle, engine thread ────────────────────────────────────────────── */

const char *AndroidHost_InternalDir(void) { return g.internalDir; }
const char *AndroidHost_ExternalDir(void) { return g.externalDir; }

bool AndroidHost_ExitRequested(void)
{
    pthread_mutex_lock(&g.lock);
    bool exit = g.exitRequested;
    pthread_mutex_unlock(&g.lock);
    return exit;
}

bool AndroidHost_WaitWhilePaused(void)
{
    for (;;)
    {
        applyPending(); /* a surface lost while paused must not stay held until the next resume */
        pthread_mutex_lock(&g.lock);
        bool exit = g.exitRequested, paused = g.paused;
        bool surfaceChanged = g.pending[SCREEN_TOP].dirty || g.pending[SCREEN_BOTTOM].dirty;
        if (!exit && paused && !surfaceChanged)
            pthread_cond_wait(&g.cond, &g.lock);
        pthread_mutex_unlock(&g.lock);
        if (exit)
            return false;
        if (!paused)
            return true;
    }
}

static void *engineThread(void *unused)
{
    (void)unused;
    JNIEnv *env;
    JavaVMAttachArgs attach = { JNI_VERSION_1_6, "Emerald3DS", NULL }; /* a name here; ART renames unnamed attached threads */
    (*g.vm)->AttachCurrentThread(g.vm, &env, &attach);
    LOGI("engine thread started");
    if (initEgl())
    {
        int result = AndroidEngine_Main();
        LOGI("engine returned %d", result);
    }
    else
        AndroidHost_ShowMessage("OpenGL ES 3.0 is not available on this device.");
    shutdownEgl();

    pthread_mutex_lock(&g.lock);
    bool requested = g.exitRequested;
    g.finished = true;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
    if (!requested)
        AndroidHost_NotifyGameExit();
    LOGI("engine thread finished");
    (*g.vm)->DetachCurrentThread(g.vm);
    return NULL;
}

/* ── Input ───────────────────────────────────────────────────────────────── */

static uint32_t buttonForKey(int keyCode)
{
    switch (keyCode)
    {
    case AKEYCODE_BUTTON_A: return ANDROID_BTN_A;
    case AKEYCODE_BUTTON_B: return ANDROID_BTN_B;
    case AKEYCODE_BUTTON_X: return ANDROID_BTN_X;
    case AKEYCODE_BUTTON_Y: return ANDROID_BTN_Y;
    case AKEYCODE_DPAD_UP: return ANDROID_BTN_DPAD_UP;
    case AKEYCODE_DPAD_DOWN: return ANDROID_BTN_DPAD_DOWN;
    case AKEYCODE_DPAD_LEFT: return ANDROID_BTN_DPAD_LEFT;
    case AKEYCODE_DPAD_RIGHT: return ANDROID_BTN_DPAD_RIGHT;
    case AKEYCODE_BUTTON_L1: return ANDROID_BTN_L1;
    case AKEYCODE_BUTTON_R1: return ANDROID_BTN_R1;
    case AKEYCODE_BUTTON_L2: return ANDROID_BTN_L2;
    case AKEYCODE_BUTTON_R2: return ANDROID_BTN_R2;
    case AKEYCODE_BUTTON_START: return ANDROID_BTN_START;
    case AKEYCODE_BUTTON_SELECT: return ANDROID_BTN_SELECT;
    case AKEYCODE_BUTTON_THUMBL: return ANDROID_BTN_THUMBL;
    case AKEYCODE_BUTTON_THUMBR: return ANDROID_BTN_THUMBR;
    case AKEYCODE_BUTTON_MODE: return ANDROID_BTN_MODE;
    case AKEYCODE_BACK: return ANDROID_BTN_BACK;
    default: return 0;
    }
}

enum { STICK_RANGE = 32767 };
#define STICK_DEADZONE 0.12f

/* Zero inside the deadzone, then rescaled so the stick still reaches full range. */
static int16_t stickValue(float v)
{
    float a = v < 0.f ? -v : v;
    if (a < STICK_DEADZONE)
        return 0;
    a = (a - STICK_DEADZONE) / (1.f - STICK_DEADZONE);
    if (a > 1.f)
        a = 1.f;
    return (int16_t)((v < 0.f ? -a : a) * STICK_RANGE);
}

static uint8_t triggerValue(float v)
{
    return v <= 0.f ? 0 : v >= 1.f ? 255 : (uint8_t)(v * 255.f);
}

void AndroidHost_GetInput(AndroidInputState *state)
{
    pthread_mutex_lock(&in.lock);
    uint32_t buttons = in.keys | in.latchedKeys;
    in.latchedKeys = 0;
    if (in.hatX < 0) buttons |= ANDROID_BTN_DPAD_LEFT;
    if (in.hatX > 0) buttons |= ANDROID_BTN_DPAD_RIGHT;
    if (in.hatY < 0) buttons |= ANDROID_BTN_DPAD_UP;
    if (in.hatY > 0) buttons |= ANDROID_BTN_DPAD_DOWN;
    if (in.leftTrigger > 127) buttons |= ANDROID_BTN_L2;
    if (in.rightTrigger > 127) buttons |= ANDROID_BTN_R2;
    state->buttons = buttons;
    state->leftX = in.leftX;
    state->leftY = in.leftY;
    state->rightX = in.rightX;
    state->rightY = in.rightY;
    state->leftTrigger = in.leftTrigger;
    state->rightTrigger = in.rightTrigger;
    state->touchActive = in.touchActive || in.latchedTouch;
    in.latchedTouch = false;
    state->touchX = in.touchX;
    state->touchY = in.touchY;
    state->serial = in.serial;
    pthread_mutex_unlock(&in.lock);
}

/* ── JNI (main thread) ───────────────────────────────────────────────────── */

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    g.vm = vm;
    jclass bridge = (*env)->FindClass(env, "com/emerald3ds/thor/NativeBridge");
    if (bridge == NULL)
        return JNI_ERR;
    g.bridge = (*env)->NewGlobalRef(env, bridge);
    g.showMessage = (*env)->GetStaticMethodID(env, bridge, "showMessage", "(Ljava/lang/String;)V");
    g.gameExited = (*env)->GetStaticMethodID(env, bridge, "gameExited", "()V");
    if (g.showMessage == NULL || g.gameExited == NULL)
        return JNI_ERR;
    /* Waits in nativeStop use a monotonic clock so a wall-clock change cannot stretch them. */
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g.cond, &attr);
    pthread_condattr_destroy(&attr);
    return JNI_VERSION_1_6;
}

static void copyJavaString(JNIEnv *env, jstring s, char *dest, size_t size)
{
    const char *chars = (*env)->GetStringUTFChars(env, s, NULL);
    strncpy(dest, chars, size - 1);
    dest[size - 1] = 0;
    (*env)->ReleaseStringUTFChars(env, s, chars);
}

JNIEXPORT jboolean JNICALL Java_com_emerald3ds_thor_NativeBridge_nativeStart(JNIEnv *env, jclass cls, jstring internalDir, jstring externalDir)
{
    (void)cls;
    pthread_mutex_lock(&g.lock);
    if (g.started && !g.finished)
    {
        pthread_mutex_unlock(&g.lock);
        return JNI_TRUE; /* the Activity was recreated; the engine keeps running */
    }
    copyJavaString(env, internalDir, g.internalDir, sizeof g.internalDir);
    copyJavaString(env, externalDir, g.externalDir, sizeof g.externalDir);
    g.exitRequested = g.finished = g.gameExitReported = false;
    g.started = pthread_create(&g.thread, NULL, engineThread, NULL) == 0;
    bool ok = g.started;
    pthread_mutex_unlock(&g.lock);
    if (!ok)
        LOGE("could not create the engine thread");
    return ok;
}

JNIEXPORT void JNICALL Java_com_emerald3ds_thor_NativeBridge_nativeSetSurface(JNIEnv *env, jclass cls, jint screen, jobject surface)
{
    (void)cls;
    if (screen < 0 || screen >= SCREEN_COUNT)
        return;
    ANativeWindow *window = surface != NULL ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&g.lock);
    ANativeWindow *replaced = g.pending[screen].window; /* posted but never applied: only the newest counts */
    g.pending[screen].window = window;
    g.pending[screen].dirty = true;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
    if (replaced != NULL)
        ANativeWindow_release(replaced);
}

JNIEXPORT void JNICALL Java_com_emerald3ds_thor_NativeBridge_nativeSurfaceSize(JNIEnv *env, jclass cls, jint screen, jint width, jint height)
{
    (void)env;
    (void)cls;
    /* The engine reads the real size from the EGL surface; this is for the log only. */
    LOGI("%s view size %dx%d", screen == SCREEN_TOP ? "top" : "bottom", width, height);
}

JNIEXPORT void JNICALL Java_com_emerald3ds_thor_NativeBridge_nativePause(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&g.lock);
    g.paused = true;
    pthread_mutex_unlock(&g.lock);
    LOGI("pause");
}

JNIEXPORT void JNICALL Java_com_emerald3ds_thor_NativeBridge_nativeResume(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&g.lock);
    g.paused = false;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
    LOGI("resume");
}

JNIEXPORT void JNICALL Java_com_emerald3ds_thor_NativeBridge_nativeStop(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&g.lock);
    g.exitRequested = true;
    pthread_cond_broadcast(&g.cond);
    if (g.started)
    {
        /* The engine polls ExitRequested every frame; do not hold the UI thread past a few of them
         * if it is stuck (it is then abandoned, and the process is going away anyway). */
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += 2;
        while (!g.finished)
            if (pthread_cond_timedwait(&g.cond, &g.lock, &deadline) != 0)
                break;
        if (g.finished)
            pthread_join(g.thread, NULL);
        else
        {
            LOGE("engine did not stop within 2 s; detaching it");
            pthread_detach(g.thread);
        }
        g.started = false;
    }
    pthread_mutex_unlock(&g.lock);
    LOGI("stop");
}

JNIEXPORT jboolean JNICALL Java_com_emerald3ds_thor_NativeBridge_nativeKey(JNIEnv *env, jclass cls, jint keyCode, jboolean down)
{
    (void)env;
    (void)cls;
    uint32_t bit = buttonForKey(keyCode);
    if (bit == 0)
        return JNI_FALSE;
    pthread_mutex_lock(&in.lock);
    if (down)
    {
        in.keys |= bit;
        in.latchedKeys |= bit;
    }
    else
        in.keys &= ~bit;
    in.serial++;
    pthread_mutex_unlock(&in.lock);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_com_emerald3ds_thor_NativeBridge_nativeMotion(JNIEnv *env, jclass cls, jfloat lx, jfloat ly, jfloat rx,
                                                                         jfloat ry, jfloat hatX, jfloat hatY, jfloat lt, jfloat rt)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&in.lock);
    in.leftX = stickValue(lx);
    in.leftY = stickValue(ly);
    in.rightX = stickValue(rx);
    in.rightY = stickValue(ry);
    in.hatX = hatX < -0.5f ? -1 : hatX > 0.5f ? 1 : 0;
    in.hatY = hatY < -0.5f ? -1 : hatY > 0.5f ? 1 : 0;
    in.leftTrigger = triggerValue(lt);
    in.rightTrigger = triggerValue(rt);
    in.serial++;
    pthread_mutex_unlock(&in.lock);
}

JNIEXPORT void JNICALL Java_com_emerald3ds_thor_NativeBridge_nativeTouch(JNIEnv *env, jclass cls, jint action, jfloat x, jfloat y,
                                                                        jint viewW, jint viewH)
{
    (void)env;
    (void)cls;
    enum { ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2, ACTION_CANCEL = 3 }; /* android.view.MotionEvent */
    pthread_mutex_lock(&in.lock);
    in.serial++;
    if (action == ACTION_DOWN || action == ACTION_MOVE)
    {
        /* View pixels -> surface pixels -> through the last draw's letterbox -> canvas pixels. */
        if (in.letterbox.valid && viewW > 0 && viewH > 0 && in.letterbox.w > 0 && in.letterbox.h > 0)
        {
            float sx = x * in.letterbox.surfaceW / viewW, sy = y * in.letterbox.surfaceH / viewH;
            int cx = (int)((sx - in.letterbox.x) * in.letterbox.canvasW / in.letterbox.w);
            int cy = (int)((sy - in.letterbox.y) * in.letterbox.canvasH / in.letterbox.h);
            if (sx < in.letterbox.x) cx = 0; /* (int) truncates toward zero: keep -0.5 from reading as inside */
            if (sy < in.letterbox.y) cy = 0;
            in.touchX = (int16_t)(cx < 0 ? 0 : cx >= in.letterbox.canvasW ? in.letterbox.canvasW - 1 : cx);
            in.touchY = (int16_t)(cy < 0 ? 0 : cy >= in.letterbox.canvasH ? in.letterbox.canvasH - 1 : cy);
            in.touchActive = true;
            in.latchedTouch = in.latchedTouch || action == ACTION_DOWN;
        }
    }
    else if (action == ACTION_UP || action == ACTION_CANCEL)
        in.touchActive = false;
    pthread_mutex_unlock(&in.lock);
}

JNIEXPORT void JNICALL Java_com_emerald3ds_thor_NativeBridge_nativeResetInput(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&in.lock);
    in.keys = in.latchedKeys = 0;
    in.hatX = in.hatY = 0;
    in.leftTrigger = in.rightTrigger = 0;
    in.leftX = in.leftY = in.rightX = in.rightY = 0;
    in.touchActive = in.latchedTouch = false;
    in.serial++;
    pthread_mutex_unlock(&in.lock);
}
