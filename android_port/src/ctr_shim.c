#define _GNU_SOURCE /* bionic declares funopen only for BSD or GNU sources */
/* The libctru subset of include/3ds.h over pthreads, the monotonic clock and the Android host. */
#include <3ds.h>
#include <android/log.h>
#include <malloc.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/iosupport.h>

#include "android_host.h"

/* ── Clock ────────────────────────────────────────────────────────────────── */

u64 svcGetSystemTick(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

void svcSleepThread(s64 nanoseconds)
{
    struct timespec ts = { (time_t)(nanoseconds / 1000000000LL), (long)(nanoseconds % 1000000000LL) };

    nanosleep(&ts, NULL);
}

void svcOutputDebugString(const char *text, int length)
{
    char line[512];

    if (length < 0 || (size_t)length >= sizeof(line))
        length = sizeof(line) - 1;
    memcpy(line, text, (size_t)length);
    line[length] = 0;
    __android_log_write(ANDROID_LOG_INFO, "Emerald3DS", line);
}

/* Reported as a recent firmware on a New 3DS: the units that branch on them take their
 * fast paths, which is what a Snapdragon wants. */
u32 osGetKernelVersion(void) { return 0x022E0000; }
u32 osGetFirmVersion(void) { return 0x022E0000; }
void osSetSpeedupEnable(bool enable) { (void)enable; }
bool osGetSpeedupEnable(void) { return true; }
float osGet3DSliderState(void) { return 0.0f; }

/* ── Locks and events ────────────────────────────────────────────────────── */

/* libctru permits static initialization with 1 (e.g. the settings saver). */
void LightLock_Init(LightLock *lock) { *lock = 1; }

void LightLock_Lock(LightLock *lock)
{
    _Atomic s32 *word = (_Atomic s32 *)lock;
    unsigned spins = 0;

    for (;;)
    {
        s32 expected = 1;

        if (atomic_compare_exchange_weak_explicit(word, &expected, 0, memory_order_acquire, memory_order_relaxed))
            return;
        if (++spins > 64)
            sched_yield();
    }
}

void LightLock_Unlock(LightLock *lock)
{
    atomic_store_explicit((_Atomic s32 *)lock, 1, memory_order_release);
}

void LightEvent_Init(LightEvent *event, ResetType type)
{
    pthread_condattr_t attr;

    pthread_mutex_init(&event->mutex, NULL);
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&event->cond, &attr);
    pthread_condattr_destroy(&attr);
    event->signalled = false;
    event->sticky = type == RESET_STICKY;
}

void LightEvent_Signal(LightEvent *event)
{
    pthread_mutex_lock(&event->mutex);
    event->signalled = true;
    pthread_cond_broadcast(&event->cond);
    pthread_mutex_unlock(&event->mutex);
}

void LightEvent_Clear(LightEvent *event)
{
    pthread_mutex_lock(&event->mutex);
    event->signalled = false;
    pthread_mutex_unlock(&event->mutex);
}

void LightEvent_Wait(LightEvent *event)
{
    pthread_mutex_lock(&event->mutex);
    while (!event->signalled)
        pthread_cond_wait(&event->cond, &event->mutex);
    if (!event->sticky)
        event->signalled = false;
    pthread_mutex_unlock(&event->mutex);
}

int LightEvent_WaitTimeout(LightEvent *event, s64 timeoutNanoseconds)
{
    struct timespec deadline;
    int timedOut = 0;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(timeoutNanoseconds / 1000000000LL);
    deadline.tv_nsec += (long)(timeoutNanoseconds % 1000000000LL);
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec++;
    }
    pthread_mutex_lock(&event->mutex);
    while (!event->signalled && !timedOut)
        timedOut = pthread_cond_timedwait(&event->cond, &event->mutex, &deadline) == ETIMEDOUT;
    if (event->signalled && !event->sticky)
        event->signalled = false;
    pthread_mutex_unlock(&event->mutex);
    return timedOut;
}

/* ── Threads ─────────────────────────────────────────────────────────────── */

struct ThreadInfo
{
    pthread_t id;
    ThreadFunc entry;
    void *arg;
};

static void *ThreadMain(void *param)
{
    struct ThreadInfo *info = param;

    info->entry(info->arg);
    return NULL;
}

