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

#include <cstddef>
#include <cstdint>

// Same-width, basic-block-local load forwarding. Never removes a guest-state
// store or changes branch displacements. Unknown instructions and stores are
// barriers; every statically addressed branch target starts with empty facts.
namespace NoodsJit {
inline unsigned forwardPpcGuestLoads(uint32_t *code, size_t words, int usrOffset) {
    if (words > 4096) return 0;
    bool target[4096] = {};
    for (size_t i = 0; i < words; ++i) {
        const uint32_t w = code[i], op = w >> 26;
        if ((op == 18 || op == 16) && (w & 2u)) return 0; // absolute-branch target not analysed
        if (op != 18 && op != 16) continue;
        const int32_t displacement = op == 16 ? (int32_t)(int16_t)(w & 0xFFFCu) :
            ((int32_t)(w << 6) >> 6) & ~3;
        const int64_t at = (int64_t)i * 4 + displacement;
        if (at >= 0 && at < (int64_t)words * 4 && !(at & 3)) target[at / 4] = true;
    }
    int tag[32];
    auto clear = [&]() { for (int &v : tag) v = -1; };
    auto kill = [&](unsigned r) { if (r == 31) clear(); else tag[r] = -1; };
    auto tracked = [](unsigned r) { return (r >= 3 && r <= 12) || (r >= 28 && r <= 30); };
    auto guestCell = [&](int off) {
        const int n = off - usrOffset;
        return n >= 0 && !(n & 3) && (n < 8 * 4 || n == 15 * 4);
    };
    clear();
    unsigned replaced = 0;
    for (size_t i = 0; i < words; ++i) {
        if (target[i]) clear();
        const uint32_t w = code[i];
        const unsigned op = w >> 26, rt = (w >> 21) & 31, ra = (w >> 16) & 31;
        const int off = (int16_t)w;
        if (op == 32) { // lwz
            int source = -1;
            if (ra == 31 && guestCell(off) && tracked(rt)) {
                for (unsigned r = 0; r < 32; ++r)
                    if (tag[r] == off && tracked(r)) { source = (int)r; break; }
            }
            kill(rt);
            if (source >= 0) {
                // mr rt,source = or rt,source,source, without touching CR/XER.
                code[i] = (31u << 26) | ((unsigned)source << 21) | (rt << 16) |
                          ((unsigned)source << 11) | (444u << 1);
                ++replaced;
            }
            if (rt != 31 && ra == 31 && guestCell(off) && tracked(rt)) tag[rt] = off;
            continue;
        }
        if (op == 36) { // stw
            if (ra != 31 || (off & 3)) { clear(); continue; }
            // Aligned stores through the Interpreter base only alias an exact
            // word. Every other/indirect/partial store is a barrier below.
            for (int &v : tag) if (v == off) v = -1;
            if (guestCell(off) && tracked(rt)) tag[rt] = off;
            continue;
        }
        switch (op) {
        case 7: case 8: case 12: case 13: case 14: case 15: // D-form arithmetic -> RT
        case 34: case 40: case 42: // non-updating byte/halfword loads -> RT
            kill(rt); break;
        case 20: case 21: case 23: // rotates -> RA
        case 24: case 25: case 26: case 27: case 28: case 29: // logical immediates -> RA
            kill(ra); break;
        case 10: case 11: // compare immediate, CR only
            break;
        case 31: {
            const unsigned xo = (w >> 1) & 1023;
            switch (xo) {
            // Whitelist only register-only operations. Killing BOTH RT and RA
            // is intentionally conservative across their two destination layouts.
            case 0: case 8: case 10: case 11: case 19: case 24: case 26: case 28:
            case 32: case 40: case 60: case 75: case 104: case 124: case 136:
            case 138: case 144: case 200: case 202: case 232: case 234: case 235:
            case 266: case 284: case 316: case 339: case 412: case 444: case 459:
            case 476: case 491: case 536: case 792: case 824: case 922: case 954:
                kill(rt); kill(ra); break;
            default: clear(); break;
            }
            break;
        }
        default: clear(); break; // branches, calls, update loads, foreign stores, unknowns
        }
    }
    return replaced;
}
} // namespace NoodsJit

#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include <cstring>
#include <cstdlib>
#include <algorithm>
#ifdef GEKKO
#include <gccore.h>
#endif

#if NOODS_MPU_PURE_JIT && NOODS_JIT_POLL_SKIP
#error "MPU_PURE_JIT is not compatible with JIT_POLL_SKIP"
#endif
#if NOODS_MPU_PURE_JIT
uint32_t g_mpuPureJitStats[4]; // marked compiles, fast dispatches, slow dispatches, blocked
#endif
#include "arm_jit.h"


#include "core.h"
#include "wii_video.h"
#if NOODS_ACWW_SAMPLE
// Sampled fast-dispatch heads and fallback handlers, NOT all executed instructions.
struct AcwwSite { uint32_t pc, op, count; uint32_t thumb; };
static AcwwSite s_acSites[4][4096] = {};
static uint32_t s_acDrops[4] = {};
uint32_t g_acSampleRng[4] = { 0x124aef95u, 0x8d632acfu, 0x73fb5ad1u, 0x379ab531u };
void AcwwSampleRecord(unsigned stream, uint32_t pc, uint32_t opcode, bool thumb) {
    unsigned hash = (pc ^ (pc >> 11) ^ (opcode >> 16) ^ (opcode * 2654435761u) ^ (thumb ? 0x7f4a7c15u : 0u)) & 4095u;
    for (unsigned i=0;i<64;i++) {
        AcwwSite &e=s_acSites[stream][(hash+i)&4095u];
        if (!e.count) { e.pc=pc; e.op=opcode; e.thumb=thumb; e.count=1; return; }
        if (e.pc==pc && e.op==opcode && e.thumb==(unsigned)thumb) { ++e.count; return; }
    }
    ++s_acDrops[stream];
}
#endif

#ifndef NOODS_JIT_RANGE_INVALIDATE
#define NOODS_JIT_RANGE_INVALIDATE 1
#endif

#ifndef NOODS_JIT_SHARED_SMC
#define NOODS_JIT_SHARED_SMC 1
#endif

#ifndef NOODS_JIT_LINE_ALIGN
#define NOODS_JIT_LINE_ALIGN 0
#endif

#ifndef NOODS_JIT_ENTRY_ONLY
#define NOODS_JIT_ENTRY_ONLY 0
#endif
static uint32_t s_sequentialPC[2], s_sequentialSuppressed;
static bool s_sequentialThumb[2];
// Optional ARM9 MRS/MSR CPSR emitters are disabled pending a demonstrated
// end-to-end benefit and further validation. See v1p191 errata.
//
//
//
//
//
//
//
#ifndef NOODS_JIT_PSR_COMPILE
#define NOODS_JIT_PSR_COMPILE 0
#endif
#ifndef NOODS_JIT_CODEGEN_KIB
#define NOODS_JIT_CODEGEN_KIB 1
#endif
#ifndef NOODS_JIT_HOT_VISITS
#define NOODS_JIT_HOT_VISITS 16
#endif
static_assert(NOODS_JIT_HOT_VISITS >= 3 && NOODS_JIT_HOT_VISITS < 255, "hot threshold must fit candidate counter");
static uint32_t s_codegenVi, s_codegenDeferred;
static uint64_t s_codegenTotal;
static uint32_t s_forwardBlocks, s_forwardLoads;
static uint32_t s_codegenBytes;
static bool codegenAllowed() {
#if NOODS_JIT_CODEGEN_KIB > 0
    const uint32_t vi = Wii_VideoRetraceCount();
    if (vi != s_codegenVi) { s_codegenVi = vi; s_codegenBytes = 0; }
    if (s_codegenBytes >= NOODS_JIT_CODEGEN_KIB * 1024u) {
        ++s_codegenDeferred;
        return false;
    }
#endif
    return true;
}

#ifndef NOODS_V130_JIT
#define NOODS_V130_JIT 1
#endif

#if NOODS_JIT_STATS
#define JIT_STAT(...) do { __VA_ARGS__; } while (0)
#else
#define JIT_STAT(...) do { } while (0)
#endif

// v1.81: inline translation of predicated (conditional) ARM data-processing
// instructions with a PPC condition-register guard. Set -DNOODS_V181_COND=0 to
// restore the v1.80 behaviour (predicated instructions end a block).
#ifndef NOODS_V181_COND
#define NOODS_V181_COND 1
#endif

