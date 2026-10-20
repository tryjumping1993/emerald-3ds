#include "3ds_mods.h"

unsigned CtrMods_LevelCap(unsigned option)
{
    static const unsigned caps[] = {100, 125, 150, 200};
    return option < 4 ? caps[option] : 100;
}

uint32_t CtrMods_Experience(uint32_t amount, unsigned option, uint32_t remaining)
{
    static const unsigned multipliers[] = {1, 2, 4, 8};
    unsigned multiplier = option < 4 ? multipliers[option] : 1;
    /* The game's EXP controller carries the award in a signed 16-bit task
     * field. Bound BEFORE multiplication, and never cross the selected cap. */
    uint32_t result = amount > 32767 / multiplier ? 32767 : amount * multiplier;
    return result < remaining ? result : remaining;
}

bool CtrMods_Legendary(unsigned average, unsigned option, uint32_t chance,
                       uint32_t choice, unsigned cap, uint16_t *national, uint8_t *level)
{
    static const struct { uint16_t national; uint8_t minimum; } pool[] = {
        {144,20}, {145,20}, {146,20}, /* Articuno, Zapdos, Moltres */
        {150,50}, {151,40},          /* Mewtwo, Mew */
        {243,25}, {244,25}, {245,25}, /* Raikou, Entei, Suicune */
        {249,50}, {250,50}, {251,40}, /* Lugia, Ho-Oh, Celebi */
        {377,15}, {378,15}, {379,15}, /* Regirock, Regice, Registeel */
        {380,35}, {381,35},          /* Latias, Latios */
        {382,50}, {383,50}, {384,60}, /* Kyogre, Groudon, Rayquaza */
        {385,40}, {386,60},          /* Jirachi, Deoxys */
    };
    static const unsigned rates[] = {0, 1, 5, 10, 100};
    unsigned count = 0;
    if (option >= 5 || average == 0 || cap < 1 || cap > 200
     || chance % 100 >= rates[option] || national == 0 || level == 0)
        return false;
    for (unsigned i = 0; i < sizeof(pool) / sizeof(pool[0]); ++i)
        if (average >= pool[i].minimum)
            ++count;
    if (count == 0)
        return false;
    unsigned pick = choice % count;
    for (unsigned i = 0; i < sizeof(pool) / sizeof(pool[0]); ++i)
        if (average >= pool[i].minimum && pick-- == 0)
        {
            int scaled = (int)average + (int)((choice / count) % 7) - 3;
            *national = pool[i].national;
            *level = scaled < 1 ? 1 : (unsigned)scaled > cap ? cap : (unsigned)scaled;
            return true;
        }
    return false;
}