/* Priority and core are the 3DS scheduler's concepts; Android places the thread itself. */
Thread threadCreate(ThreadFunc entry, void *arg, size_t stackSize, int priority, int core, bool detached)
{
    pthread_attr_t attr;
    struct ThreadInfo *info = calloc(1, sizeof(*info));

    (void)priority;
    (void)core;
    if (info == NULL)
        return NULL;
    info->entry = entry;
    info->arg = arg;
    pthread_attr_init(&attr);
    if (stackSize >= 16384)
        pthread_attr_setstacksize(&attr, stackSize);
    pthread_attr_setdetachstate(&attr, detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
    if (pthread_create(&info->id, &attr, ThreadMain, info) != 0)
    {
        free(info);
        info = NULL;
    }
    pthread_attr_destroy(&attr);
    return info;
}

Result threadJoin(Thread thread, u64 timeoutNanoseconds)
{
    (void)timeoutNanoseconds;
    return pthread_join(thread->id, NULL) == 0 ? 0 : -1;
}

void threadFree(Thread thread) { free(thread); }

Result svcGetThreadPriority(s32 *priority, Handle thread)
{
    (void)thread;
    *priority = 0x30;
    return 0;
}

Result APT_GetAppCpuTimeLimit(u32 *percent) { *percent = 80; return 0; }
Result APT_SetAppCpuTimeLimit(u32 percent) { (void)percent; return 0; }

/* ── System state ────────────────────────────────────────────────────────── */

static aptHookCookie *sHooks;

void aptHook(aptHookCookie *cookie, void (*callback)(APT_HookType, void *), void *param)
{
    cookie->callback = callback;
    cookie->param = param;
    cookie->next = sHooks;
    sHooks = cookie;
}

void aptUnhook(aptHookCookie *cookie)
{
    for (aptHookCookie **link = &sHooks; *link != NULL; link = &(*link)->next)
    {
        if (*link == cookie)
        {
            *link = cookie->next;
            return;
        }
    }
}

static void RunHooks(APT_HookType type)
{
    for (aptHookCookie *cookie = sHooks; cookie != NULL; cookie = cookie->next)
        cookie->callback(type, cookie->param);
}

bool aptMainLoop(void)
{
    if (AndroidHost_ExitRequested())
    {
        RunHooks(APTHOOK_ONEXIT);
        return false;
    }
    /* In the background the game is suspended, as the HOME Menu does; waking it is a restore.
     * The host blocks while paused and returns at once otherwise, so a call that took a
     * while was a pause. */
    u64 start = svcGetSystemTick();
    bool running = AndroidHost_WaitWhilePaused();

    if (!running)
    {
        RunHooks(APTHOOK_ONEXIT);
        return false;
    }
    if (svcGetSystemTick() - start > 50000000ULL)
        RunHooks(APTHOOK_ONRESTORE);
    return true;
}

bool aptIsHomeAllowed(void) { return true; }
void aptSetHomeAllowed(bool allowed) { (void)allowed; }
bool aptCheckHomePressRejected(void) { return false; }

Result cfguInit(void) { return 0; }
void cfguExit(void) {}
Result CFGU_GetSystemModel(u8 *model) { *model = CFG_MODEL_N3DS; return 0; }

/* ── Display and console: the host draws; these configure nothing ──────────── */

void gfxInit(GSPGPU_FramebufferFormat top, GSPGPU_FramebufferFormat bottom, bool vramBuffers)
{
    (void)top;
    (void)bottom;
    (void)vramBuffers;
}

void gfxExit(void) {}
void gfxSetScreenFormat(gfxScreen_t screen, GSPGPU_FramebufferFormat format) { (void)screen; (void)format; }
void gfxSetDoubleBuffering(gfxScreen_t screen, bool enable) { (void)screen; (void)enable; }

u8 *gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height)
{
    /* Only 3ds_log.c's blanking of the bottom screen asks: a scratch the size it clears. */
    static u8 scratch[320 * 240 * 2];

    (void)screen;
    (void)side;
    if (width) *width = 240;
    if (height) *height = 320;
    return scratch;
}

void gfxFlushBuffers(void) {}
void gfxSwapBuffers(void) {}
void gspWaitForVBlank(void) { svcSleepThread(16666667); }

void *linearAlloc(size_t size) { return calloc(1, size); }

void *linearMemAlign(size_t size, size_t alignment)
{
    void *memory = NULL;

    if (posix_memalign(&memory, alignment < sizeof(void *) ? sizeof(void *) : alignment, size) != 0)
        return NULL;
    memset(memory, 0, size);
    return memory;
}

void linearFree(void *memory) { free(memory); }
Result APT_CheckNew3DS(bool *out) { *out = true; return 0; }
void gfxSet3D(bool enable) { (void)enable; }
GSPGPU_FramebufferFormat gfxGetScreenFormat(gfxScreen_t screen) { (void)screen; return GSP_RGB565_OES; }

size_t linearSpaceFree(void) { return 0; }
size_t vramSpaceFree(void) { return 0; }

PrintConsole *consoleInit(gfxScreen_t screen, PrintConsole *console) { (void)screen; return console; }
PrintConsole *consoleSelect(PrintConsole *console) { return console; }

