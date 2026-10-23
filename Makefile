.SUFFIXES:
export DEVKITPRO ?= /opt/devkitpro
export DEVKITPPC	:= $(DEVKITPRO)/devkitPPC
export PATH := $(DEVKITPRO)/tools/bin:$(DEVKITPPC)/bin:$(PATH)
include $(DEVKITPPC)/wii_rules

JIT       ?= 1
BLOCKS    ?= 1
GBA_JIT   ?= 0
POLL_SKIP ?= 0
JIT_STATS ?= 0
BROWSER   ?= 0
FULL_RAM_DUMP ?= 0
G2_SCRIPTED_INPUT ?= 0
TEST_FAST ?= 0

CC      := $(DEVKITPPC)/bin/powerpc-eabi-g++
MACHDEP := -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -fsigned-char -ffast-math -funroll-loops -fauto-inc-dec -finline-functions
INCLUDES += -I$(DEVKITPRO)/portlibs/wii/include -I$(DEVKITPRO)/portlibs/ppc/include -I$(DEVKITPRO)/libogc/include
JIT_DEFS := -DNOODS_JIT=$(JIT) -DNOODS_JIT_BLOCKS=$(BLOCKS) -DNOODS_GBA_JIT=$(GBA_JIT) -DNOODS_JIT_POLL_SKIP=$(POLL_SKIP) -DNOODS_JIT_STATS=$(JIT_STATS)
BROWSER_DEFS := -DNOODS_FORCE_BROWSER=$(BROWSER)
TEST_DEFS :=
ifeq ($(FULL_RAM_DUMP),1)
  TEST_DEFS += -DNOODS_FULL_RAM_DUMP=1
endif
ifeq ($(G2_SCRIPTED_INPUT),1)
  TEST_DEFS += -DNOODS_G2_SCRIPTED_INPUT=1
endif
ifeq ($(TEST_FAST),1)
  TEST_DEFS += -DNOODS_TEST_FAST=1
endif
CXXFLAGS += -O3 -fno-rtti -MMD -MP \
-DENDIAN_BIG \
$(JIT_DEFS) \
$(BROWSER_DEFS) \
$(TEST_DEFS) \
$(INCLUDES) $(MACHDEP)

CONFIG_TAG := jit$(JIT)_blocks$(BLOCKS)_gba$(GBA_JIT)_stats$(JIT_STATS)_browser$(BROWSER)
ifeq ($(FULL_RAM_DUMP),1)
  CONFIG_TAG := $(CONFIG_TAG)_fullram
endif
ifeq ($(G2_SCRIPTED_INPUT),1)
  CONFIG_TAG := $(CONFIG_TAG)_scriptinput
endif
BUILDDIR   := build_$(CONFIG_TAG)
TARGET     := NooDS-Wii
CONFIG_OUT := $(TARGET)_$(CONFIG_TAG)

LDFLAGS += $(CXXFLAGS) \
-Wl,-Map,$(CONFIG_OUT).elf.map \
-L$(DEVKITPRO)/portlibs/wii/lib \
-L$(DEVKITPRO)/portlibs/ppc/lib \
-L$(DEVKITPRO)/libogc/lib \
-L$(DEVKITPRO)/libogc/lib/wii \
-lwiikeyboard -lfat -lwiiuse -lbte -logc -lm

EXCLUDE    :=
SRCDIR     := NooDS-Wii
C_SOURCES  := $(foreach dir, $(SRCDIR), $(filter-out $(EXCLUDE), $(wildcard $(dir)/*.cpp)))
AS_SOURCES := $(foreach dir, $(SRCDIR), $(filter-out $(EXCLUDE), $(wildcard $(dir)/*.S)))
OBJECTS    := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(C_SOURCES)) \
              $(patsubst %.S,$(BUILDDIR)/%.o,$(AS_SOURCES))
DEPS       := $(OBJECTS:.o=.d)

ELF2DOL := elf2dol

all: $(CONFIG_OUT).dol

-include $(DEPS)

$(CONFIG_OUT).dol: $(CONFIG_OUT).elf
	$(ELF2DOL) $< $@
	cp -f $@ $(TARGET).dol
	cp -f $< $(TARGET).elf
	cp -f $(CONFIG_OUT).elf.map $(TARGET).elf.map

$(CONFIG_OUT).elf: $(OBJECTS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(CXXFLAGS)

$(BUILDDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(CXXFLAGS)

clean:
	rm -rf $(BUILDDIR) $(CONFIG_OUT).elf $(CONFIG_OUT).dol $(CONFIG_OUT).elf.map $(TARGET).elf $(TARGET).dol $(TARGET).elf.map

clean-all:
	rm -rf build_jit* $(TARGET)_jit*.elf $(TARGET)_jit*.dol $(TARGET)_jit*.elf.map $(TARGET).elf $(TARGET).dol $(TARGET).elf.map $(SRCDIR)/*.o

.PHONY: all clean clean-all test
