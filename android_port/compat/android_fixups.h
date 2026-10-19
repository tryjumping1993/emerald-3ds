#ifndef ANDROID_FIXUPS_H
#define ANDROID_FIXUPS_H
/* Declarations clang insists on and GCC lets a translation unit omit: src/music_player.c
 * (patch 0004's software mixer) calls ChnVolSetAsm before it defines it, where GCC takes
 * the implicit declaration and clang refuses the conflicting definition. Included with
 * -include for that object only (android_port/Makefile). The fix belongs in the patch. */
struct MixerSource;
struct MP2KTrack;
void ChnVolSetAsm(struct MixerSource *chan, struct MP2KTrack *track);
#endif
