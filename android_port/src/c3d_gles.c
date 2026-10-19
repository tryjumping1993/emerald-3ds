/*
 * Citro3D and Citro2D (include/citro3d.h, citro2d.h) over OpenGL ES 3.0, so that the GPU
 * compositor (3ds_video.c), written against the PICA200's fixed function pipeline, runs on
 * the Thor as it is.
 *
 * Conventions kept from the PICA, so none of the compositor's geometry changes:
 *  - a scene is x right, y down, from the top left; the projection puts y = 0 at the top of
 *    the render target. A texture's v = 1 is its top: CPU-written textures are stored with
 *    their first memory row at v = 1 (the compositor writes them tile by tile, CtrVideo_Texel),
 *    so the upload flips the rows, and a render target drawn with the same projection reads
 *    back the same way;
 *  - colour is combined by six texture environment stages, evaluated by one shader that reads
 *    them as uniforms; Citro2D's own sprite and solid drawing is the first stage's job;
 *  - blending, alpha test, depth test, the colour write mask and the scissor are the PICA's
 *    state, set as the compositor sets them.
 *
 * Render targets are plain framebuffer objects (no 90 degree rotation, which the PICA's LCD
 * needed). The top screen is an off-screen target of an integer multiple of 400x240, shown
 * letterboxed on the top display when the frame ends.
 */
#include <GLES3/gl3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_log.h"
#include "3ds_platform.h"
#include "android_host.h"
#include "citro2d.h"

#define MAX_QUADS 8192
#define REGISTRY 96
#define SCREEN_W 400
#define SCREEN_H 240

/* ── State ───────────────────────────────────────────────────────────────── */

typedef struct
{
    float x, y, u, v;
    float blendTexture, blendTint, z;
    u32 color; /* RGBA bytes, normalised to 0..1 by the GL */
} BatchVertex;

typedef enum { MODE_RAW = 0, MODE_IMAGE = 1, MODE_SOLID = 2 } DrawMode;

static struct
{
    bool ready;
    GLuint program;
    GLint uRow0, uRow1, uScene, uTex, uMode, uStages, uSrc, uSrcA, uOp, uOpA, uConst, uAlphaFunc, uAlphaRef;
    GLuint vbo, ibo, rawVbo, rawIbo, vao, white;

    /* The frame and its targets. */
    bool inFrame;
    C3D_RenderTarget topTarget, rightTarget;
    GLuint topFbo, topTexture, topDepth;
    int topW, topH, topScale;
    GLuint blitFbo;
    C3D_RenderTarget *target;
    int viewportW, viewportH;
    int sceneW, sceneH;
    bool skip; /* drawing to a target that is not shown (the right eye, a screen with no surface) */

    /* The view: rows (a b tx) and (c d ty). */
    C3D_Mtx view;

    /* Pipeline state. */
    GPU_SCISSORMODE scissorMode;
    u32 scissor[4];
    bool blend;
    GPU_BLENDEQUATION colorEq, alphaEq;
    GPU_BLENDFACTOR srcColor, dstColor, srcAlpha, dstAlpha;
    u32 blendColor;
    bool alphaTest;
    GPU_TESTFUNC alphaFunc;
    int alphaRef;
    bool depthTest;
    GPU_TESTFUNC depthFunc;
    u8 writeMask;
    C3D_TexEnv env[6];
    C3D_Tex *bound;

    /* The voxel world's program (voxel.v.pica's GLSL twin) and what it draws with: three texture
     * units, the vertex shader's register file, the depth mapping. */
    GLuint voxProgram, voxVao, voxStream;
    GLint voxReg, voxDepth, voxTex[3], voxStages, voxSrc, voxSrcA, voxOp, voxOpA, voxConst, voxAlphaFunc, voxAlphaRef;
    float regs[32][4];
    float depthScale, depthOffset;
    bool voxelBound;
    u32 frameNo;

    /* Attributes for raw draws. */
    C3D_AttrInfo attrInfo;
    C3D_BufInfo bufInfo;
    float fixed[C3D_MAX_ATTRIBS][4];

    /* The Citro2D batch. */
    BatchVertex vertices[MAX_QUADS * 4];
    unsigned quads;
    DrawMode mode;
    C3D_Tex *batchTexture;
} G;

static void FlushBatch(void);
static bool BuildVoxelProgram(void);
static void RecordBatch(void);
static void RecordRaw(const u8 *vertices, size_t vertexBytes, const void *indices, int count, int type);
static void RecordClear(C3D_RenderTarget *target, u8 bits, u32 color, u32 depth);
static void ReplayFrameBegin(void);
static void ReplayFrameEnd(void);
static void DrawBatchNow(const BatchVertex *vertices, unsigned quads);
static void DrawRawNow(const u8 *vertices, size_t vertexBytes, const void *indices, int count, int type);
static void DrawVoxelElements(const u8 *vertices, unsigned stride, unsigned vertexCount, const void *indices, int count, int type);
static void ShowTop(void);

static C3D_Tex *sTextures[REGISTRY];
static C3D_RenderTarget *sTargets[REGISTRY];

/* Draws per target, summarised in the log every few seconds (a debugging aid). */
static unsigned sDrawCount[REGISTRY + 2];
static unsigned sRawCount[REGISTRY + 2];

static void CountDraw(const C3D_RenderTarget *target, bool raw, unsigned quads)
{
    unsigned index = REGISTRY + 1;

    if (target != NULL && target->screen)
        index = REGISTRY;
    else
        for (unsigned i = 0; i < REGISTRY; ++i)
            if (sTargets[i] == target)
                index = i;
    (raw ? sRawCount : sDrawCount)[index] += quads;
}

/* GL errors are logged the first few times, with where they came from. */
static void CheckError(const char *where)
{
    static unsigned reported;
    GLenum error = glGetError();

    if (error != GL_NO_ERROR && reported < 40)
    {
        ++reported;
        CtrLog_Write(CTR_LOG_ERROR, "c3d: GL error 0x%x after %s", error, where);
    }
}

/* Every live texture and render target, for the debug dump (DumpAll). */

static void Register(void **table, void *item)
{
    for (int i = 0; i < REGISTRY; ++i)
        if (table[i] == NULL)
        {
            table[i] = item;
            return;
        }
}

static void Unregister(void **table, void *item)
{
    for (int i = 0; i < REGISTRY; ++i)
        if (table[i] == item)
            table[i] = NULL;
}

/* ── Shaders ─────────────────────────────────────────────────────────────── */

static const char kVertexShader[] =
    "#version 300 es\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aUv;\n"
    "layout(location = 2) in vec3 aBlend;\n"
    "layout(location = 3) in vec4 aColor;\n"
    "uniform vec3 uRow0;\n"
    "uniform vec3 uRow1;\n"
    "uniform vec2 uScene;\n"
    "out vec2 vUv;\n"
    "out vec3 vBlend;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "    vec3 q = vec3(aPos, 1.0);\n"
    "    vec2 p = vec2(dot(uRow0, q), dot(uRow1, q));\n"
    "    gl_Position = vec4(p.x / uScene.x * 2.0 - 1.0, 1.0 - p.y / uScene.y * 2.0, clamp(aBlend.z, -1.0, 1.0), 1.0);\n"
    "    vUv = aUv;\n"
    "    vBlend = aBlend;\n"
    "    vColor = aColor;\n"
    "}\n";

/* The PICA's texture environment: six stages, each combining up to three sources with a
 * function, per colour and per alpha, scaled and clamped. Sources: 0 primary colour, 3 the
 * texture, 14 the stage's constant, 15 the previous stage. */
