/*
 * The engine thread's body: main_3ds.c's main() without the 3DS's heap override and HOME
 * Menu forwarder. The game owns the loop; CtrGame_Init enters AgbMain and comes back only
 * through the platform's own exit path.
 */
#define _GNU_SOURCE
#include <3ds.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/iosupport.h>
#include <ucontext.h>

#include "3ds_platform.h"
#include "3ds_video.h"
#include "android_host.h"

/*
 * A crash in the game is a bug to find, and Android's own backtrace stops at the first frame
 * the unwinder has no tables for. The engine is built with frame pointers, so this handler
 * walks them and logs each return address as an offset into the library, for
 * llvm-symbolizer against the unstripped emerald3ds.elf.
 */
static void CrashHandler(int signal, siginfo_t *info, void *context)
{
    const ucontext_t *uc = context;
    Dl_info where;
    uintptr_t base = 0;
    const uintptr_t *frame = (const uintptr_t *)uc->uc_mcontext.arm_fp;

    if (dladdr((void *)AndroidEngine_Main, &where))
        base = (uintptr_t)where.dli_fbase;
    __android_log_print(ANDROID_LOG_FATAL, "Emerald3DS", "CRASH signal %d at address %p, pc %p lr %p (library base %p)",
                        signal, info->si_addr, (void *)uc->uc_mcontext.arm_pc, (void *)uc->uc_mcontext.arm_lr, (void *)base);
    __android_log_print(ANDROID_LOG_FATAL, "Emerald3DS", "CRASH pc  offset 0x%lx", (unsigned long)(uc->uc_mcontext.arm_pc - base));
    __android_log_print(ANDROID_LOG_FATAL, "Emerald3DS", "CRASH lr  offset 0x%lx", (unsigned long)(uc->uc_mcontext.arm_lr - base));
    for (int depth = 0; depth < 24 && frame != NULL && ((uintptr_t)frame & 3) == 0; ++depth)
    {
        uintptr_t lr = frame[1];

        __android_log_print(ANDROID_LOG_FATAL, "Emerald3DS", "CRASH #%d offset 0x%lx", depth, (unsigned long)(lr - base));
        if (frame[0] <= (uintptr_t)frame)
            break;
        frame = (const uintptr_t *)frame[0];
    }
    /* Back to the default action, so the system's own report follows. */
    signal = SIGSEGV;
    sigaction(signal, &(struct sigaction){ .sa_handler = SIG_DFL }, NULL);
    raise(info->si_signo);
}

static void InstallCrashHandler(void)
{
    struct sigaction action = { .sa_sigaction = CrashHandler, .sa_flags = SA_SIGINFO | SA_RESETHAND };

    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
}

int AndroidEngine_Main(void)
{
    CtrPlatformHooks hooks = {0};

    InstallCrashHandler();

    if (!CtrPlatform_Init())
        CtrPlatform_Fatal("platform initialization failed");
    /* The log's device exists now: the game's printf goes to it. */
    CtrShim_RedirectStdio();
    hooks.vblank = CtrGame_VBlank;
    hooks.reset = CtrGame_SoftReset;
    hooks.videoPresent = CtrVideo_Present;
    CtrPlatform_SetHooks(&hooks);
    if (!CtrPlatform_BeginFrame())
        return 0;
    CtrGame_Init();
    CtrPlatform_Shutdown();
    return 0;
}

/* The game and the platform's error screens end the program with exit(), which on Android
 * would take the whole app's process down: only the engine thread ends, and the shell is
 * told to finish the Activity. Linked with --wrap=exit. */
void __wrap_exit(int status) __attribute__((noreturn));
void __wrap_exit(int status)
{
    CtrLog_Write(CTR_LOG_BOOT, "exit(%d)", status);
    CtrPlatform_Shutdown();
    AndroidHost_NotifyGameExit();
    pthread_exit(NULL);
}
