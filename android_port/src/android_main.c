/*
 * The engine thread's body: main_3ds.c's main() without the 3DS's heap override and HOME
 * Menu forwarder. The game owns the loop; CtrGame_Init enters AgbMain and comes back only
 * through the platform's own exit path.
 */
#include <3ds.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/iosupport.h>

#include "3ds_platform.h"
#include "3ds_video.h"
#include "android_host.h"

int AndroidEngine_Main(void)
{
    CtrPlatformHooks hooks = {0};

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