static const char kFragmentShader[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uTex;\n"
    "uniform int uMode;\n"
    "uniform int uStages;\n"
    "uniform ivec4 uSrc[6];\n"   /* srcRgb 0..2, funcRgb */
    "uniform ivec4 uSrcA[6];\n"  /* srcAlpha 0..2, funcAlpha */
    "uniform ivec4 uOp[6];\n"    /* opRgb 0..2, scaleRgb (1, 2, 4) */
    "uniform ivec4 uOpA[6];\n"   /* opAlpha 0..2, scaleAlpha */
    "uniform vec4 uConst[6];\n"
    "uniform int uAlphaFunc;\n"
    "uniform float uAlphaRef;\n"
    "in vec2 vUv;\n"
    "in vec3 vBlend;\n"
    "in vec4 vColor;\n"
    "out vec4 fragColor;\n"
    "vec4 source(int s, vec4 prev, vec4 tex, vec4 cst) {\n"
    "    if (s == 3) return tex;\n"
    "    if (s == 14) return cst;\n"
    "    if (s == 15) return prev;\n"
    "    return vColor;\n"
    "}\n"
    "vec3 operandRgb(int op, vec4 v) {\n"
    "    if (op == 0) return v.rgb;\n"
    "    if (op == 1) return 1.0 - v.rgb;\n"
    "    if (op == 2) return vec3(v.a);\n"
    "    if (op == 3) return vec3(1.0 - v.a);\n"
    "    if (op == 4) return vec3(v.r);\n"
    "    if (op == 5) return vec3(1.0 - v.r);\n"
    "    if (op == 8) return vec3(v.g);\n"
    "    if (op == 9) return vec3(1.0 - v.g);\n"
    "    if (op == 12) return vec3(v.b);\n"
    "    if (op == 13) return vec3(1.0 - v.b);\n"
    "    return v.rgb;\n"
    "}\n"
    "float operandA(int op, vec4 v) {\n"
    "    if (op == 0) return v.a;\n"
    "    if (op == 1) return 1.0 - v.a;\n"
    "    if (op == 2) return v.r;\n"
    "    if (op == 3) return 1.0 - v.r;\n"
    "    if (op == 4) return v.g;\n"
    "    if (op == 5) return 1.0 - v.g;\n"
    "    if (op == 6) return v.b;\n"
    "    if (op == 7) return 1.0 - v.b;\n"
    "    return v.a;\n"
    "}\n"
    "vec3 combine(int f, vec3 a, vec3 b, vec3 c) {\n"
    "    if (f == 0) return a;\n"
    "    if (f == 1) return a * b;\n"
    "    if (f == 2) return a + b;\n"
    "    if (f == 3) return a + b - 0.5;\n"
    "    if (f == 4) return a * c + b * (1.0 - c);\n"
    "    if (f == 5) return a - b;\n"
    "    if (f == 6 || f == 7) return vec3(4.0 * dot(a - 0.5, b - 0.5));\n"
    "    if (f == 8) return a * b + c;\n"
    "    return (a + b) * c;\n"
    "}\n"
    "float combineA(int f, float a, float b, float c) {\n"
    "    if (f == 0) return a;\n"
    "    if (f == 1) return a * b;\n"
    "    if (f == 2) return a + b;\n"
    "    if (f == 3) return a + b - 0.5;\n"
    "    if (f == 4) return a * c + b * (1.0 - c);\n"
    "    if (f == 5) return a - b;\n"
    "    if (f == 8) return a * b + c;\n"
    "    if (f == 9) return (a + b) * c;\n"
    "    return a;\n"
    "}\n"
    "void main() {\n"
    "    vec4 tex = vec4(1.0);\n"
    "    if (uMode != 2) tex = texture(uTex, vUv);\n"
    "    vec4 prev = vColor;\n"
    "    if (uMode == 1) prev = vec4(mix(tex.rgb, vColor.rgb, vBlend.y), tex.a * vColor.a);\n"
    "    else if (uMode == 2) prev = vColor;\n"
    "    for (int i = (uMode == 0 ? 0 : 1); i < uStages; ++i) {\n"
    "        vec4 cst = uConst[i];\n"
    "        vec3 a = operandRgb(uOp[i].x, source(uSrc[i].x, prev, tex, cst));\n"
    "        vec3 b = operandRgb(uOp[i].y, source(uSrc[i].y, prev, tex, cst));\n"
    "        vec3 c = operandRgb(uOp[i].z, source(uSrc[i].z, prev, tex, cst));\n"
    "        float aa = operandA(uOpA[i].x, source(uSrcA[i].x, prev, tex, cst));\n"
    "        float ab = operandA(uOpA[i].y, source(uSrcA[i].y, prev, tex, cst));\n"
    "        float ac = operandA(uOpA[i].z, source(uSrcA[i].z, prev, tex, cst));\n"
    "        vec3 rgb = clamp(combine(uSrc[i].w, a, b, c) * float(uOp[i].w), 0.0, 1.0);\n"
    "        float alpha = clamp(combineA(uSrcA[i].w, aa, ab, ac) * float(uOpA[i].w), 0.0, 1.0);\n"
    "        if (uSrc[i].w == 7) alpha = rgb.r;\n"
    "        prev = vec4(rgb, alpha);\n"
    "    }\n"
    "    if (uAlphaFunc != 1) {\n"
    "        float ref = uAlphaRef;\n"
    "        float al = floor(prev.a * 255.0 + 0.5) / 255.0;\n"
    "        bool pass = uAlphaFunc == 0 ? false\n"
    "                  : uAlphaFunc == 2 ? al == ref\n"
    "                  : uAlphaFunc == 3 ? al != ref\n"
    "                  : uAlphaFunc == 4 ? al < ref\n"
    "                  : uAlphaFunc == 5 ? al <= ref\n"
    "                  : uAlphaFunc == 6 ? al > ref : al >= ref;\n"
    "        if (!pass) discard;\n"
    "    }\n"
    "    fragColor = prev;\n"
    "}\n";

static GLuint Compile(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024];

        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        CtrLog_Write(CTR_LOG_ERROR, "c3d: shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool BuildProgram(void)
{
    GLuint vs = Compile(GL_VERTEX_SHADER, kVertexShader), fs = Compile(GL_FRAGMENT_SHADER, kFragmentShader);
    GLint ok = 0;

    if (vs == 0 || fs == 0)
        return false;
    G.program = glCreateProgram();
    glAttachShader(G.program, vs);
    glAttachShader(G.program, fs);
    glLinkProgram(G.program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(G.program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024];

        glGetProgramInfoLog(G.program, sizeof(log), NULL, log);
        CtrLog_Write(CTR_LOG_ERROR, "c3d: program link failed: %s", log);
        return false;
    }
    G.uRow0 = glGetUniformLocation(G.program, "uRow0");
    G.uRow1 = glGetUniformLocation(G.program, "uRow1");
    G.uScene = glGetUniformLocation(G.program, "uScene");
    G.uTex = glGetUniformLocation(G.program, "uTex");
    G.uMode = glGetUniformLocation(G.program, "uMode");
    G.uStages = glGetUniformLocation(G.program, "uStages");
    G.uSrc = glGetUniformLocation(G.program, "uSrc");
    G.uSrcA = glGetUniformLocation(G.program, "uSrcA");
    G.uOp = glGetUniformLocation(G.program, "uOp");
    G.uOpA = glGetUniformLocation(G.program, "uOpA");
    G.uConst = glGetUniformLocation(G.program, "uConst");
    G.uAlphaFunc = glGetUniformLocation(G.program, "uAlphaFunc");
    G.uAlphaRef = glGetUniformLocation(G.program, "uAlphaRef");
    return true;
}

/* ── Texture formats ─────────────────────────────────────────────────────── */

typedef struct
{
    GLenum internal, format, type;
    unsigned memory; /* bytes per texel in the tiles the compositor writes */
    unsigned upload; /* bytes per texel as uploaded */
} TexFormat;

static TexFormat FormatOf(GPU_TEXCOLOR fmt)
{
    switch (fmt)
    {
    case GPU_RGBA8: return (TexFormat){ GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 4 };
    case GPU_RGB8: return (TexFormat){ GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, 3, 3 };
    case GPU_RGBA5551: return (TexFormat){ GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2, 2 };
    case GPU_RGB565: return (TexFormat){ GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, 2 };
    case GPU_RGBA4: return (TexFormat){ GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2, 2 };
    case GPU_A8: return (TexFormat){ GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1, 4 };
    case GPU_L8: return (TexFormat){ GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, 1 };
    default: return (TexFormat){ GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 4 };
    }
}

static GLenum FilterOf(u8 filter) { return filter == GPU_LINEAR ? GL_LINEAR : GL_NEAREST; }

static GLenum WrapOf(u8 wrap)
{
    switch (wrap)
    {
    case GPU_REPEAT: return GL_REPEAT;
    case GPU_MIRRORED_REPEAT: return GL_MIRRORED_REPEAT;
    default: return GL_CLAMP_TO_EDGE;
    }
}

/* ── Setup ───────────────────────────────────────────────────────────────── */

/* Citro2D owns stage 0 until the compositor re-initialises it: drawn with, it is the sprite's
 * (texture mixed with a tint) or the solid's, and raw draws that leave it alone get the same. */
#define ENV_C2D 100

static void SetC2dEnv(void)
{
    G.voxelBound = false;
    G.env[0].srcRgb = G.env[0].srcAlpha = GPU_PREVIOUS;
    G.env[0].opRgb = G.env[0].opAlpha = 0;
    G.env[0].funcRgb = ENV_C2D;
    G.env[0].funcAlpha = GPU_REPLACE;
    G.env[0].color = 0xFFFFFFFFu;
    G.env[0].scaleRgb = G.env[0].scaleAlpha = GPU_TEVSCALE_1;
}

static void TexEnvDefault(C3D_TexEnv *env)
{
    env->srcRgb = env->srcAlpha = GPU_PREVIOUS;
    env->opRgb = env->opAlpha = 0;
    env->funcRgb = env->funcAlpha = GPU_REPLACE;
    env->color = 0xFFFFFFFFu;
    env->scaleRgb = env->scaleAlpha = GPU_TEVSCALE_1;
}

bool C3D_Init(size_t cmdBufSize)
{
    unsigned short indices[MAX_QUADS * 6];

    (void)cmdBufSize;
    AndroidHost_GlMakeCurrent();
    memset(&G, 0, sizeof(G));
    if (!BuildProgram())
        return false;
    G.depthScale = -1.0f;
    G.depthOffset = 0.0f;
    BuildVoxelProgram();
    for (unsigned q = 0; q < MAX_QUADS; ++q)
    {
        unsigned short base = (unsigned short)(q * 4);
        unsigned short *out = indices + q * 6;

        out[0] = base; out[1] = base + 1; out[2] = base + 2;
        out[3] = base + 2; out[4] = base + 1; out[5] = base + 3;
    }
    glGenVertexArrays(1, &G.vao);
    glGenBuffers(1, &G.vbo);
    glGenBuffers(1, &G.ibo);
    glGenBuffers(1, &G.rawVbo);
    glGenBuffers(1, &G.rawIbo);
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glGenFramebuffers(1, &G.blitFbo);
    /* A white texel, bound where the shader samples nothing (solid shapes). */
    glGenTextures(1, &G.white);
    glBindTexture(GL_TEXTURE_2D, G.white);
    {
        static const u32 one = 0xFFFFFFFFu;

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &one);
    }
    for (int i = 0; i < 6; ++i)
        TexEnvDefault(&G.env[i]);
    SetC2dEnv();
    Mtx_Identity(&G.view);
    G.blend = true;
    G.colorEq = G.alphaEq = GPU_BLEND_ADD;
    G.srcColor = G.srcAlpha = GPU_SRC_ALPHA;
    G.dstColor = G.dstAlpha = GPU_ONE_MINUS_SRC_ALPHA;
    G.alphaFunc = GPU_ALWAYS;
    G.depthFunc = GPU_GEQUAL;
    G.writeMask = GPU_WRITE_ALL;
    G.topTarget.screen = true;
    G.topTarget.screenId = GFX_TOP;
    G.topTarget.side = GFX_LEFT;
    G.topTarget.frameBuf.width = SCREEN_H;
    G.topTarget.frameBuf.height = SCREEN_W;
    G.rightTarget = G.topTarget;
    G.rightTarget.side = GFX_RIGHT;
    G.ready = true;
    return true;
}

void C3D_Fini(void)
{
    G.ready = false;
}

bool C2D_Init(size_t maxObjects)
{
    (void)maxObjects;
    return G.ready;
}

void C2D_Fini(void) {}

/* ── Textures ────────────────────────────────────────────────────────────── */

static void TexApplyParams(C3D_Tex *tex)
{
    if (tex->glTexture == 0)
        return;
    glBindTexture(GL_TEXTURE_2D, tex->glTexture);
    if (tex->fmt == GPU_L8)
    {
        /* Luminance: the one channel is red, green and blue; alpha is opaque. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, FilterOf(tex->filterMag));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, FilterOf(tex->filterMin));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, WrapOf(tex->wrapS));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, WrapOf(tex->wrapT));
}

/* A texture only the GPU holds still has a data pointer, as in VRAM: the compositor tests it to know the texture exists. */
static char sGpuMarker;

static bool TexCreate(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format, bool withMemory, unsigned scale)
{
    TexFormat f = FormatOf(format);
    GLuint name = 0;

    memset(tex, 0, sizeof(*tex));
    tex->fmt = format;
    tex->width = width;
    tex->height = height;
    tex->size = (size_t)width * height * f.memory;
    if (!withMemory)
        tex->data = &sGpuMarker;
    if (withMemory)
    {
        tex->data = calloc(1, tex->size);
        tex->dirtyTiles = calloc(1, ((size_t)(width / 8) * (height / 8) + 7) / 8);
        if (tex->data == NULL || tex->dirtyTiles == NULL)
            return false;
    }
    glGenTextures(1, &name);
    tex->glTexture = name;
    glBindTexture(GL_TEXTURE_2D, name);
    tex->gpuScale = (u8)(scale > 1 ? scale : 1);
    glTexStorage2D(GL_TEXTURE_2D, 1, f.internal, width * tex->gpuScale, height * tex->gpuScale);
    tex->filterMag = tex->filterMin = GPU_NEAREST;
    tex->wrapS = tex->wrapT = GPU_CLAMP_TO_EDGE;
    TexApplyParams(tex);
    tex->dirtyFirstRow = tex->dirtyEndRow = 0;
    Register((void **)sTextures, tex);
    CtrLog_Write(CTR_LOG_VIDEO, "c3d: texture gl=%u %ux%u fmt=%d %s", name, width, height, (int)format, withMemory ? "cpu" : "gpu");
    return true;
}

bool C3D_TexInit(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format)
{
    return TexCreate(tex, width, height, format, true, 1);
}

bool C3D_TexInitVRAM(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format)
{
    return TexCreate(tex, width, height, format, false, 1);
}

bool C3D_TexInitVRAMScaled(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format, unsigned scale)
{
    return TexCreate(tex, width, height, format, false, scale);
}

static void FreeShadow(C3D_Tex *tex);

void C3D_TexDelete(C3D_Tex *tex)
{
    Unregister((void **)sTextures, tex);
    if (tex->glTexture != 0)
    {
        GLuint name = tex->glTexture;

        glDeleteTextures(1, &name);
    }
    FreeShadow(tex);
    if (tex->data != &sGpuMarker)
        free(tex->data);
    free(tex->dirtyTiles);
    memset(tex, 0, sizeof(*tex));
}

void C3D_TexSetFilter(C3D_Tex *tex, GPU_TEXTURE_FILTER_PARAM magFilter, GPU_TEXTURE_FILTER_PARAM minFilter)
{
    tex->filterMag = (u8)magFilter;
    tex->filterMin = (u8)minFilter;
    TexApplyParams(tex);
}

void C3D_TexSetWrap(C3D_Tex *tex, GPU_TEXTURE_WRAP_PARAM wrapS, GPU_TEXTURE_WRAP_PARAM wrapT)
{
    tex->wrapS = (u8)wrapS;
    tex->wrapT = (u8)wrapT;
    TexApplyParams(tex);
}

size_t C3D_TexCalcTotalSize(size_t size, int maxLevel)
{
    (void)maxLevel;
    return size;
}

/*
 * A texture the CPU writes tile by tile, 8x8 texels to a tile in rows (CtrVideo_Texel), tiles
 * left to right and then down from the first memory row. The GL wants rows, the last memory row
 * first. A shadow copy of the texels last sent says which tiles changed, so a frame that
 * changes a few sprites' tiles converts and uploads a few rows.
 */
typedef struct
{
    C3D_Tex *tex;
    u8 *shadow;
    u8 *linear;
    bool uploaded;
} Shadow;

static Shadow sShadow[160];

static Shadow *ShadowFor(C3D_Tex *tex)
{
    Shadow *free = NULL;

    for (unsigned i = 0; i < sizeof(sShadow) / sizeof(sShadow[0]); ++i)
    {
        if (sShadow[i].tex == tex)
            return &sShadow[i];
        if (sShadow[i].tex == NULL && free == NULL)
            free = &sShadow[i];
    }
    if (free != NULL)
        free->tex = tex;
    return free;
}

static void FreeShadow(C3D_Tex *tex)
{
    for (unsigned i = 0; i < sizeof(sShadow) / sizeof(sShadow[0]); ++i)
        if (sShadow[i].tex == tex)
        {
            free(sShadow[i].shadow);
            free(sShadow[i].linear);
            memset(&sShadow[i], 0, sizeof(sShadow[i]));
        }
}

static inline void ConvertTexel(const u8 *source, u8 *destination, GPU_TEXCOLOR fmt)
{
    switch (fmt)
    {
    case GPU_RGBA8:
        /* ABGR bytes in memory (a word RRGGBBAA) to bytes R, G, B, A. */
        destination[0] = source[3];
        destination[1] = source[2];
        destination[2] = source[1];
        destination[3] = source[0];
        break;
    case GPU_A8:
        destination[0] = destination[1] = destination[2] = 0;
        destination[3] = source[0];
        break;
    case GPU_L8:
        destination[0] = source[0];
        break;
    case GPU_RGB8:
        destination[0] = source[2];
        destination[1] = source[1];
        destination[2] = source[0];
        break;
    default:
        destination[0] = source[0];
        destination[1] = source[1];
        break;
    }
}

static void TexUpload(C3D_Tex *tex, bool fullScan)
{
    TexFormat f;
    Shadow *shadow;
    unsigned tilesAcross, tilesDown, tileBytes;
    unsigned firstRow = ~0u, lastRow = 0;

    if (tex->dirtyTiles == NULL || tex->glTexture == 0)
        return;
    f = FormatOf(tex->fmt);
    shadow = ShadowFor(tex);
    if (shadow == NULL)
        return;
    tilesAcross = tex->width / 8;
    tilesDown = tex->height / 8;
    tileBytes = 64 * f.memory;
    if (shadow->shadow == NULL)
    {
        shadow->shadow = calloc(1, tex->size);
        shadow->linear = calloc(1, (size_t)tex->width * tex->height * f.upload);
        if (shadow->shadow == NULL || shadow->linear == NULL)
            return;
    }
    for (unsigned ty = 0; ty < tilesDown; ++ty)
    {
        for (unsigned tx = 0; tx < tilesAcross; ++tx)
        {
            size_t offset = ((size_t)ty * tilesAcross + tx) * tileBytes;
            const u8 *source = (const u8 *)tex->data + offset;

            size_t tile = (size_t)ty * tilesAcross + tx;
            bool marked = tex->dirtyTiles != NULL && (tex->dirtyTiles[tile >> 3] & (1u << (tile & 7))) != 0;

            if (shadow->uploaded && !marked && (!fullScan || memcmp(source, shadow->shadow + offset, tileBytes) == 0))
                continue;
            memcpy(shadow->shadow + offset, source, tileBytes);
            for (unsigned row = 0; row < 8; ++row)
            {
                unsigned memoryRow = ty * 8 + row;
                unsigned glRow = tex->height - 1 - memoryRow;
                u8 *destination = shadow->linear + ((size_t)glRow * tex->width + tx * 8) * f.upload;
                const u8 *rowSource = source + (size_t)row * 8 * f.memory;

                for (unsigned x = 0; x < 8; ++x)
                    ConvertTexel(rowSource + x * f.memory, destination + x * f.upload, tex->fmt);
                if (glRow < firstRow)
                    firstRow = glRow;
                if (glRow > lastRow)
                    lastRow = glRow;
            }
        }
    }
    if (!shadow->uploaded)
    {
        firstRow = 0;
        lastRow = tex->height - 1;
        shadow->uploaded = true;
    }
    if (tex->dirtyTiles != NULL)
        memset(tex->dirtyTiles, 0, ((size_t)tilesAcross * tilesDown + 7) / 8);
    tex->touched = false;
    if (firstRow > lastRow)
        return;
    glBindTexture(GL_TEXTURE_2D, tex->glTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)firstRow, tex->width, (GLsizei)(lastRow - firstRow + 1), f.format, f.type,
                    shadow->linear + (size_t)firstRow * tex->width * f.upload);
}

void C3D_TexFlush(C3D_Tex *tex) { TexUpload(tex, true); }

void C3D_TexTouch(C3D_Tex *tex, size_t offset, size_t size)
{
    size_t tileBytes = 64 * FormatOf(tex->fmt).memory, first = offset / tileBytes, last = (offset + size - 1) / tileBytes;

    if (tex->dirtyTiles == NULL)
        return;
    for (size_t tile = first; tile <= last; ++tile)
        tex->dirtyTiles[tile >> 3] |= (u8)(1u << (tile & 7));
    tex->touched = true;
}

/* ctr_voxel.c turns a texture unit off by writing citro3d's own context (the unit's slot, at
 * C3DI_CONTEXT_TEX): the bound textures live in this array, in that layout, so that it can. */
uint8_t __C3D_Context[0x400];
#define CONTEXT_UNITS ((C3D_Tex **)(void *)(__C3D_Context + 0x118))

void C3D_TexBind(int unit, C3D_Tex *tex)
{
    if (unit >= 0 && unit < 3)
        CONTEXT_UNITS[unit] = tex;
    if (unit == 0)
        G.bound = tex;
}

/* ── Render targets ──────────────────────────────────────────────────────── */

static C3D_RenderTarget *TargetMake(int width, int height, C3D_Tex *tex, GPU_DEPTHBUF depthFmt, GPU_COLORBUF colorFmt)
{
    C3D_RenderTarget *target = calloc(1, sizeof(*target));
    GLenum status;

    if (target == NULL)
        return NULL;
    target->frameBuf.width = (u16)width;
    target->frameBuf.height = (u16)height;
    target->glScale = tex->gpuScale > 1 ? tex->gpuScale : 1;
    target->frameBuf.colorFmt = colorFmt;
    target->frameBuf.depthFmt = depthFmt;
    target->tex = tex;
    glGenFramebuffers(1, &target->glFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target->glFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex->glTexture, 0);
    if ((int)depthFmt >= 0)
    {
        glGenRenderbuffers(1, &target->glDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, target->glDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, depthFmt == GPU_RB_DEPTH16 ? GL_DEPTH_COMPONENT16 : GL_DEPTH_COMPONENT24,
                              width * (int)target->glScale, height * (int)target->glScale);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target->glDepth);
    }
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE)
    {
        /* A new target holds whatever the memory held; the console cleared each when it was made. */
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(0, 0, 0, 0);
        glClearDepthf(0.0f);
        glClear(GL_COLOR_BUFFER_BIT | (target->glDepth != 0 ? GL_DEPTH_BUFFER_BIT : 0));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    Register((void **)sTargets, target);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        CtrLog_Write(CTR_LOG_ERROR, "c3d: framebuffer %dx%d incomplete (0x%x)", width, height, status);
        if (target->glDepth != 0)
            glDeleteRenderbuffers(1, &target->glDepth);
        glDeleteFramebuffers(1, &target->glFramebuffer);
        free(target);
        return NULL;
    }
    return target;
}

