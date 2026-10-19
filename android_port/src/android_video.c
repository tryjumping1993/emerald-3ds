/*
 * The video contract (3ds_video.h) for the Thor, first form: the decompilation's software
 * renderer (src/platform/gba_fast_draw.c, built here with RENDERER_FAST_DRAW) draws each
 * shown frame from the game's own VRAM, palette, OAM and registers into a 400x240 picture,
 * and the host shows it on the top screen. docs/THOR_PORT_PLAN.md phase 1.
 *
 * What the 3DS compositor does beyond that (the HD battle interface, the voxel world, the
 * margins of staged screens, the game's own screens carried to the bottom screen) is phase 2
 * and later; the calls that tell it about them are recorded here and mostly not acted on.
 * The one thing done is to centre the screens the game lays out for the GBA's 240x160
 * (stages, centred screens, the battle) where the 3DS compositor puts them, so that they
 * are readable.
 */
#include <3ds.h>
#include <string.h>

#include "3ds_platform.h"
#include "3ds_video.h"
#include "android_host.h"

void DrawFrame(uint16_t *pixels);

static uint16_t sFrame[CTR_GAME_WIDTH * CTR_GAME_HEIGHT];
static uint32_t sPicture[CTR_GAME_WIDTH * CTR_GAME_HEIGHT];
static CtrVideoStats sStats;
static CtrVideoMemory sMemory;
static bool sHold, sStage, sStageFaded, sStageZoomed, sBanner, sBattle, sTransition;
static unsigned sCentred;

bool CtrVideo_Init(void)
{
    memset(&sStats, 0, sizeof(sStats));
    CtrLog_Write(CTR_LOG_VIDEO, "software renderer: %dx%d on the top screen", CTR_GAME_WIDTH, CTR_GAME_HEIGHT);
    return true;
}

void CtrVideo_Shutdown(void) {}
void CtrVideo_Bind(CtrVideoMemory memory) { sMemory = memory; }
void CtrVideo_HoldTop(bool hold) { sHold = hold; }

/* The GBA picture's origin on the 400x240 screen, if the frame is one laid out for 240x160. */
static bool GbaFrameOrigin(int *x, int *y)
{
    if (sBattle || sTransition)
    {
        *x = CTR_BATTLE_X;
        *y = CTR_BATTLE_Y;
        return true;
    }
    if (sStage || (sCentred != CTR_CENTRED_NONE))
    {
        *x = CTR_STAGE_X;
        *y = CTR_STAGE_Y;
        return true;
    }
    return false;
}

