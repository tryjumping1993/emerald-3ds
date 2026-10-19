"""Compile the real GLES draw routing against a recording GL, without a GPU or NDK."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature + "\n{")
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


PRELUDE = r'''
#include <assert.h>
#include <stdlib.h>
#include "citro3d.h"
typedef int GLenum, GLint, GLsizei;
typedef ptrdiff_t GLsizeiptr;
enum {GL_BYTE=1, GL_UNSIGNED_BYTE, GL_SHORT, GL_FLOAT, GL_FALSE, GL_ARRAY_BUFFER,
      GL_ELEMENT_ARRAY_BUFFER, GL_STREAM_DRAW, GL_TRIANGLES, GL_UNSIGNED_SHORT};
enum {MODE_RAW, MODE_IMAGE, ENV_C2D=99};
static struct {
    bool skip, voxelBound;
    C3D_RenderTarget *target;
    C3D_BufInfo bufInfo;
    C3D_AttrInfo attrInfo;
    C3D_TexEnv env[6];
    C3D_Tex *batchTexture, *bound;
    unsigned voxVao, vao, voxStream, rawVbo, rawIbo;
    int mode;
} G;
typedef struct {const u8 *base;} VramBlock;
static VramBlock block;
static bool useBlock;
static unsigned applied, indexed, raw, recorded, arrays;
static const void *vertexData;
static size_t vertexBytes, indexBytes, offsets[2];
static int formats[2], counts[2], drawnType, drawnCount;
static int lastArrayFirst;
static void FlushBatch(void) {}
static bool ApplyVoxelState(void) {++applied; return true;}
static VramBlock *BlockOf(const void *p) {(void)p; return useBlock ? &block : NULL;}
static void BlockCurrent(VramBlock *b) {(void)b;}
static void glBindVertexArray(unsigned id) {(void)id;}
static void glBindBuffer(GLenum target, unsigned id) {(void)target; (void)id;}
static void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    (void)usage;
    if (target == GL_ARRAY_BUFFER) {vertexData = data; vertexBytes = size;}
    else indexBytes = size;
}
static void glDisableVertexAttribArray(unsigned a) {(void)a;}
static void glEnableVertexAttribArray(unsigned a) {(void)a;}
static void glVertexAttribPointer(unsigned a, GLint n, GLenum type, int normalized, GLsizei stride, const void *offset) {
    (void)normalized; assert(stride == 16); assert(a < 2);
    offsets[a] = (size_t)offset; formats[a] = type; counts[a] = n;
}
static void glDrawElements(GLenum mode, int count, GLenum type, const void *indices) {
    assert(mode == GL_TRIANGLES && indices == NULL); ++indexed; drawnType = type; drawnCount = count;
}
static void glDrawArrays(GLenum mode, int first, int count) {
    assert(mode == GL_TRIANGLES); ++arrays; lastArrayFirst=first; drawnCount=count;
}
static void CheckError(const char *where) {(void)where;}
static void CountDraw(const C3D_RenderTarget *target, bool rawDraw, unsigned quads) {(void)target; (void)rawDraw; (void)quads;}
static void RecordRaw(const u8 *v, size_t bytes, const void *i, int count, int type) {
    (void)v; (void)bytes; (void)i; (void)count; (void)type; ++recorded;
}
static void DrawRawNow(const u8 *v, size_t bytes, const void *i, int count, int type) {
    (void)v; (void)bytes; (void)i; (void)count; (void)type; ++raw;
}
'''

MAIN = r'''
int main(void) {
    u8 vertices[16*8] = {0};
    u16 indices[] = {0, 3, 1};
    u8 small[] = {0, 2, 1};
    C3D_RenderTarget target;
    G.target=&target; G.voxelBound=true;
    G.bufInfo.data=vertices; G.bufInfo.stride=16;
    G.attrInfo.loaders=2;
    G.attrInfo.loader[0].index=1; G.attrInfo.loader[0].format=GPU_FLOAT; G.attrInfo.loader[0].count=2;
    G.attrInfo.loader[1].index=0; G.attrInfo.loader[1].format=GPU_SHORT; G.attrInfo.loader[1].count=4;
    C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
    assert(applied==1 && indexed==1 && raw==0 && recorded==0);
    assert(vertexData==vertices && vertexBytes==4*16 && indexBytes==6);
    assert(offsets[0]==8 && formats[0]==GL_SHORT && counts[0]==4);
    assert(offsets[1]==0 && formats[1]==GL_FLOAT && counts[1]==2);
    assert(drawnType==GL_UNSIGNED_SHORT && drawnCount==3);
    C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_BYTE, small);
    assert(indexed==2 && vertexBytes==3*16 && indexBytes==3 && drawnType==GL_UNSIGNED_BYTE);
    useBlock=true; block.base=vertices; G.bufInfo.data=vertices+32;
    C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
    assert(offsets[1]==32 && offsets[0]==40 && indexed==3);
    C3D_DrawArrays(GPU_TRIANGLES, 2, 3);
    assert(arrays==1 && lastArrayFirst==2);
    useBlock=false; G.bufInfo.data=vertices;
    C3D_DrawArrays(GPU_TRIANGLES, 2, 3);
    assert(arrays==2 && lastArrayFirst==0 && vertexData==vertices+32 && vertexBytes==48);
    SetC2dEnv(); assert(!G.voxelBound);
    C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
    assert(recorded==1 && raw==1 && indexed==3);
    C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, NULL);
    assert(recorded==1 && raw==1);
    puts("PASS GLES: indexed voxel shader/layout, byte/short indices, stream/VRAM offsets, 2D routing restored");
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    source = (ROOT / "src/c3d_gles.c").read_text(encoding="utf-8")
    signatures = ("static void SetC2dEnv(void)",
                  "static int VoxelVertices(const u8 *data, unsigned stride, int first, int size)",
                  "static void DrawVoxelElements(const u8 *vertices, unsigned stride, unsigned vertexCount, const void *indices, int count, int type)",
                  "void C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void *indices)",
                  "void C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size)")
    with tempfile.TemporaryDirectory(prefix="thor-gles-") as tmp:
        path = Path(tmp) / "test.c"
        path.write_text(PRELUDE + "".join(function(source, sig) for sig in signatures) + MAIN, encoding="utf-8")
        exe = Path(tmp) / "test.exe"
        subprocess.run([args.cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "include"),
                        str(path), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
