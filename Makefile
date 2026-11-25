.SUFFIXES:
export DEVKITPRO ?= /opt/devkitpro
export DEVKITPPC	:= $(DEVKITPRO)/devkitPPC
export PATH := $(DEVKITPRO)/tools/bin:$(DEVKITPPC)/bin:$(PATH)
include $(DEVKITPPC)/wii_rules

JIT       ?= 1
BLOCKS    ?= 1
GBA_JIT   ?= 0
# Optional GBA OAM scanline membership cache; CPU and fast-DMA writes invalidate it.
GBA_OBJ_MASK ?= 1
# GBA affine all-windows-allow hoist; verify rerenders each line against original.
GBA_AFFINE_WINDOW ?= 1
GBA_AFFINE_VERIFY ?= 0
# Experimental exact IWRAM ARM bursts: 0 off; 1 ALU, 2 mapped transfers, 3 inline hot/cold split.
GBA_ARM_BURST ?= 3
# Experimental dedicated ARM7/IWRAM execution island (exact timing; default OFF).
GBA_DEDICATED ?= 0
# v1p170: exact-timing GamePak-ROM execution island (see gba_rom_island.inc)
GBA_ROM_ISLAND ?= 1
GBA_ISLAND_STATS ?= 0
GBA_THUMB_ISLAND ?= 1
GBA_ISLAND_VERIFY ?= 0
# Guarded, experimental sequential THUMB fetch shortcut; off by default.
GBA_FETCH_FAST ?= 0
POLL_SKIP ?= 0
# v1.104: JIT blocks continue past stores while the ARM9 runs alone
STORE_EXIT ?= 1
# GBA_FAST=1: relaxed GBA timing (block JIT, flat per-instruction cost). 0 = cycle-exact path.
GBA_FAST ?= 0
# HW3D=0: default to software 3D for accuracy; HW3D=1 is experimental GX.
# Existing noods.ini gx3D settings override this default.
HW3D ?= 1
JIT_STATS ?= 0
JIT_DISPATCH_STATS ?= 0
# Optional MPU path diagnostics; does not change MPU enforcement.
MPU_PATH_STATS ?= 0
BROWSER   ?= 0
FULL_RAM_DUMP ?= 0
G2_SCRIPTED_INPUT ?= 0
TEST_FAST ?= 0
PROFILE ?= 0
OBJ_WORD_READS ?= 1
DS_OBJ_FAST ?= 1
DS_OBJ_VERIFY ?= 0
# GX_2D_IO: 0 reference I/O, 1 targeted flushes, 2 targeted flushes + RGB565 copy, 3 also packed tile rows, 4 also immutable palette refs, 5 also packed scrolled rows, 6 also transaction TLUT cache + split backdrop/atlas binding (batch only), 7 single-quad layered TEV (batch only), 8 stable per-BG TLUT names (batch only), 9 bind only sampled four-row atlas blocks (batch only)
GX_2D_BITMAP ?= 1
GX_2D_IO ?= 9
GX_2D_PAL_CACHE ?= 1
GX_2D_BATCH ?= 1
GX_2D ?= 1
GX_2D_VERIFY ?= 0
GX_NORMAL_MATH ?= 1
GX_NORMAL_VERIFY ?= 0
DMA_GXFIFO_VERIFY ?= 0
DMA_GXFIFO_MAP ?= 1
DMA_GXFIFO_FAST ?= 1
DMA_GXFIFO_STATS ?= 0
MPU_PURE_JIT ?= 1
# v1p190: cached one-page inspect-execute check per (page, privilege) class
MPU_JIT_PAGE_CACHE ?= 1
MPU_PAGE_CACHE ?= 1
MPU_CACHE_VERIFY ?= 0
MPU_PURE_FAST ?= 1
GX_SPAN_COVERAGE ?= 1
# Experimental X coverage expansion: 0 off, 2 X-only, 1 X and Y. Not DS accurate.
GX_EDGE_EXPERIMENT ?= 2
# Whole-frame software 3D for GX frames that require DS edge/AA/wire/shadow rules.
# This is correctness-first and can be slower than GX.
GX_ACCURACY_FALLBACK ?= 0
# v1.160: GX remains primary; bounded CPU delta pass for NDS-only features.
GX_DELTA ?= 1
# Hard budgets keep the CPU delta bounded on arbitrary commercial ROMs.
GX_DELTA_MAX_POLYS ?= 2048
GX_DELTA_MAX_CRITICAL ?= 64
GX_DELTA_MAX_ROWS ?= 160
# v1.161 R&D: CPU-derived scanline geometry replayed as GX quads; no CPU framebuffer delta.
GX_OVERLAY ?= 1
# Keep the old delta-check code compiled (always budget-skipped) for A/B timing.
GX_OVERLAY_KEEP_DELTA ?= 0
# Coarse geometry guard: large commercial frames stay on the normal GX path.
GX_OVERLAY_MAX_POLYS ?= 128
ifeq ($(GX_OVERLAY),1)
ifeq ($(GX_OVERLAY_KEEP_DELTA),0)
  override GX_DELTA := 0
