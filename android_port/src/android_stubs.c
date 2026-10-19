/*
 * What the reusable units still name of the 3DS's GPU and voxel renderer, answered as the
 * absence of both: the memory pools and the voxel world are not on the Thor yet (phase 4).
 */
#include <string.h>

#include "3ds_gpumem.h"
#include "voxel/ctr_voxel.h"

void CtrGpuMem_Report(const char *why) { (void)why; }

const CtrVoxelStats *CtrVoxel_GetStats(void)
{
    static const CtrVoxelStats none;

    return &none;
}

bool CtrVoxel_HoldsVram(void) { return false; }

void CtrVoxel_ArenaUse(uint32_t *chunkUsed, uint32_t *chunkLargest, uint32_t *pageUsed, uint32_t *pageLargest)
{
    *chunkUsed = *chunkLargest = *pageUsed = *pageLargest = 0;
}

/*
 * The Mystery Gift screens (src/mystery_gift_menu.c is a GBA-only program the build leaves
 * out): only the Union Room's wireless gift sending names them, which needs the local radio
 * the Thor does not have yet.
 */
#include <stdint.h>

uint8_t gGiftIsFromEReader;
int8_t DoMysteryGiftYesNo() { return -1; }
uint16_t GetMysteryGiftBaseBlock() { return 0; }
void MG_AddMessageTextPrinter() {}
void MG_DrawCheckerboardPattern() {}
void MG_DrawTextBorder() {}
void PrintMysteryGiftMenuMessage() {}
void PrintMysteryGiftOrEReaderHeader() {}
