#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "3ds_mods.h"

int main(void)
{
    assert(CtrMods_LevelCap(0)==100 && CtrMods_LevelCap(1)==125);
    assert(CtrMods_LevelCap(2)==150 && CtrMods_LevelCap(3)==200);
    assert(CtrMods_LevelCap(99)==100);
    assert(CtrMods_Experience(100,0,1000)==100);
    assert(CtrMods_Experience(100,1,1000)==200);
    assert(CtrMods_Experience(100,2,1000)==400);
    assert(CtrMods_Experience(100,3,1000)==800);
    assert(CtrMods_Experience(100,3,25)==25);
    assert(CtrMods_Experience(100,3,0)==0);
    assert(CtrMods_Experience(UINT32_MAX,3,UINT32_MAX)==32767);
    assert(CtrMods_Experience(100,99,1000)==100);

    uint16_t dex=999;
    uint8_t level=255;
    assert(!CtrMods_Legendary(60,0,0,0,100,&dex,&level));
    assert(!CtrMods_Legendary(14,4,0,0,100,&dex,&level));
    assert(!CtrMods_Legendary(0,4,0,0,100,&dex,&level));
    assert(!CtrMods_Legendary(60,99,0,0,100,&dex,&level));
    assert(!CtrMods_Legendary(60,1,1,0,100,&dex,&level));
    assert(dex==999 && level==255);
    assert(CtrMods_Legendary(60,1,0,0,100,&dex,&level));
    assert(CtrMods_Legendary(60,2,4,0,100,&dex,&level));
    assert(!CtrMods_Legendary(60,2,5,0,100,&dex,&level));
    assert(CtrMods_Legendary(60,3,9,0,100,&dex,&level));
    assert(!CtrMods_Legendary(60,3,10,0,100,&dex,&level));

    unsigned seen[387]={0}, unique=0;
    for(unsigned pick=0;pick<21*7;++pick) {
        assert(CtrMods_Legendary(60,4,99,pick,100,&dex,&level));
        assert(dex>0 && dex<=386 && level>=57 && level<=63);
        if(!seen[dex]++) ++unique;
    }
    assert(unique==21);
    const unsigned expected[]={144,145,146,150,151,243,244,245,249,250,251,
                               377,378,379,380,381,382,383,384,385,386};
    for(unsigned i=0;i<21;++i) assert(seen[expected[i]]==7);
    for(unsigned pick=0;pick<100;++pick) {
        assert(CtrMods_Legendary(15,4,0,pick,100,&dex,&level));
        assert(dex>=377 && dex<=379 && level>=12 && level<=18);
        assert(CtrMods_Legendary(200,4,99,pick,200,&dex,&level));
        assert(level>=197 && level<=200);
        assert(CtrMods_Legendary(200,4,99,pick,100,&dex,&level) && level==100);
    }
    assert(!CtrMods_Legendary(60,4,0,0,0,&dex,&level));
    assert(!CtrMods_Legendary(60,4,0,0,201,&dex,&level));
    puts("PASS gameplay mods: caps, EXP multipliers/overflow, all 21 legendaries, rates, party tiers, level bounds, vanilla defaults");
}