C3D_RenderTarget *C3D_RenderTargetCreateFromTex(C3D_Tex *tex, GPU_TEXFACE face, int level, GPU_DEPTHBUF depthFmt)
{
    (void)face;
    (void)level;
    tex->linked = true;
    return TargetMake(tex->width, tex->height, tex, depthFmt, GPU_RB_RGBA8);
}

C3D_RenderTarget *C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, GPU_DEPTHBUF depthFmt)
{
    GPU_TEXCOLOR texFmt = colorFmt == GPU_RB_RGB565 ? GPU_RGB565 : colorFmt == GPU_RB_RGBA5551 ? GPU_RGBA5551
                        : colorFmt == GPU_RB_RGBA4 ? GPU_RGBA4 : GPU_RGBA8;
    C3D_Tex *tex = malloc(sizeof(*tex));
    C3D_RenderTarget *target;

    if (tex == NULL || !TexCreate(tex, (u16)width, (u16)height, texFmt, false, 1))
    {
        free(tex);
        return NULL;
    }
    tex->linked = true;
    target = TargetMake(width, height, tex, depthFmt, colorFmt);
    if (target == NULL)
    {
        C3D_TexDelete(tex);
        free(tex);
        return NULL;
    }
    target->bottom = colorFmt == GPU_RB_RGB565;
    /* This target owns the texture it draws into: C3D_RenderTargetDelete frees it. */
    target->screenId = (gfxScreen_t)-1;
    return target;
}

void C3D_RenderTargetDelete(C3D_RenderTarget *target)
{
    if (target == NULL || target->screen)
        return;
    Unregister((void **)sTargets, target);
    if (G.target == target)
        G.target = NULL;
    glDeleteFramebuffers(1, &target->glFramebuffer);
    if (target->glDepth != 0)
        glDeleteRenderbuffers(1, &target->glDepth);
    if ((int)target->screenId == -1)
    {
        C3D_TexDelete(target->tex);
        free(target->tex);
    }
    free(target);
}

void C3D_RenderTargetSetOutput(C3D_RenderTarget *target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags)
{
    (void)target;
    (void)screen;
    (void)side;
    (void)transferFlags;
}

bool C3D_RenderTargetReadRGBA(C3D_RenderTarget *target, int width, int height, u32 *rgba)
{
    static u32 bottomUp[512 * 512];

    FlushBatch();
    if (target == NULL || target->screen || width <= 0 || height <= 0 || width > 512 || height > 512)
        return false;
    glBindFramebuffer(GL_FRAMEBUFFER, target->glFramebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int y = 0; y < height; ++y)
        memcpy(rgba + (size_t)y * width, bottomUp + (size_t)(height - 1 - y) * width, (size_t)width * sizeof(u32));
    return true;
}

C3D_RenderTarget *C2D_CreateScreenTarget(gfxScreen_t screen, gfx3dSide_t side)
{
    if (screen != GFX_TOP)
        return NULL;
    return side == GFX_LEFT ? &G.topTarget : &G.rightTarget;
}

/* The top screen's off-screen target: an integer multiple of the 400x240 picture that fits the
 * display, shown letterboxed at the end of the frame. */
static bool TopEnsure(int surfaceW, int surfaceH)
{
    int scale = surfaceW / SCREEN_W < surfaceH / SCREEN_H ? surfaceW / SCREEN_W : surfaceH / SCREEN_H;

    if (scale < 1)
        scale = 1;
    if (G.topFbo != 0 && G.topScale == scale)
        return true;
    if (G.topFbo != 0)
    {
        glDeleteFramebuffers(1, &G.topFbo);
        glDeleteTextures(1, &G.topTexture);
        glDeleteRenderbuffers(1, &G.topDepth);
    }
    G.topScale = scale;
    G.topW = SCREEN_W * scale;
    G.topH = SCREEN_H * scale;
    glGenTextures(1, &G.topTexture);
    glBindTexture(GL_TEXTURE_2D, G.topTexture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, G.topW, G.topH);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenRenderbuffers(1, &G.topDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, G.topDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, G.topW, G.topH);
    glGenFramebuffers(1, &G.topFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, G.topFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.topTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, G.topDepth);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClearDepthf(0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    CtrLog_Write(CTR_LOG_VIDEO, "c3d: top screen target %dx%d (x%d) for a %dx%d display", G.topW, G.topH, scale, surfaceW, surfaceH);
    return true;
}

/* ── Pipeline state ──────────────────────────────────────────────────────── */

void C3D_SetScissor(GPU_SCISSORMODE mode, u32 left, u32 top, u32 right, u32 bottom)
{
    C2D_Flush();
    G.scissorMode = mode;
    G.scissor[0] = left;
    G.scissor[1] = top;
    G.scissor[2] = right;
    G.scissor[3] = bottom;
}

void C3D_AlphaBlend(GPU_BLENDEQUATION colorEq, GPU_BLENDEQUATION alphaEq, GPU_BLENDFACTOR srcClr, GPU_BLENDFACTOR dstClr,
                    GPU_BLENDFACTOR srcAlpha, GPU_BLENDFACTOR dstAlpha)
{
    C2D_Flush();
    G.colorEq = colorEq;
    G.alphaEq = alphaEq;
    G.srcColor = srcClr;
    G.dstColor = dstClr;
    G.srcAlpha = srcAlpha;
    G.dstAlpha = dstAlpha;
    G.blend = true;
}

void C3D_AlphaTest(bool enable, GPU_TESTFUNC function, int ref)
{
    C2D_Flush();
    G.alphaTest = enable;
    G.alphaFunc = function;
    G.alphaRef = ref;
}

void C3D_DepthTest(bool enable, GPU_TESTFUNC function, GPU_WRITEMASK writemask)
{
    C2D_Flush();
    G.depthTest = enable;
    G.depthFunc = function;
    G.writeMask = (u8)writemask;
}

void C3D_BlendingColor(u32 color)
{
    C2D_Flush();
    G.blendColor = color;
}

void C3D_CullFace(GPU_CULLMODE mode) { (void)mode; }

static GLenum BlendFactor(GPU_BLENDFACTOR f)
{
    switch (f)
    {
    case GPU_ZERO: return GL_ZERO;
    case GPU_ONE: return GL_ONE;
    case GPU_SRC_COLOR: return GL_SRC_COLOR;
    case GPU_ONE_MINUS_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
    case GPU_DST_COLOR: return GL_DST_COLOR;
    case GPU_ONE_MINUS_DST_COLOR: return GL_ONE_MINUS_DST_COLOR;
    case GPU_SRC_ALPHA: return GL_SRC_ALPHA;
    case GPU_ONE_MINUS_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case GPU_DST_ALPHA: return GL_DST_ALPHA;
    case GPU_ONE_MINUS_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case GPU_CONSTANT_COLOR: return GL_CONSTANT_COLOR;
    case GPU_ONE_MINUS_CONSTANT_COLOR: return GL_ONE_MINUS_CONSTANT_COLOR;
    case GPU_CONSTANT_ALPHA: return GL_CONSTANT_ALPHA;
    case GPU_ONE_MINUS_CONSTANT_ALPHA: return GL_ONE_MINUS_CONSTANT_ALPHA;
    default: return GL_SRC_ALPHA_SATURATE;
    }
}

static GLenum BlendEquation(GPU_BLENDEQUATION e)
{
    switch (e)
    {
    case GPU_BLEND_SUBTRACT: return GL_FUNC_SUBTRACT;
    case GPU_BLEND_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
    case GPU_BLEND_MIN: return GL_MIN;
    case GPU_BLEND_MAX: return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}

static GLenum DepthFunc(GPU_TESTFUNC f)
{
    switch (f)
    {
    case GPU_NEVER: return GL_NEVER;
    case GPU_ALWAYS: return GL_ALWAYS;
    case GPU_EQUAL: return GL_EQUAL;
    case GPU_NOTEQUAL: return GL_NOTEQUAL;
    case GPU_LESS: return GL_LESS;
    case GPU_LEQUAL: return GL_LEQUAL;
    case GPU_GREATER: return GL_GREATER;
    default: return GL_GEQUAL;
    }
}

/* ── The texture environment ─────────────────────────────────────────────── */

C3D_TexEnv *C3D_GetTexEnv(int id) { return &G.env[id < 0 ? 0 : id > 5 ? 5 : id]; }

void C3D_SetTexEnv(int id, C3D_TexEnv *env)
{
    C2D_Flush();
    G.env[id < 0 ? 0 : id > 5 ? 5 : id] = *env;
}

void C3D_TexEnvInit(C3D_TexEnv *env)
{
    C2D_Flush();
    TexEnvDefault(env);
}

void C3D_TexEnvSrc(C3D_TexEnv *env, C3D_TexEnvMode mode, GPU_TEVSRC s1, GPU_TEVSRC s2, GPU_TEVSRC s3)
{
    u32 packed = (u32)s1 | ((u32)s2 << 4) | ((u32)s3 << 8);

    C2D_Flush();
    if (mode & C3D_RGB)
        env->srcRgb = packed;
    if (mode & C3D_Alpha)
        env->srcAlpha = packed;
}

void C3D_TexEnvOpRgb(C3D_TexEnv *env, GPU_TEVOP_RGB o1, GPU_TEVOP_RGB o2, GPU_TEVOP_RGB o3)
{
    C2D_Flush();
    env->opRgb = (u32)o1 | ((u32)o2 << 4) | ((u32)o3 << 8);
}

void C3D_TexEnvOpAlpha(C3D_TexEnv *env, GPU_TEVOP_A o1, GPU_TEVOP_A o2, GPU_TEVOP_A o3)
{
    C2D_Flush();
    env->opAlpha = (u32)o1 | ((u32)o2 << 4) | ((u32)o3 << 8);
}

void C3D_TexEnvFunc(C3D_TexEnv *env, C3D_TexEnvMode mode, GPU_COMBINEFUNC param)
{
    C2D_Flush();
    if (mode & C3D_RGB)
        env->funcRgb = (u16)param;
    if (mode & C3D_Alpha)
        env->funcAlpha = (u16)param;
}

void C3D_TexEnvColor(C3D_TexEnv *env, u32 color)
{
    C2D_Flush();
    env->color = color;
}

void C3D_TexEnvScale(C3D_TexEnv *env, int mode, GPU_TEVSCALE scale)
{
    C2D_Flush();
    if (mode & C3D_RGB)
        env->scaleRgb = (u16)scale;
    if (mode & C3D_Alpha)
        env->scaleAlpha = (u16)scale;
}

static bool TexEnvIsDefault(const C3D_TexEnv *env)
{
    return (env->srcRgb & 0xF) == GPU_PREVIOUS && (env->srcAlpha & 0xF) == GPU_PREVIOUS && env->funcRgb == GPU_REPLACE
        && env->funcAlpha == GPU_REPLACE && env->scaleRgb == GPU_TEVSCALE_1 && env->scaleAlpha == GPU_TEVSCALE_1
        && (env->opRgb & 0xF) == 0 && (env->opAlpha & 0xF) == 0;
}

/* ── Matrices ────────────────────────────────────────────────────────────── */

void Mtx_Identity(C3D_Mtx *out)
{
    memset(out, 0, sizeof(*out));
    out->r[0].x = out->r[1].y = out->r[2].z = out->r[3].w = 1.0f;
}

/* ── Applying the state and drawing ──────────────────────────────────────── */

static void ApplyTarget(void)
{
    C3D_RenderTarget *target = G.target;

    if (target != NULL && target->screen)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, G.topFbo);
        G.viewportW = G.topW;
        G.viewportH = G.topH;
    }
    else if (target != NULL)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, target->glFramebuffer);
        G.viewportW = target->frameBuf.width * target->glScale;
        G.viewportH = target->frameBuf.height * target->glScale;
    }
    glViewport(0, 0, G.viewportW, G.viewportH);
}

/* The six stages as uniforms of the program whose locations are given; the voxel world's has no
 * Citro2D stage and no mode. */