void consoleSetWindow(PrintConsole *console, int x, int y, int width, int height)
{
    (void)console;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

void consoleClear(void) {}

/* ── stdio ─────────────────────────────────────────────────────────────────── */

Result romfsInit(void) { return 0; }
Result romfsExit(void) { return 0; }

struct Cookie
{
    void *cookie;
    cookie_io_functions_t io;
};

static int CookieRead(void *c, char *buffer, int size)
{
    struct Cookie *cookie = c;

    return (int)cookie->io.read(cookie->cookie, buffer, (size_t)size);
}

static int CookieWrite(void *c, const char *buffer, int size)
{
    struct Cookie *cookie = c;

    return (int)cookie->io.write(cookie->cookie, buffer, (size_t)size);
}

static fpos_t CookieSeek(void *c, fpos_t offset, int whence)
{
    struct Cookie *cookie = c;
    off_t position = (off_t)offset;

    if (cookie->io.seek(cookie->cookie, &position, whence) != 0)
        return -1;
    return (fpos_t)position;
}

static int CookieClose(void *c)
{
    struct Cookie *cookie = c;
    int result = cookie->io.close != NULL ? cookie->io.close(cookie->cookie) : 0;

    free(cookie);
    return result;
}

FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t functions)
{
    struct Cookie *wrapper = malloc(sizeof(*wrapper));
    FILE *stream;

    (void)mode;
    if (wrapper == NULL)
        return NULL;
    wrapper->cookie = cookie;
    wrapper->io = functions;
    stream = funopen(wrapper, functions.read ? CookieRead : NULL, functions.write ? CookieWrite : NULL,
                     functions.seek ? CookieSeek : NULL, CookieClose);
    if (stream == NULL)
        free(wrapper);
    return stream;
}

/* 3ds_log.c installs a device that turns the game's printf into log lines. */
const devoptab_t *devoptab_list[3];

static ssize_t StdioWrite(void *cookie, const char *buffer, size_t size)
{
    const devoptab_t *device = devoptab_list[(int)(intptr_t)cookie];
    struct _reent reent = { 0 };

    return device != NULL ? device->write_r(&reent, NULL, buffer, size) : (ssize_t)size;
}

void CtrShim_RedirectStdio(void)
{
    cookie_io_functions_t functions = { .write = StdioWrite };

    for (int fd = STD_OUT; fd <= STD_ERR; ++fd)
    {
        FILE *stream = fopencookie((void *)(intptr_t)fd, "w", functions);

        if (stream == NULL)
            continue;
        setvbuf(stream, NULL, _IONBF, 0);
        if (fd == STD_OUT)
            stdout = stream;
        else
            stderr = stream;
    }
}

/* ── Input: the host's state in libctru's terms ─────────────────────────────── */

static u32 sHeld, sPrevious;
static circlePosition sCircle;
static touchPosition sTouch;

void hidScanInput(void)
{
    static const struct { u32 android, key; } map[] =
    {
        { ANDROID_BTN_A, KEY_A }, { ANDROID_BTN_B, KEY_B }, { ANDROID_BTN_X, KEY_X }, { ANDROID_BTN_Y, KEY_Y },
        { ANDROID_BTN_START, KEY_START }, { ANDROID_BTN_SELECT, KEY_SELECT },
        { ANDROID_BTN_DPAD_UP, KEY_DUP }, { ANDROID_BTN_DPAD_DOWN, KEY_DDOWN },
        { ANDROID_BTN_DPAD_LEFT, KEY_DLEFT }, { ANDROID_BTN_DPAD_RIGHT, KEY_DRIGHT },
        { ANDROID_BTN_L1, KEY_L }, { ANDROID_BTN_R1, KEY_R }, { ANDROID_BTN_L2, KEY_ZL }, { ANDROID_BTN_R2, KEY_ZR },
    };
    AndroidInputState state;
    u32 held = 0;

    AndroidHost_GetInput(&state);
    sPrevious = sHeld;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (state.buttons & map[i].android)
            held |= map[i].key;
    /* The Circle Pad reads +-156 at its rim, +Y up. */
    sCircle.dx = (s16)((int)state.leftX * 156 / 32767);
    sCircle.dy = (s16)(-(int)state.leftY * 156 / 32767);
    if (state.touchActive)
    {
        held |= KEY_TOUCH;
        sTouch.px = (u16)state.touchX;
        sTouch.py = (u16)state.touchY;
    }
    if (held != sHeld)
        __android_log_print(ANDROID_LOG_INFO, "Emerald3DS", "[INPUT] keys %05x -> %05x touch %d,%d", (unsigned)sHeld, (unsigned)held,
                            (int)sTouch.px, (int)sTouch.py);
    sHeld = held;
}

u32 hidKeysHeld(void) { return sHeld; }
u32 hidKeysDown(void) { return sHeld & ~sPrevious; }
void hidCircleRead(circlePosition *position) { *position = sCircle; }
void hidTouchRead(touchPosition *position) { *position = sTouch; }
