# Emerald 3DS on AYN Thor

The Thor port runs natively on Android, with the game on the top display
and the touch interface on the bottom display. It needs a Thor that can
run `armeabi-v7a` applications. Local wireless and stereoscopic 3D are not
available on this target.

## Install

1. Download **Emerald3DS-Thor.apk** from the release and install it on the Thor.
   For an update, install over the existing app so your progress stays in place.
2. Open **Emerald 3DS Thor**. If game data is missing or belongs to an older
   version, setup opens automatically.
3. Tap **Choose Emerald ROM** and select your own clean **English Emerald
   (BPEE)** `.gba` file, or a ZIP containing it, from Android's file picker.
4. Wait while the Thor prepares the game data. The APK includes the builder,
   runtime, scenery generators and its matching reconstruction recipe. This
   works offline and does not require a computer, another app or a downloaded
   `.pak` file. The game starts after verification and installation finish.

The ROM is processed locally. Its temporary copy is deleted after the build.
The app remembers the document permission when the provider allows it, so a
later APK update can rebuild automatically from the same selected ROM. If the
file moved or access expired, choose it again.

An interrupted, cancelled or failed build keeps the previous pack, save and
settings. The new pack is fully verified before replacing the old one.
The first build includes scenery generation and can take several minutes;
keep the setup screen open until it finishes.
Updates reuse scenery from the previous pack when every generated file still
matches the new release, so those updates avoid repeating the scenery build.

**Back → Choose Emerald ROM** starts setup manually. **Import data pack
(advanced)** still accepts a prebuilt pack, checks it against the installed
engine and leaves saves/settings alone. Desktop and web builders remain
optional alternatives, not requirements for Android installation.

## Controls and displays

Face buttons, D-pad and shoulders keep the 3DS layout. The left stick
moves, the right stick controls the voxel camera, and ZL/ZR step the
fast-forward speed. Android **Back → Controller buttons** opens the remap
page. Select a game action and press the physical button to assign it.
Mappings survive app updates; **Reset defaults** restores the initial
layout. Analogue triggers and D-pad hats share their key-event mappings.
Stick axes retain their movement and camera roles.

The bottom display dims to 20% after 30 seconds without a touch. The first
touch wakes it and is consumed; the next touch operates the interface.
Letterbox bars remain black. If the secondary display disappears, the
bottom canvas moves below the game on the main display. Sleep and
backgrounding suspend the engine and audio; held inputs are cleared.

**OPTION → ENHANCEMENTS → second page → INTERPOLATION** is enabled by
default. At an actual 120 Hz top-display mode, each game tick gets two pictures:
the previous completed voxel state, then the midpoint toward the current one.
The voxel camera and moving sprites interpolate without advancing gameplay;
presentation trails the simulation by one tick (about 17 ms). Map changes,
battle transitions and large jumps reset the history. Classic 2D scenes and
menus repeat their picture; they do not gain generated motion frames.
The game still advances at about 60 ticks per second with **SPEED = 1X**.
The FPS overlay reports display presents / game ticks (about **120/60**).
Fast-forward repeats the finished picture on that second refresh to save
the extra world draw. Panels with other rates are asked for 60 Hz.

## Saves, settings and updates

The app keeps files in its external app directory:

```text
/sdcard/Android/data/com.emerald3ds.thor/files/emerald3ds/
```

`emerald3ds.sav` is the game save, `settings.txt` contains game settings,
and `emerald3ds.pak` is the generated data. Controller mappings live in
Android's private app preferences. **Uninstalling the app removes its app
data**, so back up your save first. Installing an update signed with the
same key preserves it. A release signed with another key cannot update a
development APK in place; back up before uninstalling that build.

Save in game before importing an update's pack and closing the app.
Install the new APK over the existing app. If the data changed, the app
prepares a new pack from your selected ROM or asks you to choose it again.
Only the pack is replaced; saves, settings and controller mappings stay.

Useful optional lines in `settings.txt`:

```ini
voxel=1
voxel_battle=1
surface_scale=4
refresh_hz=120
interpolation=1
```

`surface_scale` accepts 1–4 and defaults to 4. `refresh_hz=60` is a system
hint: if Android keeps the panel at 120 Hz, the engine continues to present
twice per tick. Choose a 60 Hz display mode in the device's display controls
when you want to ensure 60 Hz output. Keep `gpu_prof=0` for normal play;
profiling forces GPU synchronization and changes performance.

If Android cannot show the second display, bring the app to the foreground
again and check the device's secondary-display settings. Developer builds
can force the alternate routes for testing; see [THOR_BUILD.md](THOR_BUILD.md).
