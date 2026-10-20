"""Run the real voxel render interpolation without GL or advancing gameplay."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from save_storage_test import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    source = (ROOT / '3ds_port/src/voxel/ctr_voxel.c').read_text(encoding='utf-8')
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "voxel_camera.h"
typedef struct {float u, v; int16_t x, y, z, shade;} VoxelGpuVertex;
typedef void C3D_RenderTarget;
#define VOXEL_DYNAMIC_VERTICES 16
#define VOXEL_REFLECTION_FIRST 8
#define VOXEL_POS_SCALE 512.0f
#define CTR_VOXEL_LIGHTING 0
#define CTR_LOG_VIDEO 0
static VoxelCamera sCamera;
static uint32_t sFrame;
static VoxelGpuVertex vertices[16];
static VoxelGpuVertex *sDynamic = vertices;
static unsigned sSpriteVertices = 1, sReflectionVertices;
static int sDynamicX, sDynamicZ, sMeshMapGroup, sMeshMapNum;
static struct {bool on;} sBattle;
static bool sReady = true;
static unsigned sDrawCount = 1;
static int option = 1, speed = 2;
static bool recording = true;
static bool C3D_ReplayRecording(void) {return recording;}
static int CtrSettings_GetInt(const char *key, int fallback) {(void)key; (void)fallback; return option;}
static int CtrSettings_SpeedHalves(void) {return speed;}
static void CtrLog_Write(int level, const char *fmt, ...) {(void)level; (void)fmt;}
'''
    harness += source[source.index('static VoxelCamera sCameraLast,'):source.index('static void DynRanges(')]
    for signature in ('static void DynRanges(', 'static void ReplayNote(',
                      'static float InterpolatePosition(', 'static bool ReplayInterpolate('):
        harness += function(source, signature)
    harness += r'''
static float shownX;
static int shownVertex;
static void DrawWorldBody(C3D_RenderTarget *target, int eye) {
    (void)target; (void)eye; shownX = sCamera.x; shownVertex = sDynBound[0].x;
}
'''
    harness += function(source, 'static void DrawWorld(')
    harness += r'''
static void tick(unsigned frame, float x, int vertex) {
    sFrame = frame; sCamera.x = x; vertices[0].x = vertex;
}
int main(void) {
    tick(1, 0, 0); DrawWorld(NULL, 0, 0); assert(shownX == 0);
    tick(2, 2, 512);
    VoxelCamera unchanged = sCamera;
    VoxelGpuVertex unchangedVertices[16]; memcpy(unchangedVertices, vertices, sizeof(vertices));
    DrawWorld(NULL, 0, 0);
    assert(shownX == 0 && shownVertex == 0);
    DrawWorld(NULL, 0, 0.5f);
    assert(shownX == 1 && shownVertex == 256);
    assert(sFrame == 2 && memcmp(&sCamera, &unchanged, sizeof(sCamera)) == 0);
    assert(memcmp(vertices, unchangedVertices, sizeof(vertices)) == 0);
    assert(sDynBound == sDynamic);
    // Stopping never predicts past the current position.
    tick(3, 2, 512); DrawWorld(NULL, 0, 0); DrawWorld(NULL, 0, 0.5f);
    assert(shownX == 2 && shownVertex == 512);
    // The display option and fast-forward are independent controls.
    option = 0; tick(4, 4, 768); DrawWorld(NULL, 0, 0); assert(shownX == 4);
    option = 1; speed = 4; tick(5, 6, 1024); DrawWorld(NULL, 0, 0); assert(shownX == 6);
    speed = 2; recording = false; tick(6, 8, 1280); DrawWorld(NULL, 0, 0); assert(shownX == 8);
    // Map changes, battle transitions and skipped ticks never blend unrelated scenes.
    recording = true; ++sMeshMapNum;
    tick(7, 9, 1408); DrawWorld(NULL, 0, 0); DrawWorld(NULL, 0, 0.5f); assert(shownX == 9);
    sBattle.on = true;
    tick(8, 10, 1536); DrawWorld(NULL, 0, 0); DrawWorld(NULL, 0, 0.5f); assert(shownX == 10);
    tick(10, 11, 1664); DrawWorld(NULL, 0, 0); DrawWorld(NULL, 0, 0.5f); assert(shownX == 11);
    tick(11, 20, 8192); DrawWorld(NULL, 0, 0); DrawWorld(NULL, 0, 0.5f);
    assert(shownX == 20 && shownVertex == 8192);
    // A replaced sprite with different UVs stays at its own position.
    tick(12, 21, 8448); vertices[0].u = 1;
    DrawWorld(NULL, 0, 0); DrawWorld(NULL, 0, 0.5f);
    assert(shownX == 20.5f && shownVertex == 8448);
    puts("PASS interpolation: midpoint, stop, immutable simulation, independent speed, scene cuts and sprite replacement");
}
'''
    with tempfile.TemporaryDirectory() as folder:
        src, exe = Path(folder) / 'interpolation.c', Path(folder) / 'interpolation.exe'
        src.write_text(harness, encoding='utf-8')
        subprocess.run([args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / '3ds_port/src/voxel'), str(src), '-lm', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == '__main__':
    main()
