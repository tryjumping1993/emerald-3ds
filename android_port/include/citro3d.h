#ifndef ANDROID_SHIM_CITRO3D_H
#define ANDROID_SHIM_CITRO3D_H
/*
 * The part of Citro3D (and the PICA200 enumerations it is driven with) that the compositor
 * (3ds_video.c) uses, over OpenGL ES 3.0 (c3d_gles.c). The compositor was written against
 * the PICA's fixed function pipeline - texture environment stages, a blend unit, render
 * targets in VRAM, a command list per frame - and runs here unchanged through this layer.
 *
 * What the console does that has no meaning here is accepted and ignored: command list
 * splits, cache flushes, VRAM placement, the GX queue.
 */
#include <3ds.h>

/* ── PICA enumerations (names and meaning as in libctru's gpu/enums.h) ─────── */
typedef enum { GPU_RGBA8 = 0, GPU_RGB8, GPU_RGBA5551, GPU_RGB565, GPU_RGBA4, GPU_LA8, GPU_HILO8, GPU_L8, GPU_A8, GPU_LA4,
               GPU_L4, GPU_A4, GPU_ETC1, GPU_ETC1A4 } GPU_TEXCOLOR;
typedef enum { GPU_RB_RGBA8 = 0, GPU_RB_RGB8 = 1, GPU_RB_RGBA5551 = 2, GPU_RB_RGB565 = 3, GPU_RB_RGBA4 = 4 } GPU_COLORBUF;
typedef enum { GPU_RB_DEPTH16 = 0, GPU_RB_DEPTH24 = 2, GPU_RB_DEPTH24_STENCIL8 = 3 } GPU_DEPTHBUF;
typedef enum { GPU_NEAREST = 0, GPU_LINEAR = 1 } GPU_TEXTURE_FILTER_PARAM;
typedef enum { GPU_CLAMP_TO_EDGE = 0, GPU_CLAMP_TO_BORDER = 1, GPU_REPEAT = 2, GPU_MIRRORED_REPEAT = 3 } GPU_TEXTURE_WRAP_PARAM;
typedef enum { GPU_TEXTURE_MAG_FILTER, GPU_TEXTURE_MIN_FILTER, GPU_TEXTURE_WRAP_S, GPU_TEXTURE_WRAP_T } GPU_TEXTURE_PARAM_NAME;
typedef enum { GPU_TEXFACE_2D = 0 } GPU_TEXFACE;
typedef enum { GPU_TEXUNIT0 = 1, GPU_TEXUNIT1 = 2, GPU_TEXUNIT2 = 4 } GPU_TEXUNIT;
typedef enum { GPU_TRIANGLES = 0, GPU_TRIANGLE_STRIP = 0x100, GPU_TRIANGLE_FAN = 0x200 } GPU_Primitive_t;
typedef enum { GPU_BYTE = 0, GPU_UNSIGNED_BYTE = 1, GPU_SHORT = 2, GPU_FLOAT = 3 } GPU_FORMATS;
typedef enum { C3D_UNSIGNED_BYTE = 0, C3D_UNSIGNED_SHORT = 1 } C3D_IndexType;

typedef enum { GPU_NEVER = 0, GPU_ALWAYS, GPU_EQUAL, GPU_NOTEQUAL, GPU_LESS, GPU_LEQUAL, GPU_GREATER, GPU_GEQUAL } GPU_TESTFUNC;
typedef enum { GPU_SCISSOR_DISABLE = 0, GPU_SCISSOR_INVERT = 1, GPU_SCISSOR_NORMAL = 3 } GPU_SCISSORMODE;
typedef enum { GPU_WRITE_RED = 0x01, GPU_WRITE_GREEN = 0x02, GPU_WRITE_BLUE = 0x04, GPU_WRITE_ALPHA = 0x08, GPU_WRITE_DEPTH = 0x10,
               GPU_WRITE_COLOR = 0x0F, GPU_WRITE_ALL = 0x1F } GPU_WRITEMASK;
