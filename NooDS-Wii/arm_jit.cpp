/*
    Copyright (C) 2026 radicalten

    This file is part of NooDS-Wii.

    NooDS-Wii is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS-Wii is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS-Wii. If not, see <https://www.gnu.org/licenses/>.
*/

#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include <cstring>
#include <algorithm>
#ifdef GEKKO
#include <gccore.h>
#endif
#include "arm_jit.h"
#include "core.h"

#if NOODS_JIT_STATS
#define JIT_STAT(...) do { __VA_ARGS__; } while (0)
#else
#define JIT_STAT(...) do { } while (0)
#endif

// Diagnostics only: count the ARM7 Thumb fallback instruction families (bits 15:10).
#ifndef NOODS_JIT_THUMB_HIST
#define NOODS_JIT_THUMB_HIST 0
#endif
#if NOODS_JIT_THUMB_HIST
static uint32_t s_thumbFallback7[64];
#endif
#ifndef NOODS_JIT_LOAD_HIST
#define NOODS_JIT_LOAD_HIST 0
#endif
#if NOODS_JIT_LOAD_HIST
static uint32_t s_leadingLoadCount[2][256];
static uint32_t s_leadingLoadLast[2][256];
#endif

// Diagnostics: per-guest-PC dispatch profile (block hits, stub hits, interpreter
// fallbacks) per CPU/ISA. Opt-in; used to decide which opcode classes are worth
// translating natively.
#ifndef NOODS_JIT_HOTPC
#define NOODS_JIT_HOTPC 0
#endif
#if NOODS_JIT_HOTPC
#define FBPC_SLOTS 2048
enum FbKind { FBK_BLOCK = 0, FBK_STUB = 1, FBK_FALLBACK = 2, FBK_KINDS = 3 };
struct FbSite { uint32_t pc; uint32_t opcode; uint32_t n[FBK_KINDS]; };
static FbSite s_fbSite[4][FBPC_SLOTS];
static inline void fbSiteRecord(int idx, uint32_t pc, uint32_t opcode, int kind) {
    FbSite *tab = s_fbSite[idx];
    uint32_t key = (pc ^ (pc >> 9) ^ (opcode >> 13)) & (FBPC_SLOTS - 1);
    for (uint32_t probe = 0; probe < 128; probe++) {
        FbSite &e = tab[(key + probe) & (FBPC_SLOTS - 1)];
        uint32_t total = e.n[0] + e.n[1] + e.n[2];
        if (total == 0) { e.pc = pc; e.opcode = opcode; e.n[kind] = 1; return; }
        if (e.pc == pc) { e.n[kind]++; e.opcode = opcode; return; }
    }
}
#endif

#if NOODS_JIT_PROFILE
extern "C" {
    #include <tuxedo/ppc/clock.h>
}
// Guest-timebase profiling of dispatch categories. Buckets:
// 0 block, 1 stub, 2 fallback LDR/STR/halfword, 3 fallback LDM/STM, 4 fallback B/BL,
// 5 fallback other ARM, 6 fallback Thumb, 7 fallback ARM9 CP15
static uint64_t s_profTicks[2][16];
static uint64_t s_profCount[2][16];
static int s_profLast = -1;
static const char *const s_profNames[16] = {"block","stub","fb_mem","fb_ldm","fb_branch","fb_arm_other","fb_thumb","fb_cp15",
                                            "block_loadfirst","fb_pcdst","fb_halfword","","","","",""};

struct ProfGuard {
    int cpuIdx; uint32_t opcode; bool thumb; uint32_t t0;
    ProfGuard(bool arm7, uint32_t op, bool th) : cpuIdx(arm7 ? 1 : 0), opcode(op), thumb(th) {
        s_profLast = -1;
        t0 = PPCGetTickCount();
    }
    ~ProfGuard() {
        uint32_t dt = PPCGetTickCount() - t0;
        int b = s_profLast;
        if (b < 0) {
            if (thumb) b = 6;
            else if (cpuIdx == 0 && (opcode & 0x0F000010u) == 0x0E000010u) b = 7;
            else {
                uint32_t c = (opcode >> 25) & 7;
                bool halfword = (c == 0) && (opcode & 0x90u) == 0x90u && (opcode & 0x60u) != 0;
                bool pcDst = ((opcode >> 12) & 0xFu) == 0xFu && c != 5;
                if (pcDst && !halfword) b = 9;
                else if (halfword) b = 10;
                else if ((c == 2) || (c == 3)) b = 2;
                else if (c == 4) b = 3;
                else if (c == 5) b = 4;
                else b = 5;
            }
        }
        s_profTicks[cpuIdx][b] += dt;
        s_profCount[cpuIdx][b]++;
    }
};
#define PROF_GUARD(arm7, op, th) ProfGuard _profGuard((arm7), (op), (th))
#define PROF_MARK(v) (s_profLast = (v))
#else
#define PROF_GUARD(arm7, op, th) ((void)0)
#define PROF_MARK(v) ((void)0)
#endif

void *Noods_MEM2_Alloc(size_t size);

// R15 value (guest address of the instruction + 2*step) of the instruction being emitted; used by helper slow paths.
static uint32_t s_emitInstrR15 = 0;
static uint32_t s_candPc[2][ArmPpcJit::BLOCK_TABLE_SIZE];
static uint8_t  s_candHits[2][ArmPpcJit::BLOCK_TABLE_SIZE];

// ---- Native guest load/store classification (ARM & Thumb) ----
static inline bool isMemClassArm(uint32_t op) {
    if ((op & 0x0C000000u) == 0x04000000u) return true;
    if ((op & 0x0E000090u) == 0x00000090u && (op & 0x60u) != 0) return true;
    if ((op & 0xFFFF8000u) == 0xE92D0000u || (op & 0xFFFF8000u) == 0xE8BD0000u) return true;
    return false;
}

#ifndef NOODS_JIT_DISPSTAT_FAST
#define NOODS_JIT_DISPSTAT_FAST 1
#endif
#ifndef NOODS_JIT_KEYINPUT_FAST
#define NOODS_JIT_KEYINPUT_FAST 1
#endif
#ifndef NOODS_JIT_IPCSYNC_FAST
#define NOODS_JIT_IPCSYNC_FAST 1
#endif

// Pre-indexed, no writeback, load to a non-PC register (including PC-relative constant pools!).
static inline bool isNativeLoadArm(bool arm7, uint32_t op) {
    (void)arm7;
    if ((op >> 28) == 0xFu) return false;
    if (((op >> 12) & 0xFu) == 15u) return false;
    if ((op & (1u << 20)) == 0) return false;   // L = 1
    if ((op & (1u << 24)) == 0) return false;   // P = 1 (pre-indexed)
    if ((op & (1u << 21)) != 0) return false;   // W = 0
    if ((op & 0x0C000000u) == 0x04000000u) {    // LDR / LDRB
        if ((op & (1u << 25)) != 0) {
            if (((op >> 16) & 0xFu) == 15u) return false;
            if ((op & (1u << 4)) != 0) return false;
            if ((op & 0xFu) == 15u) return false;
            if (((op >> 5) & 3u) != 0u) return false;    // only LSL-scaled register offsets
        }
        return true;
    }
    if ((op & 0x0E000090u) == 0x00000090u && (op & 0x60u) != 0u) { // LDRH / LDRSB / LDRSH
        if ((op & (1u << 22)) == 0) {           // register offset
            if (((op >> 16) & 0xFu) == 15u) return false;
            if ((op & 0xF00u) != 0) return false;
            if ((op & 0xFu) == 15u) return false;
        }
        return true;
    }
    return false;
}

// Native STR / STRB / STRH, pre- or post-indexed, immediate or LSL register offsets, unconditional, Rn/Rd != PC.
// Excludes P=0, W=1 (unprivileged STRT/STRBT) so privilege-override instructions stay in the interpreter.
static inline bool isNativeStoreArm(uint32_t op) {
    if ((op >> 28) != 0xEu) return false;
    if ((op & (1u << 20)) != 0) return false;            // L = 0 (store)
    if (((op >> 16) & 0xFu) == 15u) return false;
    if (((op >> 12) & 0xFu) == 15u) return false;
    const bool pre = (op & (1u << 24)) != 0;
    const bool wb  = (op & (1u << 21)) != 0;
    if ((op & 0x0C000000u) == 0x04000000u) {             // STR / STRB
        if (!pre && wb) return false;                    // STRT / STRBT
        if ((op & (1u << 25)) != 0) {
            if ((op & (1u << 4)) != 0) return false;
            if ((op & 0xFu) == 15u) return false;
            if (((op >> 5) & 3u) != 0u) return false;
        }
        return true;
    }
    if ((op & 0x0E000090u) != 0x00000090u || (op & 0xF0u) != 0xB0u) return false; // STRH
    if ((op & (1u << 22)) == 0) {                        // register offset (unshifted)
        if ((op & 0xF00u) != 0) return false;
        if ((op & 0xFu) == 15u) return false;
    }
    return true;
}

static inline bool isNativePushPopArm(uint32_t op) {
    if ((op & 0xFFFFu) == 0u) return false;
    if ((op & (1u << 13)) != 0u) return false;           // SP not in list
    if ((op & (1u << 15)) != 0u) return false;           // R15 not in list
    return (op & 0xFFFF0000u) == 0xE92D0000u || (op & 0xFFFF0000u) == 0xE8BD0000u;
}

static inline bool isNativeLoadThumb(uint16_t op) {
    if ((op & 0xF800u) == 0x4800u) return true; // LDR Rd, [PC, #imm8]
    if ((op & 0xF000u) == 0x5000u && (op & 0x0E00u) >= 0x0600u) return true; // LDSB/LDR/LDRH/LDRB/LDSH Rd, [Rb, Ro]
    if ((op & 0xF800u) == 0x6800u) return true; // LDR Rd, [Rb, #imm5]
    if ((op & 0xF800u) == 0x7800u) return true; // LDRB Rd, [Rb, #imm5]
    if ((op & 0xF800u) == 0x8800u) return true; // LDRH Rd, [Rb, #imm5]
    if ((op & 0xF800u) == 0x9800u) return true; // LDR Rd, [SP, #imm8]
    return false;
}

static inline bool isNativeStoreThumb(uint16_t op) {
    if ((op & 0xF000u) == 0x5000u && (op & 0x0E00u) < 0x0600u) return true; // STR/STRH/STRB Rd, [Rb, Ro]
    if ((op & 0xF800u) == 0x6000u) return true; // STR Rd, [Rb, #imm5]
    if ((op & 0xF800u) == 0x7000u) return true; // STRB Rd, [Rb, #imm5]
    if ((op & 0xF800u) == 0x8000u) return true; // STRH Rd, [Rb, #imm5]
    if ((op & 0xF800u) == 0x9000u) return true; // STR Rd, [SP, #imm8]
    return false;
}

static inline bool isNativePushPopThumb(uint16_t op) {
    if ((op & 0xFE00u) == 0xB400u && (op & 0x01FFu) != 0u) return true; // PUSH {rlist[, lr]}
    if ((op & 0xFF00u) == 0xBC00u && (op & 0x00FFu) != 0u) return true; // POP {rlist}
    return false;
}

// ARM LDR/LDRB/LDRH with Rd == 15: a computed branch. Only pre-indexed forms are compiled natively;
// post-indexed / unprivileged forms keep their old interpreter fallback. Non-PC loads are untouched.
static inline bool isPcDestLoadTerminatorArm(bool arm7, uint32_t op) {
    (void)arm7;
    if ((op >> 28) == 0xFu) return false;
    if (((op >> 12) & 0xFu) != 15u) return false;
    if ((op & (1u << 20)) == 0) return false;              // L (load)
    if ((op & (1u << 24)) == 0) return false;              // P (pre-indexed only)
    if (((op >> 16) & 0xFu) == 15u && (op & (1u << 21)) != 0) return false; // PC base with writeback
    if ((op & 0x0C000000u) == 0x04000000u) {               // LDR / LDRB
        if ((op & (1u << 25)) != 0) {                      // register offset
            if ((op & (1u << 4)) != 0) return false;
            if ((op & 0xFu) == 15u) return false;
            if (((op >> 5) & 3u) != 0u) return false;      // only LSL-scaled register offsets
        }
        return true;
    }
    if ((op & 0x0E000090u) == 0x00000090u && (op & 0xF0u) == 0xB0u) { // LDRH only
        if ((op & (1u << 22)) == 0) {                      // register offset (unshifted)
            if ((op & 0xF00u) != 0) return false;
            if ((op & 0xFu) == 15u) return false;
        }
        return true;
    }
    return false;
}


// 4 MiB aligned executable code pool directly in fast Wii MEM1 1T-SRAM (0x80xxxxxx):
// 1. Real Wii hardware: 4x lower I-cache refill latency than DDR2 MEM2 (0x90xxxxxx).
// 2. Dolphin JIT64/JITArm64: FastBlockMap ONLY covers MEM1 (0x80000000..0x81FFFFFF)!
//    Putting JIT code in MEM2 forced every single `bctr` block entry to miss FastBlockMap
//    and trap out of native x86_64/ARM64 JIT into C++ slow lookup!
alignas(32) static uint32_t s_mem1CodePool[1048576];

uint32_t *ArmPpcJit::codePool = nullptr;
size_t ArmPpcJit::poolWordsUsed = 0;
size_t ArmPpcJit::poolLimitWords = ArmPpcJit::DEFAULT_POOL_WORDS;
uint32_t ArmPpcJit::currentEpoch = 1;
bool ArmPpcJit::initialized = false;

ArmPpcJit::StubEntry ArmPpcJit::stubTable[2][ArmPpcJit::STUB_TABLE_SIZE];
ArmPpcJit::BlockEntry ArmPpcJit::blockTable[2][ArmPpcJit::BLOCK_TABLE_SIZE];
uint32_t ArmPpcJit::pageEpoch[2][ArmPpcJit::PAGE_EPOCH_SIZE];
uint32_t ArmPpcJit::pageHasBlockBits[2][ArmPpcJit::PAGE_EPOCH_SIZE / 32];
uint32_t ArmPpcJit::codeLineBits[ArmPpcJit::CODE_LINE_WORDS];
ArmPpcJit::Stats ArmPpcJit::stats = {};

int ArmPpcJit::fallbackArm(Interpreter *cpu, uint32_t opcode) {
    return (cpu->*Interpreter::armInstrs[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0xF)])(opcode);
}

int ArmPpcJit::fallbackThumb(Interpreter *cpu, uint32_t opcode) {
    return (cpu->*Interpreter::thumbInstrs[(opcode >> 6) & 0x3FF])((uint16_t)opcode);
}

uintptr_t ArmPpcJit::resolveArmHandler(uint32_t opcode) {
    struct Pmf { uintptr_t fn; ptrdiff_t adj; };
    Pmf pmf;
    auto m = Interpreter::armInstrs[((opcode >> 16) & 0xFF0u) | ((opcode >> 4) & 0xFu)];
    std::memcpy(&pmf, &m, sizeof(pmf));
    return ((pmf.fn & 1u) == 0 && pmf.adj == 0) ? pmf.fn : (uintptr_t)&ArmPpcJit::fallbackArm;
}

uintptr_t ArmPpcJit::resolveThumbHandler(uint16_t opcode) {
    struct Pmf { uintptr_t fn; ptrdiff_t adj; };
    Pmf pmf;
    auto m = Interpreter::thumbInstrs[(opcode >> 6) & 0x3FFu];
    std::memcpy(&pmf, &m, sizeof(pmf));
    return ((pmf.fn & 1u) == 0 && pmf.adj == 0) ? pmf.fn : (uintptr_t)&ArmPpcJit::fallbackThumb;
}

int ArmPpcJit::condBranchArm(Interpreter *cpu, uint32_t opcode) {
    switch (Interpreter::condition[((opcode >> 24) & 0xF0) | (cpu->cpsr >> 28)]) {
        case 0: return 1;
        case 2: return cpu->handleReserved(opcode);
        default: return (cpu->*Interpreter::armInstrs[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0xF)])(opcode);
    }
}

int ArmPpcJit::execBranchArm(Interpreter *cpu, uint32_t opcode, int32_t remDelta, int32_t totalDelta, int32_t prefixCycles) {
    uint32_t oldR15 = cpu->registersUsr[15];
    uint8_t cond = (uint8_t)(opcode >> 28);
    if (cond != 0xEu) {
        uint8_t c = Interpreter::condition[((opcode >> 24) & 0xF0u) | (cpu->cpsr >> 28)];
        if (c == 0) {
            cpu->registersUsr[15] = oldR15 + (uint32_t)remDelta;
            uint8_t *pd = cpu->pcData + totalDelta;
            cpu->pcData = pd;
            cpu->pipeline[0] = U8TO32(pd - 4, 0);
            cpu->pipeline[1] = U8TO32(pd, 0);
            return prefixCycles + 1;
        }
        if (c == 2) {
            cpu->registersUsr[15] = oldR15 + (uint32_t)remDelta;
            uint8_t *pd = cpu->pcData + totalDelta;
            cpu->pcData = pd;
            cpu->pipeline[0] = U8TO32(pd - 4, 0);
            cpu->pipeline[1] = U8TO32(pd, 0);
            return prefixCycles + cpu->handleReserved(opcode);
        }
    }
    if ((opcode & 0x0E000000u) == 0x0A000000u) {
        int32_t offset = (int32_t)(opcode << 8) >> 6;
        uint32_t r15 = oldR15 + (uint32_t)remDelta;
        if (opcode & (1u << 24)) // BL
            *cpu->registers[14] = r15 - 4;
        uint32_t target = (r15 + (uint32_t)offset) & ~3u;
        uint32_t newR15 = target + 4u;
        if ((((target ^ oldR15) | (newR15 ^ oldR15)) >> 12) == 0) {
            uint8_t *pd = cpu->pcData + (int32_t)(newR15 - oldR15) + (totalDelta - remDelta);
            cpu->registersUsr[15] = newR15;
            cpu->pcData = pd;
            cpu->pipeline[0] = U8TO32(pd - 4, 0);
            cpu->pipeline[1] = U8TO32(pd, 0);
            return prefixCycles + 3;
        }
        cpu->registersUsr[15] = target;
        cpu->flushPipeline();
        return prefixCycles + 3;
    }
    // BX Rm / ARM9 BLX Rm. Read Rm before writing LR (Rm may be R14).
    cpu->registersUsr[15] = oldR15 + (uint32_t)remDelta;
    uint32_t target = *cpu->registers[opcode & 0xFu];
    if (!cpu->arm7 && (opcode & (1u << 5)))
        *cpu->registers[14] = cpu->registersUsr[15] - 4u;
    cpu->cpsr |= (target & 1u) << 5;
    cpu->registersUsr[15] = target;
    cpu->flushPipeline();
    return prefixCycles + 3;
}

int ArmPpcJit::execBranchThumb(Interpreter *cpu, uint32_t opcode, int32_t remDelta, int32_t totalDelta, int32_t prefixCycles) {
    uint32_t oldR15 = cpu->registersUsr[15];
    if ((opcode & 0xF000u) == 0xD000u) { // Bcc label
        if (!Interpreter::condition[((opcode >> 4) & 0xF0u) | (cpu->cpsr >> 28)]) {
            cpu->registersUsr[15] = oldR15 + (uint32_t)remDelta;
            uint8_t *pd = cpu->pcData + totalDelta;
            cpu->pcData = pd;
            cpu->pipeline[0] = U8TO16(pd - 2, 0);
            cpu->pipeline[1] = U8TO16(pd, 0);
            return prefixCycles + 1;
        }
        int32_t offset = (int32_t)(int8_t)(opcode & 0xFFu) << 1;
        uint32_t target = (oldR15 + (uint32_t)(remDelta + offset)) & ~1u;
        uint32_t newR15 = target + 2u;
        if ((((target ^ oldR15) | (newR15 ^ oldR15)) >> 12) == 0) {
            uint8_t *pd = cpu->pcData + (int32_t)(newR15 - oldR15) + (totalDelta - remDelta);
            cpu->registersUsr[15] = newR15;
            cpu->pcData = pd;
            cpu->pipeline[0] = U8TO16(pd - 2, 0);
            cpu->pipeline[1] = U8TO16(pd, 0);
            return prefixCycles + 3;
        }
        cpu->registersUsr[15] = target;
        cpu->flushPipeline();
        return prefixCycles + 3;
    }
    if ((opcode & 0xF800u) == 0xE000u) { // B label
        int32_t offset = ((int32_t)(int16_t)(opcode << 5)) >> 4;
        uint32_t target = (oldR15 + (uint32_t)(remDelta + offset)) & ~1u;
        uint32_t newR15 = target + 2u;
        if ((((target ^ oldR15) | (newR15 ^ oldR15)) >> 12) == 0) {
            uint8_t *pd = cpu->pcData + (int32_t)(newR15 - oldR15) + (totalDelta - remDelta);
            cpu->registersUsr[15] = newR15;
            cpu->pcData = pd;
            cpu->pipeline[0] = U8TO16(pd - 2, 0);
            cpu->pipeline[1] = U8TO16(pd, 0);
            return prefixCycles + 3;
        }
        cpu->registersUsr[15] = target;
        cpu->flushPipeline();
        return prefixCycles + 3;
    }
    if ((opcode & 0xFF00u) == 0x4700u) { // BX Rs or BLX Rs
        cpu->registersUsr[15] = oldR15 + (uint32_t)remDelta;
        uint32_t target = *cpu->registers[(opcode >> 3) & 0xFu];
        if (opcode & 0x0080u) { // BLX Rs (ARM9 only)
            *cpu->registers[14] = cpu->registersUsr[15] - 1;
        }
        cpu->cpsr &= ~((~target & 1u) << 5);
        cpu->registersUsr[15] = target;
        cpu->flushPipeline();
        return prefixCycles + 3;
    }
    // BL / BLX suffix (0xF800..0xFFFF or 0xE800..0xEFFF)
    uint32_t r15 = oldR15 + (uint32_t)remDelta;
    uint32_t offset = (opcode & 0x7FFu) << 1;
    if ((opcode & 0x1000u) == 0) { // BLX suffix (0xE800..0xEFFF, ARM9 only)
        cpu->cpsr &= ~(1u << 5);
    }
    uint32_t ret = r15 - 1;
    cpu->registersUsr[15] = *cpu->registers[14] + offset;
    *cpu->registers[14] = ret;
    cpu->flushPipeline();
    return prefixCycles + 3;
}

extern "C" int jit_fallback_arm(Interpreter *cpu, uint32_t opcode) {
    return ArmPpcJit::fallbackArm(cpu, opcode);
}

extern "C" int jit_fallback_thumb(Interpreter *cpu, uint32_t opcode) {
    return ArmPpcJit::fallbackThumb(cpu, opcode);
}

void ArmPpcJit::flushCodeRange(void *start, size_t byteCount) {
    if (!start || byteCount == 0) return;
    uintptr_t addr = (uintptr_t)start & ~(uintptr_t)31;
    uintptr_t end  = ((uintptr_t)start + byteCount + 31) & ~(uintptr_t)31;
#ifdef GEKKO
    for (uintptr_t p = addr; p < end; p += 32) {
        asm volatile("dcbst 0, %0" :: "r"(p) : "memory");
    }
    asm volatile("sync" ::: "memory");
    for (uintptr_t p = addr; p < end; p += 32) {
        asm volatile("icbi 0, %0" :: "r"(p) : "memory");
    }
    asm volatile("sync; isync" ::: "memory");
#else
    (void)addr;
    (void)end;
#endif
}

void ArmPpcJit::init() {
    if (!codePool) {
        codePool = s_mem1CodePool;
        poolLimitWords = sizeof(s_mem1CodePool) / sizeof(uint32_t);
    }
    poolWordsUsed = 0;
    if (poolLimitWords == 0 || poolLimitWords > (sizeof(s_mem1CodePool) / sizeof(uint32_t)))
        poolLimitWords = sizeof(s_mem1CodePool) / sizeof(uint32_t);
    currentEpoch = 1;
    std::memset(stubTable, 0, sizeof(stubTable));
    std::memset(blockTable, 0, sizeof(blockTable));
    std::memset(s_candPc, 0, sizeof(s_candPc));
    std::memset(s_candHits, 0, sizeof(s_candHits));
    std::memset(pageHasBlockBits, 0, sizeof(pageHasBlockBits));
    std::memset(codeLineBits, 0, sizeof(codeLineBits));
    for (int c = 0; c < 2; c++) {
        for (size_t i = 0; i < PAGE_EPOCH_SIZE; i++)
            pageEpoch[c][i] = 1;
    }
    JIT_STAT(std::memset(&stats, 0, sizeof(stats)));
    JIT_STAT(stats.poolLimitWords = poolLimitWords);
    initialized = true;
#ifdef GEKKO
    SYS_Report("[JIT] init codePool=%p poolLimitWords=%zu\n", (void*)codePool, poolLimitWords);
#endif
}

void ArmPpcJit::reset() {
#if NOODS_JIT_POLL_SKIP
    pollEpoch++;
#endif
    if (!initialized) {
        init();
        return;
    }
    size_t savedLimit = poolLimitWords;
    init();
    poolLimitWords = savedLimit;
    JIT_STAT(stats.poolLimitWords = savedLimit);
}

void ArmPpcJit::setPoolLimitWords(size_t maxWords) {
    if (!initialized) init();
    if (maxWords >= MAX_BLOCK_WORDS * 2 && maxWords <= DEFAULT_POOL_WORDS) {
        poolLimitWords = maxWords;
        JIT_STAT(stats.poolLimitWords = maxWords);
    }
}

void ArmPpcJit::wrapPool() {
    poolWordsUsed = 0;
    currentEpoch++;
    if (currentEpoch == 0 || currentEpoch >= 0x3FFFFFFFu) {
        currentEpoch = 1;
        std::memset(stubTable, 0, sizeof(stubTable));
        std::memset(blockTable, 0, sizeof(blockTable));
    }
    std::memset(pageHasBlockBits, 0, sizeof(pageHasBlockBits));
    std::memset(codeLineBits, 0, sizeof(codeLineBits));
    JIT_STAT(stats.poolWraps++);
    JIT_STAT(stats.poolWordsUsed = 0);
}

void ArmPpcJit::ensureSpace(size_t wordsNeeded) {
    if (poolWordsUsed + wordsNeeded > poolLimitWords) {
        wrapPool();
    }
}

static inline void emitLoadImm32(uint32_t *&p, uint8_t rd, uint32_t val) {
    if ((int32_t)val >= -32768 && (int32_t)val <= 32767) {
        *p++ = JitPpc::li(rd, (int16_t)val);
    } else if ((val & 0xFFFFu) == 0) {
        *p++ = JitPpc::lis(rd, (int16_t)(val >> 16));
    } else {
        *p++ = JitPpc::lis(rd, (int16_t)(val >> 16));
        *p++ = JitPpc::ori(rd, rd, (uint16_t)(val & 0xFFFFu));
    }
}

