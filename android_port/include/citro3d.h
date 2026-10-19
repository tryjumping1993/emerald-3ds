#ifndef ANDROID_SHIM_CITRO3D_H
#define ANDROID_SHIM_CITRO3D_H
/* ctr_voxel.h names Citro3D's render target and texture in a few prototypes. The Thor's
 * voxel renderer is not built yet (VOXEL=0): this keeps the headers that mention them
 * compiling, with the types left opaque. */
typedef struct C3D_RenderTarget C3D_RenderTarget;
typedef struct C3D_Tex C3D_Tex;
#endif