static void FillEnv(GLint uSrc, GLint uSrcA, GLint uOp, GLint uOpA, GLint uConst, GLint uStages, bool voxel)
{
    GLint src[24], srcA[24], op[24], opA[24];
    float constants[24];
    int stages = 0;
    static const int scales[3] = { 1, 2, 4 };

    for (int i = 0; i < 6; ++i)
    {
        const C3D_TexEnv *e = &G.env[i];

        src[i * 4] = (int)(e->srcRgb & 15);
        src[i * 4 + 1] = (int)((e->srcRgb >> 4) & 15);
        src[i * 4 + 2] = (int)((e->srcRgb >> 8) & 15);
        src[i * 4 + 3] = e->funcRgb;
        srcA[i * 4] = (int)(e->srcAlpha & 15);
        srcA[i * 4 + 1] = (int)((e->srcAlpha >> 4) & 15);
        srcA[i * 4 + 2] = (int)((e->srcAlpha >> 8) & 15);
        srcA[i * 4 + 3] = e->funcAlpha;
        op[i * 4] = (int)(e->opRgb & 15);
        op[i * 4 + 1] = (int)((e->opRgb >> 4) & 15);
        op[i * 4 + 2] = (int)((e->opRgb >> 8) & 15);
        op[i * 4 + 3] = scales[e->scaleRgb > 2 ? 2 : e->scaleRgb];
        opA[i * 4] = (int)(e->opAlpha & 15);
        opA[i * 4 + 1] = (int)((e->opAlpha >> 4) & 15);
        opA[i * 4 + 2] = (int)((e->opAlpha >> 8) & 15);
        opA[i * 4 + 3] = scales[e->scaleAlpha > 2 ? 2 : e->scaleAlpha];
        constants[i * 4] = (float)(e->color & 255) / 255.0f;
        constants[i * 4 + 1] = (float)((e->color >> 8) & 255) / 255.0f;
        constants[i * 4 + 2] = (float)((e->color >> 16) & 255) / 255.0f;
        constants[i * 4 + 3] = (float)((e->color >> 24) & 255) / 255.0f;
        if (voxel ? !TexEnvIsDefault(e) : i != 0 ? !TexEnvIsDefault(e) : (G.mode == MODE_RAW || e->funcRgb == ENV_C2D))
            stages = i + 1;
    }
    /* Stage 0 is Citro2D's for sprites and solids; later stages are the compositor's. */
    if (!voxel && G.mode == MODE_RAW && stages == 0)
        stages = 1;
    glUniform4iv(uSrc, 6, src);
    glUniform4iv(uSrcA, 6, srcA);
    glUniform4iv(uOp, 6, op);
    glUniform4iv(uOpA, 6, opA);
    glUniform4fv(uConst, 6, constants);
    glUniform1i(uStages, stages);
}

static void UploadEnv(void)
{
    FillEnv(G.uSrc, G.uSrcA, G.uOp, G.uOpA, G.uConst, G.uStages, false);
    glUniform1i(G.uMode, (int)G.mode);
}

static void ApplyPipeline(void);

static void ApplyState(void)
{
    const C3D_Tex *tex = G.mode == MODE_SOLID || G.batchTexture == NULL ? NULL : G.batchTexture;

    if (tex != NULL && tex->touched)
        TexUpload((C3D_Tex *)tex, false);
    ApplyTarget();
    glUseProgram(G.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex != NULL ? tex->glTexture : G.white);
    glUniform1i(G.uTex, 0);
    glUniform3f(G.uRow0, G.view.r[0].x, G.view.r[0].y, G.view.r[0].w);
    glUniform3f(G.uRow1, G.view.r[1].x, G.view.r[1].y, G.view.r[1].w);
    glUniform2f(G.uScene, (float)G.sceneW, (float)G.sceneH);
    UploadEnv();

    glUniform1i(G.uAlphaFunc, G.alphaTest ? (int)G.alphaFunc : (int)GPU_ALWAYS);
    glUniform1f(G.uAlphaRef, (float)G.alphaRef / 255.0f);
    ApplyPipeline();
}

/* Blending, the depth test, the write mask and the scissor, whichever program draws. */
static void ApplyPipeline(void)
{
    glEnable(GL_BLEND);
    glBlendEquationSeparate(BlendEquation(G.colorEq), BlendEquation(G.alphaEq));
    glBlendFuncSeparate(BlendFactor(G.srcColor), BlendFactor(G.dstColor), BlendFactor(G.srcAlpha), BlendFactor(G.dstAlpha));
    glBlendColor((float)(G.blendColor & 255) / 255.0f, (float)((G.blendColor >> 8) & 255) / 255.0f,
                 (float)((G.blendColor >> 16) & 255) / 255.0f, (float)((G.blendColor >> 24) & 255) / 255.0f);

    if (G.depthTest)
    {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(DepthFunc(G.depthFunc));
    }
    else
        glDisable(GL_DEPTH_TEST);
    glDepthMask((G.writeMask & GPU_WRITE_DEPTH) != 0);
    glColorMask((G.writeMask & GPU_WRITE_RED) != 0, (G.writeMask & GPU_WRITE_GREEN) != 0, (G.writeMask & GPU_WRITE_BLUE) != 0,
                (G.writeMask & GPU_WRITE_ALPHA) != 0);

    if (G.scissorMode == GPU_SCISSOR_NORMAL)
    {
        /* The compositor counts rows from the far edge of the target (top = height - y1), which is
         * GL's own convention; a screen target's rectangle is in scene units. */
        float kx = (float)G.viewportW / (float)G.sceneW, ky = (float)G.viewportH / (float)G.sceneH;
        GLint x = (GLint)lroundf((float)G.scissor[0] * kx), y = (GLint)lroundf((float)G.scissor[1] * ky);
        GLint w = (GLint)lroundf((float)(G.scissor[2] - G.scissor[0]) * kx), h = (GLint)lroundf((float)(G.scissor[3] - G.scissor[1]) * ky);

        glEnable(GL_SCISSOR_TEST);
        glScissor(x, y, w < 0 ? 0 : w, h < 0 ? 0 : h);
    }
    else
        glDisable(GL_SCISSOR_TEST);
}

static bool sTrace;
static unsigned sTraceCount;

static void TraceBatch(void)
{
    const BatchVertex *v = G.vertices;

    if (!sTrace || sTraceCount >= 150)
        return;
    ++sTraceCount;
    CtrLog_Write(CTR_LOG_VIDEO, "TRACE %s tex=%u q=%u v0=(%.0f,%.0f uv %.3f,%.3f col %08x tintblend %.2f z%.2f blend %d%d%d%d scissor %d env0=%x/%x:%d stages=%d",
                 G.mode == MODE_IMAGE ? "img" : G.mode == MODE_SOLID ? "solid" : "raw", G.batchTexture ? G.batchTexture->glTexture : 0,
                 G.quads, v->x, v->y, v->u, v->v, v->color, v->blendTint, v->z, G.srcColor, G.dstColor, G.srcAlpha, G.dstAlpha, (int)G.scissorMode,
                 G.env[0].srcRgb, G.env[0].srcAlpha, G.env[0].funcRgb, (int)G.alphaTest);
}

static void DrawBatchNow(const BatchVertex *vertices, unsigned quads)
{
    ApplyState();
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(quads * 4 * sizeof(BatchVertex)), vertices, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void *)offsetof(BatchVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void *)offsetof(BatchVertex, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void *)offsetof(BatchVertex, blendTexture));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(BatchVertex), (void *)offsetof(BatchVertex, color));
    glDrawElements(GL_TRIANGLES, (GLsizei)(quads * 6), GL_UNSIGNED_SHORT, 0);
    CheckError(G.mode == MODE_IMAGE ? "image batch" : "solid batch");
}

static void FlushBatch(void)
{
    if (G.quads == 0)
        return;
    TraceBatch();
    if (!G.skip && G.target != NULL)
    {
        RecordBatch();
        DrawBatchNow(G.vertices, G.quads);
        CountDraw(G.target, false, G.quads);
    }
    /* What Citro2D last drew with stays bound for the raw draws that follow: the compositor sends a
     * quad of no size to have the atlas bound, then draws its own vertices. */
    if (G.mode == MODE_IMAGE && G.batchTexture != NULL)
        G.bound = G.batchTexture;
    G.quads = 0;
}

void C2D_Flush(void) { FlushBatch(); }

void C3D_FrameSplit(u8 flags)
{
    (void)flags;
    FlushBatch();
}

static void BeginDraw(DrawMode mode, C3D_Tex *tex)
{
    if (G.env[0].funcRgb != ENV_C2D)
    {
        FlushBatch();
        SetC2dEnv();
    }
    if (G.quads != 0 && (G.mode != mode || G.batchTexture != tex || G.quads >= MAX_QUADS))
        FlushBatch();
    G.mode = mode;
    G.batchTexture = tex;
}

static BatchVertex *Quad(void) { return &G.vertices[G.quads++ * 4]; }

static void SetVertex(BatchVertex *v, float x, float y, float u, float t, u32 color, float blend, float z)
{
    v->x = x;
    v->y = y;
    v->u = u;
    v->v = t;
    v->blendTexture = 0.0f;
    v->blendTint = blend;
    v->z = z;
    v->color = color;
}

/* ── Frames and scenes ───────────────────────────────────────────────────── */

bool C3D_FrameBegin(u8 flags)
{
    (void)flags;
    AndroidHost_GlMakeCurrent();
    G.inFrame = true;
    G.quads = 0;
    ++G.frameNo;
    ReplayFrameBegin();
    return true;
}

/* Shows the top screen: the off-screen target, letterboxed, on the display. */
static void ShowTop(void)
{
    int surfaceW, surfaceH;

    if (!AndroidHost_TopBegin(&surfaceW, &surfaceH))
        return;
    if (G.topFbo != 0)
    {
        int w, h, x, y;

        if ((int64_t)surfaceW * SCREEN_H <= (int64_t)surfaceH * SCREEN_W)
        {
            w = surfaceW;
            h = (int)((int64_t)SCREEN_H * surfaceW / SCREEN_W);
        }
        else
        {
            h = surfaceH;
            w = (int)((int64_t)SCREEN_W * surfaceH / SCREEN_H);
        }
        x = (surfaceW - w) / 2;
        y = (surfaceH - h) / 2;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, G.topFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        /* Smooth only the last step, from the integer multiple to the display's size. */
        CheckError("frame");
        glBlitFramebuffer(0, 0, G.topW, G.topH, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, w == G.topW ? GL_NEAREST : GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    AndroidHost_TopEnd();
}

/*
 * Debugging aid: when sdmc:/3ds/emerald3ds/dump.txt exists, every live texture, render target and
 * the top screen are written beside it as raw RGBA8 files (name_WxH.rgba, rows top first), and the
 * marker is removed. tools/dump_view.py turns them into PNGs.
 */
static void DumpFramebuffer(GLuint fbo, int w, int h, const char *name)
{
    u32 *pixels = malloc((size_t)w * h * 4), *flipped = malloc((size_t)w * h * 4);
    char path[96];
    FILE *file;

    if (pixels == NULL || flipped == NULL)
        goto done;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (int y = 0; y < h; ++y)
        memcpy(flipped + (size_t)y * w, pixels + (size_t)(h - 1 - y) * w, (size_t)w * 4);
    snprintf(path, sizeof(path), "sdmc:/3ds/emerald3ds/%s_%dx%d.rgba", name, w, h);
    file = fopen(path, "wb");
    if (file != NULL)
    {
        fwrite(flipped, 1, (size_t)w * h * 4, file);
        fclose(file);
    }
done:
    free(pixels);
    free(flipped);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void DumpAll(void)
{
    char name[32];

    if (G.topFbo != 0)
        DumpFramebuffer(G.topFbo, G.topW, G.topH, "top");
    for (int i = 0; i < REGISTRY; ++i)
    {
        C3D_RenderTarget *target = sTargets[i];

        if (target != NULL && target->tex != NULL)
        {
            snprintf(name, sizeof(name), "target%02d", i);
            DumpFramebuffer(target->glFramebuffer, target->frameBuf.width * target->glScale, target->frameBuf.height * target->glScale, name);
        }
    }
    for (int i = 0; i < REGISTRY; ++i)
    {
        C3D_Tex *tex = sTextures[i];

        if (tex != NULL && tex->glTexture != 0 && !tex->linked && tex->width <= 2048)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, G.blitFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex->glTexture, 0);
            snprintf(name, sizeof(name), "texture%02d", i);
            DumpFramebuffer(G.blitFbo, tex->width * (tex->gpuScale > 1 ? tex->gpuScale : 1), tex->height * (tex->gpuScale > 1 ? tex->gpuScale : 1), name);
        }
    }
    CtrLog_Write(CTR_LOG_VIDEO, "c3d: dumped the surfaces");
}

static void TraceIfAsked(void)
{
    static unsigned frame;
    FILE *marker;

    sTrace = false;
    if (++frame % 30 != 0)
        return;
    marker = fopen("sdmc:/3ds/emerald3ds/trace.txt", "rb");
    if (marker == NULL)
        return;
    fclose(marker);
    remove("sdmc:/3ds/emerald3ds/trace.txt");
    sTrace = true;
    sTraceCount = 0;
}

static void DumpIfAsked(void)
{
    static unsigned frame;
    FILE *marker;

    if (++frame % 30 != 0)
        return;
    marker = fopen("sdmc:/3ds/emerald3ds/dump.txt", "rb");
    if (marker == NULL)
        return;
    fclose(marker);
    remove("sdmc:/3ds/emerald3ds/dump.txt");
    DumpAll();
}

void C3D_FrameEnd(u8 flags)
{
    (void)flags;
    FlushBatch();
    DumpIfAsked();
    TraceIfAsked();
    {
        static unsigned frames;

        if (++frames % 300 == 0)
        {
            char text[512];
            int used = 0;

            for (unsigned i = 0; i < REGISTRY + 2 && used < (int)sizeof(text) - 40; ++i)
                if (sDrawCount[i] != 0 || sRawCount[i] != 0)
                    used += snprintf(text + used, sizeof(text) - (size_t)used, " %s%u:%u+%u", i == REGISTRY ? "screen" : i == REGISTRY + 1 ? "other" : "t", i < REGISTRY ? i : 0,
                                     sDrawCount[i], sRawCount[i]);
            CtrLog_Write(CTR_LOG_VIDEO, "c3d: draws since last (quads+raw quads) by target:%s", text);
            memset(sDrawCount, 0, sizeof(sDrawCount));
            memset(sRawCount, 0, sizeof(sRawCount));
        }
    }
    ReplayFrameEnd();
    ShowTop();
    G.inFrame = false;
    G.target = NULL;
}

bool C3D_FrameDrawOn(C3D_RenderTarget *target)
{
    FlushBatch();
    G.target = target;
    G.skip = target == NULL || target == &G.rightTarget;
    if (target != NULL && target->screen)
    {
        int surfaceW = 0, surfaceH = 0;

        AndroidHost_TopSurfaceSize(&surfaceW, &surfaceH);
        if (surfaceW <= 0 || surfaceH <= 0 || !TopEnsure(surfaceW, surfaceH))
            G.skip = true;
        G.sceneW = SCREEN_W;
        G.sceneH = SCREEN_H;
    }
    else if (target != NULL)
    {
        G.sceneW = target->frameBuf.width;
        G.sceneH = target->frameBuf.height;
    }
    ApplyTarget();
    return true;
}

float C3D_GetDrawingTime(void) { return 0.0f; }
float C3D_GetCmdBufUsage(void) { return 0.0f; }

void C3D_RenderTargetClear(C3D_RenderTarget *target, u8 clearBits, u32 clearColor, u32 clearDepth)
{
    C3D_RenderTarget *saved = G.target;

    FlushBatch();
    if (target == NULL || (target == &G.rightTarget))
        return;
    if (target->screen)
    {
        int surfaceW = 0, surfaceH = 0;

        AndroidHost_TopSurfaceSize(&surfaceW, &surfaceH);
        if (surfaceW <= 0 || !TopEnsure(surfaceW, surfaceH))
            return;
    }
    RecordClear(target, clearBits, clearColor, clearDepth);
    G.target = target;
    ApplyTarget();
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    /* The colour is a word RRGGBBAA, as the PICA's clear takes it. */
    glClearColor((float)((clearColor >> 24) & 255) / 255.0f, (float)((clearColor >> 16) & 255) / 255.0f,
                 (float)((clearColor >> 8) & 255) / 255.0f, (float)(clearColor & 255) / 255.0f);
    glClearDepthf((float)clearDepth / 65535.0f);
    glClear(((clearBits & C3D_CLEAR_COLOR) ? GL_COLOR_BUFFER_BIT : 0) | ((clearBits & C3D_CLEAR_DEPTH) ? GL_DEPTH_BUFFER_BIT : 0));
    G.target = saved;
    if (saved != NULL)
        ApplyTarget();
}

void C2D_TargetClear(C3D_RenderTarget *target, u32 color)
{
    C3D_RenderTargetClear(target, C3D_CLEAR_ALL, __builtin_bswap32(color), 0);
}

void C2D_Prepare(void)
{
    FlushBatch();
    SetC2dEnv();
    G.depthTest = true;
    G.depthFunc = GPU_GEQUAL;
    G.writeMask = GPU_WRITE_ALL;
    G.blend = true;
    G.colorEq = G.alphaEq = GPU_BLEND_ADD;
    G.srcColor = G.srcAlpha = GPU_SRC_ALPHA;
    G.dstColor = G.dstAlpha = GPU_ONE_MINUS_SRC_ALPHA;
}

void C2D_SceneBegin(C3D_RenderTarget *target)
{
    C3D_FrameDrawOn(target);
}

/* The size is the framebuffer's, the PICA's way: a tilted scene (the LCDs' sideways
 * framebuffers) is the other way round. */
void C2D_SceneSize(u32 width, u32 height, bool tilt)
{
    FlushBatch();
    G.sceneW = (int)(tilt ? height : width);
    G.sceneH = (int)(tilt ? width : height);
}

void C2D_ViewReset(void)
{
    FlushBatch();
    Mtx_Identity(&G.view);
}

void C2D_ViewTranslate(float x, float y)
{
    FlushBatch();
    G.view.r[0].w += G.view.r[0].x * x + G.view.r[0].y * y;
    G.view.r[1].w += G.view.r[1].x * x + G.view.r[1].y * y;
}

void C2D_ViewScale(float x, float y)
{
    FlushBatch();
    G.view.r[0].x *= x;
    G.view.r[1].x *= x;
    G.view.r[0].y *= y;
    G.view.r[1].y *= y;
}

void C2D_ViewSave(C3D_Mtx *matrix) { *matrix = G.view; }

void C2D_ViewRestore(const C3D_Mtx *matrix)
{
    FlushBatch();
    G.view = *matrix;
}

/* ── Citro2D drawing ─────────────────────────────────────────────────────── */

bool C2D_DrawImageAt(C2D_Image img, float x, float y, float depth, const C2D_ImageTint *tint, float scaleX, float scaleY)
{
    const Tex3DS_SubTexture *sub = img.subtex;
    float w = (float)sub->width * scaleX, h = (float)sub->height * scaleY;
    static const C2D_ImageTint none = { { { 0xFFFFFFFFu, 0.0f }, { 0xFFFFFFFFu, 0.0f }, { 0xFFFFFFFFu, 0.0f }, { 0xFFFFFFFFu, 0.0f } } };
    BatchVertex *v;

    if (tint == NULL)
        tint = &none;
    if (img.tex == NULL || G.quads >= MAX_QUADS)
        return false;
    BeginDraw(MODE_IMAGE, img.tex);
    v = Quad();
    SetVertex(&v[0], x, y, sub->left, sub->top, tint->corners[C2D_TopLeft].color, tint->corners[C2D_TopLeft].blend, depth);
    SetVertex(&v[1], x + w, y, sub->right, sub->top, tint->corners[C2D_TopRight].color, tint->corners[C2D_TopRight].blend, depth);
    SetVertex(&v[2], x, y + h, sub->left, sub->bottom, tint->corners[C2D_BotLeft].color, tint->corners[C2D_BotLeft].blend, depth);
    SetVertex(&v[3], x + w, y + h, sub->right, sub->bottom, tint->corners[C2D_BotRight].color, tint->corners[C2D_BotRight].blend, depth);
    return true;
}

bool C2D_DrawRectangle(float x, float y, float z, float w, float h, u32 clrTL, u32 clrTR, u32 clrBL, u32 clrBR)
{
    BatchVertex *v;

    if (G.quads >= MAX_QUADS)
        return false;
    BeginDraw(MODE_SOLID, NULL);
    v = Quad();
    SetVertex(&v[0], x, y, 0, 0, clrTL, 0, z);
    SetVertex(&v[1], x + w, y, 0, 0, clrTR, 0, z);
    SetVertex(&v[2], x, y + h, 0, 0, clrBL, 0, z);
    SetVertex(&v[3], x + w, y + h, 0, 0, clrBR, 0, z);
    return true;
}

bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 clr)
{
    return C2D_DrawRectangle(x, y, z, w, h, clr, clr, clr, clr);
}

