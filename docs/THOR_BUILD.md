# Building the AYN Thor port on Windows

Everything below runs natively on Windows (no WSL, no devkitPro). The plan is
[THOR_PORT_PLAN.md](THOR_PORT_PLAN.md); the host shell is described in
[android_port/README.md](../android_port/README.md).

## Tools

| Tool | Used for | Where it came from |
|---|---|---|
| Android NDK r27 (Windows) | `clang` for the game's C and the native units, `make.exe` | the SDK's `ndk/27.x` |
| MSYS2 UCRT64 | host compiler and `libpng` for the decomp's tools, `sh`, `sed`, `xargs` | `pacman -S mingw-w64-ucrt-x86_64-libpng` |
| `mingw-w64-ucrt-x86_64-arm-none-eabi-binutils` | GNU `as` for the game's data assembly (clang's assembler rejects its macros), `nm`, `objcopy`, `readelf` for the bundle generators | `pacman` |
| Python 3.11+ with Pillow | the generators and `bootstrap.py` | |
| JDK 17 to 21 and the Android SDK | Gradle (the APK) | Android Studio's `jbr` works; JDK 25 does not |

`git config core.autocrlf` is irrelevant: `tools/bootstrap.py` never lets git convert the
pokeemerald checkout (the patches have LF endings).

## Build

```
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:<ndk>/prebuilt/windows-x86_64/bin:$PATH"
python tools/bootstrap.py                       # pins pokeemerald, applies the patches, places the ports
cd build/upstream && make tools -j16 PYTHON=python && make generated -j16 PYTHON=python
cd 3ds_port
make -f ../android_port/Makefile -j24 PYTHON=python apk-files pak
```

`apk-files` links `libemerald3ds.so` (armeabi-v7a), generates the asset table, the script, song
and game data bundles and the ABI from it, and stages the library and the engine files in
`build/android-out/`. `pak` builds the data pack (`build/emerald3ds.pak`). Copy the engine files to
`android_port/app/src/main/assets/romfs/` and build the APK:

```
cd android_port
./gradlew assembleDebug -Pemerald3dsLib=<abs path>/build/android-out/lib
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb push build/upstream/3ds_port/build/emerald3ds.pak /sdcard/Android/data/com.emerald3ds.thor/files/emerald3ds/
```

