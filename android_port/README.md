# Android host (AYN Thor)

The Java/Kotlin shell, the JNI + EGL host (`src/android_host.c`) and a self-test engine (`host_test/`).
The engine backend links against `include/android_host.h`; the host never includes engine headers.

## Build and install

Gradle root is this directory. Needs JDK 17 to 21 (JDK 25 breaks the Kotlin plugin; Android Studio's `jbr` works),
the Android SDK with NDK 27.3.13750724 and CMake 3.31.6. Put `sdk.dir=<sdk path>` in the git-ignored `local.properties`.

    export JAVA_HOME=<jdk 17-21> ANDROID_HOME=<sdk>
    ./gradlew assembleDebug                       # signed with the standard debug keystore
    adb -s <serial> install -r app/build/outputs/apk/debug/app-debug.apk
    adb -s <serial> shell am start -n com.emerald3ds.thor/.MainActivity

The library is armeabi-v7a only (the game is 32-bit), so Android starts the app as a 32-bit process.
Without anything else the self-test library is built from `host_test/CMakeLists.txt`: a test pattern on both
screens (top 400x240 with a frame counter, moving bar, button boxes and stick positions; bottom 320x240 with a
grid, a corner marker and a dot under the touch), button/stick/touch changes in logcat.

## The real engine

Build the engine as `libemerald3ds.so` for armeabi-v7a (it provides `AndroidEngine_Main`, the host links in with it),
put it in `<dir>/armeabi-v7a/`, and package it instead of the self-test:

    ./gradlew assembleDebug -Pemerald3dsLib=<dir>

Engine files go in `app/src/main/assets/romfs/` (with an optional `BUILD_ID` text file; without one the APK's
install time is the key). They are extracted to `filesDir/romfs/` on first run and whenever the key changes.

## Two displays

`MainActivity` shows the top screen and picks a home for the bottom screen (`CompanionRouter`), in order:

1. **presentation**: a `Presentation` on the secondary display (a public, non-default display that is on).
   This is what the Thor (Android 13) takes.
2. **activity**: `CompanionActivity` launched on that display with `setLaunchDisplayId`.
3. **inline**: the bottom view stacked under the top view on the one screen (phones, or both routes failed).

Both secondary windows forward controller keys and motion to `MainActivity`, because a touch on the bottom display
moves input focus there. Debug builds accept `am start ... --es companion activity|inline` to force a fallback.
Native pause/resume follow `onStop`/`onStart` (not `onPause`/`onResume`: a touch on the other display can pause
the main Activity while it is still on screen).

## logcat (`adb logcat -s Emerald3DS`)

- `companion route=presentation|activity|inline` and `displays top=<id> ... usable=[...]`: the chosen route.
- `engine thread started`, `GL: <renderer> ...`, `top|bottom surface created WxH`, `... surface released`.
- `pause` / `resume`, `top surface dropped (EGL 0x300d)` (a surface destroyed under a frame in flight, handled).
- Self-test: `buttons 0x... -> 0x...`, `sticks ...`, `touch active=1 x= y=` (canvas coordinates), `frame N` every 300 frames.