// Insert standard bit srcBitStd (0=LSB..31=MSB) of srcReg into standard bit dstBitStd of dstReg
static inline void emitInsertBit(uint32_t *&p, uint8_t dstReg, uint8_t srcReg, uint8_t dstBitStd, uint8_t srcBitStd) {
    uint8_t sh = (uint8_t)((dstBitStd - srcBitStd) & 31u);
    uint8_t ppcBit = (uint8_t)(31u - dstBitStd);
    *p++ = JitPpc::rlwimi(dstReg, srcReg, sh, ppcBit, ppcBit);
}

void ArmPpcJit::emitLoadGuestReg(uint32_t *&p, uint8_t dstPpc, uint8_t guestReg) {
    const int16_t offRegUsr = (int16_t)offsetof(Interpreter, registersUsr);
    const int16_t offRegs   = (int16_t)offsetof(Interpreter, registers);
    if (guestReg < 8 || guestReg == 15) {
        *p++ = JitPpc::lwz(dstPpc, JitPpc::R31, (int16_t)(offRegUsr + guestReg * 4));
    } else {
        *p++ = JitPpc::lwz(JitPpc::R11, JitPpc::R31, (int16_t)(offRegs + guestReg * 4));
        *p++ = JitPpc::lwz(dstPpc, JitPpc::R11, 0);
    }
}

void ArmPpcJit::emitStoreGuestReg(uint32_t *&p, uint8_t srcPpc, uint8_t guestReg) {
    const int16_t offRegUsr = (int16_t)offsetof(Interpreter, registersUsr);
    const int16_t offRegs   = (int16_t)offsetof(Interpreter, registers);
    if (guestReg < 8 || guestReg == 15) {
        *p++ = JitPpc::stw(srcPpc, JitPpc::R31, (int16_t)(offRegUsr + guestReg * 4));
    } else {
        *p++ = JitPpc::lwz(JitPpc::R11, JitPpc::R31, (int16_t)(offRegs + guestReg * 4));
        *p++ = JitPpc::stw(srcPpc, JitPpc::R11, 0);
    }
}

void ArmPpcJit::emitUpdateNZ(uint32_t *&p, uint8_t resReg) {
    const int16_t offCpsr = (int16_t)offsetof(Interpreter, cpsr);
    *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
    emitInsertBit(p, JitPpc::R6, resReg, 31, 31);          // N = res[31]
    *p++ = JitPpc::cntlzw(JitPpc::R7, resReg);             // R7 = 32 (bit 5) iff res == 0
    emitInsertBit(p, JitPpc::R6, JitPpc::R7, 30, 5);       // Z = R7[5]
    *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offCpsr);
}

// Requires: R3 = res (from addc R3, R4, R5), R4 = op1, R5 = op2
void ArmPpcJit::emitUpdateAddNZCV(uint32_t *&p) {
    const int16_t offCpsr = (int16_t)offsetof(Interpreter, cpsr);
    *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
    emitInsertBit(p, JitPpc::R6, JitPpc::R3, 31, 31);      // N = res[31]
    *p++ = JitPpc::cntlzw(JitPpc::R7, JitPpc::R3);
    emitInsertBit(p, JitPpc::R6, JitPpc::R7, 30, 5);       // Z = (res == 0)
    *p++ = JitPpc::mfxer(JitPpc::R7);
    emitInsertBit(p, JitPpc::R6, JitPpc::R7, 29, 29);      // C = XER.CA (standard bit 29)
    *p++ = JitPpc::xor_(JitPpc::R8, JitPpc::R3, JitPpc::R5); // R8 = res ^ op2
    *p++ = JitPpc::eqv(JitPpc::R9, JitPpc::R4, JitPpc::R5);  // R9 = ~(op1 ^ op2)
    *p++ = JitPpc::and_(JitPpc::R8, JitPpc::R8, JitPpc::R9); // R8 = ~(op1 ^ op2) & (res ^ op2)
    emitInsertBit(p, JitPpc::R6, JitPpc::R8, 28, 31);      // V = R8[31]
    *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offCpsr);
}

// Requires: R3 = res (from subfc R3, R5, R4 = R4 - R5), R4 = op1, R5 = op2
void ArmPpcJit::emitUpdateSubNZCV(uint32_t *&p) {
    const int16_t offCpsr = (int16_t)offsetof(Interpreter, cpsr);
    *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
    emitInsertBit(p, JitPpc::R6, JitPpc::R3, 31, 31);      // N = res[31]
    *p++ = JitPpc::cntlzw(JitPpc::R7, JitPpc::R3);
    emitInsertBit(p, JitPpc::R6, JitPpc::R7, 30, 5);       // Z = (res == 0)
    *p++ = JitPpc::mfxer(JitPpc::R7);
    emitInsertBit(p, JitPpc::R6, JitPpc::R7, 29, 29);      // C = XER.CA (standard bit 29)
    *p++ = JitPpc::xor_(JitPpc::R8, JitPpc::R4, JitPpc::R5); // R8 = op1 ^ op2
    *p++ = JitPpc::eqv(JitPpc::R9, JitPpc::R3, JitPpc::R5);  // R9 = ~(res ^ op2)
    *p++ = JitPpc::and_(JitPpc::R8, JitPpc::R8, JitPpc::R9); // R8 = (op1 ^ op2) & ~(res ^ op2)
    emitInsertBit(p, JitPpc::R6, JitPpc::R8, 28, 31);      // V = R8[31]
    *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offCpsr);
}

bool ArmPpcJit::emitNativeThumb(uint32_t *&p, Interpreter *cpu, uint16_t opcode, int &outCycles, bool emitFlags) {
    if (isNativeLoadThumb(opcode)) return emitNativeLoadThumb(p, cpu, opcode, outCycles);
    if (isNativeStoreThumb(opcode)) return emitNativeStoreThumb(p, cpu, opcode, outCycles);
    if (isNativePushPopThumb(opcode)) {
        if ((opcode & 0xFE00u) == 0xB400u) {
            uint16_t mask = (uint16_t)((opcode & 0xFFu) | ((opcode & 0x100u) ? (1u << 14) : 0u));
            return emitNativePushPop(p, cpu, mask, false, opcode, true, outCycles);
        }
        return emitNativePushPop(p, cpu, (uint16_t)(opcode & 0xFFu), true, opcode, true, outCycles);
    }
    const int16_t offCpsr = (int16_t)offsetof(Interpreter, cpsr);
    uint8_t top3 = (uint8_t)(opcode >> 13);

    switch (top3) {
    case 0: { // 000xx: Shift by immediate or add/sub reg/imm3
        uint8_t subOp = (uint8_t)((opcode >> 11) & 0x3);
        uint8_t rd = (uint8_t)(opcode & 0x7);
        uint8_t rs = (uint8_t)((opcode >> 3) & 0x7);
        uint8_t imm5 = (uint8_t)((opcode >> 6) & 0x1F);

        if (subOp == 0) { // LSL Rd, Rs, #imm5
            emitLoadGuestReg(p, JitPpc::R4, rs);
            if (imm5 == 0) {
                *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R4);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                if (emitFlags) emitUpdateNZ(p, JitPpc::R3);
            } else {
                *p++ = JitPpc::slwi(JitPpc::R3, JitPpc::R4, imm5);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                if (emitFlags) {
                    *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
                    emitInsertBit(p, JitPpc::R6, JitPpc::R3, 31, 31);
                    *p++ = JitPpc::cntlzw(JitPpc::R7, JitPpc::R3);
                    emitInsertBit(p, JitPpc::R6, JitPpc::R7, 30, 5);
                    emitInsertBit(p, JitPpc::R6, JitPpc::R4, 29, (uint8_t)(32 - imm5));
                    *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offCpsr);
                }
            }
            outCycles = 1;
            return true;
        }
        if (subOp == 1) { // LSR Rd, Rs, #imm5
            emitLoadGuestReg(p, JitPpc::R4, rs);
            if (imm5 == 0) {
                *p++ = JitPpc::li(JitPpc::R3, 0);
            } else {
                *p++ = JitPpc::srwi(JitPpc::R3, JitPpc::R4, imm5);
            }
            emitStoreGuestReg(p, JitPpc::R3, rd);
            if (emitFlags) {
                *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
                emitInsertBit(p, JitPpc::R6, JitPpc::R3, 31, 31);
                *p++ = JitPpc::cntlzw(JitPpc::R7, JitPpc::R3);
                emitInsertBit(p, JitPpc::R6, JitPpc::R7, 30, 5);
                emitInsertBit(p, JitPpc::R6, JitPpc::R4, 29, (uint8_t)(imm5 ? (imm5 - 1) : 31));
                *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offCpsr);
            }
            outCycles = 1;
            return true;
        }
        if (subOp == 2) { // ASR Rd, Rs, #imm5
            emitLoadGuestReg(p, JitPpc::R4, rs);
            *p++ = JitPpc::srawi(JitPpc::R3, JitPpc::R4, (uint8_t)(imm5 ? imm5 : 31));
            emitStoreGuestReg(p, JitPpc::R3, rd);
            if (emitFlags) {
                *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
                emitInsertBit(p, JitPpc::R6, JitPpc::R3, 31, 31);
                *p++ = JitPpc::cntlzw(JitPpc::R7, JitPpc::R3);
                emitInsertBit(p, JitPpc::R6, JitPpc::R7, 30, 5);
                emitInsertBit(p, JitPpc::R6, JitPpc::R4, 29, (uint8_t)(imm5 ? (imm5 - 1) : 31));
                *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offCpsr);
            }
            outCycles = 1;
            return true;
        }
        // subOp == 3: ADD/SUB reg or imm3
        uint8_t kind = (uint8_t)((opcode >> 9) & 0x3);
        uint8_t rn_or_imm = (uint8_t)((opcode >> 6) & 0x7);
        emitLoadGuestReg(p, JitPpc::R4, rs);
        if (kind & 2) {
            *p++ = JitPpc::li(JitPpc::R5, (int16_t)rn_or_imm);
        } else {
            emitLoadGuestReg(p, JitPpc::R5, rn_or_imm);
        }
        if (kind & 1) { // SUB
            if (emitFlags) {
                *p++ = JitPpc::subfc(JitPpc::R3, JitPpc::R5, JitPpc::R4);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                emitUpdateSubNZCV(p);
            } else {
                *p++ = JitPpc::subf(JitPpc::R3, JitPpc::R5, JitPpc::R4);
                emitStoreGuestReg(p, JitPpc::R3, rd);
            }
        } else { // ADD
            if (emitFlags) {
                *p++ = JitPpc::addc(JitPpc::R3, JitPpc::R4, JitPpc::R5);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                emitUpdateAddNZCV(p);
            } else {
                *p++ = JitPpc::add(JitPpc::R3, JitPpc::R4, JitPpc::R5);
                emitStoreGuestReg(p, JitPpc::R3, rd);
            }
        }
        outCycles = 1;
        return true;
    }

    case 1: { // 001xx: MOV/CMP/ADD/SUB Rd, #imm8
        uint8_t op = (uint8_t)((opcode >> 11) & 0x3);
        uint8_t rd = (uint8_t)((opcode >> 8) & 0x7);
        uint8_t imm8 = (uint8_t)(opcode & 0xFF);
        if (op == 0) { // MOV Rd, #imm8
            *p++ = JitPpc::li(JitPpc::R3, (int16_t)imm8);
            emitStoreGuestReg(p, JitPpc::R3, rd);
            if (emitFlags) emitUpdateNZ(p, JitPpc::R3);
            outCycles = 1;
            return true;
        }
        if (op == 1) { // CMP Rd, #imm8
            if (emitFlags) {
                emitLoadGuestReg(p, JitPpc::R4, rd);
                *p++ = JitPpc::li(JitPpc::R5, (int16_t)imm8);
                *p++ = JitPpc::subfc(JitPpc::R3, JitPpc::R5, JitPpc::R4);
                emitUpdateSubNZCV(p);
            }
            outCycles = 1;
            return true;
        }
        emitLoadGuestReg(p, JitPpc::R4, rd);
        *p++ = JitPpc::li(JitPpc::R5, (int16_t)imm8);
        if (op == 2) { // ADD Rd, #imm8
            if (emitFlags) {
                *p++ = JitPpc::addc(JitPpc::R3, JitPpc::R4, JitPpc::R5);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                emitUpdateAddNZCV(p);
            } else {
                *p++ = JitPpc::add(JitPpc::R3, JitPpc::R4, JitPpc::R5);
                emitStoreGuestReg(p, JitPpc::R3, rd);
            }
        } else { // SUB Rd, #imm8
            if (emitFlags) {
                *p++ = JitPpc::subfc(JitPpc::R3, JitPpc::R5, JitPpc::R4);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                emitUpdateSubNZCV(p);
            } else {
                *p++ = JitPpc::subf(JitPpc::R3, JitPpc::R5, JitPpc::R4);
                emitStoreGuestReg(p, JitPpc::R3, rd);
            }
        }
        outCycles = 1;
        return true;
    }

    case 2: {
        if ((opcode & 0xFC00u) == 0x4000u) { // 010000: Data-processing register
            uint8_t aluOp = (uint8_t)((opcode >> 6) & 0xF);
            uint8_t rd = (uint8_t)(opcode & 0x7);
            uint8_t rs = (uint8_t)((opcode >> 3) & 0x7);
            switch (aluOp) {
            case 0x0: // AND Rd, Rs
                emitLoadGuestReg(p, JitPpc::R4, rd);
                emitLoadGuestReg(p, JitPpc::R5, rs);
                *p++ = JitPpc::and_(JitPpc::R3, JitPpc::R4, JitPpc::R5);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                if (emitFlags) emitUpdateNZ(p, JitPpc::R3);
                outCycles = 1;
                return true;
            case 0x1: // EOR Rd, Rs
                emitLoadGuestReg(p, JitPpc::R4, rd);
                emitLoadGuestReg(p, JitPpc::R5, rs);
                *p++ = JitPpc::xor_(JitPpc::R3, JitPpc::R4, JitPpc::R5);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                if (emitFlags) emitUpdateNZ(p, JitPpc::R3);
                outCycles = 1;
                return true;
            case 0x8: // TST Rd, Rs
                if (emitFlags) {
                    emitLoadGuestReg(p, JitPpc::R4, rd);
                    emitLoadGuestReg(p, JitPpc::R5, rs);
                    *p++ = JitPpc::and_(JitPpc::R3, JitPpc::R4, JitPpc::R5);
                    emitUpdateNZ(p, JitPpc::R3);
                }
                outCycles = 1;
                return true;
            case 0x9: // NEG Rd, Rs (Rd = 0 - Rs)
                *p++ = JitPpc::li(JitPpc::R4, 0);
                emitLoadGuestReg(p, JitPpc::R5, rs);
                if (emitFlags) {
                    *p++ = JitPpc::subfc(JitPpc::R3, JitPpc::R5, JitPpc::R4);
                    emitStoreGuestReg(p, JitPpc::R3, rd);
                    emitUpdateSubNZCV(p);
                } else {
                    *p++ = JitPpc::subf(JitPpc::R3, JitPpc::R5, JitPpc::R4);
                    emitStoreGuestReg(p, JitPpc::R3, rd);
                }
                outCycles = 1;
                return true;
            case 0xA: // CMP Rd, Rs
                if (emitFlags) {
                    emitLoadGuestReg(p, JitPpc::R4, rd);
                    emitLoadGuestReg(p, JitPpc::R5, rs);
                    *p++ = JitPpc::subfc(JitPpc::R3, JitPpc::R5, JitPpc::R4);
                    emitUpdateSubNZCV(p);
                }
                outCycles = 1;
                return true;
            case 0xB: // CMN Rd, Rs
                if (emitFlags) {
                    emitLoadGuestReg(p, JitPpc::R4, rd);
                    emitLoadGuestReg(p, JitPpc::R5, rs);
                    *p++ = JitPpc::addc(JitPpc::R3, JitPpc::R4, JitPpc::R5);
                    emitUpdateAddNZCV(p);
                }
                outCycles = 1;
                return true;
            case 0xC: // ORR Rd, Rs
                emitLoadGuestReg(p, JitPpc::R4, rd);
                emitLoadGuestReg(p, JitPpc::R5, rs);
                *p++ = JitPpc::or_(JitPpc::R3, JitPpc::R4, JitPpc::R5);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                if (emitFlags) emitUpdateNZ(p, JitPpc::R3);
                outCycles = 1;
                return true;
            case 0xE: // BIC Rd, Rs
                emitLoadGuestReg(p, JitPpc::R4, rd);
                emitLoadGuestReg(p, JitPpc::R5, rs);
                *p++ = JitPpc::andc(JitPpc::R3, JitPpc::R4, JitPpc::R5);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                if (emitFlags) emitUpdateNZ(p, JitPpc::R3);
                outCycles = 1;
                return true;
            case 0xF: // MVN Rd, Rs
                emitLoadGuestReg(p, JitPpc::R5, rs);
                *p++ = JitPpc::not_(JitPpc::R3, JitPpc::R5);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                if (emitFlags) emitUpdateNZ(p, JitPpc::R3);
                outCycles = 1;
                return true;
            default:
                return false;
            }
        }
        if ((opcode & 0xFC00u) == 0x4400u) { // 010001: Special data processing (high regs)
            uint8_t op = (uint8_t)((opcode >> 8) & 0x3);
            uint8_t rd = (uint8_t)(((opcode >> 4) & 0x8) | (opcode & 0x7));
            uint8_t rs = (uint8_t)((opcode >> 3) & 0xF);
            if (op == 0 && rd != 15) { // ADD Rd, Rs (no flags)
                emitLoadGuestReg(p, JitPpc::R4, rd);
                emitLoadGuestReg(p, JitPpc::R5, rs);
                *p++ = JitPpc::add(JitPpc::R3, JitPpc::R4, JitPpc::R5);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                outCycles = 1;
                return true;
            }
            if (op == 1) { // CMP Rd, Rs
                if (emitFlags) {
                    emitLoadGuestReg(p, JitPpc::R4, rd);
                    emitLoadGuestReg(p, JitPpc::R5, rs);
                    *p++ = JitPpc::subfc(JitPpc::R3, JitPpc::R5, JitPpc::R4);
                    emitUpdateSubNZCV(p);
                }
                outCycles = 1;
                return true;
            }
            if (op == 2 && rd != 15) { // MOV Rd, Rs (no flags)
                emitLoadGuestReg(p, JitPpc::R3, rs);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                outCycles = 1;
                return true;
            }
        }
        return false;
    }

    case 5: { // 101xx: ADD Rd, PC/SP, #imm8 or ADD SP, #simm7
        if ((opcode & 0xF800u) == 0xA000u) { // ADD Rd, PC, #imm8
            uint8_t rd = (uint8_t)((opcode >> 8) & 0x7);
            uint16_t imm = (uint16_t)((opcode & 0xFFu) << 2);
            emitLoadGuestReg(p, JitPpc::R4, 15);
            *p++ = JitPpc::rlwinm(JitPpc::R4, JitPpc::R4, 0, 0, 29); // & ~3
            *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R4, (int16_t)imm);
            emitStoreGuestReg(p, JitPpc::R3, rd);
            outCycles = 1;
            return true;
        }
        if ((opcode & 0xF800u) == 0xA800u) { // ADD Rd, SP, #imm8
            uint8_t rd = (uint8_t)((opcode >> 8) & 0x7);
            uint16_t imm = (uint16_t)((opcode & 0xFFu) << 2);
            emitLoadGuestReg(p, JitPpc::R4, 13);
            *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R4, (int16_t)imm);
            emitStoreGuestReg(p, JitPpc::R3, rd);
            outCycles = 1;
            return true;
        }
        if ((opcode & 0xFF00u) == 0xB000u) { // ADD SP, #simm7
            int16_t simm = (int16_t)(((opcode & 0x80u) ? -(int32_t)(opcode & 0x7Fu) : (int32_t)(opcode & 0x7Fu)) << 2);
            emitLoadGuestReg(p, JitPpc::R4, 13);
            *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R4, simm);
            emitStoreGuestReg(p, JitPpc::R3, 13);
            outCycles = 1;
            return true;
        }
        return false;
    }

    case 7: {
        if ((opcode & 0xF800u) == 0xF000u) { // BL setup: LR = PC + (sign_ext11 << 12)
            int32_t off = ((int32_t)(int16_t)(opcode << 5) >> 4) << 11;
            emitLoadGuestReg(p, JitPpc::R4, 15);
            emitLoadImm32(p, JitPpc::R5, (uint32_t)off);
            *p++ = JitPpc::add(JitPpc::R3, JitPpc::R4, JitPpc::R5);
            emitStoreGuestReg(p, JitPpc::R3, 14);
            outCycles = 1;
            return true;
        }
        return false;
    }

    default:
        return false;
    }
}

// Native LDR/LDRB/LDRH (pre-indexed, no writeback). Fast path reads guest memory directly through the
// CPU read page map (same semantics as Memory::read<T>); on a missing page map entry it calls the
// interpreter handler for this opcode, which performs the exact original access. The handler's cycle
// return is (arm7 << 1) + 1 for every non-PC destination, so the static cycle count is unchanged.
// Guest registers and flags live in memory between instructions, so the helper call needs no state
// transfer; the block epilogue restores LR (clobbered by bctrl).
// Helper slow paths read R15 (open bus / BIOS reads depend on it). Blocks only sync R15 lazily, so set the
// exact value of the current instruction around the helper call. Save: R28 = synced R15 (restored after).
void ArmPpcJit::emitHelperR15Sync(uint32_t *&p, bool enter) {
    const int16_t offR15   = (int16_t)(offsetof(Interpreter, registersUsr) + 15 * 4);
    const int16_t offPcDat = (int16_t)offsetof(Interpreter, pcData);
    if (enter) {
        *p++ = JitPpc::li(JitPpc::R30, 1); // Slow-path MMIO/SMC exit: finish this iteration, then return to scheduler
        *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R31, offR15);
        *p++ = JitPpc::stw(JitPpc::R0, JitPpc::R1, 8);
        *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R31, offPcDat);
        *p++ = JitPpc::stw(JitPpc::R0, JitPpc::R1, 12);
        emitLoadImm32(p, JitPpc::R0, s_emitInstrR15);
        *p++ = JitPpc::stw(JitPpc::R0, JitPpc::R31, offR15);
    } else {
        *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 8);
        *p++ = JitPpc::stw(JitPpc::R0, JitPpc::R31, offR15);
        *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 12);
        *p++ = JitPpc::stw(JitPpc::R0, JitPpc::R31, offPcDat);
    }
}

// Inline guarded fast-reads for hot read-only / poll MMIO halfword registers (DISPSTAT, VCOUNT, KEYINPUT, IPCSYNC)
// used by BOTH ARM and Thumb native halfword loads with zero C++ helper overhead!
void ArmPpcJit::emitGuardedPollHalfLoad(uint32_t *&p, Interpreter *cpu, uint8_t rd,
                                        void (*storeFn)(uint32_t *&, uint8_t, uint8_t),
                                        uint32_t **outJumpSlots, int &jumpCount) {
    // 1. Combined (addr & ~3) == 0x04000004 covers BOTH REG_DISPSTAT (0x04000004) and REG_VCOUNT (0x04000006)!
    *p++ = JitPpc::rlwinm(JitPpc::R4, JitPpc::R3, 0, 0, 29);
    emitLoadImm32(p, JitPpc::R5, 0x04000004u);
    *p++ = JitPpc::cmplw(0, JitPpc::R4, JitPpc::R5);
    uint32_t *bNotGpu = p++;
    *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, (int16_t)offsetof(Interpreter, core));
    *p++ = JitPpc::andi_rc(JitPpc::R0, JitPpc::R3, 2u);
    uint32_t *bIsDisp = p++;
    const uint32_t offVcount = (uint32_t)(offsetof(Core, gpu) + offsetof(Gpu, vCount));
    emitLoadImm32(p, JitPpc::R5, offVcount);
    uint32_t *bGpuLoad = p++;
    *bIsDisp = JitPpc::beq((int16_t)((p - bIsDisp) * 4));
    const uint32_t offDispstat = (uint32_t)(offsetof(Core, gpu) + offsetof(Gpu, dispStat) + (cpu->arm7 ? sizeof(uint16_t) : 0u));
    emitLoadImm32(p, JitPpc::R5, offDispstat);
    *bGpuLoad = JitPpc::b((int32_t)((p - bGpuLoad) * 4));
    *p++ = JitPpc::add(JitPpc::R4, JitPpc::R4, JitPpc::R5);
    *p++ = JitPpc::lhz(JitPpc::R8, JitPpc::R4, 0);
    if (cpu->arm7) {
        *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 3, 28, 28);
        *p++ = JitPpc::subfic(JitPpc::R9, JitPpc::R9, 32);
        *p++ = JitPpc::rlwnm(JitPpc::R8, JitPpc::R8, JitPpc::R9, 0, 31);
    }
    storeFn(p, JitPpc::R8, rd);
    outJumpSlots[jumpCount++] = p++;
    *bNotGpu = JitPpc::bne((int16_t)((p - bNotGpu) * 4));

    // 2. REG_IPCSYNC (0x04000180) on BOTH ARM9 and ARM7
    *p++ = JitPpc::rlwinm(JitPpc::R4, JitPpc::R3, 0, 0, 30);
    emitLoadImm32(p, JitPpc::R5, 0x04000180u);
    *p++ = JitPpc::cmplw(0, JitPpc::R4, JitPpc::R5);
    uint32_t *bNotIpc = p++;
    *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, (int16_t)offsetof(Interpreter, core));
    const uint32_t offIpcSync = (uint32_t)(offsetof(Core, ipc) + offsetof(Ipc, ipcSync) + (cpu->arm7 ? sizeof(uint16_t) : 0u));
    emitLoadImm32(p, JitPpc::R5, offIpcSync);
    *p++ = JitPpc::add(JitPpc::R4, JitPpc::R4, JitPpc::R5);
    *p++ = JitPpc::lhz(JitPpc::R8, JitPpc::R4, 0);
    if (cpu->arm7) {
        *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 3, 28, 28);
        *p++ = JitPpc::subfic(JitPpc::R9, JitPpc::R9, 32);
        *p++ = JitPpc::rlwnm(JitPpc::R8, JitPpc::R8, JitPpc::R9, 0, 31);
    }
    storeFn(p, JitPpc::R8, rd);
    outJumpSlots[jumpCount++] = p++;
    *bNotIpc = JitPpc::bne((int16_t)((p - bNotIpc) * 4));

    // 3. REG_KEYINPUT (0x04000130)
    *p++ = JitPpc::rlwinm(JitPpc::R4, JitPpc::R3, 0, 0, 30);
    emitLoadImm32(p, JitPpc::R5, 0x04000130u);
    *p++ = JitPpc::cmplw(0, JitPpc::R4, JitPpc::R5);
    uint32_t *bNotKey = p++;
    *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, (int16_t)offsetof(Interpreter, core));
    const uint32_t offKeyinput = (uint32_t)(offsetof(Core, input) + offsetof(Input, keyInput));
    emitLoadImm32(p, JitPpc::R5, offKeyinput);
    *p++ = JitPpc::add(JitPpc::R4, JitPpc::R4, JitPpc::R5);
    *p++ = JitPpc::lhz(JitPpc::R8, JitPpc::R4, 0);
    if (cpu->arm7) {
        *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 3, 28, 28);
        *p++ = JitPpc::subfic(JitPpc::R9, JitPpc::R9, 32);
        *p++ = JitPpc::rlwnm(JitPpc::R8, JitPpc::R8, JitPpc::R9, 0, 31);
    }
    storeFn(p, JitPpc::R8, rd);
    outJumpSlots[jumpCount++] = p++;
    *bNotKey = JitPpc::bne((int16_t)((p - bNotKey) * 4));
}

