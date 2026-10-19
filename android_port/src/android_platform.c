/*
 * 3ds_platform.c for the Thor: the frame loop, pacing, timing, threads and shutdown are the
 * 3DS's own, over the libctru shim. What differs is how a fatal error and a data error are
 * told to the player: there is no console to write on, so they go to the shell's status
 * overlay and the app waits there, as the console's text screens wait for START.
 *
 * 3ds_platform.c calls those two itself, which a linker --wrap would not catch; its
 * definitions are renamed out of the way here and replaced below.
 */
#define CtrPlatform_Fatal CtrPlatform_Fatal_console
#define CtrPlatform_ShowDataError CtrPlatform_ShowDataError_console
#include "3ds_platform.c"
#undef CtrPlatform_Fatal
#undef CtrPlatform_ShowDataError

#include "android_host.h"

/* Shows the text and keeps the lifecycle answering until START or Back, then ends the game. */
static void Park(const char *text) __attribute__((noreturn));
static void Park(const char *text)
{
    AndroidHost_ShowMessage(text);
    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        svcSleepThread(16000000);
    }
    AndroidHost_ShowMessage("");
    exit(EXIT_FAILURE);
}

void CtrPlatform_ShowDataError(const char *title, const char *detail)
{
    char text[1024];

    CtrLog_Write(CTR_LOG_ERROR, "data error: %s: %s", title, detail);
    snprintf(text, sizeof(text), "Emerald 3DS\n\n%s\n\n%s\n\nData folder:\n%s\n\nUse the Thor builder's matching emerald3ds.pak.\nPress Android Back to import it, or START to exit.",
             title, detail ? detail : "", AndroidHost_ExternalDir());
    Park(text);
}

void CtrPlatform_Fatal(const char *reason)
{
    char text[512];

    CtrLog_Write(CTR_LOG_ERROR, "%s", reason);
    snprintf(text, sizeof(text), "Emerald 3DS\n\n%s\n\nPress START to exit.", reason);
    Park(text);
}