endif
endif
GX_TEX_INDEX ?= 1
GX_INDEXED ?= 0
GX_INDEXED_VERIFY ?= 0
JIT_FORWARD ?= 1
REJECT_DISPATCH ?= 1
JIT_CONTEXT_CANDIDATES ?= 0
ARM_NOFIT_STEP ?= 1
DIRECT_HANDLERS ?= 1
SOLO_TEXT ?= 0
SOLO_TEXT_VERIFY ?= 0
BUDGET_DISPATCH ?= 1
GX_EMPTY_FAST ?= 1
GX_UNLOCK_WAIT ?= 1
JIT_RANGE_INVALIDATE ?= 1
JIT_ENTRY_ONLY ?= 0
JIT_SHARED_SMC ?= 1
JIT_CODEGEN_KIB ?= 48
JIT_HOT_VISITS ?= 16
JIT_LINE_ALIGN ?= 0
V130_JIT ?= 1
V130_GX ?= 1
LTO_JOBS ?= 2
EXTRA_DEFS ?=

ifeq ($(GBA_AFFINE_VERIFY),1)
  ifneq ($(GBA_AFFINE_WINDOW),1)
    $(error GBA_AFFINE_VERIFY requires GBA_AFFINE_WINDOW=1)
  endif
endif

# The release-tested execution island must not silently mix with other GBA cores.
ifneq ($(GBA_DEDICATED),0)
  ifneq ($(GBA_FAST),0)
    $(error GBA_DEDICATED requires GBA_FAST=0; relaxed timing is not supported)
  endif
  ifneq ($(GBA_JIT),0)
    $(error GBA_DEDICATED requires GBA_JIT=0; use one execution experiment at a time)
  endif
  ifneq ($(GBA_ARM_BURST),0)
    $(error GBA_DEDICATED requires GBA_ARM_BURST=0; use one execution experiment at a time)
  endif
endif

