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

Options (command line of the make): `VOXEL=1` (the voxel world, not ported yet), `EMBED_DATA=1`
(the game data in the APK instead of the pack), `MID_CHUNK=n` (songs per recursive make:
Windows' command line is 32 KiB).

## Debugging

- `adb logcat -s Emerald3DS`. Create an empty `debug.txt` in the app's files directory for the
  game's own log (`port.log`).
- A crash handler logs frame-pointer offsets into the library: symbolize them with
  `llvm-symbolizer --obj=build/upstream/3ds_port/emerald3ds.elf <offset>`.
- `dump.txt` in the files directory makes the GL layer write every live texture and render target
  (and the top screen) as raw RGBA; `android_port/scripts/dump_view.py` turns them into PNGs.
  `trace.txt` logs one frame's draw batches.