Options (command line of the make): `VOXEL=1` (the voxel world and battles; default), `EMBED_DATA=1`
(the game data in the APK instead of the pack), `MID_CHUNK=n` (songs per recursive make:
Windows' command line is 32 KiB).

## Repeatable development loop

After the initial bootstrap, use the tracked helper instead of keeping
machine-specific scripts in the ignored `build/` directory. In PowerShell:

```powershell
$env:ANDROID_HOME = 'Z:\android-sdk'  # use your SDK location
$env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr'
python tools/thor_dev.py deploy --serial DEVICE_SERIAL --jobs 16
python tools/thor_dev.py run --serial DEVICE_SERIAL --continue-save
```

`deploy` refreshes the `3ds_port`, `android_port`, builder and tools overlays,
builds the library and pack, stages engine-only assets under
`build/android-assets/romfs/`, builds the APK, installs it and pushes the
matching pack. It does not fetch, reset or patch the upstream game tree.
Use `sync` for just the overlay update, or `build` to omit installation.
`--sdk`, `--ndk`, `--msys`, `--tree` and `--jobs` override the local paths.
`GRADLE_USER_HOME` can point to an existing Gradle cache.

Keep Android and 3DS staging trees separate: their generated voxel textures
use different orders. `--tree build/upstream-thor` selects a dedicated Thor
tree; pass that directory to bootstrap when creating it. Do not reuse an
Android `build/` object directory for a 3DS link.

The top surface uses a 400x240 logical view in a 512x256 texture. It draws
at `surface_scale` times that size (1–4; default 4). The actual top display
mode selects one picture per tick at 60 Hz or two at 120 Hz. Mode selection
comes from Android's display callbacks, so a map-loading stall cannot be
mistaken for a different panel. `refresh_hz=60` requests 60 Hz through the
window and native surface; it is still a hint, and an actual 120 Hz mode
continues to receive two pictures per tick. Other panel rates request 60 Hz
and use a software tick limit if that request is refused. Fast-forward
repeats the finished image for the second refresh instead of rendering a
second voxel world.

Physical screenshot IDs differ from Android's logical display IDs:

```text
adb shell dumpsys SurfaceFlinger --display-id
python tools/thor_dev.py shot --serial DEVICE_SERIAL --display PHYSICAL_ID --out build/top.png
adb shell input -d LOGICAL_BOTTOM_ID tap 620 540
```

`--measure` compiles the optional `measure.txt` warp/battle hooks into a
development build. Each line is `FRAME warp GROUP MAP X Y` or `FRAME battle`.
The frame is counted from entering the field. The battle hook supplies a
Torchic only if the party is empty. Keep `measure.txt` out of normal play;
release builds reject `--measure` and compile the hooks out. Back up saves
before running scenarios that change game state.

## Signing and release payload

`python tools/thor_dev.py build --release` creates a signed release APK and
audits the uncompressed APK contents. Set `THOR_KEYSTORE`, `THOR_KEY_ALIAS`,
`THOR_STORE_PASSWORD` and `THOR_KEY_PASSWORD` in the environment. The helper
also accepts a maintainer-local `%USERPROFILE%/.android/emerald3ds-thor-signing.json`
containing those four fields; environment values take precedence. Keep the
keystore and configuration private and backed up. Neither belongs in Git.
JKS and keystore files are ignored. The debug APK uses Android's development
key, so a production APK cannot update it in place under a different key.

Produce the recipe from the same staged engine tree:

```text
python tools/build_thor_payload.py --apk build/Emerald3DS-Thor.apk --rom emerald.gba --gba-elf pokeemerald.elf --version 0.4.0 --nm arm-none-eabi-nm --apksigner SDK/build-tools/VERSION/apksigner.bat
```

The original GBA ELF supplies the ROM symbol addresses and is required for
complete voxel generator metadata. It must match the ROM. The tool checks
the APK's signature and the staged ABI/build ID, scans for ROM content,
then applies the recipe's literal budgets and byte-for-byte reconstruction
checks. `--language es|fr|it` writes a corresponding subdirectory after
building that localized Android tree. Pass the resulting `dist/thor-payload`
to `tools/build_release.py --thor-payload` to include the APKs and recipes
in the desktop and web payloads and APKs in the release checksum list.

The APK contains engine files only. The ROM, intermediate ELF, generated
pack, local signing configuration and literal reports stay on the builder's
machine. [THOR_INSTALLATION.md](THOR_INSTALLATION.md) covers end-user import
and updates.

## Checks

```text
python -m unittest discover -s builder/tests -v
python android_port/tests/gles_draw_test.py --cc gcc
python android_port/tests/save_storage_test.py --cc gcc
python android_port/tests/interpolation_test.py --cc gcc
```

Run the existing voxel host `verify-*` targets against the staged tree.
For an Android `buildings.bin`, set `CTR_TEXEL_ORDER=rows` when running the
animated-building test; the harness compiles its decoder in the same order.
`video_decode_test.c` can be compiled both with and without
`-DPLATFORM_ANDROID` to test the two texel layouts. The GLES host harness
checks the real draw functions with a recording GL, including indexed
models, byte/short indices, vertex offsets, and return to 2D drawing.

`gradlew.bat testDebugUnitTest -Pemerald3dsLib=ABSOLUTE_LIBRARY_FOLDER`
checks successful pack import, CRC/truncation rejection, and preservation
of the previous pack and save. CI has separate 3DS and Windows Thor builds;
the Thor job publishes a development APK, not a production-signed release.
The latest local results and hardware coverage are recorded in
[THOR_VERIFICATION.md](THOR_VERIFICATION.md).

## Debugging

- `adb logcat -s Emerald3DS`. Create an empty `debug.txt` in the app's files directory for the
  game's own log (`port.log`).
- A crash handler logs frame-pointer offsets into the library: symbolize them with
  `llvm-symbolizer --obj=build/upstream/3ds_port/emerald3ds.elf <offset>`.
- `dump.txt` in the files directory makes the GL layer write every live texture and render target
  (and the top screen) as raw RGBA; `android_port/scripts/dump_view.py` turns them into PNGs.
  `trace.txt` logs one frame's draw batches.