bool C2D_DrawTriangle(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1, float x2, float y2, u32 clr2, float depth)
{
    BatchVertex *v;

    if (G.quads >= MAX_QUADS)
        return false;
    BeginDraw(MODE_SOLID, NULL);
    v = Quad();
    SetVertex(&v[0], x0, y0, 0, 0, clr0, 0, depth);
    SetVertex(&v[1], x1, y1, 0, 0, clr1, 0, depth);
    SetVertex(&v[2], x2, y2, 0, 0, clr2, 0, depth);
    SetVertex(&v[3], x2, y2, 0, 0, clr2, 0, depth);
    return true;
}

bool C2D_DrawLine(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1, float thickness, float depth)
{
    float dx = x1 - x0, dy = y1 - y0, length = sqrtf(dx * dx + dy * dy), nx, ny;
    BatchVertex *v;

    if (length <= 0.0f || G.quads >= MAX_QUADS)
        return false;
    nx = -dy / length * thickness * 0.5f;
    ny = dx / length * thickness * 0.5f;
    BeginDraw(MODE_SOLID, NULL);
    v = Quad();
    SetVertex(&v[0], x0 + nx, y0 + ny, 0, 0, clr0, 0, depth);
    SetVertex(&v[1], x0 - nx, y0 - ny, 0, 0, clr0, 0, depth);
    SetVertex(&v[2], x1 + nx, y1 + ny, 0, 0, clr1, 0, depth);
    SetVertex(&v[3], x1 - nx, y1 - ny, 0, 0, clr1, 0, depth);
    return true;
}

bool C2D_DrawCircleSolid(float x, float y, float z, float radius, u32 clr)
{
    enum { SEGMENTS = 32 };

    for (int i = 0; i < SEGMENTS; ++i)
    {
        float a0 = (float)i / SEGMENTS * 6.2831853f, a1 = (float)(i + 1) / SEGMENTS * 6.2831853f;

        if (!C2D_DrawTriangle(x, y, clr, x + cosf(a0) * radius, y + sinf(a0) * radius, clr,
                              x + cosf(a1) * radius, y + sinf(a1) * radius, clr, z))
            return false;
    }
    return true;
}

/* ── Raw vertex drawing (the compositor's own vertex buffer of cells and sprites) ─ */

void AttrInfo_Init(C3D_AttrInfo *info) { memset(info, 0, sizeof(*info)); }

int AttrInfo_AddLoader(C3D_AttrInfo *info, int regId, GPU_FORMATS format, int count)
{
    if (info->loaders >= C3D_MAX_ATTRIBS)
        return -1;
    info->loader[info->loaders].index = (u8)regId;
    info->loader[info->loaders].format = (u8)format;
    info->loader[info->loaders].count = (u8)count;
    return info->loaders++;
}

int AttrInfo_AddFixed(C3D_AttrInfo *info, int regId)
{
    if (info->fixedCount >= C3D_MAX_ATTRIBS)
        return -1;
    info->fixed[info->fixedCount].index = (u8)regId;
    return info->fixedCount++;
}

void BufInfo_Init(C3D_BufInfo *info) { memset(info, 0, sizeof(*info)); }

int BufInfo_Add(C3D_BufInfo *info, const void *data, ptrdiff_t stride, int attribCount, u64 permutation)
{
    info->data = data;
    info->stride = (u32)stride;
    info->attribCount = (u8)attribCount;
    info->permutation = permutation;
    return 0;
}

C3D_AttrInfo *C3D_GetAttrInfo(void) { return &G.attrInfo; }
void C3D_SetAttrInfo(C3D_AttrInfo *attrInfo) { G.attrInfo = *attrInfo; }
C3D_BufInfo *C3D_GetBufInfo(void) { return &G.bufInfo; }
void C3D_SetBufInfo(C3D_BufInfo *bufInfo) { G.bufInfo = *bufInfo; }

void C3D_FixedAttribSet(int id, float x, float y, float z, float w)
{
    if (id < 0 || id >= C3D_MAX_ATTRIBS)
        return;
    G.fixed[id][0] = x;
    G.fixed[id][1] = y;
    G.fixed[id][2] = z;
    G.fixed[id][3] = w;
}

void C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void *indices)
{
    unsigned maxIndex = 0;

    FlushBatch();
    if (G.skip || G.target == NULL || count <= 0 || indices == NULL || G.bufInfo.data == NULL || primitive != GPU_TRIANGLES ||
        (type != C3D_UNSIGNED_BYTE && type != C3D_UNSIGNED_SHORT))
        return;
    for (int i = 0; i < count; ++i)
    {
        unsigned index = type == C3D_UNSIGNED_BYTE ? ((const u8 *)indices)[i] : ((const u16 *)indices)[i];

        if (index > maxIndex)
            maxIndex = index;
    }
    if (G.voxelBound)
    {
        DrawVoxelElements(G.bufInfo.data, G.bufInfo.stride, maxIndex + 1, indices, count, type);
        return;
    }
    G.mode = G.env[0].funcRgb == ENV_C2D ? MODE_IMAGE : MODE_RAW;
    G.batchTexture = G.bound;
    RecordRaw(G.bufInfo.data, (size_t)(maxIndex + 1) * G.bufInfo.stride, indices, count, type);
    DrawRawNow(G.bufInfo.data, (size_t)(maxIndex + 1) * G.bufInfo.stride, indices, count, type);
    CountDraw(G.target, true, (unsigned)count / 6);
}

static void DrawRawNow(const u8 *vertices, size_t vertexBytes, const void *indices, int count, int type)
{
    unsigned offset = 0;
    size_t indexBytes = type == C3D_UNSIGNED_BYTE ? 1 : 2;

    ApplyState();
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.rawVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vertexBytes, vertices, GL_STREAM_DRAW);
    for (int a = 0; a < 4; ++a)
        glDisableVertexAttribArray(a);
    for (unsigned i = 0; i < G.attrInfo.loaders; ++i)
    {
        static const GLenum glFormats[4] = { GL_BYTE, GL_UNSIGNED_BYTE, GL_SHORT, GL_FLOAT };
        static const unsigned sizes[4] = { 1, 1, 2, 4 };
        unsigned index = G.attrInfo.loader[i].index, format = G.attrInfo.loader[i].format, components = G.attrInfo.loader[i].count;

        if (index > 3)
            continue;
        glEnableVertexAttribArray(index);
        glVertexAttribPointer(index, (GLint)components, glFormats[format], GL_FALSE, (GLsizei)G.bufInfo.stride, (void *)(uintptr_t)offset);
        offset += components * sizes[format];
    }
    /* Fixed attributes: 2 is (blend...), 3 is a colour given as 0..255. */
    glVertexAttrib3f(2, G.fixed[2][0], G.fixed[2][1], G.fixed[2][2]);
    glVertexAttrib4f(3, G.fixed[3][0] / 255.0f, G.fixed[3][1] / 255.0f, G.fixed[3][2] / 255.0f, G.fixed[3][3] / 255.0f);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.rawIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(count * indexBytes), indices, GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, count, type == C3D_UNSIGNED_BYTE ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT, 0);
    CheckError("raw draw");
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.ibo);
    G.mode = MODE_IMAGE;
}

/* ══ The voxel world: a programmable vertex stage, three texture units, memory it manages itself ══ */

/* ── Matrices ────────────────────────────────────────────────────────────── */

static inline float *Element(C3D_Mtx *m, int row, int col) { return &m->r[row].c[3 - col]; }
static inline float ElementOf(const C3D_Mtx *m, int row, int col) { return m->r[row].c[3 - col]; }

void Mtx_Multiply(C3D_Mtx *out, const C3D_Mtx *a, const C3D_Mtx *b)
{
    C3D_Mtx result;

    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float sum = 0.0f;

            for (int k = 0; k < 4; ++k)
                sum += ElementOf(a, i, k) * ElementOf(b, k, j);
            *Element(&result, i, j) = sum;
        }
    *out = result;
}

C3D_FVec Mtx_MultiplyFVec4(const C3D_Mtx *mtx, C3D_FVec v)
{
    return FVec4_New(FVec4_Dot(mtx->r[0], v), FVec4_Dot(mtx->r[1], v), FVec4_Dot(mtx->r[2], v), FVec4_Dot(mtx->r[3], v));
}

void Mtx_Translate(C3D_Mtx *mtx, float x, float y, float z, bool bRightSide)
{
    C3D_FVec v = FVec4_New(x, y, z, 1.0f);

    if (bRightSide)
        for (int i = 0; i < 4; ++i)
            mtx->r[i].w = FVec4_Dot(mtx->r[i], v);
    else
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j)
                mtx->r[i].c[j] += mtx->r[3].c[j] * v.c[3 - i];
}

void Mtx_Persp(C3D_Mtx *mtx, float fovy, float aspect, float near, float far, bool isLeftHanded)
{
    float tangent = tanf(fovy / 2.0f);

    Mtx_Zeros(mtx);
    mtx->r[0].x = 1.0f / (tangent * aspect);
    mtx->r[1].y = 1.0f / tangent;
    if (isLeftHanded)
    {
        mtx->r[2].z = near / (far - near);
        mtx->r[2].w = -near * far / (far - near);
        mtx->r[3].z = 1.0f;
    }
    else
    {
        mtx->r[2].z = near / (near - far);
        mtx->r[2].w = far * near / (near - far);
        mtx->r[3].z = -1.0f;
    }
}

void Mtx_Ortho(C3D_Mtx *mtx, float left, float right, float bottom, float top, float near, float far, bool isLeftHanded)
{
    Mtx_Zeros(mtx);
    mtx->r[0].x = 2.0f / (right - left);
    mtx->r[0].w = (left + right) / (left - right);
    mtx->r[1].y = 2.0f / (top - bottom);
    mtx->r[1].w = (bottom + top) / (bottom - top);
    mtx->r[2].z = (isLeftHanded ? 1.0f : -1.0f) / (far - near);
    mtx->r[2].w = 0.5f * (near + far) / (near - far) - 0.5f;
    mtx->r[3].w = 1.0f;
}

