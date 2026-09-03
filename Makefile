#---------------------------------------------------------------------------------
# executive_nx -- The Executive wrapper for Nintendo Switch
#
#   dkp-pacman -S switch-dev
#   dkp-pacman -S switch-mesa switch-libdrm_nouveau switch-libpng switch-zlib \
#                 switch-libwebp switch-ffmpeg switch-pkg-config
#
# Ships no game code and no game assets. See tools/prepare_game.sh.
#
# icon.jpg is the port's own icon (256x256 JPEG, which is what elf2nro wants).
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Source $$DEVKITPRO/switchvars.sh or install devkitPro.)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := executive_nx
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := source

APP_TITLE   := The Executive
APP_AUTHOR  := ChanseyIsTheBest
APP_VERSION := 1.0.0
ICON        := icon.jpg

# APP_AUTHOR is the PORT's author, not the game's -- it is what the Switch
# home menu shows under the title. The game is by Riverman Media and nothing
# of theirs ships in this repository.

#---------------------------------------------------------------------------------
# -mtp=soft is not optional. Both loaded modules are built for bionic and read
# their stack canary from TPIDR_EL0+0x28. If the compiler is allowed to emit
# hardware TLS accesses for our own code they fight over the same register.
# See util.c / install_bionic_tls.
#---------------------------------------------------------------------------------
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

#---------------------------------------------------------------------------------
# Logging and instrumentation.
#
# TWO INDEPENDENT SWITCHES, because "off" turned out to mean two things.
#
#   DEBUG   what LEVEL of detail is compiled in
#   LOG     whether a log file is written AT ALL
#
#   make                 release. DEBUG and VERBOSE lines and the pthread wait
#                        instrumentation are compiled out entirely -- not the
#                        call, not the argument evaluation, not the format
#                        string in .rodata. A small executive_nx.log still gets
#                        INFO and above: both module load addresses, the EGL
#                        surface size, audio and input startup, every warning
#                        and every error.
#
#   make DEBUG=1         bring-up. Everything, including the per-file
#                        "asset miss" line -- and resolve_asset_name probes six
#                        directories per file, so five misses per hit is the
#                        STEADY STATE. Across a few thousand assets that is a
#                        large log and a lot of SD writes on the thread the
#                        frame loop waits on. Loading is visibly slower.
#
#   make LOG=0           silent. No log file is created and nothing is written.
#                        Every exec_log() call folds away; the three
#                        __android_log_* resolver targets stay real functions,
#                        because both modules import them by name, but they
#                        return on a null handle.
#
# WHAT LOG=0 COSTS, stated plainly because it is not obvious until you need it:
# a crash then leaves you an Atmosphere report and nothing else. The two module
# load addresses are the lines that turn an absolute faulting address into
# `addr - base` and a function name from the .so's own symbol table, and every
# crash in this lineage was diagnosed that way. DEBUG=0 is the quiet build;
# LOG=0 is the deaf one.
#---------------------------------------------------------------------------------
DEBUG ?= 0
LOG   ?= 1

DEFINES := -D__SWITCH__
ifeq ($(DEBUG),0)
DEFINES += -DDEBUG_LOG=0 -DEXEC_DIAG=0
else
DEFINES += -DDEBUG_LOG=1 -DEXEC_DIAG=1
endif

ifeq ($(LOG),0)
DEFINES += -DEXEC_LOG_SILENT=1
endif

#---------------------------------------------------------------------------------
# Music: decoded on the console, no conversion step.
#
# The music tracks are AAC-LC in MP4 and libnx has no decoder, so exec_decode.c
# uses switch-ffmpeg. The assets are played exactly as they came out of the
# APK.
#
# Resolve the link chain through pkg-config, NOT by hand: switch-ffmpeg's
# transitive deps (dav1d, bzip2, opus, vorbis, ogg, ...) vary with how the
# portlib was built, and a written-out list goes stale. devkitPro puts the
# cross pkg-config in different places depending on version and host, so try
# the known spellings. --static matters: the portlib archives need their own
# dependencies named explicitly.
#
# DO NOT add -lvulkan here, and be suspicious of any tool that suggests it.
# libavutil contains ffmpeg's Vulkan hwcontext, so an archive-level dependency
# scan concludes that vulkan is needed -- but the linker never pulls that
# member in, because nothing this port calls references it. Adding -lvulkan
# anyway makes the link fail with about a hundred multiple-definition errors:
# switch-mesa's libvulkan.a and libEGL.a both statically embed Mesa's util
# layer (ralloc, hash_table, glsl_types, set, softfloat, u_debug). This port
# lost a build to exactly that.
#
#   make MUSIC=0     compiles the music path out. Sound effects, which are
#                    RIFF PCM decoded in exec_audio.c, are unaffected.
#---------------------------------------------------------------------------------
MUSIC ?= 1

ifeq ($(MUSIC),0)
DEFINES     += -DEXEC_NO_MUSIC
FFMPEG_LIBS :=
else
FFMPEG_PKGS := libavformat libavcodec libswresample libavutil

PKGCONF := $(firstword $(wildcard \
             $(PORTLIBS)/bin/aarch64-none-elf-pkg-config \
             $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config \
             $(PORTLIBS)/bin/pkg-config))