bool ArmPpcJit::emitInlineBranchArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, uint32_t instrPC,
                                    int32_t totalDelta, int32_t prefixCycles, bool helperInBlock, uint32_t *loopTop) {
    (void)cpu;
    const uint8_t cond = (uint8_t)(opcode >> 28);
    if (cond == 0xFu) return false;
    if ((opcode & 0x0E000000u) != 0x0A000000u) return false;
    const uint32_t brAddr = instrPC + (uint32_t)totalDelta;
    const uint32_t r15 = brAddr + 8u;
    const int32_t offset = (int32_t)(opcode << 8) >> 6;
    const uint32_t target = (r15 + (uint32_t)offset) & ~3u;
    const uint32_t newR15 = target + 4u;
    if ((((target ^ instrPC) | (newR15 ^ instrPC)) >> 12) != 0) return false;
    const bool link = (opcode & (1u << 24)) != 0;
    const int32_t takenDelta = (int32_t)newR15 - (int32_t)(instrPC + 8u);
    if (takenDelta > 0x7FF || takenDelta < -0x800) return false;

    const int16_t offR15   = (int16_t)(offsetof(Interpreter, registersUsr) + 15 * 4);
    const int16_t offRegs  = (int16_t)(offsetof(Interpreter, registers) + 14 * 4);
    const int16_t offPcDat = (int16_t)offsetof(Interpreter, pcData);
    const int16_t offPipe0 = (int16_t)offsetof(Interpreter, pipeline);
    const int16_t offPipe1 = (int16_t)(offsetof(Interpreter, pipeline) + 4);
    const int16_t offCpsr  = (int16_t)offsetof(Interpreter, cpsr);
    const int16_t loopStep = (int16_t)(prefixCycles + 3);

    auto emitExit = [&](int32_t delta, uint32_t r15Abs, int32_t baseCycles, bool addLoopCount) {
        emitLoadImm32(p, JitPpc::R5, r15Abs);
        *p++ = JitPpc::stw(JitPpc::R5, JitPpc::R31, offR15);
        *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, offPcDat);
        if (delta != 0) *p++ = JitPpc::addi(JitPpc::R4, JitPpc::R4, (int16_t)delta);
        *p++ = JitPpc::stw(JitPpc::R4, JitPpc::R31, offPcDat);
        *p++ = JitPpc::addi(JitPpc::R5, JitPpc::R4, -4);
        *p++ = JitPpc::lwbrx(JitPpc::R6, 0, JitPpc::R5);
        *p++ = JitPpc::lwbrx(JitPpc::R7, 0, JitPpc::R4);
        *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offPipe0);
        *p++ = JitPpc::stw(JitPpc::R7, JitPpc::R31, offPipe1);
        if (addLoopCount) {
            *p++ = JitPpc::mulli(JitPpc::R3, JitPpc::R29, loopStep);
            if (baseCycles != 0) *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)baseCycles);
        } else {
            *p++ = JitPpc::li(JitPpc::R3, (int16_t)baseCycles);
        }
        if (helperInBlock) {
            *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 36);
            *p++ = JitPpc::mtlr(JitPpc::R0);
        }
        *p++ = JitPpc::lmw(JitPpc::R28, JitPpc::R1, 16);
        *p++ = JitPpc::addi(JitPpc::R1, JitPpc::R1, 32);
        *p++ = JitPpc::blr();
    };

    if (cond == 0xEu) {
        if (link) {
            *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, offRegs);
            emitLoadImm32(p, JitPpc::R5, brAddr + 4u);
            *p++ = JitPpc::stw(JitPpc::R5, JitPpc::R4, 0);
        }
        if (loopTop) {
            *p++ = JitPpc::addi(JitPpc::R29, JitPpc::R29, 1);
            *p++ = JitPpc::addic_rc(JitPpc::R30, JitPpc::R30, -1);
            int32_t backBytes = (int32_t)(loopTop - p) * 4;
            assert(backBytes >= -32768 && backBytes <= 32764);
            *p++ = JitPpc::bne((int16_t)backBytes);
            emitExit(takenDelta, newR15, 0, true);
            return true;
        }
        emitExit(takenDelta, newR15, prefixCycles + 3, false);
        return true;
    }

    *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
    *p++ = JitPpc::rlwinm(JitPpc::R6, JitPpc::R6, 4, 28, 31);
    *p++ = JitPpc::addi(JitPpc::R6, JitPpc::R6, (int16_t)(cond << 4));
    emitLoadImm32(p, JitPpc::R7, (uint32_t)(uintptr_t)Interpreter::condition);
    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R7, JitPpc::R6);
    *p++ = JitPpc::lbz(JitPpc::R6, JitPpc::R7, 0);
    *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
    uint32_t *bFalse = p++;

    if (link) {
        *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, offRegs);
        emitLoadImm32(p, JitPpc::R5, brAddr + 4u);
        *p++ = JitPpc::stw(JitPpc::R5, JitPpc::R4, 0);
    }
    if (loopTop) {
        *p++ = JitPpc::addi(JitPpc::R29, JitPpc::R29, 1);
        *p++ = JitPpc::addic_rc(JitPpc::R30, JitPpc::R30, -1);
        int32_t backBytes = (int32_t)(loopTop - p) * 4;
        assert(backBytes >= -32768 && backBytes <= 32764);
        *p++ = JitPpc::bne((int16_t)backBytes);
        emitExit(takenDelta, newR15, 0, true);
        *bFalse = JitPpc::beq((int16_t)((p - bFalse) * 4));
        emitExit((int32_t)totalDelta, brAddr + 8u, prefixCycles + 1, true);
        return true;
    }

    emitExit(takenDelta, newR15, prefixCycles + 3, false);
    *bFalse = JitPpc::beq((int16_t)((p - bFalse) * 4));
    emitExit((int32_t)totalDelta, brAddr + 8u, prefixCycles + 1, false);
    return true;
}

bool ArmPpcJit::emitInlineBranchThumb(uint32_t *&p, Interpreter *cpu, uint16_t opcode, uint32_t instrPC,
                                      int32_t totalDelta, int32_t prefixCycles, bool helperInBlock, uint32_t *loopTop) {
    (void)cpu;
    bool isCond = ((opcode & 0xF000u) == 0xD000u && (opcode & 0x0F00u) < 0x0E00u);
    bool isUncond = ((opcode & 0xF800u) == 0xE000u);
    if (!isCond && !isUncond) return false;

    const uint32_t brAddr = instrPC + (uint32_t)totalDelta;
    const uint32_t r15 = brAddr + 4u;
    const int32_t offset = isCond ? ((int32_t)(int8_t)(opcode & 0xFFu) << 1)
                                  : (((int32_t)(int16_t)(opcode << 5)) >> 4);
    const uint32_t target = (r15 + (uint32_t)offset) & ~1u;
    const uint32_t newR15 = target + 2u;
    if ((((target ^ instrPC) | (newR15 ^ instrPC)) >> 12) != 0) return false;
    const int32_t takenDelta = (int32_t)newR15 - (int32_t)(instrPC + 4u);
    if (takenDelta > 0x7FF || takenDelta < -0x800) return false;

    const int16_t offR15   = (int16_t)(offsetof(Interpreter, registersUsr) + 15 * 4);
    const int16_t offPcDat = (int16_t)offsetof(Interpreter, pcData);
    const int16_t offPipe0 = (int16_t)offsetof(Interpreter, pipeline);
    const int16_t offPipe1 = (int16_t)(offsetof(Interpreter, pipeline) + 4);
    const int16_t offCpsr  = (int16_t)offsetof(Interpreter, cpsr);
    const int16_t loopStep = (int16_t)(prefixCycles + 3);

    auto emitExit = [&](int32_t delta, uint32_t r15Abs, int32_t baseCycles, bool addLoopCount) {
        emitLoadImm32(p, JitPpc::R5, r15Abs);
        *p++ = JitPpc::stw(JitPpc::R5, JitPpc::R31, offR15);
        *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, offPcDat);
        if (delta != 0) *p++ = JitPpc::addi(JitPpc::R4, JitPpc::R4, (int16_t)delta);
        *p++ = JitPpc::stw(JitPpc::R4, JitPpc::R31, offPcDat);
        *p++ = JitPpc::addi(JitPpc::R5, JitPpc::R4, -2);
        *p++ = JitPpc::lhbrx(JitPpc::R6, 0, JitPpc::R5);
        *p++ = JitPpc::lhbrx(JitPpc::R7, 0, JitPpc::R4);
        *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offPipe0);
        *p++ = JitPpc::stw(JitPpc::R7, JitPpc::R31, offPipe1);
        if (addLoopCount) {
            *p++ = JitPpc::mulli(JitPpc::R3, JitPpc::R29, loopStep);
            if (baseCycles != 0) *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)baseCycles);
        } else {
            *p++ = JitPpc::li(JitPpc::R3, (int16_t)baseCycles);
        }
        if (helperInBlock) {
            *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 36);
            *p++ = JitPpc::mtlr(JitPpc::R0);
        }
        *p++ = JitPpc::lmw(JitPpc::R28, JitPpc::R1, 16);
        *p++ = JitPpc::addi(JitPpc::R1, JitPpc::R1, 32);
        *p++ = JitPpc::blr();
    };

    if (isUncond) {
        if (loopTop) {
            *p++ = JitPpc::addi(JitPpc::R29, JitPpc::R29, 1);
            *p++ = JitPpc::addic_rc(JitPpc::R30, JitPpc::R30, -1);
            int32_t backBytes = (int32_t)(loopTop - p) * 4;
            assert(backBytes >= -32768 && backBytes <= 32764);
            *p++ = JitPpc::bne((int16_t)backBytes);
            emitExit(takenDelta, newR15, 0, true);
            return true;
        }
        emitExit(takenDelta, newR15, prefixCycles + 3, false);
        return true;
    }

    uint8_t cond = (uint8_t)((opcode >> 8) & 0xF);
    *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
    *p++ = JitPpc::rlwinm(JitPpc::R6, JitPpc::R6, 4, 28, 31);
    *p++ = JitPpc::addi(JitPpc::R6, JitPpc::R6, (int16_t)(cond << 4));
    emitLoadImm32(p, JitPpc::R7, (uint32_t)(uintptr_t)Interpreter::condition);
    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R7, JitPpc::R6);
    *p++ = JitPpc::lbz(JitPpc::R6, JitPpc::R7, 0);
    *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
    uint32_t *bFalse = p++;

    if (loopTop) {
        *p++ = JitPpc::addi(JitPpc::R29, JitPpc::R29, 1);
        *p++ = JitPpc::addic_rc(JitPpc::R30, JitPpc::R30, -1);
        int32_t backBytes = (int32_t)(loopTop - p) * 4;
        assert(backBytes >= -32768 && backBytes <= 32764);
        *p++ = JitPpc::bne((int16_t)backBytes);
        emitExit(takenDelta, newR15, 0, true);
        *bFalse = JitPpc::beq((int16_t)((p - bFalse) * 4));
        emitExit((int32_t)totalDelta, brAddr + 4u, prefixCycles + 1, true);
        return true;
    }

    emitExit(takenDelta, newR15, prefixCycles + 3, false);
    *bFalse = JitPpc::beq((int16_t)((p - bFalse) * 4));
    emitExit((int32_t)totalDelta, brAddr + 4u, prefixCycles + 1, false);
    return true;
}

bool ArmPpcJit::emitInlineBxArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, uint32_t instrPC,
                                int32_t totalDelta, int32_t prefixCycles, bool helperInBlock) {
    const uint8_t cond = (uint8_t)(opcode >> 28);
    if (cond == 0xFu) return false;
    bool isBx = (opcode & 0x0FFFFFF0u) == 0x012FFF10u;
    bool isBlx = !cpu->arm7 && (opcode & 0x0FFFFFF0u) == 0x012FFF30u;
    if (!isBx && !isBlx) return false;

    const uint8_t rm = (uint8_t)(opcode & 0xFu);
    const uint32_t brAddr = instrPC + (uint32_t)totalDelta;
    const int16_t offR15   = (int16_t)(offsetof(Interpreter, registersUsr) + 15 * 4);
    const int16_t offPcDat = (int16_t)offsetof(Interpreter, pcData);
    const int16_t offPipe0 = (int16_t)offsetof(Interpreter, pipeline);
    const int16_t offPipe1 = (int16_t)(offsetof(Interpreter, pipeline) + 4);
    const int16_t offCpsr  = (int16_t)offsetof(Interpreter, cpsr);
    const int16_t offCore  = (int16_t)offsetof(Interpreter, core);
    const uint32_t offReadMap = (uint32_t)(offsetof(Core, memory) + (cpu->arm7 ? offsetof(Memory, readMap7) : offsetof(Memory, readMap9A)));

    auto emitRet = [&](int cycles) {
        *p++ = JitPpc::li(JitPpc::R3, (int16_t)cycles);
        if (helperInBlock) {
            *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 36);
            *p++ = JitPpc::mtlr(JitPpc::R0);
        }
        *p++ = JitPpc::lmw(JitPpc::R28, JitPpc::R1, 16);
        *p++ = JitPpc::addi(JitPpc::R1, JitPpc::R1, 32);
        *p++ = JitPpc::blr();
    };

    uint32_t *bNotTaken = nullptr;
    if (cond != 0xEu) {
        *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
        *p++ = JitPpc::rlwinm(JitPpc::R6, JitPpc::R6, 4, 28, 31);
        *p++ = JitPpc::addi(JitPpc::R6, JitPpc::R6, (int16_t)(cond << 4));
        emitLoadImm32(p, JitPpc::R7, (uint32_t)(uintptr_t)Interpreter::condition);
        *p++ = JitPpc::add(JitPpc::R7, JitPpc::R7, JitPpc::R6);
        *p++ = JitPpc::lbz(JitPpc::R6, JitPpc::R7, 0);
        *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
        bNotTaken = p++;
    }

    // Load target BEFORE writing LR (Rm may be R14!)
    if (rm == 15) emitLoadImm32(p, JitPpc::R8, brAddr + 8u);
    else emitLoadGuestReg(p, JitPpc::R8, rm);
    if (isBlx) {
        emitLoadImm32(p, JitPpc::R5, brAddr + 4u);
        emitStoreGuestReg(p, JitPpc::R5, 14);
    }

    // Check mapped page fast refill for both ARM (bit0==0) and Thumb (bit0==1) targets!
    uint32_t *slowFix[6];
    uint8_t slowKind[6];
    int slowCnt = 0;
    auto fixSlow = [&](uint8_t k) { slowFix[slowCnt] = p++; slowKind[slowCnt] = k; slowCnt++; };

    *p++ = JitPpc::rlwinm(JitPpc::R10, JitPpc::R8, 0, 20, 30); // target & 0xFFE
    *p++ = JitPpc::cmpwi(0, JitPpc::R10, 8);
    fixSlow(2); // blt slow
    *p++ = JitPpc::cmplwi(0, JitPpc::R10, 0xFA0);
    fixSlow(3); // bgt slow

    *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R8, 22, 10, 29); // (address >> 12) * 4
    // The Core and its inline page tables remain stable until JIT reset.
    emitLoadImm32(p, JitPpc::R4, (uint32_t)(uintptr_t)cpu->core + offReadMap);
    *p++ = JitPpc::lwzx(JitPpc::R6, JitPpc::R4, JitPpc::R5);
    *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
    fixSlow(0); // beq slow

    // Update CPSR.T bit from bit 0 of target
    *p++ = JitPpc::lwz(JitPpc::R9, JitPpc::R31, offCpsr);
    emitInsertBit(p, JitPpc::R9, JitPpc::R8, 5, 0);
    *p++ = JitPpc::stw(JitPpc::R9, JitPpc::R31, offCpsr);

    *p++ = JitPpc::andi_rc(JitPpc::R0, JitPpc::R8, 1u);
    uint32_t *bThumbTarget = p++;

    // ARM return target
    *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R8, 0, 0, 29);   // A = target & ~3
    *p++ = JitPpc::rlwinm(JitPpc::R10, JitPpc::R9, 0, 20, 29); // A & 0xFFC
    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R6, JitPpc::R10);
    *p++ = JitPpc::lwbrx(JitPpc::R11, 0, JitPpc::R7);
    *p++ = JitPpc::addi(JitPpc::R7, JitPpc::R7, 4);
    *p++ = JitPpc::lwbrx(JitPpc::R12, 0, JitPpc::R7);
    *p++ = JitPpc::stw(JitPpc::R11, JitPpc::R31, offPipe0);
    *p++ = JitPpc::stw(JitPpc::R12, JitPpc::R31, offPipe1);
    *p++ = JitPpc::stw(JitPpc::R7, JitPpc::R31, offPcDat);
    *p++ = JitPpc::addi(JitPpc::R9, JitPpc::R9, 4);
    *p++ = JitPpc::stw(JitPpc::R9, JitPpc::R31, offR15);
    emitRet(prefixCycles + 3);

    // Thumb return target
    *bThumbTarget = JitPpc::bne((int16_t)((p - bThumbTarget) * 4));
    *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R8, 0, 0, 30);   // A = target & ~1
    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R6, JitPpc::R10);
    *p++ = JitPpc::lhbrx(JitPpc::R11, 0, JitPpc::R7);
    *p++ = JitPpc::addi(JitPpc::R7, JitPpc::R7, 2);
    *p++ = JitPpc::lhbrx(JitPpc::R12, 0, JitPpc::R7);
    *p++ = JitPpc::stw(JitPpc::R11, JitPpc::R31, offPipe0);
    *p++ = JitPpc::stw(JitPpc::R12, JitPpc::R31, offPipe1);
    *p++ = JitPpc::stw(JitPpc::R7, JitPpc::R31, offPcDat);
    *p++ = JitPpc::addi(JitPpc::R9, JitPpc::R9, 2);
    *p++ = JitPpc::stw(JitPpc::R9, JitPpc::R31, offR15);
    emitRet(prefixCycles + 3);

    // Slow fallback: flushPipeline via C helper (note LR already set if isBlx; write target & call flushPipeline via execBranchArm with safe non-link BX opcode!)
    uint32_t *slow = p;
    for (int i = 0; i < slowCnt; i++) {
        int16_t d = (int16_t)((slow - slowFix[i]) * 4);
        if (slowKind[i] == 0) *slowFix[i] = JitPpc::beq(d);
        else if (slowKind[i] == 2) *slowFix[i] = JitPpc::blt(d);
        else *slowFix[i] = JitPpc::bgt(d);
    }
    *p++ = JitPpc::lwz(JitPpc::R9, JitPpc::R31, offCpsr);
    emitInsertBit(p, JitPpc::R9, JitPpc::R8, 5, 0);
    *p++ = JitPpc::stw(JitPpc::R9, JitPpc::R31, offCpsr);
    *p++ = JitPpc::stw(JitPpc::R8, JitPpc::R31, offR15);
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    *p++ = JitPpc::li(JitPpc::R4, 0x0F); // bxReg with Rm = R15 already written to registersUsr[15]!
    emitLoadImm32(p, JitPpc::R12, (uint32_t)resolveArmHandler(0xE12FFF1Fu));
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctrl();
    *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 36);
    *p++ = JitPpc::mtlr(JitPpc::R0);
    emitRet(prefixCycles + 3);

    if (bNotTaken) {
        *bNotTaken = JitPpc::beq((int16_t)((p - bNotTaken) * 4));
        emitLoadImm32(p, JitPpc::R5, brAddr + 8u);
        *p++ = JitPpc::stw(JitPpc::R5, JitPpc::R31, offR15);
        *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, offPcDat);
        if (totalDelta != 0) *p++ = JitPpc::addi(JitPpc::R4, JitPpc::R4, (int16_t)totalDelta);
        *p++ = JitPpc::stw(JitPpc::R4, JitPpc::R31, offPcDat);
        *p++ = JitPpc::addi(JitPpc::R5, JitPpc::R4, -4);
        *p++ = JitPpc::lwbrx(JitPpc::R6, 0, JitPpc::R5);
        *p++ = JitPpc::lwbrx(JitPpc::R7, 0, JitPpc::R4);
        *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offPipe0);
        *p++ = JitPpc::stw(JitPpc::R7, JitPpc::R31, offPipe1);
        emitRet(prefixCycles + 1);
    }
    return true;
}

bool ArmPpcJit::emitInlineBxThumb(uint32_t *&p, Interpreter *cpu, uint16_t opcode, uint32_t instrPC,
                                  int32_t totalDelta, int32_t prefixCycles, bool helperInBlock) {
    if ((opcode & 0xFF00u) != 0x4700u) return false;
    bool isBlx = (opcode & 0x0080u) != 0u;
    if (isBlx && cpu->arm7) return false;

    const uint8_t rs = (uint8_t)((opcode >> 3) & 0xFu);
    const uint32_t brAddr = instrPC + (uint32_t)totalDelta;
    const int16_t offR15   = (int16_t)(offsetof(Interpreter, registersUsr) + 15 * 4);
    const int16_t offPcDat = (int16_t)offsetof(Interpreter, pcData);
    const int16_t offPipe0 = (int16_t)offsetof(Interpreter, pipeline);
    const int16_t offPipe1 = (int16_t)(offsetof(Interpreter, pipeline) + 4);
    const int16_t offCpsr  = (int16_t)offsetof(Interpreter, cpsr);
    const int16_t offCore  = (int16_t)offsetof(Interpreter, core);
    const uint32_t offReadMap = (uint32_t)(offsetof(Core, memory) + (cpu->arm7 ? offsetof(Memory, readMap7) : offsetof(Memory, readMap9A)));

    auto emitRet = [&](int cycles) {
        *p++ = JitPpc::li(JitPpc::R3, (int16_t)cycles);
        if (helperInBlock) {
            *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 36);
            *p++ = JitPpc::mtlr(JitPpc::R0);
        }
        *p++ = JitPpc::lmw(JitPpc::R28, JitPpc::R1, 16);
        *p++ = JitPpc::addi(JitPpc::R1, JitPpc::R1, 32);
        *p++ = JitPpc::blr();
    };

    if (rs == 15) emitLoadImm32(p, JitPpc::R8, (brAddr + 4u) & ~3u); // Thumb BX PC reads aligned PC!
    else emitLoadGuestReg(p, JitPpc::R8, rs);
    if (isBlx) {
        emitLoadImm32(p, JitPpc::R5, (brAddr + 2u) | 1u);
        emitStoreGuestReg(p, JitPpc::R5, 14);
    }

    uint32_t *slowFix[6];
    uint8_t slowKind[6];
    int slowCnt = 0;
    auto fixSlow = [&](uint8_t k) { slowFix[slowCnt] = p++; slowKind[slowCnt] = k; slowCnt++; };

    *p++ = JitPpc::rlwinm(JitPpc::R10, JitPpc::R8, 0, 20, 30);
    *p++ = JitPpc::cmpwi(0, JitPpc::R10, 8);
    fixSlow(2);
    *p++ = JitPpc::cmplwi(0, JitPpc::R10, 0xFA0);
    fixSlow(3);

    *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R8, 22, 10, 29); // (address >> 12) * 4
    // The Core and its inline page tables remain stable until JIT reset.
    emitLoadImm32(p, JitPpc::R4, (uint32_t)(uintptr_t)cpu->core + offReadMap);
    *p++ = JitPpc::lwzx(JitPpc::R6, JitPpc::R4, JitPpc::R5);
    *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
    fixSlow(0);

    *p++ = JitPpc::lwz(JitPpc::R9, JitPpc::R31, offCpsr);
    emitInsertBit(p, JitPpc::R9, JitPpc::R8, 5, 0);
    *p++ = JitPpc::stw(JitPpc::R9, JitPpc::R31, offCpsr);

    *p++ = JitPpc::andi_rc(JitPpc::R0, JitPpc::R8, 1u);
    uint32_t *bArmTarget = p++;

    // Thumb return target (most common case in Thumb!)
    *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R8, 0, 0, 30);
    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R6, JitPpc::R10);
    *p++ = JitPpc::lhbrx(JitPpc::R11, 0, JitPpc::R7);
    *p++ = JitPpc::addi(JitPpc::R7, JitPpc::R7, 2);
    *p++ = JitPpc::lhbrx(JitPpc::R12, 0, JitPpc::R7);
    *p++ = JitPpc::stw(JitPpc::R11, JitPpc::R31, offPipe0);
    *p++ = JitPpc::stw(JitPpc::R12, JitPpc::R31, offPipe1);
    *p++ = JitPpc::stw(JitPpc::R7, JitPpc::R31, offPcDat);
    *p++ = JitPpc::addi(JitPpc::R9, JitPpc::R9, 2);
    *p++ = JitPpc::stw(JitPpc::R9, JitPpc::R31, offR15);
    emitRet(prefixCycles + 3);

    // ARM return target
    *bArmTarget = JitPpc::beq((int16_t)((p - bArmTarget) * 4));
    *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R8, 0, 0, 29);
    *p++ = JitPpc::rlwinm(JitPpc::R10, JitPpc::R9, 0, 20, 29);
    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R6, JitPpc::R10);
    *p++ = JitPpc::lwbrx(JitPpc::R11, 0, JitPpc::R7);
    *p++ = JitPpc::addi(JitPpc::R7, JitPpc::R7, 4);
    *p++ = JitPpc::lwbrx(JitPpc::R12, 0, JitPpc::R7);
    *p++ = JitPpc::stw(JitPpc::R11, JitPpc::R31, offPipe0);
    *p++ = JitPpc::stw(JitPpc::R12, JitPpc::R31, offPipe1);
    *p++ = JitPpc::stw(JitPpc::R7, JitPpc::R31, offPcDat);
    *p++ = JitPpc::addi(JitPpc::R9, JitPpc::R9, 4);
    *p++ = JitPpc::stw(JitPpc::R9, JitPpc::R31, offR15);
    emitRet(prefixCycles + 3);

    uint32_t *slow = p;
    for (int i = 0; i < slowCnt; i++) {
        int16_t d = (int16_t)((slow - slowFix[i]) * 4);
        if (slowKind[i] == 0) *slowFix[i] = JitPpc::beq(d);
        else if (slowKind[i] == 2) *slowFix[i] = JitPpc::blt(d);
        else *slowFix[i] = JitPpc::bgt(d);
    }
    *p++ = JitPpc::lwz(JitPpc::R9, JitPpc::R31, offCpsr);
    emitInsertBit(p, JitPpc::R9, JitPpc::R8, 5, 0);
    *p++ = JitPpc::stw(JitPpc::R9, JitPpc::R31, offCpsr);
    *p++ = JitPpc::stw(JitPpc::R8, JitPpc::R31, offR15);
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    *p++ = JitPpc::li(JitPpc::R4, 0x0F); // bxReg with Rm = R15 already written to registersUsr[15]!
    emitLoadImm32(p, JitPpc::R12, (uint32_t)resolveArmHandler(0xE12FFF1Fu));
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctrl();
    *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 36);
    *p++ = JitPpc::mtlr(JitPpc::R0);
    emitRet(prefixCycles + 3);
    return true;
}

