#ifndef CTR_MODS_H
#define CTR_MODS_H
#include <stdbool.h>
#include <stdint.h>

/* Rules shared by the game hooks and ROM-free host tests. Species here are
 * National Dex numbers; the game converts them to its internal species IDs. */
unsigned CtrMods_LevelCap(unsigned option);
uint32_t CtrMods_Experience(uint32_t amount, unsigned option, uint32_t remaining);
bool CtrMods_Legendary(unsigned average, unsigned option, uint32_t chance,
                       uint32_t choice, unsigned cap, uint16_t *national, uint8_t *level);
#endif
