#ifndef ANDROID_SHIM_3DS_H
#define ANDROID_SHIM_3DS_H
/*
 * The part of libctru the port's reusable native units (3ds_platform.c, 3ds_input.c,
 * 3ds_fs.c, 3ds_data.c, 3ds_log.c, 3ds_settings.c, 3ds_prof.c, 3ds_scenario.c) use, over
 * pthreads, the monotonic clock and the Android host (android_host.h), so that they build
 * for the Thor unchanged. Implemented in ctr_shim.c. Only what those units call is here;
 * anything else is a compile error on purpose.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef volatile u32 vu32;

typedef s32 Result;
typedef u32 Handle;
#define R_SUCCEEDED(res) ((Result)(res) >= 0)
#define R_FAILED(res) ((Result)(res) < 0)
#define U64_MAX UINT64_MAX

/* ── Clock ────────────────────────────────────────────────────────────────── */
/* A tick is a nanosecond of CLOCK_MONOTONIC. */
#define SYSCLOCK_ARM11 1000000000ULL
u64 svcGetSystemTick(void);
void svcSleepThread(s64 nanoseconds);
void svcOutputDebugString(const char *text, int length);
u32 osGetKernelVersion(void);
u32 osGetFirmVersion(void);
#define GET_VERSION_MAJOR(v) (((v) >> 24) & 0xFF)
#define GET_VERSION_MINOR(v) (((v) >> 16) & 0xFF)
#define GET_VERSION_REVISION(v) (((v) >> 8) & 0xFF)
float osGet3DSliderState(void);
void osSetSpeedupEnable(bool enable);
bool osGetSpeedupEnable(void);

/* ── Locks, events, threads ──────────────────────────────────────────────── */
typedef s32 LightLock;
void LightLock_Init(LightLock *lock);
void LightLock_Lock(LightLock *lock);
void LightLock_Unlock(LightLock *lock);

typedef enum { RESET_ONESHOT = 0, RESET_STICKY = 1 } ResetType;
typedef struct
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool signalled;
    bool sticky;
} LightEvent;
void LightEvent_Init(LightEvent *event, ResetType type);
void LightEvent_Signal(LightEvent *event);
void LightEvent_Clear(LightEvent *event);
void LightEvent_Wait(LightEvent *event);
/* True if it timed out. */
int LightEvent_WaitTimeout(LightEvent *event, s64 timeoutNanoseconds);

typedef struct ThreadInfo *Thread;
typedef void (*ThreadFunc)(void *);
#define CUR_THREAD_HANDLE 0xFFFF8000u
#define CUR_PROCESS_HANDLE 0xFFFF8001u
Thread threadCreate(ThreadFunc entry, void *arg, size_t stackSize, int priority, int core, bool detached);
Result threadJoin(Thread thread, u64 timeoutNanoseconds);
void threadFree(Thread thread);
Result svcGetThreadPriority(s32 *priority, Handle thread);
Result APT_GetAppCpuTimeLimit(u32 *percent);
Result APT_SetAppCpuTimeLimit(u32 percent);

/* ── System state ────────────────────────────────────────────────────────── */
typedef enum { APTHOOK_ONSUSPEND, APTHOOK_ONRESTORE, APTHOOK_ONSLEEP, APTHOOK_ONWAKEUP, APTHOOK_ONEXIT, APTHOOK_COUNT } APT_HookType;
typedef struct aptHookCookie
{
    struct aptHookCookie *next;
    void (*callback)(APT_HookType, void *);
    void *param;
} aptHookCookie;
void aptHook(aptHookCookie *cookie, void (*callback)(APT_HookType, void *), void *param);
void aptUnhook(aptHookCookie *cookie);
/* False once the app is asked to stop; blocks while it is in the background. */
bool aptMainLoop(void);
bool aptIsHomeAllowed(void);
void aptSetHomeAllowed(bool allowed);
bool aptCheckHomePressRejected(void);

