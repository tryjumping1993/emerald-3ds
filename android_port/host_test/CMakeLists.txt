# libemerald3ds.so for the Android host without the game: android_host.c plus a self-test engine that
# draws a test pattern on both screens. Gradle builds this by default; the real engine replaces it by
# passing -Pemerald3dsLib=<dir> (see ../README.md). Standalone:
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-29
cmake_minimum_required(VERSION 3.22)
project(emerald3ds_selftest C)

add_library(emerald3ds SHARED ../src/android_host.c selftest.c)
target_include_directories(emerald3ds PRIVATE ../include)
target_compile_options(emerald3ds PRIVATE -Wall -Wextra -O2)
target_link_libraries(emerald3ds PRIVATE android log EGL GLESv3)
