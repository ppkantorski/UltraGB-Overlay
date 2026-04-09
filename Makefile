##################################################################################
# Makefile for UltraGB Overlay
# Author: ppkantorski
# Description:
#   This Makefile is used to build the UltraGB Overlay homebrew application for
#   Nintendo Switch.
#
#   For more details and usage instructions, please refer to the project's
#   documentation and README.md.
#
#   GitHub Repository: https://github.com/ppkantorski/UltraGB-Overlay
#
# Licensed under GPLv2
# Copyright (c) 2026 ppkantorski
##################################################################################


.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
APP_TITLE   := UltraGB
APP_AUTHOR  := ppkantorski
APP_VERSION := 1.0.0
TARGET      := ultragb
BUILD       := build
SOURCES     := source
INCLUDES    := source include
NO_ICON     := 1

# libultrahand — keep the same relative location as Tetris Overlay
include ${TOPDIR}/lib/libultrahand/ultrahand.mk

#---------------------------------------------------------------------------------
# Walnut-GB compile-time options (passed to both C and C++ TUs)
#---------------------------------------------------------------------------------
WALNUT_DEFINES := -DENABLE_LCD=1 \
                  -DWALNUT_FULL_GBC_SUPPORT=1

#---------------------------------------------------------------------------------
ARCH := -march=armv8-a+simd+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -Wall -Os \
          -ffunction-sections -fdata-sections -flto=6 \
          -fuse-linker-plugin -fomit-frame-pointer \
          -fno-strict-aliasing -frename-registers \
          -falign-functions=16 \
          -fvisibility=hidden \
          -fmerge-all-constants \
          $(ARCH) $(DEFINES) $(WALNUT_DEFINES)

CFLAGS += $(INCLUDE) -D__SWITCH__ -DAPP_VERSION="\"$(APP_VERSION)\"" -D_FORTIFY_SOURCE=2

# Overlay UI config path
UI_OVERRIDE_PATH := /config/ultragb/
CFLAGS += -DUI_OVERRIDE_PATH="\"$(UI_OVERRIDE_PATH)\""

# Enable Widget
USING_WIDGET_DIRECTIVE := 1  # or true
CFLAGS += -DUSING_WIDGET_DIRECTIVE=$(USING_WIDGET_DIRECTIVE)

# Back button override (we handle it ourselves in the emulator screen)
NO_BACK_KEY_DIRECTIVE := 1
CFLAGS += -DNO_BACK_KEY_DIRECTIVE=$(NO_BACK_KEY_DIRECTIVE)

# Targeted speed optimizations
CFLAGS += -DTESLA_TARGETED_SPEED

# Targeted size optimizations
CFLAGS += -DULTRA_TARGETED_SIZE

# FPS Indicator (for debugging)
#CFLAGS += -DGB_FPS

CXXFLAGS := $(CFLAGS) -std=c++26 \
            -Wno-dangling-else \
            -ffast-math \
            -fno-unwind-tables \
            -fno-asynchronous-unwind-tables \
            -fno-exceptions \
            -fno-rtti

ASFLAGS  := $(ARCH)
LDFLAGS  += -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS := -lcurl -lz -lmbedtls -lmbedx509 -lmbedcrypto -lnx

LDFLAGS  += -Wl,--gc-sections -Wl,--as-needed

# LTO with parallel LTRANS jobs — run make -j6
LDFLAGS  += -flto=6

#---------------------------------------------------------------------------------
LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)

export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES  := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
    jsons := $(wildcard *.json)
    ifneq (,$(findstring $(TARGET).json,$(jsons)))
        export APP_JSON := $(TOPDIR)/$(TARGET).json
    else
        ifneq (,$(findstring config.json,$(jsons)))
            export APP_JSON := $(TOPDIR)/config.json
        endif
    endif
else
    export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.jpg)
    ifneq (,$(findstring $(TARGET).jpg,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).jpg
    else
        ifneq (,$(findstring icon.jpg,$(icons)))
            export APP_ICON := $(TOPDIR)/icon.jpg
        endif
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
    export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
    export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
    export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
    export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(TARGET).ovl $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).ovl

$(OUTPUT).ovl: $(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ $(NROFLAGS)
	@echo "built ... $(notdir $(OUTPUT).ovl)"
	@printf 'ULTR' >> $@
	@printf "Ultrahand signature has been added.\n"

$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES_BIN)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
