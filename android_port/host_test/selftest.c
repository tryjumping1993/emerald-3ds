/*
 * A stand-in AndroidEngine_Main that exercises the host without the game: an animated pattern on
 * both screens at 60 fps, the controller and touch state logged to logcat. The top frame is 400x240
 * and the bottom 320x240, like the 3DS's.
 */
#include <android/log.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "android_host.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Emerald3DS", __VA_ARGS__)

enum { TOP_W = 400, TOP_H = 240, BOTTOM_W = 320, BOTTOM_H = 240 };

typedef struct { uint32_t *px; int w, h; } Canvas;

static uint32_t rgb(int r, int g, int b) { return 0xFF000000u | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r; }

static void fillRect(Canvas c, int x, int y, int w, int h, uint32_t color)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < c.h; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < c.w; i++)
            c.px[j * c.w + i] = color;
}

/* 3x5 digits, one row per 3 bits, top row first. */
static const uint8_t kDigits[10][5] = {
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
    { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
};

static void drawNumber(Canvas c, int x, int y, int scale, unsigned value, int digits, uint32_t color)
{
    for (int d = digits - 1; d >= 0; d--, value /= 10)
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (kDigits[value % 10][row] & (4 >> col))
                    fillRect(c, x + (d * 4 + col) * scale, y + row * scale, scale, scale, color);
}

static void drawTop(Canvas c, unsigned frame, const AndroidInputState *in)
{
    for (int y = 0; y < c.h; y++)
        for (int x = 0; x < c.w; x++)
            c.px[y * c.w + x] = rgb(20 + x * 60 / c.w, 30 + y * 60 / c.h, 90);
    fillRect(c, (int)(frame * 4 % (c.w + 24)) - 12, 0, 12, c.h, rgb(255, 255, 255)); /* the moving bar */
    fillRect(c, 0, 0, c.w, 2, rgb(255, 60, 60));
    fillRect(c, 0, c.h - 2, c.w, 2, rgb(255, 60, 60));
    fillRect(c, 0, 0, 2, c.h, rgb(255, 60, 60));
    fillRect(c, c.w - 2, 0, 2, c.h, rgb(255, 60, 60));
    drawNumber(c, 12, 12, 6, frame, 6, rgb(255, 255, 0));
    for (int b = 0; b < 18; b++) /* one box per ANDROID_BTN_* bit, lit while held */
        fillRect(c, 12 + b * 20, 100, 16, 16, (in->buttons >> b) & 1 ? rgb(60, 255, 60) : rgb(40, 40, 60));
    for (int s = 0; s < 2; s++) /* the sticks */
    {
        int bx = 120 + s * 100, by = 140, lx = s ? in->rightX : in->leftX, ly = s ? in->rightY : in->leftY;
        fillRect(c, bx, by, 60, 60, rgb(40, 40, 60));
        fillRect(c, bx + 28 + lx * 28 / 32767, by + 28 + ly * 28 / 32767, 4, 4, rgb(255, 160, 0));
    }
    fillRect(c, 12, 140, 8, 60 * in->leftTrigger / 255, rgb(255, 160, 0));
    fillRect(c, 24, 140, 8, 60 * in->rightTrigger / 255, rgb(255, 160, 0));
}

static void drawBottom(Canvas c, unsigned frame, const AndroidInputState *in)
{
    fillRect(c, 0, 0, c.w, c.h, rgb(24, 24, 24));
    for (int x = 0; x < c.w; x += 20)
        fillRect(c, x, 0, 1, c.h, rgb(90, 90, 90));
    for (int y = 0; y < c.h; y += 20)
        fillRect(c, 0, y, c.w, 1, rgb(90, 90, 90));
    fillRect(c, 0, 0, c.w, 2, rgb(255, 60, 60));
    fillRect(c, 0, c.h - 2, c.w, 2, rgb(255, 60, 60));
    fillRect(c, 0, 0, 2, c.h, rgb(255, 60, 60));
    fillRect(c, c.w - 2, 0, 2, c.h, rgb(255, 60, 60));
    fillRect(c, 2, 2, 24, 24, rgb(255, 60, 60)); /* the top-left corner marker */
    fillRect(c, c.w - 28, c.h - 28, 24, 24, rgb(60, 140, 255));
    fillRect(c, c.w / 2 - 4, 4, 8, 8, (frame / 30) & 1 ? rgb(60, 255, 60) : rgb(0, 80, 0)); /* heartbeat */
    if (in->touchActive)
    {
        for (int dy = -7; dy <= 7; dy++)
            for (int dx = -7; dx <= 7; dx++)
                if (dx * dx + dy * dy <= 49)
                    fillRect(c, in->touchX + dx, in->touchY + dy, 1, 1, rgb(255, 255, 0));
        drawNumber(c, c.w / 2 - 40, c.h / 2 - 5, 3, (unsigned)in->touchX, 3, rgb(255, 255, 255));
        drawNumber(c, c.w / 2 + 4, c.h / 2 - 5, 3, (unsigned)in->touchY, 3, rgb(255, 255, 255));
    }
}

static void logInputChanges(const AndroidInputState *now, AndroidInputState *last)
{
    if (now->buttons != last->buttons)
        LOGI("buttons 0x%05x -> 0x%05x", last->buttons, now->buttons);
    if (now->leftX / 4096 != last->leftX / 4096 || now->leftY / 4096 != last->leftY / 4096 ||
        now->rightX / 4096 != last->rightX / 4096 || now->rightY / 4096 != last->rightY / 4096)
        LOGI("sticks L(%d,%d) R(%d,%d)", now->leftX, now->leftY, now->rightX, now->rightY);
    if (now->leftTrigger / 32 != last->leftTrigger / 32 || now->rightTrigger / 32 != last->rightTrigger / 32)
        LOGI("triggers L=%d R=%d", now->leftTrigger, now->rightTrigger);
    if (now->touchActive != last->touchActive || (now->touchActive && (now->touchX != last->touchX || now->touchY != last->touchY)))
        LOGI("touch active=%d x=%d y=%d", now->touchActive, now->touchX, now->touchY);
    *last = *now;
}

int AndroidEngine_Main(void)
{
    static uint32_t topPixels[TOP_W * TOP_H], bottomPixels[BOTTOM_W * BOTTOM_H];
    Canvas top = { topPixels, TOP_W, TOP_H }, bottom = { bottomPixels, BOTTOM_W, BOTTOM_H };
    AndroidInputState last;
    memset(&last, 0, sizeof last);
    LOGI("self-test engine: internal=%s external=%s", AndroidHost_InternalDir(), AndroidHost_ExternalDir());
    AndroidHost_ShowMessage("Self-test: no game data, drawing test patterns");

    unsigned frame = 0, bottomShown = ~0u;
    int topW = 0, topH = 0, bottomW = 0, bottomH = 0;
    AndroidInputState shownInput;
    memset(&shownInput, 0, sizeof shownInput);
    while (!AndroidHost_ExitRequested())
    {
        if (!AndroidHost_WaitWhilePaused())
            break;
        AndroidInputState in;
        AndroidHost_GetInput(&in);
        logInputChanges(&in, &last);

        int w, h;
        AndroidHost_TopSurfaceSize(&w, &h);
        if (w != topW || h != topH)
            LOGI("top surface now %dx%d", topW = w, topH = h);
        AndroidHost_BottomSurfaceSize(&w, &h);
        if (w != bottomW || h != bottomH)
            LOGI("bottom surface now %dx%d", bottomW = w, bottomH = h);

        drawTop(top, frame, &in);
        if (topW > 0)
            AndroidHost_PresentTop(topPixels, TOP_W, TOP_H); /* vsync paced */
        else
            usleep(16000); /* nothing to pace against while the top screen has no surface */

        /* The bottom canvas is presented on change only, as the game will: touch, or the heartbeat. */
        if (in.touchActive != shownInput.touchActive || in.touchX != shownInput.touchX || in.touchY != shownInput.touchY ||
            frame / 30 != bottomShown)
        {
            drawBottom(bottom, frame, &in);
            AndroidHost_PresentBottom(bottomPixels, BOTTOM_W, BOTTOM_H);
            shownInput = in;
            bottomShown = frame / 30;
        }
        if (frame == 300)
            AndroidHost_ShowMessage("");
        if (++frame % 300 == 0)
            LOGI("frame %u", frame);
    }
    LOGI("self-test engine exiting after %u frames", frame);
    return 0;
}