typedef enum { GPU_BLEND_ADD = 0, GPU_BLEND_SUBTRACT = 1, GPU_BLEND_REVERSE_SUBTRACT = 2, GPU_BLEND_MIN = 3, GPU_BLEND_MAX = 4 } GPU_BLENDEQUATION;
typedef enum { GPU_ZERO = 0, GPU_ONE, GPU_SRC_COLOR, GPU_ONE_MINUS_SRC_COLOR, GPU_DST_COLOR, GPU_ONE_MINUS_DST_COLOR, GPU_SRC_ALPHA,
               GPU_ONE_MINUS_SRC_ALPHA, GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA, GPU_CONSTANT_COLOR, GPU_ONE_MINUS_CONSTANT_COLOR,
               GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA, GPU_SRC_ALPHA_SATURATE } GPU_BLENDFACTOR;

typedef enum { GPU_PRIMARY_COLOR = 0, GPU_FRAGMENT_PRIMARY_COLOR, GPU_FRAGMENT_SECONDARY_COLOR, GPU_TEXTURE0, GPU_TEXTURE1,
               GPU_TEXTURE2, GPU_TEXTURE3, GPU_PREVIOUS_BUFFER = 13, GPU_CONSTANT = 14, GPU_PREVIOUS = 15 } GPU_TEVSRC;
typedef enum { GPU_TEVOP_RGB_SRC_COLOR = 0, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA,
               GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_ONE_MINUS_SRC_R, GPU_TEVOP_RGB_0x06, GPU_TEVOP_RGB_0x07, GPU_TEVOP_RGB_SRC_G,
               GPU_TEVOP_RGB_ONE_MINUS_SRC_G, GPU_TEVOP_RGB_0x0A, GPU_TEVOP_RGB_0x0B, GPU_TEVOP_RGB_SRC_B, GPU_TEVOP_RGB_ONE_MINUS_SRC_B } GPU_TEVOP_RGB;
typedef enum { GPU_TEVOP_A_SRC_ALPHA = 0, GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_ONE_MINUS_SRC_R, GPU_TEVOP_A_SRC_G,
               GPU_TEVOP_A_ONE_MINUS_SRC_G, GPU_TEVOP_A_SRC_B, GPU_TEVOP_A_ONE_MINUS_SRC_B } GPU_TEVOP_A;
typedef enum { GPU_REPLACE = 0, GPU_MODULATE, GPU_ADD, GPU_ADD_SIGNED, GPU_INTERPOLATE, GPU_SUBTRACT, GPU_DOT3_RGB, GPU_DOT3_RGBA,
               GPU_MULTIPLY_ADD, GPU_ADD_MULTIPLY } GPU_COMBINEFUNC;
typedef enum { GPU_TEVSCALE_1 = 0, GPU_TEVSCALE_2, GPU_TEVSCALE_4 } GPU_TEVSCALE;
typedef enum { GPU_CULL_NONE = 0, GPU_CULL_FRONT_CCW, GPU_CULL_BACK_CCW } GPU_CULLMODE;

typedef enum { C3D_RGB = 1, C3D_Alpha = 2, C3D_Both = 3 } C3D_TexEnvMode;
typedef enum { GPU_VERTEX_SHADER = 0, GPU_GEOMETRY_SHADER = 1 } GPU_SHADER_TYPE;
typedef enum { GPU_LOGICOP_CLEAR = 0, GPU_LOGICOP_AND, GPU_LOGICOP_AND_REVERSE, GPU_LOGICOP_COPY, GPU_LOGICOP_SET,
               GPU_LOGICOP_COPY_INVERTED, GPU_LOGICOP_NOOP, GPU_LOGICOP_INVERT } GPU_LOGICOP;
#define GPU_TEX_2D 0
#define GPU_TEXTURE_MODE(m) (((u32)(m) & 7u) << 28)

/* ── Types ──────────────────────────────────────────────────────────────── */
typedef struct
{
    void *data;                /* the texels, in the compositor's tile-major order (CtrVideo_Texel) */
    GPU_TEXCOLOR fmt;
    size_t size;
    u16 width, height;
    u32 glTexture;             /* the GL texture the data is uploaded to */
    u32 dirtyFirstRow, dirtyEndRow;  /* rows C3D_TexFlush has not uploaded yet; first >= end: none */
    u8 filterMag, filterMin, wrapS, wrapT;
    u32 borderColor;
    bool linked;               /* a render target draws into it: the GL texture is the truth */
    u8 *dirtyTiles;            /* a bit per 8x8 tile the compositor has written since the last upload */
    bool touched;
    u32 param;                 /* the PICA's texture mode bits (GPU_TEXTURE_MODE); only the voxel world sets it */
    u32 scanned;               /* the frame whose texels were last compared with what the GL holds (see TexReady) */
    bool lazy;                 /* made by the first draw over memory the voxel world carved out, written through copies and flushes only */
} C3D_Tex;