typedef enum { CFG_MODEL_3DS, CFG_MODEL_3DSXL, CFG_MODEL_N3DS, CFG_MODEL_2DS, CFG_MODEL_N3DSXL, CFG_MODEL_N2DSXL } CFG_SystemModel;
Result cfguInit(void);
void cfguExit(void);
Result CFGU_GetSystemModel(u8 *model);

/* ── RomFS: the engine files are ordinary files (the "romfs:/" prefix is remapped, android_paths.c) ── */
Result romfsInit(void);
Result romfsExit(void);

/* ── glibc's fopencookie, which bionic lacks, over BSD funopen (ctr_shim.c) ── */
typedef struct
{
    ssize_t (*read)(void *cookie, char *buffer, size_t size);
    ssize_t (*write)(void *cookie, const char *buffer, size_t size);
    int (*seek)(void *cookie, off_t *offset, int whence);
    int (*close)(void *cookie);
} cookie_io_functions_t;
FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t functions);

/* ── Display: nothing to configure, the host draws ────────────────────────── */
typedef enum { GFX_TOP = 0, GFX_BOTTOM = 1 } gfxScreen_t;
typedef enum { GFX_LEFT = 0, GFX_RIGHT = 1 } gfx3dSide_t;
typedef enum { GSP_RGBA8_OES = 0, GSP_BGR8_OES = 1, GSP_RGB565_OES = 2, GSP_RGB5_A1_OES = 3, GSP_RGBA4_OES = 4 } GSPGPU_FramebufferFormat;
void gfxInit(GSPGPU_FramebufferFormat top, GSPGPU_FramebufferFormat bottom, bool vramBuffers);
void gfxExit(void);
void gfxSetScreenFormat(gfxScreen_t screen, GSPGPU_FramebufferFormat format);
void gfxSetDoubleBuffering(gfxScreen_t screen, bool enable);
u8 *gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height);
void gfxFlushBuffers(void);
void gfxSwapBuffers(void);
void gspWaitForVBlank(void);

/* The linear heap is ordinary memory here; the flush calls have no cache to flush. */
void *linearAlloc(size_t size);
void *linearMemAlign(size_t size, size_t alignment);
void linearFree(void *memory);
Result APT_CheckNew3DS(bool *out);
void gfxSet3D(bool enable);
GSPGPU_FramebufferFormat gfxGetScreenFormat(gfxScreen_t screen);

/* The 3DS's memory pools: the Thor has none to report. */
size_t linearSpaceFree(void);
size_t vramSpaceFree(void);

/* ── Console: the log's text screens are not built on Android ─────────────── */
typedef struct { int unused; } PrintConsole;
PrintConsole *consoleInit(gfxScreen_t screen, PrintConsole *console);
PrintConsole *consoleSelect(PrintConsole *console);
void consoleSetWindow(PrintConsole *console, int x, int y, int width, int height);
void consoleClear(void);

/* ── Input: the host's controller and bottom-screen touch ─────────────────── */
enum
{
    KEY_A = 1 << 0, KEY_B = 1 << 1, KEY_SELECT = 1 << 2, KEY_START = 1 << 3,
    KEY_DRIGHT = 1 << 4, KEY_DLEFT = 1 << 5, KEY_DUP = 1 << 6, KEY_DDOWN = 1 << 7,
    KEY_R = 1 << 8, KEY_L = 1 << 9, KEY_X = 1 << 10, KEY_Y = 1 << 11,
    KEY_ZL = 1 << 14, KEY_ZR = 1 << 15, KEY_TOUCH = 1 << 20
};
typedef struct { s16 dx, dy; } circlePosition;
typedef struct { u16 px, py; } touchPosition;
void hidScanInput(void);
u32 hidKeysHeld(void);
u32 hidKeysDown(void);
void hidCircleRead(circlePosition *position);
void hidTouchRead(touchPosition *position);

#endif