CC      := $(DEVKITPPC)/bin/powerpc-eabi-g++
MACHDEP := -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -fsigned-char -ffast-math -funroll-loops -fauto-inc-dec -finline-functions
INCLUDES += -I$(DEVKITPRO)/portlibs/wii/include -I$(DEVKITPRO)/portlibs/ppc/include -I$(DEVKITPRO)/libogc/include
JIT_DEFS := -DNOODS_MPU_PATH_STATS=$(MPU_PATH_STATS) -DNOODS_JIT_DISPATCH_STATS=$(JIT_DISPATCH_STATS) -DNOODS_GX_NORMAL_MATH=$(GX_NORMAL_MATH) -DNOODS_GX_NORMAL_VERIFY=$(GX_NORMAL_VERIFY) -DNOODS_DMA_GXFIFO_VERIFY=$(DMA_GXFIFO_VERIFY) -DNOODS_DMA_GXFIFO_MAP=$(DMA_GXFIFO_MAP) -DNOODS_DMA_GXFIFO_FAST=$(DMA_GXFIFO_FAST) -DNOODS_DMA_GXFIFO_STATS=$(DMA_GXFIFO_STATS) -DNOODS_MPU_PURE_JIT=$(MPU_PURE_JIT) -DNOODS_MPU_JIT_PAGE_CACHE=$(MPU_JIT_PAGE_CACHE) -DNOODS_MPU_PAGE_CACHE=$(MPU_PAGE_CACHE) -DNOODS_MPU_CACHE_VERIFY=$(MPU_CACHE_VERIFY) -DNOODS_MPU_PURE_FAST=$(MPU_PURE_FAST) -DNOODS_GX_SPAN_COVERAGE=$(GX_SPAN_COVERAGE) -DNOODS_GX_ACCURACY_FALLBACK=$(GX_ACCURACY_FALLBACK) -DNOODS_V160_GX_DELTA=$(GX_DELTA) -DNOODS_GX_DELTA_MAX_POLYS=$(GX_DELTA_MAX_POLYS) -DNOODS_GX_DELTA_MAX_CRITICAL=$(GX_DELTA_MAX_CRITICAL) -DNOODS_GX_DELTA_MAX_ROWS=$(GX_DELTA_MAX_ROWS) -DNOODS_V161_GX_OVERLAY=$(GX_OVERLAY) -DNOODS_GX_OVERLAY_MAX_POLYS=$(GX_OVERLAY_MAX_POLYS) -DNOODS_GBA_AFFINE_WINDOW=$(GBA_AFFINE_WINDOW) -DNOODS_GBA_AFFINE_VERIFY=$(GBA_AFFINE_VERIFY) -DNOODS_GX_TEX_INDEX=$(GX_TEX_INDEX) -DNOODS_GBA_DEDICATED=$(GBA_DEDICATED) -DNOODS_GBA_ROM_ISLAND=$(GBA_ROM_ISLAND) -DNOODS_GBA_ISLAND_STATS=$(GBA_ISLAND_STATS) -DNOODS_GBA_THUMB_ISLAND=$(GBA_THUMB_ISLAND) -DNOODS_GBA_ISLAND_VERIFY=$(GBA_ISLAND_VERIFY) -DNOODS_GBA_ARM_BURST=$(GBA_ARM_BURST) -DNOODS_GBA_OBJ_MASK=$(GBA_OBJ_MASK) -DNOODS_GX_2D_BITMAP=$(GX_2D_BITMAP) -DNOODS_GX_2D_IO=$(GX_2D_IO) -DNOODS_GX_2D_PAL_CACHE=$(GX_2D_PAL_CACHE) -DNOODS_GX_2D_BATCH=$(GX_2D_BATCH) -DNOODS_GX_2D=$(GX_2D) -DNOODS_GX_2D_VERIFY=$(GX_2D_VERIFY) -DNOODS_GX_INDEXED=$(GX_INDEXED) -DNOODS_GX_INDEXED_VERIFY=$(GX_INDEXED_VERIFY) -DNOODS_JIT_FORWARD=$(JIT_FORWARD) -DNOODS_REJECT_DISPATCH=$(REJECT_DISPATCH) -DNOODS_JIT_CONTEXT_CANDIDATES=$(JIT_CONTEXT_CANDIDATES) -DNOODS_ARM_NOFIT_STEP=$(ARM_NOFIT_STEP) -DNOODS_OBJ_WORD_READS=$(OBJ_WORD_READS) -DNOODS_DS_OBJ_FAST=$(DS_OBJ_FAST) -DNOODS_DS_OBJ_VERIFY=$(DS_OBJ_VERIFY) -DNOODS_DIRECT_HANDLERS=$(DIRECT_HANDLERS) -DNOODS_SOLO_TEXT=$(SOLO_TEXT) -DNOODS_SOLO_TEXT_VERIFY=$(SOLO_TEXT_VERIFY) -DNOODS_BUDGET_DISPATCH=$(BUDGET_DISPATCH) -DNOODS_GX_EMPTY_FAST=$(GX_EMPTY_FAST) -DNOODS_GX_UNLOCK_WAIT=$(GX_UNLOCK_WAIT) -DNOODS_JIT_RANGE_INVALIDATE=$(JIT_RANGE_INVALIDATE) -DNOODS_JIT_ENTRY_ONLY=$(JIT_ENTRY_ONLY) -DNOODS_JIT_SHARED_SMC=$(JIT_SHARED_SMC) -DNOODS_JIT_CODEGEN_KIB=$(JIT_CODEGEN_KIB) -DNOODS_JIT_HOT_VISITS=$(JIT_HOT_VISITS) -DNOODS_JIT_LINE_ALIGN=$(JIT_LINE_ALIGN) -DNOODS_V130_JIT=$(V130_JIT) -DNOODS_V130_GX=$(V130_GX) -DNOODS_JIT=$(JIT) -DNOODS_JIT_BLOCKS=$(BLOCKS) -DNOODS_GBA_JIT=$(GBA_JIT) -DNOODS_JIT_POLL_SKIP=$(POLL_SKIP) -DNOODS_JIT_STATS=$(JIT_STATS) -DNOODS_STORE_EXIT=$(STORE_EXIT) -DNOODS_GBA_FAST=$(GBA_FAST) -DNOODS_HW3D=$(HW3D)
BROWSER_DEFS := -DNOODS_FORCE_BROWSER=$(BROWSER)
TEST_DEFS := -DNOODS_GBA_FETCH_FAST=$(GBA_FETCH_FAST) -DNOODS_GX_EDGE_EXPERIMENT=$(GX_EDGE_EXPERIMENT)
ifeq ($(FULL_RAM_DUMP),1)
  TEST_DEFS += -DNOODS_FULL_RAM_DUMP=1