typedef struct
{
    void *colorBuf;
    void *depthBuf;
    u16 width, height;
    GPU_COLORBUF colorFmt;
    GPU_DEPTHBUF depthFmt;
    bool block32;
    u8 colorMask, depthMask;
} C3D_FrameBuf;

typedef struct C3D_RenderTarget_tag
{
    C3D_FrameBuf frameBuf;
    bool used;
    bool screen;                /* a display's target: the host's surface, not a texture */
    gfxScreen_t screenId;
    gfx3dSide_t side;
    u32 glFramebuffer, glDepth;
    C3D_Tex *tex;
} C3D_RenderTarget;

typedef struct
{
    u32 srcRgb, srcAlpha;       /* three 4-bit sources each */
    u32 opRgb, opAlpha;
    u16 funcRgb, funcAlpha;
    u32 color;
    u16 scaleRgb, scaleAlpha;
} C3D_TexEnv;

/* A vector as the PICA holds it: w first in memory (x last). */
typedef union { struct { float w, z, y, x; }; float c[4]; } C3D_FVec;
typedef union { C3D_FVec r[4]; float m[16]; } C3D_Mtx;
static inline C3D_FVec FVec4_New(float x, float y, float z, float w) { C3D_FVec v = { .w = w, .z = z, .y = y, .x = x }; return v; }
static inline C3D_FVec FVec4_Scale(C3D_FVec v, float s) { return FVec4_New(v.x * s, v.y * s, v.z * s, v.w * s); }

#define C3D_MAX_ATTRIBS 12
typedef struct
{
    u8 loaders;                 /* attributes loaded from the buffer, in order */
    struct { u8 index, format, count; } loader[C3D_MAX_ATTRIBS];
    struct { u8 index; } fixed[C3D_MAX_ATTRIBS];
    u8 fixedCount;
} C3D_AttrInfo;

typedef struct
{
    const void *data;
    u32 stride;
    u8 attribCount;
    u64 permutation;
} C3D_BufInfo;

#define C3D_FRAME_SYNCDRAW 1
#define C3D_FRAME_NONBLOCK 2
#define C3D_CLEAR_COLOR 1
#define C3D_CLEAR_DEPTH 2
#define C3D_CLEAR_ALL 3
#define C3D_DEFAULT_CMDBUF_SIZE 0x40000

#define C3D_RGB(r, g, b) (((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))

/* ── Init and frames ─────────────────────────────────────────────────────── */
bool C3D_Init(size_t cmdBufSize);
void C3D_Fini(void);
bool C3D_FrameBegin(u8 flags);
void C3D_FrameEnd(u8 flags);
void C3D_FrameSplit(u8 flags);
bool C3D_FrameDrawOn(C3D_RenderTarget *target);
float C3D_GetDrawingTime(void);
float C3D_GetCmdBufUsage(void);

/* ── Textures ────────────────────────────────────────────────────────────── */
bool C3D_TexInit(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format);
bool C3D_TexInitVRAM(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format);
void C3D_TexDelete(C3D_Tex *tex);
void C3D_TexBind(int unit, C3D_Tex *tex);
void C3D_TexFlush(C3D_Tex *tex);
/* Bytes [offset, offset + size) of the texels were written by the CPU: on the console the GPU read
 * them from the linear heap after a flush; here they are uploaded before the texture is drawn from. */
void C3D_TexTouch(C3D_Tex *tex, size_t offset, size_t size);
void C3D_TexSetFilter(C3D_Tex *tex, GPU_TEXTURE_FILTER_PARAM magFilter, GPU_TEXTURE_FILTER_PARAM minFilter);
void C3D_TexSetWrap(C3D_Tex *tex, GPU_TEXTURE_WRAP_PARAM wrapS, GPU_TEXTURE_WRAP_PARAM wrapT);
size_t C3D_TexCalcTotalSize(size_t size, int maxLevel);