// Inline exit for an ARM LDR/LDRB/LDRH targeting PC (computed branch / dispatch table). Mirrors the
// interpreter's ldrOf / ldrbOf / ldrhOf: the load runs natively when its page is mapped, R15 receives the raw
// loaded word (after the word / ARM7-halfword misalign rotates), the THUMB bit is latched from bit 0 on ARM9,
// and the pipeline is refilled inline when the target page is mapped and stays clear of the page ends.
// Anything else (unmapped source page, THUMB target, guard zone) falls into the interpreter handler for the
// opcode, which performs the whole instruction. Cycles match the handlers: taken -> prefix + 5, not taken -> +1.
bool ArmPpcJit::emitPcDestLoadTerminatorArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, uint32_t instrPC,
                                            int32_t totalDelta, int32_t prefixCycles, bool helperInBlock) {
    const uint8_t cond = (uint8_t)(opcode >> 28);
    if (cond == 0xFu) return false;
    if (!isPcDestLoadTerminatorArm(cpu->arm7, opcode)) return false;
    const bool single = (opcode & 0x0C000000u) == 0x04000000u;
    const bool isByte = single && (opcode & (1u << 22)) != 0;
    const bool isHalf = !single;
    const bool up = (opcode & (1u << 23)) != 0;
    const bool wb = (opcode & (1u << 21)) != 0;
    const bool immOff = single ? ((opcode & (1u << 25)) == 0) : ((opcode & (1u << 22)) != 0);
    const uint8_t rn = (uint8_t)((opcode >> 16) & 0xF);
    const uint8_t rm = (uint8_t)(opcode & 0xF);
    const uint8_t shift = single ? (uint8_t)((opcode >> 7) & 0x1F) : 0;
    const uint32_t imm = single ? (opcode & 0xFFFu) : (uint32_t)(((opcode >> 4) & 0xF0u) | (opcode & 0xFu));

    const uint32_t brAddr = instrPC + (uint32_t)totalDelta;
    const uint32_t r15Instr = brAddr + 8u;
    const int16_t offR15   = (int16_t)(offsetof(Interpreter, registersUsr) + 15 * 4);
    const int16_t offPcDat = (int16_t)offsetof(Interpreter, pcData);
    const int16_t offPipe0 = (int16_t)offsetof(Interpreter, pipeline);
    const int16_t offPipe1 = (int16_t)(offsetof(Interpreter, pipeline) + 4);
    const int16_t offCpsr  = (int16_t)offsetof(Interpreter, cpsr);
    const int16_t offCore  = (int16_t)offsetof(Interpreter, core);
    const uint32_t offReadMap = (uint32_t)(offsetof(Core, memory) + (cpu->arm7 ? offsetof(Memory, readMap7) : offsetof(Memory, readMap9A)));

    auto emitReturn = [&](int cycles, bool addToR3) {
        if (addToR3) {
            if (cycles != 0) *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)cycles);
        } else {
            *p++ = JitPpc::li(JitPpc::R3, (int16_t)cycles);
        }
        if (helperInBlock) {
            *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 36);
            *p++ = JitPpc::mtlr(JitPpc::R0);
        }
        *p++ = JitPpc::lmw(JitPpc::R28, JitPpc::R1, 16);
        *p++ = JitPpc::addi(JitPpc::R1, JitPpc::R1, 32);
        *p++ = JitPpc::blr();
    };

    // Forward branches into the interpreter-handler path (kind: 0 = beq, 1 = bne, 2 = blt, 3 = bgt)
    uint32_t *slowFix[8];
    uint8_t slowKind[8];
    int slowCount = 0;
    auto fixSlow = [&](uint8_t kind) { slowFix[slowCount] = p++; slowKind[slowCount] = kind; slowCount++; };

    uint32_t *bNotTaken = nullptr;
    if (cond != 0xEu) {
        // Interpreter::condition[(cond << 4) | (cpsr >> 28)]; values are 0/1 for every cond != 0xF
        *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
        *p++ = JitPpc::rlwinm(JitPpc::R6, JitPpc::R6, 4, 28, 31);
        *p++ = JitPpc::addi(JitPpc::R6, JitPpc::R6, (int16_t)(cond << 4));
        emitLoadImm32(p, JitPpc::R7, (uint32_t)(uintptr_t)Interpreter::condition);
        *p++ = JitPpc::add(JitPpc::R7, JitPpc::R7, JitPpc::R6);
        *p++ = JitPpc::lbz(JitPpc::R6, JitPpc::R7, 0);
        *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
        bNotTaken = p++;
    }

    // Effective address in R3; PC-relative bases use the compile-time instruction PC, never the runtime R15
    if (rn == 15) emitLoadImm32(p, JitPpc::R3, r15Instr);
    else emitLoadGuestReg(p, JitPpc::R3, rn);
    if (immOff) {
        if (imm != 0) *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)(up ? (int32_t)imm : -(int32_t)imm));
    } else {
        emitLoadGuestReg(p, JitPpc::R4, rm);
        if (shift) *p++ = JitPpc::slwi(JitPpc::R4, JitPpc::R4, shift);
        if (up) *p++ = JitPpc::add(JitPpc::R3, JitPpc::R3, JitPpc::R4);
        else    *p++ = JitPpc::subf(JitPpc::R3, JitPpc::R4, JitPpc::R3);
    }

    // Mapped read of the loaded value into R8 (same page-map fast path as emitNativeLoadArm)
    *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R3, 22, 10, 29); // (address >> 12) * 4
    // The Core and its inline page tables remain stable until JIT reset.
    emitLoadImm32(p, JitPpc::R4, (uint32_t)(uintptr_t)cpu->core + offReadMap);
    *p++ = JitPpc::lwzx(JitPpc::R6, JitPpc::R4, JitPpc::R5);
    *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
    fixSlow(0);
    *p++ = JitPpc::rlwinm(JitPpc::R7, JitPpc::R3, 0, 20, (uint8_t)(isByte ? 31 : (isHalf ? 30 : 29)));
    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R6, JitPpc::R7);
    if (isByte)      *p++ = JitPpc::lbz(JitPpc::R8, JitPpc::R7, 0);
    else if (isHalf) *p++ = JitPpc::lhbrx(JitPpc::R8, 0, JitPpc::R7);
    else             *p++ = JitPpc::lwbrx(JitPpc::R8, 0, JitPpc::R7);
    if (!isByte) {
        // Misaligned rotates, exactly as the interpreter handlers apply them
        if (!isHalf)       *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 3, 27, 28);   // (addr & 3) << 3
        else if (cpu->arm7) *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 3, 28, 28);  // (addr & 1) << 3
        if (!isHalf || cpu->arm7) {
            *p++ = JitPpc::subfic(JitPpc::R9, JitPpc::R9, 32);
            *p++ = JitPpc::rlwnm(JitPpc::R8, JitPpc::R8, JitPpc::R9, 0, 31);
        }
    }

    if (!cpu->arm7) {
        // Latch the THUMB bit from bit 0; a THUMB target needs the interpreter's 16-bit refill
        *p++ = JitPpc::rlwinm(JitPpc::R10, JitPpc::R8, 5, 5, 5);
        *p++ = JitPpc::lwz(JitPpc::R9, JitPpc::R31, offCpsr);
        *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R9, 0, 27, 25);          // clear T bit
        *p++ = JitPpc::or_(JitPpc::R9, JitPpc::R9, JitPpc::R10);
        *p++ = JitPpc::stw(JitPpc::R9, JitPpc::R31, offCpsr);
        *p++ = JitPpc::andi_rc(JitPpc::R10, JitPpc::R8, 1);
        fixSlow(1);
    }

    // ARM target: A = value & ~3, refilled only while it stays clear of the mapped page ends
    *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R8, 0, 0, 29);               // A
    *p++ = JitPpc::rlwinm(JitPpc::R10, JitPpc::R9, 0, 20, 29);             // A & 0xFFC
    *p++ = JitPpc::cmpwi(0, JitPpc::R10, 8);
    fixSlow(2);
    *p++ = JitPpc::cmplwi(0, JitPpc::R10, 0xFA0);
    fixSlow(3);
    *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R9, 22, 10, 29); // (address >> 12) * 4
    // The Core and its inline page tables remain stable until JIT reset.
    emitLoadImm32(p, JitPpc::R4, (uint32_t)(uintptr_t)cpu->core + offReadMap);
    *p++ = JitPpc::lwzx(JitPpc::R6, JitPpc::R4, JitPpc::R5);
    *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
    fixSlow(0);

    if (wb) emitStoreGuestReg(p, JitPpc::R3, rn);                          // pre-index writeback (fast path only)

    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R6, JitPpc::R10);               // pointer to the word at A
    *p++ = JitPpc::lwbrx(JitPpc::R11, 0, JitPpc::R7);                      // pipeline[0]
    *p++ = JitPpc::addi(JitPpc::R7, JitPpc::R7, 4);                        // pcData
    *p++ = JitPpc::lwbrx(JitPpc::R12, 0, JitPpc::R7);                      // pipeline[1]
    *p++ = JitPpc::stw(JitPpc::R11, JitPpc::R31, offPipe0);
    *p++ = JitPpc::stw(JitPpc::R12, JitPpc::R31, offPipe1);
    *p++ = JitPpc::stw(JitPpc::R7, JitPpc::R31, offPcDat);
    *p++ = JitPpc::addi(JitPpc::R9, JitPpc::R9, 4);                        // R15 = A + 4
    *p++ = JitPpc::stw(JitPpc::R9, JitPpc::R31, offR15);
    emitReturn(prefixCycles + 5, false);

    // Slow path: the interpreter handler performs the whole instruction (load, R15, THUMB bit, refill)
    uint32_t *slow = p;
    emitLoadImm32(p, JitPpc::R0, r15Instr);
    *p++ = JitPpc::stw(JitPpc::R0, JitPpc::R31, offR15);
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    emitLoadImm32(p, JitPpc::R4, opcode);
    emitLoadImm32(p, JitPpc::R12, (uint32_t)resolveArmHandler(opcode));
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctrl();
    *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 36);                        // bctrl clobbered LR
    *p++ = JitPpc::mtlr(JitPpc::R0);
    emitReturn(prefixCycles, true);                                        // R3 = handler cycles (5)

    if (bNotTaken) {
        *bNotTaken = JitPpc::beq((int16_t)((p - bNotTaken) * 4));
        emitLoadImm32(p, JitPpc::R5, r15Instr);
        *p++ = JitPpc::stw(JitPpc::R5, JitPpc::R31, offR15);
        *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, offPcDat);
        if (totalDelta != 0) *p++ = JitPpc::addi(JitPpc::R4, JitPpc::R4, (int16_t)totalDelta);
        *p++ = JitPpc::stw(JitPpc::R4, JitPpc::R31, offPcDat);
        *p++ = JitPpc::addi(JitPpc::R5, JitPpc::R4, -4);
        *p++ = JitPpc::lwbrx(JitPpc::R6, 0, JitPpc::R5);
        *p++ = JitPpc::lwbrx(JitPpc::R7, 0, JitPpc::R4);
        *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offPipe0);
        *p++ = JitPpc::stw(JitPpc::R7, JitPpc::R31, offPipe1);
        emitReturn(prefixCycles + 1, false);
    }

    for (int i = 0; i < slowCount; i++) {
        const int16_t d = (int16_t)((slow - slowFix[i]) * 4);
        switch (slowKind[i]) {
            case 0: *slowFix[i] = JitPpc::beq(d); break;
            case 1: *slowFix[i] = JitPpc::bne(d); break;
            case 2: *slowFix[i] = JitPpc::blt(d); break;
            default: *slowFix[i] = JitPpc::bgt(d); break;
        }
    }
    return true;
}

bool ArmPpcJit::emitNativeLoadThumb(uint32_t *&p, Interpreter *cpu, uint16_t opcode, int &outCycles) {
    bool isByte = false, isHalf = false, isSigned = false;
    uint8_t rd = 0;

    if ((opcode & 0xF800u) == 0x4800u) { // LDR Rd, [PC, #imm8<<2]
        rd = (uint8_t)((opcode >> 8) & 7u);
        uint32_t addr = (s_emitInstrR15 & ~3u) + ((opcode & 0xFFu) << 2);
        emitLoadImm32(p, JitPpc::R3, addr);
    } else if ((opcode & 0xF800u) == 0x9800u) { // LDR Rd, [SP, #imm8<<2]
        rd = (uint8_t)((opcode >> 8) & 7u);
        uint32_t off = (opcode & 0xFFu) << 2;
        emitLoadGuestReg(p, JitPpc::R3, 13);
        if (off) *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)off);
    } else if ((opcode & 0xF000u) == 0x5000u) { // Reg offset
        rd = (uint8_t)(opcode & 7u);
        uint8_t rb = (uint8_t)((opcode >> 3) & 7u);
        uint8_t ro = (uint8_t)((opcode >> 6) & 7u);
        uint8_t sub = (uint8_t)((opcode >> 9) & 7u);
        if (sub == 3) { isByte = true; isSigned = true; }       // LDSB
        else if (sub == 5) { isHalf = true; }                   // LDRH
        else if (sub == 6) { isByte = true; }                   // LDRB
        else if (sub == 7) { isHalf = true; isSigned = true; }  // LDSH
        emitLoadGuestReg(p, JitPpc::R3, rb);
        emitLoadGuestReg(p, JitPpc::R4, ro);
        *p++ = JitPpc::add(JitPpc::R3, JitPpc::R3, JitPpc::R4);
    } else {
        rd = (uint8_t)(opcode & 7u);
        uint8_t rb = (uint8_t)((opcode >> 3) & 7u);
        uint32_t imm5 = (opcode >> 6) & 0x1Fu;
        if ((opcode & 0xF800u) == 0x7800u) { isByte = true; }
        else if ((opcode & 0xF800u) == 0x8800u) { isHalf = true; imm5 <<= 1; }
        else { imm5 <<= 2; }
        emitLoadGuestReg(p, JitPpc::R3, rb);
        if (imm5) *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)imm5);
    }

    uint32_t *fastJumps[4];
    int fastJumpCnt = 0;
    if (isHalf && !isSigned) {
        emitGuardedPollHalfLoad(p, cpu, rd, &ArmPpcJit::emitStoreGuestReg, fastJumps, fastJumpCnt);
    }

    const uint32_t offTblOff = (uint32_t)(offsetof(Core, memory) + (cpu->arm7 ? offsetof(Memory, readMap7) : offsetof(Memory, readMap9A)));
    *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R3, 22, 10, 29); // (address >> 12) * 4
    // The Core and its inline page tables remain stable until JIT reset.
    emitLoadImm32(p, JitPpc::R4, (uint32_t)(uintptr_t)cpu->core + offTblOff);
    *p++ = JitPpc::lwzx(JitPpc::R6, JitPpc::R4, JitPpc::R5);
    *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
    uint32_t *bSlow = p++;

    *p++ = JitPpc::rlwinm(JitPpc::R7, JitPpc::R3, 0, 20, (uint8_t)(isByte ? 31 : (isHalf ? 30 : 29)));
    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R6, JitPpc::R7);
    if (isByte) {
        *p++ = JitPpc::lbz(JitPpc::R8, JitPpc::R7, 0);
        if (isSigned) *p++ = JitPpc::extsb(JitPpc::R8, JitPpc::R8);
    } else if (isHalf) {
        *p++ = JitPpc::lhbrx(JitPpc::R8, 0, JitPpc::R7);
        if (isSigned) {
            *p++ = JitPpc::extsh(JitPpc::R8, JitPpc::R8);
            if (cpu->arm7) {
                *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 3, 28, 28);
                *p++ = JitPpc::sraw(JitPpc::R8, JitPpc::R8, JitPpc::R9);
            }
        } else if (cpu->arm7) {
            *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 3, 28, 28);
            *p++ = JitPpc::subfic(JitPpc::R9, JitPpc::R9, 32);
            *p++ = JitPpc::rlwnm(JitPpc::R8, JitPpc::R8, JitPpc::R9, 0, 31);
        }
    } else {
        *p++ = JitPpc::lwbrx(JitPpc::R8, 0, JitPpc::R7);
        if ((opcode & 0xF800u) != 0x4800u) {
            *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 3, 27, 28);
            *p++ = JitPpc::subfic(JitPpc::R9, JitPpc::R9, 32);
            *p++ = JitPpc::rlwnm(JitPpc::R8, JitPpc::R8, JitPpc::R9, 0, 31);
        }
    }
    emitStoreGuestReg(p, JitPpc::R8, rd);
    uint32_t *bOver = p++;

    uint32_t *slow = p;
    uintptr_t helper = resolveThumbHandler(opcode);
    emitHelperR15Sync(p, true);
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    emitLoadImm32(p, JitPpc::R4, opcode);
    emitLoadImm32(p, JitPpc::R12, (uint32_t)helper);
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctrl();
    emitHelperR15Sync(p, false);

    uint32_t *join = p;
    *bSlow = JitPpc::beq((int16_t)((slow - bSlow) * 4));
    *bOver = JitPpc::b((int32_t)((join - bOver) * 4));
    for (int i = 0; i < fastJumpCnt; i++)
        *fastJumps[i] = JitPpc::b((int32_t)((join - fastJumps[i]) * 4));

    outCycles = cpu->arm7 ? 3 : 1;
    return true;
}

bool ArmPpcJit::emitNativeStoreThumb(uint32_t *&p, Interpreter *cpu, uint16_t opcode, int &outCycles) {
    bool isByte = false, isHalf = false;
    uint8_t rd = 0;

    if ((opcode & 0xF800u) == 0x9000u) { // STR Rd, [SP, #imm8<<2]
        rd = (uint8_t)((opcode >> 8) & 7u);
        uint32_t off = (opcode & 0xFFu) << 2;
        emitLoadGuestReg(p, JitPpc::R3, 13);
        if (off) *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)off);
    } else if ((opcode & 0xF000u) == 0x5000u) { // STR/STRH/STRB Rd, [Rb, Ro]
        rd = (uint8_t)(opcode & 7u);
        uint8_t rb = (uint8_t)((opcode >> 3) & 7u);
        uint8_t ro = (uint8_t)((opcode >> 6) & 7u);
        uint8_t sub = (uint8_t)((opcode >> 9) & 7u);
        if (sub == 1) isHalf = true;
        else if (sub == 2) isByte = true;
        emitLoadGuestReg(p, JitPpc::R3, rb);
        emitLoadGuestReg(p, JitPpc::R4, ro);
        *p++ = JitPpc::add(JitPpc::R3, JitPpc::R3, JitPpc::R4);
    } else {
        rd = (uint8_t)(opcode & 7u);
        uint8_t rb = (uint8_t)((opcode >> 3) & 7u);
        uint32_t imm5 = (opcode >> 6) & 0x1Fu;
        if ((opcode & 0xF800u) == 0x7000u) { isByte = true; }
        else if ((opcode & 0xF800u) == 0x8000u) { isHalf = true; imm5 <<= 1; }
        else { imm5 <<= 2; }
        emitLoadGuestReg(p, JitPpc::R3, rb);
        if (imm5) *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)imm5);
    }
    emitLoadGuestReg(p, JitPpc::R8, rd);

    *p++ = JitPpc::srwi(JitPpc::R5, JitPpc::R3, 27);
    *p++ = JitPpc::cmpwi(0, JitPpc::R5, 0);
    uint32_t *bNoCode = p++;
    *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 26, 27, 31);
    *p++ = JitPpc::li(JitPpc::R11, 1);
    *p++ = JitPpc::slw(JitPpc::R11, JitPpc::R11, JitPpc::R9);
    *p++ = JitPpc::rlwinm(JitPpc::R12, JitPpc::R3, 23, 14, 29);
    emitLoadImm32(p, JitPpc::R6, (uint32_t)(uintptr_t)&codeLineBits[0]);
    *p++ = JitPpc::lwzx(JitPpc::R9, JitPpc::R6, JitPpc::R12);
    *p++ = JitPpc::and_(JitPpc::R9, JitPpc::R9, JitPpc::R11);
    *p++ = JitPpc::cmpwi(0, JitPpc::R9, 0);
    uint32_t *bBusy = p++;
    *bNoCode = JitPpc::bne((int16_t)((p - bNoCode) * 4));

    const uint32_t wmTblOff = (uint32_t)(offsetof(Core, memory) + (cpu->arm7 ? offsetof(Memory, writeMap7) : offsetof(Memory, writeMap9A)));
    *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R3, 22, 10, 29); // (address >> 12) * 4
    // The Core and its inline page tables remain stable until JIT reset.
    emitLoadImm32(p, JitPpc::R4, (uint32_t)(uintptr_t)cpu->core + wmTblOff);
    *p++ = JitPpc::lwzx(JitPpc::R6, JitPpc::R4, JitPpc::R5);
    *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
    uint32_t *bSlow = p++;

    *p++ = JitPpc::rlwinm(JitPpc::R7, JitPpc::R3, 0, 20, (uint8_t)(isByte ? 31 : (isHalf ? 30 : 29)));
    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R6, JitPpc::R7);
    if (isByte)      *p++ = JitPpc::stb(JitPpc::R8, JitPpc::R7, 0);
    else if (isHalf) *p++ = JitPpc::sthbrx(JitPpc::R8, 0, JitPpc::R7);
    else             *p++ = JitPpc::stwbrx(JitPpc::R8, 0, JitPpc::R7);
    uint32_t *bOver = p++;

    uint32_t *slow = p;
    uintptr_t helper = resolveThumbHandler(opcode);
    emitHelperR15Sync(p, true);
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    emitLoadImm32(p, JitPpc::R4, opcode);
    emitLoadImm32(p, JitPpc::R12, (uint32_t)helper);
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctrl();
    emitHelperR15Sync(p, false);

    uint32_t *join = p;
    *bBusy = JitPpc::bne((int16_t)((slow - bBusy) * 4));
    *bSlow = JitPpc::beq((int16_t)((slow - bSlow) * 4));
    *bOver = JitPpc::b((int32_t)((join - bOver) * 4));

    outCycles = cpu->arm7 ? 2 : 1;
    return true;
}

bool ArmPpcJit::emitNativePushPop(uint32_t *&p, Interpreter *cpu, uint16_t regMask, bool isLoad,
                                  uint32_t rawOpcode, bool isThumb, int &outCycles) {
    int m = __builtin_popcount((unsigned)regMask);
    if (m <= 0) return false;

    uint32_t *slowFix[6];
    int slowCnt = 0;

    // Load SP into R3; for PUSH pre-decrement by 4*m
    emitLoadGuestReg(p, JitPpc::R3, 13);
    if (!isLoad) {
        *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)(-4 * m));
    }
    // Require word alignment & all m words inside the same 2 KB codeLineBits / 4 KB page window!
    *p++ = JitPpc::andi_rc(JitPpc::R0, JitPpc::R3, 3u);
    slowFix[slowCnt++] = p++; // bne slow
    if (m > 1) {
        *p++ = JitPpc::addi(JitPpc::R4, JitPpc::R3, (int16_t)(4 * (m - 1)));
        *p++ = JitPpc::xor_(JitPpc::R5, JitPpc::R3, JitPpc::R4);
        *p++ = JitPpc::srwi(JitPpc::R5, JitPpc::R5, 11);
        *p++ = JitPpc::cmpwi(0, JitPpc::R5, 0);
        slowFix[slowCnt++] = p++; // bne slow
    }
    if (!isLoad) {
        *p++ = JitPpc::srwi(JitPpc::R5, JitPpc::R3, 27);
        *p++ = JitPpc::cmpwi(0, JitPpc::R5, 0);
        uint32_t *bNoCode = p++;
        *p++ = JitPpc::rlwinm(JitPpc::R12, JitPpc::R3, 23, 14, 29);
        emitLoadImm32(p, JitPpc::R6, (uint32_t)(uintptr_t)&codeLineBits[0]);
        *p++ = JitPpc::lwzx(JitPpc::R9, JitPpc::R6, JitPpc::R12);
        *p++ = JitPpc::cmpwi(0, JitPpc::R9, 0);
        slowFix[slowCnt++] = p++; // bne slow
        *bNoCode = JitPpc::bne((int16_t)((p - bNoCode) * 4));
    }

    const uint32_t tblOff = (uint32_t)(offsetof(Core, memory) + (isLoad
        ? (cpu->arm7 ? offsetof(Memory, readMap7) : offsetof(Memory, readMap9A))
        : (cpu->arm7 ? offsetof(Memory, writeMap7) : offsetof(Memory, writeMap9A))));
    *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R3, 22, 10, 29); // (address >> 12) * 4
    *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, (int16_t)offsetof(Interpreter, core));
    emitLoadImm32(p, JitPpc::R7, tblOff);
    *p++ = JitPpc::add(JitPpc::R4, JitPpc::R4, JitPpc::R7);
    *p++ = JitPpc::lwzx(JitPpc::R6, JitPpc::R4, JitPpc::R5);
    *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
    uint32_t *bNull = p++;

    // R7 = host stack pointer inside the mapped 4 KB page!
    *p++ = JitPpc::rlwinm(JitPpc::R7, JitPpc::R3, 0, 20, 29);
    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R6, JitPpc::R7);

    int slot = 0;
    for (int reg = 0; reg < 15; reg++) {
        if (!(regMask & (1u << reg))) continue;
        if (slot > 0) *p++ = JitPpc::addi(JitPpc::R7, JitPpc::R7, 4);
        if (isLoad) {
            *p++ = JitPpc::lwbrx(JitPpc::R8, 0, JitPpc::R7);
            emitStoreGuestReg(p, JitPpc::R8, (uint8_t)reg);
        } else {
            emitLoadGuestReg(p, JitPpc::R8, (uint8_t)reg);
            *p++ = JitPpc::stwbrx(JitPpc::R8, 0, JitPpc::R7);
        }
        slot++;
    }
    if (isLoad) {
        *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)(4 * m));
    }
    emitStoreGuestReg(p, JitPpc::R3, 13);
    uint32_t *bOver = p++;

    // Slow path: invoke exact original interpreter handler
    uint32_t *slow = p;
    uintptr_t helper = isThumb ? resolveThumbHandler((uint16_t)rawOpcode) : resolveArmHandler(rawOpcode);
    emitHelperR15Sync(p, true);
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    emitLoadImm32(p, JitPpc::R4, rawOpcode);
    emitLoadImm32(p, JitPpc::R12, (uint32_t)helper);
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctrl();
    emitHelperR15Sync(p, false);

    uint32_t *join = p;
    for (int i = 0; i < slowCnt; i++)
        *slowFix[i] = JitPpc::bne((int16_t)((slow - slowFix[i]) * 4));
    *bNull = JitPpc::beq((int16_t)((slow - bNull) * 4));
    *bOver = JitPpc::b((int32_t)((join - bOver) * 4));

    outCycles = isLoad ? (m + (cpu->arm7 ? 2 : (m < 2))) : (m + (cpu->arm7 || m < 2));
    return true;
}

