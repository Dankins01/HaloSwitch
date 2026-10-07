#---------------------------------------------------------------------------------
# The Switch host (port/switch/README.md): builds opence.nro with devkitA64.
#
# It needs two outputs of the Android guest build (run first, from the
# repository root: python configure.py --release, then
# ninja build/android/halo_guest.elf build/android/host/host_import_table.c):
#
#   $(GUEST)/halo_guest.elf                 the game image (into the romfs)
#   $(GUEST)/host/host_import_table.c       the host's import table
#
#   make GUEST=../../build/android
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment: export DEVKITPRO=<path to>/devkitpro")
endif

include $(DEVKITPRO)/libnx/switch_rules

TARGET      := opence
BUILD       := build
GUEST       ?= ../../build/android
# (in the log's first line, to match a crash to its build)
SWITCH_BUILD ?= local
ROMFS       := $(BUILD)/romfs

APP_TITLE   := OpenCE
APP_AUTHOR  := OpenCE Switch port
# (the Homebrew Menu shows it: which build is on the SD card)
APP_VERSION := 0.1.0-$(SWITCH_BUILD)
# 256x256 JPEG: replace art/icon.jpg with an image of your own to change it
ICON        := art/icon.jpg

PKG_CONFIG  := $(PORTLIBS)/bin/aarch64-none-elf-pkg-config

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
INCLUDE  := -Ihost -Ishim -I../android/include -I../linux/src -I$(LIBNX)/include -I$(PORTLIBS)/include
CFLAGS   := -g -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -ffunction-sections $(ARCH) -D__SWITCH__ -DSWITCH_BUILD='"$(SWITCH_BUILD)"' \
            $(INCLUDE) $(shell $(PKG_CONFIG) --cflags sdl2)
ASFLAGS  := -g $(ARCH)
LDFLAGS  := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,-Map,$(BUILD)/$(TARGET).map
LIBS     := $(shell $(PKG_CONFIG) --libs sdl2) -lEGL -lglapi -ldrm_nouveau -lnx -lm

HOST_C   := $(wildcard host/*.c)
HOST_S   := $(wildcard host/*.s)
OBJECTS  := $(patsubst host/%.c,$(BUILD)/%.o,$(HOST_C)) \
            $(patsubst host/%.s,$(BUILD)/%_s.o,$(HOST_S)) \
            $(BUILD)/xiso.o $(BUILD)/host_import_table.o

.PHONY: all clean

all: $(TARGET).nro

$(BUILD):
	@mkdir -p $(BUILD) $(ROMFS)

$(BUILD)/%.o: host/%.c host/switch_host.h Makefile | $(BUILD)
	@echo $(notdir $<)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%_s.o: host/%.s | $(BUILD)
	@echo $(notdir $<)
	@$(CC) $(ASFLAGS) -x assembler-with-cpp -c $< -o $@

# a copy, so that its "platform.h" is shim/platform.h (see there)
$(BUILD)/xiso.c: ../linux/src/xiso.c | $(BUILD)
	@cp $< $@

$(BUILD)/xiso.o: $(BUILD)/xiso.c shim/platform.h
	@echo xiso.c
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/host_import_table.o: $(GUEST)/host/host_import_table.c | $(BUILD)
	@echo host_import_table.c
	@$(CC) $(CFLAGS) -w -c $< -o $@

$(ROMFS)/halo_guest.elf: $(GUEST)/halo_guest.elf | $(BUILD)
	@cp $< $@

$(ROMFS)/brokers.txt: ../assets/network/brokers.txt | $(BUILD)
	@cp $< $@

$(BUILD)/$(TARGET).elf: $(OBJECTS)
	@echo linking $(notdir $@)
	@$(CC) $(LDFLAGS) $(OBJECTS) $(LIBS) -o $@

$(BUILD)/$(TARGET).nacp: Makefile | $(BUILD)
	@$(DEVKITPRO)/tools/bin/nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(TARGET).nro: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).nacp $(ROMFS)/halo_guest.elf $(ROMFS)/brokers.txt $(ICON)
	@$(DEVKITPRO)/tools/bin/elf2nro $< $@ --icon=$(ICON) --nacp=$(BUILD)/$(TARGET).nacp --romfsdir=$(ROMFS)
	@echo built $@

clean:
	@rm -rf $(BUILD) $(TARGET).nro