/* ── Render targets ──────────────────────────────────────────────────────── */
C3D_RenderTarget *C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, GPU_DEPTHBUF depthFmt);
C3D_RenderTarget *C3D_RenderTargetCreateFromTex(C3D_Tex *tex, GPU_TEXFACE face, int level, GPU_DEPTHBUF depthFmt);
void C3D_RenderTargetDelete(C3D_RenderTarget *target);
void C3D_RenderTargetClear(C3D_RenderTarget *target, u8 clearBits, u32 clearColor, u32 clearDepth);
/* The target's pixels, rows top first, as RGBA8 bytes; false for a target that cannot be read. */
bool C3D_RenderTargetReadRGBA(C3D_RenderTarget *target, int width, int height, u32 *rgba);
void C3D_RenderTargetSetOutput(C3D_RenderTarget *target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags);

/* ── Pipeline state ──────────────────────────────────────────────────────── */
void C3D_SetScissor(GPU_SCISSORMODE mode, u32 left, u32 top, u32 right, u32 bottom);
void C3D_AlphaBlend(GPU_BLENDEQUATION colorEq, GPU_BLENDEQUATION alphaEq, GPU_BLENDFACTOR srcClr, GPU_BLENDFACTOR dstClr,
                    GPU_BLENDFACTOR srcAlpha, GPU_BLENDFACTOR dstAlpha);
void C3D_AlphaTest(bool enable, GPU_TESTFUNC function, int ref);
void C3D_DepthTest(bool enable, GPU_TESTFUNC function, GPU_WRITEMASK writemask);
void C3D_BlendingColor(u32 color);
void C3D_CullFace(GPU_CULLMODE mode);

/* ── The texture environment: six stages, as on the PICA ──────────────────── */
C3D_TexEnv *C3D_GetTexEnv(int id);
void C3D_SetTexEnv(int id, C3D_TexEnv *env);
void C3D_TexEnvInit(C3D_TexEnv *env);
void C3D_TexEnvSrc(C3D_TexEnv *env, C3D_TexEnvMode mode, GPU_TEVSRC s1, GPU_TEVSRC s2, GPU_TEVSRC s3);
void C3D_TexEnvOpRgb(C3D_TexEnv *env, GPU_TEVOP_RGB o1, GPU_TEVOP_RGB o2, GPU_TEVOP_RGB o3);
void C3D_TexEnvOpAlpha(C3D_TexEnv *env, GPU_TEVOP_A o1, GPU_TEVOP_A o2, GPU_TEVOP_A o3);
void C3D_TexEnvFunc(C3D_TexEnv *env, C3D_TexEnvMode mode, GPU_COMBINEFUNC param);
void C3D_TexEnvColor(C3D_TexEnv *env, u32 color);
void C3D_TexEnvScale(C3D_TexEnv *env, int mode, GPU_TEVSCALE scale);

/* ── Vertices ────────────────────────────────────────────────────────────── */
void AttrInfo_Init(C3D_AttrInfo *info);
int AttrInfo_AddLoader(C3D_AttrInfo *info, int regId, GPU_FORMATS format, int count);
int AttrInfo_AddFixed(C3D_AttrInfo *info, int regId);
void BufInfo_Init(C3D_BufInfo *info);
int BufInfo_Add(C3D_BufInfo *info, const void *data, ptrdiff_t stride, int attribCount, u64 permutation);
C3D_AttrInfo *C3D_GetAttrInfo(void);
void C3D_SetAttrInfo(C3D_AttrInfo *attrInfo);
C3D_BufInfo *C3D_GetBufInfo(void);
void C3D_SetBufInfo(C3D_BufInfo *bufInfo);
void C3D_FixedAttribSet(int id, float x, float y, float z, float w);
void C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void *indices);

void Mtx_Identity(C3D_Mtx *out);

/* ── Matrices and vectors (citro3d's math, right handed, in the PICA's clip space: x and y in
 *    [-w, w], z in [-w, 0] from the near plane to the far one) ────────────────────────────── */
