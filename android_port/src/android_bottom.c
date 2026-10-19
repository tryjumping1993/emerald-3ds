/*
 * Native half of the bottom screen (3ds_bottom.h) for the Thor: the 3DS's framebuffer is a
 * RGB565 array laid out in columns, each running bottom to top, which the game side's canvas
 * already matches. The blits copy into a copy of it kept here, convert what changed to the
 * host's row-major RGBA, and hand the picture to the bottom surface; the game calls them
 * only when what the screen shows changes, which is the right model for an OLED.
 */
#include <3ds.h>
#include <string.h>

#include "3ds_bottom.h"
#include "android_host.h"

static uint16_t sFramebuffer[CTR_BOTTOM_WIDTH * CTR_BOTTOM_HEIGHT];
static uint32_t sPicture[CTR_BOTTOM_WIDTH * CTR_BOTTOM_HEIGHT];
static uint64_t sCopyTicks;

void CtrBottom_LastBlitTicks(uint64_t *copy, uint64_t *flush)
{
    *copy = sCopyTicks;
    *flush = 0;
}

static uint32_t Expand(uint16_t v)
{
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;

    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

/* Rows [y0, y1) of columns [x0, x1) into the picture, each channel scaled by level/4. */
static void Convert(int x0, int y0, int x1, int y1, int level)
{
    for (int x = x0; x < x1; ++x)
    {
        for (int y = y0; y < y1; ++y)
        {
            uint32_t pixel = Expand(sFramebuffer[x * CTR_BOTTOM_HEIGHT + (CTR_BOTTOM_HEIGHT - 1 - y)]);

            if (level < 4)
                pixel = 0xFF000000u | (((pixel & 0xFF) * (uint32_t)level / 4) & 0xFF)
                    | ((((pixel >> 8) & 0xFF) * (uint32_t)level / 4) << 8)
                    | ((((pixel >> 16) & 0xFF) * (uint32_t)level / 4) << 16);
            sPicture[y * CTR_BOTTOM_WIDTH + x] = pixel;
        }
    }
}

static void Present(void)
{
    AndroidHost_PresentBottom(sPicture, CTR_BOTTOM_WIDTH, CTR_BOTTOM_HEIGHT);
}

void CtrBottom_Blit(const uint16_t *canvas, int x0, int x1)
{
    uint64_t start;

    sCopyTicks = 0;
    if (x0 >= x1)
        return;
    start = svcGetSystemTick();
    memcpy(sFramebuffer + x0 * CTR_BOTTOM_HEIGHT, canvas + x0 * CTR_BOTTOM_HEIGHT,
           (size_t)(x1 - x0) * CTR_BOTTOM_HEIGHT * sizeof(uint16_t));
    Convert(x0, 0, x1, CTR_BOTTOM_HEIGHT, 4);
    Present();
    sCopyTicks = svcGetSystemTick() - start;
}

void CtrBottom_BlitRect(const uint16_t *canvas, int x0, int y0, int x1, int y1)
{
    /* A column runs bottom to top, so rows [y0, y1) are one contiguous run of it. */
    int first = CTR_BOTTOM_HEIGHT - y1, count = y1 - y0;
    uint64_t start;

    sCopyTicks = 0;
    if (x0 >= x1 || count <= 0)
        return;
    start = svcGetSystemTick();
    for (int x = x0; x < x1; ++x)
        memcpy(sFramebuffer + x * CTR_BOTTOM_HEIGHT + first, canvas + x * CTR_BOTTOM_HEIGHT + first,
               (size_t)count * sizeof(uint16_t));
    Convert(x0, y0, x1, y1, 4);
    Present();
    sCopyTicks = svcGetSystemTick() - start;
}

void CtrBottom_BlitDim(const uint16_t *canvas, int level)
{
    if (level > 4)
        level = 4;
    else if (level < 0)
        level = 0;
    memcpy(sFramebuffer, canvas, sizeof(sFramebuffer));
    Convert(0, 0, CTR_BOTTOM_WIDTH, CTR_BOTTOM_HEIGHT, level);
    Present();
}