ifneq ($(PKGCONF),)
FFMPEG_LIBS := $(shell $(PKGCONF) --libs --static $(FFMPEG_PKGS) 2>/dev/null)
endif

ifeq ($(strip $(FFMPEG_LIBS)),)
# Fallback for an install with no pkg-config. dav1d and bzip2 look unrelated
# to a game whose only codec is AAC, and they are: libavcodec.a and
# libavformat.a are built with every decoder and demuxer switch-ffmpeg
# enables, and the codec registry references all of them. Only the ones
# actually installed are added, so this list can be generous.
FFMPEG_MAYBE := dav1d bz2 opus vorbisenc vorbisfile vorbis ogg lzma \
                mbedtls mbedx509 mbedcrypto xml2 speex theoradec theoraenc \
                vpx x264 webp mp3lame soxr
FFMPEG_EXTRA := $(strip $(foreach l,$(FFMPEG_MAYBE),\
                  $(if $(wildcard $(PORTLIBS)/lib/lib$(l).a),-l$(l))))
FFMPEG_LIBS  := -Wl,--start-group -lavformat -lavcodec -lswresample -lavutil \
                $(FFMPEG_EXTRA) -Wl,--end-group
$(warning No cross pkg-config found; guessing switch-ffmpeg's dependencies.)
$(warning Using: $(FFMPEG_EXTRA))
$(warning `dkp-pacman -S switch-pkg-config` gives an exact answer instead.)
endif

ifeq ($(wildcard $(PORTLIBS)/lib/libavcodec.a),)
$(error switch-ffmpeg is not installed. Either `dkp-pacman -S switch-ffmpeg`, \
        or build without music: make MUSIC=0)
endif
endif

CFLAGS  := -Wall -Wextra -Wno-unused-parameter -O2 \
           -ffunction-sections -fdata-sections $(ARCH) $(DEFINES)

# On aarch64 an implicit declaration is not a style problem: the compiler
# assumes an int return and guesses the arguments, which corrupts registers
# rather than merely warning.
CFLAGS  += -Werror=implicit-function-declaration -Werror=implicit-int
CFLAGS  += -Werror=incompatible-pointer-types
CFLAGS  += -Wno-unused-but-set-variable
CFLAGS  += $(INCLUDE)

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,--gc-sections \
            -Wl,-Map,$(notdir $*.map)

# Link with $(CXX) even though this tree is pure C: switch-mesa's libEGL.a
# carries the nouveau shader compiler, which is C++.
# mesa provides GLESv2/EGL on top of drm_nouveau; the group is because those
# three reference each other in both directions. -lpng is for nx_pointer.c's
# optional cursor.png and depends on zlib, so it must come BEFORE -lz: the
# linker resolves left to right and would otherwise be finished with zlib
# before it learned libpng needed it.
ifeq ($(wildcard $(PORTLIBS)/lib/libwebp.a),)
$(error switch-libwebp is not installed, and every image asset in this game \
        is WebP: dkp-pacman -S switch-libwebp)
endif

LIBS    := $(FFMPEG_LIBS) \
           -Wl,--start-group -lGLESv2 -lEGL -lglapi -ldrm_nouveau -Wl,--end-group \
           -lwebp -lpng -lz -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_SRC)
export HFILES_BIN :=

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(PORTLIBS)/include \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

#---------------------------------------------------------------------------------
# NRO metadata.
#
# These have to be EXPORTED and they have to reach NROFLAGS, and neither was
# happening: APP_TITLE and friends were set but not exported, so the %.nacp
# rule in switch_rules ran in the sub-make without them, and ICON was assigned
# to a variable nothing reads. elf2nro was being invoked with no --icon and no
# --nacp at all, so the build produced an NRO with the default icon and no
# title, author or version -- silently, because a missing flag is not an error.
#
# switch_rules fills in defaults for any of the three that are empty, which is
# why nothing ever complained.
#---------------------------------------------------------------------------------
export APP_TITLE
export APP_AUTHOR
export APP_VERSION
export APP_ICON  := $(TOPDIR)/$(ICON)
export NROFLAGS  += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: all clean check imports

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).elf $(TARGET).nacp

#---------------------------------------------------------------------------------
# Host-side audits. These read your own copies of the two .so files, so point
# EXEC_LIB and EXEC_CXX at them. Nothing here needs a Switch.
#---------------------------------------------------------------------------------
EXEC_LIB ?= libexecutive_android.so
EXEC_CXX ?= libc++_shared.so

check:
	bash tools/check_compile.sh source
	python3 tools/check_loader.py source/main.c
	python3 tools/check_atomics.py source
	python3 tools/check_targets.py source/imports_exec.c
	python3 tools/check_links.py source
	python3 tools/verify_imports.py $(EXEC_LIB) $(EXEC_CXX) source/imports_exec.c
	python3 tools/check_jni_slots.py source/exec_jni.c $(EXEC_LIB)
	python3 tools/check_entrypoints.py $(EXEC_LIB) source/main.c

# Regenerate the resolver table from your own libraries. Re-apply nothing by
# hand: intentional mappings live in tools/gen_imports.py.
imports:
	python3 tools/gen_imports.py $(EXEC_LIB) $(EXEC_CXX) > source/imports_exec.c


else

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf : $(OFILES)

-include $(DEPENDS)

endif