typedef struct { float x, y, z; } C3D_FVec3;
static inline C3D_FVec3 FVec3_New(float x, float y, float z) { C3D_FVec3 v = { x, y, z }; return v; }
static inline float C3D_AngleFromDegrees(float degrees) { return degrees * 3.14159265358979f / 180.0f; }
static inline void Mtx_Zeros(C3D_Mtx *out) { memset(out, 0, sizeof(*out)); }
static inline void Mtx_Copy(C3D_Mtx *out, const C3D_Mtx *in) { *out = *in; }
static inline float FVec4_Dot(C3D_FVec a, C3D_FVec b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
void Mtx_Multiply(C3D_Mtx *out, const C3D_Mtx *a, const C3D_Mtx *b);
C3D_FVec Mtx_MultiplyFVec4(const C3D_Mtx *mtx, C3D_FVec v);
void Mtx_Translate(C3D_Mtx *mtx, float x, float y, float z, bool bRightSide);
void Mtx_Persp(C3D_Mtx *mtx, float fovy, float aspect, float near, float far, bool isLeftHanded);
void Mtx_Ortho(C3D_Mtx *mtx, float left, float right, float bottom, float top, float near, float far, bool isLeftHanded);
void Mtx_LookAt(C3D_Mtx *out, C3D_FVec3 cameraPosition, C3D_FVec3 cameraTarget, C3D_FVec3 cameraUpVector, bool isLeftHanded);

/* ── Shader programs. The voxel world's vertex shader (voxel.v.pica) is a GLSL twin inside
 *    c3d_gles.c: the binary is not read, its uniforms are the PICA's register numbers. ─────── */
typedef struct { int unused; } DVLE_s;
typedef struct { u32 numDVLE; DVLE_s *DVLE; } DVLB_s;
typedef struct { void *vertexShader; } shaderProgram_s;
DVLB_s *DVLB_ParseFile(const void *shbin, u32 size);
void DVLB_Free(DVLB_s *dvlb);
void shaderProgramInit(shaderProgram_s *program);
Result shaderProgramSetVsh(shaderProgram_s *program, DVLE_s *vsh);
Result shaderProgramFree(shaderProgram_s *program);
int shaderInstanceGetUniformLocation(void *instance, const char *name);
void C3D_BindProgram(shaderProgram_s *program);
void C3D_FVUnifSet(GPU_SHADER_TYPE type, int id, float x, float y, float z, float w);
void C3D_FVUnifMtx4x4(GPU_SHADER_TYPE type, int id, const C3D_Mtx *mtx);
void C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size);
/* d = zScale * (z / w) + zOffset is the depth the tests compare. */
void C3D_DepthMap(bool bIsZBuffer, float zScale, float zOffset);
void C3D_ColorLogicOp(GPU_LOGICOP op);

/* ── Memory the voxel world manages itself ─────────────────────────────────────────────────
 * VRAM is ordinary memory here; blocks drawn from as vertex buffers are mirrored in a GL buffer,
 * and the copies and cache flushes below say which bytes changed (of a texture's texels or of a
 * mirrored block). */
void *vramAlloc(size_t size);
void vramFree(void *memory);
void GSPGPU_FlushDataCache(const void *address, u32 size);
void C3D_SyncTextureCopy(const u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 size, u32 flags);

/* ctr_voxel.c asks the console for VRAM textures it fills itself: here they are CPU textures. */
#ifdef CTR_TEXINITVRAM_CPU
#define C3D_TexInitVRAM C3D_TexInit
#endif

/* ── What the console needs and the Thor does not ────────────────────────── */
/* Raw command list writes the compositor made to drop the texture cache and flush the
 * framebuffer: the driver does both. */
#define GPUREG_TEXUNIT_CONFIG 0
#define GPUREG_FRAMEBUFFER_FLUSH 1
#define GPUREG_FRAMEBUFFER_INVALIDATE 2
static inline void GPUCMD_AddWrite(u32 reg, u32 value) { (void)reg; (void)value; }
static inline void GPUCMD_AddMaskedWrite(u32 reg, u32 mask, u32 value) { (void)reg; (void)mask; (void)value; }

typedef struct { u16 maxEntries, numEntries, lastEntry; } gxCmdQueue_s;
#define GX_CMDLIST_FLUSH 1

#endif