static C3D_FVec3 Cross3(C3D_FVec3 a, C3D_FVec3 b) { return FVec3_New(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }

static C3D_FVec3 Normalize3(C3D_FVec3 v)
{
    float length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);

    return length > 0.0f ? FVec3_New(v.x / length, v.y / length, v.z / length) : v;
}

void Mtx_LookAt(C3D_Mtx *out, C3D_FVec3 position, C3D_FVec3 target, C3D_FVec3 up, bool isLeftHanded)
{
    C3D_FVec3 lz = Normalize3(FVec3_New(position.x - target.x, position.y - target.y, position.z - target.z)), lx, ly;

    if (isLeftHanded)
        lz = FVec3_New(-lz.x, -lz.y, -lz.z);
    lx = Normalize3(Cross3(up, lz));
    ly = Normalize3(Cross3(lz, lx));
    out->r[0] = FVec4_New(lx.x, lx.y, lx.z, -(lx.x * position.x + lx.y * position.y + lx.z * position.z));
    out->r[1] = FVec4_New(ly.x, ly.y, ly.z, -(ly.x * position.x + ly.y * position.y + ly.z * position.z));
    out->r[2] = FVec4_New(lz.x, lz.y, lz.z, -(lz.x * position.x + lz.y * position.y + lz.z * position.z));
    out->r[3] = FVec4_New(0.0f, 0.0f, 0.0f, 1.0f);
}

/* ── The program ─────────────────────────────────────────────────────────── */

/* voxel.v.pica: positions in 1/512 tile and a shade in 1/16384 as four shorts, a texture
 * coordinate; per vertex the colour grade, the haze weight (in alpha) and the sun dapples'
 * coordinates. The register file is the PICA's: projection 0..3, modelView 4..7, shadeTint 8,
 * tintDiff 9, grade 10, fog 11, dappleU 12, dappleV 13. The depth is the PICA's (zScale * z / w +
 * zOffset, larger is nearer) put in GL's window range. */
static const char kVoxelVertexShader[] =
    "#version 300 es\n"
    "layout(location = 0) in vec4 aPos;\n"
    "layout(location = 1) in vec2 aUv;\n"
    "uniform vec4 uReg[14];\n"
    "uniform vec2 uDepth;\n"
    "out vec2 vTc0;\n"
    "out vec2 vTc1;\n"
    "out vec2 vTc2;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "    vec4 p = vec4(aPos.xyz * 0.001953125, 1.0);\n"
    "    vec4 v = vec4(dot(uReg[4], p), dot(uReg[5], p), dot(uReg[6], p), dot(uReg[7], p));\n"
    "    vec4 c = vec4(dot(uReg[0], v), dot(uReg[1], v), dot(uReg[2], v), dot(uReg[3], v));\n"
    "    gl_Position = vec4(c.xy, 2.0 * uDepth.x * c.z + (2.0 * uDepth.y - 1.0) * c.w, c.w);\n"
    "    vTc0 = aUv;\n"
    "    vTc2 = aUv;\n"
    "    vTc1 = vec2(dot(uReg[12], p), dot(uReg[13], p));\n"
    "    float shade = aPos.w * 0.00006103515625;\n"
    "    float t = clamp(shade * uReg[10].x + uReg[10].y, 0.0, 1.0);\n"
    "    vec3 tint = shade * (uReg[8].xyz + uReg[9].xyz * t);\n"
    "    float haze = clamp(uReg[11].x * v.z + uReg[11].y, 0.0, uReg[11].z);\n"
    "    vColor = clamp(vec4(tint, 1.0 - haze), 0.0, 1.0);\n"
    "}\n";

/*
 * The fragment stage is generated from the texture environment (a PICA draw's six stages): every stage's
 * sources, operands, function and scale are written into the shader as constants, so a draw runs only what
 * its environment says, not the compositor's general loop. One program per distinct environment (and alpha
 * test), made when first drawn with and kept; the vertex shader is shared.
 */
typedef struct
{
    u32 stages, alphaFunc;
    struct { u32 srcRgb, srcAlpha, opRgb, opAlpha; u16 funcRgb, funcAlpha, scaleRgb, scaleAlpha; } s[6];
} VoxKey;

typedef struct
{
    VoxKey key;
    GLuint program;
    GLint reg, depth, constants, alphaRef;
    float appliedReg[14][4], appliedConst[24], appliedDepth[2], appliedRef;
    bool applied;
} VoxVariant;

#define VOX_VARIANTS 128
static VoxVariant sVox[VOX_VARIANTS];
static unsigned sVoxCount;
static GLuint sVoxVs;

static bool BuildVoxelProgram(void)
{
    sVoxVs = Compile(GL_VERTEX_SHADER, kVoxelVertexShader);
    if (sVoxVs == 0)
        return false;
    G.voxProgram = 1; /* the world can be drawn */
    glGenVertexArrays(1, &G.voxVao);
    glGenBuffers(1, &G.voxStream);
    G.depthScale = -1.0f;
    G.depthOffset = 0.0f;
    CtrLog_Write(CTR_LOG_VIDEO, "c3d: voxel vertex shader ready");
    return true;
}

static const char *VoxSource(char *out, size_t size, int source, int stage)
{
    switch (source)
    {
    case GPU_TEXTURE0: return "t0";
    case GPU_TEXTURE1: return "t1";
    case GPU_TEXTURE2: return "t2";
    case GPU_CONSTANT: snprintf(out, size, "uConst[%d]", stage); return out;
    case GPU_PREVIOUS: return "prev";
    default: return "vColor";
    }
}

static void VoxOperandRgb(char *out, size_t size, int op, const char *v)
{
    switch (op)
    {
    case 1: snprintf(out, size, "(1.0 - (%s).rgb)", v); break;
    case 2: snprintf(out, size, "vec3((%s).a)", v); break;
    case 3: snprintf(out, size, "vec3(1.0 - (%s).a)", v); break;
    case 4: snprintf(out, size, "vec3((%s).r)", v); break;
    case 5: snprintf(out, size, "vec3(1.0 - (%s).r)", v); break;
    case 8: snprintf(out, size, "vec3((%s).g)", v); break;
    case 9: snprintf(out, size, "vec3(1.0 - (%s).g)", v); break;
    case 12: snprintf(out, size, "vec3((%s).b)", v); break;
    case 13: snprintf(out, size, "vec3(1.0 - (%s).b)", v); break;
    default: snprintf(out, size, "(%s).rgb", v); break;
    }
}

static void VoxOperandA(char *out, size_t size, int op, const char *v)
{
    static const char *const channel[4] = { "a", "r", "g", "b" };

    if (op >= 0 && op < 8)
        snprintf(out, size, op & 1 ? "(1.0 - (%s).%s)" : "(%s).%s", v, channel[op >> 1]);
    else
        snprintf(out, size, "(%s).a", v);
}

static size_t VoxGenerate(const VoxKey *key, char *out, size_t size)
{
    static const char *const scales[3] = { "1.0", "2.0", "4.0" };
    bool uses[3] = { false, false, false };
    size_t n = 0;

    for (unsigned i = 0; i < key->stages; ++i)
        for (int k = 0; k < 3; ++k)
        {
            int rgb = (int)((key->s[i].srcRgb >> (4 * k)) & 15), alpha = (int)((key->s[i].srcAlpha >> (4 * k)) & 15);

            if (rgb >= GPU_TEXTURE0 && rgb <= GPU_TEXTURE2)
                uses[rgb - GPU_TEXTURE0] = true;
            if (alpha >= GPU_TEXTURE0 && alpha <= GPU_TEXTURE2)
                uses[alpha - GPU_TEXTURE0] = true;
        }
#define EMIT(...) n += (size_t)snprintf(out + n, n < size ? size - n : 0, __VA_ARGS__)
    EMIT("#version 300 es\nprecision highp float;\n");
    for (int k = 0; k < 3; ++k)
        if (uses[k])
            EMIT("uniform sampler2D uTex%d;\n", k);
    EMIT("uniform vec4 uConst[6];\nuniform float uAlphaRef;\nin vec2 vTc0;\nin vec2 vTc1;\nin vec2 vTc2;\nin vec4 vColor;\nout vec4 fragColor;\n");
    EMIT("void main() {\n");
    for (int k = 0; k < 3; ++k)
        if (uses[k])
            EMIT("    vec4 t%d = texture(uTex%d, vTc%d);\n", k, k, k);
    EMIT("    vec4 prev = vColor;\n");
    for (unsigned i = 0; i < key->stages; ++i)
    {
        char buffer[3][40], operand[3][96], operandA[3][96];
        int func = key->s[i].funcRgb, funcA = key->s[i].funcAlpha;
        u32 srcRgb = key->s[i].srcRgb, srcAlpha = key->s[i].srcAlpha, opRgb = key->s[i].opRgb, opAlpha = key->s[i].opAlpha;
        const char *combine, *combineA;
        char combineText[160], combineAText[160];

        for (int k = 0; k < 3; ++k)
        {
            const char *source = VoxSource(buffer[k], sizeof(buffer[k]), (int)((srcRgb >> (4 * k)) & 15), (int)i);
            const char *sourceA = VoxSource(buffer[k], sizeof(buffer[k]), (int)((srcAlpha >> (4 * k)) & 15), (int)i);

            VoxOperandRgb(operand[k], sizeof(operand[k]), (int)((opRgb >> (4 * k)) & 15), source);
            VoxOperandA(operandA[k], sizeof(operandA[k]), (int)((opAlpha >> (4 * k)) & 15), sourceA);
        }
        switch (func)
        {
        case 0: combine = "a"; break;
        case 1: combine = "a * b"; break;
        case 2: combine = "a + b"; break;
        case 3: combine = "a + b - 0.5"; break;
        case 4: combine = "a * c + b * (1.0 - c)"; break;
        case 5: combine = "a - b"; break;
        case 6: case 7: combine = "vec3(4.0 * dot(a - 0.5, b - 0.5))"; break;
        case 8: combine = "a * b + c"; break;
        default: combine = "(a + b) * c"; break;
        }
        switch (funcA)
        {
        case 1: combineA = "aa * ab"; break;
        case 2: combineA = "aa + ab"; break;
        case 3: combineA = "aa + ab - 0.5"; break;
        case 4: combineA = "aa * ac + ab * (1.0 - ac)"; break;
        case 5: combineA = "aa - ab"; break;
        case 8: combineA = "aa * ab + ac"; break;
        case 9: combineA = "(aa + ab) * ac"; break;
        default: combineA = "aa"; break;
        }
        snprintf(combineText, sizeof(combineText), "%s", combine);
        snprintf(combineAText, sizeof(combineAText), "%s", combineA);
        EMIT("    {\n        vec3 a = %s, b = %s, c = %s;\n        float aa = %s, ab = %s, ac = %s;\n", operand[0], operand[1], operand[2],
             operandA[0], operandA[1], operandA[2]);
        EMIT("        vec3 rgb = clamp((%s) * %s, 0.0, 1.0);\n        float alpha = clamp((%s) * %s, 0.0, 1.0);\n", combineText,
             scales[key->s[i].scaleRgb > 2 ? 2 : key->s[i].scaleRgb], combineAText, scales[key->s[i].scaleAlpha > 2 ? 2 : key->s[i].scaleAlpha]);
        if (func == GPU_DOT3_RGBA)
            EMIT("        alpha = rgb.r;\n");
        EMIT("        prev = vec4(rgb, alpha);\n    }\n");
    }
    if (key->alphaFunc != GPU_ALWAYS)
    {
        static const char *const tests[8] = { "false", "true", "al == uAlphaRef", "al != uAlphaRef", "al < uAlphaRef", "al <= uAlphaRef",
                                              "al > uAlphaRef", "al >= uAlphaRef" };

        EMIT("    float al = floor(prev.a * 255.0 + 0.5) / 255.0;\n    if (!(%s)) discard;\n", tests[key->alphaFunc & 7]);
    }
    EMIT("    fragColor = prev;\n}\n");
#undef EMIT
    return n;
}

static VoxVariant *VoxFor(void)
{
    VoxKey key;
    VoxVariant *variant;
    static const int scales_unused = 0;
    char *source;
    GLuint fs;
    GLint ok = 0;

    (void)scales_unused;
    memset(&key, 0, sizeof(key));
    for (unsigned i = 0; i < 6; ++i)
    {
        const C3D_TexEnv *e = &G.env[i];

        key.s[i].srcRgb = e->srcRgb & 0xFFF;
        key.s[i].srcAlpha = e->srcAlpha & 0xFFF;
        key.s[i].opRgb = e->opRgb & 0xFFF;
        key.s[i].opAlpha = e->opAlpha & 0xFFF;
        key.s[i].funcRgb = e->funcRgb;
        key.s[i].funcAlpha = e->funcAlpha;
        key.s[i].scaleRgb = e->scaleRgb;
        key.s[i].scaleAlpha = e->scaleAlpha;
        if (!TexEnvIsDefault(e))
            key.stages = i + 1;
    }
    key.alphaFunc = G.alphaTest ? (u32)G.alphaFunc : (u32)GPU_ALWAYS;
    /* A stage past the last used one does not matter: its fields are zero in the key. */
    for (unsigned i = key.stages; i < 6; ++i)
        memset(&key.s[i], 0, sizeof(key.s[i]));
    for (unsigned i = 0; i < sVoxCount; ++i)
        if (memcmp(&sVox[i].key, &key, sizeof(key)) == 0)
            return &sVox[i];
    if (sVoxCount == VOX_VARIANTS)
    {
        CtrLog_Write(CTR_LOG_ERROR, "c3d: too many voxel shader variants");
        return &sVox[VOX_VARIANTS - 1];
    }
    source = malloc(16384);
    if (source == NULL)
        return &sVox[0];
    VoxGenerate(&key, source, 16384);
    fs = Compile(GL_FRAGMENT_SHADER, source);
    variant = &sVox[sVoxCount];
    memset(variant, 0, sizeof(*variant));
    variant->key = key;
    if (fs != 0)
    {
        variant->program = glCreateProgram();
        glAttachShader(variant->program, sVoxVs);
        glAttachShader(variant->program, fs);
        glLinkProgram(variant->program);
        glDeleteShader(fs);
        glGetProgramiv(variant->program, GL_LINK_STATUS, &ok);
    }
    if (!ok)
    {
        CtrLog_Write(CTR_LOG_ERROR, "c3d: voxel variant failed:\n%s", source);
        free(source);
        if (variant->program != 0)
            glDeleteProgram(variant->program);
        variant->program = 0;
        ++sVoxCount;
        return variant;
    }
    free(source);
    variant->reg = glGetUniformLocation(variant->program, "uReg");
    variant->depth = glGetUniformLocation(variant->program, "uDepth");
    variant->constants = glGetUniformLocation(variant->program, "uConst");
    variant->alphaRef = glGetUniformLocation(variant->program, "uAlphaRef");
    glUseProgram(variant->program);
    for (int k = 0; k < 3; ++k)
    {
        char name[8];
        GLint location;

        snprintf(name, sizeof(name), "uTex%d", k);
        location = glGetUniformLocation(variant->program, name);
        if (location >= 0)
            glUniform1i(location, k);
    }
    ++sVoxCount;
    CtrLog_Write(CTR_LOG_VIDEO, "c3d: voxel shader variant %u: %u stage(s), alpha test %u", sVoxCount, key.stages, key.alphaFunc);
    return variant;
}

