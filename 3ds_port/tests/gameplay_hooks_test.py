"""Production patched game functions: levels, daycare, starters and wild mods.

Run after bootstrap with --tree build/upstream. No ROM or GPU is needed.
"""
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
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    pokemon = (args.tree / 'src/pokemon.c').read_text(encoding='utf-8')
    starter = (args.tree / 'src/starter_choose.c').read_text(encoding='utf-8')
    daycare = (args.tree / 'src/daycare.c').read_text(encoding='utf-8')
    wild = (args.tree / 'src/wild_encounter.c').read_text(encoding='utf-8')
    source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef uint64_t u64; typedef int32_t s32; typedef bool bool8;
#define PLATFORM_3DS 1
#define TRUE 1
#define FALSE 0
#define STARTER_MON_COUNT 3
#define PARTY_SIZE 6
#define PYRAMID_LOCATION_NONE 0
#include "constants/species.h"
#include "constants/pokedex.h"
#include "constants/pokemon.h"
#include "3ds_mods.h"
#include "src/data/pokemon/experience_tables.h"
enum {MON_DATA_SPECIES,MON_DATA_LEVEL,MON_DATA_EXP,MON_DATA_IS_EGG};
struct Pokemon {u16 species; u8 level,egg; u32 exp;};
struct BoxPokemon {u16 species; u32 exp;};
static struct {u8 growthRate;} gSpeciesInfo[NUM_SPECIES];
static struct Pokemon gPlayerParty[PARTY_SIZE];
static unsigned cap=100, legendary=0, randomCalls;
static int starterDex;
static bool safari, pike, pyramid;
static u32 GetMonData(struct Pokemon *m,int field,...) {
    return field==MON_DATA_SPECIES ? m->species : field==MON_DATA_LEVEL ? m->level
         : field==MON_DATA_EXP ? m->exp : m->egg;
}
static void SetMonData(struct Pokemon *m,int field,const void *p) {
    if(field==MON_DATA_EXP) m->exp=*(const u32 *)p;
    else if(field==MON_DATA_LEVEL) m->level=*(const u8 *)p;
    else assert(0);
}
static u32 GetBoxMonData(struct BoxPokemon *m,int field,...) {
    return field==MON_DATA_EXP ? m->exp : m->species;
}
static void SetBoxMonData(struct BoxPokemon *m,int field,const void *p) {
    assert(field==MON_DATA_EXP);m->exp=*(const u32 *)p;
}
static unsigned CtrExtras_LevelCap(void) {return cap;}
static unsigned CtrExtras_StarterDex(void) {return starterDex>0 && starterDex<=386 ? starterDex : 0;}
static int CtrExtras_Get(const char *key) {assert(strcmp(key,"wild_legendaries")==0);return legendary;}
static u16 Random(void) {++randomCalls;return 0;}
static bool GetSafariZoneFlag(void) {return safari;}
static bool InBattlePike(void) {return pike;}
static int CurrentBattlePyramidLocation(void) {return pyramid;}
#define SPECIES_TO_NATIONAL(name) [SPECIES_##name - 1] = NATIONAL_DEX_##name
'''
    start = pokemon.index('static const u16 sSpeciesToNationalPokedexNum[')
    source += pokemon[start:pokemon.index('\n};', start)+3] + '\n'
    start = starter.index('static const u16 sStarterMon[')
    source += starter[start:starter.index('\n};', start)+3] + '\n'
    for signature in ['static bool8 IsModFatefulSpecies(', 'u8 GetMaxPokemonLevel(', 'u8 GetLevelFromMonExp(',
                      'u8 GetLevelFromBoxMonExp(', 'bool8 TryIncrementMonLevel(',
                      'u16 NationalPokedexNumToSpecies(']:
        source += function(pokemon, signature)
    source += function(starter, 'u16 GetStarterPokemon(')
    source += function(daycare, 'static u8 GetLevelAfterDaycareSteps(')
    source += function(wild, 'static void TryModLegendary(')
    source += r'''
