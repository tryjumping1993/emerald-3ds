# AYN Thor port: plan

## Implementation status — 2026-10-19

The Android engine now runs on an attached AYN Thor. The remainder of this
document preserves the original conversion design; this status section and
[THOR_BUILD.md](THOR_BUILD.md) describe the implementation as it stands.

| Area | Status |
|---|---|
| Engine, input, audio, files and dual displays | Implemented; Presentation, second Activity and inline routes |
| GLES compositor, voxel field and voxel battles | Implemented at 4x logical render resolution |
| 120 Hz output | Two pictures per game tick, with half-tick voxel motion; mode selected from Android display callbacks |
| Northeast black region | Fixed: indexed static meshes now use the voxel shader and vertex layout |
| Bottom OLED idle behavior | 20% brightness after 30 seconds; first touch wakes without acting |
| Remapping | Android Back menu, persistent button mapping, hats and triggers use the same mappings |
| Sleep / display changes | Engine and audio suspend; held inputs reset; missing secondary display falls back inline |
| Fast-forward | Retains display pacing, reuses the second picture to avoid an extra world render |
| Data installation | System document picker imports and verifies a pack before replacing it |
| Builder / web variants | Target-aware recipes and texture generation; optional manifest `platform` field |
| Release tooling | Environment/private-file signing, payload creation, recursive APK audit, release assets/checksums |

Device checks cover Littleroot, Rustboro, Slateport and a Route 101 voxel
battle. The black region is gone and the 120 Hz mode stays selected through
map warps; normal gameplay remains around 59.6 ticks per second. Host voxel
checks, the GLES draw regression and synthetic builder tests run locally.
See [THOR_VERIFICATION.md](THOR_VERIFICATION.md) for the device, host, recipe,
signing and release-audit results, and the 3DS baseline comparison.
Long-duration battery drain,
every weather condition and every firmware/display combination still need
broader hardware coverage. The English release assets and the 3DS build
have been checked locally; the assets have not been published.

The Android game build currently retains `PLATFORM_3DS` for the existing
portable game hooks and additionally defines `PLATFORM_ANDROID`; the global
`PLATFORM_PORT` rename proposed below has not been applied.

