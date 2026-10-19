#!/bin/sh
# The C preprocessor stage of the game's data assembly (the role arm-none-eabi-cpp has on
# the 3DS side): clang in assembler-with-cpp mode, stdin to stdout.
exec "$CTR_CLANG" --target=armv7a-linux-androideabi29 -E -x assembler-with-cpp "$@"