bool ArmPpcJit::emitNativeLoadArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, int &outCycles) {
    const bool single = (opcode & 0x0C000000u) == 0x04000000u;
    const uint8_t shField = (uint8_t)((opcode >> 5) & 3u);
    const bool isByte = single ? ((opcode & (1u << 22)) != 0) : (shField == 2u);
    const bool isHalf = !single && (shField == 1u || shField == 3u);
    const bool isSigned = !single && (shField >= 2u);
    const bool up     = (opcode & (1u << 23)) != 0;
    const uint8_t rn = (uint8_t)((opcode >> 16) & 0xF);
    const uint8_t rd = (uint8_t)((opcode >> 12) & 0xF);
    const bool immOff = single ? ((opcode & (1u << 25)) == 0) : ((opcode & (1u << 22)) != 0);

    // R3 = guest address (compile-time constant for [PC, +/-#imm]!)
    if (rn == 15) {
        uint32_t off = single ? (opcode & 0xFFFu) : (((opcode >> 4) & 0xF0u) | (opcode & 0xFu));
        uint32_t addr = up ? (s_emitInstrR15 + off) : (s_emitInstrR15 - off);
        emitLoadImm32(p, JitPpc::R3, addr);
    } else {
        emitLoadGuestReg(p, JitPpc::R3, rn);
        if (immOff) {
            uint32_t off = single ? (opcode & 0xFFFu) : (((opcode >> 4) & 0xF0u) | (opcode & 0xFu));
            if (off != 0) {
                *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)(up ? (int32_t)off : -(int32_t)off));
            }
        } else {
            uint8_t rm = (uint8_t)(opcode & 0xF);
            uint8_t sh = single ? (uint8_t)((opcode >> 7) & 0x1F) : (uint8_t)0;
            emitLoadGuestReg(p, JitPpc::R4, rm);
            if (sh) *p++ = JitPpc::slwi(JitPpc::R4, JitPpc::R4, sh);
            if (up) *p++ = JitPpc::add(JitPpc::R3, JitPpc::R3, JitPpc::R4);
            else    *p++ = JitPpc::subf(JitPpc::R3, JitPpc::R4, JitPpc::R3);
        }
    }

    uint32_t *fastJumps[4];
    int fastJumpCnt = 0;
    if (isHalf && !isSigned) {
        emitGuardedPollHalfLoad(p, cpu, rd, &ArmPpcJit::emitStoreGuestReg, fastJumps, fastJumpCnt);
    }

    const uint32_t offTblOff = (uint32_t)(offsetof(Core, memory) + (cpu->arm7 ? offsetof(Memory, readMap7) : offsetof(Memory, readMap9A)));
    *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R3, 22, 10, 29); // (address >> 12) * 4
    // The Core and its inline page tables remain stable until JIT reset.
    emitLoadImm32(p, JitPpc::R4, (uint32_t)(uintptr_t)cpu->core + offTblOff);
    *p++ = JitPpc::lwzx(JitPpc::R6, JitPpc::R4, JitPpc::R5);
    *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
    uint32_t *bSlow = p++;

    if (isByte)      *p++ = JitPpc::rlwinm(JitPpc::R7, JitPpc::R3, 0, 20, 31);
    else if (isHalf) *p++ = JitPpc::rlwinm(JitPpc::R7, JitPpc::R3, 0, 20, 30);
    else             *p++ = JitPpc::rlwinm(JitPpc::R7, JitPpc::R3, 0, 20, 29);
    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R6, JitPpc::R7);
    if (isByte) {
        *p++ = JitPpc::lbz(JitPpc::R8, JitPpc::R7, 0);
        if (isSigned) *p++ = JitPpc::extsb(JitPpc::R8, JitPpc::R8);
    } else if (isHalf) {
        *p++ = JitPpc::lhbrx(JitPpc::R8, 0, JitPpc::R7);
        if (isSigned) {
            *p++ = JitPpc::extsh(JitPpc::R8, JitPpc::R8);
            if (cpu->arm7) {
                *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 3, 28, 28);
                *p++ = JitPpc::sraw(JitPpc::R8, JitPpc::R8, JitPpc::R9);
            }
        } else if (cpu->arm7) {
            *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 3, 28, 28);
            *p++ = JitPpc::subfic(JitPpc::R9, JitPpc::R9, 32);
            *p++ = JitPpc::rlwnm(JitPpc::R8, JitPpc::R8, JitPpc::R9, 0, 31);
        }
    } else {
        *p++ = JitPpc::lwbrx(JitPpc::R8, 0, JitPpc::R7);
        if (!(rn == 15 && (((s_emitInstrR15 + (up ? (opcode & 0xFFFu) : -(opcode & 0xFFFu))) & 3u) == 0u))) {
            *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 3, 27, 28);
            *p++ = JitPpc::subfic(JitPpc::R9, JitPpc::R9, 32);
            *p++ = JitPpc::rlwnm(JitPpc::R8, JitPpc::R8, JitPpc::R9, 0, 31);
        }
    }
    emitStoreGuestReg(p, JitPpc::R8, rd);
    uint32_t *bOver = p++;

    uint32_t *slow = p;
    uintptr_t helper = resolveArmHandler(opcode);
    emitHelperR15Sync(p, true);
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    emitLoadImm32(p, JitPpc::R4, opcode);
    emitLoadImm32(p, JitPpc::R12, (uint32_t)helper);
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctrl();
    emitHelperR15Sync(p, false);

    uint32_t *join = p;
    *bSlow = JitPpc::beq((int16_t)((slow - bSlow) * 4));
    *bOver = JitPpc::b((int32_t)((join - bOver) * 4));
    for (int i = 0; i < fastJumpCnt; i++)
        *fastJumps[i] = JitPpc::b((int32_t)((join - fastJumps[i]) * 4));

    outCycles = cpu->arm7 ? 3 : 1;
    return true;
}

bool ArmPpcJit::emitNativeStoreArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, int &outCycles) {
    // STR / STRB / STRH, pre- or post-indexed, immediate or LSL register offsets
    const bool single = (opcode & 0x0C000000u) == 0x04000000u;
    const bool isByte = single && (opcode & (1u << 22)) != 0;
    const bool isHalf = !single;
    const bool pre = (opcode & (1u << 24)) != 0;
    const bool up  = (opcode & (1u << 23)) != 0;
    const bool wb  = ((opcode & (1u << 21)) != 0) || !pre;
    const bool immOff = single ? ((opcode & (1u << 25)) == 0) : ((opcode & (1u << 22)) != 0);
    const uint8_t rn = (uint8_t)((opcode >> 16) & 0xF);
    const uint8_t rd = (uint8_t)((opcode >> 12) & 0xF);
    const uint8_t rm = (uint8_t)(opcode & 0xF);
    const uint8_t shift = single ? (uint8_t)((opcode >> 7) & 0x1F) : 0;
    const uint32_t imm = single ? (opcode & 0xFFFu) : (uint32_t)(((opcode >> 4) & 0xF0u) | (opcode & 0xFu));

    emitLoadGuestReg(p, JitPpc::R3, rn);
    if (pre) {
        if (immOff) {
            if (imm != 0) *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)(up ? (int32_t)imm : -(int32_t)imm));
        } else {
            emitLoadGuestReg(p, JitPpc::R4, rm);
            if (shift) *p++ = JitPpc::slwi(JitPpc::R4, JitPpc::R4, shift);
            if (up) *p++ = JitPpc::add(JitPpc::R3, JitPpc::R3, JitPpc::R4);
            else    *p++ = JitPpc::subf(JitPpc::R3, JitPpc::R4, JitPpc::R3);
        }
        if (wb) *p++ = JitPpc::mr(JitPpc::R28, JitPpc::R3);
    } else {
        if (immOff) {
            *p++ = JitPpc::addi(JitPpc::R28, JitPpc::R3, (int16_t)(up ? (int32_t)imm : -(int32_t)imm));
        } else {
            emitLoadGuestReg(p, JitPpc::R4, rm);
            if (shift) *p++ = JitPpc::slwi(JitPpc::R4, JitPpc::R4, shift);
            if (up) *p++ = JitPpc::add(JitPpc::R28, JitPpc::R3, JitPpc::R4);
            else    *p++ = JitPpc::subf(JitPpc::R28, JitPpc::R4, JitPpc::R3);
        }
    }
    emitLoadGuestReg(p, JitPpc::R8, rd);

    // Exact 64-byte line SMC check (zero false invalidations on same-page stack / globals!)
    *p++ = JitPpc::srwi(JitPpc::R5, JitPpc::R3, 27);
    *p++ = JitPpc::cmpwi(0, JitPpc::R5, 0);
    uint32_t *bNoCode = p++;
    *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 26, 27, 31);
    *p++ = JitPpc::li(JitPpc::R11, 1);
    *p++ = JitPpc::slw(JitPpc::R11, JitPpc::R11, JitPpc::R9);
    *p++ = JitPpc::rlwinm(JitPpc::R12, JitPpc::R3, 23, 14, 29);
    emitLoadImm32(p, JitPpc::R6, (uint32_t)(uintptr_t)&codeLineBits[0]);
    *p++ = JitPpc::lwzx(JitPpc::R9, JitPpc::R6, JitPpc::R12);
    *p++ = JitPpc::and_(JitPpc::R9, JitPpc::R9, JitPpc::R11);
    *p++ = JitPpc::cmpwi(0, JitPpc::R9, 0);
    uint32_t *bBusy = p++;
    *bNoCode = JitPpc::bne((int16_t)((p - bNoCode) * 4));

    const uint32_t wmTblOff = (uint32_t)(offsetof(Core, memory) + (cpu->arm7 ? offsetof(Memory, writeMap7) : offsetof(Memory, writeMap9A)));
    *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R3, 22, 10, 29); // (address >> 12) * 4
    // The Core and its inline page tables remain stable until JIT reset.
    emitLoadImm32(p, JitPpc::R4, (uint32_t)(uintptr_t)cpu->core + wmTblOff);
    *p++ = JitPpc::lwzx(JitPpc::R6, JitPpc::R4, JitPpc::R5);
    *p++ = JitPpc::cmpwi(0, JitPpc::R6, 0);
    uint32_t *bSlow = p++;

    *p++ = JitPpc::rlwinm(JitPpc::R7, JitPpc::R3, 0, 20, (uint8_t)(isByte ? 31 : (isHalf ? 30 : 29)));
    *p++ = JitPpc::add(JitPpc::R7, JitPpc::R6, JitPpc::R7);
    if (isByte)      *p++ = JitPpc::stb(JitPpc::R8, JitPpc::R7, 0);
    else if (isHalf) *p++ = JitPpc::sthbrx(JitPpc::R8, 0, JitPpc::R7);
    else             *p++ = JitPpc::stwbrx(JitPpc::R8, 0, JitPpc::R7);
    if (wb) emitStoreGuestReg(p, JitPpc::R28, rn);
    uint32_t *bOver = p++;

    uint32_t *slow = p;
    uintptr_t helper = resolveArmHandler(opcode);
    emitHelperR15Sync(p, true);
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    emitLoadImm32(p, JitPpc::R4, opcode);
    emitLoadImm32(p, JitPpc::R12, (uint32_t)helper);
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctrl();
    emitHelperR15Sync(p, false);

    uint32_t *join = p;
    *bBusy = JitPpc::bne((int16_t)((slow - bBusy) * 4));
    *bSlow = JitPpc::beq((int16_t)((slow - bSlow) * 4));
    *bOver = JitPpc::b((int32_t)((join - bOver) * 4));

    outCycles = cpu->arm7 ? 2 : 1;
    return true;
}

