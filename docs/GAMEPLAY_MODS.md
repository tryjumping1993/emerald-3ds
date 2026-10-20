# Optional gameplay mods

Open **OPTIONS → CHEATS → GAMEPLAY MODS**. These options persist in
`settings.txt`; the original starter, level 100 cap, normal EXP, and ordinary
wild encounters are the defaults. They work in the 3DS and Android builds.

| Option | Values / effect |
|---|---|
| LEVEL CAP | 100, 125, 150, 200; applies to battle EXP, Rare Candies, and daycare |
| EXP MULTIPLIER | 1×, 2×, 4×, 8×; stacks with the existing EXP Share and EXP for catching options |
| LEFT STARTER | ORIGINAL or any of the 386 National Dex Pokémon; replaces the left ball in Birch's bag |
| STARTER STEP | Advance the starter selector by 1, 10, or 50 entries |
| WILD LEGENDARIES | OFF, 1%, 5%, 10%, 100% of ordinary generated encounters, subject to the party-level tiers below |
| PERFECT WILD IVS | Newly generated wild Pokémon have 31 in all six IVs |
| HEAL PARTY | Restores party HP, status, and PP immediately; save the game to retain the result |

EXP awards stop at the selected level cap and at 32,767 EXP per recipient
per award, the range supported by the game's EXP controller.

Choose the starter before opening Birch's bag. Its picture, name, cry, and
the Pokémon received use the selected species. The middle and right balls
remain Torchic and Mudkip. This does not replace an existing starter or
change the rival's teams. The choice does not require a completed Pokédex.

Legendary encounters replace ordinary grass, surfing, Rock Smash, and fishing
encounters; they do not create encounters on tiles that normally have none.
The encounter-rate option still applies, and Repels and encounter-suppressing
abilities check the resulting level. Safari and Frontier encounter generation
keeps its original species. Scripted encounters, outbreaks, roaming Pokémon,
and visible overworld Pokémon keep their own encounter paths.

The pool expands with the integer average of all non-egg party Pokémon,
including fainted ones. An empty party or an average below 15 adds none.
Every species stays in the pool after its tier unlocks:

| Average level | Species added |
|---|---|
| 15 | Regirock, Regice, Registeel |
| 20 | Articuno, Zapdos, Moltres |
| 25 | Raikou, Entei, Suicune |
| 35 | Latias, Latios |
| 40 | Mew, Celebi, Jirachi |
| 50 | Mewtwo, Lugia, Ho-Oh, Kyogre, Groudon |
| 60 | Rayquaza, Deoxys |

An eligible species is selected randomly and appears within three levels of
the party average, bounded by level 1 and the selected cap. These are normal
catchable wild Pokémon; the existing ALWAYS CATCH option also works. Their
appearance does not consume or change the game's legendary event flags.
Mew and Deoxys created by these options receive the Pokémon's fateful
encounter bit, so Emerald's special obedience check accepts them.

Levels 0–100 retain Emerald's original EXP values. Above 100, each growth
curve continues cubically from its level-100 value. Changing to a lower cap
stops further growth without reducing existing levels or EXP, including in
storage and daycare. Continue using a mod-enabled build for Pokémon above
100; an original Emerald build does not have these extended EXP tables.
The original save format's met-level field has only seven bits, so Pokémon
created above 127 record a met level of 127 while retaining their actual
current level and EXP.

Settings keys use option indices: `level_cap=0..3`,
`exp_multiplier=0..3`, `wild_legendaries=0..4`, `perfect_wild=0..1`,
`starter_step=0..2`. `starter_dex=0` means ORIGINAL and `1..386` selects a
National Dex entry. Settings are independent of the game save; Pokémon,
items, and healing are retained by saving normally.

Validation:

```sh
gcc -std=c99 -O2 -Wall -Wextra -Werror -I3ds_port/include \
    3ds_port/tests/gameplay_mods_test.c 3ds_port/src/3ds_mods.c -o gameplay_mods_test
./gameplay_mods_test
python 3ds_port/tests/gameplay_hooks_test.py --tree build/upstream --cc gcc
```

The first test uses no bootstrapped tree and runs in CI. The second compiles
the patched game functions and real EXP and species tables, checking levels
through 200, selected caps, daycare overflow, retained high-level Pokémon,
all 386 starter choices, egg exclusion, and protected encounter generation.
