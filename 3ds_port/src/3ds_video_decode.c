#include "3ds_video.h"

uint32_t CtrVideo_TextMapOffset(unsigned x, unsigned y, unsigned size)
{
    unsigned width = (size & 1) ? 64 : 32;
    unsigned height = (size & 2) ? 64 : 32;
    x &= width - 1;
    y &= height - 1;
    return (((y / 32) * (width / 32) + x / 32) * 1024
            + (y & 31) * 32 + (x & 31)) * 2;
}

uint32_t CtrVideo_Texel(unsigned x, unsigned y, unsigned width)
{
#ifdef PLATFORM_ANDROID
    /* Tiles of 8x8 texels left to right and down, each in rows: the Thor's textures are
     * untiled when uploaded (android_port/src/c3d_gles.c), so the PICA's Morton order inside
     * a tile has no meaning there. */
    return ((y / 8) * (width / 8) + x / 8) * 64 + (y & 7) * 8 + (x & 7);
#else
    unsigned morton = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1)
                    | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return ((y / 8) * (width / 8) + x / 8) * 64 + morton;
#endif
}

uint32_t CtrVideo_RGBA8(uint16_t color, bool opaque)
{
    unsigned r = color & 31, g = (color >> 5) & 31, b = (color >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    /* PICA texture memory is ABGR bytes (little endian uint32 = RRGGBBAA). */
    return (r << 24) | (g << 16) | (b << 8) | (opaque ? 255 : 0);
}

uint16_t CtrVideo_RGBA5551(uint16_t color)
{
    /* GBA BGR555 -> PICA RGBA5551, with no loss of source colour precision. */
    return ((color & 31) << 11) | ((color & 0x3e0) << 1)
         | ((color >> 9) & 0x3e) | 1;
}

unsigned CtrVideo_ObjTile(unsigned base, unsigned x, unsigned y,
                          unsigned width, bool color256, bool mapping1d)
{
    unsigned units = color256 ? 2 : 1;
    if (color256)
        base &= ~1u;
    return (base + y * (mapping1d ? width / 8 * units : 32) + x * units) & 1023;
}

int32_t CtrVideo_AffineReference(uint32_t value)
{
    value &= 0x0fffffff;
    return (value & 0x08000000) ? (int32_t)(value | 0xf0000000) : (int32_t)value;
}
