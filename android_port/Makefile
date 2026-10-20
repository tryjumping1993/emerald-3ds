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

ARCH := --target=armv7a-linux-androideabi$(API) -mfpu=neon -mfloat-abi=softfp -marm -fPIC -funwind-tables -fno-omit-frame-pointer
# The game's own data assembly: GNU as needs the symbols the macros test.
export CTR_AS := $(MSYS_BIN)/arm-none-eabi-as.exe
export CTR_OBJCOPY := $(MSYS_BIN)/arm-none-eabi-objcopy.exe
AS := $(ANDROID_ROOT)/scripts/as.sh
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
# 1: measure.txt in the files directory warps the player and starts a wild battle (3ds_game_full.c, MeasureAid).
MEASURE_AID ?= 0
ifeq ($(MEASURE_AID),1)
GAMEFLAGS += -DCTR_MEASURE_AID
endif
CFLAGS := $(ARCH) -O2 -g -std=gnu99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
GAMECFLAGS := $(filter-out -Werror,$(CFLAGS)) -fsigned-char -fno-strict-aliasing
GAMECFLAGS += -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration
LDFLAGS :=
MID_CHUNK := 60
-include local.mk
# The voxel world's vertex shader is GLSL inside c3d_gles.c: no picasso, no shader binary.
PORT_NO_PICASSO := 1
# Texture data the generators write is read as CtrVideo_Texel orders it: rows inside a tile here.
export CTR_TEXEL_ORDER := rows
include full.mk

# The software renderer of phase 1 is compiled out of the game tree unless asked for.
build/root/src/platform/gba_fast_draw.o: FULLCFLAGS += -DRENDERER_FAST_DRAW
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

# ── The native units: the port's reusable backend over the shim (include/3ds.h), and the
# Android ones. Built without the game's preprocessing: they never see the game headers.
NATIVE_CPPFLAGS := -DPORTABLE -DPLATFORM_3DS -DPLATFORM_ANDROID -DCTR_VOXEL_ENABLED=$(VOXEL)
NATIVE_CPPFLAGS += -DCTR_VOXEL_LIGHTING=$(VOXEL_LIGHTING) -DCTR_SHOW_FPS=$(SHOW_FPS) -DCTR_DATA_BACKEND=$(DATA_BACKEND)
NATIVE_CPPFLAGS += -Iinclude -Isrc -I$(ANDROID_ROOT)/include
NATIVE_CFLAGS := $(filter-out -Werror,$(CFLAGS)) -fvisibility=hidden
REUSED_SRCS := 3ds_log 3ds_input 3ds_input_decode 3ds_fs 3ds_data 3ds_pak 3ds_settings \
	3ds_prof 3ds_scenario 3ds_daynight 3ds_clock_store 3ds_encounters 3ds_field_view 3ds_video_decode 3ds_video