endif
ifeq ($(G2_SCRIPTED_INPUT),1)
  TEST_DEFS += -DNOODS_G2_SCRIPTED_INPUT=1
endif
ifeq ($(TEST_FAST),1)
  TEST_DEFS += -DNOODS_TEST_FAST=1
endif
ifeq ($(PROFILE),1)
  TEST_DEFS += -DNOODS_V180_PROFILE=1 -DNOODS_NDS_TASK_PROFILE=1
endif
CXXFLAGS += -O3 -fno-rtti -MMD -MP \
-DENDIAN_BIG \
-DNOODS_V172_OPT -DNOODS_V174_OPT -DNOODS_V175_OPT -DNOODS_V176_OPT -DNOODS_V177_OPT -DNOODS_V178_OPT -DNOODS_V180_OPT \
$(JIT_DEFS) \
$(BROWSER_DEFS) \
$(TEST_DEFS) $(EXTRA_DEFS) \
$(INCLUDES) $(MACHDEP) \
-finline-limit=1000 -fgcse-after-reload -fpredictive-commoning -ftree-vectorize -flto=$(LTO_JOBS) -fno-stack-protector
LDFLAGS += -flto=$(LTO_JOBS) -fuse-linker-plugin -Wl,--gc-sections

CONFIG_TAG ?= p170
ifneq ($(GX_EDGE_EXPERIMENT),0)
  CONFIG_TAG := $(CONFIG_TAG)_gxedge$(GX_EDGE_EXPERIMENT)
endif
ifeq ($(GX_OVERLAY),1)
  CONFIG_TAG := $(CONFIG_TAG)_puregx
endif
ifeq ($(GX_ACCURACY_FALLBACK),1)
  CONFIG_TAG := $(CONFIG_TAG)_edgefb
endif
ifeq ($(GX_OVERLAY_KEEP_DELTA),1)
  CONFIG_TAG := $(CONFIG_TAG)_keepdelta
endif
ifeq ($(FULL_RAM_DUMP),1)
  CONFIG_TAG := $(CONFIG_TAG)_fullram
endif
ifeq ($(G2_SCRIPTED_INPUT),1)
  CONFIG_TAG := $(CONFIG_TAG)_si
endif
ifeq ($(PROFILE),1)
  CONFIG_TAG := $(CONFIG_TAG)_profile
endif
ifeq ($(HW3D),0)
  CONFIG_TAG := $(CONFIG_TAG)_sw3d
endif
ifeq ($(GBA_FAST),0)
  CONFIG_TAG := $(CONFIG_TAG)_gbaexact
endif
ifeq ($(GBA_ROM_ISLAND),1)
  CONFIG_TAG := $(CONFIG_TAG)_romisl
endif
ifeq ($(GBA_FETCH_FAST),1)
  CONFIG_TAG := $(CONFIG_TAG)_fetchfast
endif
ifeq ($(GBA_DEDICATED),1)
  CONFIG_TAG := $(CONFIG_TAG)_gbaded
endif
ifeq ($(STORE_EXIT),0)
  CONFIG_TAG := $(CONFIG_TAG)_nostoreexit
endif
ifneq ($(EXTRA_DEFS),)
  CONFIG_TAG := $(CONFIG_TAG)_$(subst =,,$(subst -D,,$(subst $() ,_,$(strip $(EXTRA_DEFS)))))
endif
ifeq ($(GX_2D_BITMAP),1)
  CONFIG_TAG := m_$(CONFIG_TAG)
endif
ifeq ($(GBA_OBJ_MASK),1)
  CONFIG_TAG := o_$(CONFIG_TAG)
endif
ifneq ($(GBA_ARM_BURST),0)
  CONFIG_TAG := r$(GBA_ARM_BURST)_$(CONFIG_TAG)
endif
ifneq ($(GBA_DEDICATED),0)
  CONFIG_TAG := d_$(CONFIG_TAG)
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

# Always select the requested configuration, even when its tagged DOL is
# already up to date (e.g. switching back from a scripted-input build).
all: $(CONFIG_OUT).dol
	cp -f $(CONFIG_OUT).dol $(TARGET).dol
	cp -f $(CONFIG_OUT).elf $(TARGET).elf
	cp -f $(CONFIG_OUT).elf.map $(TARGET).elf.map

-include $(DEPS)

$(CONFIG_OUT).dol: $(CONFIG_OUT).elf
	$(ELF2DOL) $< $@

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