/* The shader binary is not read: one vertex shader, with its uniforms' register numbers. */
static DVLE_s sDvle;
static DVLB_s sDvlb = { 1, &sDvle };
static int sShaderInstance;

DVLB_s *DVLB_ParseFile(const void *shbin, u32 size)
{
    (void)shbin;
    (void)size;
    return &sDvlb;
}

void DVLB_Free(DVLB_s *dvlb) { (void)dvlb; }
void shaderProgramInit(shaderProgram_s *program) { program->vertexShader = NULL; }

Result shaderProgramSetVsh(shaderProgram_s *program, DVLE_s *vsh)
{
    (void)vsh;
    program->vertexShader = &sShaderInstance;
    return 0;
}

Result shaderProgramFree(shaderProgram_s *program)
{
    program->vertexShader = NULL;
    return 0;
}

int shaderInstanceGetUniformLocation(void *instance, const char *name)
{
    static const struct { const char *name; int reg; } registers[] = {
        { "projection", 0 }, { "modelView", 4 }, { "shadeTint", 8 }, { "tintDiff", 9 },
        { "grade", 10 }, { "fog", 11 }, { "dappleU", 12 }, { "dappleV", 13 },
    };

    (void)instance;
    for (unsigned i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i)
        if (strcmp(registers[i].name, name) == 0)
            return registers[i].reg;
    return -1;
}

void C3D_BindProgram(shaderProgram_s *program) { G.voxelBound = program != NULL && program->vertexShader != NULL && G.voxProgram != 0; }

void C3D_FVUnifSet(GPU_SHADER_TYPE type, int id, float x, float y, float z, float w)
{
    (void)type;
    if (id < 0 || id >= 32)
        return;
    G.regs[id][0] = x;
    G.regs[id][1] = y;
    G.regs[id][2] = z;
    G.regs[id][3] = w;
}

void C3D_FVUnifMtx4x4(GPU_SHADER_TYPE type, int id, const C3D_Mtx *mtx)
{
    for (int i = 0; i < 4; ++i)
        C3D_FVUnifSet(type, id + i, mtx->r[i].x, mtx->r[i].y, mtx->r[i].z, mtx->r[i].w);
}

void C3D_DepthMap(bool bIsZBuffer, float zScale, float zOffset)
{
    (void)bIsZBuffer;
    G.depthScale = zScale;
    G.depthOffset = zOffset;
}

void C3D_ColorLogicOp(GPU_LOGICOP op) { (void)op; }

/* ── Memory: blocks standing for VRAM, and which texture or block an address is in ── */

typedef struct
{
    u8 *base;
    size_t size;
    GLuint vbo;       /* the GL copy, made when a draw first reads from the block */
    size_t lo, hi;    /* bytes written since the copy was last made current */
} VramBlock;

static VramBlock sBlocks[8];

void *vramAlloc(size_t size)
{
    void *memory = NULL;

    if (size == 0 || posix_memalign(&memory, 256, size) != 0)
        return NULL;
    memset(memory, 0, size);
    for (unsigned i = 0; i < sizeof(sBlocks) / sizeof(sBlocks[0]); ++i)
        if (sBlocks[i].base == NULL)
        {
            sBlocks[i].base = memory;
            sBlocks[i].size = size;
            sBlocks[i].vbo = 0;
            sBlocks[i].lo = size;
            sBlocks[i].hi = 0;
            return memory;
        }
    free(memory);
    return NULL;
}

void vramFree(void *memory)
{
    for (unsigned i = 0; i < sizeof(sBlocks) / sizeof(sBlocks[0]); ++i)
        if (sBlocks[i].base == memory && memory != NULL)
        {
            if (sBlocks[i].vbo != 0)
            {
                GLuint name = sBlocks[i].vbo;

                glDeleteBuffers(1, &name);
            }
            memset(&sBlocks[i], 0, sizeof(sBlocks[i]));
        }
    free(memory);
}

static VramBlock *BlockOf(const void *address)
{
    const u8 *p = address;

    for (unsigned i = 0; i < sizeof(sBlocks) / sizeof(sBlocks[0]); ++i)
        if (sBlocks[i].base != NULL && p >= sBlocks[i].base && p < sBlocks[i].base + sBlocks[i].size)
            return &sBlocks[i];
    return NULL;
}

/* Bytes of a block were written: the GL copy, if there is one, takes them before its next draw. */
static void BlockWritten(const void *address, size_t size)
{
    VramBlock *block = BlockOf(address);
    size_t offset;

    if (block == NULL || block->vbo == 0)
        return;
    offset = (size_t)((const u8 *)address - block->base);
    if (offset + size > block->size)
        size = block->size - offset;
    if (offset < block->lo)
        block->lo = offset;
    if (offset + size > block->hi)
        block->hi = offset + size;
}

static void BlockCurrent(VramBlock *block)
{
    if (block->vbo == 0)
    {
        glGenBuffers(1, &block->vbo);
        glBindBuffer(GL_ARRAY_BUFFER, block->vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)block->size, block->base, GL_DYNAMIC_DRAW);
        block->lo = block->size;
        block->hi = 0;
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, block->vbo);
    if (block->hi > block->lo)
    {
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)block->lo, (GLsizeiptr)(block->hi - block->lo), block->base + block->lo);
        block->lo = block->size;
        block->hi = 0;
    }
}

/* A texture over memory the voxel world carved out itself (a piece of its page arena) has no GL texture
 * until it is first drawn from, and loses its handle each time the world sets it up again: the GL
 * textures are kept per C3D_Tex and reused while the size and format stay. */
typedef struct
{
    C3D_Tex *tex;
    GLuint name;
    u16 width, height;
    GPU_TEXCOLOR fmt;
    u8 *dirty;
} LazyTexture;

static LazyTexture sLazy[96];

static bool TexLazyCreate(C3D_Tex *tex)
{
    LazyTexture *entry = NULL;
    TexFormat f = FormatOf(tex->fmt);
    size_t tiles = (size_t)(tex->width / 8) * (tex->height / 8), bytes = (tiles + 7) / 8;
    bool registered = false;

    if (tex->data == NULL || tex->width == 0 || tex->height == 0 || (tex->width & 7) != 0 || (tex->height & 7) != 0)
        return false;
    for (unsigned i = 0; i < sizeof(sLazy) / sizeof(sLazy[0]); ++i)
        if (sLazy[i].tex == tex)
            entry = &sLazy[i];
    for (unsigned i = 0; entry == NULL && i < sizeof(sLazy) / sizeof(sLazy[0]); ++i)
        if (sLazy[i].tex == NULL)
        {
            entry = &sLazy[i];
            entry->tex = tex;
        }
    if (entry == NULL)
        return false;
    if (entry->name != 0 && (entry->width != tex->width || entry->height != tex->height || entry->fmt != tex->fmt))
    {
        GLuint old = entry->name;

        glDeleteTextures(1, &old);
        free(entry->dirty);
        entry->name = 0;
        entry->dirty = NULL;
        FreeShadow(tex);
    }
    if (entry->name == 0)
    {
        GLuint name = 0;

        entry->dirty = calloc(1, bytes);
        if (entry->dirty == NULL)
            return false;
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexStorage2D(GL_TEXTURE_2D, 1, f.internal, tex->width, tex->height);
        entry->name = name;
        entry->width = tex->width;
        entry->height = tex->height;
        entry->fmt = tex->fmt;
        FreeShadow(tex);
        CtrLog_Write(CTR_LOG_VIDEO, "c3d: page texture gl=%u %ux%u fmt=%d", name, tex->width, tex->height, (int)tex->fmt);
    }
    for (int i = 0; i < REGISTRY; ++i)
        registered |= sTextures[i] == tex;
    if (!registered)
        Register((void **)sTextures, tex);
    tex->glTexture = entry->name;
    tex->lazy = true;
    tex->dirtyTiles = entry->dirty;
    memset(entry->dirty, 0xFF, bytes);
    tex->touched = true;
    TexApplyParams(tex);
    return true;
}

/* The texture a draw samples from one of its units, made and brought up to date as needed. */
static GLuint TexReady(C3D_Tex *tex)
{
    if (tex == NULL || tex->data == NULL || (tex->data == &sGpuMarker && tex->glTexture == 0))
        return G.white;
    if (tex->glTexture == 0 && !TexLazyCreate(tex))
        return G.white;
    /* A texture the CPU writes without saying so (the sprite atlas, the atlases of tiles) is compared
     * with what the GL holds once a frame, where it is first drawn from; the others say what changed. */
    if (!tex->lazy && tex->dirtyTiles != NULL && tex->scanned != G.frameNo)
    {
        tex->scanned = G.frameNo;
        TexUpload(tex, true);
    }
    else if (tex->touched)
        TexUpload(tex, false);
    return tex->glTexture;
}

/* Bytes of CPU memory were written (a copy into a texture's piece of the arena, or a cache flush that on
 * the console made the GPU see them): the tiles they cover are uploaded before the texture is drawn from. */
static void MemoryWritten(const void *address, size_t size)
{
    const u8 *p = address;

    if (size == 0)
        return;
    BlockWritten(address, size);
    for (int i = 0; i < REGISTRY; ++i)
    {
        C3D_Tex *tex = sTextures[i];
        const u8 *base;
        size_t tileBytes, first, last, tiles, lead = 0, from = 0;

        if (tex == NULL || tex->data == NULL || tex->data == &sGpuMarker || tex->dirtyTiles == NULL || tex->size == 0)
            continue;
        base = tex->data;
        if (p + size <= base || p >= base + tex->size)
            continue;
        tileBytes = 64 * FormatOf(tex->fmt).memory;
        tiles = (size_t)(tex->width / 8) * (tex->height / 8);
        if (p > base)
            from = (size_t)(p - base);
        else
            lead = (size_t)(base - p);
        first = from / tileBytes;
        last = (from + (size - lead) - 1) / tileBytes;
        if (last >= tiles)
            last = tiles - 1;
        for (size_t tile = first; tile <= last; ++tile)
            tex->dirtyTiles[tile >> 3] |= (u8)(1u << (tile & 7));
        tex->touched = true;
    }
}

void GSPGPU_FlushDataCache(const void *address, u32 size) { MemoryWritten(address, size); }

void C3D_SyncTextureCopy(const u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 size, u32 flags)
{
    (void)indim;
    (void)outdim;
    (void)flags;
    memcpy(outadr, inadr, size);
    MemoryWritten(outadr, size);
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */

static bool ApplyVoxelState(void)
{
    VoxVariant *variant = VoxFor();
    float constants[24];

    ApplyTarget();
    if (variant->program == 0)
        return false;
    glUseProgram(variant->program);
    for (int unit = 0; unit < 3; ++unit)
    {
        glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
        glBindTexture(GL_TEXTURE_2D, TexReady(CONTEXT_UNITS[unit]));
    }
    glActiveTexture(GL_TEXTURE0);
    /* Only what differs from what this program last drew with is sent: per chunk, the model-view alone. */
    for (int i = 0; i < 14;)
    {
        int j = i;

        while (j < 14 && (!variant->applied || memcmp(variant->appliedReg[j], G.regs[j], sizeof(G.regs[j])) != 0))
            ++j;
        if (j > i)
        {
            glUniform4fv(variant->reg + i, j - i, &G.regs[i][0]);
            memcpy(variant->appliedReg[i], G.regs[i], (size_t)(j - i) * sizeof(G.regs[0]));
            i = j;
        }
        else
            ++i;
    }
    if (!variant->applied || variant->appliedDepth[0] != G.depthScale || variant->appliedDepth[1] != G.depthOffset)
    {
        glUniform2f(variant->depth, G.depthScale, G.depthOffset);
        variant->appliedDepth[0] = G.depthScale;
        variant->appliedDepth[1] = G.depthOffset;
    }
    for (int i = 0; i < 6; ++i)
    {
        const C3D_TexEnv *e = &G.env[i];

        constants[i * 4] = (float)(e->color & 255) / 255.0f;
        constants[i * 4 + 1] = (float)((e->color >> 8) & 255) / 255.0f;
        constants[i * 4 + 2] = (float)((e->color >> 16) & 255) / 255.0f;
        constants[i * 4 + 3] = (float)((e->color >> 24) & 255) / 255.0f;
    }
    if (!variant->applied || memcmp(variant->appliedConst, constants, sizeof(constants)) != 0)
    {
        glUniform4fv(variant->constants, 6, constants);
        memcpy(variant->appliedConst, constants, sizeof(constants));
    }
    if (!variant->applied || variant->appliedRef != (float)G.alphaRef)
    {
        glUniform1f(variant->alphaRef, (float)G.alphaRef / 255.0f);
        variant->appliedRef = (float)G.alphaRef;
    }
    variant->applied = true;
    ApplyPipeline();
    return true;
}

/* Both indexed static models and streamed terrain use the voxel vertex layout and shader. */
static int VoxelVertices(const u8 *data, unsigned stride, int first, int size)
{
    static const GLenum glFormats[4] = { GL_BYTE, GL_UNSIGNED_BYTE, GL_SHORT, GL_FLOAT };
    static const unsigned sizes[4] = { 1, 1, 2, 4 };
    unsigned offset = 0;
    VramBlock *block;

    if (!ApplyVoxelState())
        return -1;
    glBindVertexArray(G.voxVao);
    block = BlockOf(data);
    if (block != NULL)
    {
        BlockCurrent(block);
        offset = (unsigned)(data - block->base);
    }
    else
    {
        glBindBuffer(GL_ARRAY_BUFFER, G.voxStream);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)size * stride), data + (size_t)first * stride, GL_STREAM_DRAW);
    }
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    for (unsigned i = 0; i < G.attrInfo.loaders; ++i)
    {
        unsigned index = G.attrInfo.loader[i].index, format = G.attrInfo.loader[i].format, components = G.attrInfo.loader[i].count;

        if (index <= 1)
        {
            glEnableVertexAttribArray(index);
            glVertexAttribPointer(index, (GLint)components, glFormats[format], GL_FALSE, (GLsizei)stride, (void *)(uintptr_t)offset);
        }
        offset += components * sizes[format];
    }
    return block != NULL ? first : 0;
}