A native Android port of Emerald 3DS for the AYN Thor, the dual-screen
clamshell handheld: the game on the top screen, the touch interface on the
bottom one, as on the 3DS, without an emulator. This document plans the
**conversion**: what is kept, what is replaced, in which order, and how each
step is checked before any hardware is in hand. Hardware testing is planned
separately once the first builds exist (see [What waits for hardware](#what-waits-for-hardware)).

The plan is written against the tree at version 0.4.0. Section numbers are
referred to from the phases.

- [1. The target](#1-the-target)
- [2. What the port is made of today](#2-what-the-port-is-made-of-today)
- [3. Decisions](#3-decisions)
- [4. The Android backend, module by module](#4-the-android-backend-module-by-module)
- [5. Build and data pipeline](#5-build-and-data-pipeline)
- [6. Phases](#6-phases)
- [7. Risks and open questions](#7-risks-and-open-questions)
- [8. What waits for hardware](#8-what-waits-for-hardware)

## 1. The target

| | AYN Thor (Lite / Base / Pro / Max) |
|---|---|
| SoC | Snapdragon 865 (Lite); Snapdragon 8 Gen 2 (Base, Pro, Max). Adreno 650 / 740 |
| Memory | 8 GiB LPDDR4X (Lite); 12 to 16 GiB LPDDR5X |
| Top screen | 6.0" AMOLED, 1920x1080, up to 120 Hz, 16:9 |
| Bottom screen | 3.92" OLED, 1240x1080, 60 Hz, touch. Nearly square (1.15:1); the 3DS bottom screen is 4:3 |
| OS | Android 13 at launch; field reports of units on Android 14. AYN's dual-screen launcher ("Thor Control Center") drives the bottom screen between apps |
| Controls | Face buttons, D-pad, two sticks, L1/R1, L2/R2, back buttons, Start/Select, Home; presented to apps as a standard Android game controller |
| No | Stereoscopic 3D, 3D slider, DSP firmware, local wireless (UDS), HOME Menu forwarder, SD card as `sdmc:/` |

Facts that matter for the design, from projects that already target the
device (the dual-screen support of ScummVM, OpenNOW and a Minish Cap port):

- The bottom screen is a **second Android display** in the same process.
  Apps draw on it either with a `Presentation` (works on units running
  Android 14) or by launching a second `Activity` on that display with
  `ActivityOptions.setLaunchDisplayId` (the route taken for Android 13,
  where a `Presentation` on an internal display was refused). The display is
  found with `DisplayManager` (a public, non-default display that is on).
- A touch on the bottom display can **steal input focus** from the
  controller unless the window that owns it is `FLAG_NOT_FOCUSABLE` (the
  second-activity route) or the controller events are routed back.
- Display numbering is not to be assumed: on the Thor the activity's default
  display is the physical top screen, but other devices differ.
- The secondary display reports 1080x1240 in its own orientation and is
  shown landscape by the launcher: the port reads the size and rotation it
  is given rather than assuming either.

Sources are listed at the end of this document.

## 2. What the port is made of today

See [ARCHITECTURE.md](ARCHITECTURE.md). What matters for the conversion is
the boundary the port already draws between two kinds of translation unit:

- **Game units**, compiled with `-DPORT_BRIDGE -DPLATFORM_3DS -DPORTABLE`:
  the decompilation plus the port's bridge files. They never include
  libctru. They talk to the backend through the plain C headers in
  `3ds_port/include/` and `3ds_port/compat/`.
- **Native units**, the 3DS backend proper: libctru, Citro2D/Citro3D,
  threads, files.

That boundary is the port's single most valuable asset for this work: the
Android port is a second native backend behind the same headers, and the
game units carry over as they are.

### 2.1 Inventory

Sizes are lines of C. "SDK" lists what a unit needs from the 3DS SDK.

| Unit | Lines | Kind | SDK | Android |
|---|---:|---|---|---|
| `main_3ds.c` | 102 | native | heap override, Luma forwarder, `aptMainLoop` | **rewrite** (JNI entry, engine thread) |
| `3ds_platform.c` | 588 | native | APT hooks, `svcGetSystemTick`, threads, `osSetSpeedupEnable`, console | **rewrite**; the frame pacing and fast-forward logic (`FrameShown`, `PaceBehind`) is portable and moves across unchanged |
| `3ds_video.c` | 11361 | native | Citro3D, GX transfers, PICA texture formats, stereo, `gfx*` | **port to OpenGL ES** (the biggest item, see 4.3) |
| `3ds_video_decode.c` | 50 | native | none (host-tested), but `CtrVideo_Texel` encodes the PICA's Morton texel order | reuse, with the texel order chosen per target (4.8) |
| `3ds_audio.c` | 437 | native | NDSP, `linearAlloc`, `LightEvent`, `threadCreate`, `APT_CheckNew3DS` | **rewrite** on AAudio (4.4) |
| `3ds_input.c` | 52 | native | HID | **rewrite** (4.5); `3ds_input_decode.c` (46 lines, host-tested) is reused |
| `3ds_fs.c` | 89 | native | `romfsInit`, `sdmc:/` | **rewrite** (4.2) |
| `3ds_data.c` | 459 | native | `LightLock` only; stdio otherwise | reuse with a lock shim and path constants |
| `3ds_pak.c` | 120 | native | none (host-tested) | reuse |
| `3ds_log.c` | 438 | native | thread, `LightEvent`, `svcOutputDebugString`, console overlay | reuse the batching; replace the thread, the paths and the console with logcat |
| `3ds_settings.c` | 420 | native | `LightLock`, `LightEvent`, thread | reuse with the lock/thread shim (a pthread stand-in already exists for its host test, `tests/settings_host/3ds.h`) |
| `3ds_prof.c`, `3ds_scenario.c` | 464, 555 | native | ticks, kernel and firmware version, system model, slider; the scenario unit includes `<3ds.h>` but calls nothing | reuse with a tick shim; drop the console-specific identity lines |
| `3ds_gpumem.c` | 320 | native | linear/VRAM allocators (`--wrap`) | **drop** (no GPU arenas on Android) |
| `3ds_bottom_screen.c` | 114 | native | bottom framebuffer, cache flush | **rewrite** as a GL blit of the canvas (4.7) |
| `3ds_local.c` | 1357 | native | UDS, thread | **stub first**, Wi-Fi later (4.9) |
| `3ds_daynight.c`, `3ds_clock_store.c`, `3ds_encounters.c`, `3ds_field_view.c` | 257, 117, 23, 157 | native | none (host-tested) | reuse |
| `voxel/ctr_voxel.c` | 8034 | native | Citro3D, PICA shader, VRAM arenas, GX queue, stereo | **port to OpenGL ES** (4.8) |
| `voxel/voxel_*.c` (13 units) | ~9500 | game | none | reuse |
| `3ds_assets.c`, `3ds_map_loader.c`, `3ds_script_loader.c`, `3ds_compat.c`, `3ds_game_full.c`, `3ds_game_bridge.c` | ~3300 | game | none | reuse |
| `3ds_bottom_ui.c`, `3ds_battle_hud.c`, `3ds_extras.c` | 7868, 1224, 300 | game | none | reuse |
| `3ds_rfu_adapter.c`, `3ds_sound.c`, `3ds_map_events.c`, `3ds_sprite_ids.c`, `3ds_prof_wrap.c`, `3ds_encounter_tables.c`, `3ds_cycle_test.c` | ~2000 | game | none | reuse |
| `patches/pokeemerald/*.patch` | 63769 | game tree | none (see 3.4 for `PLATFORM_3DS`) | reuse |

Roughly: of 52,000 lines in `3ds_port/src`, about 20,000 are native code
that talks to the SDK, and 19,400 of those are the two GPU renderers. The
rest reuses.

### 2.2 Things the game tree assumes

- **Pointers are 32 bits.** The script bundle, the song bundle and the game
  data bundle store relocation sites as 4-byte words at unaligned offsets
  (`3ds_script_loader.c`, `scripts/ctr_bundle.py`); the asset index maps
  INCBIN stub *addresses* as `u32` (`3ds_assets.c`); the decompilation
  itself reads pointers out of byte streams as 32-bit words
  (`FieldEffectScript_ReadWord`, the event script commands). None of this
  works in a 64-bit process without rewriting how the game addresses its
  data. See decision 3.1.
- **The logical display is 400x240** (`DISPLAY_WIDTH` under `PLATFORM_3DS`,
  `include/gba/defines.h` in patch 0004), with the field view widened and
  GBA-sized screens composed centred with margins. This is game-side
  layout and carries over; the Thor's 16:9 top screen shows it pillarboxed
  (3.6).
- **ARM state.** The 3DS build is ARM-only because ARMv6K has no Thumb
  hard-float ABI. The architecture notes speak of guards around function
  pointers with bit 0 set, but the patches hold none today: the one upstream
  idiom that masks a Thumb bit (`SoundMainRAM & ~1` in `m4a.c`) is already
  under `#ifndef PORTABLE`, and the script tables use plain `.int symbol`.
  The NDK's armeabi-v7a is Thumb-2 with interworking done by the toolchain;
  `-marm` for the game units is the escape hatch if a hidden assumption
  turns up.
- **Little endian, unaligned reads through `memcpy`.** Same on ARMv7. The
  BIOS emulation (`src/platform/bios.c`) and the emulated DMA copy through
  `u32 *` without alignment checks, which ARMv7 tolerates as ARMv6K does.
- **Absolute addresses in the asset index.** `asset_index.bin` and
  `asset_ptr_index.bin` hold the INCBIN stubs' link-time addresses and are
  compared against raw pointers with no load delta (`3ds_assets.c`), while
  the bundles rebase themselves through a reference symbol
  (`3ds_script_loader.c`). A 3DSX loads at a fixed base; an Android library
  does not (ASLR), so the index must learn the same delta (4.2).
- **The PICA's texel order leaks into game units.** The voxel atlases,
  sprites, building pages and grading look-ups are composed in Morton
  (8x8 swizzled) order through `CtrVideo_Texel` (`voxel_atlas.c`,
  `voxel_entities.c`, `voxel_building.c`, `voxel_grade.c`), and two
  generated files ship pre-swizzled: `voxel/buildings.bin` (game data, built
  by the builder) and `voxel/trees.rgba5551` (an engine file). The order is
  one function and one generator flag away from linear (4.8).
- **`romfs:/` and `sdmc:/` written as literals in game units**:
  `3ds_assets.c`, `3ds_script_loader.c`, `3ds_battle_hud.c`,
  `3ds_bottom_ui.c`, `voxel/voxel_tree.h` and the save path in
  `3ds_compat.c`. Each goes through `CtrFs` or a path macro (4.2).
- **A software renderer exists but is not built.** `src/platform/gba_fast_draw.c`
  (patch 0004) draws the GBA picture scanline by scanline into a 16-bit
  buffer at `DISPLAY_WIDTH` x `DISPLAY_HEIGHT` under `RENDERER_FAST_DRAW`.
  It is the bring-up renderer of this plan (6, phase 1).

## 3. Decisions

### 3.1 A 32-bit build (`armeabi-v7a`)

The port compiles for `armeabi-v7a`, not `arm64-v8a`, because of 2.2: the
data pipeline, the bundles and the game's own pointer handling are 32-bit.
The alternative, a 64-bit build, means teaching the decompilation and every
generator to use 32-bit *offsets* instead of pointers, or placing all game
code and data below 4 GiB in a 64-bit process, which Android's loader does
not offer. Neither is a port; both are projects of their own.

All four Thor models execute 32-bit code: the Snapdragon 865 outright, and
the Snapdragon 8 Gen 2 on the cores Qualcomm kept 32-bit capable (reported
as the Cortex-A710 pair and the A510 cluster). A 32-bit process is
scheduled on those cores only, which still leaves it several times a New
3DS. Android 13 and 14 run 32-bit apps on such hardware. The APK is
sideloaded, so Google Play's 64-bit requirement does not apply.

This is the plan's first thing to confirm on a device (8): `getprop
ro.product.cpu.abilist` lists `armeabi-v7a`, and a 32-bit test APK starts.
Until then the Android emulator (x86_64 images with ARM translation) or any
Android phone that runs 32-bit apps stands in.

### 3.2 OpenGL ES 3.x through EGL, one context, two window surfaces

Not Vulkan. The compositor and the voxel renderer are written against
Citro3D's small immediate API; OpenGL ES 3.0 maps onto it almost one to one
(textures, framebuffer objects, vertex buffers, one program per pass), on a
mature Adreno driver, in a fraction of the code Vulkan would take. The two
screens are two `EGLSurface`s on one shared context, drawn from the engine
thread. Vulkan stays an option for the voxel world later; nothing in this
plan depends on it.

### 3.3 A thin Java shell, the engine in C on its own thread

`AgbMain` owns the loop, as on the 3DS, so the engine runs on a thread of
its own and never on Android's main thread. The Java side is deliberately
small: one `Activity` with a `SurfaceView` for the top screen; the bottom
screen's `SurfaceView` lives in a `Presentation` where that works and in a
second, non-focusable `Activity` launched on the second display where it
does not; both hand their `Surface` to the engine through JNI
(`ANativeWindow_fromSurface`). Controller events come from the activity's
`dispatchKeyEvent` / `dispatchGenericMotionEvent`, touch from the bottom
view, both queued to the engine thread. Lifecycle (`onPause`, `onResume`,
display added/removed) is forwarded the way the APT hooks are today.

A `NativeActivity` would save the Java, but it owns one window only and has
no clean way to host a second display's surface; the shell above does.

### 3.4 Game-side guards: `PLATFORM_3DS` becomes `PLATFORM_PORT` where it means the port

The game tree carries 325 `PLATFORM_3DS` guards. Nearly all of them are the
port's *design* (the wide view, the bottom screen, touch on the game's own
screens, staged pictures, the HD battle HUD) rather than 3DS hardware, and
the Thor wants every one of them. Phase 0 splits the macro:

- `PLATFORM_PORT`: defined by both builds; the design.
- `PLATFORM_3DS`: only what is truly the console (the 3D slider, the New
  3DS, the HOME Menu, UDS).
- `PLATFORM_ANDROID`: the Thor build.

The split is mechanical (rename, then review the few true hardware guards)
and is verified on the 3DS side by the recipe report, which shows every byte
of the game's own objects that stopped matching: the rename must change
none.

Patch files are generated from the maintainer's workspace
(`tools/public_export.py`, not in this repository); the rename is made there
and exported, as any other change to the game tree.

### 3.5 The data pipeline stays; the Thor build is a payload variant

The Android executable contains no game content, like the 3DSX: the same
INCBIN stubs, map descriptors, bundled scripts, songs and `.gamedata`
region, the same `emerald3ds.pak` with the same relative paths. Pointers
inside the bundles are link-time addresses of *this* executable, so the
Thor build needs its own recipe and data ABI, exactly as the Spanish,
French and Italian builds do today. The builder already understands payload
variants chosen by the ROM's SHA-1 (`builder/emerald3ds_builder/build.py`,
`Payload.for_rom`); the plan extends the choice by **target** (3DS, Thor)
and keeps the language matrix (5.4).

### 3.6 Resolution: logical 400x240, drawn at the screen's resolution

The game logic keeps `DISPLAY_WIDTH` 400: changing it touches the recipe,
the ABI, the field view and dozens of layout hooks, for a 7 % wider
picture. The top screen shows the 400x240 frame at 4.5x (1800x1080) with
60-pixel bars at each side. What the compositor draws itself at the screen's
resolution on the 3DS (the HD battle interface, the voxel world, the
tilt-shift blur, the text box) is drawn at 1920x1080 on the Thor, where it
is genuinely HD; the GBA layers and sprites are scaled from their pixel
grid with nearest filtering.

The bottom canvas stays 320x240 and is shown letterboxed at 3.875x
(1240x930, 75-pixel bars above and below). Touch coordinates are mapped
back through the same transform. A later phase may give the canvas the
spare rows (3.6 of the canvas's aspect against the panel's), which is a
layout change in `3ds_bottom_ui.c`, not a renderer change.

Widening the logical view to 16:9 is listed as a later option (6, phase 7),
not a requirement.

### 3.7 What is dropped

Stereoscopic 3D and the 3D slider (`CTR_VOXEL_STEREO=0`, slider fixed at
0), the New 3DS core logic, the HOME Menu forwarder and the Luma argument,
the DSP firmware dependency, the GPU memory arenas (`3ds_gpumem.c`,
`voxel_arena.c`'s VRAM part), `boot.txt`'s RomFS marker. Local wireless is
compiled out first and comes back on Wi-Fi later (4.9).

## 4. The Android backend, module by module

Proposed location: `android_port/`, a sibling of `3ds_port/` with the same
shape (`src/`, `include/` for Android-only headers, `app/` for the Gradle
project, `scripts/` for what differs in the staging). Reused units are
referenced in place (`../3ds_port/src/3ds_pak.c` and so on) rather than
copied, until both backends are stable enough to move the shared files into
a common directory and drop the `3ds_` prefix from their names.

### 4.1 Process, thread, lifecycle (`main_3ds.c`, `3ds_platform.c`)

- `JNI_OnLoad` and a handful of `native` methods: `start(topSurface,
  bottomSurface, filesDir, engineDir)`, `setBottomSurface`, `pause`,
  `resume`, `stop`, `keyEvent`, `motionEvent`, `touchEvent`,
  `displayChanged`.
- The engine thread runs `CtrPlatform_Init` then `CtrGame_Init`
  (`AgbMain`), as `main()` does now. Exit, reset and soft reset keep their
  hooks.
- `CtrPlatform_Milliseconds`, `Ticks`, `TickMs`: `clock_gettime(CLOCK_MONOTONIC)`;
  `SYSCLOCK_ARM11` becomes a tick rate constant of the shim (1 GHz
  nanoseconds), used by `3ds_prof.c`, `3ds_scenario.c`, `3ds_log.c` through
  the same functions.
- `CtrLock` over a `pthread_mutex_t` (the header fixes its size at 4 bytes:
  change it to an opaque struct, or keep a handle table), `CtrPlatform_StartThread`
  over `pthread_create` with the core argument ignored, `CtrPlatform_SleepUs`
  over `nanosleep`. `LightEvent` uses in `3ds_log.c`, `3ds_settings.c` and
  `3ds_audio.c` become a small `CtrEvent` (mutex + condition variable) in
  the same header, so those files compile for both targets.
- Lifecycle: `onPause` plays the APT suspend hook (the save is already on
  disk after every sector write; the clock store is told), `onResume` the
  wake hook (`CtrDayNight_Resume`, input cleared, pacing restarted). A
  display removed while running pauses the bottom canvas; added, it is
  re-attached. The engine never blocks the main thread: surface changes are
  posted to it and picked up at the frame boundary.
- Frame pacing: `CtrPlatform_EndFrame` keeps `FrameShown`, fast-forward
  and the real-time pacing logic verbatim. `PACE_HZ` (the LCD's 59.8261 Hz)
  becomes the top display's reported refresh rate at 60 Hz mode; on a 120 Hz
  panel the app asks for 60 with `ANativeWindow_setFrameRate` and presents
  every vsync it gets.
- `CtrPlatform_ShowDataError` and `CtrPlatform_Fatal`: a text screen drawn
  by the GL backend (a tiny bitmap font) instead of libctru's console, with
  the same messages, and the error also sent to logcat and the log file.
- Memory: no heap override. The 32-bit process has about 3 GiB; the game's
  heaps, the asset cache budget and the voxel caches are generous but
  unchanged.

### 4.2 Files and data (`3ds_fs.c`, `3ds_data.c`, `3ds_log.c`, `3ds_settings.c`)

- `sdmc:/3ds/emerald3ds/` becomes the app's external files directory,
  `/sdcard/Android/data/<package>/files/emerald3ds/`, same file names:
  `emerald3ds.pak`, `emerald3ds.sav`, `emerald3ds.clock(.bak)`,
  `settings.txt`, `debug.txt`, `port.log`, `scenario.txt`, `devdata/`. The
  player copies the pack there over USB (MTP) or through a file picker in
  the app (Storage Access Framework) that copies it into place. Saves are
  the same 128 KiB flash image as on the 3DS, so a save moves between the
  two consoles by copying it.
- `romfs:/` (engine files: the asset index, the `.rel` tables, shaders, the
  port's art, `engine/abi.bin`, `engine/build_id.bin`) ships inside the APK
  as assets and is extracted on first start into the app's internal files
  directory, keyed by the build id so an update replaces it. `CtrFs_OpenAsset`
  opens from there with `fopen`; the game units that write a `romfs:/` or
  `sdmc:/` literal (listed in 2.2) go through `CtrFs_OpenAsset`,
  `CtrFs_OpenData` or a path macro instead.
- The asset index is rebased when it is loaded: `gen_asset_table.py` records
  the link-time address of the same reference symbol the bundles use
  (`AgbMain`), and `LoadAssetMap` adds the load delta to every `addr`,
  `ptr` and `base` before the first lookup. On the 3DS the delta is zero;
  on Android it is the library's base. `3ds_map_loader.c` compares against
  linker symbols and needs nothing.
- `3ds_data.c`'s three backends keep their meaning: loose files under
  `devdata/` with the marker, embedded data (the APK's assets carry
  `data.embedded` and the game data in a development build), the pack.
- `3ds_log.c`: the writer thread stays (SD latency is not the issue, but
  the file is still best written off the render thread); `svcOutputDebugString`
  becomes `__android_log_write`; the overlay console goes.

### 4.3 The compositor (`3ds_video.c` to `android_video.c`)

The contract is `3ds_video.h`: `CtrVideo_Init`, `Bind`, `Present`,
`HoldTop`, the OAM anchors and weather marks, the modes and routes, the
voxel upload admission (`CtrVideo_TryVoxelUpload`), `InBattle`, the
bottom-screen uses (`CtrVideo_BottomInUse`). The game side calls only
these. Everything below the contract is rewritten against OpenGL ES 3.0,
keeping the structure of the PICA version where its ideas are platform
independent:

| In `3ds_video.c` | On the Thor |
|---|---|
| Citro2D as the drawing layer: about 390 call sites (`C2D_DrawImageAt`, `DrawRectSolid`, tints, `C2D_Flush` at every blend-state change, screen targets) | a small quad batcher of the port's own: one vertex buffer, one textured program, one solid program, the same tint and blend parameters; `C2D_Flush` becomes "end the batch" |
| Changed-tile decode into a 1024x1024 RGBA5551 atlas in linear memory, Morton order, uploaded with `C3D_SyncTextureCopy` | the same atlas in linear order, `GL_RGB5_A1` (or RGBA8), `glTexSubImage2D` of the changed rows from a staging buffer (PBO on 3.0) |
| Per-BG render-to-texture layers at tilemap size, dirty 8x8 cells redrawn through the compositor's own program (`FastCells`) | the same layer FBOs and the same dirty-cell program; GLES 3.0 has everything it needs |
| GBA colour 0 as one-bit alpha with an alpha test, destination-alpha coverage counting for the OBJ window, extra TexEnv stages for the day/night tint | `discard` in the fragment shader, the same coverage trick with `glColorMask` and blend functions, the tint as uniforms |
| BG/OBJ layers drawn as quads per cell with texture combiners (TexEnv) for palette, blending and windows | one program per layer kind; the combiner setup becomes uniforms and a small fragment shader; GBA alpha blending, brightness and windows as before |
| A 512x256 RGBA8 logical surface with depth, drawn as one quad onto the rotated top-screen target | an FBO of the same size for the GBA layers, then a nearest-filtered blit into the 1920x1080 window surface at 4.5x; passes that are HD on the 3DS (battle HUD, voxel, blur) draw into the window surface directly; no rotated targets, no `v = 1 - y` |
| Stereo: two eyes, depth planes (three 512x256 targets by priority group), battle sprite slots, per-eye voxel cameras | removed; the mono route only (`CTR_ROUTE_MONO`, `VOXEL`, `BATTLE_WORLD`, `TRANSITION`, `BLANK`, `BOTTOM`). About 560 lines of depth planes and a large part of the 2,150-line battle section go with it |
| Raw register writes to drop the texture cache and flush the framebuffer on target switches, `GX_MemoryFill` clears, private Citro3D context fields | nothing: the driver owns these |
| Staged / centred screens and their margins (`DrawStageBg`, `ctr_gba_*.h`) | the same geometry; the margin art and fades become ordinary textured quads |
| The game drawn into the bottom screen's 240x240 area (PokéNav, PC, bag, Pokédex) | the same composition into the bottom surface, through the canvas blit (4.7) |
| GX queue admission and early GPU start (`--wrap=GX_*`) | gone; a byte budget per frame for uploads keeps the voxel streaming bounded |
| `C3D_FrameBegin/End`, `gfxSwapBuffers` | `eglSwapBuffers` on the top surface once per shown frame; the bottom surface only when its canvas changed, with swap interval 0 so it never waits for its own vsync |

For sizing, the unit's sections (lines): GX wrappers and flushes 330,
OAM/VRAM tracking 420, palettes and atlas 370, blending 150, text and
staged backgrounds 840, battle HUD and text box 1,100, centred screens 310,
layer textures 720, intro leaves and affine 550, sprites 460, windows 580,
battle scene with stereo slots 2,150, bottom screen 300, voxel
post-processing 490, battle over voxel 360, transitions 400, depth planes
560, present 600. The stereo, GX and register sections are not ported, the
rest is translated.

The port is done in the order the game reaches the screens: plain 2D layers
and sprites first (title, menus), then the field view (widened view,
weather lattices, fog, OAM anchors), then staged and centred screens, then
the battle (1.4x zoom, HD boxes and text box), then the bottom-screen uses,
then the voxel battle route once 4.8 exists. The software renderer of phase
1 stays available as a build option (`RENDERER=soft`) for diagnosing
compositor differences: the two must agree on every 2D screen.

### 4.4 Audio (`3ds_audio.c`)

- AAudio (or Oboe over it), one stream, stereo 16-bit, performance mode low
  latency, sharing mode shared. The stream's rate is whatever the device
  offers (48000 Hz typically); the mixer's 42060 Hz output goes through a
  small linear resampler in the queue, where NDSP's rate control used to
  be. The drift trim (`CorrectDrift`) becomes the resampler's ratio nudge:
  same quantities, same limits.
- The queue is a ring of mixer frames read by the stream's data callback.
  The worker that mixes on the DSP's clock (`CtrAudio_SetClock`, real-time
  pacing) is kicked from the callback when fewer than the target depth are
  queued: a direct translation. Without the worker the mix stays on the
  game thread, once per shown frame, as now.
- The mixer's fixed point (1 << 23 to full scale) is converted as today;
  the dump option writes to the files directory.
- No DSP firmware: audio is always available. `CtrAudio_Available` stays
  true unless the stream fails, in which case the game runs silent and says
  so in the log, as the 3DS does without the firmware.

### 4.5 Input (`3ds_input.c`)

- `CtrInputSample` is filled from the queued Android events on the engine
  thread: `KEYCODE_BUTTON_A/B/X/Y`, `DPAD_*`, `BUTTON_L1/R1` (L/R),
  `BUTTON_L2/R2` (ZL/ZR: fast-forward steps), `BUTTON_START/SELECT`,
  `AXIS_X/Y` of the left stick scaled to the Circle Pad's range (so
  `CircleDirection` and its hysteresis apply unchanged), `AXIS_HAT_X/Y` as
  D-pad. The right stick and the back buttons are free for later use (the
  voxel camera's angle and zoom are natural candidates).
- Which physical button carries which label on the Thor is confirmed on
  the device; the mapping is a table in `settings.txt` with a sensible
  default, and the OPTIONS page gets a remap screen later (phase 5).
- Touch: the bottom view's `MotionEvent`s, mapped from the view's pixels
  through the letterbox transform to the 320x240 canvas; `touchActive`,
  `touchX`, `touchY` as today. Touches on the top screen are ignored.
- The exit chord (START+SELECT for a second) is kept; Android's back
  button opens the same confirmation.

### 4.6 Settings, clock, scenarios

`3ds_settings.c` and `3ds_clock_store.c` are reused: file paths through
`CtrFs_OpenData`, the save thread through the shim. The scenario recorder
and the profiler (`3ds_scenario.c`, `3ds_prof.c`) are reused too; the
profiler's identity block lists the Android version, the SoC and the ABI
instead of the kernel and firmware versions.

### 4.7 The bottom screen (`3ds_bottom_screen.c`)

The game side (`3ds_bottom_ui.c`) draws into a 320x240 RGB565 canvas laid
out like the 3DS framebuffer: column-major, each column bottom to top. The
native side becomes:

- a 320x240 `GL_RGB565` texture, updated with `glTexSubImage2D` for the
  columns `CtrBottom_Blit` and `BlitRect` name (the canvas is uploaded as
  is, and a two-line fragment shader reads it transposed, so the game side
  keeps its layout and its dirty-rectangle logic);
- a quad into the bottom surface, letterboxed (3.6), nearest filtered;
- `CtrBottom_BlitDim` as a uniform, not a CPU pass;
- `eglSwapBuffers` on the bottom surface only when something was blitted.

The 3DS reasons for a CPU-drawn canvas (no GPU time and no VRAM to spare on
an Old 3DS) do not hold on the Thor; the canvas is kept because it is what
the 7,900 lines of UI draw into, and because its redraw-on-change model is
right for an OLED. Drawing the UI at a higher resolution is a later
project of its own.

### 4.8 The voxel overworld (`voxel/ctr_voxel.c`)

The contract is `ctr_voxel.h`, called from the game-side voxel modules and
the compositor. What is replaced:

| In `ctr_voxel.c` | On the Thor |
|---|---|
| `voxel.v.pica` (vertex shader) | a GLSL ES 3.00 vertex shader with the same attributes and uniforms; the fragment stage, fixed-function on the PICA (texture, fog, shade), becomes a short fragment shader |
| VRAM arenas and linear staging VBOs (`voxel_arena.c`, `3ds_gpumem.c`) | plain `glBufferData` / `glBufferSubData` ring buffers; the arena bookkeeping on the CPU side stays where the game-side modules use it |
| Chunk uploads capped by the GX queue | capped by a byte budget per frame; the worker thread keeps building meshes and streaming building textures on the CPU, and the render thread uploads, exactly as the thread split is today (GL contexts are thread-bound) |
| Stereo pass per eye | removed |
| Render target 400x240 | the window surface at 1920x1080: the world is drawn once, at full resolution, with the 2D text and weather composed over it as on the 3DS |
| Texel order: atlases, sprite atlas, building pages and the tree texture in the PICA's Morton order, two of them pre-swizzled at generation time | `CtrVideo_Texel` returns the linear index on Android (one function in `3ds_video_decode.c`, selected by the target); `gen_voxel_buildings.py` and `gen_voxel_trees.py` take a `--texel-order linear` flag, so the game-side modules do not change and the Thor pack carries linear pages. The host tests of the atlas and buildings run in both orders |
| Fixed-function combiners for terrain light, haze, screen fade, building glow and lamps (`TerrainTexEnv` and friends); a private-context poke to unbind texture units | one fragment shader with the same stages as uniforms; texture units bound explicitly |
| VFP inline assembly for vertex packing (`__arm__`, with a portable fallback) | builds as is on ARMv7 |
| Tilt-shift blur | an FBO pair and a separable blur at full or half resolution |

The lighting, shadows, day/night grading and billboards are translated
pass by pass; the game-side modules (`voxel_world.c`, `voxel_mesh_builder.c`,
`voxel_atlas.c`, `voxel_lighting.c`, the buildings, trees, signs and
relief) do not change. The host tests of those modules keep running.

### 4.9 Local wireless (`3ds_local.c`)

Phase 1 to 5 build with `LOCAL_WIRELESS=0`: a `LocalLink_*` stub that
reports no radio, so the link counters say nothing is nearby and the HOME
blocking logic is inert. `3ds_rfu_adapter.c` (game side) stays as it is.

Later, the radio can be Wi-Fi on the same network: UDP discovery with the
same beacon (build id, counter, protocol version) and the same reliable
stream per pair (the go-back-N layer in `3ds_local.c` is written over
unreliable frames already). Thor to Thor only; a 3DS cannot be reached, and
the build id would refuse it anyway.

## 5. Build and data pipeline

### 5.1 Toolchain

- Android NDK r27 (clang), `armeabi-v7a`, `-mfpu=neon`, Thumb-2, API 29
  as the minimum (Android 10; the Thor ships 13), target 34.
- GNU make for the engine, as today: `android_port/Makefile` includes the
  existing `full.mk` for the game units with the compiler, assembler and
  flags taken from variables (`CC`, `AS`, `ARCH`, `GAME_ARCH`), and lists
  the native units of the Android backend. It produces `libemerald3ds.so`
  and the `romfs/` staging. Gradle packages the prebuilt library from
  `jniLibs/` and the engine files from `assets/`. Keeping make keeps the
  data staging and the host tests exactly where they are; Gradle never
  needs to understand the game build.
- The game compile pipeline (`cpp | preproc | cc`) is unchanged. The
  decomp's tools (`preproc`, `mapjson`, `wav2agb`, `scaninc`) are host
  tools and are built as now.
- Binutils in the Python scripts: `scripts/ctr_bundle.py`,
  `scripts/gen_gamedata_bundle.py`, `scripts/gen_build_id.py`,
  `tools/gen_recipe.py` and `tools/port_common/gen_asset_table.py` call
  `arm-none-eabi-nm`, `readelf` and `objcopy`, and the asset table reads
  GNU ld's map file. Phase 0 makes the tool prefix a parameter (the NDK's
  `llvm-nm`, `llvm-readelf`, `llvm-objcopy` print the same formats for the
  options used) and derives the asset addresses from `nm` on the linked
  ELF rather than from the map file, so both linkers are served by one
  code path.

### 5.2 The link

The executable is a shared library loaded by the Android linker, so it is
position independent. The port's link tricks each have an lld equivalent:

| 3DS (`emerald3ds.ld.in`, Makefile) | Android (lld) |
|---|---|
| Full custom linker script with `.gamedata` `NOLOAD`, `__ctr_game_data/bss` markers, blob regions | a partial script with `INSERT AFTER` that adds `.gamedata (NOLOAD)` and the marker symbols to lld's default shared-library layout, never a full replacement (the dynamic sections must stay lld's) |
| Two links, the image with contents and the shipped one `NOLOAD`, `--emit-relocs`, `.rel.gamedata` stripped with objcopy | the same two links; lld supports `--emit-relocs`; the dynamic relocations the loader applies inside the `NOLOAD` region are overwritten by the bundle loader, which re-applies its own list, so the result is the same |
| Bundles position independent through a reference symbol's delta | unchanged; the delta is the library's load base |
| `--wrap` for the profiler, the sound engine, the map setters, GPU memory | `--wrap` is supported; the GPU memory wraps go away |
| `--gc-sections`, `-ffunction-sections` | the same |

This scheme is the first spike of phase 0, on the host, before any backend
code exists: link the game units into a `.so` and run the three bundle
generators and `staging.py abi` against it.

### 5.3 Compiler differences

The game tree compiles with GCC today. clang differences to expect, all
known from the SDL ports of the decompilation that build with the NDK:
warnings that the game's flags silence by name (`-Wno-error=...` lists need
their clang names), `-fno-strict-aliasing` and `-fsigned-char` kept, a few
GNU extensions in `global.h` macros, and the `.s` data files, which the
integrated assembler takes after the existing `.4byte` conversion plus
`.syntax unified`. Struct layouts follow the same AAPCS on both, so the
recipe's learnt record maps (`R` operations) hold; the recipe report says
exactly how many bytes of the game's objects moved, and the literal budget
check (`--max-literal`) keeps the Thor recipe honest.

### 5.4 Variants, recipe, builder, website

- `voxel/buildings.bin` is generated by the builder in the order the
  target wants (the pack is per target anyway); `voxel/trees.rgba5551`,
  an engine file, is generated in the APK's assets in linear order.
- The release gains `payload/android/` (the APK and its recipe) with
  `payload/android/es/`, `fr/`, `it/` for the languages, each built from a
  localized tree exactly as `build_localized_variant` in
  `tools/build_release.py` does for the 3DS.
- The builder chooses a variant by ROM **and target**: a `--target 3ds|thor`
  option and a toggle in the window; `Payload.for_rom` gains the target.
  The web manifest's `variants` entries get an optional `platform` field
  (schema version unchanged; the website without it keeps building the
  3DS). Install writes the pack where the player wants it: the SD card for
  the 3DS, a chosen folder (or the phone over MTP) for the Thor.
- The APK is a release asset (`Emerald3DS-Thor.apk`), signed with the
  project's key, listed in `SHA256SUMS.txt`; `release_audit.py` learns the
  `.apk` suffix (a ZIP it scans like the others) and the allowlist gets its
  size entry. Nothing in the APK is game content.
- CI: a job on the NDK image that bootstraps, builds the `.so`, runs the
  host tests and the bundle generators, and audits the APK, next to the
  existing `build-3ds` job.

### 5.5 Development loop

Same three paths as `docs/DEVELOPMENT.md`: a development APK that embeds
its data (`EMBED_DATA=1`, the game data in the APK's assets), loose data
in `devdata/` pushed with `tools/dev_assets.py --adb` (an `adb push` next to
the existing FTP path), and the release path (engine-only APK plus the
pack). Logs come from the file and from logcat. The scenario recorder gives
reproducible runs on the emulator from day one.

## 6. Phases

Each phase ends with something runnable or verifiable; none needs a Thor
until phase 5. Sizes are rough, for one developer who knows the tree.

### Phase 0: Groundwork (host only, 2 weeks)

1. Macro split `PLATFORM_3DS` / `PLATFORM_PORT` / `PLATFORM_ANDROID` (3.4);
   3DS build unchanged, recipe report shows zero drift.
2. Tool prefix and map-file independence in the Python scripts (5.1);
   3DS output byte-identical.
3. Link spike (5.2): game units compiled with the NDK for armeabi-v7a into
   a `.so` with no backend (stub native units), the three bundles and the
   asset index generated from it, `staging.py abi` and `gen_recipe.py`
   run against a ROM on the maintainer's machine; the index rebasing of
   4.2 added and exercised. This is the go/no-go for decisions 3.1 and 5.2.
3b. Texel order behind one function and one generator flag (4.8), with the
   atlas and building host tests run in both orders; the 3DS output stays
   byte-identical.
4. The thread, lock, event and tick shim (`CtrLock`, `CtrEvent`,
   `CtrPlatform_StartThread`, ticks) as the first Android native unit,
   and the reused native units (`3ds_data.c`, `3ds_log.c`,
   `3ds_settings.c`, `3ds_prof.c`, `3ds_scenario.c`) compiling against it.
5. CI job for the above.

### Phase 1: Boot on a screen (3 weeks)

1. The Gradle shell: one activity, top `SurfaceView`, JNI entry, engine
   thread, lifecycle forwarding; the bottom display found and its surface
   handed over (Presentation or second activity, chosen at runtime by
   trying the first).
2. `android_platform.c`, `android_fs.c` (engine files from assets, data
   directory), `android_input.c`, `android_audio.c` (4.1, 4.2, 4.4, 4.5).
3. Video, first form: `RENDERER_FAST_DRAW`'s `DrawFrame` into a 400x240
   RGB565 buffer, uploaded as a texture and drawn into the top surface;
   the bottom canvas blitted the same way (4.7).
4. Milestone: title screen, a new game, Littleroot, a battle, a save and a
   reload, with sound, on the emulator or any 32-bit-capable Android
   device; the bottom screen's map, party and bag respond to touch. The
   scenario recorder captures the run for later comparisons.

### Phase 2: The compositor (6 to 8 weeks)

The GPU compositor on OpenGL ES in the order of 4.3, each step checked
against the software renderer with scenario playback and screenshots of
the same frames: 2D screens; field view with weather; staged and centred
screens; battle with the HD interface; the bottom-screen game screens.
Milestone: `RENDERER=soft` and the GL compositor agree on every 2D screen
of a recorded hour of play, and the GL path renders HD where the 3DS does.

### Phase 3: The bottom screen and the Thor's two displays (1 to 2 weeks)

Letterboxing, touch transform, dimming, swap-only-when-changed; both
display routes exercised on the emulator with a simulated secondary
display; a fallback layout for single-screen Android devices (both
canvases stacked on one surface), which costs little and widens who can
test. Focus behaviour (controller events while the bottom window exists)
settled on the second-activity route with `FLAG_NOT_FOCUSABLE`.

### Phase 4: The voxel overworld (4 to 5 weeks)

`ctr_voxel.c` on OpenGL ES (4.8), at full resolution, with lighting,
shadows, day/night and the blur; the voxel battle route in the compositor.
Milestone: the existing voxel host tests pass unchanged, and the recorded
scenarios play through Littleroot, Oldale, Petalburg, Slateport and Route
110 in voxel mode at 60 fps on the emulator's GPU.

### Phase 5: Thor fit and finish (2 weeks, overlaps the first hardware runs)

Refresh rate and pacing on the 120 Hz panel, OLED-friendly black bars and
idle dimming of the bottom screen, the input remap page, the data-error
screens with the Android paths, display hotplug and lid behaviour, battery
behaviour under fast-forward, the `adb`-based development loop documented.

### Phase 6: Distribution (2 weeks)

Payload variant, builder target option, web manifest field, APK signing and
release assets, `release_audit.py` rules, install and update guide
(`docs/THOR_INSTALLATION.md`), CHANGELOG.

### Phase 7: Later, if wanted

Local wireless over Wi-Fi (4.9); a 16:9 logical view; the bottom canvas
using the panel's extra rows or drawn at 2x; Vulkan for the voxel world;
an arm64 build if the pointer-width work is ever done.

## 7. Risks and open questions

| Risk | Where it bites | What the plan does |
|---|---|---|
| The Thor's firmware refuses 32-bit apps | everything | Confirmed first on hardware (8). No cheap fallback: a 64-bit build is the pointer-width project described in 3.1 |
| lld and the `NOLOAD` two-link scheme in a shared library | 5.2 | Phase 0's spike, before any backend code |
| clang-compiled game objects differ from GCC's in ways the recipe cannot learn | 5.3 | The recipe tool reports the literal pool; a larger budget or a per-target record map is the fix, not a redesign |
| `Presentation` refused on the unit's Android version | 1, 3.3 | Both routes implemented; chosen at runtime |
| Touch on the bottom screen steals controller focus | 4.5 | Non-focusable bottom window; events that still arrive are forwarded |
| The compositor is the port's largest body of PICA-specific code | 4.3 | Phase 2 is sized for it; the software renderer carries the game until it lands and remains the reference afterwards |
| Thumb-2 interworking against a hidden Thumb-bit assumption in the game tree | 2.2 | None found in the patches; the upstream tree is checked in phase 0's spike (a grep for `& ~1` and `\| 1` on function pointers); `-marm` for the game units is the escape hatch |
| ASLR: the asset index compares raw pointers against link-time addresses | 4.2 | Rebased at load through the bundles' reference symbol; checked by the phase 0 spike, which loads the index against a library at a non-zero base |
| 32-bit processes run on the 8 Gen 2's small cores only | performance | Still several times a New 3DS; measured in phase 5 with the frame profiler |
| OLED burn-in from a static bottom UI | 4.7 | Idle dimming (phase 5), black letterbox |
| Two activities, one engine: surfaces arriving late or twice | 4.1 | Surfaces are posted to the engine thread and bound at the frame boundary; the engine draws nothing to a surface it does not hold |

Open questions, to settle in phase 0 and 1:

- Package name and app title.
- Whether the bottom display is exposed to apps only through AYN's
  launcher settings (a "second screen" mode) and what the default is: it
  decides the first-run instructions.
- Whether `ANativeWindow_setFrameRate` at 60 is honoured on the top panel,
  or the app must pace itself at 120 Hz with every other vsync.
- The native rate of the audio stream and its latency in low-latency mode,
  which sets the ring depth.

## 8. What waits for hardware

To do as soon as a Thor is available, in this order:

1. `adb shell getprop ro.product.cpu.abilist`, `ro.build.version.release`,
   and a 32-bit hello-world APK (3.1).
2. The display list (`DisplayManager.getDisplays()` with sizes, rotation,
   refresh rates, touch capability) and which of `Presentation` / second
   activity works on that firmware.
3. The controller's key codes and axes, and the stick's range.
4. The audio stream's native rate, buffer size and whether the low-latency
   path is granted.
5. Phase 1's milestone build, then the recorded scenarios of phases 2 and
   4 at 60 fps, with the profiler's log.

Everything else in this plan is designed so that it can be built and
checked on the emulator and on the host first.

## Sources

- AYN Thor hardware: [DROIX review](https://droix.net/blogs/ayn-thor-handheld-review/),
  [Retro Dodo review](https://retrododo.com/ayn-thor-review/),
  [TechEBlog specifications](https://www.techeblog.com/ayn-thor-dual-screen-android-handheld-specs/),
  [Handheld Base launch coverage](https://handheldbase.com/news/ayn-thor-dual-screen-gaming-handheld-at-just-249/),
  [DROIX product page](https://droix.eu/product/ayn-thor/).
- Dual-screen Android on the Thor:
  [ScummVM second-screen touchpad PR](https://github.com/scummvm/scummvm/pull/7500) (Android 14, 1080x1920 and 1080x1240, `Presentation`),
  [OpenNOW companion deck PR](https://github.com/OpenCloudGaming/OpenNOW/pull/1075) (Android 13, second activity on the display, `FLAG_NOT_FOCUSABLE`),
  [Minish Cap dual-screen port](https://github.com/samyost1/tmc-android) (a GBA game with a `Presentation` bottom panel),
  [Daggerfall Unity focus issue](https://github.com/shashwahpple/daggerfall-unity-android/issues/1),
  [Yokoi emulator display order issue](https://github.com/RetroValou/Yokoi_Game_-_Watch_emulator_3ds/issues/41),
  [pokesunrecomp dual-screen issue](https://github.com/melikootje/pokesunrecomp/issues/16),
  [Retro Game Corps dual-screen handheld guide](https://retrogamecorps.com/2025/10/27/dual-screen-android-handheld-guide/).
- 32-bit on the Snapdragon 8 Gen 2:
  [XDA](https://www.xda-developers.com/qualcomm-snapdragon-8-gen-2/),
  [Android Police](https://www.androidpolice.com/qualcomm-snapdragon-8-gen-2-early-launch/),
  [Android Authority](https://androidauthority.com/snapdragon-8-gen-2-vs-dimensity-9200-3236000),
  [HotHardware](https://www.Hothardware.com/reviews/qualcomm-snapdragon-8-gen-2-benchmark-preview-first-look).
- A 32-bit SDL build of the decompilation on Android, the precedent for
  decision 3.1 and the voxel renderer's origin:
  [pokeemerald-multiplatform](https://github.com/gradenGnostic/pokeemerald-multiplatform).