// v1.103 block-shape options.
//   NOODS_SINGLE_BLOCK  a lone translatable instruction is compiled as a one-instruction
//                       block instead of permanently rejecting its site (on by default).
//   NOODS_PAGE_TAIL     blocks may start in the last 128 bytes of a 4 KB page (on by default).
//   NOODS_STORE_EXIT    while the ARM9 runs alone (ARM7 halted), blocks continue past
//                       stores instead of ending two instructions after the first one. A
//                       store that takes its slow path still ends the block at the old
//                       place. These blocks live in their own table (blockTable[2]); with
//                       both CPUs running the old block shapes are used, because there the
//                       block length decides how the two CPUs interleave.
#ifndef NOODS_STORE_EXIT
#define NOODS_STORE_EXIT 1
#endif
#ifndef NOODS_SIDE_EXIT
#define NOODS_SIDE_EXIT NOODS_STORE_EXIT
#endif
#ifndef NOODS_SINGLE_BLOCK
#define NOODS_SINGLE_BLOCK 1
#endif
#ifndef NOODS_PAGE_TAIL
#define NOODS_PAGE_TAIL 1
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
#define FBPC_SLOTS 16384
enum FbKind { FBK_BLOCK = 0, FBK_STUB = 1, FBK_FALLBACK = 2, FBK_KINDS = 3 };
struct FbSite { uint32_t pc; uint32_t opcode; uint32_t n[FBK_KINDS]; };
static FbSite s_fbSite[4][FBPC_SLOTS];
static inline void fbSiteRecord(int idx, uint32_t pc, uint32_t opcode, int kind) {
    FbSite *tab = s_fbSite[idx];
    uint32_t key = (pc ^ (pc >> 9) ^ (opcode >> 13)) & (FBPC_SLOTS - 1);
    for (uint32_t probe = 0; probe < 2048; probe++) {
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
// Match candidate history to the compiled table: ARM9 dual, ARM7, ARM9 solo.
// A rejection/hotness record from one ARM9 execution context is not evidence
// about eligibility in the other table (store/loop policy can differ there).
static constexpr unsigned CAND_CONTEXTS = NOODS_JIT_CONTEXT_CANDIDATES ? 3 : 2;
static uint32_t s_candPc[CAND_CONTEXTS][ArmPpcJit::BLOCK_TABLE_SIZE];
static uint8_t  s_candHits[CAND_CONTEXTS][ArmPpcJit::BLOCK_TABLE_SIZE];
static inline unsigned candidateContext(unsigned cpuIdx) {
    return (NOODS_JIT_CONTEXT_CANDIDATES && cpuIdx == 0 && ArmPpcJit::soloBit) ? 2u : cpuIdx;
}

// ---- Native guest load/store classification (ARM & Thumb) ----
static inline bool isRegShiftAluArm(bool arm7, uint32_t opcode);
// v1.113: ARM9 MUL/MLA and UMULL/UMLAL/SMULL/SMLAL without S and without PC operands
// (fixed 2 / 3 cycles on the ARM9; the ARM7 timing is data dependent and stays interpreted).
#ifndef NO_TMUL
#define NO_TMUL 0 // v1.117: on, as single-instruction blocks only (see headTMul)
#endif
#ifndef NO_AMUL
#define NO_AMUL 0
#endif
static inline bool isNativeMulArm(bool arm7, uint32_t op) {
    if (NO_AMUL) return false;
    if (arm7) return false;
    if ((op & 0x0F0000F0u) != 0x00000090u) return false;
    if (op & 0x00100000u) return false;                 // S forms
    if ((op & 0x00C00000u) == 0x00400000u) return false; // undefined space
    if ((op & 0xFu) == 15u || ((op >> 8) & 0xFu) == 15u || ((op >> 16) & 0xFu) == 15u) return false;
    const bool lng = (op & 0x00800000u) != 0, acc = (op & 0x00200000u) != 0;
    if ((lng || acc) && ((op >> 12) & 0xFu) == 15u) return false;
    return true;
}
static inline bool isNativeDspMulArm(bool arm7, uint32_t op) {
    if (NO_AMUL) return false;
    if (arm7) return false;
    if ((op & 0x0E000010u) != 0x00000000u) return false;
    const uint8_t rd = (uint8_t)((op >> 16) & 0xFu);
    const uint8_t rn = (uint8_t)((op >> 12) & 0xFu);
    const uint8_t rs = (uint8_t)((op >> 8) & 0xFu);
    const uint8_t rm = (uint8_t)(op & 0xFu);
    if (rd == 15u || rs == 15u || rm == 15u) return false;
    if ((op & 0x0FF00090u) == 0x01000080u) return rn != 15u; // SMLAxy
    if ((op & 0x0FF00090u) == 0x01600080u) return true;      // SMULxy
    if ((op & 0x0FF000B0u) == 0x012000A0u) return true;      // SMULWy
    if ((op & 0x0FF000B0u) == 0x01200080u) return rn != 15u; // SMLAWy
    if ((op & 0x0FF00090u) == 0x01400080u) return rn != 15u; // SMLALxy
    return false;
}
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

// Native LDR / LDRB / LDRH / LDRSB / LDRSH (pre- or post-indexed, immediate or register offsets)
static inline bool isNativeLoadArm(bool arm7, uint32_t op) {
    (void)arm7;
    if ((op >> 28) == 0xFu) return false;
    if (((op >> 12) & 0xFu) == 15u) return false; // Rd != PC
    if ((op & (1u << 20)) == 0) return false;   // L = 1
    const bool pre = (op & (1u << 24)) != 0;
    const bool wb  = ((op & (1u << 21)) != 0) || !pre;
    const uint8_t rn = (uint8_t)((op >> 16) & 0xFu);
    const uint8_t rd = (uint8_t)((op >> 12) & 0xFu);
    if (rn == 15 && (!pre || wb)) return false;
    if (!pre && (op & (1u << 21)) != 0) return false; // LDRT / LDRBT (unprivileged user-mode loads)
    if (wb && rn == rd) return false;
    if ((op & 0x0C000000u) == 0x04000000u) {    // LDR / LDRB
        if ((op & (1u << 25)) != 0) {
            if (rn == 15) return false;
            if ((op & (1u << 4)) != 0) return false;
            if ((op & 0xFu) == 15u) return false;
            if (((op >> 5) & 3u) != 0u) return false;    // only LSL-scaled register offsets
        }
        return true;
    }
    if ((op & 0x0E000090u) == 0x00000090u && (op & 0x60u) != 0u) { // LDRH / LDRSB / LDRSH
        if ((op & (1u << 22)) == 0) {           // register offset
            if (rn == 15) return false;
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

static inline bool isNativeMrcArm(bool arm7, uint32_t op) {
    if (arm7) return false;
    if ((op >> 28) != 0xEu) return false;
    if ((op & 0x0F100F10u) != 0x0E100F10u) return false;
    uint8_t rd = (uint8_t)((op >> 12) & 0xFu);
    if (rd == 15u) return false;
    return true;
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


// Fallback pool in MEM1 for GBA (whose large ROMs need the shared arena) and
// systems with insufficient arena space. NDS can use a larger shared-arena pool.
// Aligned executable code pool directly in fast Wii MEM1 1T-SRAM (0x80xxxxxx):
// 1. Real Wii hardware: 4x lower I-cache refill latency than DDR2 MEM2 (0x90xxxxxx).
// 2. Dolphin JIT64/JITArm64: FastBlockMap ONLY covers MEM1 (0x80000000..0x81FFFFFF)!
//    Putting JIT code in MEM2 forced every single `bctr` block entry to miss FastBlockMap
//    and trap out of native x86_64/ARM64 JIT into C++ slow lookup!
// Test only: reducing fallback frees MEM1 for the shared NDS arena.
// A tiny GBA/low-memory fallback is deliberately NOT release-safe.
#ifndef NOODS_TEST_FALLBACK_POOL_WORDS
#define NOODS_TEST_FALLBACK_POOL_WORDS 262144
#endif
alignas(32) static uint32_t s_mem1CodePool[NOODS_TEST_FALLBACK_POOL_WORDS];
static size_t s_codeCapacityWords = sizeof(s_mem1CodePool) / sizeof(uint32_t);
#if NOODS_TEST_DYNAMIC_FALLBACK
static uint32_t *s_testDynamicFallback = nullptr;
// TEST ONLY: allocate the old 1 MiB fallback from heap on demand, otherwise
// use the small static emergency pool. Process lifetime; not release-ready.
static uint32_t *testSelectFallback(size_t &capacityWords) {
    if (!s_testDynamicFallback) {
        s_testDynamicFallback = (uint32_t*)Noods_MEM2_Alloc(1024u * 1024u);
#ifdef GEKKO
        SYS_Report("[NOODS] TEST_FALLBACK_ALLOC ptr=%p bytes=%u\n", s_testDynamicFallback, 1024u*1024u);
#endif
    }
    if (s_testDynamicFallback) { capacityWords = 262144u; return s_testDynamicFallback; }
    capacityWords = sizeof(s_mem1CodePool)/sizeof(uint32_t);
    return s_mem1CodePool;
}
#endif
static size_t s_requestedPoolWords = 0;

uint32_t *ArmPpcJit::codePool = nullptr;
size_t ArmPpcJit::poolWordsUsed = 0;
size_t ArmPpcJit::poolLimitWords = ArmPpcJit::DEFAULT_POOL_WORDS;
uint32_t ArmPpcJit::currentEpoch = 1;
uint32_t ArmPpcJit::soloBit = 0;
void ArmPpcJit::setSolo(bool solo) {
    soloBit = (NOODS_STORE_EXIT && solo) ? 1u : 0u;
    arm9Table = blockTable[soloBit ? 2 : 0];
}
bool ArmPpcJit::initialized = false;

ArmPpcJit::StubEntry ArmPpcJit::stubTable[2][ArmPpcJit::STUB_TABLE_SIZE];
ArmPpcJit::BlockEntry ArmPpcJit::blockTable[3][ArmPpcJit::BLOCK_TABLE_SIZE];
ArmPpcJit::BlockEntry *ArmPpcJit::arm9Table = ArmPpcJit::blockTable[0];
uint32_t ArmPpcJit::pageEpoch[2][ArmPpcJit::PAGE_EPOCH_SIZE];
uint32_t ArmPpcJit::pageHasBlockBits[2][ArmPpcJit::PAGE_EPOCH_SIZE / 32];
uint32_t ArmPpcJit::codeLineBits[ArmPpcJit::CODE_LINE_WORDS];
ArmPpcJit::Stats ArmPpcJit::stats = {};

#if NOODS_JIT_STATS
// Diagnostics: interpreter fallbacks per CPU by opcode class (ARM bits 27-20, Thumb bits 15-8).
static uint32_t s_fbHist[2][2][256], s_fbHistLast[2][2][256];
#endif
// Predecoded non-virtual handler tables: same interpreter bodies and timing.
// Only the C++ member-pointer dispatch mechanics are removed. Exotic member
// pointers retain the original checked member call via fallbackArm/Thumb.
#if NOODS_DIRECT_HANDLERS
using ArmDirectHandler = int (*)(Interpreter *, uint32_t);
using ThumbDirectHandler = int (*)(Interpreter *, uint16_t);
static ArmDirectHandler s_armDirect[4096];
static ThumbDirectHandler s_thumbDirect[1024];
#endif
int ArmPpcJit::dispatchArm(Interpreter *cpu, uint32_t opcode) {
#if NOODS_DIRECT_HANDLERS
#if NOODS_JIT_STATS
    s_fbHist[cpu->arm7 ? 1 : 0][0][(opcode >> 20) & 0xFF]++; s_fbHistLast[cpu->arm7 ? 1 : 0][0][(opcode >> 20) & 0xFF] = opcode;
#endif
    return s_armDirect[((opcode >> 16) & 0xFF0u) | ((opcode >> 4) & 15u)](cpu, opcode);
#else
    return fallbackArm(cpu, opcode);
#endif
}
int ArmPpcJit::dispatchThumb(Interpreter *cpu, uint32_t opcode) {
#if NOODS_DIRECT_HANDLERS
#if NOODS_JIT_STATS
    s_fbHist[cpu->arm7 ? 1 : 0][1][(opcode >> 8) & 0xFF]++; s_fbHistLast[cpu->arm7 ? 1 : 0][1][(opcode >> 8) & 0xFF] = opcode;
#endif
    return s_thumbDirect[(opcode >> 6) & 0x3FFu](cpu, (uint16_t)opcode);
#else
    return fallbackThumb(cpu, opcode);
#endif
}

int ArmPpcJit::fallbackArm(Interpreter *cpu, uint32_t opcode) {
#if NOODS_ACWW_SAMPLE
    acwwMaybeSample(cpu->arm7 ? 3u : 2u, cpu->registersUsr[15]-8u, opcode, false);
#endif
#if NOODS_JIT_STATS
    s_fbHist[cpu->arm7 ? 1 : 0][0][(opcode >> 20) & 0xFF]++; s_fbHistLast[cpu->arm7 ? 1 : 0][0][(opcode >> 20) & 0xFF] = opcode;
#endif
    return (cpu->*Interpreter::armInstrs[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0xF)])(opcode);
}

// v1.117: body of a "step block" (see tryExecuteBlock). Entered from fastDispatch, which has
// advanced R15/pcData by one instruction but has not shifted the pipeline; this does the rest
// of what Interpreter::runOpcode + executeThumb do before the handler runs.
int ArmPpcJit::stepThumb(Interpreter *cpu, uint32_t opcode) {
#if NOODS_JIT_POLL_SKIP
    pollEpoch++;
#endif
    cpu->pipeline[0] = cpu->pipeline[1];
    cpu->pipeline[1] = U8TO16(cpu->pcData, 0);
    return dispatchThumb(cpu, opcode);
}

static inline void emitLoadImm32(uint32_t *&p, uint8_t rd, uint32_t val);
// v1.121: the same for ARM code. The dispatcher has already found the condition true.
int ArmPpcJit::stepArm(Interpreter *cpu, uint32_t opcode) {
    cpu->pipeline[0] = cpu->pipeline[1];
    cpu->pipeline[1] = U8TO32(cpu->pcData, 0);
    return dispatchArm(cpu, opcode);
}

// A depleted emission allowance cannot produce a new native block this VI.
// On a block miss, avoid walking the slow JIT lookup/warm-up/rejection path
// again for every interpreted instruction. The dispatcher has already checked
// the ARM condition and same-page fetch. MPU permission is still mandatory.
int ArmPpcJit::tryBudgetStep(Interpreter *cpu, uint32_t opcode, bool thumb, bool lookupMiss) {
#if NOODS_BUDGET_DISPATCH && NOODS_JIT_CODEGEN_KIB > 0
    const bool budgetBlocked = s_codegenBytes >= NOODS_JIT_CODEGEN_KIB * 1024u &&
        s_codegenVi == Wii_VideoRetraceCount();
#if NOODS_REJECT_DISPATCH && !NOODS_JIT_ENTRY_ONLY && !NOODS_JIT_POLL_SKIP
    bool rejected = false;
    if (!budgetBlocked && lookupMiss) {
        const unsigned cpuIdx = cpu->arm7 ? 1u : 0u;
        const uint32_t pc = cpu->registersUsr[15] - (thumb ? 2u : 4u);
        const unsigned bIdx = ((pc >> (thumb ? 1 : 2)) ^ (pc >> 11)) & (BLOCK_TABLE_SIZE - 1);
        const unsigned ci = candidateContext(cpuIdx);
        // The unchanged slow path refuses to emit at this exact rejected site.
        // Both live block homes were checked before lookupMiss was set. A stale
        // block hit must still reach tail-hash validation through the old path.
        rejected = s_candPc[ci][bIdx] == pc && s_candHits[ci][bIdx] == 0xFFu;
    }
    if (!budgetBlocked && !rejected) return 0;
#else
    (void)lookupMiss;
    if (!budgetBlocked) return 0;
#endif
    Core *core = cpu->core;
    if (core->gbaMode || core->dsiMode || !cpu->pcData) return 0;
    if (!cpu->arm7 && core->cp15.mpuRestricted) {
        if ((cpu->cpsr & 0x1Fu) == 0x10u) return 0;
        const uint32_t page = (cpu->registersUsr[15] - (thumb ? 2u : 4u)) >> 12;
        if (page != core->cp15.execOkPage) {
            if (!core->cp15.mpuFastPriv || !(core->cp15.getPagePerm(page) & 0x04)) return 0;
            core->cp15.execOkPage = page;
        }
    }
#if NOODS_REJECT_DISPATCH && !NOODS_JIT_ENTRY_ONLY && !NOODS_JIT_POLL_SKIP
    if (rejected) {
        const unsigned idx = cpu->arm7 ? 1u : 0u;
        const uint32_t pc = cpu->registersUsr[15] - (thumb ? 2u : 4u);
        if (NOODS_NOFIT_CHAIN && pc == noFitNext[idx] && core->events.frontCycles == noFitEv[idx])
            noFitNext[idx] = pc + (thumb ? 2u : 4u);
    }
#endif
    cpu->registersUsr[15] += thumb ? 2u : 4u;
    cpu->pcData += thumb ? 2u : 4u;
    if (thumb) return stepThumb(cpu, opcode);
#if NOODS_JIT_POLL_SKIP
    ++pollEpoch;
#endif
    return stepArm(cpu, opcode);
#else
    (void)cpu; (void)opcode; (void)thumb; (void)lookupMiss;
    return 0;
#endif
}

// v1.121: writes the step block of one instruction (see tryExecuteBlock) into the table.
// False if this instruction cannot have one; the caller then marks it "never".
bool ArmPpcJit::makeStepBlock(Interpreter *cpu, uint32_t instrPC, uint32_t opcode, uint32_t opcode1, bool thumb) {
#if NOODS_STEP_BLOCK
    const uint32_t r15s = cpu->registersUsr[15];
    // v1p191 permits step blocks closer to a page end. R15 is pipelined
    // and is not necessarily instrPC + step at this call site. The inline
    // dispatcher has its own prefetch-boundary checks.
//
//
//
//
//
    const uint32_t step = thumb ? 2u : 4u;
    if (cpu->core->gbaMode) return false;
    if ((instrPC & 0xFFFu) < 8u || (instrPC & 0xFFFu) > (0x1000u - step)) return false;
    if (thumb ? cpu->arm7 : !NOODS_STEP_ARM) return false;
    const int cpuIdx = cpu->arm7 ? 1 : 0;
    const size_t bIdx = ((instrPC >> (thumb ? 1 : 2)) ^ (instrPC >> 11)) & (BLOCK_TABLE_SIZE - 1);
    BlockEntry *const bTbl = cpuIdx ? blockTable[1] : arm9Table;
    BlockEntry *bp = &bTbl[bIdx];
    if (bp->pc != instrPC && (bp->epochAndFlags & 1u) && (bp->epochAndFlags >> 2) == currentEpoch) {
        BlockEntry *alt = &bTbl[bIdx ^ BLOCK_ALT_XOR];
        if (alt->pc == instrPC || !(alt->epochAndFlags & 1u) || (alt->epochAndFlags >> 2) != currentEpoch)
            bp = alt;
    }
    ensureSpace(16);
    alignCodePool();
    uint32_t *start = codePool + poolWordsUsed;
    uint32_t *p = start;
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    emitLoadImm32(p, JitPpc::R4, thumb ? (uint32_t)(uint16_t)opcode : opcode);
    emitLoadImm32(p, JitPpc::R12, thumb ? (uint32_t)(uintptr_t)&ArmPpcJit::stepThumb : (uint32_t)(uintptr_t)&ArmPpcJit::stepArm);
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctrl();
    emitLoadImm32(p, JitPpc::R12, (uint32_t)(uintptr_t)&jit_exit);
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctr();
    const size_t words = (size_t)(p - start);
    poolWordsUsed += words;
    flushCodeRange(start, words * sizeof(uint32_t));
    bp->pc = instrPC;
    bp->opcode0 = opcode;
    bp->opcode1 = opcode1;
    bp->tailHash = 0;
    bp->epochAndFlags = (currentEpoch << 2) | (thumb ? 3u : 1u);
    bp->pageEpoch = pageEpoch[cpuIdx][(instrPC >> 12) & (PAGE_EPOCH_SIZE - 1)];
    bp->instrCount = (uint16_t)(1u | 0x8000u);
    bp->prefixCycles = 0;
    bp->code = start;
    s_candHits[candidateContext(cpuIdx)][bIdx] = NOODS_JIT_HOT_VISITS;
    return true;
#else
    (void)cpu; (void)instrPC; (void)opcode; (void)opcode1; (void)thumb;
    return false;
#endif
}

// v1.121: ARM instructions that executeArmCore hands to the interpreter without a block
// lookup (LDM/STM, coprocessor, SWI, results written to R15) get their step block here, on
// the third visit like any block.
void ArmPpcJit::stepCandArm(Interpreter *cpu, uint32_t opcode) {
#if NOODS_STEP_BLOCK && NOODS_STEP_ARM
    if (cpu->core->gbaMode) return;
    const uint32_t instrPC = cpu->registersUsr[15] - 8u;
    const int cpuIdx = cpu->arm7 ? 1 : 0;
    const size_t bIdx = ((instrPC >> 2) ^ (instrPC >> 11)) & (BLOCK_TABLE_SIZE - 1);
    BlockEntry *const bTbl = cpuIdx ? blockTable[1] : arm9Table;
    const uint32_t opcode1 = cpu->pipeline[0];
    const uint32_t flags = (currentEpoch << 2) | 1u;
    BlockEntry *bp = &bTbl[bIdx];
    if (!(bp->pc == instrPC && bp->opcode0 == opcode && bp->opcode1 == opcode1 && bp->epochAndFlags == flags))
        bp = &bTbl[bIdx ^ BLOCK_ALT_XOR];
    if (bp->pc == instrPC && bp->opcode0 == opcode && bp->opcode1 == opcode1 && bp->epochAndFlags == flags) {
        bp->pageEpoch = pageEpoch[cpuIdx][(instrPC >> 12) & (PAGE_EPOCH_SIZE - 1)]; // one instruction, checked by opcode
        return;
    }
    if (s_candPc[candidateContext(cpuIdx)][bIdx] != instrPC) {
        s_candPc[candidateContext(cpuIdx)][bIdx] = instrPC;
        s_candHits[candidateContext(cpuIdx)][bIdx] = 1;
        return;
    }
    uint8_t ch = s_candHits[candidateContext(cpuIdx)][bIdx];
    if (ch == 0xFFu) return;
    if (++ch < NOODS_JIT_HOT_VISITS) { s_candHits[candidateContext(cpuIdx)][bIdx] = ch; return; }
    if (!codegenAllowed()) return;
    if (!makeStepBlock(cpu, instrPC, opcode, opcode1, false))
        s_candHits[candidateContext(cpuIdx)][bIdx] = 0xFFu;
#else
    (void)cpu; (void)opcode;
#endif
}

// v1.117: slow path of a plain 32-bit store on the ARM9 (address not backed by a page, or on a
// line that holds compiled code). Does what the interpreter handler does - one
// Memory::write<uint32_t> - without decoding the opcode again, and hands a write to a
// geometry command port straight to the command FIFO, which is where it would end up.
int ArmPpcJit::storeWord9(Interpreter *cpu, uint32_t addr, uint32_t value) {
    Core *core = cpu->core;
    uint32_t cmd;
    if (Gpu3D::commandPort(addr, cmd) && !core->memory.writeMap9A[addr >> 12] && !core->memory.mpuDataCheckActive) {
        core->gpu3D.writeCommandPort(cmd, value);
        return 1;
    }
    core->memory.write<uint32_t>(false, addr, value);
    return 1;
}

int ArmPpcJit::fallbackThumb(Interpreter *cpu, uint32_t opcode) {
#if NOODS_ACWW_SAMPLE
    acwwMaybeSample(cpu->arm7 ? 3u : 2u, cpu->registersUsr[15]-4u, opcode & 0xffffu, true);
#endif
#if NOODS_JIT_STATS
    s_fbHist[cpu->arm7 ? 1 : 0][1][(opcode >> 8) & 0xFF]++; s_fbHistLast[cpu->arm7 ? 1 : 0][1][(opcode >> 8) & 0xFF] = opcode;
#endif
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

// v1.103: fast-forward for count-down delay loops ("SUBS Rd, Rd, #k ; BNE/BCS self").
//
// Called once on entry to such a self-loop block. It applies j iterations that are known
// to take the branch (Rd -= j * k) and returns j; the generated loop then runs the remaining
// iterations normally, always at least one, so the final SUBS still computes the flags and
// the real branch test decides the exit. Register, flags, exit and cycle count (the loop
// multiplies its per-iteration cost by the iteration counter) are the same as running every
// iteration.
uint32_t ArmPpcJit::spinFastForward(Interpreter *cpu, uint32_t maxIters, uint32_t rd, uint32_t k, uint32_t cond) {
    if (maxIters < 2u) return 0u;
    uint32_t *reg = cpu->registers[rd];
    const uint32_t v = *reg;
    uint32_t taken; // iterations guaranteed to take the branch
    if (cond == 1u) { // BNE: taken until the result is zero
        // k <= 0xFFFF and maxIters <= 4096, so j * k never wraps: zero is only reached when v
        // is an exact positive multiple of k, on iteration v / k.
        taken = (v != 0u && v % k == 0u) ? (v / k - 1u) : 0xFFFFFFFFu;
    } else { // BCS: taken while the value before the subtraction is >= k (no borrow)
        taken = v / k;
    }
    const uint32_t j = (taken < maxIters - 1u) ? taken : (maxIters - 1u);
    *reg = v - j * k;
    return j;
}

// v1.105: wait loops that poll VCOUNT and/or count a stack variable down.
//
// Code such as
//     for (volatile timeout = N; timeout; timeout--) { now = REG_VCOUNT; if (now < last) break; last = now; }
// compiles to one short Thumb block that branches back to its own head. VCOUNT only changes in
// a scheduled event, so between two events every pass through the loop does the same thing
// except for moving the counter. NOODS_POLL_LOOP recognises such a block when it is compiled
// (analysePollLoop runs it symbolically) and, on entry, applies all the passes that are
// certain to complete before the next event in one step: counter, registers, flags and
// cycles end up exactly as if the passes had run. At least the last pass before the event
// always runs for real. ARM9-alone (solo) blocks only.
#ifndef NOODS_POLL_LOOP
#define NOODS_POLL_LOOP NOODS_STORE_EXIT
#endif
struct ArmPpcJit::PollLoop {
    uint32_t iterCycles;   // cycles of one pass that takes the closing branch
    uint32_t prefix;       // the block's dispatch budget (BlockEntry::prefixCycles & 0xFF)
    int32_t regCntK[8];    // registers that end a pass holding counter + k
    int32_t consK[4];      // counter + k must be non-zero in every pass
    int32_t flagK;         // FL_CNT0: k; FL_VIMM: immediate
    uint16_t cntOff;       // counter cell = [SP + cntOff]
    uint8_t ioBase;        // register holding the I/O address, 0xFF = none
    uint8_t hasCnt, cntStep; // cntStep 1: the cell goes down by one per pass; 0: read only
    uint8_t mustEqV;       // registers that must already hold the current VCOUNT value
    uint8_t regV;          // registers that end a pass holding the VCOUNT value
    uint8_t regCnt;        // registers that end a pass holding counter + regCntK[r]
    uint8_t nCons, nRt;
    uint8_t flagKind;
    struct { uint8_t imm, cond, taken; } rt[4]; // CMP VCOUNT, #imm ; Bcond - outcome it must have
};
enum { FL_BAD = 0, FL_CNT0, FL_VV, FL_VIMM };

static inline uint32_t cmpFlags(uint32_t a, uint32_t b) { // NZCV of CMP a, b
    const uint32_t r = a - b;
    return ((r >> 31) << 3) | ((r == 0u) << 2) | ((a >= b) << 1) | ((((a ^ b) & (a ^ r)) >> 31) << 0);
}
static inline bool condHolds(uint32_t cond, uint32_t f) {
    const bool n = f & 8u, z = f & 4u, c = f & 2u, v = f & 1u;
    switch (cond) {
        case 0x0: return z;        case 0x1: return !z;
        case 0x2: return c;        case 0x3: return !c;
        case 0x4: return n;        case 0x5: return !n;
        case 0x6: return v;        case 0x7: return !v;
        case 0x8: return c && !z;  case 0x9: return !c || z;
        case 0xA: return n == v;   case 0xB: return n != v;
        case 0xC: return !z && n == v;
        default:  return z || n != v; // 0xD
    }
}

bool ArmPpcJit::analysePollLoop(const uint32_t *ops, int count, uint32_t instrPC, PollLoop &d) {
    enum { K_INIT, K_V, K_CNT };
    struct Val { uint8_t kind, r; int32_t k; };
    Val reg[8];
    for (int i = 0; i < 8; i++) reg[i] = Val{ K_INIT, (uint8_t)i, 0 };
    std::memset(&d, 0, sizeof(d));
    d.ioBase = 0xFF;
    bool cellKnown = false, stored = false;
    int32_t cellK = 0;
    uint8_t fl = FL_BAD;
    int32_t flK = 0;
    auto vish = [&](const Val &v) -> bool { // the VCOUNT value, or a register assumed to hold it
        if (v.kind == K_V) return true;
        if (v.kind != K_INIT) return false;
        d.mustEqV |= (uint8_t)(1u << v.r);
        return true;
    };
    if (count < 2) return false;
    for (int m = 0; m < count; m++) {
        const uint16_t op = (uint16_t)ops[m];
        const bool last = (m == count - 1);
        if ((op & 0xF000u) == 0xD000u && (op & 0x0F00u) < 0x0E00u) {
            const uint32_t cond = (op >> 8) & 0xFu;
            bool taken;
            if (fl == FL_CNT0) {
                if (cond > 1u || d.nCons >= 4) return false;
                d.consK[d.nCons++] = flK; // counter + k != 0
                taken = (cond == 1u);
            } else if (fl == FL_VV) {
                taken = condHolds(cond, 6u); // CMP x, x: Z and C set
            } else if (fl == FL_VIMM) {
                if (d.nRt >= 4) return false;
                taken = last;
                d.rt[d.nRt].imm = (uint8_t)flK; d.rt[d.nRt].cond = (uint8_t)cond; d.rt[d.nRt].taken = taken;
                d.nRt++;
            } else {
                return false;
            }
            if (taken != last) return false;
            if (last) {
                const uint32_t brAddr = instrPC + (uint32_t)m * 2u;
                const uint32_t target = (brAddr + 4u + (uint32_t)((int32_t)(int8_t)(op & 0xFFu) << 1)) & ~1u;
                if (target != instrPC) return false;
            }
            continue;
        }
        if (last) return false;
        if ((op & 0xF800u) == 0x9800u) { // LDR Rd, [SP, #imm]
            const uint32_t off = (op & 0xFFu) * 4u;
            if (!cellKnown) { cellKnown = true; d.cntOff = (uint16_t)off; cellK = 0; d.hasCnt = 1; }
            else if (off != d.cntOff) return false;
            reg[(op >> 8) & 7u] = Val{ K_CNT, 0, cellK };
        } else if ((op & 0xF800u) == 0x9000u) { // STR Rd, [SP, #imm]
            const Val v = reg[(op >> 8) & 7u];
            if (!cellKnown || (op & 0xFFu) * 4u != d.cntOff || v.kind != K_CNT) return false;
            cellK = v.k; stored = true;
        } else if ((op & 0xF000u) == 0x3000u) { // ADD / SUB Rd, #imm8
            Val &v = reg[(op >> 8) & 7u];
            if (v.kind != K_CNT) return false;
            v.k += (op & 0x0800u) ? -(int32_t)(op & 0xFFu) : (int32_t)(op & 0xFFu);
            fl = FL_BAD;
        } else if ((op & 0xF800u) == 0x2800u) { // CMP Rd, #imm8
            const Val v = reg[(op >> 8) & 7u];
            const uint32_t imm = op & 0xFFu;
            if (v.kind == K_CNT) { if (imm != 0u) return false; fl = FL_CNT0; flK = v.k; }
            else if (vish(v)) { fl = FL_VIMM; flK = (int32_t)imm; }
            else return false;
        } else if ((op & 0xFFC0u) == 0x0000u) { // MOVS Rd, Rs (LSL #0): N and Z only
            reg[op & 7u] = reg[(op >> 3) & 7u];
            fl = FL_BAD;
        } else if ((op & 0xFFC0u) == 0x4280u) { // CMP Rn, Rm
            const Val a = reg[op & 7u], b = reg[(op >> 3) & 7u];
            if (!vish(a) || !vish(b)) return false;
            fl = FL_VV;
        } else if ((op & 0xFFC0u) == 0x8800u) { // LDRH Rd, [Rb]
            const uint32_t rb = (op >> 3) & 7u;
            if (reg[rb].kind != K_INIT || reg[rb].r != rb) return false;
            if (d.ioBase != 0xFF && d.ioBase != rb) return false;
            d.ioBase = (uint8_t)rb;
            reg[op & 7u] = Val{ K_V, 0, 0 };
        } else {
            return false;
        }
    }
    if (fl == FL_BAD) return false;
    d.flagKind = fl; d.flagK = flK;
    if (d.hasCnt) {
        if (stored) { if (cellK != -1) return false; d.cntStep = 1; }
        else d.cntStep = 0;
    }
    if (d.ioBase == 0xFF && !d.cntStep) return false;       // nothing would ever end the loop
    // Registers at the end of a pass. A copy of another register is the VCOUNT value if the
    // source is required to hold it.
    for (int i = 0; i < 8; i++) {
        Val &v = reg[i];
        if (v.kind == K_INIT && v.r != i) { d.mustEqV |= (uint8_t)(1u << v.r); v.kind = K_V; }
        if (v.kind == K_V) d.regV |= (uint8_t)(1u << i);
        else if (v.kind == K_CNT) { d.regCnt |= (uint8_t)(1u << i); d.regCntK[i] = v.k; }
    }
    if (d.mustEqV && d.ioBase == 0xFF) return false;
    if (d.ioBase != 0xFF && ((d.regV | d.regCnt | d.mustEqV) & (1u << d.ioBase))) return false;
    // What a pass requires at its head must still hold at its end.
    if (d.mustEqV & d.regCnt) return false;
    return true;
}

uint32_t ArmPpcJit::pollFastForward(Interpreter *cpu, const PollLoop *d) {
    Core *const core = cpu->core;
    uint32_t v = 0;
    if (d->ioBase != 0xFF) {
        if (*cpu->registers[d->ioBase] != 0x04000006u) return 0; // VCOUNT only
        v = core->gpu.readVCount();
        for (uint32_t r = 0; r < 8; r++)
            if ((d->mustEqV & (1u << r)) && *cpu->registers[r] != v) return 0;
        for (uint32_t i = 0; i < d->nRt; i++)
            if (condHolds(d->rt[i].cond, cmpFlags(v, d->rt[i].imm)) != (d->rt[i].taken != 0)) return 0;
    }
    // Pass j (0 = the one being entered now) is dispatched as a whole block when
    // globalCycles + j * iterCycles + prefix < frontCycles. Skip passes 0 .. n-1 and run pass n.
    const uint32_t g = core->globalCycles, front = core->events.frontCycles;
    if (front <= g || front - g <= d->prefix + d->iterCycles) return 0;
    uint32_t n = (front - g - d->prefix - 1u) / d->iterCycles;
    uint32_t addr = 0, c0 = 0;
    if (d->hasCnt) {
        addr = *cpu->registers[13] + d->cntOff;
        if ((addr & 3u) || (addr >> 24) != 0x02u || hasCodeAtAddr(addr)) return 0;
        c0 = core->memory.read<uint32_t>(false, addr);
        for (uint32_t i = 0; i < d->nCons; i++) {
            const uint32_t u = c0 + (uint32_t)d->consK[i];
            if (d->cntStep) { if (u < n) n = u; }  // counter + k - j != 0 for j < n
            else if (u == 0u) return 0;
        }
    }
    if (n == 0u) return 0;
    const uint32_t cHead = d->cntStep ? (c0 - (n - 1u)) : c0; // cell at the head of the last skipped pass
    if (d->cntStep) core->memory.write<uint32_t>(false, addr, c0 - n);
    for (uint32_t r = 0; r < 8; r++) {
        if (d->regV & (1u << r)) *cpu->registers[r] = v;
        else if (d->regCnt & (1u << r)) *cpu->registers[r] = cHead + (uint32_t)d->regCntK[r];
    }
    uint32_t f;
    if (d->flagKind == FL_VV) f = 6u;
    else if (d->flagKind == FL_VIMM) f = cmpFlags(v, (uint32_t)d->flagK);
    else f = cmpFlags(cHead + (uint32_t)d->flagK, 0u);
    cpu->cpsr = (cpu->cpsr & 0x0FFFFFFFu) | (f << 28);
    core->globalCycles = g + n * d->iterCycles;
    cpu->cycles = core->globalCycles;
    return n;
}

extern "C" int jit_fallback_arm(Interpreter *cpu, uint32_t opcode) {
    return ArmPpcJit::fallbackArm(cpu, opcode);
}

extern "C" int jit_fallback_thumb(Interpreter *cpu, uint32_t opcode) {
    return ArmPpcJit::fallbackThumb(cpu, opcode);
}

void ArmPpcJit::flushCodeRange(void *start, size_t byteCount) {
    if (!start || byteCount == 0) return;
    s_codegenBytes += (uint32_t)byteCount;
    s_codegenTotal += byteCount;
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

static uint32_t *s_dynStub = nullptr; // v1.119: shared dynamic-link routine of the current pool
static uint32_t *s_linkStub[8];
static uint32_t *s_slowTramp = nullptr; // v1.121
static uint32_t *s_smcCheck = nullptr;
static uint32_t *s_pollHalf[2] = {}; // generation-local MMIO leaf routines
void ArmPpcJit::configureCodePool(void *memory, size_t bytes) {
    if (memory && bytes >= MAX_BLOCK_WORDS * 2 * sizeof(uint32_t) &&
        ((uintptr_t)memory & 31u) == 0) {
        codePool = static_cast<uint32_t *>(memory);
        s_codeCapacityWords = bytes / sizeof(uint32_t);
    } else {
#if NOODS_TEST_DYNAMIC_FALLBACK
        codePool = testSelectFallback(s_codeCapacityWords);
#else
        codePool = s_mem1CodePool;
        s_codeCapacityWords = sizeof(s_mem1CodePool) / sizeof(uint32_t);
#endif
    }
    poolLimitWords = s_requestedPoolWords && s_requestedPoolWords <= s_codeCapacityWords
        ? s_requestedPoolWords : s_codeCapacityWords;
    initialized = false; // reset clears all references to code in the old pool
}
void ArmPpcJit::init() {
#if NOODS_DIRECT_HANDLERS
    for (unsigned i = 0; i < 4096; ++i) {
        uint32_t op = ((i & 0xFF0u) << 16) | ((i & 15u) << 4);
        s_armDirect[i] = reinterpret_cast<ArmDirectHandler>(resolveArmHandler(op));
    }
    for (unsigned i = 0; i < 1024; ++i)
        s_thumbDirect[i] = reinterpret_cast<ThumbDirectHandler>(resolveThumbHandler((uint16_t)(i << 6)));
#endif
    if (!codePool) {
#if NOODS_TEST_DYNAMIC_FALLBACK
        codePool = testSelectFallback(s_codeCapacityWords);
        poolLimitWords = s_codeCapacityWords;
#else
        codePool = s_mem1CodePool;
        poolLimitWords = sizeof(s_mem1CodePool) / sizeof(uint32_t);
#endif
    }
    poolWordsUsed = 0;
    s_codegenVi = Wii_VideoRetraceCount();
    s_codegenBytes = s_codegenDeferred = 0;
    s_codegenTotal = 0;
    s_forwardBlocks = s_forwardLoads = 0;
    s_sequentialPC[0] = s_sequentialPC[1] = 0;
    s_sequentialSuppressed = 0;
    s_dynStub = nullptr; s_slowTramp = nullptr; s_smcCheck = nullptr; std::memset(s_pollHalf, 0, sizeof(s_pollHalf)); std::memset(s_linkStub, 0, sizeof(s_linkStub)); // v1.121: routines live in the pool
    if (poolLimitWords == 0 || poolLimitWords > s_codeCapacityWords)
        poolLimitWords = s_codeCapacityWords;
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
        s_requestedPoolWords = maxWords;
        poolLimitWords = maxWords <= s_codeCapacityWords ? maxWords : s_codeCapacityWords;
        JIT_STAT(stats.poolLimitWords = poolLimitWords);
    }
}

static bool s_linkOn = false;
void ArmPpcJit::wrapPool() {
    poolWordsUsed = 0;
    s_dynStub = nullptr;
    std::memset(s_linkStub, 0, sizeof(s_linkStub));
    s_slowTramp = nullptr;
    s_smcCheck = nullptr;
    std::memset(s_pollHalf, 0, sizeof(s_pollHalf));
    currentEpoch++;
    g_evCnt[6]++;
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

void ArmPpcJit::alignCodePool() {
#if NOODS_JIT_LINE_ALIGN
    // Do not write padding in the previous function's last I-cache line. A
    // flush of a newly emitted function must never invalidate its neighbour.
    poolWordsUsed = (poolWordsUsed + 7u) & ~(size_t)7u;
#endif
}

void ArmPpcJit::ensureSpace(size_t wordsNeeded) {
    // A compilation can emit the trampoline, two MMIO leaves, a link stub and
    // the block. Reserve alignment slack even in the smallest allocation path.
    if (poolWordsUsed + wordsNeeded + (NOODS_JIT_LINE_ALIGN ? 40u : 0u) > poolLimitWords) {
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
            case 0xD: // MUL Rd, Rs (ARM9 only)
                if (cpu->arm7) return false;
                emitLoadGuestReg(p, JitPpc::R4, rd);
                emitLoadGuestReg(p, JitPpc::R5, rs);
                *p++ = JitPpc::mullw(JitPpc::R3, JitPpc::R4, JitPpc::R5);
                emitStoreGuestReg(p, JitPpc::R3, rd);
                if (emitFlags) emitUpdateNZ(p, JitPpc::R3);
                outCycles = 4;
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
// NOODS_STORE_EXIT: while a slow-path helper runs, Core::globalCycles is moved forward to the
// value it had when the pre-v1.104 block holding this instruction was entered (timers, the
// divider's busy flag and newly scheduled events are all measured from it).
static int s_emitTimeShift = 0;
static uint32_t s_emitGlobalCyclesAddr = 0;
// v1.121: the slow path of a native load/store used to be emitted inline at every access
// (22-30 instructions: time shift, R15/pcData save, call, restore). It is now one routine per
// code pool; an access only loads its arguments and does "li R11, shift; lis/ori R0, R15; bl".
//   in : R12 = helper, R3-R5 = its arguments, R0 = R15 of the instruction, R11 = time shift
//   out: R3 = helper result; R30 = 1 (finish this iteration, then return to the scheduler)
// The return address and the shift are kept in s_slowSave (helpers never re-enter compiled
// code, and only the emulation thread runs it); 4(R1) cannot be used, the helper owns it.
static uint32_t s_slowSave[2];
void ArmPpcJit::ensureSlowTramp(Interpreter *cpu) {
    if (s_slowTramp) return;
    const int16_t offR15   = (int16_t)(offsetof(Interpreter, registersUsr) + 15 * 4);
    const int16_t offPcDat = (int16_t)offsetof(Interpreter, pcData);
    const uint32_t g = (uint32_t)(uintptr_t)&cpu->core->globalCycles;
    const int16_t gLo = (int16_t)(g & 0xFFFFu);
    const int16_t gHi = (int16_t)((g - (uint32_t)(int32_t)gLo) >> 16);
    const uint32_t s = (uint32_t)(uintptr_t)&s_slowSave[0];
    const int16_t sLo = (int16_t)(s & 0xFFFFu);
    const int16_t sHi = (int16_t)((s - (uint32_t)(int32_t)sLo) >> 16);
    alignCodePool();
    uint32_t *start = codePool + poolWordsUsed;
    uint32_t *p = start;
    *p++ = JitPpc::mflr(JitPpc::R10);
    *p++ = JitPpc::lis(JitPpc::R9, sHi);
    *p++ = JitPpc::stw(JitPpc::R10, JitPpc::R9, sLo);
    *p++ = JitPpc::stw(JitPpc::R11, JitPpc::R9, (int16_t)(sLo + 4));
    *p++ = JitPpc::lis(JitPpc::R9, gHi);
    *p++ = JitPpc::lwz(JitPpc::R10, JitPpc::R9, gLo);
    *p++ = JitPpc::add(JitPpc::R10, JitPpc::R10, JitPpc::R11);
    *p++ = JitPpc::stw(JitPpc::R10, JitPpc::R9, gLo);
    *p++ = JitPpc::li(JitPpc::R30, 1);
    *p++ = JitPpc::lwz(JitPpc::R10, JitPpc::R31, offR15);
    *p++ = JitPpc::stw(JitPpc::R10, JitPpc::R1, 8);
    *p++ = JitPpc::lwz(JitPpc::R10, JitPpc::R31, offPcDat);
    *p++ = JitPpc::stw(JitPpc::R10, JitPpc::R1, 12);
    *p++ = JitPpc::stw(JitPpc::R0, JitPpc::R31, offR15);
    *p++ = JitPpc::mtctr(JitPpc::R12);
    *p++ = JitPpc::bctrl();
    *p++ = JitPpc::lis(JitPpc::R9, sHi);
    *p++ = JitPpc::lwz(JitPpc::R11, JitPpc::R9, (int16_t)(sLo + 4));
    *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R9, sLo);
    *p++ = JitPpc::mtlr(JitPpc::R0);
    *p++ = JitPpc::lis(JitPpc::R9, gHi);
    *p++ = JitPpc::lwz(JitPpc::R10, JitPpc::R9, gLo);
    *p++ = JitPpc::subf(JitPpc::R10, JitPpc::R11, JitPpc::R10);
    *p++ = JitPpc::stw(JitPpc::R10, JitPpc::R9, gLo);
    *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 8);
    *p++ = JitPpc::stw(JitPpc::R0, JitPpc::R31, offR15);
    *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 12);
    *p++ = JitPpc::stw(JitPpc::R0, JitPpc::R31, offPcDat);
    *p++ = JitPpc::blr();
    size_t words = (size_t)(p - start);
    poolWordsUsed += words;
    flushCodeRange(start, words * sizeof(uint32_t));
    s_slowTramp = start;
#if NOODS_JIT_SHARED_SMC
    alignCodePool();
    start = codePool + poolWordsUsed;
    p = start;
    *p++ = JitPpc::srwi(JitPpc::R5, JitPpc::R3, 27);
    *p++ = JitPpc::cmpwi(0, JitPpc::R5, 0);
    uint32_t *noCode = p++;
    *p++ = JitPpc::rlwinm(JitPpc::R9, JitPpc::R3, 26, 27, 31);
    *p++ = JitPpc::rlwinm(JitPpc::R12, JitPpc::R3, 23, 14, 29);
    emitLoadImm32(p, JitPpc::R6, (uint32_t)(uintptr_t)&codeLineBits[0]);
    *p++ = JitPpc::lwzx(JitPpc::R11, JitPpc::R6, JitPpc::R12);
    *p++ = JitPpc::srw(JitPpc::R9, JitPpc::R11, JitPpc::R9);
    *p++ = JitPpc::andi_rc(JitPpc::R9, JitPpc::R9, 1);
    *p++ = JitPpc::blr();
    *noCode = JitPpc::bne((int16_t)((p - noCode) * 4));
    *p++ = JitPpc::li(JitPpc::R9, 0);
    *p++ = JitPpc::cmpwi(0, JitPpc::R9, 0);
    *p++ = JitPpc::blr();
    words = (size_t)(p - start);
    poolWordsUsed += words;
    flushCodeRange(start, words * sizeof(uint32_t));
    s_smcCheck = start;
#endif

#if NOODS_V130_JIT
    // Both CPU variants must exist before a block reserves its emission pointer.
    // No C++ calls: R3/R28-R31 and guest state are preserved; R8=value, R0=hit.
    for (int c = 0; c < 2; ++c) {
        alignCodePool();
        start = codePool + poolWordsUsed;
        p = start;
        uint32_t *hits[4];
        int count = 0;
        emitPollHalfBody(p, &cpu->core->interpreter[c], 0,
            [](uint32_t *&, uint8_t, uint8_t) {}, hits, count);
        *p++ = JitPpc::li(JitPpc::R0, 0);
        *p++ = JitPpc::blr();
        uint32_t *hit = p;
        *p++ = JitPpc::li(JitPpc::R0, 1);
        *p++ = JitPpc::blr();
        for (int i = 0; i < count; ++i)
            *hits[i] = JitPpc::b((int32_t)((hit - hits[i]) * 4));
        words = (size_t)(p - start);
        poolWordsUsed += words;
        flushCodeRange(start, words * sizeof(uint32_t));
        s_pollHalf[c] = start;
    }
#endif
}

static void emitSlowCall(uint32_t *&p) {
    *p++ = JitPpc::li(JitPpc::R11, (int16_t)s_emitTimeShift);
    emitLoadImm32(p, JitPpc::R0, s_emitInstrR15);
    *p = JitPpc::bl((int32_t)((s_slowTramp - p) * 4));
    p++;
}

// Inline guarded fast-reads for hot read-only / poll MMIO halfword registers (DISPSTAT, VCOUNT, KEYINPUT, IPCSYNC)
// used by BOTH ARM and Thumb native halfword loads with zero C++ helper overhead!
void ArmPpcJit::emitPollHalfBody(uint32_t *&p, Interpreter *cpu, uint8_t rd,
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

// v1.130: one MMIO decoder per CPU/cache generation instead of one per LDRH.
void ArmPpcJit::emitGuardedPollHalfLoad(uint32_t *&p, Interpreter *cpu, uint8_t rd,
        void (*storeFn)(uint32_t *&, uint8_t, uint8_t),
        uint32_t **outJumpSlots, int &jumpCount) {
#if NOODS_V130_JIT
    uint32_t *stub = s_pollHalf[cpu->arm7 ? 1 : 0];
    *p = JitPpc::bl((int32_t)((stub - p) * 4)); ++p;
    *p++ = JitPpc::cmpwi(0, JitPpc::R0, 0);
    uint32_t *miss = p++;
    storeFn(p, JitPpc::R8, rd);
    outJumpSlots[jumpCount++] = p++;
    *miss = JitPpc::beq((int16_t)((p - miss) * 4));
#else
    emitPollHalfBody(p, cpu, rd, storeFn, outJumpSlots, jumpCount);
#endif
}

// v1.119: block linking. Where a block of the "ARM9 alone" table ends at a branch whose target
// is fixed, the exit first tries to enter the target block directly instead of returning to
// the dispatcher. It applies the dispatcher's own tests to the target's table entry (same
// address, same two opcodes, current pool epoch, current page epoch, MPU execute page, not a
// loop block, CPU not halted) and its two time tests (the next event is still ahead, and the
// whole target block ends before it). Only if every test passes does it book the finished
// block's cycles the way the run loop would (globalCycles and the CPU's cycle count),
// advance R15/pcData by one instruction as the dispatcher does before entering a block, and
// jump. If any test fails nothing has been changed and the normal exit follows, so the
// dispatcher decides exactly as before. In: R4 = pcData, R6/R7 = pipeline, all stored already.
#ifndef NOODS_BLOCK_LINK
#define NOODS_BLOCK_LINK 1
#endif
#ifndef NOODS_LINK_BUDGET
#define NOODS_LINK_BUDGET 4 // linked exits per block
#endif
#ifndef NOODS_LINK_DUAL
#define NOODS_LINK_DUAL 1
#endif
#ifndef NOODS_LINK_GBA
#define NOODS_LINK_GBA 1
#endif
#ifndef NOODS_LINK_FALL
#define NOODS_LINK_FALL 1
#endif
// Linking of run-time targets (BX, POP pc, ...) through a shared lookup routine. Measured
// slower than the dispatcher on the Wolveslayer scene (half the lookups miss), so it is off;
// it has only been tested for identical frames on two workloads.
// v1.120: with this version's changes, turning it on changes the Wolveslayer frames. Do not
// enable it without finding out why.
#ifndef NOODS_LINK_DYN
#define NOODS_LINK_DYN 0
#endif
static int s_linkBudget = 0;
static uint32_t s_linkBlockPC = 0;
// v1.120: which run loop is active (set by the loops themselves): 0 none, 1 ARM9 alone,
// 2 both DS CPUs, 3 DS ARM7 alone, 4 GBA. A link is only taken in the loop whose rules it
// was written for. linkEvTarget is the event time runCoreNds captured for this round.
uint32_t ArmPpcJit::linkMode = 0;
uint32_t ArmPpcJit::noFitNext[2] = {1u, 1u}, ArmPpcJit::noFitEv[2] = {0u, 0u};
uint32_t ArmPpcJit::linkEvTarget = 0;

// v1.119: direct block link. Repeats, for one fixed target, every test the dispatcher makes
// before it enters a block, and the run loop's own test for "this CPU goes again"; when all
// hold, books the cycles of the block that just ended and jumps into the target. Any failure
// falls through to the normal exit with nothing changed.
// v1.120: also for the two-CPU loop (either CPU), the DS ARM7 alone and GBA code.
// v1.121: the tests live in one shared routine per kind of block (emitLinkStub); a linked
// exit only loads three values and calls it. Inline, each linked exit took about 45
// instructions and blocks grew by almost half, which a large game pays for in code-pool
// wraps (everything recompiled) long before it gains anything from the links.
//
// Link routine, entered by "bl" from a block exit with
//   R8 = target's block table entry, R10 = target address, R11 = cycles of the finished
//   block in scheduler units, R6/R7 = the two opcodes just put in the pipeline, R4 = pcData.
// Success: jumps into the target block. Failure: returns to the caller with LR restored
// from jit_enter's save slot (36(R1) always holds it) and nothing changed.
//
// Tests the dispatcher makes that are not repeated, and why:
//  - pool generation: the routine is emitted again after every pool wrap, so the generation
//    it was written in is the current one; the target's is compared with that constant.
//  - a CPU halt or a change of run loop always schedules UPDATE_RUN at the current time,
//    which makes the first time test fail.
// s_linkStub[kind * 2 + thumb] (declared above wrapPool, which clears it)
int ArmPpcJit::linkKind(Interpreter *cpu) {
    if (cpu->arm7) return cpu->core->gbaMode ? 3 : 2;
    return ArmPpcJit::soloBit != 0u ? 0 : 1;
}
bool ArmPpcJit::linkKindOn(Interpreter *cpu) {
    if (!NOODS_BLOCK_LINK) return false;
    const int k = linkKind(cpu);
    if (k == 3) return Settings::gbaFastTiming && NOODS_LINK_GBA;
    if (k != 0) return NOODS_LINK_DUAL != 0;
    return true;
}

void ArmPpcJit::emitLinkStub(Interpreter *cpu, bool thumb) {
    if (!linkKindOn(cpu)) return;
    const int kind = linkKind(cpu);
    if (s_linkStub[kind * 2 + (thumb ? 1 : 0)]) return;
    Core *core = cpu->core;
    const bool arm7 = cpu->arm7, gba = kind == 3;
    const int cpuIdx = arm7 ? 1 : 0;
    const uint32_t step = thumb ? 2u : 4u;
    const uint32_t shift = kind == 2 ? 1u : 0u;
    const int16_t offR15   = (int16_t)(offsetof(Interpreter, registersUsr) + 15 * 4);
    const int16_t offPcDat = (int16_t)offsetof(Interpreter, pcData);
    using namespace JitPpc;
    alignCodePool();
    uint32_t *const start = codePool + poolWordsUsed;
    uint32_t *p = start;
    uint32_t *fails[24]; int failKind[24]; int nf = 0;
    auto fail = [&](int k) { failKind[nf] = k; fails[nf++] = p++; }; // 0 bne, 1 beq, 2 bgt, 3 bge
    // address split for "lis r, hi; lwz r, lo(r)"
    auto hi = [](const void *a) { return (int16_t)(((uint32_t)(uintptr_t)a + 0x8000u) >> 16); };
    auto lo = [](const void *a) { return (int16_t)((uint32_t)(uintptr_t)a & 0xFFFFu); };
    // The time tests come first: they are cheap and fail most often.
    // R12 = time after the finished block, R5 = next event time (both kept to the end).
    *p++ = lis(R12, hi(&core->globalCycles));
    *p++ = lwz(R12, R12, lo(&core->globalCycles));
    *p++ = lis(R5, hi(&core->events.frontCycles));
    *p++ = lwz(R5, R5, lo(&core->events.frontCycles));
    *p++ = add(R12, R12, R11);
    *p++ = cmplw(0, R12, R5); fail(3);       // the run loop would stop here
    if (kind == 1 || kind == 2) {
        // Two-CPU loop: this CPU goes again only if the other one is not due first, and the
        // loop must still be working towards the same event it captured. ARM7 blocks are
        // also used by the "ARM7 alone" loop (no second CPU to wait for) and can be left
        // over when a DS game enters GBA mode, so they check which loop is running.
        uint32_t *bAlone = nullptr;
        if (arm7) {
            *p++ = lis(R11, hi(&ArmPpcJit::linkMode));
            *p++ = lwz(R9, R11, lo(&ArmPpcJit::linkMode));
            *p++ = cmpwi(0, R9, 3);
            bAlone = p++;
            *p++ = cmpwi(0, R9, 2); fail(0);
        }
        *p++ = lis(R11, hi(&ArmPpcJit::linkEvTarget));
        *p++ = lwz(R9, R11, lo(&ArmPpcJit::linkEvTarget));
        *p++ = cmpw(0, R9, R5); fail(0);
        *p++ = lis(R9, hi(&core->interpreter[arm7 ? 0 : 1].cycles));
        *p++ = lwz(R9, R9, lo(&core->interpreter[arm7 ? 0 : 1].cycles));
        *p++ = cmplw(0, R12, R9);
        fail(arm7 ? 3 : 2);                  // ARM9 goes first on a tie
        if (bAlone) *bAlone = beq((int16_t)((p - bAlone) * 4));
    }
    *p++ = lwz(R9, R8, 0);
    *p++ = cmpw(0, R9, R10); fail(0);
    *p++ = lwz(R9, R8, 4);
    *p++ = cmpw(0, R9, R6); fail(0);
    *p++ = lwz(R9, R8, 8);
    *p++ = cmpw(0, R9, R7); fail(0);
    if (!thumb) {
        // same table the dispatcher uses: condition[(cond << 4) | flags] must be 1 ("run
        // it"); "always" is 1 for every flag state and skips the lookup
        *p++ = srwi(R9, R6, 28);
        *p++ = cmpwi(0, R9, 14);
        uint32_t *const bAl = p++;
        *p++ = lwz(R11, R31, (int16_t)offsetof(Interpreter, cpsr));
        *p++ = slwi(R9, R9, 4);
        *p++ = rlwimi(R9, R11, 4, 28, 31);
        *p++ = addis(R9, R9, hi(&Interpreter::condition[0]));
        *p++ = lbz(R9, R9, lo(&Interpreter::condition[0]));
        *p++ = cmpwi(0, R9, 1); fail(0);
        *bAl = beq((int16_t)((p - bAl) * 4));
    }
    emitLoadImm32(p, R11, (currentEpoch << 2) | (thumb ? 3u : 1u));
    *p++ = lwz(R9, R8, 16);
    *p++ = cmpw(0, R9, R11); fail(0);
    *p++ = rlwinm(R9, R10, 22, 15, 29);      // ((target >> 12) & 0x7FFF) * 4
    *p++ = addis(R9, R9, hi(&ArmPpcJit::pageEpoch[cpuIdx][0]));
    *p++ = lwz(R11, R9, lo(&ArmPpcJit::pageEpoch[cpuIdx][0]));
    *p++ = lwz(R9, R8, 20);
    *p++ = cmpw(0, R9, R11); fail(0);
    *p++ = lhz(R11, R8, 26);                 // prefixCycles | loopCycles << 8
    *p++ = cmplwi(0, R11, 255); fail(2);     // loop blocks need an iteration budget
    if (!arm7) {
        *p++ = lis(R9, hi(&core->cp15.mpuRestricted));
        *p++ = lbz(R9, R9, lo(&core->cp15.mpuRestricted));
        *p++ = cmpwi(0, R9, 0);
        uint32_t *bNoMpu = p++;
        *p++ = lwz(R9, R31, (int16_t)offsetof(Interpreter, cpsr));
        *p++ = andi_rc(R9, R9, 0x1F);
        *p++ = cmpwi(0, R9, 0x10); fail(1);  // user mode: dispatcher takes its slow path
        *p++ = lis(R9, hi(&core->cp15.execOkPage));
        *p++ = lwz(R9, R9, lo(&core->cp15.execOkPage));
        *p++ = srwi(R0, R10, 12);
        *p++ = cmpw(0, R9, R0); fail(0);
        *bNoMpu = beq((int16_t)((p - bNoMpu) * 4));
    }
    if (shift) *p++ = slwi(R11, R11, shift);
    *p++ = add(R9, R12, R11);
    *p++ = cmplw(0, R9, R5); fail(3);        // target block does not fit before the event
    // Every test passed: commit and enter the target block.
    *p++ = lis(R9, hi(&core->globalCycles));
    *p++ = stw(R12, R9, lo(&core->globalCycles));
    *p++ = stw(R12, R31, (int16_t)offsetof(Interpreter, cycles));
    *p++ = addi(R9, R10, (int16_t)(2u * step));
    *p++ = stw(R9, R31, offR15);
    *p++ = addi(R4, R4, (int16_t)step);
    *p++ = stw(R4, R31, offPcDat);
    *p++ = lwz(R0, R1, 36);
    *p++ = mtlr(R0);
    *p++ = li(R30, 1);
    *p++ = lwz(R12, R8, 28);
    *p++ = mtctr(R12);
    *p++ = bctr();
    for (int i = 0; i < nf; i++) {
        const int16_t d = (int16_t)((p - fails[i]) * 4);
        *fails[i] = failKind[i] == 0 ? bne(d) : failKind[i] == 1 ? beq(d) : failKind[i] == 2 ? bgt(d) : bge(d);
    }
    *p++ = mflr(R9);
    *p++ = mtctr(R9);
    *p++ = lwz(R0, R1, 36);
    *p++ = mtlr(R0);
    *p++ = bctr();
    (void)gba;
    const size_t words = (size_t)(p - start);
    poolWordsUsed += words;
    flushCodeRange(start, words * sizeof(uint32_t));
    s_linkStub[kind * 2 + (thumb ? 1 : 0)] = start;
}

void ArmPpcJit::emitLinkTry(uint32_t *&p, Interpreter *cpu, uint32_t targetPC, bool thumb, int32_t baseCycles, bool helperInBlock) {
    (void)helperInBlock;
    if (!NOODS_BLOCK_LINK || s_linkBudget <= 0 || !linkKindOn(cpu)) return;
    const int kind = linkKind(cpu);
    uint32_t *const stub = s_linkStub[kind * 2 + (thumb ? 1 : 0)];
    if (!stub) return;
    const bool arm7 = cpu->arm7;
    const uint32_t step = thumb ? 2u : 4u;
    const uint32_t r15 = targetPC + step;
    if (((r15 + step) & (thumb ? 0xFFEu : 0xFFCu)) == 0u) return;
    if (baseCycles <= 0) return;
    // Time the finished block costs, in scheduler units, as the run loop would add it.
    int32_t scaled = baseCycles;
    if (kind == 3) {
        // The GBA loop scales a block's cycles by the region and state it started in; the
        // loop only sees the first block of a chain, so a chain must stay within one class.
        const uint32_t regA = (s_linkBlockPC + step) >> 24, regB = (targetPC + step) >> 24;
        const int clsA = regA >= 8u ? 2 : (regA == 2u ? 1 : 0), clsB = regB >= 8u ? 2 : (regB == 2u ? 1 : 0);
        if (clsA != clsB) return;
        if (clsA == 2) scaled += thumb ? scaled : (scaled << 1);
        else if (clsA == 1) scaled *= thumb ? 3 : 6;
    } else if (kind == 2) {
        scaled <<= 1;
    }
    if (scaled > 0x7FFF) return;
    s_linkBudget--;
    const size_t bIdx = ((targetPC >> (thumb ? 1 : 2)) ^ (targetPC >> 11)) & (ArmPpcJit::BLOCK_TABLE_SIZE - 1);
    const ArmPpcJit::BlockEntry *e = &(arm7 ? blockTable[1] : blockTable[soloBit ? 2 : 0])[bIdx];
    using namespace JitPpc;
    emitLoadImm32(p, R8, (uint32_t)(uintptr_t)e);
    emitLoadImm32(p, R10, targetPC);
    *p++ = li(R11, (int16_t)scaled);
    *p = bl((int32_t)((stub - p) * 4)); p++;
}

// v1.119: the dynamic form of block linking, for exits whose target is only known at run time
// (BX, returns, far branches). One routine per code pool; a block of the "ARM9 alone" table
// jumps here instead of returning, with R3 = its cycle count and the guest state complete.
// The routine is the dispatcher's block lookup written out in PowerPC: same table slot, same
// tests, same time rules as fastDispatch (see emitLinkTry); anything it is not sure about
// returns to the dispatcher with nothing changed.
void ArmPpcJit::emitDynStub(Interpreter *cpu) {
    using namespace JitPpc;
    Core *core = cpu->core;
    alignCodePool();
    uint32_t *const start = codePool + poolWordsUsed;
    uint32_t *p = start;
    const int16_t offR15   = (int16_t)(offsetof(Interpreter, registersUsr) + 15 * 4);
    const int16_t offPcDat = (int16_t)offsetof(Interpreter, pcData);
    const int16_t offPipe0 = (int16_t)offsetof(Interpreter, pipeline);
    uint32_t *fails[24]; uint32_t failKind[24]; int nf = 0;
    auto fail = [&](int kind) { failKind[nf] = kind; fails[nf++] = p++; }; // 0 bne, 1 beq, 2 bgt, 3 bge
    *p++ = cmpwi(0, R3, 0); fail(1);
    *p++ = lbz(R9, R31, (int16_t)offsetof(Interpreter, halted));
    *p++ = cmpwi(0, R9, 0); fail(0);
    *p++ = lwz(R4, R31, offPcDat);
    *p++ = cmpwi(0, R4, 0); fail(1);
    *p++ = lwz(R5, R31, offR15);
    *p++ = lwz(R6, R31, offPipe0);
    *p++ = lwz(R7, R31, (int16_t)(offPipe0 + 4));
    *p++ = lwz(R9, R31, (int16_t)offsetof(Interpreter, cpsr));
    *p++ = andi_rc(R10, R9, 0x20);
    uint32_t *const bArm = p++;
    // Thumb
    *p++ = addi(R10, R5, 2);
    *p++ = andi_rc(R10, R10, 0xFFE); fail(1);
    *p++ = addi(R5, R5, -2);
    *p++ = srwi(R8, R5, 1);
    *p++ = li(R11, 3);
    *p++ = li(R12, 2);
    uint32_t *const bCommon = p++;
    // ARM
    *bArm = beq((int16_t)((p - bArm) * 4));
    *p++ = addi(R10, R5, 4);
    *p++ = andi_rc(R10, R10, 0xFFC); fail(1);
    *p++ = rlwinm(R10, R6, 8, 24, 27);                   // condition[(cond << 4) | flags] == 1
    *p++ = rlwimi(R10, R9, 4, 28, 31);
    emitLoadImm32(p, R8, (uint32_t)(uintptr_t)&Interpreter::condition[0]);
    *p++ = add(R10, R10, R8);
    *p++ = lbz(R10, R10, 0);
    *p++ = cmpwi(0, R10, 1); fail(0);
    *p++ = addi(R5, R5, -4);
    *p++ = srwi(R8, R5, 2);
    *p++ = li(R11, 1);
    *p++ = li(R12, 4);
    *bCommon = b((int32_t)((p - bCommon) * 4));
    // R5 = instruction address, R8 = address >> (1 or 2), R11 = flag bits, R12 = step
    *p++ = srwi(R10, R5, 11);
    *p++ = xor_(R8, R8, R10);
    *p++ = rlwinm(R8, R8, 5, 12, 26);                    // (index & 0x7FFF) * 32
    emitLoadImm32(p, R10, (uint32_t)(uintptr_t)&blockTable[2][0]);
    *p++ = add(R8, R8, R10);
    *p++ = lwz(R10, R8, 0);
    *p++ = cmpw(0, R10, R5); fail(0);
    *p++ = lwz(R10, R8, 4);
    *p++ = cmpw(0, R10, R6); fail(0);
    *p++ = lwz(R10, R8, 8);
    *p++ = cmpw(0, R10, R7); fail(0);
    emitLoadImm32(p, R10, (uint32_t)(uintptr_t)&currentEpoch);
    *p++ = lwz(R10, R10, 0);
    *p++ = slwi(R10, R10, 2);
    *p++ = or_(R10, R10, R11);
    *p++ = lwz(R0, R8, 16);
    *p++ = cmpw(0, R0, R10); fail(0);
    *p++ = srwi(R10, R5, 12);
    *p++ = rlwinm(R10, R10, 2, 15, 29);                  // (page & 0x7FFF) * 4
    emitLoadImm32(p, R7, (uint32_t)(uintptr_t)&pageEpoch[0][0]);
    *p++ = lwzx(R10, R7, R10);
    *p++ = lwz(R0, R8, 20);
    *p++ = cmpw(0, R0, R10); fail(0);
    *p++ = lhz(R11, R8, 26);
    *p++ = cmplwi(0, R11, 255); fail(2);
    emitLoadImm32(p, R7, (uint32_t)(uintptr_t)&core->cp15.mpuRestricted);
    *p++ = lbz(R10, R7, 0);
    *p++ = cmpwi(0, R10, 0);
    uint32_t *const bNoMpu = p++;
    *p++ = andi_rc(R10, R9, 0x1F);
    *p++ = cmpwi(0, R10, 0x10); fail(1);
    emitLoadImm32(p, R7, (uint32_t)(uintptr_t)&core->cp15.execOkPage);
    *p++ = lwz(R10, R7, 0);
    *p++ = srwi(R4, R5, 12);
    *p++ = cmpw(0, R10, R4);
    uint32_t *const bPageOk = p++;
    // Another page than the last one checked: ask the same question the dispatcher asks
    // (linkMpuPage), then start over - the page now matches, or we leave.
    *p++ = mr(R28, R3);
    *p++ = mr(R3, R31);
    emitLoadImm32(p, R12, (uint32_t)(uintptr_t)&ArmPpcJit::linkMpuPage);
    *p++ = mtctr(R12);
    *p++ = bctrl();
    *p++ = cmpwi(0, R3, 0);
    *p++ = mr(R3, R28);
    fail(1);
    *p = b((int32_t)((start - p) * 4)); p++;
    *bPageOk = beq((int16_t)((p - bPageOk) * 4));
    *bNoMpu = beq((int16_t)((p - bNoMpu) * 4));
    emitLoadImm32(p, R7, (uint32_t)(uintptr_t)&core->globalCycles);
    *p++ = lwz(R10, R7, 0);
    *p++ = add(R10, R10, R3);
    emitLoadImm32(p, R6, (uint32_t)(uintptr_t)&core->events.frontCycles);
    *p++ = lwz(R6, R6, 0);
    *p++ = cmplw(0, R10, R6); fail(3);
    *p++ = add(R9, R10, R11);
    *p++ = cmplw(0, R9, R6); fail(3);
    // commit
    *p++ = stw(R10, R7, 0);
    *p++ = stw(R10, R31, (int16_t)offsetof(Interpreter, cycles));
    *p++ = lwz(R5, R31, offR15);
    *p++ = add(R5, R5, R12);
    *p++ = stw(R5, R31, offR15);
    *p++ = lwz(R4, R31, offPcDat);
    *p++ = add(R4, R4, R12);
    *p++ = stw(R4, R31, offPcDat);
    *p++ = lwz(R0, R1, 36);
    *p++ = mtlr(R0);
    *p++ = li(R30, 1);
    *p++ = lwz(R12, R8, 28);
    *p++ = mtctr(R12);
    *p++ = bctr();
    for (int i = 0; i < nf; i++) {
        const int16_t d = (int16_t)((p - fails[i]) * 4);
        *fails[i] = failKind[i] == 0 ? bne(d) : failKind[i] == 1 ? beq(d) : failKind[i] == 2 ? bgt(d) : bge(d);
    }
    *p++ = lwz(R0, R1, 36);
    *p++ = mtlr(R0);
    *p++ = lmw(R28, R1, 16);
    *p++ = addi(R1, R1, 32);
    *p++ = blr();
    const size_t words = (size_t)(p - start);
    poolWordsUsed += words;
    flushCodeRange(start, words * sizeof(uint32_t));
    s_dynStub = start;
}

int ArmPpcJit::linkMpuPage(Interpreter *cpu, uint32_t page) {
    Cp15 &c = cpu->core->cp15;
    if (!c.mpuFastPriv || !(c.getPagePerm(page) & 0x04)) return 0;
    c.execOkPage = page;
    return 1;
}

// Ends a block with a jump to the dynamic-link routine (R3 = cycles). False: not available
// for this block, emit the normal return.
bool ArmPpcJit::emitDynLink(uint32_t *&p) {
    if (!NOODS_BLOCK_LINK || !NOODS_LINK_DYN || !s_linkOn || !s_dynStub) return false;
    *p = JitPpc::b((int32_t)((s_dynStub - p) * 4));
    p++;
    return true;
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
        if (!addLoopCount) emitLinkTry(p, cpu, r15Abs - 4u, false, baseCycles, helperInBlock);
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
                                      int32_t totalDelta, int32_t prefixCycles, bool helperInBlock, uint32_t *loopTop,
                                      bool fallThrough) {
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
        if (!addLoopCount) emitLinkTry(p, cpu, r15Abs - 2u, true, baseCycles, helperInBlock);
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
    if (fallThrough) return true; // side exit: the not-taken path continues in the block
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
        if (emitDynLink(p)) return;
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
        if (emitDynLink(p)) return;
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
        if (emitDynLink(p)) return;
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
#if !NOODS_V130_JIT
    if (isHalf && !isSigned) {
        emitGuardedPollHalfLoad(p, cpu, rd, &ArmPpcJit::emitStoreGuestReg, fastJumps, fastJumpCnt);
    }
#endif

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
#if NOODS_V130_JIT
    if (isHalf && !isSigned) {
        emitGuardedPollHalfLoad(p, cpu, rd, &ArmPpcJit::emitStoreGuestReg, fastJumps, fastJumpCnt);
    }
#endif
    uintptr_t helper = resolveThumbHandler(opcode);
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    emitLoadImm32(p, JitPpc::R4, opcode);
    emitLoadImm32(p, JitPpc::R12, (uint32_t)helper);
    emitSlowCall(p); // v1.121: shared trampoline (R12 = helper)

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

#if NOODS_JIT_SHARED_SMC
    // Native leaf preserves R3=address, R8=value and R28-R31; CR0 reports busy.
    *p = JitPpc::bl((int32_t)((s_smcCheck - p) * 4)); ++p;
    uint32_t *bBusy = p++;
#else
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
#endif

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
    if (NOODS_STORE_WORD9 && !cpu->arm7 && !isByte && !isHalf) {
        // v1.117: R3 = address, R8 = value (the sync above only uses R0, R11 and R12)
        *p++ = JitPpc::mr(JitPpc::R4, JitPpc::R3);
        *p++ = JitPpc::mr(JitPpc::R5, JitPpc::R8);
        *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
        emitLoadImm32(p, JitPpc::R12, (uint32_t)(uintptr_t)&ArmPpcJit::storeWord9);
    } else {
        *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
        emitLoadImm32(p, JitPpc::R4, opcode);
        emitLoadImm32(p, JitPpc::R12, (uint32_t)helper);
    }
    emitSlowCall(p); // v1.121: shared trampoline (R12 = helper)

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
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    emitLoadImm32(p, JitPpc::R4, rawOpcode);
    emitLoadImm32(p, JitPpc::R12, (uint32_t)helper);
    emitSlowCall(p); // v1.121: shared trampoline (R12 = helper)

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
    const bool pre    = (opcode & (1u << 24)) != 0;
    const bool up     = (opcode & (1u << 23)) != 0;
    const bool wb     = ((opcode & (1u << 21)) != 0) || !pre;
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
        if (pre) {
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
            if (wb) *p++ = JitPpc::mr(JitPpc::R28, JitPpc::R3);
        } else {
            // Post-indexed: access address is R3 (rn). Writeback address in R28 is rn +/- offset
            if (immOff) {
                uint32_t off = single ? (opcode & 0xFFFu) : (((opcode >> 4) & 0xF0u) | (opcode & 0xFu));
                if (off != 0) {
                    *p++ = JitPpc::addi(JitPpc::R28, JitPpc::R3, (int16_t)(up ? (int32_t)off : -(int32_t)off));
                } else {
                    *p++ = JitPpc::mr(JitPpc::R28, JitPpc::R3);
                }
            } else {
                uint8_t rm = (uint8_t)(opcode & 0xF);
                uint8_t sh = single ? (uint8_t)((opcode >> 7) & 0x1F) : (uint8_t)0;
                emitLoadGuestReg(p, JitPpc::R4, rm);
                if (sh) *p++ = JitPpc::slwi(JitPpc::R4, JitPpc::R4, sh);
                if (up) *p++ = JitPpc::add(JitPpc::R28, JitPpc::R3, JitPpc::R4);
                else    *p++ = JitPpc::subf(JitPpc::R28, JitPpc::R4, JitPpc::R3);
            }
        }
    }

    uint32_t *fastJumps[4];
    int fastJumpCnt = 0;
#if !NOODS_V130_JIT
    if (isHalf && !isSigned) {
        emitGuardedPollHalfLoad(p, cpu, rd, &ArmPpcJit::emitStoreGuestReg, fastJumps, fastJumpCnt);
    }
#endif

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
    if (wb && rn != 15 && rn != rd) {
        emitStoreGuestReg(p, JitPpc::R28, rn);
    }
    uint32_t *bOver = p++;

    uint32_t *slow = p;
#if NOODS_V130_JIT
    if (isHalf && !isSigned) {
        emitGuardedPollHalfLoad(p, cpu, rd, &ArmPpcJit::emitStoreGuestReg, fastJumps, fastJumpCnt);
    }
#endif
    uintptr_t helper = resolveArmHandler(opcode);
    *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
    emitLoadImm32(p, JitPpc::R4, opcode);
    emitLoadImm32(p, JitPpc::R12, (uint32_t)helper);
    emitSlowCall(p); // v1.121: shared trampoline (R12 = helper)

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
#if NOODS_JIT_SHARED_SMC
    // Native leaf preserves R3=address, R8=value and R28-R31; CR0 reports busy.
    *p = JitPpc::bl((int32_t)((s_smcCheck - p) * 4)); ++p;
    uint32_t *bBusy = p++;
#else
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
#endif

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
    if (NOODS_STORE_WORD9 && !cpu->arm7 && single && !isByte && pre && !wb && rd != 15 && rn != 15 && (immOff || rm != 15)) {
        // v1.117: plain STR Rd,[Rn,...] without writeback: R3 = address, R8 = value
        *p++ = JitPpc::mr(JitPpc::R4, JitPpc::R3);
        *p++ = JitPpc::mr(JitPpc::R5, JitPpc::R8);
        *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
        emitLoadImm32(p, JitPpc::R12, (uint32_t)(uintptr_t)&ArmPpcJit::storeWord9);
    } else {
        *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
        emitLoadImm32(p, JitPpc::R4, opcode);
        emitLoadImm32(p, JitPpc::R12, (uint32_t)helper);
    }
    emitSlowCall(p); // v1.121: shared trampoline (R12 = helper)

    uint32_t *join = p;
    *bBusy = JitPpc::bne((int16_t)((slow - bBusy) * 4));
    *bSlow = JitPpc::beq((int16_t)((slow - bSlow) * 4));
    *bOver = JitPpc::b((int32_t)((join - bOver) * 4));

    outCycles = cpu->arm7 ? 2 : 1;
    return true;
}

// v1p190: native status-register forms (MRS Rd,CPSR / MSR CPSR,Rm / MSR CPSR,#imm).
//
// Native code keeps guest registers and flags in Interpreter memory between
// instructions, so these can be compiled as long as the ARM9 is privileged at run
// time (checked at run time - the CPSR mode is guest state), the bits this window may
// modify cannot change the CPU mode (bit 4) or the Thumb state (bit 5), and an MSR
// cannot raise an interrupt: that is the only CPSR side effect that is not a plain
// store - setCpsr() schedules ARM9_INTERRUPT when IME & IE & IRF are set and CPSR.I
// ends up clear. Mask bits are the same in both encodings (bit16 = low byte, bit19 =
// flags byte).
static constexpr uint32_t MSR_BITS_I    = 0x00000080u; // CPSR bit 7
static constexpr uint32_t MSR_BITS_MODE = 0x00000010u; // CPSR bit 4
static constexpr uint32_t MSR_BITS_THUMB= 0x00000020u; // CPSR bit 5

// MSR CPSR,Rm  -> cond 1110 000 10 R 10 1111 0000 0000 Rm
static inline bool isNativeMsrRegArm(bool arm7, uint32_t opcode) {
    if (arm7) return false;
    if ((opcode >> 28) != 0xEu) return false;
    if ((opcode & 0x0FB0F000u) != 0x0120F000u) return false; // R = 0: CPSR, not SPSR
    if ((opcode & 0xFFu) == 15u) return false;               // Rm != PC
    return true;
}

// MSR CPSR,#rotated imm -> cond 001 10 R 10 1111 0000 rotate imm8
static inline bool isNativeMsrImmArm(bool arm7, uint32_t opcode) {
    if (arm7) return false;
    if ((opcode >> 28) != 0xEu) return false;
    if ((opcode & 0x0FB0F000u) != 0x0320F000u) return false;
    if (!(opcode & (1u << 16))) return false;                // needs the low control byte
    return true;
}

// MRS Rd,CPSR -> cond 00010 R 00 1111 Rd 0000 0000 0000
static inline bool isNativeMrsArm(bool arm7, uint32_t opcode) {
    (void)arm7;
    if ((opcode >> 28) != 0xEu) return false;
    if ((opcode & 0x0FBF0FFFu) != 0x010F0000u) return false;  // R = 0: CPSR, not SPSR
    return ((opcode >> 12) & 0xFu) != 15u;                    // Rd == PC is a branch
}

// The CPU whose instruction stream is being inspected. Only the emulation thread compiles
// (see s_slowSave), and the block compiler sets this for the duration of a scan.
static Interpreter *s_psrCpu = nullptr;


// ---------------------------------------------------------------------------
// v1p190: native MRS/MSR(CPSR) emission.
//
// Interpreter::msrRc/msrIc compute, with R (bit 22) == 0 so the CPSR is meant:
//     mask = flags(bit 19) ? 0xF8000000 : 0 | control(bit 16, privileged) ? 0xEF : 0
//     setCpsr((cpsr & ~mask) | (value & mask))
// setCpsr() has exactly two side effects besides the store: it swaps the banked
// registers when CPSR[3:0] changes, and it schedules ARM9_INTERRUPT when
// ime && (ie & irf) && !(cpsr & I). MRS Rd,CPSR is a plain read of cpu->cpsr.
//
// A form is compiled only when it is safe *without any run-time check*, so the block
// never has to leave early or call a helper in the middle of its instruction stream:
//
//   MRS                      always (pure read; the register bank is untouched).
//   MSR with the flags byte  only N,Z,C,V,Q are written, which no bank depends on.
//   MSR with the control     the CPU is privileged, the written value keeps CPSR[5:0]
//   byte, immediate form     (mode and T) exactly, and writing I cannot schedule an
//                            interrupt (I stays set, or nothing is pending and I stays
//                            clear).
//
// Everything else keeps the v1.190 behaviour: psrArmCompilable() says no, the block
// ends in front of the instruction and the dispatcher runs it through the interpreter
// (or its step block). That is why the emitter below cannot fail.
//
// armWrittenFlagsMask() reports all four flags for these forms, which closes the
// block's CR0 condition group: the CPSR is read at run time, so a tag cached before
// this instruction does not describe it.
// (psrWriteMask / psrArmCompilable live in ArmPpcJit: Interpreter's fields are private.)

static constexpr uint32_t PSR_CTRL_MASK = 0x000000EFu;
static constexpr uint32_t PSR_FLAG_MASK = 0xF8000000u;

uint32_t ArmPpcJit::psrWriteMask(Interpreter *cpu, uint32_t opcode) {
    const bool priv = (cpu->cpsr & 0x1Fu) != 0x10u;
    return ((opcode & (1u << 19)) ? PSR_FLAG_MASK : 0u) |
           (((opcode & (1u << 16)) && priv) ? PSR_CTRL_MASK : 0u);
}

bool ArmPpcJit::psrArmCompilable(Interpreter *cpu, uint32_t opcode) {
    // ARM7 keeps the v1.190 rule (see isNativeMsr*Arm): compile whatever cannot schedule an
    // interrupt. Refusing a form here is not free - a refused head instruction gets no block
    // and no step block, so it is interpreted on every visit - and ARM7 code is full of these
    // (the IRQ glue's MSR/MRS pairs). The DS ARM7 has no MPU and no mode-switching from the
    // main loop in the tests that cover it, so the guaranteed-preserved-mode rule below
    // would cost far more than it buys.
    if (cpu->arm7) {
        if (isNativeMrsArm(true, opcode)) return true;
        if (!isNativeMsrRegArm(true, opcode) && !isNativeMsrImmArm(true, opcode)) return false;
        const uint32_t m = psrWriteMask(cpu, opcode);
        if (!(m & 0x80u)) return true;                       // I is not written
        if (cpu->cpsr & 0x80u) return true;                   // I stays set unless the value clears it
        return false;                                         // I writable from clear: interpreter
    }
    if (isNativeMrsArm(false, opcode)) return true;
    if (!isNativeMsrRegArm(false, opcode) && !isNativeMsrImmArm(false, opcode)) return false;
    const uint32_t mask = psrWriteMask(cpu, opcode);
    if (mask == 0) return true;                       // handler writes nothing
    if (!(mask & PSR_CTRL_MASK)) return true;         // flags byte only: no control bits
    // Control byte written: mode and T must be preserved, and the I-bit rule must hold
    // for every possible value of the register.
    if (isNativeMsrImmArm(false, opcode)) {
        const uint32_t imm = opcode & 0xFFu;
        const uint32_t rot = (opcode >> 8) & 0xFu;
        const uint32_t value = rot ? ((imm >> (rot * 2)) | (imm << (32 - rot * 2))) : imm;
        if ((value & 0x3Fu) != (cpu->cpsr & 0x3Fu)) return false;
        const bool iNow = (cpu->cpsr & 0x80u) != 0;
        const bool iNew = (value & 0x80u) != 0;
        if (!iNow && !iNew && cpu->ime && (cpu->ie & cpu->irf)) return false;
        return true;
    }
    return false;                                     // register value is unknown
}

bool ArmPpcJit::emitNativePsrArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, int &outCycles) {
    const int16_t offCpsr = (int16_t)offsetof(Interpreter, cpsr);
    if (isNativeMrsArm(cpu->arm7, opcode)) {
        *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, offCpsr);
        emitStoreGuestReg(p, JitPpc::R4, (uint8_t)((opcode >> 12) & 0xFu));
        outCycles = 2 - cpu->arm7; // mrsRc
        return true;
    }
    const uint32_t mask = psrWriteMask(cpu, opcode);
    if (mask != 0) {
        uint8_t valueReg = JitPpc::R4;
        if (isNativeMsrImmArm(cpu->arm7, opcode)) {
            const uint32_t imm = opcode & 0xFFu;
            const uint32_t rot = (opcode >> 8) & 0xFu;
            const uint32_t value = rot ? ((imm >> (rot * 2)) | (imm << (32 - rot * 2))) : imm;
            emitLoadImm32(p, JitPpc::R4, value & mask);
        } else {
            emitLoadGuestReg(p, JitPpc::R4, (uint8_t)(opcode & 0xFu));
        }
        // cpsr = (cpsr & ~mask) | (value & mask)
        emitLoadImm32(p, JitPpc::R7, mask);
        *p++ = JitPpc::lwz(JitPpc::R6, JitPpc::R31, offCpsr);
        *p++ = JitPpc::andc(JitPpc::R6, JitPpc::R6, JitPpc::R7);
        *p++ = JitPpc::and_(JitPpc::R5, valueReg, JitPpc::R7);
        *p++ = JitPpc::or_(JitPpc::R6, JitPpc::R6, JitPpc::R5);
        *p++ = JitPpc::stw(JitPpc::R6, JitPpc::R31, offCpsr);
    }
    outCycles = 1; // msrRc / msrIc
    return true;
}

bool ArmPpcJit::emitNativeDspMulArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, int &outCycles) {
    if (cpu->arm7) return false;
    const uint8_t rd = (uint8_t)((opcode >> 16) & 0xFu);
    const uint8_t rn = (uint8_t)((opcode >> 12) & 0xFu);
    const uint8_t rs = (uint8_t)((opcode >> 8) & 0xFu);
    const uint8_t rm = (uint8_t)(opcode & 0xFu);
    const bool x = (opcode & (1u << 6)) != 0;
    const bool y = (opcode & (1u << 5)) != 0;
    const int16_t offCpsr = (int16_t)offsetof(Interpreter, cpsr);

    // SMULxy: Rd = Rm[x] * Rs[y]
    if ((opcode & 0x0FF00090u) == 0x01600080u) {
        emitLoadGuestReg(p, JitPpc::R4, rm);
        emitLoadGuestReg(p, JitPpc::R5, rs);
        if (x) *p++ = JitPpc::srawi(JitPpc::R4, JitPpc::R4, 16);
        else   *p++ = JitPpc::extsh(JitPpc::R4, JitPpc::R4);
        if (y) *p++ = JitPpc::srawi(JitPpc::R5, JitPpc::R5, 16);
        else   *p++ = JitPpc::extsh(JitPpc::R5, JitPpc::R5);
        *p++ = JitPpc::mullw(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        outCycles = 1;
        return true;
    }
    // SMLAxy: Rd = (Rm[x] * Rs[y]) + Rn (sets Q on signed 32-bit overflow)
    if ((opcode & 0x0FF00090u) == 0x01000080u) {
        emitLoadGuestReg(p, JitPpc::R4, rm);
        emitLoadGuestReg(p, JitPpc::R5, rs);
        emitLoadGuestReg(p, JitPpc::R6, rn);
        if (x) *p++ = JitPpc::srawi(JitPpc::R4, JitPpc::R4, 16);
        else   *p++ = JitPpc::extsh(JitPpc::R4, JitPpc::R4);
        if (y) *p++ = JitPpc::srawi(JitPpc::R5, JitPpc::R5, 16);
        else   *p++ = JitPpc::extsh(JitPpc::R5, JitPpc::R5);
        *p++ = JitPpc::mullw(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        *p++ = JitPpc::addo(JitPpc::R3, JitPpc::R3, JitPpc::R6);
        *p++ = JitPpc::mfxer(JitPpc::R7);
        *p++ = JitPpc::rlwinm(JitPpc::R7, JitPpc::R7, 29, 4, 4);
        *p++ = JitPpc::lwz(JitPpc::R8, JitPpc::R31, offCpsr);
        *p++ = JitPpc::or_(JitPpc::R8, JitPpc::R8, JitPpc::R7);
        *p++ = JitPpc::stw(JitPpc::R8, JitPpc::R31, offCpsr);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        outCycles = 1;
        return true;
    }
    // SMULWy: Rd = (Rm * Rs[y]) >> 16
    if ((opcode & 0x0FF000B0u) == 0x012000A0u) {
        emitLoadGuestReg(p, JitPpc::R4, rm);
        emitLoadGuestReg(p, JitPpc::R5, rs);
        if (y) *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R5, 0, 0, 15);
        else   *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R5, 16, 0, 15);
        *p++ = JitPpc::mulhw(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        outCycles = 1;
        return true;
    }
    // SMLAWy: Rd = ((Rm * Rs[y]) >> 16) + Rn (sets Q on signed 32-bit overflow)
    if ((opcode & 0x0FF000B0u) == 0x01200080u) {
        emitLoadGuestReg(p, JitPpc::R4, rm);
        emitLoadGuestReg(p, JitPpc::R5, rs);
        emitLoadGuestReg(p, JitPpc::R6, rn);
        if (y) *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R5, 0, 0, 15);
        else   *p++ = JitPpc::rlwinm(JitPpc::R5, JitPpc::R5, 16, 0, 15);
        *p++ = JitPpc::mulhw(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        *p++ = JitPpc::addo(JitPpc::R3, JitPpc::R3, JitPpc::R6);
        *p++ = JitPpc::mfxer(JitPpc::R7);
        *p++ = JitPpc::rlwinm(JitPpc::R7, JitPpc::R7, 29, 4, 4);
        *p++ = JitPpc::lwz(JitPpc::R8, JitPpc::R31, offCpsr);
        *p++ = JitPpc::or_(JitPpc::R8, JitPpc::R8, JitPpc::R7);
        *p++ = JitPpc::stw(JitPpc::R8, JitPpc::R31, offCpsr);
        emitStoreGuestReg(p, JitPpc::R3, rd);
        outCycles = 1;
        return true;
    }
    // SMLALxy: [RdHi, RdLo] = (Rm[x] * Rs[y]) + [RdHi, RdLo]
    if ((opcode & 0x0FF00090u) == 0x01400080u) {
        emitLoadGuestReg(p, JitPpc::R4, rm);
        emitLoadGuestReg(p, JitPpc::R5, rs);
        emitLoadGuestReg(p, JitPpc::R6, rn);  // RdLo
        emitLoadGuestReg(p, JitPpc::R10, rd); // RdHi
        if (x) *p++ = JitPpc::srawi(JitPpc::R4, JitPpc::R4, 16);
        else   *p++ = JitPpc::extsh(JitPpc::R4, JitPpc::R4);
        if (y) *p++ = JitPpc::srawi(JitPpc::R5, JitPpc::R5, 16);
        else   *p++ = JitPpc::extsh(JitPpc::R5, JitPpc::R5);
        *p++ = JitPpc::mullw(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        *p++ = JitPpc::srawi(JitPpc::R7, JitPpc::R3, 31);
        *p++ = JitPpc::addc(JitPpc::R3, JitPpc::R3, JitPpc::R6);
        *p++ = JitPpc::adde(JitPpc::R7, JitPpc::R7, JitPpc::R10);
        emitStoreGuestReg(p, JitPpc::R3, rn);
        emitStoreGuestReg(p, JitPpc::R7, rd);
        outCycles = 2;
        return true;
    }
    return false;
}

bool ArmPpcJit::emitNativeMrcArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, int &outCycles) {
    if (cpu->arm7) return false;
    const uint8_t rd = (uint8_t)((opcode >> 12) & 0xFu);
    const uint8_t cn = (uint8_t)((opcode >> 16) & 0xFu);
    const uint8_t cm = (uint8_t)(opcode & 0xFu);
    const uint8_t cp = (uint8_t)((opcode >> 5) & 0x7u);

    // Fast-path immediate constants:
    if (cn == 0 && cm == 0) {
        if (cp == 0 || (cp >= 3 && cp <= 7)) {
            emitLoadImm32(p, JitPpc::R3, 0x41059461u);
            emitStoreGuestReg(p, JitPpc::R3, rd);
            outCycles = 1;
            return true;
        } else if (cp == 1) {
            emitLoadImm32(p, JitPpc::R3, 0x0F0D2112u);
            emitStoreGuestReg(p, JitPpc::R3, rd);
            outCycles = 1;
            return true;
        } else if (cp == 2) {
            emitLoadImm32(p, JitPpc::R3, 0x00140180u);
            emitStoreGuestReg(p, JitPpc::R3, rd);
            outCycles = 1;
            return true;
        }
    }

    const int16_t offCore = (int16_t)offsetof(Interpreter, core);
    *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, offCore); // R4 = cpu->core

    // CP15 C5 readback packs eight region permissions into 2-bit or 4-bit
    // fields. Emit the packing natively, matching Cp15::read exactly.
    if (cn == 5 && cm == 0 && cp <= 3) {
        const unsigned bits = cp >= 2 ? 4u : 2u;
        emitLoadImm32(p, JitPpc::R5, (uint32_t)(offsetof(Core, cp15) + offsetof(Cp15, regionPerms)));
        *p++ = JitPpc::add(JitPpc::R5, JitPpc::R4, JitPpc::R5);
        *p++ = JitPpc::li(JitPpc::R3, 0);
        for (unsigned region = 0; region < 8; ++region) {
            *p++ = JitPpc::lbz(JitPpc::R6, JitPpc::R5, (int16_t)region);
            if ((cp & 1u) == 0) *p++ = JitPpc::srwi(JitPpc::R6, JitPpc::R6, 4);
            *p++ = JitPpc::andi_rc(JitPpc::R6, JitPpc::R6, bits == 4 ? 15u : 3u);
            if (region) *p++ = JitPpc::slwi(JitPpc::R6, JitPpc::R6, region * bits);
            *p++ = JitPpc::or_(JitPpc::R3, JitPpc::R3, JitPpc::R6);
        }
        emitStoreGuestReg(p, JitPpc::R3, rd);
        outCycles = 1;
        return true;
    }

    uint32_t memberOff = 0;
    bool isDirectMember = true;
    switch ((cn << 16) | (cm << 8) | cp) {
        case 0x010000: memberOff = (uint32_t)offsetof(Cp15, ctrlReg); break;
        case 0x020000: memberOff = (uint32_t)offsetof(Cp15, dataCacheConfig); break;
        case 0x020001: memberOff = (uint32_t)offsetof(Cp15, instructionCacheConfig); break;
        case 0x030000: memberOff = (uint32_t)offsetof(Cp15, writeBufferControl); break;
        case 0x090100: memberOff = (uint32_t)offsetof(Cp15, dtcmReg); break;
        case 0x090101: memberOff = (uint32_t)offsetof(Cp15, itcmReg); break;
        case 0x0D0001:
        case 0x0D0101: memberOff = (uint32_t)offsetof(Cp15, procId); break;
        default:
            if (cn == 6 && cm < 8 && cp <= 1) {
                memberOff = (uint32_t)offsetof(Cp15, regionControl) + (uint32_t)(cm * 4);
            } else {
                isDirectMember = false;
            }
            break;
    }

    if (isDirectMember) {
        uint32_t cp15Off = (uint32_t)offsetof(Core, cp15) + memberOff;
        emitLoadImm32(p, JitPpc::R5, cp15Off);
        *p++ = JitPpc::lwzx(JitPpc::R3, JitPpc::R4, JitPpc::R5); // R3 = *(cpu->core + cp15Off)
        emitStoreGuestReg(p, JitPpc::R3, rd);
        outCycles = 1;
        return true;
    }

    // Unknown registers read as zero in Cp15::read; emit that value directly.
    *p++ = JitPpc::li(JitPpc::R3, 0);
    emitStoreGuestReg(p, JitPpc::R3, rd);
    outCycles = 1;
    return true;
}

bool ArmPpcJit::emitNativeArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, int &outCycles, bool emitFlags) {
    if (isNativeLoadArm(cpu->arm7, opcode)) return emitNativeLoadArm(p, cpu, opcode, outCycles);
    if (isNativeStoreArm(opcode)) return emitNativeStoreArm(p, cpu, opcode, outCycles);
    if (isNativePushPopArm(opcode)) {
        return emitNativePushPop(p, cpu, (uint16_t)(opcode & 0xFFFFu), (opcode & (1u << 20)) != 0u, opcode, false, outCycles);
    }
    if (isNativeMrcArm(cpu->arm7, opcode)) {
        return emitNativeMrcArm(p, cpu, opcode, outCycles);
    }
    // ARM9 CP15 instructions always exit to interpreter
    if (!cpu->arm7 && (opcode & 0x0F000010u) == 0x0E000010u) {
        return false;
    }

    // v1p190: status-register forms
#if NOODS_JIT_PSR_COMPILE
    if (isNativeMrsArm(cpu->arm7, opcode) || isNativeMsrRegArm(cpu->arm7, opcode) ||
        isNativeMsrImmArm(cpu->arm7, opcode)) {
        return emitNativePsrArm(p, cpu, opcode, outCycles);
    }
#endif
    // NOTE: s_emitPrefixCycles / hadHelperCall are not plumbed into emitNativeArm (the
    // fallback helper needs no prefix-cycle sync: it runs the instruction through its
    // interpreter handler with the pipeline intact and Core::globalCycles untouched).

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

    if (isNativeMulArm(cpu->arm7, opcode)) {
        const uint8_t rdHi = (uint8_t)((opcode >> 16) & 0xF), rdLo = (uint8_t)((opcode >> 12) & 0xF);
        const uint8_t rs = (uint8_t)((opcode >> 8) & 0xF), rm = (uint8_t)(opcode & 0xF);
        const bool lng = (opcode & 0x00800000u) != 0, acc = (opcode & 0x00200000u) != 0;
        const bool sgn = (opcode & 0x00400000u) != 0;
        emitLoadGuestReg(p, JitPpc::R4, rm);
        emitLoadGuestReg(p, JitPpc::R5, rs);
        *p++ = JitPpc::mullw(JitPpc::R3, JitPpc::R4, JitPpc::R5);
        if (!lng) {
            if (acc) {
                emitLoadGuestReg(p, JitPpc::R6, rdLo);
                *p++ = JitPpc::add(JitPpc::R3, JitPpc::R3, JitPpc::R6);
            }
            emitStoreGuestReg(p, JitPpc::R3, rdHi);
            outCycles = 2;
            return true;
        }
        *p++ = sgn ? JitPpc::mulhw(JitPpc::R7, JitPpc::R4, JitPpc::R5)
                   : JitPpc::mulhwu(JitPpc::R7, JitPpc::R4, JitPpc::R5);
        if (acc) {
            emitLoadGuestReg(p, JitPpc::R6, rdLo);
            emitLoadGuestReg(p, JitPpc::R10, rdHi);
            *p++ = JitPpc::addc(JitPpc::R3, JitPpc::R3, JitPpc::R6);
            *p++ = JitPpc::adde(JitPpc::R7, JitPpc::R7, JitPpc::R10);
        }
        emitStoreGuestReg(p, JitPpc::R3, rdLo);
        emitStoreGuestReg(p, JitPpc::R7, rdHi);
        outCycles = 3;
        return true;
    }

    if (isNativeDspMulArm(cpu->arm7, opcode)) {
        return emitNativeDspMulArm(p, cpu, opcode, outCycles);
    }

    // Data processing: bits 27..26 == 00
    if ((opcode & 0x0C000000u) != 0x00000000u) return false;

    bool isImm = (opcode & (1u << 25)) != 0;
    const bool regShift = !isImm && isRegShiftAluArm(cpu->arm7, opcode);
    if (!isImm) {
        // Reject multiplies / misc / halfword transfers (bit 4 == 1)
        if ((opcode & (1u << 4)) != 0 && !regShift) return false;
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
    if (!isImm && !regShift) {
        uint8_t shiftType = (uint8_t)((opcode >> 5) & 0x3);
        uint8_t shiftImm = (uint8_t)((opcode >> 7) & 0x1F);
        // RRX (shiftType == 3 && shiftImm == 0) uses CPSR.C; leave to fallback
        if (shiftType == 3 && shiftImm == 0) return false;
    }

    bool setFlags = rawSetFlags && emitFlags;
    // Test/compare with dead flags is a 1-cycle no-op
    if (aluOp >= 0x8 && aluOp <= 0xB && !setFlags) {
        outCycles = regShift ? 2 : 1;
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
    } else if (regShift) {
        // Amount = low byte of Rs. LSL/LSR give 0 from 32 up, ASR saturates, ROR wraps.
        uint8_t rm = (uint8_t)(opcode & 0xF);
        uint8_t rs = (uint8_t)((opcode >> 8) & 0xF);
        uint8_t shiftType = (uint8_t)((opcode >> 5) & 0x3);
        emitLoadGuestReg(p, JitPpc::R6, rs);
        emitLoadGuestReg(p, JitPpc::R5, rm);
        if (shiftType == 3) {
            *p++ = JitPpc::neg(JitPpc::R6, JitPpc::R6);          // rotate left by -n (mod 32)
            *p++ = JitPpc::rlwnm(JitPpc::R5, JitPpc::R5, JitPpc::R6, 0, 31);
        } else {
            *p++ = JitPpc::rlwinm(JitPpc::R6, JitPpc::R6, 0, 24, 31);
            *p++ = JitPpc::cmplwi(1, JitPpc::R6, 32);
            uint32_t *b = p++;
            *p++ = JitPpc::li(JitPpc::R6, 32);                   // slw/srw/sraw use 6 bits: 32 = "all out"
            *b = JitPpc::blt((int16_t)((p - b) * 4), 1);
            if (shiftType == 0)      *p++ = JitPpc::slw(JitPpc::R5, JitPpc::R5, JitPpc::R6);
            else if (shiftType == 1) *p++ = JitPpc::srw(JitPpc::R5, JitPpc::R5, JitPpc::R6);
            else                     *p++ = JitPpc::sraw(JitPpc::R5, JitPpc::R5, JitPpc::R6);
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

    outCycles = regShift ? 2 : 1;
    return true;
}

void *ArmPpcJit::compileStub(Interpreter *cpu, uint32_t instrPC, uint32_t opcode, bool thumb) {
    s_linkOn = false; s_linkBudget = 0; // v1.119: linking is for blocks only
    ensureSpace(MAX_STUB_WORDS + 320);
    ensureSlowTramp(cpu);
    alignCodePool();
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

static inline bool canTranslateNativeThumb(bool arm7, uint16_t opcode) {
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
            if (aluOp == 0xD) return !arm7 && !NO_TMUL; // v1.113: MUL, fixed 4 cycles and N/Z only on the ARM9
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

// v1.113: ARM9 data processing with a register-specified shift (Rm shifted by Rs).
// Logical S forms need the shifter carry and stay with the interpreter; so does any PC operand.
static inline bool isRegShiftAluArm(bool arm7, uint32_t opcode) {
    if (arm7) return false;
    if ((opcode & 0x0E000090u) != 0x00000010u) return false;
    if (((opcode >> 12) & 0xFu) == 15u || ((opcode >> 16) & 0xFu) == 15u ||
        ((opcode >> 8) & 0xFu) == 15u || (opcode & 0xFu) == 15u) return false;
    uint8_t aluOp = (uint8_t)((opcode >> 21) & 0xFu);
    bool s = (opcode & (1u << 20)) != 0;
    if (aluOp >= 0x8u && aluOp <= 0xBu && !s) return false;
    if ((aluOp == 0x5u || aluOp == 0x6u || aluOp == 0x7u) && s) return false;
    bool logical = (aluOp == 0x0 || aluOp == 0x1 || aluOp == 0x8 || aluOp == 0x9 || aluOp >= 0xC);
    if (logical && s) return false;
    return true;
}

// ---------------------------------------------------------------------------
static inline bool canTranslateNativeArm(bool arm7, uint32_t opcode) {
    if (isNativeLoadArm(arm7, opcode)) return true;
    if (isNativeStoreArm(opcode)) return true;
    if (isNativePushPopArm(opcode)) return true;
    if (isNativeMrcArm(arm7, opcode)) return true;
#if NOODS_JIT_PSR_COMPILE
    if (!arm7 && (isNativeMsrRegArm(false, opcode) || isNativeMsrImmArm(false, opcode))) {
        return s_psrCpu ? ArmPpcJit::psrArmCompilable(s_psrCpu, opcode) : false;
    }
#endif
    if ((opcode & 0x0C000000u) != 0x00000000u) return false;
    bool isImm = (opcode & (1u << 25)) != 0;
    if (!isImm) {
        if ((opcode & (1u << 4)) != 0) {
            if ((opcode & 0x0FFF0FF0u) == 0x016F0F10u) {
                uint8_t rd = (uint8_t)((opcode >> 12) & 0xF);
                uint8_t rm = (uint8_t)(opcode & 0xF);
                return (rd != 15 && rm != 15);
            }
            return isRegShiftAluArm(arm7, opcode) || isNativeMulArm(arm7, opcode) || isNativeDspMulArm(arm7, opcode);
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

// v1.81: predicated (conditional) ARM data-processing instructions.
//
// ARM9/ARM7 execute any instruction conditionally. Historically any cond != AL
// terminated a block, so blocks averaged 2-3 instructions and a third of all
// dispatches fell back to the interpreter. We can translate them inline instead,
// guarded by a PPC condition-register test.
//
// Only instructions whose interpreter cycle cost is exactly 1 are eligible, so
// the compile-time cycle accounting is identical whether the guard fires or not
// (the interpreter charges 1 cycle for a skipped conditional instruction, and
// register-specified shifts - the only ALU case costing 2 - are excluded).
static inline bool isCondSafeArm(uint32_t opcode) {
    if ((opcode & 0x0C000000u) != 0x00000000u) return false;  // data processing only
    if ((opcode >> 28) == 0xFu) return false;                 // NV is not materialisable in CR
    if (((opcode >> 12) & 0xFu) == 0xFu) return false;        // R15 destination
    bool isImm = (opcode & (1u << 25)) != 0;
    if (!isImm) {
        if ((opcode & (1u << 4)) != 0) return false;          // multiply / swap / halfword group
        if ((opcode & (1u << 7)) != 0) return false;          // register-specified shift => 2 cycles
        uint8_t shiftType = (uint8_t)((opcode >> 5) & 0x3u);
        uint8_t shiftImm  = (uint8_t)((opcode >> 7) & 0x1Fu);
        if (shiftType == 3u && shiftImm == 0u) return false;  // RRX reads CPSR.C
    }
    // An instruction that reads R15 materialises it with a store that is emitted
    // after the guard: if the guard skips the body, the compile-time
    // appliedPcDelta bookkeeping still advances and the block's exit R15 fix-up
    // adds the wrong delta.  Predicating PC reads is not worth modelling.
    if (((opcode >> 16) & 0xFu) == 0xFu) return false;        // Rn == PC
    if (!isImm && (opcode & 0xFu) == 0xFu) return false;      // Rm == PC
    uint8_t aluOp = (uint8_t)((opcode >> 21) & 0xFu);
    bool s = (opcode & (1u << 20)) != 0;
    if (aluOp >= 0x8u && aluOp <= 0xBu && !s) return false;   // MRS/MSR encoding space
    if ((aluOp == 0x5u || aluOp == 0x6u || aluOp == 0x7u) && s) return false; // ADCS/SBCS/RSCS
    return true;
}

// CPSR NZCV are copied into CR0 as {LT,GT,EQ,SO} = {N,Z,C,V} with one mtcrf.
// Emit the CR test for an ARM condition. Returns false for AL/NV (no guard).
// `trueWhenSet` says whether the tested bit holds the condition's *true* value;
// the caller turns that into the polarity of the skip branch. Keeping the
// polarity here (rather than in a second table) matters: for LS/LE the CR7
// temporary holds the complementary result (HI/GT), which an earlier version
// of this code got backwards.
static inline bool emitCondTest(uint32_t *&p, uint8_t cond, uint32_t *&slot, uint8_t &bi,
                                bool &trueWhenSet) {
    // CR0 bit numbers: 0=N, 1=Z, 2=C, 3=V
    trueWhenSet = true;
    switch (cond) {
    case 0x0: bi = 1; break;               // EQ: Z
    case 0x1: bi = 1; trueWhenSet = false; break;   // NE: !Z
    case 0x2: bi = 2; break;               // CS: C
    case 0x3: bi = 2; trueWhenSet = false; break;   // CC: !C
    case 0x4: bi = 0; break;               // MI: N
    case 0x5: bi = 0; trueWhenSet = false; break;   // PL: !N
    case 0x6: bi = 3; break;               // VS: V
    case 0x7: bi = 3; trueWhenSet = false; break;   // VC: !V
    case 0x8:                              // HI: C && !Z
        *p++ = JitPpc::crnor(28, 1, 1);    // CR7[0] = !Z
        *p++ = JitPpc::crand(28, 2, 28);   // CR7[0] = C & !Z
        bi = 28; break;
    case 0x9:                              // LS: !(C && !Z)
        *p++ = JitPpc::crnor(28, 1, 1);    // CR7[0] = !Z
        *p++ = JitPpc::crand(28, 2, 28);   // CR7[0] = C & !Z  (= HI, i.e. !LS)
        bi = 28; trueWhenSet = false; break;
    case 0xA:                              // GE: N == V
        *p++ = JitPpc::creqv(28, 0, 3);
        bi = 28; break;
    case 0xB:                              // LT: N != V
        *p++ = JitPpc::crxor(28, 0, 3);
        bi = 28; break;
    case 0xC:                              // GT: !Z && (N == V)
        *p++ = JitPpc::creqv(28, 0, 3);
        *p++ = JitPpc::crnor(29, 1, 1);
        *p++ = JitPpc::crand(28, 28, 29);
        bi = 28; break;
    case 0xD:                              // LE: !(!Z && (N == V))
        *p++ = JitPpc::creqv(28, 0, 3);
        *p++ = JitPpc::crnor(29, 1, 1);
        *p++ = JitPpc::crand(28, 28, 29);  // CR7[0] = GT, i.e. !LE
        bi = 28; trueWhenSet = false; break;
    default:
        return false;                      // AL / NV: no guard
    }
    slot = p;                              // patched to "branch if condition false"
    *p++ = JitPpc::bc(4, bi, 0);
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
    if (isNativeMulArm(false, op)) return false;
    if (isNativeDspMulArm(false, op)) return false;
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

static inline uint8_t armWrittenFlagsMask(bool arm7, uint32_t opcode) {
    if (isMemClassArm(opcode)) return 0x0u;
    if (isNativeMrcArm(arm7, opcode)) return 0x0u;
    // v1p190: MSR CPSR with the flags byte writes N,Z,C,V,Q through cpu->cpsr, which the
    // block's CR0 tag does not see. Report them so the native path re-tags or a later
    // predicated instruction re-reads CPSR.
    // v1p190: these forms read or write cpu->cpsr directly, so a CR0 tag cached before the
    // instruction no longer describes CPSR afterwards. Reporting all four flags closes the
    // block's condition group here and the next predicated instruction re-tags CR0.
    if (!arm7 && (isNativeMsrRegArm(false, opcode) || isNativeMsrImmArm(false, opcode)) &&
        (opcode & (1u << 19))) return 0xFu;
    if ((opcode & 0x0FFF0FF0u) == 0x016F0F10u) return 0x0u; // CLZ
    if (isNativeMulArm(false, opcode)) return 0x0u;
    if (isNativeDspMulArm(false, opcode)) return 0x0u;
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
    if (isNativeMulArm(false, opcode)) return false;
    if (isNativeDspMulArm(false, opcode)) return false;
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
    if (isNativeDspMulArm(false, op)) return 1;
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
    if (isNativeLoadArm(false, op)) {
        uint8_t rd = (uint8_t)((op >> 12) & 0xFu);
        uint8_t rn = (uint8_t)((op >> 16) & 0xFu);
        bool pre = (op & (1u << 24)) != 0;
        bool wb = ((op & (1u << 21)) != 0) || !pre;
        uint32_t mask = 1u << rd;
        if (wb && rn != 15 && rn != rd) mask |= 1u << rn;
        return mask;
    }
    if (isNativeMrcArm(false, op)) return 1u << ((op >> 12) & 0xFu);
    if (isNativeMsrRegArm(false, op) || isNativeMsrImmArm(false, op)) return 0u;
    if (isMemClassArm(op)) return 1u << ((op >> 12) & 0xFu);
    if ((op & 0x0FFF0FF0u) == 0x016F0F10u) return 1u << ((op >> 12) & 0xFu);
    if (isNativeMulArm(false, op)) {
        uint32_t w = 1u << ((op >> 16) & 0xFu);
        if (op & 0x00800000u) w |= 1u << ((op >> 12) & 0xFu);
        return w;
    }
    if (isNativeDspMulArm(false, op)) {
        if ((op & 0x0FF00090u) == 0x01400080u) { // SMLALxy
            return (1u << ((op >> 16) & 0xFu)) | (1u << ((op >> 12) & 0xFu));
        }
        return 1u << ((op >> 16) & 0xFu);
    }
    uint8_t aluOp = (uint8_t)((op >> 21) & 0xFu);
    if (aluOp >= 8u && aluOp <= 11u) return 0u;
    return 1u << ((op >> 12) & 0xFu);
}

bool ArmPpcJit::compileBlock(Interpreter *cpu, uint32_t instrPC, uint32_t opcode0, bool thumb, BlockEntry &entry) {
    g_evCnt[5]++;
    s_psrCpu = cpu;
    s_linkOn = NOODS_BLOCK_LINK && !NOODS_MPU_PURE_JIT && !cpu->core->dsiMode; // v1.119, v1.120
    s_linkBudget = s_linkOn ? NOODS_LINK_BUDGET : 0;
    s_linkBlockPC = instrPC;
    int cpuIdx = cpu->arm7 ? 1 : 0;
    const int16_t step = thumb ? 2 : 4;
    const int16_t offR15   = (int16_t)((uintptr_t)&cpu->registersUsr[15] - (uintptr_t)cpu);
    const int16_t offPcDat = (int16_t)((uintptr_t)&cpu->pcData - (uintptr_t)cpu);
    const int16_t offPipe0 = (int16_t)((uintptr_t)&cpu->pipeline[0] - (uintptr_t)cpu);
    const int16_t offPipe1 = (int16_t)((uintptr_t)&cpu->pipeline[1] - (uintptr_t)cpu);

    // Pass 1: Scan guest instruction stream before touching codePool
    bool firstIsNative = thumb ? canTranslateNativeThumb(cpu->arm7, (uint16_t)opcode0) : (canTranslateNativeArm(cpu->arm7, opcode0) || isCondSafeArm(opcode0));
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
    // A block ends two instructions after its first store, because a store may rewrite code
    // and the interpreter has only those two instructions prefetched. With NOODS_STORE_EXIT
    // an unconditional store carries a run-time check instead (see Pass 3): if it took the
    // slow path (code line, I/O, unmapped) the block exits right after it with the two
    // prefetched opcodes in the pipeline; a fast-path store cannot have touched a line that
    // holds compiled code, which includes every instruction of this block, so it continues.
    // Conditional stores (only possible as the first instruction) keep the two-instruction rule.
    const bool soloBlock = NOODS_STORE_EXIT && !cpu->arm7 && soloBit != 0u && !firstIsLoad;
    auto storeCanCheck = [&](uint32_t op) { return soloBlock && (thumb || (op >> 28) == 0xEu); };
    bool legacyStore = sawStore && !storeCanCheck(opcode0);
    // Solo blocks also continue past a not-taken conditional branch ("side exit").
    const bool soloExt = soloBlock && !legacyStore && !firstIsTerm;
    bool sideExit[MAX_BLOCK_INSTRS + 4] = {};
    bool anySideExit = false;
    int postStoreLeft = legacyStore ? 2 : 99;

    if (firstIsBranch) {
        hasBranchTerm = true;
        branchOpcode = opcode0;
    } else if (firstIsPcLoad) {
        hasPcLoadTerm = true;
        branchOpcode = opcode0;
    }

    // v1.117: a Thumb MUL (ARM9) is compiled, but only as a block of its own. Letting it sit
    // inside a longer block changed game timing in testing (cause not found); as a one-
    // instruction block it runs exactly when the interpreted instruction did.
    const bool headTMul = thumb && !cpu->arm7 && (opcode0 & 0xFFC0u) == 0x4340u;
    for (uint16_t m = 1; m < ((firstIsTerm || headTMul) ? 1 : MAX_BLOCK_INSTRS) && postStoreLeft > 0; m++) {
        uint32_t simR15 = r15After0 + m * (uint32_t)step;
        if ((simR15 & 0xFFFu) < 8u || (simR15 & 0xFFFu) > (NOODS_PAGE_TAIL ? 0xFF0u : 0xFA0u)) break;

        uint32_t opM = (m == 1) ? opcode1
                                : (thumb ? (uint32_t)U8TO16(pcData0 + (m - 2) * 2, 0)
                                         : (uint32_t)U8TO32(pcData0 + (m - 2) * 4, 0));

#if NOODS_MPU_PURE_JIT
        // When compiling through the strict interpreter, end before any
        // potentially memory-touching / control-flow / banking instruction.
        if (!cpu->arm7 && cpu->core->cp15.mpuRestricted &&
            (!cpu->core->cp15.mpuFastPriv || !cpu->isPrivileged()) &&
            !(thumb ? mpuPureThumb((uint16_t)opM) : mpuPureArm(opM))) {
            // Solo ARM9 may terminate a register-only block with a direct ARM
            // branch. Dispatch later requires a same-page, inline branch and
            // both execute and data-read permission on that page.
            bool directBranch = soloBit && !thumb && (opM >> 28) != 0xFu &&
                                (opM & 0x0E000000u) == 0x0A000000u;
            if (!directBranch) break;
        }
#endif
        if (!thumb && !cpu->arm7 && (opM & 0x0F000010u) == 0x0E000010u) {
            break;
        }

        if (!thumb && (opM >> 28) != 0xEu) {
            if (isTerminatorBranchArm(cpu->arm7, opM)) {
                hasBranchTerm = true;
                branchOpcode = opM;
                count++;
                break;
            }
            if (isPcDestLoadTerminatorArm(cpu->arm7, opM)) {
                hasPcLoadTerm = true;
                branchOpcode = opM;
                count++;
                break;
            }
#if NOODS_V181_COND
            // Predicated data-processing instruction: stays inside the block and
            // is guarded at run time instead of ending the block here.
            if (isCondSafeArm(opM)) {
                opList[count++] = opM;
                continue;
            }
#endif
            break;
        }

        if (thumb && !cpu->arm7 && (opM & 0xFFC0u) == 0x4340u) break; // v1.117: see headTMul

        if (thumb ? canTranslateNativeThumb(cpu->arm7, (uint16_t)opM) : canTranslateNativeArm(cpu->arm7, opM)) {
            opList[count++] = opM;
            if (legacyStore) postStoreLeft--;
            if (isAnyStoreOp(cpu->arm7, opM, thumb)) {
                sawStore = true;
                if (!legacyStore && !storeCanCheck(opM)) {
                    legacyStore = true;
                    postStoreLeft = 2; // Match real ARM 2-stage instruction prefetch across self-modifying stores!
                }
            }
            continue;
        }

        if (soloExt && NOODS_SIDE_EXIT && thumb && (opM & 0xF000u) == 0xD000u && (opM & 0x0F00u) < 0x0E00u) {
            // Same conditions as emitInlineBranchThumb: target in this page, short delta.
            const uint32_t brAddr = instrPC + (uint32_t)(m * step);
            const uint32_t tgt = (brAddr + 4u + (uint32_t)((int32_t)(int8_t)(opM & 0xFFu) << 1)) & ~1u;
            const int32_t tDelta = (int32_t)(tgt + 2u) - (int32_t)(instrPC + 4u);
            if (tgt != instrPC && (((tgt ^ instrPC) | ((tgt + 2u) ^ instrPC)) >> 12) == 0 && tDelta >= -0x800 && tDelta <= 0x7FF) {
                sideExit[count] = true;
                anySideExit = true;
                opList[count++] = opM;
                continue;
            }
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

    // NOODS_STORE_EXIT: where the pre-v1.104 blocks covering these instructions ended
    // (oldStart[m] / oldEnd[m] = first / last instruction of the old block that held
    // instruction m). An old block ran from its start to its terminating branch, to two
    // instructions past its first store, or for MAX_BLOCK_INSTRS instructions. A load or
    // store that takes its slow path (I/O, code line, unmapped) makes the block end at that
    // same instruction, so interrupts and events raised by the access are seen exactly when
    // they were before. Returns the start of the last (unfinished) old block.
    bool exitAfter[MAX_BLOCK_INSTRS + 4] = {};
    uint8_t oldEnd[MAX_BLOCK_INSTRS + 4];
    uint8_t oldStart[MAX_BLOCK_INSTRS + 4] = {};
    uint16_t prefAt[MAX_BLOCK_INSTRS + 4] = {};  // cycles from block entry to instruction m
    auto simOldBlocks = [&](int n) -> int {
        std::memset(oldEnd, 0xFF, sizeof(oldEnd));
        std::memset(exitAfter, 0, sizeof(exitAfter));
        int segStart = 0, storeEnd = -1;
        bool segMem = false;
        for (int m = 0; m < n; m++) {
            const uint32_t opM = opList[m];
            oldStart[m] = (uint8_t)segStart;
            if (!sideExit[m]) {
                if (thumb ? isAnyMemThumb((uint16_t)opM) : isMemClassArm(opM)) segMem = true;
                if (storeEnd < 0 && isAnyStoreOp(cpu->arm7, opM, thumb)) storeEnd = m + 2;
            }
            if (sideExit[m] || m == storeEnd || m == segStart + MAX_BLOCK_INSTRS - 1) {
                for (int k = segStart; k <= m; k++) oldEnd[k] = (uint8_t)m;
                if (segMem && m < (int)count - 1) exitAfter[m] = true;
                segStart = m + 1; storeEnd = -1; segMem = false;
            }
        }
        return segStart;
    };
    // A branch back into this block at instruction s >= 1 closes a loop whose head the old
    // blocks dispatched at s. If that head block is a self-loop block it keeps iterating by
    // its own rule (an iteration that starts before the next event runs to its end), which
    // an entry through the dispatcher does not reproduce. End this block before s so the
    // loop is entered through its own block, as before.
    if (soloExt) {
        uint16_t cut = count;
        for (uint16_t k = 1; k < count; k++) {
            const bool lastIsTerm = (k == count - 1) && hasBranchTerm;
            // Terminators are held separately, not necessarily written to opList.
            const uint32_t opK = lastIsTerm ? branchOpcode : opList[k];
            const uint32_t brAddr = instrPC + (uint32_t)(k * step);
            uint32_t tgt;
            if (thumb) {
                if ((sideExit[k] || lastIsTerm) && (opK & 0xF000u) == 0xD000u && (opK & 0x0F00u) < 0x0E00u)
                    tgt = (brAddr + 4u + (uint32_t)((int32_t)(int8_t)(opK & 0xFFu) << 1)) & ~1u;
                else if (lastIsTerm && (opK & 0xF800u) == 0xE000u)
                    tgt = (brAddr + 4u + (uint32_t)(((int32_t)(int16_t)((uint16_t)opK << 5)) >> 4)) & ~1u;
                else continue;
            } else {
                if (lastIsTerm && (opK & 0x0F000000u) == 0x0A000000u && (opK >> 28) != 0xFu)
                    tgt = (brAddr + 8u + (uint32_t)((int32_t)(opK << 8) >> 6)) & ~3u;
                else continue;
            }
            if (tgt > instrPC && tgt <= brAddr) {
                const uint16_t sIdx = (uint16_t)((tgt - instrPC) / (uint32_t)step);
                if (sIdx < cut) cut = sIdx;
            }
        }
        if (cut < count) {
            count = cut;
            hasBranchTerm = false;
            hasPcLoadTerm = false;
            anySideExit = false;
            for (uint16_t k = 0; k < MAX_BLOCK_INSTRS; k++) {
                if (k >= count) sideExit[k] = false;
                anySideExit |= sideExit[k];
            }
        }
    }
    // The instruction limit stopped the scan in the middle of an old block: end at the last
    // old block boundary instead, so that the next block again starts where an old one did.
    if (soloExt && !hasBranchTerm && !hasPcLoadTerm && count == MAX_BLOCK_INSTRS) {
        const int lastStart = simOldBlocks((int)count);
        if (lastStart > 0 && lastStart < (int)count) {
            count = (uint16_t)lastStart;
            for (uint16_t k = count; k < MAX_BLOCK_INSTRS; k++) sideExit[k] = false;
        }
    }
    // A conditional branch that ended up last is an ordinary terminator.
    if (!hasBranchTerm && !hasPcLoadTerm && sideExit[count - 1]) {
        sideExit[count - 1] = false;
        hasBranchTerm = true;
        branchOpcode = opList[count - 1];
        anySideExit = false;
        for (uint16_t k = 0; k < count; k++) anySideExit |= sideExit[k];
    }
    if (count < 2 && !firstIsTerm && !(NOODS_SINGLE_BLOCK && firstIsNative) && !hasBranchTerm) {
        return false;
    }

    // Pass 2: Backward CPSR flag-liveness analysis across the native prefix
    const bool hasTerm = hasBranchTerm || hasPcLoadTerm;
    uint16_t nativeCount = hasTerm ? (uint16_t)(count - 1) : count;
    uint16_t nativeStart = firstIsLoad ? 1u : 0u;
    if (nativeCount < nativeStart || (!hasTerm && nativeCount <= nativeStart)) {
        return false;
    }
    if (soloExt) simOldBlocks((int)nativeCount);
    bool emitFlagsList[MAX_BLOCK_INSTRS];
    uint8_t liveFlags = 0xFu;
    for (int m = (int)nativeCount - 1; m >= (int)nativeStart; m--) {
        uint32_t opM = opList[m];
        // The block can end here, and a branch reads the flags: all of them must be up to date.
        if (exitAfter[m] || sideExit[m]) liveFlags = 0xFu;
        uint8_t w = thumb ? thumbWrittenFlagsMask((uint16_t)opM) : armWrittenFlagsMask(cpu->arm7, opM);
        emitFlagsList[m] = ((w & liveFlags) != 0);
#if NOODS_V181_COND
        // A predicated instruction may not execute, so its flag write cannot kill
        // liveness, and its guard reads all four flags: everything before it is live.
        if (!thumb && (opM >> 28) != 0xEu) {
            liveFlags = 0xFu;
        } else
#endif
        {
        liveFlags &= (uint8_t)~w;
        }
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
                if (target == instrPC && !firstIsLoad && !sawStore && !anySideExit) {
                    wantSelfLoop = true;
                }
            }
        }
    }

    // Do not coalesce loop iterations in restricted MPU mode: retain one
    // branch per dispatch and the existing scheduler/event budget.
    if (!cpu->arm7 && cpu->core->cp15.mpuRestricted &&
        (!cpu->core->cp15.mpuFastPriv || !cpu->isPrivileged())) wantSelfLoop = false;

    // Pass 3: Emit PowerPC code into codePool
    ensureSpace(MAX_BLOCK_WORDS + 576);
    if (NOODS_LINK_DYN && s_linkOn && !cpu->arm7 && soloBit != 0u && !s_dynStub) emitDynStub(cpu); // v1.119
    ensureSlowTramp(cpu); // v1.121
    if (s_linkOn) emitLinkStub(cpu, thumb); // v1.121
    alignCodePool();
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

    bool anyChecked = false;
    uint32_t *loopTop = nullptr;
    if (wantSelfLoop) {
        *p++ = JitPpc::li(JitPpc::R29, 0); // taken iteration accumulator = 0
        // v1.103: count-down delay loop -> skip the guaranteed-taken iterations up front.
        if (nativeCount == 1 && nativeStart == 0 && count == 2) {
            uint32_t spinRd = 16u, spinK = 0u, spinCond = 0u;
            if (!thumb) {
                const uint32_t bc = branchOpcode >> 28;
                if ((opcode0 >> 28) == 0xEu && (opcode0 & 0x0FF00000u) == 0x02500000u &&
                    ((opcode0 >> 16) & 0xFu) == ((opcode0 >> 12) & 0xFu) && (bc == 1u || bc == 2u)) {
                    const uint32_t rot = ((opcode0 >> 8) & 0xFu) * 2u, imm = opcode0 & 0xFFu;
                    spinRd = (opcode0 >> 12) & 0xFu;
                    spinK = rot ? ((imm >> rot) | (imm << (32u - rot))) : imm;
                    spinCond = bc;
                }
            } else {
                const uint16_t o0 = (uint16_t)opcode0, bo = (uint16_t)branchOpcode;
                if ((bo & 0xFF00u) == 0xD100u || (bo & 0xFF00u) == 0xD200u) {
                    if ((o0 & 0xF800u) == 0x3800u) { // SUB Rd, #imm8
                        spinRd = (o0 >> 8) & 7u; spinK = o0 & 0xFFu;
                    } else if ((o0 & 0xFE00u) == 0x1E00u && ((o0 >> 3) & 7u) == (o0 & 7u)) { // SUB Rd, Rd, #imm3
                        spinRd = o0 & 7u; spinK = (o0 >> 6) & 7u;
                    }
                    spinCond = (bo >> 8) & 0xFu;
                }
            }
            if (spinRd < 15u && spinK >= 1u && spinK <= 0xFFFFu) {
                helperInBlock = true;
                *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
                *p++ = JitPpc::mr(JitPpc::R4, JitPpc::R30);
                *p++ = JitPpc::li(JitPpc::R5, (int16_t)spinRd);
                emitLoadImm32(p, JitPpc::R6, spinK);
                *p++ = JitPpc::li(JitPpc::R7, (int16_t)spinCond);
                emitLoadImm32(p, JitPpc::R12, (uint32_t)(uintptr_t)&ArmPpcJit::spinFastForward);
                *p++ = JitPpc::mtctr(JitPpc::R12);
                *p++ = JitPpc::bctrl();
                *p++ = JitPpc::add(JitPpc::R29, JitPpc::R29, JitPpc::R3);   // iterations done
                *p++ = JitPpc::subf(JitPpc::R30, JitPpc::R3, JitPpc::R30);  // iterations left (>= 1)
            }
        }
        loopTop = p;
    }

#if NOODS_V181_COND
    // Predication state: each guard re-tags CR0 from CPSR{N,Z,C,V}, so no
    // compile-time cache can go stale across a helper call or a flag write.
    bool groupOpen = false;
    uint32_t *groupSlot = nullptr;
    uint8_t groupCond = 0, groupBi = 0;
    bool groupTrueWhenSet = true;
    // The dispatcher only enters a block whose first instruction's condition is
    // true, but for conditional heads or self-loops, we guard from nativeStart.
    const uint16_t guardFrom = ((opcode0 >> 28) != 0xEu) ? 0u : ((nativeStart == 0 && !wantSelfLoop) ? 1u : nativeStart);

    auto closeGroup = [&]() {
        if (!groupOpen) return;
        int16_t disp = (int16_t)((p - groupSlot) * 4);
        // Skip the guarded body when the condition is false: branch on the bit
        // being clear if it carries the true value, else on it being set.
        *groupSlot = JitPpc::bc(groupTrueWhenSet ? 4u : 12u, groupBi, disp);
        groupOpen = false;
        groupSlot = nullptr;
    };
#endif

    // NOODS_POLL_LOOP: a wait loop gets a fast-forward call at its head. The descriptor is
    // stored in the code stream, so it lives exactly as long as the block does.
    PollLoop *pollDesc = nullptr;
    if (NOODS_POLL_LOOP && soloExt && thumb && hasBranchTerm && !loopTop && !firstIsLoad && nativeStart == 0) {
        PollLoop pd;
        uint32_t ops[MAX_BLOCK_INSTRS];
        std::memcpy(ops, opList, sizeof(uint32_t) * (count - 1u));
        ops[count - 1u] = branchOpcode;
        if (analysePollLoop(ops, (int)count, instrPC, pd)) {
            const uint32_t words = (uint32_t)((sizeof(PollLoop) + 3u) / 4u);
            *p = JitPpc::b((int32_t)((words + 1u) * 4u));
            pollDesc = (PollLoop *)(p + 1);
            std::memcpy(pollDesc, &pd, sizeof(pd));
            p += words + 1u;
            helperInBlock = true;
            *p++ = JitPpc::mr(JitPpc::R3, JitPpc::R31);
            emitLoadImm32(p, JitPpc::R4, (uint32_t)(uintptr_t)pollDesc);
            emitLoadImm32(p, JitPpc::R12, (uint32_t)(uintptr_t)&ArmPpcJit::pollFastForward);
            *p++ = JitPpc::mtctr(JitPpc::R12);
            *p++ = JitPpc::bctrl();
        }
    }

    for (uint16_t m = nativeStart; m < nativeCount; m++) {
        uint32_t opM = opList[m];
#if NOODS_V181_COND
        const uint8_t mcond = thumb ? 0xEu : (uint8_t)(opM >> 28);
        if (mcond != 0xEu && m >= guardFrom) {
            if (!groupOpen || groupCond != mcond) {
                closeGroup();
                // Re-tag CR0 from CPSR for *every* group. CR0/CR7 are caller-saved,
                // and any helper call inside the block (a load slow path, a shift
                // helper, ...) silently destroys a cached tag; a stale tag means a
                // guarded instruction runs when it must not. Two words per group.
                *p++ = JitPpc::lwz(JitPpc::R3, JitPpc::R31, (int16_t)offsetof(Interpreter, cpsr));
                *p++ = JitPpc::mtcrf(0x80u, JitPpc::R3);       // CR0 = {N,Z,C,V}
                uint32_t *slot = nullptr;
                uint8_t bi = 0;
                bool trueWhenSet = true;
                if (!emitCondTest(p, mcond, slot, bi, trueWhenSet)) {
                    // Condition can't be materialised in CR: abandon the block.
                    return false;
                }
                JIT_STAT(stats.condGuards[cpuIdx]++);
                groupSlot = slot;
                groupCond = mcond;
                groupBi = bi;
                groupTrueWhenSet = trueWhenSet;
                groupOpen = true;
            }
        } else if (groupOpen) {
            closeGroup();
        }
#endif
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
        // NOODS_STORE_EXIT: checked memory access. R30 (loop budget) and R29 (loop counter)
        // are only used by self-loop blocks, which never use this, so they are free here.
        // Every slow path sets R30 to 1 (emitHelperR15Sync). R29 holds the index of the
        // instruction after which the block has to end (0 = none).
        const bool isMemM = !sideExit[m] && (thumb ? isAnyMemThumb((uint16_t)opM) : isMemClassArm(opM));
        const bool checkedMem = !loopTop && isMemM && exitAfter[oldEnd[m] < MAX_BLOCK_INSTRS ? oldEnd[m] : MAX_BLOCK_INSTRS];
        if (checkedMem) {
            if (!anyChecked) *p++ = JitPpc::li(JitPpc::R29, 0);
            anyChecked = true;
            *p++ = JitPpc::li(JitPpc::R30, 0);
        }
        prefAt[m] = prefixCycles;
        s_emitTimeShift = (soloExt && isMemM) ? (int)prefAt[oldStart[m]] : 0;
        s_emitGlobalCyclesAddr = (uint32_t)(uintptr_t)&cpu->core->globalCycles;
        if (sideExit[m]) {
            // Taken: leave exactly like a terminating branch. Not taken: one cycle, carry on.
            bool okS = emitInlineBranchThumb(p, cpu, (uint16_t)opM, instrPC, (int32_t)(m * step),
                                             (int32_t)prefixCycles, helperInBlock, nullptr, true);
            assert(okS);
            (void)okS;
            cm = 1;
        } else {
            bool okM = thumb ? emitNativeThumb(p, cpu, (uint16_t)opM, cm, emitFlagsList[m])
                             : emitNativeArm(p, cpu, opM, cm, emitFlagsList[m]);
            assert(okM);
            (void)okM;
        }
        s_emitTimeShift = 0;
        prefixCycles = (uint16_t)(prefixCycles + cm);
        if (checkedMem) {
            // Slow path taken: end the block where it ended before v1.104.
            *p++ = JitPpc::cmpwi(0, JitPpc::R30, 0);
            uint32_t *bFast = p++;
            *p++ = JitPpc::li(JitPpc::R29, (int16_t)oldEnd[m]);
            *bFast = JitPpc::beq((int16_t)((p - bFast) * 4));
        }
        if (exitAfter[m]) {
#if NOODS_V181_COND
            closeGroup(); // the test below must run whether or not instruction m was skipped
#endif
            if (!anyChecked) { *p++ = JitPpc::li(JitPpc::R29, 0); anyChecked = true; }
            *p++ = JitPpc::cmpwi(0, JitPpc::R29, (int16_t)m);
            uint32_t *bCont = p++;
            // Same state as a native block end after instruction m.
            emitLoadImm32(p, JitPpc::R5, instrPC + (uint32_t)((m + 2) * step));
            *p++ = JitPpc::stw(JitPpc::R5, JitPpc::R31, offR15);
            *p++ = JitPpc::lwz(JitPpc::R4, JitPpc::R31, offPcDat);
            *p++ = JitPpc::addi(JitPpc::R4, JitPpc::R4, (int16_t)(m * step));
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
            *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 36);
            *p++ = JitPpc::mtlr(JitPpc::R0);
            *p++ = JitPpc::lmw(JitPpc::R28, JitPpc::R1, 16);
            *p++ = JitPpc::addi(JitPpc::R1, JitPpc::R1, 32);
            *p++ = JitPpc::blr();
            *bCont = JitPpc::bne((int16_t)((p - bCont) * 4));
        }
#if NOODS_V181_COND
        // CR0 is stale once the flags change; close the current condition group.
        uint8_t wroteFlags = thumb ? thumbWrittenFlagsMask((uint16_t)opM) : armWrittenFlagsMask(cpu->arm7, opM);
        if (wroteFlags != 0) {
            closeGroup();   // a group may not straddle a flag write
        }
#endif
    }
#if NOODS_V181_COND
    closeGroup();
#endif

    if (pollDesc) {
        pollDesc->iterCycles = (uint32_t)prefixCycles + 3u; // closing branch taken
        pollDesc->prefix = (uint32_t)prefixCycles & 0xFFu;
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
        uintptr_t helperAddr = (uintptr_t)(thumb ? (void *)&ArmPpcJit::execBranchThumb
                                                 : (void *)&ArmPpcJit::execBranchArm);
        if (NOODS_BLOCK_LINK && NOODS_LINK_DYN && s_linkOn && s_dynStub && !cpu->arm7 && soloBit != 0u) {
            // v1.119: call the branch helper, then try to enter the target block directly
            emitLoadImm32(p, JitPpc::R12, (uint32_t)helperAddr);
            *p++ = JitPpc::mtctr(JitPpc::R12);
            *p++ = JitPpc::bctrl();
            emitDynLink(p);
        } else {
        if (helperInBlock) {
            *p++ = JitPpc::lwz(JitPpc::R0, JitPpc::R1, 36);
            *p++ = JitPpc::mtlr(JitPpc::R0);
        }
        emitLoadImm32(p, JitPpc::R12, (uint32_t)helperAddr);
        *p++ = JitPpc::mtctr(JitPpc::R12);
        *p++ = JitPpc::lmw(JitPpc::R28, JitPpc::R1, 16);
        *p++ = JitPpc::addi(JitPpc::R1, JitPpc::R1, 32);
        *p++ = JitPpc::bctr();
        }
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
        s_linkBudget = (s_linkOn && NOODS_LINK_FALL) ? 1 : 0;
        emitLinkTry(p, cpu, instrPC + (uint32_t)(count * step), thumb, (int32_t)prefixCycles, helperInBlock); // v1.119
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
#if NOODS_JIT_FORWARD
    const unsigned forwarded = NoodsJit::forwardPpcGuestLoads(start, words, (int)offsetof(Interpreter, registersUsr));
    s_forwardLoads += forwarded;
    s_forwardBlocks += forwarded != 0;
#endif
    flushCodeRange(start, words * sizeof(uint32_t));

    // Mark exact 64-byte code lines covered by [instrPC .. instrPC + (count + 2) * step]
    if ((instrPC >> 27) == 0u) {
        uint32_t endCodeAddr = instrPC + (uint32_t)((count + 2) * step);
        for (uint32_t line = (instrPC >> 6); line <= (endCodeAddr >> 6); line++) {
            codeLineBits[(line >> 5) & (CODE_LINE_WORDS - 1)] |= (1u << (line & 31u));
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
#if NOODS_MPU_PURE_JIT
    // 0x0400 is otherwise used only as a loop-kind bit. It marks a
    // no-loop, no-control-flow block, independently classified instruction by
    // instruction. Linked-block generation is disabled in this build.
    const bool strictSoloBranch = soloBit && !thumb && hasBranchTerm && isInlineSamePageBr && !emittedLoop;
    bool strictPure = !cpu->arm7 && (!hasBranchTerm || strictSoloBranch) && !hasPcLoadTerm && !anySideExit &&
                      !firstIsLoad && !anyMemInPrefix && !sawStore && !emittedLoop;
    const uint16_t strictCount = hasBranchTerm ? nativeCount : count;
    for (uint16_t m = 0; strictPure && m < strictCount; ++m)
        strictPure = thumb ? mpuPureThumb((uint16_t)opList[m]) : mpuPureArm(opList[m]);
    if (strictPure) { metaFlags |= 0x0400u; MPU_PATH_STAT(++g_mpuPureJitStats[0]); }
#endif
    entry.instrCount = (uint16_t)((count & 0xFFu) | metaFlags | (blockPure ? 0x8000u : 0u));
    uint16_t basePref = hasTerm ? prefixCycles : (uint16_t)(prefixCycles - 1);
    entry.prefixCycles = (uint16_t)((basePref & 0xFFu) | ((uint16_t)loopCyclesVal << 8));
    entry.code = start;
    return true;
}

#if NOODS_JIT_STATS
// Diagnostics: why the slow path did not run a block. 0 no pcData, 1 warm-up, 2 rejected site,
// 3 untranslatable head, 4 page edge, 5 second instruction not usable, 7 event budget,
// 8/9 rejected site with untranslatable/translatable head.
static uint32_t s_fbReason[2][12];
static uint8_t s_rejWhy[2][ArmPpcJit::BLOCK_TABLE_SIZE]; // reason a site was rejected (3, 4, 5, 6 = compile failed)
static uint32_t s_fbRej[2][8];
#endif
template <bool Track>
FORCE_INLINE int ArmPpcJit::tryExecuteBlock(Interpreter *cpu, uint32_t instrPC, uint32_t opcode, bool thumb,
                                            bool &outIsNative0, bool *outPure, uint32_t remIndep, uint32_t remShared) {
    if (Track && outPure) *outPure = false;
    // The PSR classifiers need the CPU whose stream is being inspected; every block lookup
    // (including this one, which compiles on a miss) goes through this entry.
    s_psrCpu = cpu;
    if (!cpu->pcData) {
        outIsNative0 = thumb ? canTranslateNativeThumb(cpu->arm7, (uint16_t)opcode) : (canTranslateNativeArm(cpu->arm7, opcode) || isCondSafeArm(opcode));
        JIT_STAT(s_fbReason[cpu->arm7 ? 1 : 0][0]++);
        return 0;
    }

    int cpuIdx = cpu->arm7 ? 1 : 0;
#if NOODS_JIT_ENTRY_ONLY
    const bool sequential = instrPC == s_sequentialPC[cpuIdx] && thumb == s_sequentialThumb[cpuIdx];
    s_sequentialPC[cpuIdx] = instrPC + (thumb ? 2u : 4u);
    s_sequentialThumb[cpuIdx] = thumb;
#endif
    size_t bIdx = ((instrPC >> (thumb ? 1 : 2)) ^ (instrPC >> 11)) & (BLOCK_TABLE_SIZE - 1);
    BlockEntry *const bTbl = cpuIdx ? blockTable[1] : arm9Table;
    BlockEntry *bp = &bTbl[bIdx];
    uint32_t opcode1 = cpu->pipeline[0];
    uint32_t curPageEpoch = pageEpoch[cpuIdx][(instrPC >> 12) & (PAGE_EPOCH_SIZE - 1)];
    uint32_t expectedFlags = (currentEpoch << 2) | (thumb ? 3u : 1u);

    bool hit = ((*bp).pc == instrPC &&
                (*bp).opcode0 == opcode &&
                (*bp).opcode1 == opcode1 &&
                (*bp).epochAndFlags == expectedFlags);
    // v1.113: second home for a block whose table slot is taken by another live block.
    if (__builtin_expect(!hit, 0)) {
        BlockEntry *alt = &bTbl[bIdx ^ BLOCK_ALT_XOR];
        if (alt->pc == instrPC && alt->opcode0 == opcode && alt->opcode1 == opcode1 &&
            alt->epochAndFlags == expectedFlags) {
            bp = alt;
            hit = true;
        }
    }

    if (__builtin_expect(hit, 1)) {
        if (__builtin_expect((*bp).pageEpoch != curPageEpoch, 0)) {
            if (BLK_N((*bp).instrCount) <= 2 || computeTailHash(cpu->pcData, BLK_N((*bp).instrCount), thumb) == (*bp).tailHash) {
                (*bp).pageEpoch = curPageEpoch;
            } else {
                hit = false;
                g_evCnt[9]++;
            }
        }
    }

#if NOODS_STEP_BLOCK
    // v1.117: a step block is only for the inline dispatcher; here the pipeline has already
    // been shifted, so the instruction is interpreted as before.
    if (hit && ((*bp).instrCount & 0x8000u)) {
        if (instrPC == noFitNext[cpuIdx]) noFitNext[cpuIdx] = instrPC + (thumb ? 2u : 4u);
        return 0; // v1.121: ARM step blocks too
    }
#endif
    // v1.123: a block that does not fit before the next event is stepped one instruction at
    // a time up to that event. Those addresses used to count as places where code starts:
    // after three such visits each got its own block, a copy of the tail of the block
    // being stepped through, which (being one instruction shorter) rarely fits either. A
    // 20-instruction block grew up to 19 such copies. Now an address reached this way, with
    // the same event still pending, is just interpreted. The address where execution
    // resumes after the event is treated as before.
    if (NOODS_NOFIT_CHAIN && !hit && instrPC == noFitNext[cpuIdx] && !cpu->core->gbaMode &&
        cpu->core->events.frontCycles == noFitEv[cpuIdx]) {
        noFitNext[cpuIdx] = instrPC + (thumb ? 2u : 4u);
        return 0;
    }
    if (__builtin_expect(!hit, 0)) {
#if NOODS_JIT_ENTRY_ONLY
        // A fall-through address being interpreted is not a new basic-block
        // entry. Do not compile overlapping tails simply because a scheduler
        // event or cold-code budget temporarily forced single stepping.
        if (sequential) { ++s_sequentialSuppressed; return 0; }
#endif
        if (s_candPc[candidateContext(cpuIdx)][bIdx] != instrPC) {
            s_candPc[candidateContext(cpuIdx)][bIdx] = instrPC;
            s_candHits[candidateContext(cpuIdx)][bIdx] = 1;
            JIT_STAT(s_fbReason[cpuIdx][1]++);
            return 0;
        }
        uint8_t ch = s_candHits[candidateContext(cpuIdx)][bIdx];
        if (ch == 0xFFu) {
            JIT_STAT(s_fbReason[cpuIdx][2]++);
            JIT_STAT(s_fbRej[cpuIdx][s_rejWhy[cpuIdx][bIdx] & 7]++);
            JIT_STAT(s_fbReason[cpuIdx][8 + ((thumb ? canTranslateNativeThumb(cpu->arm7, (uint16_t)opcode) : (canTranslateNativeArm(cpu->arm7, opcode) || isCondSafeArm(opcode))) ? 1 : 0)]++);
            return 0;
        }
        if (ch < NOODS_JIT_HOT_VISITS) {
            s_candHits[candidateContext(cpuIdx)][bIdx] = ++ch;
            if (ch < NOODS_JIT_HOT_VISITS) {
                JIT_STAT(s_fbReason[cpuIdx][1]++);
                return 0;
            }
        }
        if (!codegenAllowed()) return 0;

        bool firstIsNative = thumb ? canTranslateNativeThumb(cpu->arm7, (uint16_t)opcode) : (canTranslateNativeArm(cpu->arm7, opcode) || isCondSafeArm(opcode));
        outIsNative0 = firstIsNative;
        bool firstIsLoad   = !firstIsNative && (thumb ? isSingleLoadThumb((uint16_t)opcode) : isSingleLoadArm(opcode));
        bool firstIsBranch = !firstIsNative && !firstIsLoad && (thumb ? isBranchHeadThumb(cpu->arm7, (uint16_t)opcode) : isBranchHeadArm(cpu->arm7, opcode));
        bool firstIsPcLoad = !thumb && !firstIsNative && !firstIsLoad && !firstIsBranch && isPcDestLoadTerminatorArm(cpu->arm7, opcode);
        if (!firstIsNative && !firstIsLoad && !firstIsBranch && !firstIsPcLoad) {
#if NOODS_STEP_BLOCK
            // v1.117: an instruction that cannot start a compiled block gets a "step block":
            // one call to its interpreter handler. Only the inline dispatcher enters it (this
            // slow path still interprets the instruction itself, see the 0x8000 test below),
            // which saves the walk runOpcode -> executeThumb -> block lookup miss -> handler
            // every time the instruction comes round again. Same handler, same cycle count.
            if (makeStepBlock(cpu, instrPC, opcode, opcode1, thumb)) return 0;
#endif
            JIT_STAT(s_fbReason[cpuIdx][3]++); JIT_STAT(s_rejWhy[cpuIdx][bIdx] = 3);
            s_candHits[candidateContext(cpuIdx)][bIdx] = 0xFFu;
            return 0;
        }
        const bool firstIsTerm = firstIsBranch || firstIsPcLoad;
        if (firstIsTerm) {
            bool selfLoop = thumb
                ? ((opcode & 0xFFFFu) == 0xE7FEu || ((opcode & 0xF0FFu) == 0xD0FEu && ((opcode >> 8) & 0xFu) < 0xEu))
                : ((opcode & 0x0FFFFFFFu) == 0x0AFFFFFEu);
#ifndef NOODS_BRANCH_HEAD_BLOCKS
#define NOODS_BRANCH_HEAD_BLOCKS 1
#endif
            // v1.103: a lone branch is compiled as a one-instruction block. Since v1.101 a
            // block hit is dispatched inline, which is much cheaper than the interpreter path
            // (runOpcode -> executeArm/Thumb -> block lookup miss -> handler).
            if (!selfLoop && !NOODS_BRANCH_HEAD_BLOCKS) {
                s_candHits[candidateContext(cpuIdx)][bIdx] = 0xFFu;
                return 0;
            }
            (void)selfLoop;
        }

        uint32_t r15After0 = cpu->registersUsr[15];
        // The scan in compileBlock stops a block before the end of the page instruction by
        // instruction, so the head only has to leave room for itself.
        if ((r15After0 & 0xFFFu) < 8u || (r15After0 & 0xFFFu) > (NOODS_PAGE_TAIL ? 0xFF0u : 0xF80u)) {
            // v1p191: too close to the page end for a block, but a one-instruction step block
            // still fits (see makeStepBlock). Without this, the last instructions of every
            // page - where the ARM7 keeps its interrupt glue - were never compiled at all.
            JIT_STAT(s_fbReason[cpuIdx][4]++); JIT_STAT(s_rejWhy[cpuIdx][bIdx] = 4);
            if (!makeStepBlock(cpu, instrPC, opcode, opcode1, thumb))
                s_candHits[candidateContext(cpuIdx)][bIdx] = 0xFFu;
            return 0;
        }

        if (!firstIsTerm && !(NOODS_SINGLE_BLOCK && firstIsNative)) {
            if (thumb) {
                if (!canTranslateNativeThumb(cpu->arm7, (uint16_t)opcode1) && !isTerminatorBranchThumb(cpu->arm7, (uint16_t)opcode1)) {
                    JIT_STAT(s_fbReason[cpuIdx][5]++); JIT_STAT(s_rejWhy[cpuIdx][bIdx] = 5);
                    if (!makeStepBlock(cpu, instrPC, opcode, opcode1, thumb)) // v1.121
                    s_candHits[candidateContext(cpuIdx)][bIdx] = 0xFFu;
                    return 0;
                }
            } else {
                bool op1Usable = ((opcode1 >> 28) == 0xEu && canTranslateNativeArm(cpu->arm7, opcode1)) ||
                                 isCondSafeArm(opcode1) ||
                                 isTerminatorBranchArm(cpu->arm7, opcode1) ||
                                 isPcDestLoadTerminatorArm(cpu->arm7, opcode1);
                if (!op1Usable) {
                    JIT_STAT(s_fbReason[cpuIdx][5]++); JIT_STAT(s_rejWhy[cpuIdx][bIdx] = 5);
                    if (!makeStepBlock(cpu, instrPC, opcode, opcode1, thumb)) // v1.121
                    s_candHits[candidateContext(cpuIdx)][bIdx] = 0xFFu;
                    return 0;
                }
            }
        }

        if (bp->pc != instrPC && (bp->epochAndFlags & 1u) && (bp->epochAndFlags >> 2) == currentEpoch) {
            // The slot holds another block compiled since the last pool wrap: keep it and use
            // the alternate slot if that one is free or stale.
            BlockEntry *alt = &bTbl[bIdx ^ BLOCK_ALT_XOR];
            if (alt->pc == instrPC || !(alt->epochAndFlags & 1u) || (alt->epochAndFlags >> 2) != currentEpoch)
                bp = alt;
            else
                g_evCnt[8]++;
        }
        s_candHits[candidateContext(cpuIdx)][bIdx] = NOODS_JIT_HOT_VISITS;
        if (bp->pc == instrPC && (bp->epochAndFlags >> 2) == currentEpoch) {
            g_evCnt[11]++;
            if (bp->opcode0 != opcode) g_evCnt[10]++;
            else if (bp->opcode1 != opcode1) g_evCnt[10] += 0x10000;
        }
        if (!compileBlock(cpu, instrPC, opcode, thumb, (*bp))) {
            if (!makeStepBlock(cpu, instrPC, opcode, opcode1, thumb)) // v1.121
            s_candHits[candidateContext(cpuIdx)][bIdx] = 0xFFu;
            JIT_STAT(stats.blockCompileFails[cpuIdx]++); JIT_STAT(s_rejWhy[cpuIdx][bIdx] = 6);
            return 0;
        }
    }

#if NOODS_MPU_PURE_JIT
    if (!cpu->arm7 && cpu->core->cp15.mpuRestricted &&
        (!cpu->core->cp15.mpuFastPriv || !cpu->isPrivileged())) {
        const unsigned strictFlags = (*bp).instrCount & 0x0700u;
        const bool soloBranch = strictFlags == 0x0700u && soloBit &&
            (cpu->core->cp15.getPagePerm(instrPC >> 12) & (cpu->isPrivileged() ? 0x01u : 0x08u));
        if ((strictFlags != 0x0500u && !soloBranch) || BLK_LOOP((*bp).prefixCycles) != 0u) {
            MPU_PATH_STAT(++g_mpuPureJitStats[3]);
            return 0;
        }
    }
#endif
    // Verify scheduler cycle budget so block never crosses a scheduled hardware event boundary
    Core *core = cpu->core;
    uint32_t nextEventCycles = core->events[0].cycles;
    uint32_t shift = (cpu->arm7 && !core->gbaMode) ? 1u : 0u;
    uint32_t scaledPrefix = (uint32_t)BLK_PREF((*bp).prefixCycles) << shift;
    uint32_t startCycles = core->gbaMode ? cpu->cycles : core->globalCycles;
    if (__builtin_expect(startCycles + scaledPrefix >= nextEventCycles, 0)) {
        noFitNext[cpuIdx] = instrPC + (thumb ? 2u : 4u); // v1.123
        noFitEv[cpuIdx] = core->events.frontCycles;
        JIT_STAT(s_fbReason[cpuIdx][7]++);
        outIsNative0 = thumb ? canTranslateNativeThumb(cpu->arm7, (uint16_t)opcode) : (canTranslateNativeArm(cpu->arm7, opcode) || isCondSafeArm(opcode));
        return 0;
    }

    uint32_t maxIters = 1u;
    uint8_t loopCyc = (uint8_t)BLK_LOOP((*bp).prefixCycles);
    if (loopCyc != 0u) {
        uint8_t kind = (uint8_t)(((*bp).instrCount >> 10) & 3u);
        bool useIndep = (kind == 1u);
        if (kind == 3u) {
            uint32_t baseAddr = cpu->registersUsr[((*bp).instrCount >> 12) & 7u];
            useIndep = ((baseAddr - 0x04000000u) < 0x140u); // DISPSTAT / VCOUNT / KEYINPUT!
        }
        uint32_t rem = useIndep ? remIndep : remShared;
        if (rem > 1u) {
            maxIters = (rem - 1u) / (uint32_t)loopCyc + 1u;
            if (maxIters > 4096u) maxIters = 4096u;
        }
    }

    JIT_STAT(stats.blockHits[cpuIdx]++);
    JIT_STAT(stats.blockInstrsExecuted[cpuIdx] += BLK_N((*bp).instrCount));
    PROF_MARK(thumb ? 0 : (isSingleLoadArm(opcode) ? 8 : 0));
    if (Track && outPure) {
        int p = thumb ? 0 : armPurity(opcode);
        *outPure = ((*bp).instrCount & 0x8000) && (p == 1 || (p == 2 && armLoadAddrOk(opcode, cpu->registersUsr[(opcode >> 16) & 0xFu])));
    }
#if NOODS_MPU_PURE_JIT && NOODS_MPU_PATH_STATS
    if (!cpu->arm7 && cpu->core->cp15.mpuRestricted &&
        (!cpu->core->cp15.mpuFastPriv || !cpu->isPrivileged())) MPU_PATH_STAT(++g_mpuPureJitStats[2]);
#endif
    return jit_enter((*bp).code, cpu, maxIters);
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
    const bool headMrc = isNativeMrcArm(cpu->arm7, opcode);
    if ((opcode & 0x08000000u) != 0 && !headBranch && !headPushPop && !headMrc) {
        if (!cpu->arm7 && (opcode & 0x0F000010u) == 0x0E000010u) {
            JIT_STAT(stats.cp15Exits[0]++);
        } else {
            JIT_STAT(stats.fallbackInstrs[cpuIdx]++);
        }
        if (cpuIdx == 1) JIT_STAT({ stats.fbOpc7[(opcode >> 20) & 0xFF]++; stats.fbLast7[(opcode >> 20) & 0xFF] = opcode; });
#if NOODS_JIT_HOTPC
        fbSiteRecord(fbHistIdx, fbHistPC, opcode, FBK_FALLBACK);
#endif
        stepCandArm(cpu, opcode); // v1.121
        { if (Track && outPure) *outPure = armPure(opcode, cpu->registersUsr[(opcode >> 16) & 0xFu]); return dispatchArm(cpu, opcode); }
    }
    if ((opcode & 0x0000F000u) == 0x0000F000u && !headBranch && !headPushPop && !headMrc && !isPcDestLoadTerminatorArm(cpu->arm7, opcode)) {
        JIT_STAT(stats.fallbackInstrs[cpuIdx]++);
        JIT_STAT(stats.fbClassArm[cpuIdx][(opcode >> 25) & 7]++);
        if (cpuIdx == 1) JIT_STAT({ stats.fbOpc7[(opcode >> 20) & 0xFF]++; stats.fbLast7[(opcode >> 20) & 0xFF] = opcode; });
#if NOODS_JIT_HOTPC
        fbSiteRecord(fbHistIdx, fbHistPC, opcode, FBK_FALLBACK);
#endif
        stepCandArm(cpu, opcode); // v1.121
        { if (Track && outPure) *outPure = armPure(opcode, cpu->registersUsr[(opcode >> 16) & 0xFu]); return dispatchArm(cpu, opcode); }
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
    { if (Track && outPure) *outPure = armPure(opcode, cpu->registersUsr[(opcode >> 16) & 0xFu]); return dispatchArm(cpu, opcode); }
#else

    size_t idx = ((instrPC >> 2) ^ (instrPC >> 11)) & (STUB_TABLE_SIZE - 1);
    StubEntry &entry = stubTable[cpuIdx][idx];
    uint32_t expectedFlags = (currentEpoch << 2) | 1u;

    if (entry.pc == instrPC && entry.opcode == opcode && entry.epochAndFlags == expectedFlags && entry.code) {
        JIT_STAT(stats.stubHits[cpuIdx]++);
    } else {
        if (!codegenAllowed()) return dispatchArm(cpu, opcode);
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
    return dispatchThumb(cpu, opcode);
#else

    size_t idx = ((instrPC >> 1) ^ (instrPC >> 11) ^ 0x1357u) & (STUB_TABLE_SIZE - 1);
    StubEntry &entry = stubTable[cpuIdx][idx];
    uint32_t expectedFlags = (currentEpoch << 2) | 3u;

    if (entry.pc == instrPC && entry.opcode == opcode && entry.epochAndFlags == expectedFlags && entry.code) {
        JIT_STAT(stats.stubHits[cpuIdx]++);
    } else {
        if (!codegenAllowed()) return dispatchThumb(cpu, opcode);
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
#if NOODS_JIT_RANGE_INVALIDATE
            // All descriptors headed by this page are discarded below. Do not
            // retain a historical code bit that makes every future VRAM rebank
            // invalidate unrelated main-RAM code.
            pageHasBlockBits[cpuIdx][w] &= ~(1u << b);
#endif
            any = true;
        }
    }
    if (any) {
        g_evCnt[4]++;
#if NOODS_JIT_RANGE_INVALIDATE
        // Blocks never span a 4 KiB guest page. Force affected descriptors
        // invalid (not just a tail-hash recheck), and preserve unrelated ones.
        // Both ARM9 scheduler tables must be treated identically; all inline
        // and linked dispatch paths test epochAndFlags before entering code.
        for (int table = cpuIdx; table <= (cpuIdx ? 1 : 2); table += 2) {
            for (size_t i = 0; i < BLOCK_TABLE_SIZE; ++i) {
                BlockEntry &entry = blockTable[table][i];
                const uint32_t page = entry.pc >> 12;
                if ((entry.epochAndFlags & 1u) && page >= pStart && page <= pEnd)
                    entry.epochAndFlags = 0;
            }
        }
#else
        std::memset(blockTable[cpuIdx], 0, sizeof(blockTable[cpuIdx])); if (cpuIdx == 0) std::memset(blockTable[2], 0, sizeof(blockTable[2]));
#endif
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
    std::memset(blockTable[cpuIdx], 0, sizeof(blockTable[cpuIdx])); if (cpuIdx == 0) std::memset(blockTable[2], 0, sizeof(blockTable[2]));
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

#if NOODS_JIT && NOODS_JIT_BLOCKS
uint32_t g_fdWhy[2][FD_N];
uint32_t g_fdMissSub[2][6];
uint32_t g_fdMissNull[2];
#endif
extern uint32_t g_interpCount[2];

void ArmPpcJit::dumpStats(FILE *f) {
#if NOODS_ACWW_SAMPLE
    if (f) for (unsigned s=0;s<4;s++) {
        fprintf(f,"AC_SAMPLE_INFO stream=%u dropped=%u rng=%08X\n",s,s_acDrops[s],g_acSampleRng[s]);
        for (unsigned i=0;i<4096;i++) { const AcwwSite &e=s_acSites[s][i];
            if (e.count) fprintf(f,"AC_SAMPLE stream=%u pc=%08X op=%08X thumb=%u count=%u\n",s,e.pc,e.op,e.thumb,e.count);
        }
        // Make every checkpoint a separate sample window, avoiding table
        // saturation and allowing later scene-only analysis.
        std::memset(s_acSites[s], 0, sizeof(s_acSites[s]));
        s_acDrops[s] = 0;
    }
#endif
#if NOODS_TEST_POOL_INFO
    if (f) fprintf(f, "POOL_INFO ptr=%p used_words=%zu limit_words=%zu capacity_words=%zu fallback_words=%zu\n",
        (void*)codePool, poolWordsUsed, poolLimitWords, s_codeCapacityWords, sizeof(s_mem1CodePool)/sizeof(uint32_t));
#endif
    fprintf(f, "JIT_FORWARD enabled=%u blocks=%u loads=%u\n", (unsigned)NOODS_JIT_FORWARD, s_forwardBlocks, s_forwardLoads);
    if (!f) return;
    // runOpcode calls may execute a JIT block; they are not an instruction count.
    fprintf(f, "JIT_DISPATCH_STATS enabled=%u\n", (unsigned)NOODS_JIT_DISPATCH_STATS);
#if NOODS_JIT_DISPATCH_STATS
    fprintf(f, "JIT_INTERP arm9=%u arm7=%u\n", g_interpCount[0], g_interpCount[1]);
    fprintf(f, "JIT_FD9 enter=%u wrap=%u miss=%u stale=%u step=%u mpu=%u budget=%u\n",
        g_fdWhy[0][FD_ENTER], g_fdWhy[0][FD_WRAP], g_fdWhy[0][FD_MISS], g_fdWhy[0][FD_STALE],
        g_fdWhy[0][FD_STEP], g_fdWhy[0][FD_MPU], g_fdWhy[0][FD_BUDGET]);
    fprintf(f, "JIT_FD7 enter=%u wrap=%u miss=%u stale=%u step=%u mpu=%u budget=%u\n",
        g_fdWhy[1][FD_ENTER], g_fdWhy[1][FD_WRAP], g_fdWhy[1][FD_MISS], g_fdWhy[1][FD_STALE],
        g_fdWhy[1][FD_STEP], g_fdWhy[1][FD_MPU], g_fdWhy[1][FD_BUDGET]);
    fprintf(f, "JIT_MISS9 free=%u pc_collision=%u opcode_mismatch=%u null_pcdata=%u | MISS7 free=%u pc_collision=%u opcode_mismatch=%u null_pcdata=%u\n",
        g_fdMissSub[0][0], g_fdMissSub[0][1], g_fdMissSub[0][2], g_fdMissNull[0],
        g_fdMissSub[1][0], g_fdMissSub[1][1], g_fdMissSub[1][2], g_fdMissNull[1]);
#endif
    fprintf(f, "CODEGEN kib_per_vi=%u hot_visits=%u aligned=%u emitted_bytes=%llu deferred=%u\n",
        (unsigned)NOODS_JIT_CODEGEN_KIB, (unsigned)NOODS_JIT_HOT_VISITS,
        (unsigned)NOODS_JIT_LINE_ALIGN, (unsigned long long)s_codegenTotal,
        s_codegenDeferred);
    fprintf(f, "ENTRY_POLICY enabled=%u sequential_deferred=%u\n", (unsigned)NOODS_JIT_ENTRY_ONLY, s_sequentialSuppressed);
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
            for (int r = 0; r < 60; r++) {
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
    for (int c = 0; c < 2; c++) {
        fprintf(f, "JIT_FBREASON cpu=%d", c ? 7 : 9);
        for (int k = 0; k < 12; k++) fprintf(f, " %u", (unsigned)s_fbReason[c][k]);
        fprintf(f, " rej:");
        for (int k = 0; k < 8; k++) fprintf(f, " %u", (unsigned)s_fbRej[c][k]);
        fprintf(f, "\n");
    }
    for (int c = 0; c < 2; c++) for (int t = 0; t < 2; t++) for (int k = 0; k < 256; k++)
        if (s_fbHist[c][t][k])
            fprintf(f, "JIT_FB cpu=%d %s class=%02X count=%u last=%08X\n", c ? 7 : 9, t ? "thumb" : "arm", k,
                    (unsigned)s_fbHist[c][t][k], (unsigned)s_fbHistLast[c][t][k]);
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
            "smcInv9=%llu smcInv7=%llu guards9=%llu "
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
            (unsigned long long)stats.condGuards[0],
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