int main(void) {
    for(unsigned g=0;g<8;++g) {
        gSpeciesInfo[SPECIES_TREECKO].growthRate=g;
        for(unsigned l=1;l<=200;++l) {
            struct Pokemon m={SPECIES_TREECKO,l,0,gExperienceTables[g][l]};
            assert(GetLevelFromMonExp(&m)==l);
            struct BoxPokemon box={m.species,m.exp};
            assert(GetLevelFromBoxMonExp(&box)==l);
            if(l>=100 && l<200) assert(gExperienceTables[g][l+1]>m.exp);
        }
        for(unsigned choice=0;choice<4;++choice) {
            cap=CtrMods_LevelCap(choice);
            struct Pokemon m={SPECIES_TREECKO,cap-1,0,gExperienceTables[g][cap]+100};
            assert(TryIncrementMonLevel(&m) && m.level==cap && m.exp==gExperienceTables[g][cap]);
            assert(!TryIncrementMonLevel(&m));
            struct BoxPokemon box={m.species,gExperienceTables[g][cap-1]};
            assert(GetLevelAfterDaycareSteps(&box,UINT32_MAX)==cap);
            assert(box.exp==gExperienceTables[g][cap-1]);
        }
        cap=100;
        struct Pokemon high={SPECIES_TREECKO,150,0,gExperienceTables[g][150]};
        assert(!TryIncrementMonLevel(&high) && high.exp==gExperienceTables[g][150]);
        assert(GetLevelFromMonExp(&high)==150);
        struct BoxPokemon highBox={high.species,high.exp};
        assert(GetLevelAfterDaycareSteps(&highBox,UINT32_MAX)==150);
    }
    starterDex=0;assert(GetStarterPokemon(0)==SPECIES_TREECKO);
    for(starterDex=1;starterDex<=386;++starterDex) {
        u16 species=GetStarterPokemon(0);
        assert(species>0 && species<NUM_SPECIES && sSpeciesToNationalPokedexNum[species-1]==starterDex);
        assert(GetStarterPokemon(1)==SPECIES_TORCHIC && GetStarterPokemon(2)==SPECIES_MUDKIP);
    }
    starterDex=999;assert(GetStarterPokemon(0)==SPECIES_TREECKO);
    starterDex=-1;assert(GetStarterPokemon(0)==SPECIES_TREECKO);
    assert(GetStarterPokemon(3)==SPECIES_TREECKO);
    assert(!IsModFatefulSpecies(SPECIES_MEW) && !IsModFatefulSpecies(SPECIES_DEOXYS));
    starterDex=151;assert(IsModFatefulSpecies(SPECIES_MEW) && !IsModFatefulSpecies(SPECIES_DEOXYS));
    starterDex=386;assert(IsModFatefulSpecies(SPECIES_DEOXYS) && !IsModFatefulSpecies(SPECIES_MEW));
    starterDex=0;legendary=4;
    assert(IsModFatefulSpecies(SPECIES_MEW) && IsModFatefulSpecies(SPECIES_DEOXYS));
    assert(!IsModFatefulSpecies(SPECIES_TREECKO));legendary=0;
    u16 species=SPECIES_ZIGZAGOON;u8 level=5;
    TryModLegendary(&species,&level);assert(species==SPECIES_ZIGZAGOON && randomCalls==0);
    legendary=4;TryModLegendary(&species,&level);assert(randomCalls==0); // Empty party.
    gPlayerParty[0]=(struct Pokemon){SPECIES_TREECKO,20,0,0};
    gPlayerParty[1]=(struct Pokemon){SPECIES_TORCHIC,60,0,0};
    gPlayerParty[2]=(struct Pokemon){SPECIES_MUDKIP,1,1,0}; // Egg excluded.
    TryModLegendary(&species,&level);assert(level==37 && species==SPECIES_ARTICUNO);
    unsigned calls=randomCalls;safari=true;TryModLegendary(&species,&level);safari=false;
    pike=true;TryModLegendary(&species,&level);pike=false;
    pyramid=true;TryModLegendary(&species,&level);assert(randomCalls==calls);
    puts("PASS production mod hooks: eight growth curves through 200, caps, daycare overflow, preserved high-level saves, all 386 starters, party average/eggs, protected encounters");
}
'''
    with tempfile.TemporaryDirectory(prefix='gameplay-hooks-') as tmp:
        tmp = Path(tmp)
        (tmp / 'test.c').write_text(source, encoding='utf-8')
        exe = tmp / 'test.exe'
        subprocess.run([args.cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(args.tree.resolve() / 'include'), '-I' + str(args.tree.resolve()),
                        '-I' + str(ROOT / 'include'), str(tmp / 'test.c'),
                        str(ROOT / 'src/3ds_mods.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