static void DrawVoxelElements(const u8 *vertices, unsigned stride, unsigned vertexCount, const void *indices, int count, int type)
{
    if (stride == 0 || VoxelVertices(vertices, stride, 0, (int)vertexCount) < 0)
        return;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.rawIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)count * (type == C3D_UNSIGNED_BYTE ? 1 : 2), indices, GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, count, type == C3D_UNSIGNED_BYTE ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT, 0);
    CheckError("voxel indexed draw");
    CountDraw(G.target, true, (unsigned)count / 6);
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.rawVbo);
}

void C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size)
{
    int glFirst;
    FlushBatch();
    if (G.skip || G.target == NULL || first < 0 || size <= 0 || G.bufInfo.data == NULL || G.bufInfo.stride == 0 || primitive != GPU_TRIANGLES || !G.voxelBound)
        return;
    glFirst = VoxelVertices(G.bufInfo.data, G.bufInfo.stride, first, size);
    if (glFirst < 0)
        return;
    glDrawArrays(GL_TRIANGLES, glFirst, size);
    CheckError("voxel draw");
    CountDraw(G.target, true, (unsigned)size / 6);
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.rawVbo);
}

/* ══ Extra presents: the frame as it was drawn, again, for a panel that refreshes faster than the game ticks ══
 *
 * The game ticks at 60 Hz, one compositor frame each. A 120 Hz panel shows two pictures per tick: the
 * frame's own, and a second one that is the same frame with the voxel world drawn a half tick on (its
 * camera and its moving billboards carried forward by what they moved in the tick before), so that a
 * walk across the world is smooth at the panel's rate. Every draw the compositor issues is recorded as
 * it is made, with the state it was made in, and the second picture issues them again from the point
 * where the voxel world is drawn into its surface; the world itself is not recorded but redrawn through
 * the function it was drawn by (C3D_ReplayWorld), asked for a half tick later. A frame with no voxel world
 * is shown again as it is. */

typedef struct
{
    C3D_RenderTarget *target;
    bool skip;
    int sceneW, sceneH;
    C3D_Mtx view;
    GPU_SCISSORMODE scissorMode;
    u32 scissor[4];
    bool blend;
    GPU_BLENDEQUATION colorEq, alphaEq;
    GPU_BLENDFACTOR srcColor, dstColor, srcAlpha, dstAlpha;
    u32 blendColor;
    bool alphaTest;
    GPU_TESTFUNC alphaFunc;
    int alphaRef;
    bool depthTest;
    GPU_TESTFUNC depthFunc;
    u8 writeMask;
    C3D_TexEnv env[6];
    DrawMode mode;
    C3D_Tex *batchTexture;
    C3D_AttrInfo attrInfo;
    u32 stride;
    float fixed2[3], fixed3[4];
} Snap;

typedef enum { ENTRY_BATCH, ENTRY_RAW, ENTRY_CLEAR, ENTRY_WORLD } EntryKind;

typedef struct
{
    EntryKind kind;
    Snap snap;
    size_t data, bytes;        /* arena offset and size: a batch's vertices, a raw draw's vertices */
    size_t indexData, indexBytes;
    unsigned quads;
    int count, type;
    u8 clearBits;
    u32 clearColor, clearDepth;
    C3D_RenderTarget *clearTarget;
    C3D_WorldDraw world;
    int eye;
} Entry;

static struct
{
    bool enabled;              /* recording is on: the panel shows more pictures than the game ticks */
    bool recording, replaying;
    bool haveWorld;
    bool valid;                /* a frame has been drawn since the last extra picture */
    Entry *entries;
    size_t count, capacity;
    u8 *arena;
    size_t used, arenaCapacity;
    float alpha;               /* how far past the tick the picture being replayed is, in ticks */
    unsigned swaps;            /* the top screen's swaps since the tick began */
} R;

static struct { double world, issue, after; unsigned frames; uint64_t worldEnd; } sProf;

static uint64_t NowNs(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/* settings.txt gpu_prof=1: the voxel world's and the rest of the frame's GPU time, measured with a glFinish around each. */
int CtrSettings_GetInt(const char *key, int fallback);
static bool GpuProf(void)
{
    static int cached = -1;
    static unsigned asked;

    if (cached < 0 || ++asked % 600 == 0)
        cached = CtrSettings_GetInt("gpu_prof", 0) != 0;
    return cached != 0;
}

static void SnapTake(Snap *s)
{
    s->target = G.target;
    s->skip = G.skip;
    s->sceneW = G.sceneW;
    s->sceneH = G.sceneH;
    s->view = G.view;
    s->scissorMode = G.scissorMode;
    memcpy(s->scissor, G.scissor, sizeof(s->scissor));
    s->blend = G.blend;
    s->colorEq = G.colorEq;
    s->alphaEq = G.alphaEq;
    s->srcColor = G.srcColor;
    s->dstColor = G.dstColor;
    s->srcAlpha = G.srcAlpha;
    s->dstAlpha = G.dstAlpha;
    s->blendColor = G.blendColor;
    s->alphaTest = G.alphaTest;
    s->alphaFunc = G.alphaFunc;
    s->alphaRef = G.alphaRef;
    s->depthTest = G.depthTest;
    s->depthFunc = G.depthFunc;
    s->writeMask = G.writeMask;
    memcpy(s->env, G.env, sizeof(s->env));
    s->mode = G.mode;
    s->batchTexture = G.batchTexture;
    s->attrInfo = G.attrInfo;
    s->stride = G.bufInfo.stride;
    memcpy(s->fixed2, G.fixed[2], sizeof(s->fixed2));
    memcpy(s->fixed3, G.fixed[3], sizeof(s->fixed3));
}

static void SnapPut(const Snap *s)
{
    G.target = s->target;
    G.skip = s->skip;
    G.sceneW = s->sceneW;
    G.sceneH = s->sceneH;
    G.view = s->view;
    G.scissorMode = s->scissorMode;
    memcpy(G.scissor, s->scissor, sizeof(G.scissor));
    G.blend = s->blend;
    G.colorEq = s->colorEq;
    G.alphaEq = s->alphaEq;
    G.srcColor = s->srcColor;
    G.dstColor = s->dstColor;
    G.srcAlpha = s->srcAlpha;
    G.dstAlpha = s->dstAlpha;
    G.blendColor = s->blendColor;
    G.alphaTest = s->alphaTest;
    G.alphaFunc = s->alphaFunc;
    G.alphaRef = s->alphaRef;
    G.depthTest = s->depthTest;
    G.depthFunc = s->depthFunc;
    G.writeMask = s->writeMask;
    memcpy(G.env, s->env, sizeof(G.env));
    G.mode = s->mode;
    G.batchTexture = s->batchTexture;
    G.attrInfo = s->attrInfo;
    G.bufInfo.stride = s->stride;
    memcpy(G.fixed[2], s->fixed2, sizeof(s->fixed2));
    memcpy(G.fixed[3], s->fixed3, sizeof(s->fixed3));
}

static Entry *EntryAdd(EntryKind kind)
{
    Entry *entry;

    if (R.count == R.capacity)
    {
        size_t capacity = R.capacity != 0 ? R.capacity * 2 : 512;
        Entry *grown = realloc(R.entries, capacity * sizeof(Entry));

        if (grown == NULL)
        {
            R.recording = false;
            return NULL;
        }
        R.entries = grown;
        R.capacity = capacity;
    }
    entry = &R.entries[R.count++];
    memset(entry, 0, sizeof(*entry));
    entry->kind = kind;
    return entry;
}

static bool ArenaCopy(const void *data, size_t bytes, size_t *offset)
{
    if (R.used + bytes > R.arenaCapacity)
    {
        size_t capacity = R.arenaCapacity != 0 ? R.arenaCapacity : (size_t)1 << 20;
        u8 *grown;

        while (capacity < R.used + bytes)
            capacity *= 2;
        grown = realloc(R.arena, capacity);
        if (grown == NULL)
        {
            R.recording = false;
            return false;
        }
        R.arena = grown;
        R.arenaCapacity = capacity;
    }
    memcpy(R.arena + R.used, data, bytes);
    *offset = R.used;
    R.used += (bytes + 15) & ~(size_t)15;
    return true;
}

static bool Recording(void) { return R.recording && !R.replaying; }

static void RecordBatch(void)
{
    Entry *entry;
    size_t offset;

    if (!Recording() || (entry = EntryAdd(ENTRY_BATCH)) == NULL)
        return;
    SnapTake(&entry->snap);
    entry->quads = G.quads;
    if (!ArenaCopy(G.vertices, (size_t)G.quads * 4 * sizeof(BatchVertex), &offset))
        --R.count;
    else
        entry->data = offset;
}

static void RecordRaw(const u8 *vertices, size_t vertexBytes, const void *indices, int count, int type)
{
    Entry *entry;
    size_t indexBytes = (size_t)count * (type == C3D_UNSIGNED_BYTE ? 1 : 2);

    if (!Recording() || (entry = EntryAdd(ENTRY_RAW)) == NULL)
        return;
    SnapTake(&entry->snap);
    entry->count = count;
    entry->type = type;
    entry->bytes = vertexBytes;
    entry->indexBytes = indexBytes;
    if (!ArenaCopy(vertices, vertexBytes, &entry->data) || !ArenaCopy(indices, indexBytes, &entry->indexData))
        --R.count;
}

static void RecordClear(C3D_RenderTarget *target, u8 bits, u32 color, u32 depth)
{
    Entry *entry;

    if (!Recording() || (entry = EntryAdd(ENTRY_CLEAR)) == NULL)
        return;
    entry->clearTarget = target;
    entry->clearBits = bits;
    entry->clearColor = color;
    entry->clearDepth = depth;
}

void C3D_ReplayEnable(bool on)
{
    if (R.enabled != on)
    {
        R.enabled = on;
        R.recording = false;
        R.haveWorld = false;
        R.count = R.used = 0;
    }
}

/* The compositor draws the voxel world through here: the draw is made now, and remembered to be made again. */
void C3D_ReplayWorld(C3D_WorldDraw draw, C3D_RenderTarget *target, int eye)
{
    if (Recording())
    {
        Entry *entry = EntryAdd(ENTRY_WORLD);

        if (entry != NULL)
        {
            entry->world = draw;
            entry->clearTarget = target;
            entry->eye = eye;
            R.haveWorld = true;
        }
    }
    if (GpuProf())
    {
        uint64_t t0, t1;

        uint64_t tc;

        glFinish();
        t0 = NowNs();
        draw(target, eye, 0.0f);
        tc = NowNs();
        glFinish();
        t1 = NowNs();
        sProf.issue += (double)(tc - t0) / 1e6;
        sProf.world += (double)(t1 - t0) / 1e6;
        sProf.worldEnd = t1;
        return;
    }
    draw(target, eye, 0.0f);
}

static void ReplayClear(C3D_RenderTarget *target, u8 clearBits, u32 clearColor, u32 clearDepth)
{
    C3D_RenderTargetClear(target, clearBits, clearColor, clearDepth);
}

static bool EntryIsBottom(const Entry *entry)
{
    const C3D_RenderTarget *target = entry->kind == ENTRY_CLEAR || entry->kind == ENTRY_WORLD ? entry->clearTarget : entry->snap.target;

    return target != NULL && target->bottom;
}

/* Shows a picture on the top screen: the frame's recorded draws from the voxel world on, or the finished picture again. */
bool C3D_ExtraFrame(float alpha)
{
    size_t first = R.count;

    if (!R.enabled || !R.valid || G.topFbo == 0)
        return false;
    AndroidHost_GlMakeCurrent();
    if (alpha > 0.0f && R.haveWorld && R.count != 0)
    {
        Snap saved;
        bool savedInFrame = G.inFrame;
        unsigned savedQuads = G.quads;

        for (size_t i = 0; i < R.count; ++i)
            if (R.entries[i].kind == ENTRY_WORLD)
            {
                first = i;
                break;
            }
        /* Back over what cleared the surface it is drawn into just before. */
        while (first > 0 && R.entries[first - 1].kind == ENTRY_CLEAR && R.entries[first - 1].clearTarget == R.entries[first].clearTarget)
            --first;
        SnapTake(&saved);
        G.quads = 0;
        G.inFrame = true;
        R.replaying = true;
        R.alpha = alpha;
        for (size_t i = first; i < R.count; ++i)
        {
            Entry *entry = &R.entries[i];

            if (EntryIsBottom(entry))
                continue;
            switch (entry->kind)
            {
            case ENTRY_CLEAR:
                ReplayClear(entry->clearTarget, entry->clearBits, entry->clearColor, entry->clearDepth);
                break;
            case ENTRY_WORLD:
                entry->world(entry->clearTarget, entry->eye, alpha);
                break;
            case ENTRY_BATCH:
                SnapPut(&entry->snap);
                DrawBatchNow((const BatchVertex *)(R.arena + entry->data), entry->quads);
                break;
            case ENTRY_RAW:
                SnapPut(&entry->snap);
                DrawRawNow(R.arena + entry->data, entry->bytes, R.arena + entry->indexData, entry->count, entry->type);
                break;
            }
        }
        R.replaying = false;
        SnapPut(&saved);
        G.quads = savedQuads;
        G.inFrame = savedInFrame;
    }
    ShowTop();
    return true;
}

bool C3D_ReplayRecording(void) { return R.enabled; }

static void ReplayFrameEnd(void)
{
    R.recording = false;
    R.valid = R.enabled;
    if (GpuProf())
    {
        uint64_t t;

        glFinish();
        t = NowNs();
        if (sProf.worldEnd != 0)
            sProf.after += (double)(t - sProf.worldEnd) / 1e6;
        sProf.worldEnd = 0;
        if (++sProf.frames == 300)
        {
            CtrLog_Write(CTR_LOG_VIDEO, "GPU PROF per frame: world %.2f ms (%.2f ms to issue), compose after it %.2f ms", sProf.world / sProf.frames,
                         sProf.issue / sProf.frames, sProf.after / sProf.frames);
            memset(&sProf, 0, sizeof(sProf));
        }
    }
}

static void ReplayFrameBegin(void)
{
    bool wanted = AndroidHost_PresentsPerTick() > 1;

    R.valid = false;
    if (wanted != R.enabled)
        C3D_ReplayEnable(wanted);
    if (!R.enabled)
        return;
    R.count = 0;
    R.used = 0;
    R.haveWorld = false;
    R.recording = true;
}

/* After the frame's own present (3ds_platform.c): the pictures a faster panel shows for the same tick. */
void AndroidVideo_PresentExtra(void)
{
    static unsigned ticks;
    unsigned presents;

    /* settings.txt refresh_hz=60 asks the system for 60 Hz (battery); the default is the panel's best. */
    if (++ticks % 60 == 1)
        AndroidHost_SetRefreshHint(CtrSettings_GetInt("refresh_hz", 120));
    presents = AndroidHost_PresentsPerTick();

    for (unsigned i = 1; i < presents; ++i)
        /* Fast-forward keeps its vsync budget but reuses the finished picture to save a world draw. */
        C3D_ExtraFrame(CtrSettings_SpeedHalves() > 2 ? 0.0f : (float)i / (float)presents);
    R.valid = false;
}
