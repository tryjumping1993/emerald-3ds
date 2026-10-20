"""Exercise the runtime edge emitter with synthetic relief; no ROM required."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    mesh = (ROOT / 'src/voxel/voxel_mesh_builder.c').read_text()
    source = r'''
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "voxel_mesh_builder.h"
#include "voxel_relief.h"
#define SHADE_TOP 1.0f
#define SHADE_NORTH 0.5f
#define SHADE_SOUTH 0.6f
#define SHADE_WEST 0.65f
#define SHADE_EAST 0.55f
static VoxelMapInstance maps[2];
static int16_t heights[2][25], depths[2][25];
static float bases[2];
static unsigned belt, quads, uvCalls;
static bool connected, water;
static VoxelVertex points[64][4];
const VoxelMapInstance *VoxelWorld_Instance(unsigned i) { return i == 0 ? maps : NULL; }
const VoxelMapInstance *VoxelWorld_GetInstanceAt(int x, int y) {
    if (x == 0 && y == 0) return maps;
    return connected && x == 0 && y == 1 ? maps + 1 : NULL;
}
float VoxelRelief_Base(const VoxelMapInstance *m) { return bases[m - maps]; }
unsigned VoxelRelief_Belt(const VoxelMapInstance *m) { (void)m; return belt; }
const int16_t *VoxelRelief_Cell(const VoxelMapInstance *m, int x, int y) {
    (void)x; (void)y; return heights[m - maps];
}
const int16_t *VoxelRelief_Depth(const VoxelMapInstance *m, int x, int y) {
    (void)x; (void)y; return depths[m - maps];
}
float VoxelRelief_CellLift(const VoxelMapInstance *m, int x, int y) {
    return VoxelRelief_Cell(m,x,y)[12]/16.0f;
}
float VoxelRelief_CellShift(const VoxelMapInstance *m, int x, int y) {
    return VoxelRelief_Depth(m,x,y)[12]/16.0f;
}
float VoxelRelief_LiftAt(float x, float y) {
    const VoxelMapInstance *m=VoxelWorld_GetInstanceAt((int)floorf(x),(int)floorf(y));
    return VoxelRelief_Base(m)+VoxelRelief_CellLift(m,(int)x,(int)y);
}
static bool BorderWaterLevel(int x, int y, float *level, float *shift) {
    (void)x; (void)y; *level=water ? -1.0f : 0; *shift=*level; return water;
}
bool VoxelMesh_TileUV(VoxelBuilder *b, int x, int y, float *u0, float *v0, float *u1, float *v1) {
    (void)b; (void)x; (void)y; ++uvCalls; *u0=*v0=0; *u1=*v1=1; return true;
}
void VoxelBuilder_Quad(VoxelBuilder *b, const VoxelVertex *a, const VoxelVertex *c,
                       const VoxelVertex *d, const VoxelVertex *e) {
    (void)b; assert(quads<64); points[quads][0]=*a; points[quads][1]=*c;
    points[quads][2]=*d; points[quads++][3]=*e;
}
'''
    source += function(mesh, 'static void EmitSeamSkirt(')
    source += r'''
static void Reset(void) {
    memset(maps,0,sizeof(maps)); memset(heights,0,sizeof(heights));
    memset(depths,0,sizeof(depths)); memset(bases,0,sizeof(bases));
    maps[0].width=maps[0].height=maps[1].width=maps[1].height=1;
    maps[1].originY=1; belt=0; connected=water=false; quads=uvCalls=0;
}
static bool Has(float x,float y,float z) {
    for(unsigned i=0;i<quads;++i) for(unsigned k=0;k<4;++k)
        if(fabsf(points[i][k].x-x)<0.0001f && fabsf(points[i][k].y-y)<0.0001f
         && fabsf(points[i][k].z-z)<0.0001f) return true;
    return false;
}
int main(void) {
    VoxelBuilder b={0};
    /* The raised terrace above the belt's trees: every edge, including
     * the south edge, meets the trunks' ground at its true depth. */
    Reset();
    for(int j=0;j<5;++j) for(int i=0;i<5;++i)
        heights[0][j*5+i]=depths[0][j*5+i]=16+4*i;
    EmitSeamSkirt(&b,maps,0,0); assert(quads==16);
    assert(Has(0,1,1) && Has(1,2,2) && Has(0,0,0) && Has(1,0,0));
    assert(Has(0,1,2) && Has(1,2,3) && Has(0,0,1) && Has(1,0,1));
    /* Existing lowered edges still meet the belt, point by point. */
    Reset();
    for(int i=0;i<25;++i) heights[0][i]=depths[0][i]=-16;
    EmitSeamSkirt(&b,maps,0,0); assert(quads==16);
    assert(Has(0,-1,-1) && Has(0,0,0));
    /* Route 115/Rustboro: unequal sea heights AND depths. A single wall
     * at our centre depth cannot join these two edge profiles. */
    Reset(); connected=true; bases[1]=-2;
    for(int i=0;i<25;++i) heights[0][i]=depths[0][i]=-16;
    EmitSeamSkirt(&b,maps,0,0);
    for(int k=0;k<=4;++k) assert(Has(k/4.0f,-1,0) && Has(k/4.0f,-2,1));
    /* Equal world heights with a depth gap (Route 104/105) remain joined. */
    quads=0; bases[1]=-1; EmitSeamSkirt(&b,maps,0,0);
    assert(Has(0,-1,0) && Has(0,-1,1));
    /* A sloping sea edge must use the corners rather than its centre. */
    quads=0;
    for(int k=0;k<5;++k) heights[1][k]=4*k;
    EmitSeamSkirt(&b,maps,0,0);
    assert(Has(0,-1,1) && Has(1,0,1));
    /* No extra geometry on matching ground or an explicitly drawn belt. */
    Reset(); EmitSeamSkirt(&b,maps,0,0); assert(quads==0 && uvCalls==0);
    belt=3; heights[0][0]=32; EmitSeamSkirt(&b,maps,0,0); assert(quads==0);
    Reset(); water=true;
    for(int i=0;i<25;++i) heights[0][i]=depths[0][i]=-16;
    EmitSeamSkirt(&b,maps,0,0); assert(quads==0);
    puts("PASS voxel seams: raised/lowered tree belts, south closure, sea steps, depth gaps, slopes, matching edges");
}
'''
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='seam-test-') as tmp:
        tmp = Path(tmp)
        (tmp / 'test.c').write_text(source)
        exe = tmp / 'test.exe'
        subprocess.run([args.cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'src/voxel'), str(tmp / 'test.c'), '-lm', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