static uint32_t Expand(uint16_t color)
{
    uint32_t r = color & 31, g = (color >> 5) & 31, b = (color >> 10) & 31;

    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

void CtrVideo_Present(void)
{
    uint64_t start = svcGetSystemTick();
    int originX, originY;

    if (sHold)
    {
        AndroidHost_PresentTop(NULL, CTR_GAME_WIDTH, CTR_GAME_HEIGHT);
        return;
    }
    DrawFrame(sFrame);
    if (sStats.frames % 120 == 0)
    {
        unsigned lit = 0;

        for (int i = 0; i < CTR_GAME_WIDTH * CTR_GAME_HEIGHT; ++i)
            lit += sFrame[i] != 0;
        unsigned vram = 0, palette = 0, objects = 0;

        for (unsigned i = 0; sMemory.vram != NULL && i < 0x18000; ++i)
            vram += sMemory.vram[i] != 0;
        for (unsigned i = 0; sMemory.palette != NULL && i < 512; ++i)
            palette += sMemory.palette[i] != 0;
        for (unsigned i = 0; sMemory.oam != NULL && i < 128; ++i)
            objects += (sMemory.oam[i * 4] & 0x300) != 0x200;
        CtrLog_Write(CTR_LOG_VIDEO, "frame %lu: DISPCNT %04x BG0CNT %04x backdrop %04x lit %u/%u vram %u plt %u obj %u",
                     (unsigned long)sStats.frames, sMemory.regs != NULL ? sMemory.regs[0] : 0xFFFF,
                     sMemory.regs != NULL ? sMemory.regs[4] : 0xFFFF, sMemory.palette != NULL ? sMemory.palette[0] : 0xFFFF,
                     lit, CTR_GAME_WIDTH * CTR_GAME_HEIGHT, vram, palette, objects);
    }
    if (GbaFrameOrigin(&originX, &originY))
    {
        /* The renderer drew the 240x160 picture at the top left: carry it to its place. */
        for (int y = 0; y < CTR_GAME_HEIGHT; ++y)
        {
            int sourceY = y - originY;

            for (int x = 0; x < CTR_GAME_WIDTH; ++x)
            {
                int sourceX = x - originX;
                bool inside = sourceX >= 0 && sourceX < 240 && sourceY >= 0 && sourceY < 160;

                sPicture[y * CTR_GAME_WIDTH + x] = inside
                    ? Expand(sFrame[sourceY * CTR_GAME_WIDTH + sourceX]) : 0xFF000000u;
            }
        }
    }
    else
    {
        for (int i = 0; i < CTR_GAME_WIDTH * CTR_GAME_HEIGHT; ++i)
            sPicture[i] = Expand(sFrame[i]);
    }
    AndroidHost_PresentTop(sPicture, CTR_GAME_WIDTH, CTR_GAME_HEIGHT);
    ++sStats.frames;
    sStats.cpuMs = (float)((svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11);
    sStats.mode = sBattle ? CTR_MODE_BATTLE : sStage ? CTR_MODE_STAGE : sCentred ? CTR_MODE_CENTRED : CTR_MODE_FIELD;
    sStats.route = CTR_ROUTE_MONO;
}

const CtrVideoStats *CtrVideo_GetStats(void) { return &sStats; }

/* ── What the 3DS compositor is told about the frame; the software renderer needs none of it ── */

void CtrVideo_ClearOamAnchors(void) {}
void CtrVideo_SetOamAnchor(unsigned first, unsigned end, int y) { (void)first; (void)end; (void)y; }
void CtrVideo_ClearVoxelWeatherOam(void) {}
void CtrVideo_MarkVoxelWeatherOam(unsigned first, unsigned end) { (void)first; (void)end; }
void CtrVideo_ClearFogOam(void) {}
void CtrVideo_MarkFogOam(unsigned first, unsigned end) { (void)first; (void)end; }
void CtrVideo_MarkWeatherOam(unsigned first, unsigned end, bool lattice) { (void)first; (void)end; (void)lattice; }
void CtrVideo_LatchExtPalettes(const uint16_t *colors, unsigned inUse) { (void)colors; (void)inUse; }
const uint16_t *CtrVideo_ExtPalette(unsigned index) { (void)index; return NULL; }
void CtrVideo_ClearExtPaletteOam(void) {}
void CtrVideo_MarkExtPaletteOam(unsigned first, unsigned end, unsigned index) { (void)first; (void)end; (void)index; }
void CtrVideo_NotifyTilesetAnimWrite(const void *dest, unsigned bytes) { (void)dest; (void)bytes; }
void CtrVideo_DecodeCounts(uint32_t counts[8]) { memset(counts, 0, 8 * sizeof(counts[0])); }
void CtrVideo_NotifyWrite(const void *dest, unsigned bytes) { (void)dest; (void)bytes; }
const uint8_t *CtrVideo_GetBgVram(void) { return sMemory.vram; }
void CtrVideo_RequestPlaneRelease(void) {}
void CtrVideo_SetLineWindow(const uint16_t *values, unsigned lines, bool both) { (void)values; (void)lines; (void)both; }
void CtrVideo_SetFieldLight(bool on, int x, int y, int radius) { (void)on; (void)x; (void)y; (void)radius; }
void CtrVideo_SetLineScroll(unsigned reg, bool wide, const void *values, unsigned lines) { (void)reg; (void)wide; (void)values; (void)lines; }
void CtrVideo_SetLineRegisters(const uint16_t *regs, unsigned lines) { (void)regs; (void)lines; }
void CtrVideo_ClearScreenOam(void) {}
void CtrVideo_MarkScreenOam(unsigned first, unsigned end) { (void)first; (void)end; }
void CtrVideo_MarkMachineOam(unsigned first, unsigned end) { (void)first; (void)end; }
void CtrVideo_ShowNoHome(void) {}

/* ── The frame's kind, which the framing above reads ─────────────────────── */

void CtrVideo_SetStage(bool stage) { sStage = stage; }
void CtrVideo_SetStageFaded(bool faded) { sStageFaded = faded; }
void CtrVideo_SetStageZoomed(bool zoomed) { sStageZoomed = zoomed; }
bool CtrVideo_StageZoomed(void) { return sStageZoomed; }
void CtrVideo_SetFieldBanner(bool banner) { sBanner = banner; }
void CtrVideo_SetCentred(unsigned screen) { sCentred = screen; }
void CtrVideo_SetBattle(bool battle) { sBattle = battle; }
void CtrVideo_SetTransition(bool transition) { sTransition = transition; }
bool CtrVideo_InBattle(void) { return sBattle || sTransition; }

/* ── The bottom screen's share of the compositor ─────────────────────────── */

bool CtrVideo_BottomInUse(void) { return false; }
bool CtrVideo_BottomWhole(void) { return false; }
int CtrVideo_BottomPictureY(int y) { (void)y; return -1; }

/* ── The voxel world's upload admission: there is none to admit ──────────── */

bool CtrVideo_TryVoxelUpload(void) { return false; }
unsigned CtrVideo_VoxelUploadsLeft(void) { return 0; }

/* ── Scene and cursor: the compositor's own tracking ─────────────────────── */

void CtrScene_Init(void) {}
void CtrScene_Update(void) {}
unsigned CtrScene_Mode(void) { return sBattle ? CTR_MODE_BATTLE : CTR_MODE_FIELD; }
void CtrCursor_Init(const void *tiles, const uint16_t *palette) { (void)tiles; (void)palette; }
void CtrCursor_Update(bool visible) { (void)visible; }
void CtrCursor_GetPosition(int *x, int *y) { *x = 0; *y = 0; }