ANDROID_SRCS := $(patsubst $(ANDROID_ROOT)/src/%.c,%,$(wildcard $(ANDROID_ROOT)/src/*.c))
NATIVE_OBJS := $(patsubst %,build/native/%.o,$(REUSED_SRCS)) $(patsubst %,build/android/%.o,$(ANDROID_SRCS))

# The voxel world is native too (it owns the Citro3D objects); it asks for VRAM textures it fills itself.
ifeq ($(VOXEL),1)
NATIVE_OBJS += build/native/ctr_voxel.o
build/native/ctr_voxel.o: src/voxel/ctr_voxel.c Makefile $(ANDROID_ROOT)/Makefile
	@mkdir -p $(dir $@)
	"$(CC)" $(NATIVE_CPPFLAGS) -Isrc/voxel -DCTR_TEXINITVRAM_CPU $(NATIVE_CFLAGS) -MMD -MP -c $< -o $@

romfs/shaders/voxel.shbin:
	@mkdir -p $(dir $@)
	printf 'DVLB' > $@
endif

build/native/%.o: src/%.c Makefile $(ANDROID_ROOT)/Makefile
	@mkdir -p $(dir $@)
	"$(CC)" $(NATIVE_CPPFLAGS) $(NATIVE_CFLAGS) -MMD -MP -c $< -o $@

build/android/%.o: $(ANDROID_ROOT)/src/%.c $(ANDROID_ROOT)/Makefile
	@mkdir -p $(dir $@)
	"$(CC)" $(NATIVE_CPPFLAGS) $(NATIVE_CFLAGS) -MMD -MP -c $< -o $@

.PHONY: native-objs
native-objs: $(NATIVE_OBJS)
-include $(NATIVE_OBJS:.o=.d)

# ── The shared library ────────────────────────────────────────────────────────
# Wrapped C library calls: the 3DS's file system paths (android_paths.c) and exit (android_main.c).
LDFLAGS += $(foreach f,fopen rename remove mkdir stat exit IdentifyFlash,-Wl,--wrap=$(f))
LDFLAGS += -Wl,--undefined=__ctr_script_keep
SO_LIBS := -llog -landroid -laaudio -lEGL -lGLESv3 -lm
SO_OBJECTS := $(GAME_UNITS) $(NATIVE_OBJS) build/linkid.o build/buildinfo.o

# A fresh id for every link, in both links (their layouts must match): the log names the
# executable by it.
build/linkid.c: $(GAME_UNITS) $(NATIVE_OBJS)
	@mkdir -p build
	@printf 'const char gCtrLinkId[] = "E3ID-%s";\n' "$$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')" > $@
build/linkid.o: build/linkid.c
	"$(CC)" $(ARCH) -c $< -o $@

# What the boot log says this executable is (3ds_prof.c). PORT_COMMIT names the port's commit.
PORT_COMMIT ?= unknown
BUILDINFO_DEFINES := $(sort $(filter -D%,$(NATIVE_CPPFLAGS) $(GAMEFLAGS)))
define BUILDINFO_C
/* Generated by android_port/Makefile. */
const char gCtrBuildVersion[] = "$(APP_VERSION)";
const char gCtrBuildCommit[] = "$(PORT_COMMIT)";
const char gCtrBuildBranch[] = "thor";
const unsigned gCtrBuildDirty = 0;
const char gCtrBuildDiff[] = "none";
const char gCtrBuildLibs[] = "ndk=$(notdir $(NDK))";
const char gCtrBuildDefines[] = "$(BUILDINFO_DEFINES)";
const char gCtrBuildCompiler[] = __VERSION__;
endef
# Rewritten only when something it says changes, so a relink does not follow every run.
build/buildinfo.c: FORCE
	@mkdir -p build
	$(file >$@.new,$(BUILDINFO_C))
	@cmp -s $@.new $@ || cp $@.new $@
	@rm -f $@.new
build/buildinfo.o: build/buildinfo.c
	"$(CC)" $(ARCH) -c $< -o $@

build/emerald3ds_image.ld: $(ANDROID_ROOT)/emerald3ds_android.ld.in
	@mkdir -p build
	sed 's/@GAMEDATA_TYPE@//' $< > $@
build/emerald3ds_release.ld: $(ANDROID_ROOT)/emerald3ds_android.ld.in
	@mkdir -p build
	sed 's/@GAMEDATA_TYPE@/(NOLOAD)/' $< > $@

# Both links see the same objects: the image keeps .gamedata's contents so the bundle
# generators can read them; the shipped library reserves it.
SO_LINK = "$(CC)" $(ARCH) -shared -fuse-ld=lld -Wl,--gc-sections -Wl,--emit-relocs -Wl,-z,max-page-size=4096 \
	-Wl,--version-script,$(ANDROID_ROOT)/exports.version $(LDFLAGS) -Wl,-T,build/emerald3ds_$(1).ld -Wl,-Map,build/$(2).map @build/link.rsp $(SO_LIBS) -o $(3)

# The ELF names are the 3DS build's, so that full.mk's rules for the asset table, the
# script, song and game data bundles (which read them) apply as they are; the shipped
# library is the same file under the name Android loads.
build/gamedata_image.elf: $(SO_OBJECTS) build/emerald3ds_image.ld $(ANDROID_ROOT)/exports.version
	$(file >build/link.rsp,$(SO_OBJECTS))
	$(call SO_LINK,image,gamedata_image,$@)

$(TARGET).elf: $(SO_OBJECTS) build/emerald3ds_release.ld $(ANDROID_ROOT)/exports.version
	$(file >build/link.rsp,$(SO_OBJECTS))
	$(call SO_LINK,release,$(TARGET),$@)

lib$(TARGET).so: $(TARGET).elf
	cp $< $@

.PHONY: link-probe
link-probe: $(SO_OBJECTS)
	$(file >build/link.rsp,$(SO_OBJECTS))
	"$(CC)" $(ARCH) -shared -fuse-ld=lld -Wl,--no-undefined -Wl,--gc-sections $(LDFLAGS) @build/link.rsp $(SO_LIBS) -o build/probe.so

# ── The engine's files (what the 3DSX carries in its RomFS) ─────────────────────
ROMFS_DATA_OUTS := $(ROMFS_ASSET_OUTS) $(ROMFS_MAP_OUT) $(ROMFS_SCRIPT_OUTS) $(ROMFS_SOUND_OUTS) 	$(ROMFS_SHADER_OUTS) $(ROMFS_GAMEDATA_OUTS)

# The ABI digests every game-data file, so a data pack is accepted only by the library it
# was generated for.
romfs/engine/abi.bin: $(ROMFS_DATA_OUTS) $(SCRIPTS)/staging.py
	"$(PYTHON)" $(SCRIPTS)/staging.py abi --romfs romfs

# Local wireless play links only identical builds: a SHA-256 of the library without its
# debug information and of the data's ABI. (.rel.gamedata is the bundle loader's.)
build/$(TARGET)_id.elf: $(TARGET).elf
	$(MSYS_BIN)/arm-none-eabi-objcopy.exe --strip-debug --remove-section=.rel.gamedata $< $@
romfs/engine/build_id.bin: build/$(TARGET)_id.elf romfs/engine/abi.bin scripts/gen_build_id.py
	"$(PYTHON)" scripts/gen_build_id.py build/$(TARGET)_id.elf romfs/engine/abi.bin $@

# 1: the data is embedded in the APK's assets (development builds); the release path ships
# engine files only and the data pack is the player's.
EMBED_DATA ?= 0
ifeq ($(EMBED_DATA),1)
EMBED_MARKER := romfs/data.embedded
romfs/data.embedded:
	@mkdir -p romfs
	printf 'embedded\n' > $@
else
EMBED_MARKER :=
$(shell rm -f romfs/data.embedded)
endif

# What goes into the APK: the library and the engine's own files of romfs/ (assets/romfs/ in
# the package), as the release carries them. The game data is the data pack, which the player
# copies to the app's files directory (adb push for development): `make -f ... pak`.
ANDROID_OUT ?= ../../android-out
.PHONY: apk-files pak
apk-files: lib$(TARGET).so romfs/boot.txt $(ROMFS_DATA_OUTS) romfs/engine/abi.bin romfs/engine/build_id.bin $(EMBED_MARKER)
	rm -rf $(ANDROID_OUT) build/romfs_release
	mkdir -p $(ANDROID_OUT)/lib/armeabi-v7a
	cp lib$(TARGET).so $(ANDROID_OUT)/lib/armeabi-v7a/
	"$(PYTHON)" $(SCRIPTS)/staging.py engine --romfs romfs --out build/romfs_release
	cp -r build/romfs_release $(ANDROID_OUT)/romfs

pak: romfs/engine/abi.bin
	"$(PYTHON)" $(SCRIPTS)/staging.py pak --romfs romfs --out build/emerald3ds.pak --game-config $(ROOT)/include/constants/global.h

# full.mk generates the port's own battle art inside its voxel-only block; the Thor build
# without the voxel world needs it all the same.
ifneq ($(VOXEL),1)
romfs/bottom/battle.bin: scripts/gen_battle_art.py
	@mkdir -p $(@D)
	"$(PYTHON)" scripts/gen_battle_art.py --output $@

romfs/battle/hud.bin: scripts/gen_battle_hud.py
	@mkdir -p $(@D)
	"$(PYTHON)" scripts/gen_battle_hud.py --output $@
endif
