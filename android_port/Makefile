.SUFFIXES:
# The Thor build: an Android shared library for armeabi-v7a (docs/THOR_PORT_PLAN.md).
#
# Run it from the bootstrapped tree's 3ds_port directory, so that every path of
# full.mk (src/, compat/, scripts/, build/, romfs/) means what it does for the 3DS:
#
#     make -C build/upstream-thor/3ds_port -f ../android_port/Makefile game-objs
#
# Toolchain (all native on Windows, see docs/THOR_BUILD.md): the NDK's clang for C,
# GNU as (MSYS2's arm-none-eabi-binutils) for the game's data assembly, whose macros
# clang's assembler does not understand, and the NDK's own make.
SHELL := bash
.SHELLFLAGS := -o pipefail -c
.DEFAULT_GOAL := game-objs

NDK ?= Z:/android-sdk/ndk/27.3.13750724
NDK_BIN := $(NDK)/toolchains/llvm/prebuilt/windows-x86_64/bin
MSYS_BIN ?= C:/msys64/ucrt64/bin
API ?= 29
CC := $(NDK_BIN)/clang.exe
AR := $(NDK_BIN)/llvm-ar.exe
NM := $(NDK_BIN)/llvm-nm.exe
export CTR_CLANG := $(CC)
ROOT := ..
ANDROID_ROOT := $(abspath ../android_port)

ARCH := --target=armv7a-linux-androideabi$(API) -mfpu=neon -mfloat-abi=softfp -marm
# The game's own data assembly: GNU as needs the symbols the macros test.
AS := $(MSYS_BIN)/arm-none-eabi-as.exe
ASFLAGS := -march=armv7-a --defsym MODERN=1 --defsym PORTABLE=1 --defsym UBFIX=1 -I$(ROOT)
CPPTOOL := $(ANDROID_ROOT)/scripts/cpp.sh

PYTHON ?= python
HOSTCC ?= gcc
SCRIPTS := ../tools/port_common
EXESUFFIX := .exe
TARGET := emerald3ds
APP_VERSION := 0.4.0
VOXEL ?= 1
VOXEL_LIGHTING ?= 1
SHOW_FPS ?= 1
DATA_BACKEND ?= 0
GAMEFLAGS := -DPORTABLE -DPLATFORM_3DS -DPLATFORM_ANDROID -DMODERN=1 -DUBFIX -D__INTELLISENSE__
GAMEFLAGS += -iquote include -iquote ../include -iquote ../include/constants -iquote ..
GAMEFLAGS += -DCTR_VOXEL_ENABLED=$(VOXEL) -DCTR_VOXEL_LIGHTING=$(VOXEL_LIGHTING) -DCTR_SHOW_FPS=$(SHOW_FPS)
CFLAGS := $(ARCH) -O2 -g -std=gnu99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
GAMECFLAGS := $(filter-out -Werror,$(CFLAGS)) -fsigned-char -fno-strict-aliasing
GAMECFLAGS += -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration
LDFLAGS :=
MID_CHUNK := 60
-include local.mk
include full.mk

build/root/src/music_player.o: FULLCFLAGS += -include $(ANDROID_ROOT)/compat/android_fixups.h

.PHONY: FORCE
$(FULL_OBJECTS): build/config
build/config: FORCE
	@mkdir -p build
	@printf '%s\n' '$(CFLAGS) $(GAMEFLAGS) $(GAMECFLAGS) $(ASFLAGS)' > build/config.new
	@cmp -s build/config.new $@ || cp build/config.new $@
	@rm -f build/config.new

# The game's translation units and data, with no backend: the compile half of
# the phase 0 link spike.
GAME_UNITS := $(FULL_C_OBJS) $(FULL_DATA_OBJS) $(filter-out build/ctr_voxel.o,$(BACKEND_OBJS))
.PHONY: game-objs
game-objs: $(GAME_UNITS)
-include $(GAME_UNITS:.o=.d)