bool ArmPpcJit::emitNativeArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, int &outCycles, bool emitFlags) {
    if (isNativeLoadArm(cpu->arm7, opcode)) return emitNativeLoadArm(p, cpu, opcode, outCycles);
    if (isNativeStoreArm(opcode)) return emitNativeStoreArm(p, cpu, opcode, outCycles);
    if (isNativePushPopArm(opcode)) {
        return emitNativePushPop(p, cpu, (uint16_t)(opcode & 0xFFFFu), (opcode & (1u << 20)) != 0u, opcode, false, outCycles);
    }
    // ARM9 CP15 instructions always exit to interpreter
    if (!cpu->arm7 && (opcode & 0x0F000010u) == 0x0E000010u) {
        return false;
    }

    // ARMv5TE CLZ Rd, Rm (0x016F0F10)
    if ((opcode & 0x0FFF0FF0u) == 0x016F0F10u) {
        uint8_t rd = (uint8_t)((opcode >> 12) & 0xF);
        uint8_t rm = (uint8_t)(opcode & 0xF);
        if (rd == 15 || rm == 15) return false;
        if (!cpu->arm7) {
            emitLoadGuestReg(p, JitPpc::R4, rm);
            *p++ = JitPpc::cntlzw(JitPpc::R3, JitPpc::R4);
            emitStoreGuestReg(p, JitPpc::R3, rd);
        }
        outCycles = 1;
        return true;
    }

    // Data processing: bits 27..26 == 00
    if ((opcode & 0x0C000000u) != 0x00000000u) return false;

    bool isImm = (opcode & (1u << 25)) != 0;
    if (!isImm) {
        // Reject multiplies / misc / halfword transfers (bit 4 == 1)
        if ((opcode & (1u << 4)) != 0) return false;
    }

    uint8_t aluOp = (uint8_t)((opcode >> 21) & 0xF);
    bool rawSetFlags = (opcode & (1u << 20)) != 0;
    uint8_t rn = (uint8_t)((opcode >> 16) & 0xF);
    uint8_t rd = (uint8_t)((opcode >> 12) & 0xF);

    // Exclude MSR/MRS encodings (aluOp 8..11 with rawSetFlags == false)
    if (aluOp >= 0x8 && aluOp <= 0xB && !rawSetFlags) return false;
    // Exclude R15 writes (branches/pipeline flushes)
    if (rd == 15) return false;
    // ADCS/SBCS/RSCS (S==1) use interpreter handler to preserve exact upstream flag masks; non-S ADC/SBC/RSC are emitted natively
    if ((aluOp == 0x5 || aluOp == 0x6 || aluOp == 0x7) && rawSetFlags) return false;
    if (!isImm) {
        uint8_t shiftType = (uint8_t)((opcode >> 5) & 0x3);
        uint8_t shiftImm = (uint8_t)((opcode >> 7) & 0x1F);
        // RRX (shiftType == 3 && shiftImm == 0) uses CPSR.C; leave to fallback
        if (shiftType == 3 && shiftImm == 0) return false;
    }

    bool setFlags = rawSetFlags && emitFlags;
    // Test/compare with dead flags is a 1-cycle no-op
    if (aluOp >= 0x8 && aluOp <= 0xB && !setFlags) {
        outCycles = 1;
        return true;
    }

    // Prepare operand 2 in R5 (and track shifter carry-out for logical S==1 ops)
    bool logicalOp = (aluOp == 0x0 || aluOp == 0x1 || aluOp == 0x8 || aluOp == 0x9 ||
                      aluOp == 0xC || aluOp == 0xD || aluOp == 0xE || aluOp == 0xF);
    bool hasShifterCarry = false;
    uint8_t carrySrcReg = 0;
    uint8_t carrySrcBit = 0;

    if (isImm) {
        uint32_t imm8 = opcode & 0xFFu;
        uint8_t rot = (uint8_t)((opcode >> 7) & 0x1Eu);
        uint32_t val = rot ? ((imm8 << (32 - rot)) | (imm8 >> rot)) : imm8;
        emitLoadImm32(p, JitPpc::R5, val);
        if (setFlags && logicalOp && rot != 0) {
            hasShifterCarry = true;
            carrySrcReg = JitPpc::R5;
            carrySrcBit = 31; // bit 31 of rotated val equals imm8[rot - 1]
        }
    } else {
        uint8_t rm = (uint8_t)(opcode & 0xF);
        uint8_t shiftType = (uint8_t)((opcode >> 5) & 0x3);
        uint8_t shiftImm = (uint8_t)((opcode >> 7) & 0x1F);
        // RRX (shiftType == 3 && shiftImm == 0) uses CPSR.C; leave to fallback
        if (shiftType == 3 && shiftImm == 0) return false;

        if (setFlags && logicalOp && !(shiftType == 0 && shiftImm == 0)) {
            hasShifterCarry = true;
            carrySrcReg = JitPpc::R10;
            emitLoadGuestReg(p, JitPpc::R10, rm);
            if (shiftType == 0) { // LSL (shiftImm > 0)
                *p++ = JitPpc::slwi(JitPpc::R5, JitPpc::R10, shiftImm);
                carrySrcBit = (uint8_t)(32 - shiftImm);
            } else if (shiftType == 1) { // LSR
                if (shiftImm == 0) *p++ = JitPpc::li(JitPpc::R5, 0);
                else *p++ = JitPpc::srwi(JitPpc::R5, JitPpc::R10, shiftImm);
                carrySrcBit = (uint8_t)(shiftImm ? (shiftImm - 1) : 31);
            } else if (shiftType == 2) { // ASR
                *p++ = JitPpc::srawi(JitPpc::R5, JitPpc::R10, (uint8_t)(shiftImm ? shiftImm : 31));
                carrySrcBit = (uint8_t)(shiftImm ? (shiftImm - 1) : 31);
            } else { // ROR (shiftImm > 0)
                *p++ = JitPpc::rotrwi(JitPpc::R5, JitPpc::R10, shiftImm);
                carrySrcBit = (uint8_t)(shiftImm - 1);
            }
        } else {
            emitLoadGuestReg(p, JitPpc::R5, rm);
            if (shiftType == 0) { // LSL
                if (shiftImm != 0) *p++ = JitPpc::slwi(JitPpc::R5, JitPpc::R5, shiftImm);
            } else if (shiftType == 1) { // LSR
                if (shiftImm == 0) *p++ = JitPpc::li(JitPpc::R5, 0);
                else *p++ = JitPpc::srwi(JitPpc::R5, JitPpc::R5, shiftImm);
            } else if (shiftType == 2) { // ASR
                *p++ = JitPpc::srawi(JitPpc::R5, JitPpc::R5, (uint8_t)(shiftImm ? shiftImm : 31));
            } else { // ROR (shiftImm != 0)
                *p++ = JitPpc::rotrwi(JitPpc::R5, JitPpc::R5, shiftImm);
            }
        }
    }

    // Load Rn into R4 for binary ops
    if (aluOp != 0xD && aluOp != 0xF) {
        emitLoadGuestReg(p, JitPpc::R4, rn);
    }

    auto emitLogicalFlags = [&](uint8_t resReg) {
        if (!setFlags) return;
        if (!hasShifterCarry) {
            emitUpdateNZ(p, resReg);
        } else {
            const int16_t offCpsr = (int16_t)offsetof(Interpreter, cpsr);
            *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
            emitInsertBit(p, JitPpc::R6, resReg, 31, 31);
            *p++ = JitPpc::cntlzw(JitPpc::R7, resReg);
            emitInsertBit(p, JitPpc::R6, JitPpc::R7, 30, 5);
            emitInsertBit(p, JitPpc::R6, carrySrcReg, 29, carrySrcBit);
            *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offCpsr);
        }
    };

    switch (aluOp) {
    case 0x0: // AND
        *p++ = JitPpc::and_(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        emitLogicalFlags(JitPpc::R3);
        break;
    case 0x1: // EOR
        *p++ = JitPpc::xor_(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        emitLogicalFlags(JitPpc::R3);
        break;
    case 0x2: // SUB
        if (setFlags) {
            *p++ = JitPpc::subfc(JitPpc::R3, JitPpc::R5, JitPpc::R4);
            emitStoreGuestReg(p, JitPpc::R3, rd);
            emitUpdateSubNZCV(p);
        } else {
            *p++ = JitPpc::subf(JitPpc::R3, JitPpc::R5, JitPpc::R4);
            emitStoreGuestReg(p, JitPpc::R3, rd);
        }
        break;
    case 0x3: // RSB (res = op2 - op1)
        if (setFlags) {
            // Swap R4 and R5 so R4 = op2, R5 = op1 for emitUpdateSubNZCV
            *p++ = JitPpc::mr(JitPpc::R10, JitPpc::R4);
            *p++ = JitPpc::mr(JitPpc::R4, JitPpc::R5);
            *p++ = JitPpc::mr(JitPpc::R5, JitPpc::R10);
            *p++ = JitPpc::subfc(JitPpc::R3, JitPpc::R5, JitPpc::R4);
            emitStoreGuestReg(p, JitPpc::R3, rd);
            emitUpdateSubNZCV(p);
        } else {
            *p++ = JitPpc::subf(JitPpc::R3, JitPpc::R4, JitPpc::R5);
            emitStoreGuestReg(p, JitPpc::R3, rd);
        }
        break;
    case 0x4: // ADD
        if (setFlags) {
            *p++ = JitPpc::addc(JitPpc::R3, JitPpc::R4, JitPpc::R5);
            emitStoreGuestReg(p, JitPpc::R3, rd);
            emitUpdateAddNZCV(p);
        } else {
            *p++ = JitPpc::add(JitPpc::R3, JitPpc::R4, JitPpc::R5);
            emitStoreGuestReg(p, JitPpc::R3, rd);
        }
        break;
    case 0x5: { // ADC (non-S: Rd = op1 + op2 + C)
        const int16_t offCpsr = (int16_t)offsetof(Interpreter, cpsr);
        *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
        *p++ = JitPpc::rlwinm(JitPpc::R6, JitPpc::R6, 3, 31, 31); // R6 = (cpsr >> 29) & 1
        *p++ = JitPpc::add(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        *p++ = JitPpc::add(JitPpc::R3, JitPpc::R3, JitPpc::R6);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        break;
    }
    case 0x6: { // SBC (non-S: Rd = op1 - op2 - !C)
        const int16_t offCpsr = (int16_t)offsetof(Interpreter, cpsr);
        *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
        *p++ = JitPpc::rlwinm(JitPpc::R6, JitPpc::R6, 3, 31, 31);
        *p++ = JitPpc::addi(JitPpc::R6, JitPpc::R6, -1); // R6 = C - 1 = -!C
        *p++ = JitPpc::subf(JitPpc::R3, JitPpc::R5, JitPpc::R4);
        *p++ = JitPpc::add(JitPpc::R3, JitPpc::R3, JitPpc::R6);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        break;
    }
    case 0x7: { // RSC (non-S: Rd = op2 - op1 - !C)
        const int16_t offCpsr = (int16_t)offsetof(Interpreter, cpsr);
        *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
        *p++ = JitPpc::rlwinm(JitPpc::R6, JitPpc::R6, 3, 31, 31);
        *p++ = JitPpc::addi(JitPpc::R6, JitPpc::R6, -1); // R6 = C - 1 = -!C
        *p++ = JitPpc::subf(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        *p++ = JitPpc::add(JitPpc::R3, JitPpc::R3, JitPpc::R6);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        break;
    }
    case 0x8: // TST
        *p++ = JitPpc::and_(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        emitLogicalFlags(JitPpc::R3);
        break;
    case 0x9: // TEQ
        *p++ = JitPpc::xor_(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        emitLogicalFlags(JitPpc::R3);
        break;
    case 0xA: // CMP
        *p++ = JitPpc::subfc(JitPpc::R3, JitPpc::R5, JitPpc::R4);
        emitUpdateSubNZCV(p);
        break;
    case 0xB: // CMN
        *p++ = JitPpc::addc(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        emitUpdateAddNZCV(p);
        break;
    case 0xC: // ORR
        *p++ = JitPpc::or_(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        emitLogicalFlags(JitPpc::R3);
        break;
    case 0xD: // MOV
        *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R5);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        emitLogicalFlags(JitPpc::R3);
        break;
    case 0xE: // BIC
        *p++ = JitPpc::andc(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        emitLogicalFlags(JitPpc::R3);
        break;
    case 0xF: // MVN
        *p++ = JitPpc::not_(JitPpc::R3, JitPpc::R5);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        emitLogicalFlags(JitPpc::R3);
        break;
    default:
        return false;
    }

    outCycles = 1;
    return true;
}

void *ArmPpcJit::compileStub(Interpreter *cpu, uint32_t instrPC, uint32_t opcode, bool thumb) {
    ensureSpace(MAX_STUB_WORDS);
    uint32_t *start = codePool + poolWordsUsed;
    uint32_t *p = start;
    int cpuIdx = cpu->arm7 ? 1 : 0;

#if NOODS_JIT_BLOCKS
    int nativeCycles = 0;
    // Native loads/stores/push/pop call slow-path helpers with bctrl, which clobbers LR. Blocks restore LR in
    // their epilogue; single-instruction stubs do not, so memory ops in stubs take the plain helper tail call.
    s_emitInstrR15 = instrPC + (thumb ? 4u : 8u);
    bool memStub = thumb
        ? (isNativeLoadThumb((uint16_t)opcode) || isNativeStoreThumb((uint16_t)opcode) || isNativePushPopThumb((uint16_t)opcode))
        : (isNativeLoadArm(cpu->arm7, opcode) || isNativeStoreArm(opcode) || isNativePushPopArm(opcode));
    bool ok = !memStub && (thumb ? emitNativeThumb(p, cpu, (uint16_t)opcode, nativeCycles)
                                 : emitNativeArm(p, cpu, opcode, nativeCycles));
    if (ok) {
        *p++ = JitPpc::li(JitPpc::R3, (int16_t)nativeCycles);
        *p++ = JitPpc::lmw(JitPpc::R28, JitPpc::R1, 16);
        *p++ = JitPpc::addi(JitPpc::R1, JitPpc::R1, 32);
        *p++ = JitPpc::blr();

        size_t words = (size_t)(p - start);
        assert(words <= MAX_STUB_WORDS);
        poolWordsUsed += words;
        JIT_STAT(stats.poolWordsUsed = poolWordsUsed);
        JIT_STAT(stats.stubCompiles[cpuIdx]++);
        JIT_STAT(stats.nativeInstrs[cpuIdx]++);
        JIT_STAT(if (!stats.firstStubAddr) {
            stats.firstStubAddr = start;
            stats.firstStubWords = words;
            stats.firstStubPc = instrPC;
            stats.firstStubOpcode = opcode;
        });
        flushCodeRange(start, words * sizeof(uint32_t));
        return start;
    }
#endif

    // Phase 1 (or Phase 2 non-native fallback) stub:
    //   r3 = r31 (Interpreter *cpu)
    //   r4 = opcode (already fetched in Interpreter::runOpcode)
    //   bctrl resolved Interpreter handler (or fallback helper)
    //   bctr  jit_exit
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    emitLoadImm32(p, JitPpc::R4, thumb ? (uint32_t)(uint16_t)opcode : opcode);
    uintptr_t helperAddr = thumb ? resolveThumbHandler((uint16_t)opcode)
                                 : resolveArmHandler(opcode);
    emitLoadImm32(p, JitPpc::R12, (uint32_t)helperAddr);
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctrl();

    uintptr_t exitAddr = (uintptr_t)&jit_exit;
    emitLoadImm32(p, JitPpc::R12, (uint32_t)exitAddr);
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctr();

    size_t words = (size_t)(p - start);
    assert(words <= MAX_STUB_WORDS);
    poolWordsUsed += words;
    JIT_STAT(stats.poolWordsUsed = poolWordsUsed);
    JIT_STAT(stats.stubCompiles[cpuIdx]++);
    JIT_STAT(stats.fallbackInstrs[cpuIdx]++);
    JIT_STAT(if (!stats.firstStubAddr) {
        stats.firstStubAddr = start;
        stats.firstStubWords = words;
        stats.firstStubPc = instrPC;
        stats.firstStubOpcode = opcode;
    });
    flushCodeRange(start, words * sizeof(uint32_t));
    return start;
}

static inline bool isTerminatorBranchThumb(bool arm7, uint16_t op) {
    // Conditional branch Bcc (0xD000..0xDDFF), unconditional B (0xE000..0xE7FF),
    // BX Rs (0x4700..0x477F), BLX Rs on ARM9 (0x4780..0x47FF),
    // BL suffix (0xF800..0xFFFF), or BLX suffix on ARM9 (0xE800..0xEFFF)
    if ((op & 0xF000u) == 0xD000u && (op & 0x0F00u) < 0x0E00u) return true;
    if ((op & 0xF800u) == 0xE000u) return true;
    if ((op & 0xFF80u) == 0x4700u) return true;
    if (!arm7 && (op & 0xFF80u) == 0x4780u) return true;
    if ((op & 0xF800u) == 0xF800u) return true;
    if (!arm7 && (op & 0xF800u) == 0xE800u) return true;
    return false;
}

static inline bool isTerminatorBranchArm(bool arm7, uint32_t op) {
    if ((op >> 28) == 0xFu) return false;
    // B / BL, BX Rm, or ARM9-only BLX Rm. ARM7 BLX stays with its interpreter handler.
    if ((op & 0x0E000000u) == 0x0A000000u) return true;
    if ((op & 0x0FFFFFF0u) == 0x012FFF10u) return true;
    if (!arm7 && (op & 0x0FFFFFF0u) == 0x012FFF30u) return true;
    return false;
}

// Unconditional ARM B/BL (cond == AL). Eligible to START a block (branch-only block).
static inline bool isUncondBranchArm(uint32_t op) {
    return (op >> 28) == 0xEu && (op & 0x0E000000u) == 0x0A000000u;
}

// Conditional or unconditional branch/BX/BLX may START a block (branch-only block).
static inline bool isBranchHeadArm(bool arm7, uint32_t op) {
    if ((op >> 28) == 0xFu) return false;
    if ((op & 0x0E000000u) == 0x0A000000u) return true;
    if ((op & 0x0FFFFFF0u) == 0x012FFF10u) return true;
    if (!arm7 && (op & 0x0FFFFFF0u) == 0x012FFF30u) return true;
    return false;
}

static inline bool isBranchHeadThumb(bool arm7, uint16_t op) {
    if ((op & 0xF000u) == 0xD000u && (op & 0x0F00u) < 0x0E00u) return true;
    if ((op & 0xF800u) == 0xE000u) return true;
    if ((op & 0xFF80u) == 0x4700u) return true;
    if (!arm7 && (op & 0xFF80u) == 0x4780u) return true;
    return false;
}


static inline bool isSingleLoadThumb(uint16_t op) {
    if ((op & 0xF800u) == 0x4800u) return true; // LDR Rd, [PC, #imm8]
    if ((op & 0xF000u) == 0x5000u && (op & 0x0E00u) >= 0x0600u) return true; // LDSB/LDR/LDRH/LDRB/LDSH Rd, [Rb, Ro]
    if ((op & 0xF800u) == 0x6800u) return true; // LDR Rd, [Rb, #imm5]
    if ((op & 0xF800u) == 0x7800u) return true; // LDRB Rd, [Rb, #imm5]
    if ((op & 0xF800u) == 0x8800u) return true; // LDRH Rd, [Rb, #imm5]
    if ((op & 0xF800u) == 0x9800u) return true; // LDR Rd, [SP, #imm8]
    return false;
}

static inline bool isSingleLoadArm(uint32_t op) {
    uint8_t rd = (uint8_t)((op >> 12) & 0xF);
    if (rd == 15) return false;
    uint8_t rn = (uint8_t)((op >> 16) & 0xF);
    bool pre = (op & (1u << 24)) != 0;
    bool wb  = (op & (1u << 21)) != 0;
    if (rn == 15 && (!pre || wb)) return false;

    // LDR / LDRB (bits 27..26 == 01, L == 1)
    if ((op & 0x0C100000u) == 0x04100000u) {
        if ((op & (1u << 25)) != 0) { // Register offset
            if ((op & (1u << 4)) != 0) return false;
            if ((op & 0xFu) == 15u) return false;
        }
        return true;
    }
    // LDRH / LDRSB / LDRSH (bits 27..25 == 000, L == 1, bits 7..4 == 1011/1101/1111)
    if ((op & 0x0E100090u) == 0x00100090u && (op & 0x00000060u) != 0) {
        if ((op & (1u << 22)) == 0) { // Register offset
            if ((op & 0x00000F00u) != 0) return false;
            if ((op & 0xFu) == 15u) return false;
        }
        return true;
    }
    return false;
}

static inline bool canTranslateNativeThumb(uint16_t opcode) {
    if (isNativeLoadThumb(opcode)) return true;
    if (isNativeStoreThumb(opcode)) return true;
    if (isNativePushPopThumb(opcode)) return true;
    switch (opcode >> 13) {
    case 0:
    case 1:
        return true;
    case 2:
        if ((opcode & 0xFC00u) == 0x4000u) {
            uint8_t aluOp = (uint8_t)((opcode >> 6) & 0xF);
            return (aluOp == 0x0 || aluOp == 0x1 || aluOp == 0x8 || aluOp == 0x9 || aluOp == 0xA ||
                    aluOp == 0xB || aluOp == 0xC || aluOp == 0xE || aluOp == 0xF);
        }
        if ((opcode & 0xFC00u) == 0x4400u) {
            uint8_t op = (uint8_t)((opcode >> 8) & 0x3);
            uint8_t rd = (uint8_t)(((opcode >> 4) & 0x8) | (opcode & 0x7));
            if (op == 0 && rd != 15) return true;
            if (op == 1) return true;
            if (op == 2 && rd != 15) return true;
        }
        return false;
    case 5:
        return ((opcode & 0xF800u) == 0xA000u ||
                (opcode & 0xF800u) == 0xA800u ||
                (opcode & 0xFF00u) == 0xB000u);
    case 7:
        return ((opcode & 0xF800u) == 0xF000u);
    default:
        return false;
    }
}

static inline bool canTranslateNativeArm(bool arm7, uint32_t opcode) {
    if (isNativeLoadArm(arm7, opcode)) return true;
    if (isNativeStoreArm(opcode)) return true;
    if (isNativePushPopArm(opcode)) return true;
    if ((opcode & 0x0C000000u) != 0x00000000u) return false;
    bool isImm = (opcode & (1u << 25)) != 0;
    if (!isImm) {
        if ((opcode & (1u << 4)) != 0) {
            if ((opcode & 0x0FFF0FF0u) == 0x016F0F10u) {
                uint8_t rd = (uint8_t)((opcode >> 12) & 0xF);
                uint8_t rm = (uint8_t)(opcode & 0xF);
                return (rd != 15 && rm != 15);
            }
            return false;
        }
        uint8_t shiftType = (uint8_t)((opcode >> 5) & 0x3);
        uint8_t shiftImm = (uint8_t)((opcode >> 7) & 0x1F);
        if (shiftType == 3 && shiftImm == 0) return false;
    }
    uint8_t rd = (uint8_t)((opcode >> 12) & 0xF);
    if (rd == 15) return false;
    uint8_t aluOp = (uint8_t)((opcode >> 21) & 0xF);
    bool setFlags = (opcode & (1u << 20)) != 0;
    if (aluOp >= 0x8 && aluOp <= 0xB && !setFlags) return false;
    if ((aluOp == 0x5 || aluOp == 0x6 || aluOp == 0x7) && setFlags) return false;
    (void)arm7;
    return true;
}

static inline uint32_t computeTailHash(const uint8_t *pcData0, uint16_t instrCount, bool thumb) {
    uint32_t h = 2166136261u;
    for (uint16_t m = 2; m < instrCount; m++) {
        uint32_t opM = thumb ? (uint32_t)U8TO16(pcData0 + (m - 2) * 2, 0)
                             : (uint32_t)U8TO32(pcData0 + (m - 2) * 4, 0);
        h = (h ^ opM) * 16777619u;
    }
    return h;
}

static inline bool instrReadsPCThumb(uint16_t op) {
    if ((op & 0xF800u) == 0xA000u) return true; // ADD Rd, PC, #imm8
    if ((op & 0xF800u) == 0xF000u) return true; // BL setup
    if ((op & 0xFC00u) == 0x4400u) {            // High register op
        uint8_t rd = (uint8_t)(((op >> 4) & 0x8) | (op & 0x7));
        uint8_t rs = (uint8_t)((op >> 3) & 0xF);
        if (rd == 15 || rs == 15) return true;
    }
    return false;
}

static inline bool instrReadsPCArm(uint32_t op) {
    if (isMemClassArm(op)) {
        if (((op >> 16) & 0xFu) == 15u) return true;
        bool regOff = ((op & 0x0C000000u) == 0x04000000u) ? ((op & (1u << 25)) != 0)
                                                          : ((op & (1u << 22)) == 0);
        return regOff && (op & 0xFu) == 15u;
    }
    if ((op & 0x0FFF0FF0u) == 0x016F0F10u) return false; // CLZ has 0xF in bits 19..16
    uint8_t aluOp = (uint8_t)((op >> 21) & 0xF);
    uint8_t rn = (uint8_t)((op >> 16) & 0xF);
    if (aluOp != 0xD && aluOp != 0xF && rn == 15) return true;
    bool isImm = (op & (1u << 25)) != 0;
    if (!isImm && (op & 0xFu) == 15) return true;
    return false;
}

// 4-bit CPSR flag mask: N=8, Z=4, C=2, V=1
static inline uint8_t thumbWrittenFlagsMask(uint16_t opcode) {
    switch (opcode >> 13) {
    case 0: {
        uint8_t subOp = (uint8_t)((opcode >> 11) & 0x3);
        uint8_t imm5  = (uint8_t)((opcode >> 6) & 0x1F);
        if (subOp == 3) return 0xFu; // ADD/SUB sets N,Z,C,V
        if (subOp == 0 && imm5 == 0) return 0xCu; // LSL #0 sets N,Z
        return 0xEu; // LSL/LSR/ASR sets N,Z,C
    }
    case 1: {
        uint8_t op = (uint8_t)((opcode >> 11) & 0x3);
        return (op == 0) ? 0xCu : 0xFu; // MOV sets N,Z; CMP/ADD/SUB sets N,Z,C,V
    }
    case 2:
        if ((opcode & 0xFC00u) == 0x4000u) {
            uint8_t aluOp = (uint8_t)((opcode >> 6) & 0xF);
            if (aluOp == 0x9 || aluOp == 0xA || aluOp == 0xB) return 0xFu; // NEG/CMP/CMN
            return 0xCu; // AND/EOR/TST/ORR/BIC/MVN
        }
        if ((opcode & 0xFC00u) == 0x4400u) {
            uint8_t op = (uint8_t)((opcode >> 8) & 0x3);
            return (op == 1) ? 0xFu : 0x0u; // High-reg CMP sets N,Z,C,V; ADD/MOV set none
        }
        return 0x0u;
    default:
        return 0x0u;
    }
}

static inline uint8_t armWrittenFlagsMask(uint32_t opcode) {
    if (isMemClassArm(opcode)) return 0x0u;
    if ((opcode & 0x0FFF0FF0u) == 0x016F0F10u) return 0x0u; // CLZ
    bool setFlags = (opcode & (1u << 20)) != 0;
    if (!setFlags) return 0x0u;
    uint8_t aluOp = (uint8_t)((opcode >> 21) & 0xF);
    if (aluOp == 0x2 || aluOp == 0x3 || aluOp == 0x4 || aluOp == 0xA || aluOp == 0xB) {
        return 0xFu; // SUBS/RSBS/ADDS/CMP/CMN set N,Z,C,V
    }
    bool isImm = (opcode & (1u << 25)) != 0;
    if (isImm) {
        uint8_t rot = (uint8_t)((opcode >> 7) & 0x1Eu);
        return rot ? 0xEu : 0xCu;
    } else {
        uint8_t shiftType = (uint8_t)((opcode >> 5) & 0x3);
        uint8_t shiftImm  = (uint8_t)((opcode >> 7) & 0x1F);
        return (shiftType == 0 && shiftImm == 0) ? 0xCu : 0xEu;
    }
}

static inline bool armReadsCarry(uint32_t opcode) {
    if (isMemClassArm(opcode)) return false;
    if ((opcode & 0x0FFF0FF0u) == 0x016F0F10u) return false;
    uint8_t aluOp = (uint8_t)((opcode >> 21) & 0xF);
    return (aluOp == 0x5 || aluOp == 0x6 || aluOp == 0x7);
}

#define BLK_N(x)    ((x) & 0xFFu)
#define BLK_PREF(x) ((x) & 0xFFu)
#define BLK_LOOP(x) (((x) >> 8) & 0xFFu)

// Purity classification for poll-loop skipping. 1 = pure (register-only), 2 = pure iff the load address is
// side-effect free (checked at runtime), 0 = impure (stores, system/coprocessor, exception return, MSR/MRS, etc.).
static inline int armPurity(uint32_t op) {
    if ((op >> 28) == 0xFu) return 0;
    if ((op & 0x0E000000u) == 0x0A000000u) return 1;             // B / BL
    if ((op & 0x0E000000u) == 0x08000000u) return 0;             // LDM / STM
    if ((op & 0x0C000000u) == 0x0C000000u) return 0;             // coprocessor / SWI
    if ((op & 0x0C000000u) == 0x04000000u) {                     // single data transfer
        if (!(op & 0x00100000u)) return 0;                       // store
        if (op & 0x02000000u) return 0;                          // register offset: not analysed
        return 2;
    }
    if ((op & 0x0E000090u) == 0x00000090u) {                     // multiply / swap / halfword transfer
        if ((op & 0x60u) == 0) {
            if ((op & 0x0FC000F0u) == 0x00000090u) return 1;     // MUL / MLA
            if ((op & 0x0F8000F0u) == 0x00800090u) return 1;     // UMULL .. SMLAL
            return 0;                                            // SWP / SWPB
        }
        if (!(op & 0x00100000u)) return 0;                       // halfword store
        if (!(op & 0x00400000u)) return 0;                       // register offset
        return 2;
    }
    uint32_t dpOp = (op >> 21) & 0xFu;                           // data processing / PSR transfer
    bool s = (op & 0x00100000u) != 0;
    if (dpOp >= 8u && dpOp <= 11u && !s) return 0;               // MRS / MSR / BX / CLZ / ...
    if (s && ((op >> 12) & 0xFu) == 15u) return 0;               // SPSR restore form
    return 1;
}

// Runtime check for class-2 loads: the address must not touch I/O with read side effects.
static inline bool armLoadAddrOk(uint32_t op, uint32_t base) {
    bool halfword = (op & 0x0E000090u) == 0x00000090u;
    uint32_t off = halfword ? (((op >> 4) & 0xF0u) | (op & 0xFu)) : (op & 0xFFFu);
    uint32_t addr = (op & 0x01000000u) ? (((op & 0x00800000u) ? base + off : base - off)) : base;
    switch (addr >> 24) {
        case 0x00: case 0x02: case 0x03: case 0x05: case 0x06: case 0x07:
            return true;
        case 0x04:
            return addr == 0x04000004u || addr == 0x04000006u || addr == 0x04000180u; // DISPSTAT, VCOUNT, IPC_SYNC
        default:
            return false;
    }
}

static inline bool armPure(uint32_t op, uint32_t base) {
    int p = armPurity(op);
    return p == 1 || (p == 2 && armLoadAddrOk(op, base));
}

static inline bool isAnyMemThumb(uint16_t op) {
    return isNativeLoadThumb(op) || isNativeStoreThumb(op) || isNativePushPopThumb(op);
}

static inline bool isAnyStoreOp(bool arm7, uint32_t op, bool thumb) {
    (void)arm7;
    if (thumb) {
        return isNativeStoreThumb((uint16_t)op) || ((op & 0xFE00u) == 0xB400u && (op & 0x01FFu) != 0u);
    }
    return isNativeStoreArm(op) || ((op & 0xFFFF0000u) == 0xE92D0000u && (op & 0xFFFFu) != 0u);
}

static inline uint32_t writtenGuestRegsMask(uint32_t op, bool thumb) {
    if (thumb) {
        uint16_t t = (uint16_t)op;
        if (isNativeStoreThumb(t)) return 0u;
        if ((t & 0xFE00u) == 0xB400u) return (1u << 13);
        if ((t & 0xFF00u) == 0xBC00u) return (t & 0xFFu) | (1u << 13);
        if ((t & 0xF800u) == 0x4800u || (t & 0xF800u) == 0x9800u) return 1u << ((t >> 8) & 7u);
        if (isNativeLoadThumb(t)) return 1u << (t & 7u);
        switch (t >> 13) {
        case 0: return 1u << (t & 7u);
        case 1: return (((t >> 11) & 3u) == 1u) ? 0u : (1u << ((t >> 8) & 7u));
        case 2:
            if ((t & 0xFC00u) == 0x4000u) {
                uint8_t a = (uint8_t)((t >> 6) & 0xFu);
                return (a == 8u || a == 10u || a == 11u) ? 0u : (1u << (t & 7u));
            }
            if ((t & 0xFC00u) == 0x4400u) {
                uint8_t o = (uint8_t)((t >> 8) & 3u);
                return (o == 1u) ? 0u : (1u << ((((t >> 4) & 8u) | (t & 7u))));
            }
            return 0u;
        case 5:
            if ((t & 0xFF00u) == 0xB000u) return (1u << 13);
            return 1u << ((t >> 8) & 7u);
        case 7:
            return (1u << 14);
        default:
            return 0xFFFFu;
        }
    }
    if ((op & 0xFFFF0000u) == 0xE92D0000u) return (1u << 13);
    if ((op & 0xFFFF0000u) == 0xE8BD0000u) return (op & 0xFFFFu) | (1u << 13);
    if (isNativeStoreArm(op)) {
        bool pre = (op & (1u << 24)) != 0;
        bool wb = ((op & (1u << 21)) != 0) || !pre;
        return wb ? (1u << ((op >> 16) & 0xFu)) : 0u;
    }
    if (isMemClassArm(op)) return 1u << ((op >> 12) & 0xFu);
    if ((op & 0x0FFF0FF0u) == 0x016F0F10u) return 1u << ((op >> 12) & 0xFu);
    uint8_t aluOp = (uint8_t)((op >> 21) & 0xFu);
    if (aluOp >= 8u && aluOp <= 11u) return 0u;
    return 1u << ((op >> 12) & 0xFu);
}

bool ArmPpcJit::compileBlock(Interpreter *cpu, uint32_t instrPC, uint32_t opcode0, bool thumb, BlockEntry &entry) {
    int cpuIdx = cpu->arm7 ? 1 : 0;
    const int16_t step = thumb ? 2 : 4;
    const int16_t offR15   = (int16_t)((uintptr_t)&cpu->registersUsr[15] - (uintptr_t)cpu);
    const int16_t offPcDat = (int16_t)((uintptr_t)&cpu->pcData - (uintptr_t)cpu);
    const int16_t offPipe0 = (int16_t)((uintptr_t)&cpu->pipeline[0] - (uintptr_t)cpu);
    const int16_t offPipe1 = (int16_t)((uintptr_t)&cpu->pipeline[1] - (uintptr_t)cpu);

    // Pass 1: Scan guest instruction stream before touching codePool
    bool firstIsNative = thumb ? canTranslateNativeThumb((uint16_t)opcode0) : canTranslateNativeArm(cpu->arm7, opcode0);
    bool firstIsLoad   = !firstIsNative && (thumb ? isSingleLoadThumb((uint16_t)opcode0) : isSingleLoadArm(opcode0));
    bool firstIsBranch = !firstIsNative && !firstIsLoad && (thumb ? isBranchHeadThumb(cpu->arm7, (uint16_t)opcode0) : isBranchHeadArm(cpu->arm7, opcode0));
    bool firstIsPcLoad = !thumb && !firstIsNative && !firstIsLoad && !firstIsBranch && isPcDestLoadTerminatorArm(cpu->arm7, opcode0);
    if (!firstIsNative && !firstIsLoad && !firstIsBranch && !firstIsPcLoad) {
        return false;
    }
    const bool firstIsTerm = firstIsBranch || firstIsPcLoad;

    uint32_t opcode1 = cpu->pipeline[0];
    uint32_t curPageEpoch = pageEpoch[cpuIdx][(instrPC >> 12) & (PAGE_EPOCH_SIZE - 1)];
    uint32_t r15After0 = cpu->registersUsr[15];
    uint8_t *pcData0 = cpu->pcData;

    uint32_t opList[MAX_BLOCK_INSTRS];
    opList[0] = opcode0;
    uint16_t count = 1;
    bool hasBranchTerm = false;
    bool hasPcLoadTerm = false;
    uint32_t branchOpcode = 0;
    bool sawStore = !firstIsTerm && isAnyStoreOp(cpu->arm7, opcode0, thumb);
    int postStoreLeft = sawStore ? 2 : 99;

    if (firstIsBranch) {
        hasBranchTerm = true;
        branchOpcode = opcode0;
    } else if (firstIsPcLoad) {
        hasPcLoadTerm = true;
        branchOpcode = opcode0;
    }

    for (uint16_t m = 1; m < (firstIsTerm ? 1 : MAX_BLOCK_INSTRS) && postStoreLeft > 0; m++) {
        uint32_t simR15 = r15After0 + m * (uint32_t)step;
        if ((simR15 & 0xFFFu) < 8u || (simR15 & 0xFFFu) > 0xFA0u) break;

        uint32_t opM = (m == 1) ? opcode1
                                : (thumb ? (uint32_t)U8TO16(pcData0 + (m - 2) * 2, 0)
                                         : (uint32_t)U8TO32(pcData0 + (m - 2) * 4, 0));

        if (!thumb && !cpu->arm7 && (opM & 0x0F000010u) == 0x0E000010u) {
            break;
        }

        if (!thumb && (opM >> 28) != 0xEu) {
            if (isTerminatorBranchArm(cpu->arm7, opM)) {
                hasBranchTerm = true;
                branchOpcode = opM;
                count++;
            } else if (isPcDestLoadTerminatorArm(cpu->arm7, opM)) {
                hasPcLoadTerm = true;
                branchOpcode = opM;
                count++;
            }
            break;
        }

        if (thumb ? canTranslateNativeThumb((uint16_t)opM) : canTranslateNativeArm(cpu->arm7, opM)) {
            opList[count++] = opM;
            if (sawStore) postStoreLeft--;
            if (!sawStore && isAnyStoreOp(cpu->arm7, opM, thumb)) {
                sawStore = true;
                postStoreLeft = 2; // Match real ARM 2-stage instruction prefetch across self-modifying stores!
            }
            continue;
        }

        if (thumb ? isTerminatorBranchThumb(cpu->arm7, (uint16_t)opM) : isTerminatorBranchArm(cpu->arm7, opM)) {
            hasBranchTerm = true;
            branchOpcode = opM;
            count++;
        } else if (!thumb && isPcDestLoadTerminatorArm(cpu->arm7, opM)) {
            hasPcLoadTerm = true;
            branchOpcode = opM;
            count++;
        }
        break;
    }

    if (count < 2 && !firstIsTerm) {
        return false;
    }

    // Pass 2: Backward CPSR flag-liveness analysis across the native prefix
    const bool hasTerm = hasBranchTerm || hasPcLoadTerm;
    uint16_t nativeCount = hasTerm ? (uint16_t)(count - 1) : count;
    uint16_t nativeStart = firstIsLoad ? 1u : 0u;
    if (nativeCount < nativeStart || (!hasTerm && nativeCount <= nativeStart)) {
        return false;
    }
    bool emitFlagsList[MAX_BLOCK_INSTRS];
    uint8_t liveFlags = 0xFu;
    for (int m = (int)nativeCount - 1; m >= (int)nativeStart; m--) {
        uint32_t opM = opList[m];
        uint8_t w = thumb ? thumbWrittenFlagsMask((uint16_t)opM) : armWrittenFlagsMask(opM);
        emitFlagsList[m] = ((w & liveFlags) != 0);
        liveFlags &= (uint8_t)~w;
        if (!thumb && armReadsCarry(opM)) {
            liveFlags |= 0x2u;
        }
    }

    int16_t totalDelta = (int16_t)((count - 1) * step);

    // Analyse memory properties & self-loop eligibility
    bool anyMemInPrefix = firstIsLoad;
    int memLoadCount = firstIsLoad ? 1 : 0;
    int singleHalfPollBaseReg = -1;
    uint32_t bodyWrittenRegs = 0;
    for (uint16_t m = nativeStart; m < nativeCount; m++) {
        uint32_t opM = opList[m];
        bodyWrittenRegs |= writtenGuestRegsMask(opM, thumb);
        bool isMem = thumb ? isAnyMemThumb((uint16_t)opM) : isMemClassArm(opM);
        if (isMem) {
            anyMemInPrefix = true;
            memLoadCount++;
            if (thumb && (opM & 0xF800u) == 0x8800u) {
                singleHalfPollBaseReg = (int)((opM >> 3) & 7u);
            } else if (!thumb && (opM & 0x0E5000F0u) == 0x005000B0u) {
                uint8_t rn = (uint8_t)((opM >> 16) & 0xFu);
                if (rn < 8u) singleHalfPollBaseReg = (int)rn;
                else singleHalfPollBaseReg = -2;
            } else {
                singleHalfPollBaseReg = -2;
            }
        }
    }

    // Check if block ends with a backward branch right back to instrPC (hardware self-loop!)
    bool wantSelfLoop = false;
    bool isInlineSamePageBr = false;
    if (hasBranchTerm) {
        if (!thumb && (branchOpcode & 0x0E000000u) == 0x0A000000u && (branchOpcode >> 28) != 0xFu) {
            uint32_t brAddr = instrPC + (uint32_t)totalDelta;
            int32_t off = (int32_t)(branchOpcode << 8) >> 6;
            uint32_t target = (brAddr + 8u + (uint32_t)off) & ~3u;
            uint32_t newR15 = target + 4u;
            int32_t tDelta = (int32_t)newR15 - (int32_t)(instrPC + 8u);
            if ((((target ^ instrPC) | (newR15 ^ instrPC)) >> 12) == 0 && tDelta >= -0x800 && tDelta <= 0x7FF) {
                isInlineSamePageBr = true;
                if (target == instrPC && !firstIsLoad && !sawStore && (branchOpcode & (1u << 24)) == 0u) {
                    wantSelfLoop = true;
                }
            }
        } else if (thumb && (((branchOpcode & 0xF000u) == 0xD000u && (branchOpcode & 0x0F00u) < 0x0E00u) ||
                             (branchOpcode & 0xF800u) == 0xE000u)) {
            uint32_t brAddr = instrPC + (uint32_t)totalDelta;
            bool isCond = (branchOpcode & 0xF000u) == 0xD000u;
            int32_t off = isCond ? ((int32_t)(int8_t)(branchOpcode & 0xFFu) << 1)
                                 : (((int32_t)(int16_t)((uint16_t)branchOpcode << 5)) >> 4);
            uint32_t target = (brAddr + 4u + (uint32_t)off) & ~1u;
            uint32_t newR15 = target + 2u;
            int32_t tDelta = (int32_t)newR15 - (int32_t)(instrPC + 4u);
            if ((((target ^ instrPC) | (newR15 ^ instrPC)) >> 12) == 0 && tDelta >= -0x800 && tDelta <= 0x7FF) {
                isInlineSamePageBr = true;
                if (target == instrPC && !firstIsLoad && !sawStore) {
                    wantSelfLoop = true;
                }
            }
        }
    }

    // Pass 3: Emit PowerPC code into codePool
    ensureSpace(MAX_BLOCK_WORDS);
    uint32_t *start = codePool + poolWordsUsed;
    uint32_t *p = start;
    uint16_t prefixCycles = firstIsLoad ? (uint16_t)(cpu->arm7 ? 3 : 1) : 0;
    int16_t appliedPcDelta = 0;

    bool helperInBlock = firstIsLoad || hasPcLoadTerm || anyMemInPrefix;
    if (hasBranchTerm) {
        if (!thumb && ((branchOpcode & 0x0FFFFFF0u) == 0x012FFF10u || (branchOpcode & 0x0FFFFFF0u) == 0x012FFF30u))
            helperInBlock = true;
        if (thumb && (branchOpcode & 0xFF00u) == 0x4700u)
            helperInBlock = true;
    }

    if (firstIsLoad) {
        *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
        emitLoadImm32(p, JitPpc::R4, thumb ? (uint32_t)(uint16_t)opcode0 : opcode0);
        uintptr_t loadHelper = thumb ? resolveThumbHandler((uint16_t)opcode0)
                                     : resolveArmHandler(opcode0);
        emitLoadImm32(p, JitPpc::R12, (uint32_t)loadHelper);
        *p++ = JitPpc::mtctr(JitPpc::R12);
        *p++ = JitPpc::bctrl();
    }

    uint32_t *loopTop = nullptr;
    if (wantSelfLoop) {
        *p++ = JitPpc::li(JitPpc::R29, 0); // taken iteration accumulator = 0
        loopTop = p;
    }

    for (uint16_t m = nativeStart; m < nativeCount; m++) {
        uint32_t opM = opList[m];
        bool readsPC = thumb ? instrReadsPCThumb((uint16_t)opM) : instrReadsPCArm(opM);
        if (readsPC) {
            if (loopTop) {
                emitLoadImm32(p, JitPpc::R3, instrPC + (uint32_t)((m + 2) * step));
                *p++ = JitPpc::stw(JitPpc::R3, JitPpc::R31, offR15);
            } else if (m > 0) {
                int16_t neededDelta = (int16_t)(m * step);
                if (appliedPcDelta != neededDelta) {
                    *p++ = JitPpc::lwz(JitPpc::R3, JitPpc::R31, offR15);
                    *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, (int16_t)(neededDelta - appliedPcDelta));
                    *p++ = JitPpc::stw(JitPpc::R3, JitPpc::R31, offR15);
                    appliedPcDelta = neededDelta;
                }
            }
        }
        int cm = 0;
        s_emitInstrR15 = instrPC + (uint32_t)((m + 2) * step);
        bool okM = thumb ? emitNativeThumb(p, cpu, (uint16_t)opM, cm, emitFlagsList[m])
                         : emitNativeArm(p, cpu, opM, cm, emitFlagsList[m]);
        assert(okM);
        (void)okM;
        prefixCycles = (uint16_t)(prefixCycles + cm);
    }

    int16_t remDelta = (int16_t)(totalDelta - appliedPcDelta);
    bool emittedLoop = false;

    if (hasPcLoadTerm) {
        if (!emitPcDestLoadTerminatorArm(p, cpu, branchOpcode, instrPC, totalDelta, prefixCycles, helperInBlock)) {
            return false;
        }
    } else if (hasBranchTerm && !thumb && emitInlineBranchArm(p, cpu, branchOpcode, instrPC, totalDelta, prefixCycles, helperInBlock, loopTop)) {
        emittedLoop = (loopTop != nullptr);
    } else if (hasBranchTerm && thumb && emitInlineBranchThumb(p, cpu, (uint16_t)branchOpcode, instrPC, totalDelta, prefixCycles, helperInBlock, loopTop)) {
        emittedLoop = (loopTop != nullptr);
    } else if (hasBranchTerm && !thumb && emitInlineBxArm(p, cpu, branchOpcode, instrPC, totalDelta, prefixCycles, helperInBlock)) {
        // Inline ARM BX / BLX refill
    } else if (hasBranchTerm && thumb && emitInlineBxThumb(p, cpu, (uint16_t)branchOpcode, instrPC, totalDelta, prefixCycles, helperInBlock)) {
        // Inline Thumb BX / BLX refill
    } else if (hasBranchTerm) {
        *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
        emitLoadImm32(p, JitPpc::R4, branchOpcode);
        *p++ = JitPpc::li(JitPpc::R5, remDelta);
        *p++ = JitPpc::li(JitPpc::R6, totalDelta);
        *p++ = JitPpc::li(JitPpc::R7, (int16_t)prefixCycles);
        if (helperInBlock) {
            *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 36);
            *p++ = JitPpc::mtlr(JitPpc::R0);
        }
        uintptr_t helperAddr = (uintptr_t)(thumb ? (void *)&ArmPpcJit::execBranchThumb
                                                 : (void *)&ArmPpcJit::execBranchArm);
        emitLoadImm32(p, JitPpc::R12, (uint32_t)helperAddr);
        *p++ = JitPpc::mtctr(JitPpc::R12);
        *p++ = JitPpc::lmw(JitPpc::R28, JitPpc::R1, 16);
        *p++ = JitPpc::addi(JitPpc::R1, JitPpc::R1, 32);
        *p++ = JitPpc::bctr();
    } else {
        if (remDelta != 0) {
            *p++ = JitPpc::lwz(JitPpc::R3, JitPpc::R31, offR15);
            *p++ = JitPpc::addi(JitPpc::R3, JitPpc::R3, remDelta);
            *p++ = JitPpc::stw(JitPpc::R3, JitPpc::R31, offR15);
        }
        *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, offPcDat);
        *p++ = JitPpc::addi(JitPpc::R4, JitPpc::R4, totalDelta);
        *p++ = JitPpc::stw(JitPpc::R4, JitPpc::R31, offPcDat);
        *p++ = JitPpc::addi(JitPpc::R5, JitPpc::R4, (int16_t)(-step));
        if (thumb) {
            *p++ = JitPpc::lhbrx(JitPpc::R6, 0, JitPpc::R5);
            *p++ = JitPpc::lhbrx(JitPpc::R7, 0, JitPpc::R4);
        } else {
            *p++ = JitPpc::lwbrx(JitPpc::R6, 0, JitPpc::R5);
            *p++ = JitPpc::lwbrx(JitPpc::R7, 0, JitPpc::R4);
        }
        *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offPipe0);
        *p++ = JitPpc::stw(JitPpc::R7, JitPpc::R31, offPipe1);
        *p++ = JitPpc::li(JitPpc::R3, (int16_t)prefixCycles);
        if (helperInBlock) {
            *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 36);
            *p++ = JitPpc::mtlr(JitPpc::R0);
        }
        *p++ = JitPpc::lmw(JitPpc::R28, JitPpc::R1, 16);
        *p++ = JitPpc::addi(JitPpc::R1, JitPpc::R1, 32);
        *p++ = JitPpc::blr();
    }

    size_t words = (size_t)(p - start);
    assert(words <= MAX_BLOCK_WORDS);
    poolWordsUsed += words;
    JIT_STAT(stats.poolWordsUsed = poolWordsUsed);
    JIT_STAT(stats.blockCompiles[cpuIdx]++);
    JIT_STAT(stats.nativeInstrs[cpuIdx] += (nativeCount - nativeStart));
    JIT_STAT(if (firstIsLoad) stats.fallbackInstrs[cpuIdx]++);

    JIT_STAT(if (!stats.firstBlockAddr) {
        stats.firstBlockAddr = start;
        stats.firstBlockWords = words;
        stats.firstBlockPc = instrPC;
    });
    flushCodeRange(start, words * sizeof(uint32_t));

    // Mark exact 64-byte code lines covered by [instrPC .. instrPC + (count + 2) * step]
    if ((instrPC >> 27) == 0u) {
        uint32_t endCodeAddr = instrPC + (uint32_t)((count + 2) * step);
        for (uint32_t line = (instrPC >> 6); line <= (endCodeAddr >> 6); line++) {
            codeLineBits[line & (CODE_LINE_WORDS - 1)] |= (1u << (line & 31u));
        }
    }
    size_t pageIdx = (instrPC >> 12) & (PAGE_EPOCH_SIZE - 1);
    pageHasBlockBits[cpuIdx][pageIdx >> 5] |= (1u << (pageIdx & 31));

    uint8_t loopCyclesVal = emittedLoop ? (uint8_t)(prefixCycles + 3u) : 0u;
    uint16_t loopKindFlags = 0u;
    if (emittedLoop) {
        if (!anyMemInPrefix) {
            loopKindFlags = (1u << 10); // Pure register loop: independent of other CPU!
        } else if (memLoadCount == 1 && singleHalfPollBaseReg >= 0 &&
                   (bodyWrittenRegs & (1u << singleHalfPollBaseReg)) == 0u) {
            loopKindFlags = (uint16_t)((3u << 10) | ((uint16_t)singleHalfPollBaseReg << 12));
        } else {
            loopKindFlags = (2u << 10); // Shared RAM / IPCSYNC loop
        }
    }
    bool gbaPureAlu = !anyMemInPrefix && !sawStore && !hasPcLoadTerm && (!hasBranchTerm || isInlineSamePageBr);
    uint16_t metaFlags = (gbaPureAlu ? 0x0100u : 0u) |
                         (hasBranchTerm ? 0x0200u : 0u) |
                         loopKindFlags;

    entry.pc = instrPC;
    entry.opcode0 = opcode0;
    entry.opcode1 = opcode1;
    entry.tailHash = computeTailHash(pcData0, count, thumb);
    entry.epochAndFlags = (currentEpoch << 2) | (thumb ? 3u : 1u);
    entry.pageEpoch = curPageEpoch;
    bool blockPure = false;
#if NOODS_JIT_POLL_SKIP
    if (!thumb) {
        blockPure = hasPcLoadTerm ? false : (!hasBranchTerm || armPurity(branchOpcode) == 1);
        for (uint16_t m = 0; blockPure && m < nativeCount; m++) {
            int pm = armPurity(opList[m]);
            blockPure = (m == 0) ? (pm == 1 || pm == 2) : (pm == 1);
        }
    }
#endif
    entry.instrCount = (uint16_t)((count & 0xFFu) | metaFlags | (blockPure ? 0x8000u : 0u));
    uint16_t basePref = hasTerm ? prefixCycles : (uint16_t)(prefixCycles - 1);
    entry.prefixCycles = (uint16_t)((basePref & 0xFFu) | ((uint16_t)loopCyclesVal << 8));
    entry.code = start;
    return true;
}

template <bool Track>
FORCE_INLINE int ArmPpcJit::tryExecuteBlock(Interpreter *cpu, uint32_t instrPC, uint32_t opcode, bool thumb,
                                            bool &outIsNative0, bool *outPure, uint32_t remIndep, uint32_t remShared) {
    if (Track && outPure) *outPure = false;
    if (!cpu->pcData) {
        outIsNative0 = thumb ? canTranslateNativeThumb((uint16_t)opcode) : canTranslateNativeArm(cpu->arm7, opcode);
        return 0;
    }

    int cpuIdx = cpu->arm7 ? 1 : 0;
    size_t bIdx = ((instrPC >> (thumb ? 1 : 2)) ^ (instrPC >> 11)) & (BLOCK_TABLE_SIZE - 1);
    BlockEntry &bEntry = blockTable[cpuIdx][bIdx];
    uint32_t opcode1 = cpu->pipeline[0];
    uint32_t curPageEpoch = pageEpoch[cpuIdx][(instrPC >> 12) & (PAGE_EPOCH_SIZE - 1)];
    uint32_t expectedFlags = (currentEpoch << 2) | (thumb ? 3u : 1u);

    bool hit = (bEntry.pc == instrPC &&
                bEntry.opcode0 == opcode &&
                bEntry.opcode1 == opcode1 &&
                bEntry.epochAndFlags == expectedFlags);

    if (__builtin_expect(hit, 1)) {
        if (__builtin_expect(bEntry.pageEpoch != curPageEpoch, 0)) {
            if (BLK_N(bEntry.instrCount) <= 2 || computeTailHash(cpu->pcData, BLK_N(bEntry.instrCount), thumb) == bEntry.tailHash) {
                bEntry.pageEpoch = curPageEpoch;
            } else {
                hit = false;
            }
        }
    }

    if (__builtin_expect(!hit, 0)) {
        if (s_candPc[cpuIdx][bIdx] != instrPC) {
            s_candPc[cpuIdx][bIdx] = instrPC;
            s_candHits[cpuIdx][bIdx] = 1;
            return 0;
        }
        uint8_t ch = s_candHits[cpuIdx][bIdx];
        if (ch == 0xFFu)
            return 0;
        if (++ch < 3u) {
            s_candHits[cpuIdx][bIdx] = ch;
            return 0;
        }

        bool firstIsNative = thumb ? canTranslateNativeThumb((uint16_t)opcode) : canTranslateNativeArm(cpu->arm7, opcode);
        outIsNative0 = firstIsNative;
        bool firstIsLoad   = !firstIsNative && (thumb ? isSingleLoadThumb((uint16_t)opcode) : isSingleLoadArm(opcode));
        bool firstIsBranch = !firstIsNative && !firstIsLoad && (thumb ? isBranchHeadThumb(cpu->arm7, (uint16_t)opcode) : isBranchHeadArm(cpu->arm7, opcode));
        bool firstIsPcLoad = !thumb && !firstIsNative && !firstIsLoad && !firstIsBranch && isPcDestLoadTerminatorArm(cpu->arm7, opcode);
        if (!firstIsNative && !firstIsLoad && !firstIsBranch && !firstIsPcLoad) {
            s_candHits[cpuIdx][bIdx] = 0xFFu;
            return 0;
        }
        const bool firstIsTerm = firstIsBranch || firstIsPcLoad;
        if (firstIsTerm) {
            bool selfLoop = thumb
                ? ((opcode & 0xFFFFu) == 0xE7FEu || ((opcode & 0xF0FFu) == 0xD0FEu && ((opcode >> 8) & 0xFu) < 0xEu))
                : ((opcode & 0x0FFFFFFFu) == 0x0AFFFFFEu);
            if (!selfLoop) {
                s_candHits[cpuIdx][bIdx] = 0xFFu;
                return 0;
            }
        }

        uint32_t r15After0 = cpu->registersUsr[15];
        if ((r15After0 & 0xFFFu) < 8u || (r15After0 & 0xFFFu) > 0xF80u) {
            s_candHits[cpuIdx][bIdx] = 0xFFu;
            return 0;
        }

        if (!firstIsTerm) {
            if (thumb) {
                if (!canTranslateNativeThumb((uint16_t)opcode1) && !isTerminatorBranchThumb(cpu->arm7, (uint16_t)opcode1)) {
                    s_candHits[cpuIdx][bIdx] = 0xFFu;
                    return 0;
                }
            } else {
                if (((opcode1 >> 28) != 0xEu || !canTranslateNativeArm(cpu->arm7, opcode1)) &&
                    !isTerminatorBranchArm(cpu->arm7, opcode1) &&
                    !isPcDestLoadTerminatorArm(cpu->arm7, opcode1)) {
                    s_candHits[cpuIdx][bIdx] = 0xFFu;
                    return 0;
                }
            }
        }

        s_candHits[cpuIdx][bIdx] = 3u;
        if (!compileBlock(cpu, instrPC, opcode, thumb, bEntry)) {
            s_candHits[cpuIdx][bIdx] = 0xFFu;
            JIT_STAT(stats.blockCompileFails[cpuIdx]++);
            return 0;
        }
    }

    // Verify scheduler cycle budget so block never crosses a scheduled hardware event boundary
    Core *core = cpu->core;
    uint32_t nextEventCycles = core->events[0].cycles;
    uint32_t shift = (cpu->arm7 && !core->gbaMode) ? 1u : 0u;
    uint32_t scaledPrefix = (uint32_t)BLK_PREF(bEntry.prefixCycles) << shift;
    uint32_t startCycles = core->gbaMode ? cpu->cycles : core->globalCycles;
    if (__builtin_expect(startCycles + scaledPrefix >= nextEventCycles, 0)) {
        outIsNative0 = thumb ? canTranslateNativeThumb((uint16_t)opcode) : canTranslateNativeArm(cpu->arm7, opcode);
        return 0;
    }

    uint32_t maxIters = 1u;
    uint8_t loopCyc = (uint8_t)BLK_LOOP(bEntry.prefixCycles);
    if (loopCyc != 0u) {
        uint8_t kind = (uint8_t)((bEntry.instrCount >> 10) & 3u);
        bool useIndep = (kind == 1u);
        if (kind == 3u) {
            uint32_t baseAddr = cpu->registersUsr[(bEntry.instrCount >> 12) & 7u];
            useIndep = ((baseAddr - 0x04000000u) < 0x140u); // DISPSTAT / VCOUNT / KEYINPUT!
        }
        uint32_t rem = useIndep ? remIndep : remShared;
        if (rem > 1u) {
            maxIters = (rem - 1u) / (uint32_t)loopCyc + 1u;
            if (maxIters > 4096u) maxIters = 4096u;
        }
    }

    JIT_STAT(stats.blockHits[cpuIdx]++);
    JIT_STAT(stats.blockInstrsExecuted[cpuIdx] += BLK_N(bEntry.instrCount));
    PROF_MARK(thumb ? 0 : (isSingleLoadArm(opcode) ? 8 : 0));
    if (Track && outPure) {
        int p = thumb ? 0 : armPurity(opcode);
        *outPure = (bEntry.instrCount & 0x8000) && (p == 1 || (p == 2 && armLoadAddrOk(opcode, cpu->registersUsr[(opcode >> 16) & 0xFu])));
    }
    return jit_enter(bEntry.code, cpu, maxIters);
}

bool ArmPpcJit::tryExecuteGbaFast(Interpreter *cpu, uint16_t waitCnt, uint32_t targetCycles) {
    if (!initialized) init();
    Core *core = cpu->core;

    bool thumb = (cpu->cpsr & (1u << 5)) != 0u;
    uint32_t r15Before = cpu->registersUsr[15];
    uint32_t instrPC = r15Before - (thumb ? 2u : 4u);
    size_t bIdx = ((instrPC >> (thumb ? 1 : 2)) ^ (instrPC >> 11)) & (BLOCK_TABLE_SIZE - 1);
    if (s_candPc[1][bIdx] == instrPC && s_candHits[1][bIdx] == 0xFFu)
        return false;

    uint8_t region = (uint8_t)(instrPC >> 24);

    uint32_t seqCost = 1u;
    uint32_t deltaTaken = 0u;
    if (region >= 0x08u) {
        if ((waitCnt & (1u << 14)) || region >= 0x0Eu) return false;
        if ((instrPC & 0x1F000u) == 0u || ((instrPC + 64u) & 0x1F000u) == 0u) return false;
        uint32_t s = (uint32_t)GbaBus::romWaitS(instrPC, waitCnt);
        uint32_t n = (uint32_t)GbaBus::romWaitN(instrPC, waitCnt);
        seqCost = thumb ? s : (2u * s);
        deltaTaken = n - s; // n >= s? Note: romWaitN can be 3 while romWaitS is 5 for WS1/2! Handle signed below!
        int32_t signedDelta = (int32_t)n - (int32_t)s;
        if (signedDelta < 0) return false; // Rare inverted waitstate config: fall back cleanly to runGbaOpcode
        deltaTaken = (uint32_t)signedDelta;
    } else {
        if (region == 0x04u) return false;
        if (region == 0x02u) seqCost = thumb ? 3u : 6u;
        else if (region == 0x05u || region == 0x06u) seqCost = thumb ? 1u : 2u;
        else seqCost = 1u;
    }

    // Must ensure r15 increment for instruction 0 stays inside the mapped page!
    uint32_t nextR15 = r15Before + (thumb ? 2u : 4u);
    if ((nextR15 & 0xFFFu) < 8u || (nextR15 & 0xFFFu) > 0xF80u) return false;

    uint32_t opcode0 = cpu->pipeline[0];
    if (!thumb && (opcode0 >> 28) != 0xEu) {
        uint8_t c = Interpreter::condition[((opcode0 >> 24) & 0xF0u) | (cpu->cpsr >> 28)];
        if (c != 1u) return false;
    }

    // Lookup existing block (or compile if eligible pure ALU!)
    uint32_t opcode1 = cpu->pipeline[1];
    BlockEntry &bEntry = blockTable[1][bIdx];
    uint32_t curPageEpoch = pageEpoch[1][(instrPC >> 12) & (PAGE_EPOCH_SIZE - 1)];
    uint32_t expectedFlags = (currentEpoch << 2) | (thumb ? 3u : 1u);

    bool hit = (bEntry.pc == instrPC &&
                bEntry.opcode0 == opcode0 &&
                bEntry.opcode1 == opcode1 &&
                bEntry.epochAndFlags == expectedFlags);
    if (hit && bEntry.pageEpoch != curPageEpoch) {
        // Note: cpu->pcData currently points to instrPC + step (pipeline[1]); after step 0 advance it points to instrPC + 2*step!
        uint8_t *pcDataAfter0 = cpu->pcData + (thumb ? 2 : 4);
        if (BLK_N(bEntry.instrCount) <= 2 || computeTailHash(pcDataAfter0, BLK_N(bEntry.instrCount), thumb) == bEntry.tailHash) {
            bEntry.pageEpoch = curPageEpoch;
        } else {
            hit = false;
        }
    }

    if (!hit) {
        if (s_candPc[1][bIdx] != instrPC) {
            s_candPc[1][bIdx] = instrPC;
            s_candHits[1][bIdx] = 1;
            return false;
        }
        uint8_t ch = s_candHits[1][bIdx];
        if (++ch < 8u) {
            s_candHits[1][bIdx] = ch;
            return false;
        }

        // Quick precheck: reject memory instructions at slot 0 or slot 1 before compiling!
        bool mem0 = thumb ? isAnyMemThumb((uint16_t)opcode0) : isMemClassArm(opcode0);
        bool branch0 = thumb ? isBranchHeadThumb(true, (uint16_t)opcode0) : isBranchHeadArm(true, opcode0);
        bool mem1 = !branch0 && (thumb ? isAnyMemThumb((uint16_t)opcode1) : isMemClassArm(opcode1));
        if (mem0 || mem1) {
            s_candHits[1][bIdx] = 0xFFu;
            return false;
        }
        // Temporarily advance pipeline by 1 step (exact same state as runOpcode entry), compile, and if rejected roll back!
        uint8_t *savedPcData = cpu->pcData;
        uint32_t savedP0 = cpu->pipeline[0];
        uint32_t savedP1 = cpu->pipeline[1];
        cpu->registersUsr[15] = nextR15;
        cpu->pcData = savedPcData + (thumb ? 2 : 4);
        cpu->pipeline[0] = savedP1;
        cpu->pipeline[1] = thumb ? U8TO16(cpu->pcData, 0) : U8TO32(cpu->pcData, 0);
        bool ok = compileBlock(cpu, instrPC, opcode0, thumb, bEntry);
        bool worthwhile = ok && (bEntry.instrCount & 0x0100u) &&
                          (BLK_LOOP(bEntry.prefixCycles) != 0u || BLK_N(bEntry.instrCount) >= 4u);
        if (!worthwhile) {
            if (ok) bEntry.epochAndFlags = 0u;
            s_candHits[1][bIdx] = 0xFFu;
            cpu->registersUsr[15] = r15Before;
            cpu->pcData = savedPcData;
            cpu->pipeline[0] = savedP0;
            cpu->pipeline[1] = savedP1;
            return false;
        }
        s_candHits[1][bIdx] = 3u;
    } else {
        if (!(bEntry.instrCount & 0x0100u) || (BLK_LOOP(bEntry.prefixCycles) == 0u && BLK_N(bEntry.instrCount) < 4u))
            return false;
        if (core->gbaBus.active && (bEntry.instrCount & 0x0200u)) return false;
        // Advance pipeline for instruction 0 before entering block!
        cpu->registersUsr[15] = nextR15;
        cpu->pcData += (thumb ? 2 : 4);
        cpu->pipeline[0] = opcode1;
        cpu->pipeline[1] = thumb ? U8TO16(cpu->pcData, 0) : U8TO32(cpu->pcData, 0);
    }

    if (core->gbaBus.active && (bEntry.instrCount & 0x0200u)) {
        // Rollback pre-advanced pipeline if active prefetcher meets a branch block right after compile
        cpu->registersUsr[15] = r15Before;
        cpu->pcData -= (thumb ? 2 : 4);
        cpu->pipeline[1] = cpu->pipeline[0];
        cpu->pipeline[0] = opcode0;
        return false;
    }

    uint32_t maxSingleCyc = ((uint32_t)BLK_PREF(bEntry.prefixCycles) + 3u) * seqCost + deltaTaken;
    if (cpu->cycles + maxSingleCyc >= targetCycles) {
        cpu->registersUsr[15] = r15Before;
        cpu->pcData -= (thumb ? 2 : 4);
        cpu->pipeline[1] = cpu->pipeline[0];
        cpu->pipeline[0] = opcode0;
        return false;
    }

    uint32_t maxIters = 1u;
    uint8_t loopCyc = (uint8_t)BLK_LOOP(bEntry.prefixCycles);
    if (loopCyc != 0u) {
        uint32_t iterGbaCyc = (uint32_t)loopCyc * seqCost + deltaTaken;
        uint32_t rem = targetCycles - cpu->cycles;
        if (rem > 1u && iterGbaCyc > 0u) {
            maxIters = (rem - 1u) / iterGbaCyc + 1u;
            if (maxIters > 4096u) maxIters = 4096u;
        }
    }

    JIT_STAT(stats.dispatches[1]++);
    JIT_STAT(stats.blockHits[1]++);
    JIT_STAT(stats.blockInstrsExecuted[1] += BLK_N(bEntry.instrCount));

    int rawCyc = jit_enter(bEntry.code, cpu, maxIters);
    uint32_t taken = 0u;
    if (loopCyc != 0u) {
        taken = (uint32_t)rawCyc / (uint32_t)loopCyc;
    } else if ((bEntry.instrCount & 0x0200u) && rawCyc == (int)BLK_PREF(bEntry.prefixCycles) + 3) {
        taken = 1u;
    }
    uint32_t totalGbaCyc = (uint32_t)rawCyc * seqCost + taken * deltaTaken;

    core->gbaBus.lastFetchUsedDuty = false;
    if (taken > 0u) {
        core->gbaBus.active = false;
        core->gbaBus.count = 0;
        core->gbaBus.duty = 0;
    } else if (region < 0x08u) {
        core->gbaBus.advancePrefetchOnly((int)totalGbaCyc, waitCnt);
    }
    core->gbaBus.nextSeq = true;
    cpu->cycles = (core->globalCycles += totalGbaCyc);
    core->memory.clearGbaDmaOpenBus();
    return true;
}

int ArmPpcJit::executeArm(Interpreter *cpu, uint32_t opcode, uint32_t remIndep, uint32_t remShared) {
#if NOODS_JIT_POLL_SKIP
    (void)remIndep; (void)remShared;
    return pollExecArm(cpu, opcode);
#else
    return executeArmCore<false>(cpu, opcode, nullptr, remIndep, remShared);
#endif
}

template <bool Track>
int ArmPpcJit::executeArmCore(Interpreter *cpu, uint32_t opcode, bool *outPure, uint32_t remIndep, uint32_t remShared) {
    if (Track && outPure) *outPure = false;
    PROF_GUARD(cpu->arm7, opcode, false);
    if (!initialized) init();
    int cpuIdx = cpu->arm7 ? 1 : 0;
    JIT_STAT(stats.dispatches[cpuIdx]++);
#if NOODS_JIT_HOTPC
    const uint32_t fbHistPC = cpu->registersUsr[15] - 8;
    const int fbHistIdx = cpuIdx;
    fbSiteRecord(fbHistIdx, fbHistPC, opcode, FBK_BLOCK);
#endif

#if NOODS_JIT_BLOCKS
    const bool headBranch = isBranchHeadArm(cpu->arm7, opcode);
    const bool headPushPop = isNativePushPopArm(opcode);
    if ((opcode & 0x08000000u) != 0 && !headBranch && !headPushPop) {
        if (!cpu->arm7 && (opcode & 0x0F000010u) == 0x0E000010u) {
            JIT_STAT(stats.cp15Exits[0]++);
        } else {
            JIT_STAT(stats.fallbackInstrs[cpuIdx]++);
        }
        if (cpuIdx == 1) JIT_STAT({ stats.fbOpc7[(opcode >> 20) & 0xFF]++; stats.fbLast7[(opcode >> 20) & 0xFF] = opcode; });
#if NOODS_JIT_HOTPC
        fbSiteRecord(fbHistIdx, fbHistPC, opcode, FBK_FALLBACK);
#endif
        { if (Track && outPure) *outPure = armPure(opcode, cpu->registersUsr[(opcode >> 16) & 0xFu]); return fallbackArm(cpu, opcode); }
    }
    if ((opcode & 0x0000F000u) == 0x0000F000u && !headBranch && !headPushPop && !isPcDestLoadTerminatorArm(cpu->arm7, opcode)) {
        JIT_STAT(stats.fallbackInstrs[cpuIdx]++);
        JIT_STAT(stats.fbClassArm[cpuIdx][(opcode >> 25) & 7]++);
        if (cpuIdx == 1) JIT_STAT({ stats.fbOpc7[(opcode >> 20) & 0xFF]++; stats.fbLast7[(opcode >> 20) & 0xFF] = opcode; });
#if NOODS_JIT_HOTPC
        fbSiteRecord(fbHistIdx, fbHistPC, opcode, FBK_FALLBACK);
#endif
        { if (Track && outPure) *outPure = armPure(opcode, cpu->registersUsr[(opcode >> 16) & 0xFu]); return fallbackArm(cpu, opcode); }
    }
#endif

    uint32_t instrPC = cpu->registersUsr[15] - 8;

#if NOODS_JIT_BLOCKS
    bool isNative0 = false;
    if (int blockCycles = tryExecuteBlock<Track>(cpu, instrPC, opcode, false, isNative0, outPure, remIndep, remShared)) {
        return blockCycles;
    }
    JIT_STAT(stats.fallbackInstrs[cpuIdx]++);
    JIT_STAT(stats.fbClassArm[cpuIdx][(opcode >> 25) & 7]++);
    if (cpuIdx == 1) JIT_STAT({ stats.fbOpc7[(opcode >> 20) & 0xFF]++; stats.fbLast7[(opcode >> 20) & 0xFF] = opcode; });
#if NOODS_JIT_HOTPC
    fbSiteRecord(fbHistIdx, fbHistPC, opcode, FBK_FALLBACK);
#endif
    { if (Track && outPure) *outPure = armPure(opcode, cpu->registersUsr[(opcode >> 16) & 0xFu]); return fallbackArm(cpu, opcode); }
#else

    size_t idx = ((instrPC >> 2) ^ (instrPC >> 11)) & (STUB_TABLE_SIZE - 1);
    StubEntry &entry = stubTable[cpuIdx][idx];
    uint32_t expectedFlags = (currentEpoch << 2) | 1u;

    if (entry.pc == instrPC && entry.opcode == opcode && entry.epochAndFlags == expectedFlags && entry.code) {
        JIT_STAT(stats.stubHits[cpuIdx]++);
    } else {
        void *code = compileStub(cpu, instrPC, opcode, false);
        expectedFlags = (currentEpoch << 2) | 1u;
        entry.pc = instrPC;
        entry.opcode = opcode;
        entry.epochAndFlags = expectedFlags;
        entry.code = code;
    }

    if (Track && outPure) *outPure = armPure(opcode, cpu->registersUsr[(opcode >> 16) & 0xFu]);
#if NOODS_JIT_HOTPC
    fbSiteRecord(fbHistIdx, fbHistPC, opcode, FBK_STUB);
#endif
    PROF_MARK(1);
    return jit_enter(entry.code, cpu, 1u);
#endif
}

int ArmPpcJit::executeThumb(Interpreter *cpu, uint32_t opcode, uint32_t remIndep, uint32_t remShared) {
#if NOODS_JIT_POLL_SKIP
    pollEpoch++;
#endif
    PROF_GUARD(cpu->arm7, opcode, true);
    if (!initialized) init();
    int cpuIdx = cpu->arm7 ? 1 : 0;
    JIT_STAT(stats.dispatches[cpuIdx]++);
#if NOODS_JIT_HOTPC
    fbSiteRecord(2 + cpuIdx, cpu->registersUsr[15] - 4, opcode & 0xFFFFu, FBK_BLOCK);
#endif

    uint32_t instrPC = cpu->registersUsr[15] - 4;

#if NOODS_JIT_BLOCKS
    bool isNative0 = false;
    if (int blockCycles = tryExecuteBlock(cpu, instrPC, opcode, true, isNative0, nullptr, remIndep, remShared)) {
        return blockCycles;
    }
    JIT_STAT(stats.fallbackInstrs[cpuIdx]++);
    JIT_STAT(stats.fbClassThumb[cpuIdx][(opcode >> 13) & 7]++);
#if NOODS_JIT_HOTPC
    fbSiteRecord(2 + cpuIdx, cpu->registersUsr[15] - 4, opcode & 0xFFFFu, FBK_FALLBACK);
#endif
#if NOODS_JIT_THUMB_HIST
    if (cpu->arm7) s_thumbFallback7[((uint16_t)opcode >> 10) & 63u]++;
#endif
    return fallbackThumb(cpu, opcode);
#else

    size_t idx = ((instrPC >> 1) ^ (instrPC >> 11) ^ 0x1357u) & (STUB_TABLE_SIZE - 1);
    StubEntry &entry = stubTable[cpuIdx][idx];
    uint32_t expectedFlags = (currentEpoch << 2) | 3u;

    if (entry.pc == instrPC && entry.opcode == opcode && entry.epochAndFlags == expectedFlags && entry.code) {
        JIT_STAT(stats.stubHits[cpuIdx]++);
    } else {
        void *code = compileStub(cpu, instrPC, opcode, true);
        expectedFlags = (currentEpoch << 2) | 3u;
        entry.pc = instrPC;
        entry.opcode = opcode;
        entry.epochAndFlags = expectedFlags;
        entry.code = code;
    }

    PROF_MARK(1);
    return jit_enter(entry.code, cpu, 1u);
#endif
}

void ArmPpcJit::invalidateAddr(bool arm7, uint32_t addr) {
    if (!initialized) return;
    if ((addr >> 27) != 0u || !hasCodeAtAddr(addr)) return;
    int cpuIdx = arm7 ? 1 : 0;
    size_t pageIdx = (addr >> 12) & (PAGE_EPOCH_SIZE - 1);
    if (pageHasBlockBits[cpuIdx][pageIdx >> 5] & (1u << (pageIdx & 31))) {
        JIT_STAT(stats.smcInvalidations[cpuIdx]++);
        pageEpoch[cpuIdx][pageIdx]++;
        if (pageEpoch[cpuIdx][pageIdx] == 0) pageEpoch[cpuIdx][pageIdx] = 1;
    }
}

void ArmPpcJit::invalidateSharedAddr(uint32_t addr) {
    if (!initialized) return;
    if ((addr >> 27) != 0u || !hasCodeAtAddr(addr)) return;
    size_t pageIdx = (addr >> 12) & (PAGE_EPOCH_SIZE - 1);
    size_t wordIdx = pageIdx >> 5;
    uint32_t bitMask = 1u << (pageIdx & 31);
    for (int c = 0; c < 2; c++) {
        if (pageHasBlockBits[c][wordIdx] & bitMask) {
            JIT_STAT(stats.smcInvalidations[c]++);
            pageEpoch[c][pageIdx]++;
            if (pageEpoch[c][pageIdx] == 0) pageEpoch[c][pageIdx] = 1;
        }
    }
}

void ArmPpcJit::invalidateRange(bool arm7, uint32_t startAddr, uint32_t endAddr) {
    (void)arm7;
    if (!initialized || endAddr < startAddr) return;
    if ((startAddr >> 27) != 0u) return;
    uint32_t clampedEnd = (endAddr >= 0x08000000u) ? 0x07FFFFFFu : endAddr;
    uint32_t wStart = startAddr >> 11; // 2 KB chunks (1 word of codeLineBits = 32 x 64B lines)
    uint32_t wEnd   = clampedEnd >> 11;
    for (uint32_t w = wStart; w <= wEnd; w++) {
        uint32_t bits = codeLineBits[w & (CODE_LINE_WORDS - 1)];
        if (!bits) continue;
        // At least one 64B code line exists in this 2 KB half-page: bump its 4 KB pageEpoch!
        size_t pageIdx = (w >> 1) & (PAGE_EPOCH_SIZE - 1);
        for (int c = 0; c < 2; c++) {
            if (pageHasBlockBits[c][pageIdx >> 5] & (1u << (pageIdx & 31))) {
                JIT_STAT(stats.smcInvalidations[c]++);
                pageEpoch[c][pageIdx]++;
                if (pageEpoch[c][pageIdx] == 0) pageEpoch[c][pageIdx] = 1;
            }
        }
        w |= 1u; // Skip second half of same 4 KB page since pageEpoch already bumped!
    }
    // Clear negative-cache entries on self-modifying code writes so modified instructions can recompile
    if (startAddr < 0x04000000u)
        std::memset(s_candHits, 0, sizeof(s_candHits));
}

void ArmPpcJit::invalidateMapRange(bool arm7, uint32_t startAddr, uint32_t endAddr) {
    if (!initialized || endAddr <= startAddr) return;
    int cpuIdx = arm7 ? 1 : 0;
    // Check 128 KB chunks via pageHasBlockBits words; if no code lives in [startAddr..endAddr) (e.g. VRAMCNT!), zero cost!
    if (startAddr >= 0x08000000u) {
        // High BIOS / cartridge window: only invalidate if any block exists at all and PC could alias
        return;
    }
    uint32_t clampedEnd = (endAddr > 0x08000000u) ? 0x08000000u : endAddr;
    uint32_t pStart = startAddr >> 12;
    uint32_t pEnd   = (clampedEnd - 1u) >> 12;
    uint32_t wStart = pStart >> 5;
    uint32_t wEnd   = pEnd >> 5;
    bool any = false;
    for (uint32_t w = wStart; w <= wEnd; w++) {
        uint32_t mask = pageHasBlockBits[cpuIdx][w];
        if (!mask) continue;
        for (uint32_t b = 0; b < 32u; b++) {
            if (!(mask & (1u << b))) continue;
            uint32_t p = (w << 5) | b;
            if (p < pStart || p > pEnd) continue;
            pageEpoch[cpuIdx][p]++;
            if (pageEpoch[cpuIdx][p] == 0) pageEpoch[cpuIdx][p] = 1;
            // Invalidate matching entries in blockTable only when code was actually remapped (e.g. TCM/WRAM banking)
            any = true;
        }
    }
    if (any) {
        std::memset(blockTable[cpuIdx], 0, sizeof(blockTable[cpuIdx]));
        JIT_STAT(stats.mapInvalidations[cpuIdx]++);
    }
}

void ArmPpcJit::invalidateCpu(bool arm7) {
    if (!initialized) return;
    int cpuIdx = arm7 ? 1 : 0;
    for (size_t i = 0; i < PAGE_EPOCH_SIZE; i++) {
        pageEpoch[cpuIdx][i]++;
        if (pageEpoch[cpuIdx][i] == 0) pageEpoch[cpuIdx][i] = 1;
    }
    std::memset(blockTable[cpuIdx], 0, sizeof(blockTable[cpuIdx]));
    std::memset(pageHasBlockBits[cpuIdx], 0, sizeof(pageHasBlockBits[cpuIdx]));
    JIT_STAT(stats.mapInvalidations[cpuIdx]++);
}

void ArmPpcJit::invalidateAll() {
    if (!initialized) return;
    invalidateCpu(false);
    invalidateCpu(true);
}


#if NOODS_JIT_POLL_SKIP
uint32_t ArmPpcJit::pollEpoch = 0;
ArmPpcJit::PollTrack ArmPpcJit::pollTrack[2];

void ArmPpcJit::pollSnapshot(PollTrack &t, Interpreter *cpu, uint32_t S) {
    for (int i = 0; i < 16; i++) t.snap[i] = cpu->registersUsr[i];
    t.snap[16] = cpu->cpsr;
    t.snap[17] = cpu->pipeline[0];
    t.snap[18] = cpu->pipeline[1];
    t.snapStart = S;
    t.snapEpoch = pollEpoch;
}

// Fixed-point poll-loop skipping.
//
// A CPU is "looping" when it arrives at the same head PC twice with an identical post-fetch state (registers,
// CPSR, pipeline) and no impure dispatch or event happened in between (pollEpoch unchanged). The iteration
// between two arrivals is then deterministic: every input is register state (identical), or memory (unchanged,
// since memory only changes via impure dispatches or events). So every further iteration repeats exactly, and
// its duration is the measured period.
//
// A joint skip is applied at a closing dispatch (post-dispatch PC = head+4) when the other CPU is also looping
// (or halted) under the same epoch. Both CPUs are then advanced by whole iterations that end at or before the
// next scheduled event. Their registers do not change, and nothing else can change before that event, so cycle
// counts and state match stepping one dispatch at a time. The skip never crosses an event, so partial iterations
// run normally.
int ArmPpcJit::pollExecArm(Interpreter *cpu, uint32_t opcode) {
    Core *core = cpu->core;
    if (core->gbaMode || core->dsiMode) return executeArmCore<false>(cpu, opcode, nullptr);

    const int ci = cpu->arm7 ? 1 : 0;
    PollTrack &t = pollTrack[ci];
    const uint32_t P = cpu->registersUsr[15] - 8u;
    const uint32_t S = core->globalCycles;

    if (t.armed && P == t.head) {
        bool same = (t.snapEpoch == pollEpoch) &&
                    cpu->cpsr == t.snap[16] && cpu->pipeline[0] == t.snap[17] && cpu->pipeline[1] == t.snap[18];
        for (int i = 0; same && i < 16; i++) same = (cpu->registersUsr[i] == t.snap[i]);
        if (same && S > t.snapStart) {
            t.period = S - t.snapStart;
            t.valid = true;
            t.validEpoch = pollEpoch;
        } else {
            t.valid = false;
        }
        pollSnapshot(t, cpu, S);
    } else if (P <= t.prevPC) {
        // A backward or self branch: treat its target as the head of a candidate loop
        t.armed = true;
        t.head = P;
        t.valid = false;
        pollSnapshot(t, cpu, S);
    }
    t.prevPC = P;

    bool pure = false;
    int r = executeArmCore<true>(cpu, opcode, &pure);
    if (!pure) pollEpoch++;

    if (t.valid && t.validEpoch == pollEpoch && r > 0 && cpu->registersUsr[15] == t.head + 4u)
        r = pollJointSkip(cpu, S, r, ci);
    return r;
}

int ArmPpcJit::pollJointSkip(Interpreter *cpu, uint32_t S, int r, int ci) {
    Core *core = cpu->core;
    const int shift = ci ? 1 : 0; // NDS mode only (GBA and DSi excluded by the caller)
    PollTrack &tc = pollTrack[ci];
    Interpreter &o = core->interpreter[ci ^ 1];
    PollTrack &to = pollTrack[ci ^ 1];

    // The other CPU must be halted or looping under the current epoch
    if (!o.halted && !(to.armed && to.valid && to.validEpoch == pollEpoch)) return r;
    if (!tc.period || (!o.halted && !to.period)) return r;

    const uint32_t E = core->events[0].cycles;

    // This CPU: its next iteration starts at tC; skip whole iterations that end at or before E
    const uint32_t tC = S + ((uint32_t)r << shift);
    uint32_t nC = (E > tC) ? (E - tC) / tc.period : 0;
    uint32_t extra = nC * tc.period;
    if (nC && !(shift && (extra & 1u))) {
        tc.snapStart = tC + extra - tc.period;
        r += (int)(extra >> shift);
        JIT_STAT(stats.pollSkips[ci]++);
        JIT_STAT(stats.pollSkipIters[ci] += nC);
    }

    // Other CPU: parked at its own head; advance its clock directly (its state is unchanged)
    if (!o.halted) {
        uint32_t tO = o.cycles;
        uint32_t nO = (E > tO) ? (E - tO) / to.period : 0;
        uint32_t addO = nO * to.period;
        if (nO && !((ci ^ 1) && (addO & 1u))) {
            o.cycles = tO + addO;
            to.snapStart = o.cycles - to.period;
            JIT_STAT(stats.pollSkips[ci ^ 1]++);
            JIT_STAT(stats.pollSkipIters[ci ^ 1] += nO);
        }
    }
    return r;
}
#endif

void ArmPpcJit::dumpStats(FILE *f) {
    if (!f) return;
#if NOODS_JIT_LOAD_HIST
    for (unsigned c = 0; c < 2; ++c) {
        for (unsigned i = 0; i < 256; ++i) {
            if (s_leadingLoadCount[c][i] > 1000)
                fprintf(f, "JIT_LEAD_LOAD cpu=%u bits27_20=%02X count=%u last=%08X\n",
                        c ? 7u : 9u, i, s_leadingLoadCount[c][i], s_leadingLoadLast[c][i]);
        }
    }
#endif
#if NOODS_JIT_THUMB_HIST
    for (unsigned i = 0; i < 64; i++) {
        if (s_thumbFallback7[i] > 1000)
            fprintf(f, "JIT_THUMB_FB bits15_10=%02X count=%u\n", i, s_thumbFallback7[i]);
    }
#endif
#if NOODS_JIT_STATS
    fprintf(f, "JIT_POLL cpu=9 skips=%u iters=%llu cpu=7 skips=%u iters=%llu\n", (unsigned)stats.pollSkips[0], (unsigned long long)stats.pollSkipIters[0], (unsigned)stats.pollSkips[1], (unsigned long long)stats.pollSkipIters[1]);
#endif
#if NOODS_JIT_HOTPC
    {
        static const char *const siteNames[4] = {"ARM9_arm", "ARM7_arm", "ARM9_thumb", "ARM7_thumb"};
        for (int idx = 0; idx < 4; idx++) {
            static FbSite sorted[FBPC_SLOTS];
            std::memcpy(sorted, s_fbSite[idx], sizeof(sorted));
            for (int a = 1; a < FBPC_SLOTS; a++) {
                FbSite v = sorted[a];
                uint32_t vTotal = v.n[0] + v.n[1] + v.n[2];
                if (!vTotal) continue;
                int b = a - 1;
                while (b >= 0) {
                    uint32_t t = sorted[b].n[0] + sorted[b].n[1] + sorted[b].n[2];
                    if (t < vTotal) { sorted[b + 1] = sorted[b]; b--; } else break;
                }
                sorted[b + 1] = v;
            }
            for (int r = 0; r < 20; r++) {
                uint32_t total = sorted[r].n[0] + sorted[r].n[1] + sorted[r].n[2];
                if (!total) break;
                fprintf(f, "JIT_HOTPC cpu=%s pc=%08X opcode=%08X disp=%u block=%u stub=%u fallback=%u\n",
                        siteNames[idx], sorted[r].pc, sorted[r].opcode, total,
                        sorted[r].n[0], sorted[r].n[1], sorted[r].n[2]);
            }
            std::memset(s_fbSite[idx], 0, sizeof(s_fbSite[idx]));
        }
    }
#endif
#if NOODS_JIT_PROFILE
    for (int c = 0; c < 2; c++) {
        for (int b = 0; b < 16; b++) {
            if (!s_profCount[c][b]) continue;
            fprintf(f, "JIT_PROF cpu=%d bucket=%s count=%llu ticks=%llu us=%llu\n",
                    c == 0 ? 9 : 7, s_profNames[b],
                    (unsigned long long)s_profCount[c][b],
                    (unsigned long long)s_profTicks[c][b],
                    (unsigned long long)PPCTicksToUs(s_profTicks[c][b]));
        }
    }
#endif
#if NOODS_JIT_STATS
    fprintf(f,
            "JIT_STATS enabled=1 blocks=%d "
            "disp9=%llu disp7=%llu "
            "stubComp9=%llu stubComp7=%llu "
            "stubHit9=%llu stubHit7=%llu "
            "blkComp9=%llu blkComp7=%llu "
            "blkHit9=%llu blkHit7=%llu "
            "blkInsn9=%llu blkInsn7=%llu "
            "natInsn9=%llu natInsn7=%llu "
            "fbInsn9=%llu fbInsn7=%llu "
            "cp15Exit9=%llu "
            "smcInv9=%llu smcInv7=%llu "
            "mapInv9=%llu mapInv7=%llu "
            "poolUsed=%zu poolLimit=%zu poolWraps=%llu "
            "poolAddr=%p "
            "blkFail7=%llu blkFail9=%llu "
            "fbArm7=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu "
            "fbThumb7=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
            (int)NOODS_JIT_BLOCKS,
            (unsigned long long)stats.dispatches[0],
            (unsigned long long)stats.dispatches[1],
            (unsigned long long)stats.stubCompiles[0],
            (unsigned long long)stats.stubCompiles[1],
            (unsigned long long)stats.stubHits[0],
            (unsigned long long)stats.stubHits[1],
            (unsigned long long)stats.blockCompiles[0],
            (unsigned long long)stats.blockCompiles[1],
            (unsigned long long)stats.blockHits[0],
            (unsigned long long)stats.blockHits[1],
            (unsigned long long)stats.blockInstrsExecuted[0],
            (unsigned long long)stats.blockInstrsExecuted[1],
            (unsigned long long)stats.nativeInstrs[0],
            (unsigned long long)stats.nativeInstrs[1],
            (unsigned long long)stats.fallbackInstrs[0],
            (unsigned long long)stats.fallbackInstrs[1],
            (unsigned long long)stats.cp15Exits[0],
            (unsigned long long)stats.smcInvalidations[0],
            (unsigned long long)stats.smcInvalidations[1],
            (unsigned long long)stats.mapInvalidations[0],
            (unsigned long long)stats.mapInvalidations[1],
            stats.poolWordsUsed,
            stats.poolLimitWords,
            (unsigned long long)stats.poolWraps,
            (void *)codePool,
            (unsigned long long)stats.blockCompileFails[1],
            (unsigned long long)stats.blockCompileFails[0],
            (unsigned long long)stats.fbClassArm[1][0],
            (unsigned long long)stats.fbClassArm[1][1],
            (unsigned long long)stats.fbClassArm[1][2],
            (unsigned long long)stats.fbClassArm[1][3],
            (unsigned long long)stats.fbClassArm[1][4],
            (unsigned long long)stats.fbClassArm[1][5],
            (unsigned long long)stats.fbClassArm[1][6],
            (unsigned long long)stats.fbClassArm[1][7],
            (unsigned long long)stats.fbClassThumb[1][0],
            (unsigned long long)stats.fbClassThumb[1][1],
            (unsigned long long)stats.fbClassThumb[1][2],
            (unsigned long long)stats.fbClassThumb[1][3],
            (unsigned long long)stats.fbClassThumb[1][4],
            (unsigned long long)stats.fbClassThumb[1][5],
            (unsigned long long)stats.fbClassThumb[1][6],
            (unsigned long long)stats.fbClassThumb[1][7]);

    for (unsigned i = 0; i < 256; i++) {
        if (stats.fbOpc7[i] > 1000) fprintf(f, "JIT_FBOPC7 bits27_20=%02X count=%u last=%08X\n", i, stats.fbOpc7[i], stats.fbLast7[i]);
    }

    if (stats.firstStubAddr && stats.firstStubWords > 0) {
        fprintf(f, "JIT_FIRST_STUB addr=%p guest_pc=%08X opcode=%08X words=%zu:",
                (const void *)stats.firstStubAddr, stats.firstStubPc, stats.firstStubOpcode, stats.firstStubWords);
        for (size_t i = 0; i < stats.firstStubWords; i++) {
            fprintf(f, " %08X", stats.firstStubAddr[i]);
        }
        fprintf(f, "\n");
    }
    if (stats.firstBlockAddr && stats.firstBlockWords > 0) {
        fprintf(f, "JIT_FIRST_BLOCK addr=%p guest_pc=%08X words=%zu:",
                (const void *)stats.firstBlockAddr, stats.firstBlockPc, stats.firstBlockWords);
        for (size_t i = 0; i < stats.firstBlockWords; i++) {
            fprintf(f, " %08X", stats.firstBlockAddr[i]);
        }
        fprintf(f, "\n");
    }
#else
    fprintf(f, "JIT_STATS enabled=0 counters=disabled\n");
#endif
}
