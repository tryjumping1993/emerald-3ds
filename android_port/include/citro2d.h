#ifndef ANDROID_SHIM_CITRO2D_H
#define ANDROID_SHIM_CITRO2D_H
/*
 * The part of Citro2D the compositor uses, over the Citro3D layer (c2d_gles.c): sprites
 * drawn from a texture with a per-corner tint, solid rectangles and shapes, a scene
 * with a view transform, and its batching (C2D_Flush).
 */
#include <citro3d.h>

typedef struct
{
    u16 width, height;
    float left, top, right, bottom;
} Tex3DS_SubTexture;

typedef struct { u32 color; float blend; } C2D_Tint;
typedef enum { C2D_TopLeft, C2D_TopRight, C2D_BotLeft, C2D_BotRight } C2D_Corner;
typedef struct { C2D_Tint corners[4]; } C2D_ImageTint;
typedef struct { C3D_Tex *tex; const Tex3DS_SubTexture *subtex; } C2D_Image;

#define C2D_DEFAULT_MAX_OBJECTS 4096

static inline u32 C2D_Color32(u8 r, u8 g, u8 b, u8 a)
{
    return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
}

static inline u32 C2D_Color32f(float r, float g, float b, float a)
{
    return C2D_Color32((u8)(r * 255.0f + 0.5f), (u8)(g * 255.0f + 0.5f), (u8)(b * 255.0f + 0.5f), (u8)(a * 255.0f + 0.5f));
}

static inline void C2D_SetImageTint(C2D_ImageTint *tint, C2D_Corner corner, u32 color, float blend)
{
    tint->corners[corner].color = color;
    tint->corners[corner].blend = blend;
}

static inline void C2D_PlainImageTint(C2D_ImageTint *tint, u32 color, float blend)
{
    for (int i = 0; i < 4; ++i)
        C2D_SetImageTint(tint, (C2D_Corner)i, color, blend);
}

static inline void C2D_AlphaImageTint(C2D_ImageTint *tint, float alpha)
{
    C2D_PlainImageTint(tint, C2D_Color32f(1.0f, 1.0f, 1.0f, alpha), 0.0f);
}

static inline void C2D_TopImageTint(C2D_ImageTint *tint, u32 color, float blend)
{
    C2D_SetImageTint(tint, C2D_TopLeft, color, blend);
    C2D_SetImageTint(tint, C2D_TopRight, color, blend);
}

static inline void C2D_BottomImageTint(C2D_ImageTint *tint, u32 color, float blend)
{
    C2D_SetImageTint(tint, C2D_BotLeft, color, blend);
    C2D_SetImageTint(tint, C2D_BotRight, color, blend);
}

bool C2D_Init(size_t maxObjects);
void C2D_Fini(void);
void C2D_Prepare(void);
void C2D_Flush(void);
C3D_RenderTarget *C2D_CreateScreenTarget(gfxScreen_t screen, gfx3dSide_t side);
void C2D_SceneBegin(C3D_RenderTarget *target);
void C2D_SceneSize(u32 width, u32 height, bool tilt);
void C2D_TargetClear(C3D_RenderTarget *target, u32 color);
void C2D_ViewReset(void);
void C2D_ViewTranslate(float x, float y);
void C2D_ViewScale(float x, float y);
void C2D_ViewSave(C3D_Mtx *matrix);
void C2D_ViewRestore(const C3D_Mtx *matrix);

bool C2D_DrawImageAt(C2D_Image img, float x, float y, float depth, const C2D_ImageTint *tint, float scaleX, float scaleY);
bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 clr);
bool C2D_DrawRectangle(float x, float y, float z, float w, float h, u32 clrTL, u32 clrTR, u32 clrBL, u32 clrBR);
bool C2D_DrawTriangle(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1, float x2, float y2, u32 clr2, float depth);
bool C2D_DrawLine(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1, float thickness, float depth);
bool C2D_DrawCircleSolid(float x, float y, float z, float radius, u32 clr);

#endif
