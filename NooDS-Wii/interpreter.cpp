/*
    Copyright (C) 2019-2025 Hydr8gon
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

#include <cstdint>
#include "arm_jit.h"
#include "core.h"
// fastDispatch() decides, *before* any guest state is touched, whether the instruction at
// the head of the pipeline starts a compiled block that the slow path would run right now.
// It applies exactly the slow path's conditions (MPU gate, ARM condition, same-page
// prefetch, block table hit incl. opcode0/opcode1/pool epoch/page epoch, event budget,
// loop iteration budget). If they all hold it advances R15/pcData by one instruction (the
// only part of the pipeline shift that compiled code depends on - every block exit rewrites
// both pipeline slots) and enters the block. Otherwise it returns 0 with nothing modified
// and the caller runs the unchanged slow path, so block selection, cycle counts and
// ARM9/ARM7 interleaving are identical to v1.86 by construction.


#if NOODS_MPU_PURE_JIT
extern uint32_t g_mpuPureJitStats[4];
#endif
#include "core.h"
#if NOODS_ACWW_SAMPLE
extern uint32_t g_acSampleRng[4];
void AcwwSampleRecord(unsigned stream, uint32_t pc, uint32_t opcode, bool thumb);
static inline void acwwMaybeSample(unsigned stream, uint32_t pc, uint32_t opcode, bool thumb) {
    uint32_t v = g_acSampleRng[stream] * 1664525u + 1013904223u;
    g_acSampleRng[stream] = v;
    if ((v >> 20) == 0u) AcwwSampleRecord(stream, pc, opcode, thumb);
}
#endif

// v1p190 diagnostics: why fastDispatch did not enter a compiled block.
// Opt-in via NOODS_JIT_DISPATCH_STATS; printed by dumpStats only when enabled.

extern uint32_t g_fdMissSub[2][6];   // 0 free slot, 1 pc collision, 2 opcode/epoch mismatch
extern uint32_t g_fdMissNull[2];

#if NOODS_JIT && NOODS_JIT_BLOCKS

// v1.117: ceil(2^32 / d) for the loop iteration budget (a loop costs at least 3 cycles, so
// d is never below 3; entries 0 and 1 are unused).
struct LoopRecipTable {
    uint32_t v[256];
    constexpr LoopRecipTable() : v() {
        for (uint32_t d = 2; d < 256; d++) v[d] = (uint32_t)(((1ull << 32) + d - 1) / d);
    }
};
inline constexpr LoopRecipTable g_loopRecip;

template <typename RemFn>
FORCE_INLINE int ArmPpcJit::fastDispatch(Interpreter *cpu, Core *core, const bool arm7, const uint32_t shift, RemFn &&remFn) {
    const uint32_t cpsr = cpu->cpsr;
    const uint32_t r15 = cpu->registersUsr[15];
    uint8_t *const pd = cpu->pcData;
    const uint32_t op0 = cpu->pipeline[0];
#if NOODS_ACWW_SAMPLE
    acwwMaybeSample(arm7 ? 1u : 0u, r15 - ((cpsr & 0x20u) ? 2u : 4u), op0, (cpsr & 0x20u) != 0);
#endif
    uint32_t instrPC, step, wantFlags;
    size_t bIdx;
    if (cpsr & 0x20u) {
        if (__builtin_expect(((r15 + 2u) & 0xFFEu) == 0u, 0)) { DISPATCH_STAT(++g_fdWhy[arm7?1:0][FD_WRAP]); return 0; }
        instrPC = r15 - 2u;
        step = 2u;
        wantFlags = (currentEpoch << 2) | 3u;
        bIdx = ((instrPC >> 1) ^ (instrPC >> 11)) & (BLOCK_TABLE_SIZE - 1);
    } else {
        if (__builtin_expect(((r15 + 4u) & 0xFFCu) == 0u, 0)) { DISPATCH_STAT(++g_fdWhy[arm7?1:0][FD_WRAP]); return 0; }
        const uint8_t cond = Interpreter::condition[((op0 >> 24) & 0xF0u) | (cpsr >> 28)];
        if (__builtin_expect(cond != 1, 0)) {
#if NOODS_SKIP_FALSE
            // v1.121: condition false. The slow path would shift the pipeline, add 4 to R15
            // and charge one cycle (runOpcode); do exactly that here, behind the same MPU
            // gate. Not in GBA mode, which has its own run loop and timing.
            if (cond == 0 && pd != nullptr && !core->gbaMode) {
                if (!arm7 && __builtin_expect(core->cp15.mpuRestricted, 0)) {
                    const bool privileged = (cpsr & 0x1Fu) != 0x10u;
                    const uint32_t page = (r15 - 4u) >> 12;
                    if (privileged && core->cp15.mpuFastPriv) {
                        if (page != core->cp15.execOkPage) {
                            if (!(core->cp15.getPagePerm(page) & 0x04)) return 0;
                            core->cp15.execOkPage = page;
                        }
                    } else {
                        // False condition: no data access or other guest state
                        // is changed. The slow path checks instruction execute
                        // permission for this exact page and privilege level.
                        // Do not reuse the privileged execOkPage for user mode.
                        if (!(core->cp15.getPagePerm(page) & (privileged ? 0x04u : 0x20u)))
                            return 0;
                    }
                }
                cpu->pipeline[0] = cpu->pipeline[1];
                cpu->registersUsr[15] = r15 + 4u;
                cpu->pcData = pd + 4;
                cpu->pipeline[1] = U8TO32(pd + 4, 0);
                return 1;
            }
#endif
            return 0;
        }
        instrPC = r15 - 4u;
        step = 4u;
        wantFlags = (currentEpoch << 2) | 1u;
        bIdx = ((instrPC >> 2) ^ (instrPC >> 11)) & (BLOCK_TABLE_SIZE - 1);
    }
    if (__builtin_expect(pd == nullptr, 0)) return 0;

    const BlockEntry *ep = &(arm7 ? blockTable[1] : arm9Table)[bIdx];
    if (__builtin_expect(ep->pc != instrPC || ep->opcode0 != op0 || ep->opcode1 != cpu->pipeline[1] ||
                         ep->epochAndFlags != wantFlags, 0)) {
        // v1.113: a block whose slot was taken lives in the alternate slot.
        ep += (ptrdiff_t)(bIdx ^ BLOCK_ALT_XOR) - (ptrdiff_t)bIdx;
        if (ep->pc != instrPC || ep->opcode0 != op0 || ep->opcode1 != cpu->pipeline[1] ||
            ep->epochAndFlags != wantFlags) {
#if NOODS_JIT_DISPATCH_STATS
            { const unsigned ci = arm7?1:0;
              ++g_fdWhy[ci][FD_MISS];
              g_fdMissNull[ci] += (pd == nullptr);
              if (!ep || !(ep->epochAndFlags & 1u)) ++g_fdMissSub[ci][0];
              else if (ep->pc != instrPC) ++g_fdMissSub[ci][1];
              else ++g_fdMissSub[ci][2]; }
#endif
#if NOODS_BUDGET_DISPATCH
            return tryBudgetStep(cpu, op0, (cpsr & 0x20u) != 0, true);
#else
            return 0;
#endif
        }
    }
    const BlockEntry &e = *ep;
    if (__builtin_expect(e.pageEpoch != pageEpoch[arm7 ? 1 : 0][(instrPC >> 12) & (PAGE_EPOCH_SIZE - 1)], 0)) {
        DISPATCH_STAT(++g_fdWhy[arm7?1:0][FD_STALE]);
#if NOODS_BUDGET_DISPATCH
        return tryBudgetStep(cpu, op0, (cpsr & 0x20u) != 0);
#else
        return 0;
#endif
    }

    if (!arm7 && __builtin_expect(core->cp15.mpuRestricted, 0)) {
#if NOODS_MPU_PURE_JIT && NOODS_MPU_JIT_PAGE_CACHE
        // v1p190: one inspect-execute page check per (page, privilege) class, remembered in
        // Cp15::execOkPage/execOkPriv (reset by updateMpuState). Authoritative: consults the
        // same generation-stamped 4 KB permission table as Cp15::checkAccessPage, using the
        // execute bit of the current privilege class. This replaces the per-dispatch
        // getPagePerm() lookup plus the stricter pure/solo-branch block filter.
        {
            const bool priv = (cpsr & 0x1Fu) != 0x10u;
            const uint32_t page = instrPC >> 12;
            if (__builtin_expect(page == core->cp15.execOkPage &&
                                 priv == (core->cp15.execOkPriv != 0u), 1)) {
                // already proven executable for this privilege class
            } else {
                const uint8_t pagePerm = core->cp15.getPagePerm(page);
                if (__builtin_expect(!(pagePerm & (priv ? 0x04u : 0x20u)), 0)) {
                    MPU_PATH_STAT(++g_mpuPureJitStats[3]); DISPATCH_STAT(++g_fdWhy[0][FD_MPU]); return 0;
                }
                core->cp15.execOkPage = page;
                core->cp15.execOkPriv = priv ? 1u : 0u;
            }
        }
#elif NOODS_MPU_PURE_JIT
        const bool priv = (cpsr & 0x1Fu) != 0x10u;
        if (!priv || !core->cp15.mpuFastPriv) {
            // No linked blocks and no branches: all instructions stay in the
            // same page and are register-only. Do not reuse execOkPage for a
            // different privilege level.
            const unsigned strictFlags = e.instrCount & 0x0700u;
            const uint8_t pagePerm = core->cp15.getPagePerm(instrPC >> 12);
            const bool soloBranch = strictFlags == 0x0700u && soloBit &&
                                    (pagePerm & (priv ? 0x01u : 0x08u));
            if ((strictFlags != 0x0500u && !soloBranch) || (e.prefixCycles >> 8) != 0u ||
                !(pagePerm & (priv ? 0x04u : 0x20u))) {
                MPU_PATH_STAT(++g_mpuPureJitStats[3]); return 0;
            }
        } else
#endif
        {
            if ((cpsr & 0x1Fu) == 0x10u) return 0;
            const uint32_t page = instrPC >> 12;
            if (__builtin_expect(page != core->cp15.execOkPage, 0)) {
                if (!core->cp15.mpuFastPriv || !(core->cp15.getPagePerm(page) & 0x04))
                    return 0;
                core->cp15.execOkPage = page;
            }
        }
    }

    // Same budget rule as tryExecuteBlock: the whole block must end before the next event.
    const uint32_t pref = e.prefixCycles;
    if (__builtin_expect(core->globalCycles + ((pref & 0xFFu) << shift) >= core->events.frontCycles, 0)) {
#if NOODS_STEP_BLOCK
        // v1.117: the block does not fit before the next event, so the slow path would look
        // the block up again, reject it for the same reason and interpret this one
        // instruction. Do that directly (Thumb on the ARM9 only).
        // v1.118: also for the ARM7 of a DS game (not in GBA mode, which has its own timing).
        if ((cpsr & 0x20u) && (!arm7 || !core->gbaMode)) {
            cpu->registersUsr[15] = r15 + 2u;
            cpu->pcData = pd + 2;
            noFitNext[arm7 ? 1 : 0] = instrPC + 2u; // v1.123
            noFitEv[arm7 ? 1 : 0] = core->events.frontCycles;
            DISPATCH_STAT(++g_fdWhy[arm7?1:0][FD_STEP]);
            return stepThumb(cpu, op0);
        }
#endif
#if NOODS_ARM_NOFIT_STEP
        // The ARM condition, mapped fetch and privileged MPU gate above have
        // already succeeded. The slow path would reject this same block at
        // the same event budget and run exactly one interpreter instruction.
        // Preserve that pipeline transition and no-fit chain bookkeeping here.
        // GBA/DSi keep their original timing paths.
        if (!(cpsr & 0x20u) && !core->gbaMode && !core->dsiMode) {
            cpu->registersUsr[15] = r15 + 4u;
            cpu->pcData = pd + 4;
            noFitNext[arm7 ? 1 : 0] = instrPC + 4u;
            noFitEv[arm7 ? 1 : 0] = core->events.frontCycles;
#if NOODS_JIT_POLL_SKIP
            ++pollEpoch;
#endif
            DISPATCH_STAT(++g_fdWhy[arm7?1:0][FD_STEP]);
            return stepArm(cpu, op0);
        }
#endif
        DISPATCH_STAT(++g_fdWhy[arm7?1:0][FD_BUDGET]);
        return 0;
    }

    uint32_t maxIters = 1u;
    const uint32_t loopCyc = pref >> 8;
    if (__builtin_expect(loopCyc != 0u, 0)) {
        const uint32_t kind = (e.instrCount >> 10) & 3u;
        bool useIndep = (kind == 1u);
        if (kind == 3u) {
            uint32_t baseAddr = cpu->registersUsr[(e.instrCount >> 12) & 7u];
            useIndep = ((baseAddr - 0x04000000u) < 0x140u);
        }
        const uint32_t rem = remFn(useIndep);
        if (rem > 1u) {
            // v1.117: (rem - 1) / loopCyc + 1, capped at 4096, without the divide (this runs
            // for every entry into a loop block). Past the cap no quotient is needed; below
            // it rem - 1 < 4096 * 255, where a 32-bit reciprocal gives the exact quotient.
            const uint32_t n = rem - 1u;
            if (__builtin_expect(loopCyc < 2u, 0)) {
                maxIters = n / loopCyc + 1u;
                if (maxIters > 4096u) maxIters = 4096u;
            } else if (n >= loopCyc * 4095u)
                maxIters = 4096u;
            else
                maxIters = (uint32_t)(((uint64_t)n * g_loopRecip.v[loopCyc]) >> 32) + 1u;
        }
    }

    cpu->registersUsr[15] = r15 + step;
    cpu->pcData = pd + step;
    DISPATCH_STAT(++g_fdWhy[arm7?1:0][FD_ENTER]);
#if NOODS_MPU_PURE_JIT && NOODS_MPU_PATH_STATS
    if (!arm7 && core->cp15.mpuRestricted &&
        (!core->cp15.mpuFastPriv || ((cpsr & 0x1Fu) == 0x10u))) MPU_PATH_STAT(++g_mpuPureJitStats[1]);
#endif
    return jit_enter(e.code, cpu, maxIters);
}

#endif


#include <cstdint>

// Side-effect-free ARM data-processing operations. Conditions are checked by
// the caller. PC writes, register-controlled shifts and miscellaneous encodings
// stay in the original interpreter. Every accepted instruction costs one fetch
// cycle and no data/internal/branch cycles on ARM7TDMI in IWRAM.
namespace GbaArmAlu {
inline bool eligible(uint32_t op) {
    if (op&0x0C000000u) return false;
    if (!(op&0x02000000u) && (op&0x10u)) return false;
    const unsigned kind=(op>>21)&15;
    if (kind>=8 && kind<=11 && !(op&0x00100000u)) return false;
    return ((op>>12)&15)!=15;
}
inline uint32_t ror(uint32_t v,unsigned s) {
    return s ? (v>>s)|(v<<(32-s)) : v;
}
inline void execute(uint32_t **regs,uint32_t &cpsr,uint32_t op) {
    const unsigned kind=(op>>21)&15;
    const bool flags=op&0x00100000u;
    const bool arithmetic=(kind>=2 && kind<=7) || kind==10 || kind==11;
    const uint32_t oldCarry=(cpsr>>29)&1;
    uint32_t carry=oldCarry,b;
    if (op&0x02000000u) {
        const unsigned shift=(op>>7)&30;
        b=ror(op&255u,shift);
        if (shift) carry=b>>31;
    } else {
        const uint32_t v=*regs[op&15];
        const unsigned shift=(op>>7)&31;
        switch((op>>5)&3) {
        case 0: b=v<<shift;if(shift)carry=(v>>(32-shift))&1;break;
        case 1: b=shift?v>>shift:0;carry=(v>>(shift?shift-1:31))&1;break;
        case 2: b=uint32_t(int32_t(v)>>(shift?shift:31));carry=(v>>(shift?shift-1:31))&1;break;
        default:
            b=shift?ror(v,shift):(oldCarry<<31)|(v>>1);
            carry=(v>>(shift?shift-1:0))&1;break;
        }
    }
    const uint32_t a=*regs[(op>>16)&15];
    uint32_t result,overflow=0;
    if (!flags) {
        switch(kind) {
        case 0: result=a&b;break;case 1:result=a^b;break;
        case 2:result=a-b;break;case 3:result=b-a;break;
        case 4:result=a+b;break;case 5:result=a+b+oldCarry;break;
        case 6:result=a-b-1+oldCarry;break;case 7:result=b-a-1+oldCarry;break;
        case 12:result=a|b;break;case 13:result=b;break;
        case 14:result=a&~b;break;default:result=~b;break;
        }
        *regs[(op>>12)&15]=result;return;
    }
    switch(kind) {
    case 0: case 8: result=a&b;break;
    case 1: case 9: result=a^b;break;
    case 2: case 10:
        result=a-b;carry=a>=b;overflow=((a^b)&(a^result))>>31;break;
    case 3:
        result=b-a;carry=b>=a;overflow=((a^b)&(b^result))>>31;break;
    case 4: case 11:
        result=a+b;carry=result<a;overflow=(~(a^b)&(a^result))>>31;break;
    case 5:
        result=a+b+oldCarry;carry=(result<a)||(b==0xFFFFFFFFu&&oldCarry);
        overflow=(~(a^b)&(a^result))>>31;break;
    case 6:
        result=a-b-1+oldCarry;carry=(a>=b)&&((a!=b)||oldCarry);
        overflow=((a^b)&(a^result))>>31;break;
    case 7:
        result=b-a-1+oldCarry;carry=(b>=a)&&((b!=a)||oldCarry);
        overflow=((a^b)&(b^result))>>31;break;
    case 12:result=a|b;break;
    case 13:result=b;break;
    case 14:result=a&~b;break;
    default:result=~b;break;
    }
    if (kind<8 || kind>11) *regs[(op>>12)&15]=result;
    if (flags) {
        const uint32_t mask=arithmetic?0xF0000000u:0xE0000000u;
        cpsr=(cpsr&~mask)|(result&0x80000000u)|(uint32_t(result==0)<<30)|(carry<<29);
        if(arithmetic)cpsr|=overflow<<28;
    }
}
}
#if NOODS_GBA_DEDICATED || NOODS_GBA_ROM_ISLAND
namespace ArmAluSpecialized {
using GbaArmAlu::ror;
template <unsigned Kind,bool Flags,int Shape=-1,typename Registers> __attribute__((always_inline)) inline void executeOp(Registers &regs,uint32_t &cpsr,uint32_t op) {
    constexpr unsigned kind=Kind;
    constexpr bool flags=Flags;
    constexpr bool arithmetic=(kind>=2 && kind<=7) || kind==10 || kind==11;
    const uint32_t oldCarry=(cpsr>>29)&1;
    uint32_t carry=oldCarry,b;
    if (Shape==4 || (Shape<0 && (op&0x02000000u))) {
        const unsigned shift=(op>>7)&30;
        b=ror(op&255u,shift);
        if (shift) carry=b>>31;
    } else {
        const uint32_t v=*regs[op&15];
        const unsigned shift=(op>>7)&31;
        switch(Shape<0?((op>>5)&3):unsigned(Shape)) {
        case 0: b=v<<shift;if(shift)carry=(v>>(32-shift))&1;break;
        case 1: b=shift?v>>shift:0;carry=(v>>(shift?shift-1:31))&1;break;
        case 2: b=uint32_t(int32_t(v)>>(shift?shift:31));carry=(v>>(shift?shift-1:31))&1;break;
        default:
            b=shift?ror(v,shift):(oldCarry<<31)|(v>>1);
            carry=(v>>(shift?shift-1:0))&1;break;
        }
    }
    const uint32_t a=*regs[(op>>16)&15];
    uint32_t result,overflow=0;
    if (!flags) {
        switch(kind) {
        case 0: result=a&b;break;case 1:result=a^b;break;
        case 2:result=a-b;break;case 3:result=b-a;break;
        case 4:result=a+b;break;case 5:result=a+b+oldCarry;break;
        case 6:result=a-b-1+oldCarry;break;case 7:result=b-a-1+oldCarry;break;
        case 12:result=a|b;break;case 13:result=b;break;
        case 14:result=a&~b;break;default:result=~b;break;
        }
        *regs[(op>>12)&15]=result;return;
    }
    switch(kind) {
    case 0: case 8: result=a&b;break;
    case 1: case 9: result=a^b;break;
    case 2: case 10:
        result=a-b;carry=a>=b;overflow=((a^b)&(a^result))>>31;break;
    case 3:
        result=b-a;carry=b>=a;overflow=((a^b)&(b^result))>>31;break;
    case 4: case 11:
        result=a+b;carry=result<a;overflow=(~(a^b)&(a^result))>>31;break;
    case 5:
        result=a+b+oldCarry;carry=(result<a)||(b==0xFFFFFFFFu&&oldCarry);
        overflow=(~(a^b)&(a^result))>>31;break;
    case 6:
        result=a-b-1+oldCarry;carry=(a>=b)&&((a!=b)||oldCarry);
        overflow=((a^b)&(a^result))>>31;break;
    case 7:
        result=b-a-1+oldCarry;carry=(b>=a)&&((b!=a)||oldCarry);
        overflow=((a^b)&(b^result))>>31;break;
    case 12:result=a|b;break;
    case 13:result=b;break;
    case 14:result=a&~b;break;
    default:result=~b;break;
    }
    if (kind<8 || kind>11) *regs[(op>>12)&15]=result;
    if (flags) {
        const uint32_t mask=arithmetic?0xF0000000u:0xE0000000u;
        cpsr=(cpsr&~mask)|(result&0x80000000u)|(uint32_t(result==0)<<30)|(carry<<29);
        if(arithmetic)cpsr|=overflow<<28;
    }
}
template<class Registers> __attribute__((always_inline)) inline void execute(Registers &regs,uint32_t &cpsr,uint32_t op){
 switch((op>>20)&31){
 case 0:executeOp<0,false>(regs,cpsr,op);break;
 case 1:executeOp<0,true>(regs,cpsr,op);break;
 case 2:executeOp<1,false>(regs,cpsr,op);break;
 case 3:executeOp<1,true>(regs,cpsr,op);break;
 case 4:executeOp<2,false>(regs,cpsr,op);break;
 case 5:executeOp<2,true>(regs,cpsr,op);break;
 case 6:executeOp<3,false>(regs,cpsr,op);break;
 case 7:executeOp<3,true>(regs,cpsr,op);break;
 case 8:executeOp<4,false>(regs,cpsr,op);break;
 case 9:executeOp<4,true>(regs,cpsr,op);break;
 case 10:executeOp<5,false>(regs,cpsr,op);break;
 case 11:executeOp<5,true>(regs,cpsr,op);break;
 case 12:executeOp<6,false>(regs,cpsr,op);break;
 case 13:executeOp<6,true>(regs,cpsr,op);break;
 case 14:executeOp<7,false>(regs,cpsr,op);break;
 case 15:executeOp<7,true>(regs,cpsr,op);break;
 case 17:executeOp<8,true>(regs,cpsr,op);break;
 case 19:executeOp<9,true>(regs,cpsr,op);break;
 case 21:executeOp<10,true>(regs,cpsr,op);break;
 case 23:executeOp<11,true>(regs,cpsr,op);break;
 case 24:executeOp<12,false>(regs,cpsr,op);break;
 case 25:executeOp<12,true>(regs,cpsr,op);break;
 case 26:executeOp<13,false>(regs,cpsr,op);break;
 case 27:executeOp<13,true>(regs,cpsr,op);break;
 case 28:executeOp<14,false>(regs,cpsr,op);break;
 case 29:executeOp<14,true>(regs,cpsr,op);break;
 case 30:executeOp<15,false>(regs,cpsr,op);break;
 case 31:executeOp<15,true>(regs,cpsr,op);break;
 default:__builtin_unreachable();
 }
}
}
#endif

#include "core.h"
#include "arm_jit.h"


#include <tuxedo/thread.h>

#ifndef NOODS_STRICT_SINGLE_PRECHECK
#define NOODS_STRICT_SINGLE_PRECHECK 1
#endif
#ifndef NOODS_STRICT_BRANCH_PRECHECK
#define NOODS_STRICT_BRANCH_PRECHECK 1
#endif

#if NOODS_NDS_TASK_PROFILE
// Profile every scheduler mode, not just the dual-CPU NDS loop. The old
// NDS_CPU counter deliberately remains dual-only for historical comparisons.
static uint64_t singleCpuTicks[3]; // NDS ARM9 only, NDS ARM7 only, GBA
static uint64_t singleTaskTicks[3][MAX_TASKS];
static uint32_t singleTaskCalls[3][MAX_TASKS];
#endif

// v1p190: number of guest instructions that reached the interpreter (per CPU).
uint32_t g_interpCount[2];

Interpreter::Interpreter(Core *core, bool arm7): core(core), arm7(arm7) {
    // Initialize the registers for user mode
    for (int i = 0; i < 32; i++)
        registers[i] = &registersUsr[i & 0xF];
}

void Interpreter::saveState(FILE *file) {
    // Write state data to the file
    fwrite(pipeline, 4, sizeof(pipeline) / 4, file);
    fwrite(registersUsr, 4, sizeof(registersUsr) / 4, file);
    fwrite(registersFiq, 4, sizeof(registersFiq) / 4, file);
    fwrite(registersSvc, 4, sizeof(registersSvc) / 4, file);
    fwrite(registersAbt, 4, sizeof(registersAbt) / 4, file);
    fwrite(registersIrq, 4, sizeof(registersIrq) / 4, file);
    fwrite(registersUnd, 4, sizeof(registersUnd) / 4, file);
    fwrite(&cpsr, sizeof(cpsr), 1, file);
    fwrite(&spsrFiq, sizeof(spsrFiq), 1, file);
    fwrite(&spsrSvc, sizeof(spsrSvc), 1, file);
    fwrite(&spsrAbt, sizeof(spsrAbt), 1, file);
    fwrite(&spsrIrq, sizeof(spsrIrq), 1, file);
    fwrite(&spsrUnd, sizeof(spsrUnd), 1, file);
    fwrite(&cycles, sizeof(cycles), 1, file);
    fwrite(&halted, sizeof(halted), 1, file);
    fwrite(&dsiCycle, sizeof(dsiCycle), 1, file);
    fwrite(&ime, sizeof(ime), 1, file);
    fwrite(&ie, sizeof(ie), 1, file);
    fwrite(&irf, sizeof(irf), 1, file);
    fwrite(&postFlg, sizeof(postFlg), 1, file);
}

void Interpreter::loadState(FILE *file) {
    // Read state data from the file
    fread(pipeline, 4, sizeof(pipeline) / 4, file);
    fread(registersUsr, 4, sizeof(registersUsr) / 4, file);
    fread(registersFiq, 4, sizeof(registersFiq) / 4, file);
    fread(registersSvc, 4, sizeof(registersSvc) / 4, file);
    fread(registersAbt, 4, sizeof(registersAbt) / 4, file);
    fread(registersIrq, 4, sizeof(registersIrq) / 4, file);
    fread(registersUnd, 4, sizeof(registersUnd) / 4, file);
    fread(&cpsr, sizeof(cpsr), 1, file);
    fread(&spsrFiq, sizeof(spsrFiq), 1, file);
    fread(&spsrSvc, sizeof(spsrSvc), 1, file);
    fread(&spsrAbt, sizeof(spsrAbt), 1, file);
    fread(&spsrIrq, sizeof(spsrIrq), 1, file);
    fread(&spsrUnd, sizeof(spsrUnd), 1, file);
    fread(&cycles, sizeof(cycles), 1, file);
    fread(&halted, sizeof(halted), 1, file);
    fread(&dsiCycle, sizeof(dsiCycle), 1, file);
    fread(&ime, sizeof(ime), 1, file);
    fread(&ie, sizeof(ie), 1, file);
    fread(&irf, sizeof(irf), 1, file);
    fread(&postFlg, sizeof(postFlg), 1, file);

    // Update mapped registers
    swapRegisters(cpsr);
    pcData = nullptr;
#if NOODS_JIT
    ArmPpcJit::invalidateCpu(arm7);
#endif
}

void Interpreter::init() {
    // Prepare to boot the BIOS
    spsrFiq = spsrSvc = spsrAbt = spsrIrq = spsrUnd = 0x10;
    setCpsr(0x000000D3); // Supervisor, interrupts off
    registersUsr[15] = arm7 ? 0x00000000 : 0xFFFF0000;
    flushPipeline();
#if NOODS_JIT
    ArmPpcJit::invalidateCpu(arm7);
#endif

    // Reset the I/O registers
    ime = 0;
    ie = irf = 0;
    postFlg = 0;
}

void Interpreter::directBoot() {
    // Prepare to directly boot a ROM
    spsrFiq = spsrSvc = spsrAbt = spsrIrq = spsrUnd = 0x10;
    setCpsr(0x000000DF); // System, interrupts off
    registersUsr[15] = core->gbaMode ? 0x8000000 : entryAddr;
    registersUsr[14] = registersUsr[15];
    registersUsr[12] = registersUsr[15];
    registersUsr[13] = arm7 ? (core->gbaMode ? 0x3007F00 : 0x380FD80) : 0x3002F7C;
    registersIrq[0] = arm7 ? (core->gbaMode ? 0x3007FA0 : 0x380FF80) : 0x3003F80;
    registersSvc[0] = arm7 ? (core->gbaMode ? 0x3007FE0 : 0x380FFC0) : 0x3003FC0;
    flushPipeline();
#if NOODS_JIT
    ArmPpcJit::invalidateCpu(arm7);
#endif
}

void Interpreter::resetCycles() {
    // Preserve 0xFFFFFFFF sentinel when halted; otherwise adjust for global cycle reset
    if (cycles != 0xFFFFFFFFu)
        cycles -= std::min(core->globalCycles, cycles);
}

void Interpreter::runCoreNone(Core &core) {
    // Run the core with no active CPUs
    while (true) {
        // v1.116: the flag is only read here in the common case; interrupts are masked only
        // when a stop request is actually taken (this runs once per scheduler event).
        if (__builtin_expect(!core.running, 0)) {
            PPCIrqState st = PPCIrqLockByMsr();
            core.running = true;
            PPCIrqUnlockByMsr(st);
            break;
        }

        // Jump to the next task and run all that are scheduled at this timestamp
        uint32_t target = core.events[0].cycles;
        while (core.events[0].cycles <= target) {
            SchedEvent ev = core.events[0];
            core.events.popFront();
            core.globalCycles = ev.cycles;
#if NOODS_JIT && NOODS_JIT_POLL_SKIP
            ArmPpcJit::pollEpoch++;
#endif
            core.tasks[ev.task]();
            if (ev.task == RESET_CYCLES)
                target = 0;
        }
    }
}

#if NOODS_GBA_ARM_BURST
#if NOODS_NDS_TASK_PROFILE
static uint32_t gbaBurstCounts[4]; // successful runs, instructions, ALU, mapped transfers
#endif
#if NOODS_GBA_ARM_BURST >= 3
__attribute__((always_inline)) inline
#endif
bool Interpreter::runGbaArmBurst() {
    Core &c=*core;GbaBus &bus=c.gbaBus;
    uint32_t r15=*registers[15];
    if ((cpsr&0x20u) || (r15>>24)!=3 || (r15&3) || !pcData || bus.active ||
        c.globalCycles!=cycles || c.events.frontCycles<=cycles+1) return false;
    unsigned slots=(0xFFCu-(r15&0xFFFu))/4;
    if (!slots) return false;
    uint32_t clock=cycles,op=pipeline[0],next=pipeline[1];
    uint8_t *pc=pcData;unsigned done=0,last=0;
    do {
        const unsigned cond=condition[((op>>24)&0xF0u)|(cpsr>>28)];
        if (cond==2) break;
        const bool alu=GbaArmAlu::eligible(op);
        uint32_t address=0,dataCost=0;bool load=false,byte=false;
        if (cond && !alu) {
#if NOODS_GBA_ARM_BURST >= 2
            // Pre-indexed, no-writeback, ordinary word/byte transfers only.
            // All I/O, BIOS/open-bus, GamePak and special encodings fall back.
            if ((op&0x0D200000u)!=0x05000000u || ((op>>12)&15)==15 ||
                ((op&0x02000000u) && (op&0x10u))) break;
            auto reg=[&](unsigned n){return n==15?r15+4:*registers[n];};
            uint32_t offset=op&0xFFFu;
            if(op&0x02000000u) {
                const uint32_t v=reg(op&15);const unsigned n=(op>>7)&31;
                switch((op>>5)&3) {
                case 0:offset=v<<n;break;case 1:offset=n?v>>n:0;break;
                case 2:offset=uint32_t(int32_t(v)>>(n?n:31));break;
                default:offset=n?GbaArmAlu::ror(v,n):((cpsr&0x20000000u)<<2)|(v>>1);break;
                }
            }
            address=reg((op>>16)&15);address=(op&0x00800000u)?address+offset:address-offset;
            load=op&0x00100000u;byte=op&0x00400000u;
            const unsigned region=address>>24;
            if(region!=2 && region!=3 && region!=6) break;
            if(!(load?c.memory.readMap7:c.memory.writeMap7)[address>>12]) break;
            dataCost=region==2?(byte?3:6):region==6?(byte?1:2):1;
#else
            break;
#endif
        }
        const unsigned cost=1+dataCost+(load?1:0);
        if(c.events.frontCycles-clock<=cost)break;
        const uint32_t executing=op;
        r15+=4;*registers[15]=r15;op=next;pc+=4;next=U8TO32(pc,0);
        last=0;
        if(cond) {
            if(alu) {
                GbaArmAlu::execute(registers,cpsr,executing);
#if NOODS_NDS_TASK_PROFILE
                ++gbaBurstCounts[2];
#endif
            } else {
                last=load?2:1;
                if(load) {
                    const uint8_t *data=c.memory.readMap7[address>>12];
                    uint32_t value=byte?data[address&0xFFFu]:U8TO32(data,address&0xFFCu);
                    if(!byte && (address&3))value=GbaArmAlu::ror(value,(address&3)*8);
                    *registers[(executing>>12)&15]=value;
                } else {
                    // Preserve ordinary write-side SMC invalidation and GBA byte-VRAM rules.
                    cycles=clock+1+dataCost;c.globalCycles=cycles;
                    bus.beginInstruction();bus.nextSeq=false;bus.inSeqAccess=bus.hadDataAccess=true;
                    bus.lastFetchUsedDuty=false;
                    const uint32_t value=*registers[(executing>>12)&15];
                    if(byte)c.memory.write<uint8_t>(true,address,value);
                    else c.memory.write<uint32_t>(true,address,value);
                }
#if NOODS_NDS_TASK_PROFILE
                ++gbaBurstCounts[3];
#endif
            }
        }
        clock+=cost;++done;
    } while(done<slots);
    if (!done) return false;
    pipeline[0]=op;pipeline[1]=next;pcData=pc;cycles=clock;c.globalCycles=clock;
    bus.lastFetchUsedDuty=false;bus.beginInstruction();bus.nextSeq=last==0;
    if(last){bus.inSeqAccess=bus.hadDataAccess=true;bus.hadLoad=last==2;}
    c.memory.clearGbaDmaOpenBus();
#if NOODS_NDS_TASK_PROFILE
    ++gbaBurstCounts[0];gbaBurstCounts[1]+=done;
#endif
    return true;
}
#endif
#if NOODS_GBA_DEDICATED
// Dedicated exact ARM7/IWRAM execution island (iteration 18).
// Compile-time ALU specialization; branches remain inside the execution island.
// Exact event/page guards and original memory-write fallback remain mandatory.
// No CPU mode, interrupt, ROM/prefetch or unsupported instruction crosses the island.
__attribute__((always_inline)) inline bool Interpreter::runGbaDedicated() {
    Core &c=*core;GbaBus &bus=c.gbaBus;
    uint32_t r15=*registers[15];
    if ((cpsr&0x20u) || (r15>>24)!=3 || (r15&3) || !pcData || bus.active ||
        c.globalCycles!=cycles || c.events.frontCycles<=cycles+1) return false;
    auto regs=registers;
    uint32_t clock=cycles,op=pipeline[0],next=pipeline[1];
    uint8_t *pc=pcData;unsigned done=0,last=0;bool branchSeen=false;
    while((r15&0xFFFu)!=0xFFCu) {
        const unsigned cond=condition[((op>>24)&0xF0u)|(cpsr>>28)];
        if (cond==2) break;
        unsigned type=0,cost=1;
        uint32_t address=0,target=0,dataCost=0;bool load=false,byte=false,link=false;
        if(cond) {
            if(GbaArmAlu::eligible(op))type=1;
            else if((op&0x0E000000u)==0x0A000000u || (op&0x0FFFFFF0u)==0x012FFF10u) {
                if((op&0x0E000000u)==0x0A000000u) {
                    target=r15+4+(uint32_t(int32_t(op<<8)>>6));link=op&0x01000000u;
                } else {
                    target=(op&15)==15?r15+4:*regs[op&15];
                    if(target&1)break;
                    target&=~3u;
                }
                if((target>>24)!=3 || (target&0xFFFu)>0xFF8u || !c.memory.readMap7[target>>12])break;
                cost=3;type=3;
            } else {
                if ((op&0x0D200000u)!=0x05000000u || ((op>>12)&15)==15 ||
                    ((op&0x02000000u) && (op&0x10u))) break;
                auto reg=[&](unsigned n){return n==15?r15+4:*regs[n];};
                uint32_t offset=op&0xFFFu;
                if(op&0x02000000u) {
                    const uint32_t v=reg(op&15);const unsigned n=(op>>7)&31;
                    switch((op>>5)&3) {
                    case 0:offset=v<<n;break;case 1:offset=n?v>>n:0;break;
                    case 2:offset=uint32_t(int32_t(v)>>(n?n:31));break;
                    default:offset=n?GbaArmAlu::ror(v,n):((cpsr&0x20000000u)<<2)|(v>>1);break;
                    }
                }
                address=reg((op>>16)&15);address=(op&0x00800000u)?address+offset:address-offset;
                load=op&0x00100000u;byte=op&0x00400000u;
                const unsigned region=address>>24;
                if(region!=2 && region!=3 && region!=6) break;
                if(!(load?c.memory.readMap7:c.memory.writeMap7)[address>>12]) break;
                dataCost=region==2?(byte?3:6):region==6?(byte?1:2):1;
                cost=1+dataCost+(load?1:0);type=2;
            }
        }
        if(c.events.frontCycles-clock<=cost)break;
        const uint32_t executing=op;
        r15+=4;*regs[15]=r15;op=next;pc+=4;next=U8TO32(pc,0);
        last=0;
        if(type==1) {
            ArmAluSpecialized::execute(regs,cpsr,executing);
        }
        else if(type==2) {
            last=load?2:1;
            if(load) {
                const uint8_t *data=c.memory.readMap7[address>>12];
                uint32_t value=byte?data[address&0xFFFu]:U8TO32(data,address&0xFFCu);
                if(!byte && (address&3))value=GbaArmAlu::ror(value,(address&3)*8);
                *regs[(executing>>12)&15]=value;
            } else {
                cycles=clock+1+dataCost;c.globalCycles=cycles;
                bus.beginInstruction();bus.nextSeq=false;bus.inSeqAccess=bus.hadDataAccess=true;
                bus.lastFetchUsedDuty=false;
                const uint32_t value=*regs[(executing>>12)&15];
                if(byte)c.memory.write<uint8_t>(true,address,value);
                else c.memory.write<uint32_t>(true,address,value);
            }
        } else if(type==3) {
            branchSeen=true;
            if(link)*regs[14]=r15-4;
            r15=target+4;*regs[15]=r15;
            pc=c.memory.readMap7[target>>12]+(target&0xFFFu);
            op=U8TO32(pc,0);pc+=4;next=U8TO32(pc,0);last=3;
        }
        clock+=cost;++done;
    }
    if (!done) return false;
    pipeline[0]=op;pipeline[1]=next;pcData=pc;cycles=clock;c.globalCycles=clock;
    bus.lastFetchUsedDuty=false;bus.beginInstruction();bus.nextSeq=last==0 || last==3;
    bus.active=false;if(branchSeen)bus.count=bus.duty=0;
    if(last==3){bus.count=bus.duty=0;bus.branched=true;}
    else if(last==1 || last==2){bus.inSeqAccess=bus.hadDataAccess=true;bus.hadLoad=last==2;}
    c.memory.clearGbaDmaOpenBus();
    return true;
}

#endif
#if NOODS_GBA_ROM_ISLAND
#include "arm7tdmi_mul.h"
// v1p170: Dedicated exact ARM7/GamePak-ROM execution island.
//
// Runs sequential-ish ARM code fetched from the cartridge ROM (regions 0x08-0x0D)
// in a tight loop while charging cycles through the *real* GbaBus model
// (fetchOpcode/branchRefill/dataAccess/stepInternal), so timing is identical to
// runGbaOpcode instruction-for-instruction. Unlike runGbaDedicated, no costs are
// hard-coded: the prefetch buffer state machine is exercised for every fetch,
// which keeps DMA-prefetch and H-blank observable behaviour unchanged.
//
// Supported encodings (everything else falls back to the interpreter):
//  * any false-condition instruction (fetch only, no side effects)
//  * data-processing via GbaArmAlu::eligible / ArmAluSpecialized (no PC dest)
//  * B / BL with a mapped target (pipeline reads via readMap7)
//  * MUL / MLA without the S bit (Rd != PC), exact signedMulTicks charging
//  * LDR / LDRB / STR / STRB, P=1 W=0 (offset forms, no writeback),
//    Rd != PC, register offset only with a zero shift field (bits 11-4),
//    loads from mapped {EWRAM,IWRAM,pal,VRAM,OAM,ROM}, stores to
//    {EWRAM,IWRAM,pal,VRAM,OAM,ROM} through the full Memory::write path.
// The loop breaks whenever an event comes within 96 cycles (worst-case handled
// instruction cost is 45), so mid-instruction DMA preemption stays exclusively
// in runGbaOpcode. IO, halfword/signed transfers, LDM/STM, SWI, BX, Thumb and
// unmapped accesses never cross the island.
#if NOODS_GBA_ISLAND_STATS
uint32_t g_islEntries=0,g_islFalseEntry=0,g_islInstrs=0;
uint32_t g_islBrk[8]={0}; // 0 cond2,1 page,2 region,3 margin,4 unhandled,5 unmapped,6 gpio,7 (spare)
uint32_t g_islCat[6]={0}; // alu,false,mul,branch,xfer,block
uint32_t g_islWhy[6]={0}; // thumb,notrom,unaligned,nopc,desync,margin
#endif
__attribute__((noinline)) bool Interpreter::runGbaRomIsland() {
    Core &c = *core;
    GbaBus &bus = c.gbaBus;
    uint32_t r15 = *registers[15];
    const uint32_t region0 = r15 >> 24;
    if ((cpsr & 0x20u) || region0 < 8u || region0 > 0xDu || (r15 & 3) || !pcData ||
        c.globalCycles != cycles || (int32_t)(c.events.frontCycles - cycles) <= 20) {
#if NOODS_GBA_ISLAND_STATS
        ++g_islFalseEntry;
        if (cpsr & 0x20u) ++g_islWhy[0];            // thumb
        else if (region0 < 8u || region0 > 0xDu) ++g_islWhy[1]; // not ROM
        else if (r15 & 3) ++g_islWhy[2];           // unaligned
        else if (!pcData) ++g_islWhy[3];           // no pc mirror
        else if (c.globalCycles != cycles) ++g_islWhy[4]; // cycle desync
        else ++g_islWhy[5];                        // event margin
#endif
        return false;
    }

    auto regs = registers;
    uint8_t **readMap = c.memory.readMap7;
    if (!readMap[r15 >> 12]) {
#if NOODS_GBA_ISLAND_STATS
        ++g_islFalseEntry;
#endif
        return false;
    }

    uint32_t clock = cycles;
    uint32_t op = pipeline[0], next = pipeline[1];
    uint8_t *pc = pcData;
    unsigned done = 0;

    while ((r15 & 0xFFFu) != 0xFFCu) {
        // Stay on GamePak ROM as the execution source; the interpreter continues
        // identically from the island's exit state (pcData preserved) if not.
        const uint32_t pcRegion = (r15 - 4u) >> 24;
        if (pcRegion < 8u || pcRegion > 0xDu) {
#if NOODS_GBA_ISLAND_STATS
            ++g_islBrk[2];
#endif
            break;
        }
        const uint16_t waitCnt = c.memory.getWaitCnt();
        const unsigned cond = condition[((op >> 24) & 0xF0u) | (cpsr >> 28)];
        if (cond == 2) {
#if NOODS_GBA_ISLAND_STATS
            ++g_islBrk[0];
#endif
            break;
        }

        // ---- classify (cond true only) ----
        bool alu = false, mulOp = false, branch = false, xfer = false, block = false;
        bool link = false, load = false, byte = false, mla = false;
        uint32_t target = 0, address = 0;
        unsigned blkM = 0;
        uint32_t blkList = 0, blkLow = 0;
        if (cond) {
            if (GbaArmAlu::eligible(op)) {
                alu = true;
            }
#if !defined(ROMISL_NO_BRANCH)
            else if ((op & 0x0E000000u) == 0x0A000000u) {
                // B / BL: at handler time r15 is PC+8; here it is still PC+4.
                target = (r15 + 4u + (uint32_t)((int32_t)(op << 8) >> 6)) & ~3u;
                if (!readMap[target >> 12] || !readMap[(target + 4u) >> 12])
                    break;
                link = (op & 0x01000000u) != 0;
                branch = true;
            }
#endif
#if !defined(ROMISL_NO_MUL)
            else if ((op & 0x0FC000F0u) == 0x00000090u && !(op & 0x00100000u) &&
                     ((op >> 16) & 0xF) != 15) {
                // MUL / MLA without flags; Rd is bits 16-19 on ARM7TDMI.
                // Rs/Rm values (and the tick count) are read after the pipeline
                // advance so a PC operand sees PC+8 like the interpreter does.
                mla = (op & 0x00200000u) != 0;
                mulOp = true;
            }
#endif
#if !defined(ROMISL_NO_XFER)
            else if ((op & 0x0D200000u) == 0x05000000u && ((op >> 12) & 0xF) != 15) {
                // LDR(B)/STR(B), P=1 W=0, immediate or plain register offset
                uint32_t offset;
                if (op & 0x02000000u) {
                    if (op & 0x0FF0u)
                        break; // shifted register offset: stay on the interpreter
                    offset = ((op & 0xF) == 15) ? (r15 + 4u) : *regs[op & 0xF];
                }
                else {
                    offset = op & 0xFFFu;
                }
                const uint32_t base = ((op >> 16) & 0xF) == 15 ? (r15 + 4u) : *regs[(op >> 16) & 0xF];
                address = (op & 0x00800000u) ? (base + offset) : (base - offset);
                load = (op & 0x00100000u) != 0;
                byte = (op & 0x00400000u) != 0;
                const uint32_t region = address >> 24;
                const bool okRegion = region == 2 || region == 3 || region == 5 ||
                                      region == 6 || region == 7 ||
                                      (region >= 8 && region <= 0xD);
                if (!okRegion)
                    break;
                if (load) {
                    if (!readMap[address >> 12])
                        break;
                }
                else if (address >= 0x80000C4u && address < 0x80000CAu) {
                    break; // GPIO: stay on the interpreter
                }
                xfer = true;
            }
#endif
#if !defined(ROMISL_NO_BLOCK)
            else if ((op & 0x0E000000u) == 0x08000000u && !(op & 0x00400000u) &&
                     (op & 0xFFFFu) != 0 && !(op & 0x8000u) && ((op >> 16) & 0xF) != 15) {
                // LDM/STM without S, non-empty list, no PC in list, Rn != PC
                const unsigned rn = (op >> 16) & 0xF;
                const uint32_t base = *regs[rn];
                blkM = bitCount[op & 0xFF] + bitCount[(op >> 8) & 0xFF];
                blkList = op & 0xFFFFu;
                const bool up = (op & 0x00800000u) != 0;
                const bool pre = (op & 0x01000000u) != 0;
                load = (op & 0x00100000u) != 0;
                blkLow = up ? base : (base - (blkM << 2));
                address = up ? (pre ? base + 4u : base) : (pre ? blkLow : blkLow + 4u);
                // every transfer address must be in a handled region (and mapped
                // for loads); transfers ascend by 4
                bool ok = true;
                uint32_t a = address;
                for (unsigned k = 0; k < blkM; k++, a += 4u) {
                    const uint32_t region = a >> 24;
                    const bool okRegion = region == 2 || region == 3 || region == 5 ||
                                          region == 6 || region == 7 ||
                                          (region >= 8 && region <= 0xD);
                    if (!okRegion || (load && !readMap[a >> 12]) ||
                        (!load && a >= 0x80000C4u && a < 0x80000CAu)) {
                        ok = false;
                        break;
                    }
                }
                if (!ok)
                    break;
                block = true;
            }
#endif
            else {
#if NOODS_GBA_ISLAND_STATS
                ++g_islBrk[4];
#endif
                break;
            }
        }

        // ---- event safety margin ----
        // Exact worst-case costs per category keep the margin tight so more
        // instructions stay in the island near events, while guaranteeing an
        // event can never come due inside an island instruction (mid-
        // instruction DMA preemption stays in runGbaOpcode).
        //   fetch (ARM, ROM, worst)      = 9+9 = 18
        //   ALU / false condition        = 18
        //   MUL/MLA                      = 18 + 5 internal
        //   LDR/STR                      = 18 + (1 stall + 9N + 9S) + 1I = 38
        //   B/BL                         = 18 + branchRefill(18+18) = 54
        //   LDM/STM                      = 18 + m*19 + 1I
        int32_t need = 20;
        if (block)
            need = (int32_t)(21 + blkM * 19);
        else if (branch)
            need = 56;
        else if (xfer)
            need = 40;
        else if (mulOp)
            need = 25;
        if ((int32_t)(c.events.frontCycles - clock) <= need) {
#if NOODS_GBA_ISLAND_STATS
            ++g_islBrk[3];
#endif
            break;
        }

        // ---- 1. fetch phase: advance pipeline, charge the real prefetch model ----
        const uint32_t executing = op;
        r15 += 4;
        *regs[15] = r15;
        op = next;
        pc += 4;
        next = U8TO32(pc, 0);
        clock += bus.fetchOpcode(r15, true, waitCnt);
        cycles = (c.globalCycles = clock);

        // Per-instruction bus flags are reset exactly where runGbaOpcode does it
        // (step 3, after the fetch): the first data access of every instruction
        // must see inSeqAccess == false and charge non-sequential timing.
        bus.beginInstruction();

        // ---- 2. execute + data-phase charging ----
        if (cond) {
            if (alu) {
                ArmAluSpecialized::execute(regs, cpsr, executing);
            }
            else if (mulOp) {
                // Read operands after the advance: registers[15] is PC+8 here,
                // exactly like the interpreter's handler environment.
                const uint32_t rm = *regs[executing & 0xF];
                const uint32_t rs = *regs[(executing >> 8) & 0xF];
                const int mulTicks = Arm7TdmiMul::signedMulTicks(rs);
                uint32_t result = rm * rs;
                if (mla)
                    result += *regs[(executing >> 12) & 0xF];
                *regs[(executing >> 16) & 0xF] = result;
                const int internal = mulTicks + (mla ? 2 : 1) - 1;
                if (internal > 0)
                    clock += bus.stepInternal(internal, waitCnt);
                cycles = (c.globalCycles = clock);
            }
            else if (branch) {
                if (link)
                    *regs[14] = r15 - 4u; // PC+8 at handler time, minus 4
                uint8_t *base0 = readMap[target >> 12];
                uint8_t *base1 = readMap[(target + 4u) >> 12];
                r15 = target + 4u;
                *regs[15] = r15;
                op = U8TO32(base0 + (target & 0xFFFu), 0);
                next = U8TO32(base1 + ((target + 4u) & 0xFFFu), 0);
                pc = base1 + ((target + 4u) & 0xFFFu);
                clock += bus.branchRefill(target, true, waitCnt);
                cycles = (c.globalCycles = clock);
            }
            else if (xfer) {
                if (load) {
                    // Charge exactly like Memory::gbaDataAccess, then read the
                    // mapped page exactly like the Memory::read fast path.
                    int cyc;
                    const uint32_t region = address >> 24;
                    if (!bus.active && region < 8 && ((0x9Bu >> region) & 1)) {
                        bus.nextSeq = false;
                        bus.inSeqAccess = true;
                        bus.hadDataAccess = true;
                        bus.hadLoad = true;
                        cyc = 1;
                    }
                    else {
                        cyc = bus.dataAccess(address, byte ? 1 : 4, bus.inSeqAccess, waitCnt, false);
                        bus.inSeqAccess = true;
                        bus.hadDataAccess = true;
                        bus.hadLoad = true;
                    }
                    clock += cyc;
                    cycles = (c.globalCycles = clock);
                    const uint8_t *data = readMap[address >> 12];
                    uint32_t value = byte ? data[address & 0xFFFu] : U8TO32(data, address & 0xFFCu);
                    if (!byte && (address & 3))
                        value = GbaArmAlu::ror(value, (address & 3) * 8);
                    *regs[(executing >> 12) & 0xF] = value;
                    // trailing 1I after loads (runGbaOpcode step 4)
                    clock += bus.stepInternal(1, waitCnt);
                    cycles = (c.globalCycles = clock);
                }
                else {
                    // Stores run through the full write path so IO-free regions
                    // keep VRAM byte rules and exact bus charging.
                    cycles = (c.globalCycles = clock);
                    bool prev = c.memory.gbaCpuExecActive;
                    c.memory.gbaCpuExecActive = true;
                    const uint32_t value = *regs[(executing >> 12) & 0xF];
                    if (byte)
                        c.memory.write<uint8_t>(true, address, value);
                    else
                        c.memory.write<uint32_t>(true, address, value);
                    c.memory.gbaCpuExecActive = prev;
                    clock = cycles; // onGbaCpuDataAccess charged through advanceGbaCycles
                }
            }
            else { // block: LDM/STM, ARM7 semantics matching the interpreter handlers
                const unsigned rn = (executing >> 16) & 0xF;
                const bool up = (executing & 0x00800000u) != 0;
                const bool wb = (executing & 0x00200000u) != 0;
                const uint32_t finalWB = up ? (blkLow + (blkM << 2)) : blkLow;
                uint32_t a = address;
                if (!load) {
                    // ARM7: if Rn is listed and not the first listed register,
                    // the writeback value is already in Rn for the stores.
                    if (wb && (blkList & (BIT(rn + 1) - 1)) > BIT(rn))
                        *regs[rn] = finalWB;
                    bool prev = c.memory.gbaCpuExecActive;
                    c.memory.gbaCpuExecActive = true;
                    for (unsigned i = 0; i < 16; i++) {
                        if (!(blkList & BIT(i)))
                            continue;
                        cycles = (c.globalCycles = clock);
                        c.memory.write<uint32_t>(true, a, *regs[i]);
                        clock = cycles;
                        a += 4u;
                    }
                    c.memory.gbaCpuExecActive = prev;
                    if (wb)
                        *regs[rn] = finalWB;
                }
                else {
                    // ARM7: Rn is updated before the loads; a listed Rn is then
                    // overwritten by its loaded value.
                    if (wb)
                        *regs[rn] = finalWB;
                    for (unsigned i = 0; i < 16; i++) {
                        if (!(blkList & BIT(i)))
                            continue;
                        int cyc;
                        const uint32_t region = a >> 24;
                        if (!bus.active && region < 8 && ((0x9Bu >> region) & 1)) {
                            bus.nextSeq = false;
                            bus.inSeqAccess = true;
                            bus.hadDataAccess = true;
                            bus.hadLoad = true;
                            cyc = 1;
                        }
                        else {
                            cyc = bus.dataAccess(a, 4, bus.inSeqAccess, waitCnt, false);
                            bus.inSeqAccess = true;
                            bus.hadDataAccess = true;
                            bus.hadLoad = true;
                        }
                        clock += cyc;
                        cycles = (c.globalCycles = clock);
                        // No misalignment rotation here: the interpreter's LDM
                        // handlers assign Memory::read<uint32_t>'s aligned word
                        // directly, unlike LDR.
                        const uint8_t *data = readMap[a >> 12];
                        *regs[i] = U8TO32(data, a & 0xFFCu);
                        a += 4u;
                    }
                    // trailing 1I after loads (runGbaOpcode step 4)
                    clock += bus.stepInternal(1, waitCnt);
                    cycles = (c.globalCycles = clock);
                }
            }
        }
#if NOODS_GBA_ISLAND_STATS
        ++g_islInstrs;
        if (!cond) ++g_islCat[1];
        else if (alu) ++g_islCat[0];
        else if (mulOp) ++g_islCat[2];
        else if (branch) ++g_islCat[3];
        else if (xfer) ++g_islCat[4];
        else ++g_islCat[5];
#endif
        ++done;
    }

    if (!done) {
#if NOODS_GBA_ISLAND_STATS
        ++g_islBrk[5];
#endif
        return false;
    }
#if NOODS_GBA_ISLAND_STATS
    ++g_islEntries;
#endif
    pipeline[0] = op;
    pipeline[1] = next;
    pcData = pc;
    cycles = clock;
    c.globalCycles = clock;
    bus.beginInstruction();
    c.memory.clearGbaDmaOpenBus();
    return true;
}

#endif
#if NOODS_GBA_THUMB_ISLAND
#if !NOODS_GBA_ROM_ISLAND
#include "arm7tdmi_mul.h"
#endif
#if NOODS_GBA_ISLAND_STATS
uint32_t g_tisEntries=0,g_tisFalseEntry=0,g_tisInstrs=0;
uint32_t g_tisBrk[8]={0};  // 0 region,1 margin,2 spare,3 spare,4 unhandled,5 region/mapped,6 page,7 spare
uint32_t g_tisCat[7]={0};  // kind 0..6
#endif
// ============================================================================
// NooDS-Wii v1p170 — cycle-exact THUMB execution island for GamePak ROM.
//
// Most GBA games keep their hot loops in THUMB code (measured: 99.79% of the
// instructions executed by F-Zero: Maximum Velocity are THUMB), so the ARM-only
// island in gba_rom_island.inc never ran for them and only added entry cost.
// This island is the THUMB counterpart, built on the same principle:
//
//   * every fetch and data access is charged through the real GbaBus model
//     (fetchOpcode / branchRefill / dataAccess / stepInternal), so the cycle
//     counts are bit-identical to Interpreter::runGbaOpcode();
//   * only encodings whose semantics can be reproduced exactly are handled, and
//     anything else falls back to the interpreter with no state change;
//   * the loop exits before an event can come due, so mid-instruction DMA
//     preemption (runGbaOpcode step 2) stays exclusively in the interpreter.
//
// What is saved per instruction: the indirect handler-table call, repeated
// waitstate reads, the DMA preemption scan, the gbaCpuExecActive toggles and the
// branchy step-4 epilogue.
// ============================================================================

#if !defined(TISL_MAX_KIND)
#define TISL_MAX_KIND 6
#endif
#if !defined(TISL_MAX_DEPTH)
#define TISL_MAX_DEPTH 4096
#endif
#if !defined(TISL_KEEP_ISLAND)
#define TISL_KEEP_ISLAND 0
#endif
#if !defined(TISL_DMA_SCAN)
#define TISL_DMA_SCAN 0
#endif
#if !defined(TISL_SYNC_PIPELINE)
#define TISL_SYNC_PIPELINE 0
#endif
#if TISL_DMA_SCAN
static uint32_t g_tisPreempt=0, g_tisOverrun=0;
#endif

#if NOODS_GBA_ISLAND_VERIFY
// Differential self-check: run the instruction in the island, then rewind and let
// Interpreter::runGbaOpcode() run the very same instruction, and compare the full
// visible state. Any mismatch is reported with PC and opcode; after too many the
// island disables itself so the run keeps going on the interpreter.
static bool g_tisVerifyOff = false;
int g_tisVerifyFails = 0;
uint32_t g_tisFirstPc = 0, g_tisFirstOp = 0, g_tisFirstDepth = 0;
char g_tisFirstWhy[16] = {0};
uint32_t g_tisLastPc = 0, g_tisLastOp = 0;
uint32_t g_tisFirstI0=0, g_tisFirstI1=0, g_tisFirstP0=0, g_tisFirstP1=0, g_tisFirstR15=0, g_tisFirstIPc=0;
uint32_t g_tisRegIdx=99, g_tisRegIsl=0, g_tisRegRef=0, g_tisCpsrIsl=0, g_tisCpsrRef=0;
char g_tisLastWhy[16] = {0};
#define TISL_SNAPSHOT()                                                          \
    uint32_t vR[15];                                                             \
    for (unsigned vi = 0; vi < 15; vi++) vR[vi] = *regs[vi];                     \
    const uint32_t vR15 = r15, vCpsr = cpsr, vP0 = o, vP1 = next;                 \
    const uint32_t vClk = clock; uint8_t *const vPc = pc;                        \
    GbaBus vBus = bus; const uint16_t vO = o;
#define TISL_VERIFY() do {                                                       \
    uint32_t iR[15];                                                             \
    for (unsigned vi = 0; vi < 15; vi++) iR[vi] = *regs[vi];                     \
    const uint32_t iR15 = r15, iCpsr = cpsr, iP0 = op, iP1 = next;                  \
    const uint32_t iClk = clock; GbaBus iBus = bus; uint8_t *const iPc = pc;      \
    for (unsigned vi = 0; vi < 15; vi++) *regs[vi] = vR[vi];                     \
    *regs[15] = vR15; cpsr = vCpsr;                                              \
    pipeline[0] = vP0; pipeline[1] = vP1; pc = vPc; pcData = vPc; bus = vBus;    \
    r15 = vR15; clock = vClk; cycles = vClk; c.globalCycles = vClk;              \
    op = (uint16_t)vP0; next = (uint16_t)vP1;                                    \
    runGbaOpcode();                                                              \
    const char *why = nullptr;                                                   \
    for (unsigned vi = 0; vi < 15; vi++)                                         \
        if (*regs[vi] != iR[vi]) { why = "reg"; g_tisRegIdx = vi;                \
            g_tisRegIsl = iR[vi]; g_tisRegRef = *regs[vi]; break; }              \
    if (!why && cpsr != iCpsr) { why = "cpsr"; g_tisCpsrIsl = iCpsr; g_tisCpsrRef = cpsr; } \
    else if (!why) { g_tisCpsrIsl = iCpsr; g_tisCpsrRef = cpsr; }                 \
    if (!why && *regs[15] != iR15) why = "r15";                                  \
    if (!why && iClk != (uint32_t)c.globalCycles) why = "cycles";                \
    if (!why && (pipeline[0] != iP0 || pipeline[1] != iP1)) why = "pipe";        \
    if (!why && iBus.headAddr != bus.headAddr) why = "bus.head";                 \
    if (!why && iBus.tailAddr != bus.tailAddr) why = "bus.tail";                 \
    if (!why && iBus.count != bus.count) why = "bus.count";                      \
    if (!why && iBus.duty != bus.duty) why = "bus.duty";                         \
    if (!why && iBus.active != bus.active) why = "bus.active";                   \
    if (!why && iBus.nextSeq != bus.nextSeq) why = "bus.nextSeq";                \
    if (!why && iBus.inSeqAccess != bus.inSeqAccess) why = "bus.inSeq";          \
    if (!why && iBus.hadDataAccess != bus.hadDataAccess) why = "bus.hadData";    \
    if (!why && iBus.hadLoad != bus.hadLoad) why = "bus.hadLoad";                \
    if (!why && iBus.branched != bus.branched) why = "bus.branched";             \
    if (!why && iBus.lastFetchUsedDuty != bus.lastFetchUsedDuty) why = "bus.dutyUsed"; \
    if (!why && iBus.preSwiSaved != bus.preSwiSaved) why = "bus.preSwi";         \
    if (why) {                                                                   \
        if (g_tisVerifyFails == 0) {                                             \
            g_tisFirstPc = vR15 - 2u; g_tisFirstOp = vO; g_tisFirstDepth = done;  \
            g_tisFirstI0 = iP0; g_tisFirstI1 = iP1;                              \
            g_tisFirstP0 = pipeline[0]; g_tisFirstP1 = pipeline[1];              \
            g_tisFirstR15 = iR15; g_tisFirstIPc = (uint32_t)(iPc - readMap[iR15 >> 12]); \
            { unsigned k=0; while (why[k] && k < 15) { g_tisFirstWhy[k]=why[k]; ++k; } g_tisFirstWhy[k]=0; } \
        }                                                                        \
        g_tisLastPc = vR15 - 2u; g_tisLastOp = vO;                               \
        { unsigned k=0; while (why[k] && k < 15) { g_tisLastWhy[k]=why[k]; ++k; } g_tisLastWhy[k]=0; } \
        if (++g_tisVerifyFails > 60) g_tisVerifyOff = true;                      \
    }                                                                            \
    if (TISL_KEEP_ISLAND && !why) {                                              \
        /* continue from the island's own state: only difference left vs a      \
           non-verify build is that runGbaOpcode() also ran */                  \
        for (unsigned vi = 0; vi < 15; vi++) *regs[vi] = iR[vi];                 \
        *regs[15] = iR15; r15 = iR15; cpsr = iCpsr;                              \
        pipeline[0] = iP0; pipeline[1] = iP1; bus = iBus;                        \
        clock = iClk; cycles = iClk; c.globalCycles = iClk;                      \
        op = (uint16_t)iP0; next = (uint16_t)iP1; pc = iPc; pcData = iPc;        \
    } else {                                                                     \
        r15 = *regs[15]; clock = c.globalCycles; cycles = clock;                 \
        op = (uint16_t)pipeline[0]; next = (uint16_t)pipeline[1]; pc = pcData;   \
    }                                                                            \
} while (0)
#else
#define TISL_SNAPSHOT() ((void)0)
#define TISL_VERIFY() ((void)0)
#endif

__attribute__((noinline)) bool Interpreter::runGbaThumbIsland() {
    Core &c = *core;
    GbaBus &bus = c.gbaBus;
    uint32_t r15 = *registers[15];
    const uint32_t region0 = r15 >> 24;
#if NOODS_GBA_ISLAND_VERIFY
    if (g_tisVerifyOff) return false;
#endif
    if (!(cpsr & 0x20u) || region0 < 8u || region0 > 0xDu || (r15 & 1) || !pcData ||
        c.globalCycles != cycles || (int32_t)(c.events.frontCycles - cycles) <= 24) {
#if NOODS_GBA_ISLAND_STATS
        ++g_tisFalseEntry;
#endif
        return false;
    }

    auto regs = registers;
    uint8_t **readMap = c.memory.readMap7;
    if (!readMap[r15 >> 12]) {
#if NOODS_GBA_ISLAND_STATS
        ++g_tisFalseEntry;
#endif
        return false;
    }

    uint32_t clock = cycles;
    uint16_t op = (uint16_t)pipeline[0];
    uint16_t next = (uint16_t)pipeline[1];
    uint8_t *pc = pcData;
    unsigned done = 0;

    while (true) {
        // Stay on GamePak ROM as the execution source (executing address r15-2).
        if (done >= TISL_MAX_DEPTH) break;
        const uint32_t pcRegion = (r15 - 2u) >> 24;
        if (pcRegion < 8u || pcRegion > 0xDu) {
#if NOODS_GBA_ISLAND_STATS
            ++g_tisBrk[0];
#endif
            break;
        }
        uint16_t waitCnt = c.memory.getWaitCnt();

        // ---- classify ----
        // kind: 0 = unsupported, 1 = alu, 2 = mul, 3 = xfer, 4 = block, 5 = branch,
        //       6 = BL setup (no branch)
        unsigned kind = 0;
        bool load = false, byte = false, link = false;
        uint32_t address = 0, target = 0;
        unsigned m = 0, rd = 0;
        uint32_t list = 0, low = 0;
        const uint16_t o = op;
        const unsigned hi5 = o >> 11;

        if (hi5 < 3) {
            kind = 1; // LSL/LSR/ASR Rd,Rs,#imm
        }
        else if (hi5 == 3) {
            kind = 1; // ADD/SUB register and 3-bit immediate
        }
        else if (hi5 < 8) {
            kind = 1; // MOV/CMP/ADD/SUB Rd,#imm8
        }
        else if ((o & 0xFC00) == 0x4000) {
            kind = (((o >> 6) & 0xF) == 13) ? 2 : 1; // ALU ops / MUL
        }
        else if ((o & 0xF800) == 0x4800) {
            kind = 3; load = true; // LDR Rd,[PC,#imm8*4]
            // Handlers see PC+4; at classify time r15 is still PC+2.
            // Rd lives in bits 10-8 here, not bits 2-0.
            rd = (o >> 8) & 7;
            address = ((r15 + 2u) & ~3u) + (uint32_t)((o & 0xFF) << 2);
        }
        else if ((o & 0xF000) == 0x5000) {
            // STR/STRB/LDR/LDRB Rd,[Rb,Ro]; halfword and signed forms stay on the
            // interpreter (their alignment rules differ).
            const unsigned sel = (o >> 9) & 7;
            if (sel & 1) {
#if NOODS_GBA_ISLAND_STATS
                ++g_tisBrk[4];
#endif
                break;
            }
            load = (sel & 4) != 0;
            byte = (sel & 2) != 0;
            rd = o & 7;
            address = *regs[(o >> 3) & 7] + *regs[(o >> 6) & 7];
            kind = 3;
        }
        else if ((o & 0xE000) == 0x6000) {
            const unsigned sel = (o >> 11) & 3; // 00 STR 01 LDR 10 STRB 11 LDRB
            load = (sel & 1) != 0;
            byte = (sel & 2) != 0;
            const uint32_t imm = (o >> 6) & 0x1F;
            rd = o & 7;
            address = *regs[(o >> 3) & 7] + (byte ? imm : (imm << 2));
            kind = 3;
        }
        else if ((o & 0xF000) == 0x9000) {
            load = (o & 0x0800) != 0; // STR/LDR Rd,[SP,#imm8*4]
            rd = (o >> 8) & 7;        // Rd in bits 10-8
            address = *regs[13] + (uint32_t)((o & 0xFF) << 2);
            kind = 3;
        }
        else if (hi5 == 20 || hi5 == 21) {
            kind = 1; // ADD Rd,PC/SP,#imm8*4
        }
        else if ((o & 0xFF80) == 0xB000 || (o & 0xFF80) == 0xB080) {
            kind = 1; // ADD/SUB SP,#imm7*4
        }
        else if ((o & 0xF600) == 0xB400) {
            const bool isPop = (o & 0x0800) != 0;
            const bool extra = (o & 0x0100) != 0;
            if (isPop && extra) {
#if NOODS_GBA_ISLAND_STATS
                ++g_tisBrk[4];
#endif
                break; // POP {...,PC} branches
            }
            list = o & 0xFF;
            m = bitCount[list] + (extra ? 1 : 0);
            load = isPop;
            kind = 4;
            low = isPop ? *regs[13] : (*regs[13] - (m << 2));
            address = isPop ? low : low;
        }
        else if ((o & 0xF000) == 0xC000) {
            list = o & 0xFF;
            if (!list) {
#if NOODS_GBA_ISLAND_STATS
                ++g_tisBrk[4];
#endif
                break; // empty list has PC load/store special cases
            }
            load = (o & 0x0800) != 0;
            m = bitCount[list];
            kind = 4;
            const uint32_t base = *regs[(o >> 8) & 7];
            low = base;
            address = base;
        }
        else if ((o & 0xFF00) == 0xDF00) {
#if NOODS_GBA_ISLAND_STATS
            ++g_tisBrk[4];
#endif
            break; // SWI
        }
        else if ((o & 0xF000) == 0xD000) {
            const unsigned cc = (o >> 8) & 0xF;
            if (cc >= 14) {
#if NOODS_GBA_ISLAND_STATS
                ++g_tisBrk[4];
#endif
                break;
            }
            const unsigned cond = condition[(cc << 4) | (cpsr >> 28)];
            if (cond != 1) {
                kind = 1; // not taken: one cycle, no flags changed
            }
            else {
                kind = 5;
                target = r15 + 2u + (uint32_t)((int32_t)(int8_t)(uint8_t)o << 1);
            }
        }
        else if ((o & 0xF800) == 0xE000) {
            kind = 5; // B label
            target = r15 + 2u + (uint32_t)((int32_t)(int16_t)(uint16_t)(o << 5) >> 4);
        }
        else if ((o & 0xF800) == 0xF000) {
            kind = 6; // BL setup: LR = PC + (imm11 << 11), no branch
        }
        else if ((o & 0xF800) == 0xF800) {
            kind = 5; link = true; // BL label
            target = *regs[14] + (uint32_t)((o & 0x7FF) << 1);
        }
        else {
#if NOODS_GBA_ISLAND_STATS
            ++g_tisBrk[4];
#endif
            break;
        }

#if TISL_MAX_KIND < 6
        if (kind > TISL_MAX_KIND) break;
#endif
        // ---- region / mapping validation for data accesses ----
        if (kind == 3) {
            const uint32_t region = address >> 24;
            const bool okRegion = region == 2 || region == 3 || region == 5 ||
                                  region == 6 || region == 7 ||
                                  (region >= 8 && region <= 0xD);
            if (!okRegion || (load && !readMap[address >> 12]) ||
                (!load && address >= 0x80000C4u && address < 0x80000CAu)) {
#if NOODS_GBA_ISLAND_STATS
                ++g_tisBrk[5];
#endif
                break;
            }
        }
        else if (kind == 4) {
            bool ok = true;
            for (uint32_t a = address, k = 0; k < m; k++, a += 4u) {
                const uint32_t region = a >> 24;
                const bool okRegion = region == 2 || region == 3 || region == 5 ||
                                      region == 6 || region == 7 ||
                                      (region >= 8 && region <= 0xD);
                if (!okRegion || (load && !readMap[a >> 12]) ||
                    (!load && a >= 0x80000C4u && a < 0x80000CAu)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
#if NOODS_GBA_ISLAND_STATS
                ++g_tisBrk[5];
#endif
                break;
            }
        }

        // ---- event safety margin: exact worst-case cost per category ----
        //   fetch (THUMB, ROM, worst) = 9+9 = 18 (16-bit ROM bus halves)
        //   ALU / not-taken branch    = 18
        //   MUL                       = 18 + 5 internal
        //   LDR/STR                   = 18 + (1 stall + 9N + 9S) + 1I = 38
        //   B/BL                      = 18 + branchRefill(18+18) = 54
        //   PUSH/POP/STMIA/LDMIA      = 18 + m*19 + 1I
        int32_t need = 20;
        if (kind == 4)
            need = (int32_t)(21 + m * 19);
        else if (kind == 5)
            need = 56;
        else if (kind == 3)
            need = 40;
        else if (kind == 2)
            need = 25;
        if ((int32_t)(c.events.frontCycles - clock) <= need) {
#if NOODS_GBA_ISLAND_STATS
            ++g_tisBrk[1];
#endif
            break;
        }

        // ---- 1. fetch phase: advance the pipeline, charge the prefetch model ----
        TISL_SNAPSHOT();
        const uint16_t executing = o;
        const uint32_t newPc = r15 + 2u;
        uint8_t *newPtr;
        if ((newPc & 0xFFEu) && pc) {
            newPtr = pc + 2;
        }
        else { // page switch: re-derive the opcode pointer exactly like getOpcode16
            uint8_t *np = readMap[newPc >> 12];
            if (!np) {
#if NOODS_GBA_ISLAND_STATS
                ++g_tisBrk[6];
#endif
                break;
            }
            newPtr = np + (newPc & 0xFFEu);
        }
        const uint16_t newWord = U8TO16(newPtr, 0);
        // commit the pipeline advance (nothing above may break after this point)
        r15 = newPc;
        *regs[15] = r15;
        op = next; // pipeline[0] = pipeline[1], then the freshly fetched halfword
        next = newWord;
        pc = newPtr;
        clock += bus.fetchOpcode(r15, false, waitCnt);
        cycles = (c.globalCycles = clock);
#if TISL_DMA_SCAN
        // runGbaOpcode step 2, copied verbatim: a DMA that became ready by the end
        // of the fetch preempts the bus before the data phase.
        if (!halted && c.events.frontCycles <= clock) {
            ++g_tisPreempt;
            bool usedDuty = bus.lastFetchUsedDuty;
            for (size_t idx = 0; idx < c.events.size() && c.events[idx].cycles <= clock; ) {
                SchedTask t = c.events[idx].task;
                if (t >= DMA7_TRANSFER0 && t <= DMA7_TRANSFER3) {
                    c.events.eraseAt(idx);
                    c.globalCycles = clock;
                    c.tasks[t]();
                    c.globalCycles = clock;
                    int sWait = GbaBus::romWaitS(r15, waitCnt);
                    if (usedDuty && ((c.dma[1].firstPakWasWrite && sWait == 3) ||
                                     (c.dma[1].firstPakWasRead && !(cpsr & BIT(5)) && sWait == 2))) {
                        clock = (c.globalCycles += 1);
                        usedDuty = false;
                    }
                    idx = 0;
                    continue;
                }
                idx++;
            }
            cycles = clock;
            waitCnt = c.memory.getWaitCnt();
        }
#endif
        bus.beginInstruction();

        // ---- 2. execute + data-phase charging ----
        // Match runGbaOpcode step 3: the DMA open-bus latch is CPU-execution aware,
        // so every island instruction has to toggle the same flag and clear the
        // latch afterwards, exactly like the interpreter does.
        c.memory.gbaCpuExecActive = true;
        if (kind == 1) {
            const unsigned e5 = executing >> 11;
            if (e5 < 3) { // LSL/LSR/ASR Rd,Rs,#imm5
                const uint8_t shift = (executing >> 6) & 0x1F;
                const uint32_t src = *regs[(executing >> 3) & 7];
                uint32_t res;
                bool carryOut;
                if (e5 == 0) { // LSL
                    res = src << shift;
                    carryOut = shift > 0 ? (bool)(src & BIT(32 - shift)) : (bool)(cpsr & BIT(29));
                    *regs[executing & 7] = res;
                    cpsr = (cpsr & ~0xC0000000) | (res & BIT(31)) | ((res == 0) << 30);
                    if (shift > 0) cpsr = (cpsr & ~BIT(29)) | (carryOut << 29);
                }
                else if (e5 == 1) { // LSR
                    res = shift ? (src >> shift) : 0;
                    carryOut = (bool)(src & BIT(shift ? (shift - 1) : 31));
                    *regs[executing & 7] = res;
                    cpsr = (cpsr & ~0xE0000000) | (res & BIT(31)) | ((res == 0) << 30) | (carryOut << 29);
                }
                else { // ASR
                    res = shift ? ((int32_t)src >> shift) : ((src & BIT(31)) ? 0xFFFFFFFFu : 0);
                    carryOut = (bool)(src & BIT(shift ? (shift - 1) : 31));
                    *regs[executing & 7] = res;
                    cpsr = (cpsr & ~0xE0000000) | (res & BIT(31)) | ((res == 0) << 30) | (carryOut << 29);
                }
            }
            else if (e5 == 3) { // ADD/SUB register and 3-bit immediate
                const uint32_t a = *regs[(executing >> 3) & 7];
                const bool isSub = (executing & BIT(9)) != 0;
                const uint32_t b = (executing & BIT(10)) ? (uint32_t)((executing >> 6) & 7)
                                                         : *regs[(executing >> 6) & 7];
                const uint32_t res = isSub ? (a - b) : (a + b);
                *regs[executing & 7] = res;
                cpsr = (cpsr & ~0xF0000000) | (res & BIT(31)) | ((res == 0) << 30) |
                       (isSub ? ((a >= res) << 29) : ((a > res) << 29)) |
                       (isSub ? ((uint32_t)(((b ^ a) & ~(res ^ b) & BIT(31))) >> 3)
                              : ((uint32_t)((~(b ^ a) & (res ^ b) & BIT(31))) >> 3));
            }
            else if (e5 < 8) { // MOV/CMP/ADD/SUB Rd,#imm8
                const unsigned rd = (executing >> 8) & 7;
                const uint32_t imm = executing & 0xFF;
                const unsigned sel = (executing >> 11) & 3;
                if (sel == 0) { // MOV
                    cpsr = (cpsr & ~0xC0000000) | (imm & BIT(31)) | ((imm == 0) << 30);
                    *regs[rd] = imm;
                }
                else if (sel == 1) { // CMP
                    const uint32_t a = *regs[rd];
                    const uint32_t res = a - imm;
                    cpsr = (cpsr & ~0xF0000000) | (res & BIT(31)) | ((res == 0) << 30) |
                           ((a >= res) << 29) |
                           ((uint32_t)(((imm ^ a) & ~(res ^ imm) & BIT(31))) >> 3);
                }
                else { // ADD / SUB
                    const uint32_t a = *regs[rd];
                    const bool isSub = (sel == 3);
                    const uint32_t res = isSub ? (a - imm) : (a + imm);
                    *regs[rd] = res;
                    cpsr = (cpsr & ~0xF0000000) | (res & BIT(31)) | ((res == 0) << 30) |
                           (isSub ? ((a >= res) << 29) : ((a > res) << 29)) |
                           (isSub ? ((uint32_t)(((imm ^ a) & ~(res ^ imm) & BIT(31))) >> 3)
                                  : ((uint32_t)((~(imm ^ a) & (res ^ imm) & BIT(31))) >> 3));
                }
            }
            else if ((executing & 0xFC00) == 0x4000) { // ALU ops
                const unsigned sub = (executing >> 6) & 0xF;
                uint32_t *rd = regs[executing & 7];
                const uint32_t a = *rd;
                const uint32_t b = *regs[(executing >> 3) & 7];
                switch (sub) {
                case 2: { // LSL
                    const uint8_t s = (uint8_t)b;
                    const uint32_t res = (s < 32) ? (a << s) : 0;
                    *rd = res;
                    cpsr = (cpsr & ~0xC0000000) | (res & BIT(31)) | ((res == 0) << 30);
                    if (s > 0) cpsr = (cpsr & ~BIT(29)) | ((bool)(s <= 32 && (a & BIT(32 - s))) << 29);
                    break;
                }
                case 3: { // LSR
                    const uint8_t s = (uint8_t)b;
                    const uint32_t res = (s < 32) ? (a >> s) : 0;
                    *rd = res;
                    cpsr = (cpsr & ~0xC0000000) | (res & BIT(31)) | ((res == 0) << 30);
                    if (s > 0) cpsr = (cpsr & ~BIT(29)) | ((bool)(s <= 32 && (a & BIT(s - 1))) << 29);
                    break;
                }
                case 4: { // ASR
                    const uint8_t s = (uint8_t)b;
                    const uint32_t res = (s < 32) ? ((int32_t)a >> s) : ((a & BIT(31)) ? 0xFFFFFFFFu : 0);
                    *rd = res;
                    cpsr = (cpsr & ~0xC0000000) | (res & BIT(31)) | ((res == 0) << 30);
                    if (s > 0) cpsr = (cpsr & ~BIT(29)) | ((bool)(a & BIT((s <= 32) ? (s - 1) : 31)) << 29);
                    break;
                }
                case 7: { // ROR
                    const uint8_t s = (uint8_t)b;
                    const unsigned amt = s & 31;
                    const uint32_t res = amt ? ((a >> amt) | (a << (32 - amt))) : a;
                    *rd = res;
                    cpsr = (cpsr & ~0xC0000000) | (res & BIT(31)) | ((res == 0) << 30);
                    if (s > 0) cpsr = (cpsr & ~BIT(29)) | ((bool)(a & BIT((s - 1) & 0x1F)) << 29);
                    break;
                }
                case 0: { // AND
                    const uint32_t res = a & b;
                    *rd = res;
                    cpsr = (cpsr & ~0xC0000000) | (res & BIT(31)) | ((res == 0) << 30);
                    break;
                }
                case 1: { // EOR
                    const uint32_t res = a ^ b;
                    *rd = res;
                    cpsr = (cpsr & ~0xC0000000) | (res & BIT(31)) | ((res == 0) << 30);
                    break;
                }
                case 5: { // ADC
                    const uint32_t carryIn = (cpsr & BIT(29)) >> 29;
                    const uint32_t res = a + b + carryIn;
                    *rd = res;
                    cpsr = (cpsr & ~0xF0000000) | (res & BIT(31)) | ((res == 0) << 30) |
                           (((a > res || (b == 0xFFFFFFFFu && (cpsr & BIT(29)))) << 29) & 0xFFFFFFFFu) |
                           ((uint32_t)((~(b ^ a) & (res ^ b) & BIT(31))) >> 3);
                    break;
                }
                case 6: { // SBC
                    const uint32_t carryIn = (cpsr & BIT(29)) >> 29;
                    const uint32_t res = a - b - 1 + carryIn;
                    *rd = res;
                    cpsr = (cpsr & ~0xF0000000) | (res & BIT(31)) | ((res == 0) << 30) |
                           (((a >= res && (b != 0xFFFFFFFFu || (cpsr & BIT(29)))) << 29) & 0xFFFFFFFFu) |
                           ((uint32_t)(((b ^ a) & ~(res ^ b) & BIT(31))) >> 3);
                    break;
                }
                case 8: { // TST
                    const uint32_t res = a & b;
                    cpsr = (cpsr & ~0xC0000000) | (res & BIT(31)) | ((res == 0) << 30);
                    break;
                }
                case 9: { // NEG
                    const uint32_t res = 0 - b;
                    *rd = res;
                    cpsr = (cpsr & ~0xF0000000) | (res & BIT(31)) | ((res == 0) << 30) |
                           ((b == 0) << 29) | ((b == 0x80000000u) << 28);
                    break;
                }
                case 10: { // CMP
                    const uint32_t res = a - b;
                    cpsr = (cpsr & ~0xF0000000) | (res & BIT(31)) | ((res == 0) << 30) |
                           ((a >= res) << 29) |
                           ((uint32_t)(((b ^ a) & ~(res ^ b) & BIT(31))) >> 3);
                    break;
                }
                case 11: { // CMN
                    const uint32_t res = a + b;
                    cpsr = (cpsr & ~0xF0000000) | (res & BIT(31)) | ((res == 0) << 30) |
                           ((a > res) << 29) |
                           ((uint32_t)((~(b ^ a) & (res ^ b) & BIT(31))) >> 3);
                    break;
                }
                case 12: { // ORR
                    const uint32_t res = a | b;
                    *rd = res;
                    cpsr = (cpsr & ~0xC0000000) | (res & BIT(31)) | ((res == 0) << 30);
                    break;
                }
                case 14: { // BIC
                    const uint32_t res = a & ~b;
                    *rd = res;
                    cpsr = (cpsr & ~0xC0000000) | (res & BIT(31)) | ((res == 0) << 30);
                    break;
                }
                default: { // MVN
                    const uint32_t res = ~b;
                    *rd = res;
                    cpsr = (cpsr & ~0xC0000000) | (res & BIT(31)) | ((res == 0) << 30);
                    break;
                }
                }
            }
            else if (e5 == 20 || e5 == 21) { // ADD Rd,PC/SP,#imm8*4
                const uint32_t src = (e5 == 20) ? (*regs[15] & ~3u) : *regs[13];
                *regs[(executing >> 8) & 7] = src + (uint32_t)((executing & 0xFF) << 2);
            }
            else if (e5 == 22) { // ADD/SUB SP,#imm7*4
                const uint32_t imm = (uint32_t)(((executing & BIT(7)) ? (0 - (executing & 0x7F))
                                                                      : (executing & 0x7F)) << 2);
                *regs[13] += imm;
            }
            else { // not-taken conditional branch: one cycle, nothing changes
            }
        }
        else if (kind == 2) { // MUL Rd,Rs (ARM7: exact ticks and carry)
            const uint32_t a = *regs[executing & 7];
            const uint32_t b = *regs[(executing >> 3) & 7];
            auto r = Arm7TdmiMul::execute(Arm7TdmiMul::SHORT, b, a);
            const uint32_t res = (uint32_t)r.output;
            *regs[executing & 7] = res;
            cpsr = (cpsr & ~0xE0000000) | (res & BIT(31)) | ((res == 0) << 30) | ((uint32_t)r.carry << 29);
            const int internal = r.ticks; // handler returns ticks+1, one is the fetch
            if (internal > 0)
                clock += bus.stepInternal(internal, waitCnt);
            cycles = (c.globalCycles = clock);
        }
        else if (kind == 3) {
            if (load) {
                int cyc;
                const uint32_t region = address >> 24;
                if (!bus.active && region < 8 && ((0x9Bu >> region) & 1)) {
                    bus.nextSeq = false;
                    bus.inSeqAccess = true;
                    bus.hadDataAccess = true;
                    bus.hadLoad = true;
                    cyc = 1;
                }
                else {
                    cyc = bus.dataAccess(address, byte ? 1 : 4, bus.inSeqAccess, waitCnt, false);
                    bus.inSeqAccess = true;
                    bus.hadDataAccess = true;
                    bus.hadLoad = true;
                }
                clock += cyc;
                cycles = (c.globalCycles = clock);
                const uint8_t *data = readMap[address >> 12];
                uint32_t value = byte ? data[address & 0xFFFu] : U8TO32(data, address & 0xFFCu);
                if (!byte && (address & 3))
                    value = GbaArmAlu::ror(value, (address & 3) * 8);
                *regs[rd] = value;
                clock += bus.stepInternal(1, waitCnt); // trailing 1I after loads
                cycles = (c.globalCycles = clock);
            }
            else {
                cycles = (c.globalCycles = clock);
                bool prev = c.memory.gbaCpuExecActive;
                c.memory.gbaCpuExecActive = true;
                const uint32_t value = *regs[rd];
                if (byte)
                    c.memory.write<uint8_t>(true, address, value);
                else
                    c.memory.write<uint32_t>(true, address, value);
                c.memory.gbaCpuExecActive = prev;
                clock = cycles; // charged through advanceGbaCycles inside write
            }
        }
        else if (kind == 4) { // PUSH/POP/STMIA/LDMIA
            uint32_t a = address;
            if ((executing & 0xF600) == 0xB400) { // PUSH/POP
                if (load) {
                    for (unsigned i = 0; i < 8; i++) {
                        if (!(list & BIT(i))) continue;
                        int cyc;
                        const uint32_t region = a >> 24;
                        if (!bus.active && region < 8 && ((0x9Bu >> region) & 1)) {
                            bus.nextSeq = false; bus.inSeqAccess = true;
                            bus.hadDataAccess = true; bus.hadLoad = true;
                            cyc = 1;
                        }
                        else {
                            cyc = bus.dataAccess(a, 4, bus.inSeqAccess, waitCnt, false);
                            bus.inSeqAccess = true; bus.hadDataAccess = true; bus.hadLoad = true;
                        }
                        clock += cyc;
                        cycles = (c.globalCycles = clock);
                        *regs[i] = U8TO32(readMap[a >> 12], a & 0xFFCu);
                        *regs[13] = (a += 4u);
                    }
                }
                else {
                    const uint32_t sp0 = *regs[13] - (m << 2);
                    *regs[13] = sp0;
                    a = sp0;
                    bool prev = c.memory.gbaCpuExecActive;
                    c.memory.gbaCpuExecActive = true;
                    for (unsigned i = 0; i < 8; i++) {
                        if (!(list & BIT(i))) continue;
                        cycles = (c.globalCycles = clock);
                        c.memory.write<uint32_t>(true, a, *regs[i]);
                        clock = cycles;
                        a += 4u;
                    }
                    if (executing & 0x0100) { // PUSH {...,LR}
                        cycles = (c.globalCycles = clock);
                        c.memory.write<uint32_t>(true, a, *regs[14]);
                        clock = cycles;
                    }
                    c.memory.gbaCpuExecActive = prev;
                }
            }
            else { // STMIA/LDMIA Rb!,<Rlist>
                const unsigned rb = (executing >> 8) & 7;
                if (load) {
                    *regs[rb] += (m << 2); // writeback happens before the loads
                    for (unsigned i = 0; i < 8; i++) {
                        if (!(list & BIT(i))) continue;
                        int cyc;
                        const uint32_t region = a >> 24;
                        if (!bus.active && region < 8 && ((0x9Bu >> region) & 1)) {
                            bus.nextSeq = false; bus.inSeqAccess = true;
                            bus.hadDataAccess = true; bus.hadLoad = true;
                            cyc = 1;
                        }
                        else {
                            cyc = bus.dataAccess(a, 4, bus.inSeqAccess, waitCnt, false);
                            bus.inSeqAccess = true; bus.hadDataAccess = true; bus.hadLoad = true;
                        }
                        clock += cyc;
                        cycles = (c.globalCycles = clock);
                        *regs[i] = U8TO32(readMap[a >> 12], a & 0xFFCu);
                        a += 4u;
                    }
                }
                else {
                    bool prev = c.memory.gbaCpuExecActive;
                    c.memory.gbaCpuExecActive = true;
                    if ((list & (BIT(rb + 1) - 1)) > BIT(rb)) // ARM7 quirk
                        *regs[rb] = a + (m << 2);
                    for (unsigned i = 0; i < 8; i++) {
                        if (!(list & BIT(i))) continue;
                        cycles = (c.globalCycles = clock);
                        c.memory.write<uint32_t>(true, a, *regs[i]);
                        clock = cycles;
                        a += 4u;
                    }
                    *regs[rb] = a;
                    c.memory.gbaCpuExecActive = prev;
                }
            }
            if (load) {
                clock += bus.stepInternal(1, waitCnt); // trailing 1I after loads
                cycles = (c.globalCycles = clock);
            }
        }
        else if (kind == 6) { // BL setup
            *regs[14] = r15 + (uint32_t)(((int32_t)(int16_t)(uint16_t)(executing << 5) >> 4) << 11);
        }
        else { // branch
            if (link)
                *regs[14] = r15 - 1u; // return address with the THUMB bit set
            const uint32_t tp = target & ~1u;
            uint8_t *b0 = readMap[tp >> 12];
            uint8_t *b1 = readMap[(tp + 2u) >> 12];
            if (!b0 || !b1) {
#if NOODS_GBA_ISLAND_STATS
                ++g_tisBrk[6];
#endif
                break;
            }
            r15 = tp + 2u;
            *regs[15] = r15;
            op = U8TO16(b0 + (tp & 0xFFFu), 0);
            next = U8TO16(b1 + ((tp + 2u) & 0xFFFu), 0);
            pc = b1 + ((tp + 2u) & 0xFFFu);
            clock += bus.branchRefill(tp, false, waitCnt);
            cycles = (c.globalCycles = clock);
        }

        c.memory.gbaCpuExecActive = false;
        c.memory.clearGbaDmaOpenBus();
#if TISL_SYNC_PIPELINE
        pipeline[0] = op;
        pipeline[1] = next;
        pcData = pc;
        cycles = clock;
        c.globalCycles = clock;
#endif
#if TISL_DMA_SCAN
        if ((int32_t)(c.events.frontCycles - clock) < 0) {
            if (g_tisOverrun < 40)
                fprintf(stderr, "TISL_OVERRUN pc=%08X op=%04X kind=%u by=%d\n",
                        (unsigned)(r15 - 2u), (unsigned)executing, kind,
                        (int)(clock - c.events.frontCycles));
            ++g_tisOverrun;
        }
#endif
        TISL_VERIFY();
#if NOODS_GBA_ISLAND_STATS
        ++g_tisInstrs;
        if (kind <= 6) ++g_tisCat[kind];
#endif
        ++done;
    }

    if (!done)
        return false;
#if NOODS_GBA_ISLAND_STATS
    ++g_tisEntries;
#endif
    pipeline[0] = op;
    pipeline[1] = next;
    pcData = pc;
    cycles = clock;
    c.globalCycles = clock;
    bus.beginInstruction();
    c.memory.clearGbaDmaOpenBus();
    return true;
}

#if NOODS_GBA_ISLAND_STATS
void dumpGbaThumbIslandStats(FILE *f) {
    if (!f) return;
    fprintf(f, "GBA_TISLAND entries=%u false_entries=%u instrs=%u cats alu=%u x2=%u mul=%u xfer=%u block=%u branch=%u blsetup=%u\n",
        g_tisEntries,g_tisFalseEntry,g_tisInstrs,g_tisCat[0],g_tisCat[1],g_tisCat[2],g_tisCat[3],g_tisCat[4],g_tisCat[5],g_tisCat[6]);
    fprintf(f, "GBA_TISLAND_BREAKS region=%u margin=%u b2=%u b3=%u unhandled=%u regionmap=%u page=%u\n",
        g_tisBrk[0],g_tisBrk[1],g_tisBrk[2],g_tisBrk[3],g_tisBrk[4],g_tisBrk[5],g_tisBrk[6]);
#if NOODS_GBA_ISLAND_VERIFY
    { extern int g_tisVerifyFails; extern uint32_t g_tisFirstPc, g_tisFirstOp, g_tisFirstDepth, g_tisLastPc, g_tisLastOp;
      extern char g_tisFirstWhy[16], g_tisLastWhy[16];
      extern uint32_t g_tisFirstI0, g_tisFirstI1, g_tisFirstP0, g_tisFirstP1, g_tisFirstR15, g_tisFirstIPc;
      fprintf(f, "GBA_TISLAND_VERIFY fails=%d first=%s pc=%08X op=%04X depth=%u last=%s pc=%08X op=%04X\n",
        g_tisVerifyFails, g_tisFirstWhy, g_tisFirstPc, g_tisFirstOp, g_tisFirstDepth,
        g_tisLastWhy, g_tisLastPc, g_tisLastOp);
      extern uint32_t g_tisRegIdx, g_tisRegIsl, g_tisRegRef, g_tisCpsrIsl, g_tisCpsrRef;
      fprintf(f, "GBA_TISLAND_PIPE isl=[%04X,%04X] ref=[%04X,%04X] r15=%08X pcOff=%u\n",
        g_tisFirstI0, g_tisFirstI1, g_tisFirstP0, g_tisFirstP1, g_tisFirstR15, g_tisFirstIPc);
      fprintf(f, "GBA_TISLAND_REG idx=%u isl=%08X ref=%08X cpsr isl=%08X ref=%08X\n",
        g_tisRegIdx, g_tisRegIsl, g_tisRegRef, g_tisCpsrIsl, g_tisCpsrRef); }
#endif
}
#endif
#endif
#if NOODS_GBA_ROM_ISLAND
#if NOODS_GBA_ISLAND_STATS
uint32_t g_loopIters=0, g_loopThumb=0, g_loopOpcode=0;
void dumpGbaIslandStats(FILE *f) {
    if (f) fprintf(f, "GBA_LOOP iters=%u thumb=%u runGbaOpcode=%u\n", g_loopIters, g_loopThumb, g_loopOpcode);
    if (!f) return;
    fprintf(f, "GBA_ISLAND entries=%u false_entries=%u instrs=%u cats alu=%u false=%u mul=%u branch=%u xfer=%u block=%u\n",
        g_islEntries,g_islFalseEntry,g_islInstrs,g_islCat[0],g_islCat[1],g_islCat[2],g_islCat[3],g_islCat[4],g_islCat[5]);
    fprintf(f, "GBA_ISLAND_BREAKS cond2=%u page=%u region=%u margin=%u unhandled=%u noprog=%u gpio=%u\n",
        g_islBrk[0],g_islBrk[1],g_islBrk[2],g_islBrk[3],g_islBrk[4],g_islBrk[5],g_islBrk[6]);
    fprintf(f, "GBA_ISLAND_WHY thumb=%u notrom=%u unaligned=%u nopc=%u desync=%u margin=%u\n",
        g_islWhy[0],g_islWhy[1],g_islWhy[2],g_islWhy[3],g_islWhy[4],g_islWhy[5]);
}
#endif
#endif
#if NOODS_JIT && NOODS_JIT_BLOCKS
// v1p171: relaxed GBA timing, opt-in via Settings::gbaFastTiming (off by default).
// Kept out of line on purpose: inlining this loop into runCoreSingle cost the exact
// path ~3% (LTO re-decides inlining for the whole hot function), so it lives here and
// the exact path keeps v1p170's code shape.
void __attribute__((noinline)) Interpreter::runGbaFastTiming(Interpreter &arm, Core &core) {
    const bool jitOn = Settings::armJit != 0;
    ArmPpcJit::linkMode = 4u; // v1.120
    while (core.events.frontCycles > arm.cycles) {
        const uint32_t pcNow = arm.registersUsr[15];
        const uint32_t thumbNow = arm.cpsr & 0x20u;
        int c = 0;
        if (__builtin_expect(jitOn, 1))
            c = ArmPpcJit::fastDispatch(&arm, &core, true, 0u, [&](bool) {
                return core.events.frontCycles - arm.cycles; });
        if (__builtin_expect(c == 0, 0)) {
            uint32_t rem = core.events[0].cycles - arm.cycles;
            c = arm.runOpcode(rem, rem);
        }
        const uint32_t region = pcNow >> 24;
        if (region >= 8u) c += thumbNow ? c : (c << 1);          // cartridge ROM
        else if (region == 2u) c *= thumbNow ? 3 : 6;            // external work RAM
        arm.cycles = (core.globalCycles += c);
    }
}
#endif
template void Interpreter::runCoreSingle<false, 0>(Core &core);
template void Interpreter::runCoreSingle<true, 0>(Core &core);
template void Interpreter::runCoreSingle<true, 1>(Core &core);
template <bool _arm7, int shift> void Interpreter::runCoreSingle(Core &core) {
    // Run the core with one active CPU
    Interpreter &arm = core.interpreter[_arm7];
    while (true) {
        // v1.116: the flag is only read here in the common case; interrupts are masked only
        // when a stop request is actually taken (this runs once per scheduler event).
        if (__builtin_expect(!core.running, 0)) {
            PPCIrqState st = PPCIrqLockByMsr();
            core.running = true;
            PPCIrqUnlockByMsr(st);
            break;
        }

#if NOODS_NDS_TASK_PROFILE
        const unsigned profileMode = core.gbaMode ? 2u : (_arm7 ? 1u : 0u);
        const uint64_t singleCpuStart = PPCGetTickCount();
#endif
        // Run a CPU until the next scheduled task
        core.globalCycles = std::max(core.globalCycles, arm.cycles);
#if NOODS_JIT && NOODS_JIT_BLOCKS
        ArmPpcJit::linkMode = 0u; // v1.120: set again by the loop that supports block links
#endif
#if NOODS_JIT && NOODS_JIT_BLOCKS
        if (__builtin_expect(Settings::gbaFastTiming && _arm7 && core.gbaMode && shift == 0, 0))
            runGbaFastTiming(arm, core);
        else
#endif
        if (_arm7 && core.gbaMode) {
            while (core.events.frontCycles > arm.cycles) {
#if NOODS_GBA_ISLAND_STATS
                ++g_loopIters;
                if (arm.isThumb()) ++g_loopThumb;
#endif
                // Cheap inline mode gate: the islands are mode-specific, and
                // nearly all GBA titles run THUMB code, so an unguarded ARM-only
                // island would cost a function call per THUMB instruction.
#if NOODS_GBA_THUMB_ISLAND
                if(arm.isThumb()) { if(arm.runGbaThumbIsland()) continue; }
                else
#endif
#if NOODS_GBA_ROM_ISLAND
                if(!arm.isThumb() && arm.runGbaRomIsland()) continue;
#if NOODS_GBA_THUMB_ISLAND
                else
#endif
#endif
#if NOODS_GBA_THUMB_ISLAND && !NOODS_GBA_ROM_ISLAND
                else
#endif
                { }
#if NOODS_GBA_DEDICATED
                if(arm.runGbaDedicated()) continue;
#endif
#if NOODS_GBA_ARM_BURST
                if(arm.runGbaArmBurst()) continue;
#endif
#if NOODS_JIT && NOODS_JIT_BLOCKS && NOODS_GBA_JIT
                if (Settings::armJit && arm.pcData && core.gbaBus.nextSeq &&
                    ArmPpcJit::tryExecuteGbaFast(&arm, core.memory.getWaitCnt(), core.events[0].cycles))
                    continue;
#endif
#if NOODS_GBA_ISLAND_STATS
                ++g_loopOpcode;
#endif
                arm.runGbaOpcode();
            }
        }
        else {
#if NOODS_JIT && NOODS_JIT_BLOCKS
            // <true, 0> is the GBA instantiation; its NDS branch is never taken, and is kept
            // as it was so the GBA loop's code generation does not change.
            if (_arm7 && shift == 0) {
                while (core.events[0].cycles > arm.cycles) {
                    uint32_t rem = (core.events[0].cycles - arm.cycles + shift) >> shift;
                    arm.cycles = (core.globalCycles += arm.runOpcode(rem, rem) << shift);
                }
            }
            else {
            // v1.101: try the inline block-hit path first; fall back to the unchanged slow path.
            const bool jitOn = Settings::armJit != 0;
            ArmPpcJit::linkMode = _arm7 ? 3u : 1u; // v1.120
            while (core.events.frontCycles > arm.cycles) {
                int c = 0;
                if (__builtin_expect(jitOn, 1))
                    c = ArmPpcJit::fastDispatch(&arm, &core, _arm7, (uint32_t)shift, [&](bool) {
                        return (core.events.frontCycles - arm.cycles + shift) >> shift; });
                if (__builtin_expect(c == 0, 0)) {
                    uint32_t rem = (core.events[0].cycles - arm.cycles + shift) >> shift;
                    c = arm.runOpcode(rem, rem);
                }
                arm.cycles = (core.globalCycles += c << shift);
            }
            }
#else
            while (core.events[0].cycles > arm.cycles) {
                uint32_t rem = (core.events[0].cycles - arm.cycles + shift) >> shift;
                arm.cycles = (core.globalCycles += arm.runOpcode(rem, rem) << shift);
            }
#endif
        }

#if NOODS_NDS_TASK_PROFILE
        singleCpuTicks[profileMode] += PPCGetTickCount() - singleCpuStart;
#endif
        // Run all tasks scheduled up to the current timestamp, keeping mid-instruction GBA DMAs for runGbaOpcode Step 2!
        uint32_t initialTarget = core.events[0].cycles;
        uint32_t limit = std::max(initialTarget, arm.halted ? initialTarget : arm.cycles);
        while (core.events[0].cycles <= limit) {
            SchedEvent ev = core.events[0];
            if (core.gbaMode && !Settings::gbaFastTiming && ev.cycles > initialTarget &&
                ev.task >= DMA7_TRANSFER0 && ev.task <= DMA7_TRANSFER3) {
                break;
            }
            core.events.popFront();
            core.globalCycles = ev.cycles;
#if NOODS_JIT && NOODS_JIT_POLL_SKIP
            ArmPpcJit::pollEpoch++;
#endif
#if NOODS_NDS_TASK_PROFILE
            const uint64_t singleTaskStart = PPCGetTickCount();
            core.tasks[ev.task]();
            singleTaskTicks[profileMode][ev.task] += PPCGetTickCount() - singleTaskStart;
            ++singleTaskCalls[profileMode][ev.task];
#else
            core.tasks[ev.task]();
#endif
            if (ev.task == RESET_CYCLES) {
                initialTarget = 0;
                limit = arm.halted ? 0u : arm.cycles;
            }
            if (!arm.halted && arm.cycles > limit)
                limit = arm.cycles;
        }
        if (!arm.halted)
            core.globalCycles = std::max(core.globalCycles, arm.cycles);
    }
}

#if NOODS_NDS_TASK_PROFILE
// Diagnostic only: aggregate PPC time in CPU scheduling and each scheduled task.
static uint64_t ndsCpuTicks;
static uint64_t ndsTaskTicks[MAX_TASKS];
static uint32_t ndsTaskCalls[MAX_TASKS];

void Interpreter::dumpNdsTaskProfile(FILE *f) {
#if NOODS_GBA_ARM_BURST
    fprintf(f,"GBA_BURST runs=%u instructions=%u alu=%u memory=%u\n",gbaBurstCounts[0],gbaBurstCounts[1],gbaBurstCounts[2],gbaBurstCounts[3]);
#endif


    for (unsigned mode = 0; mode < 3; ++mode) {
        fprintf(f, "SINGLE_CPU mode=%u us=%llu\n", mode, (unsigned long long)PPCTicksToUs(singleCpuTicks[mode]));
        for (unsigned task = 0; task < MAX_TASKS; ++task) {
            if (singleTaskCalls[mode][task])
                fprintf(f, "SINGLE_TASK mode=%u id=%u count=%u us=%llu\n", mode, task, singleTaskCalls[mode][task], (unsigned long long)PPCTicksToUs(singleTaskTicks[mode][task]));
        }
    }
    fprintf(f, "NDS_CPU us=%llu\n", (unsigned long long)PPCTicksToUs(ndsCpuTicks));
    for (unsigned i = 0; i < MAX_TASKS; ++i) {
        if (ndsTaskCalls[i])
            fprintf(f, "NDS_TASK id=%u count=%u us=%llu\n", i, ndsTaskCalls[i],
                    (unsigned long long)PPCTicksToUs(ndsTaskTicks[i]));
    }
}
#endif

void Interpreter::runCoreNds(Core &core) {
    // Run the core with both CPUs active in NDS mode
    Interpreter &arm9 = core.interpreter[0];
    Interpreter &arm7 = core.interpreter[1];
    while (true) {
        // v1.116: the flag is only read here in the common case; interrupts are masked only
        // when a stop request is actually taken (this runs once per scheduler event).
        if (__builtin_expect(!core.running, 0)) {
            PPCIrqState st = PPCIrqLockByMsr();
            core.running = true;
            PPCIrqUnlockByMsr(st);
            break;
        }

        // Run the ARM9 and half-speed ARM7 until the next scheduled task
#if NOODS_NDS_TASK_PROFILE
        uint64_t cpuStart = PPCGetTickCount();
#endif
#if NOODS_JIT && NOODS_JIT_BLOCKS
        const bool jitOn = Settings::armJit != 0;
#endif
#if NOODS_JIT && NOODS_JIT_BLOCKS
        ArmPpcJit::linkMode = 2u; // v1.120
#endif
        while (core.events.frontCycles > core.globalCycles) {
            const uint32_t evTarget = core.events.frontCycles;
#if NOODS_JIT && NOODS_JIT_BLOCKS
            ArmPpcJit::linkEvTarget = evTarget;
#endif
            if (core.globalCycles >= arm9.cycles) {
#if NOODS_JIT && NOODS_JIT_BLOCKS
                // v1.101: the remaining-cycle bounds are only needed by loop blocks and by the
                // slow path, so they are computed lazily.
                auto remIndep9 = [&]() -> uint32_t { return evTarget - core.globalCycles; };
                auto remShared9 = [&]() -> uint32_t {
                    uint32_t safeEnd9 = arm7.halted ? evTarget : std::min<uint32_t>(evTarget, std::max<uint32_t>(core.globalCycles + 1u, arm7.cycles));
                    return safeEnd9 - core.globalCycles;
                };
                int c9 = 0;
                if (__builtin_expect(jitOn, 1))
                    c9 = ArmPpcJit::fastDispatch(&arm9, &core, false, 0u, [&](bool indep) {
                        return indep ? remIndep9() : remShared9(); });
                if (__builtin_expect(c9 == 0, 0))
                    c9 = arm9.runOpcode(remIndep9(), remShared9());
                arm9.cycles = core.globalCycles + c9;
#else
                uint32_t remIndep9 = evTarget - core.globalCycles;
                uint32_t safeEnd9 = arm7.halted ? evTarget : std::min<uint32_t>(evTarget, std::max<uint32_t>(core.globalCycles + 1u, arm7.cycles));
                uint32_t remShared9 = safeEnd9 - core.globalCycles;
                arm9.cycles = core.globalCycles + arm9.runOpcode(remIndep9, remShared9);
#endif
            }
            if (core.globalCycles >= arm7.cycles) {
#if NOODS_JIT && NOODS_JIT_BLOCKS
                auto remIndep7 = [&]() -> uint32_t { return (evTarget - core.globalCycles + 1u) >> 1; };
                auto remShared7 = [&]() -> uint32_t {
                    uint32_t safeEnd7 = arm9.halted ? evTarget : std::min<uint32_t>(evTarget, std::max<uint32_t>(core.globalCycles + 2u, arm9.cycles));
                    return (safeEnd7 - core.globalCycles + 1u) >> 1;
                };
                int c7 = 0;
                if (__builtin_expect(jitOn, 1))
                    c7 = ArmPpcJit::fastDispatch(&arm7, &core, true, 1u, [&](bool indep) {
                        return indep ? remIndep7() : remShared7(); });
                if (__builtin_expect(c7 == 0, 0))
                    c7 = arm7.runOpcode(remIndep7(), remShared7());
                arm7.cycles = core.globalCycles + (c7 << 1);
#else
                uint32_t remIndep7 = (evTarget - core.globalCycles + 1u) >> 1;
                uint32_t safeEnd7 = arm9.halted ? evTarget : std::min<uint32_t>(evTarget, std::max<uint32_t>(core.globalCycles + 2u, arm9.cycles));
                uint32_t remShared7 = (safeEnd7 - core.globalCycles + 1u) >> 1;
                arm7.cycles = core.globalCycles + (arm7.runOpcode(remIndep7, remShared7) << 1);
#endif
            }
            core.globalCycles = std::min<uint32_t>(arm9.cycles, arm7.cycles);
        }

#if NOODS_NDS_TASK_PROFILE
        ndsCpuTicks += PPCGetTickCount() - cpuStart;
#endif
        // Jump to the next task and run all that are scheduled now
        uint32_t target = core.events[0].cycles;
        core.globalCycles = target;
        while (core.events[0].cycles <= target) {
            SchedEvent ev = core.events[0];
            core.events.popFront();
#if NOODS_JIT && NOODS_JIT_POLL_SKIP
            ArmPpcJit::pollEpoch++;
#endif
#if NOODS_NDS_TASK_PROFILE
            uint64_t taskStart = PPCGetTickCount();
            core.tasks[ev.task]();
            ndsTaskTicks[ev.task] += PPCGetTickCount() - taskStart;
            ndsTaskCalls[ev.task]++;
#else
            core.tasks[ev.task]();
#endif
            if (ev.task == RESET_CYCLES)
                target = 0;
        }
    }
}

void Interpreter::runCoreDsi(Core &core) {
    // Run the core in DSi mode
    Interpreter &arm9 = core.interpreter[0];
    Interpreter &arm7 = core.interpreter[1];
    while (true) {
        // v1.116: the flag is only read here in the common case; interrupts are masked only
        // when a stop request is actually taken (this runs once per scheduler event).
        if (__builtin_expect(!core.running, 0)) {
            PPCIrqState st = PPCIrqLockByMsr();
            core.running = true;
            PPCIrqUnlockByMsr(st);
            break;
        }

        // Run both CPUs until the next scheduled task
#if NOODS_JIT && NOODS_JIT_BLOCKS
        ArmPpcJit::linkMode = 0u; // v1.120: no block links in DSi mode
#endif
        while (core.events[0].cycles > core.globalCycles) {
            // Run the ARM9 twice as fast as usual
            if (core.globalCycles >= arm9.cycles) {
                int cycles = arm9.runOpcode() + arm9.dsiCycle;
                arm9.cycles = core.globalCycles + (cycles >> 1);
                arm9.dsiCycle = (cycles & 0x1);
            }

            // Run the ARM7 at half speed and advance to the next opcode cycle
            if (core.globalCycles >= arm7.cycles)
                arm7.cycles = core.globalCycles + (arm7.runOpcode() << 1);
            core.globalCycles = std::min<uint32_t>(arm9.cycles, arm7.cycles);
        }

        // Jump to the next task and run all that are scheduled now
        uint32_t target = core.events[0].cycles;
        core.globalCycles = target;
        while (core.events[0].cycles <= target) {
            SchedEvent ev = core.events[0];
            core.events.popFront();
#if NOODS_JIT && NOODS_JIT_POLL_SKIP
            ArmPpcJit::pollEpoch++;
#endif
            core.tasks[ev.task]();
            if (ev.task == RESET_CYCLES)
                target = 0;
        }
    }
}

#ifndef NOODS_MPU_PURE_FAST
#define NOODS_MPU_PURE_FAST 0
#endif
uint32_t g_mpuPureFastStats[3]; // arm pure, thumb pure, condition false

// Conservative instruction classes that cannot access DS data memory and do
// not change PC, modes or status-register banking. Unrecognized forms retain
// the original checked path. Instruction permission is checked separately.
FORCE_INLINE int Interpreter::runOpcode(uint32_t remIndep, uint32_t remShared) {
    DISPATCH_STAT(++g_interpCount[arm7 ? 1 : 0]);
    if (!arm7 && __builtin_expect(core->cp15.mpuRestricted, 0)) {
        const uint32_t faultPc = *registers[15] - ((cpsr & BIT(5)) ? 2u : 4u);
        const bool priv = isPrivileged();
        // Fast gate: standard privileged layout (e.g. libnds default MPU) with executable code page
        // needs no per-instruction data check on mapped RAM/IO; only enter slow path on User mode,
        // custom data restrictions, or non-executable PC page.
        if (!priv || !core->cp15.mpuFastPriv ||
            !(core->cp15.getPagePerm(faultPc >> 12) & 0x04)) {
            if (!core->cp15.checkAccessPage(faultPc, false, false, priv))
                return takeAbort(0x0C, faultPc + 4);

#if NOODS_MPU_PURE_FAST
            // The same checked interpreter dispatch with no rollback snapshot
            // is safe only if the instruction provably cannot touch data.
            // Condition-false ARM instructions do not execute at all.
            const uint32_t pureOp = pipeline[0];
            const bool thumbPure = (cpsr & BIT(5)) && mpuPureThumb((uint16_t)pureOp);
            const int armCond = !(cpsr & BIT(5)) ? condition[((pureOp >> 24) & 0xF0) | (cpsr >> 28)] : 2;
            const bool falseArm = !(cpsr & BIT(5)) && armCond == 0;
            const bool armPure = !(cpsr & BIT(5)) && armCond == 1 && mpuPureArm(pureOp);
            if (thumbPure || armPure || falseArm) {
                pipeline[0] = pipeline[1];
                if (cpsr & BIT(5)) {
                    pipeline[1] = (((*registers[15] += 2) & 0xFFE) && pcData) ? U8TO16(pcData += 2, 0) : getOpcode16();
                    MPU_PATH_STAT(++g_mpuPureFastStats[1]);
#if NOODS_MPU_PURE_JIT && NOODS_JIT && NOODS_JIT_BLOCKS
                    if (Settings::armJit) return ArmPpcJit::executeThumb(this, pureOp, remIndep, remShared);
#endif
                    return (this->*thumbInstrs[(pureOp >> 6) & 0x3FF])(pureOp);
                }
                pipeline[1] = (((*registers[15] += 4) & 0xFFC) && pcData) ? U8TO32(pcData += 4, 0) : getOpcode32();
                if (falseArm) { MPU_PATH_STAT(++g_mpuPureFastStats[2]); return 1; }
                MPU_PATH_STAT(++g_mpuPureFastStats[0]);
#if NOODS_MPU_PURE_JIT && NOODS_JIT && NOODS_JIT_BLOCKS
                if (Settings::armJit) return ArmPpcJit::executeArm(this, pureOp, remIndep, remShared);
#endif
                return (this->*armInstrs[((pureOp >> 16) & 0xFF0) | ((pureOp >> 4) & 0xF)])(pureOp);
            }
#endif
#if NOODS_STRICT_SINGLE_PRECHECK
            // Preflight a single immediate-offset ARM load/store. The handler
            // performs exactly one data access at this address, so if that
            // access is permitted there is no possible data abort to roll
            // back. Keep PC operands, writeback, register offsets and all
            // other instruction classes on the original snapshot path.
            // The address passed to checkAccessPage matches Memory::read/write
            // before their alignment behavior, including unsigned wraparound.
            const uint32_t strictOp = pipeline[0];
            if (!(cpsr & BIT(5)) && condition[((strictOp >> 24) & 0xF0) | (cpsr >> 28)] == 1 &&
                (strictOp & 0x0F200000u) == 0x05000000u &&
                ((strictOp >> 16) & 15u) != 15u && ((strictOp >> 12) & 15u) != 15u) {
                const uint32_t base = *registers[(strictOp >> 16) & 15u];
                const uint32_t imm = strictOp & 0xFFFu;
                const uint32_t address = (strictOp & BIT(23)) ? base + imm : base - imm;
                // Unmapped accesses can invoke MMIO and nested DMA operations;
                // keep those on the original checked path as well.
                const bool isWrite = !(strictOp & BIT(20));
                const uint32_t page = address >> 12;
                uint8_t *const dataPage = isWrite ? core->memory.writeMap9A[page] : core->memory.readMap9A[page];
                if (dataPage && core->cp15.checkAccessPage(address, isWrite, true, priv)) {
                    // A mapped, word-sized read has no MMIO effects. When the
                    // next opcode is also a direct pointer fetch, no mapping
                    // can change between the permission check and this load.
                    // Preserve Memory::read's aligned-word and ARM rotation rules.
                    const uint8_t *const data = dataPage + (address & 0xFFCu);
                    if (!isWrite && !(strictOp & BIT(22)) && !core->memory.mpuDataCheckActive &&
                        pcData && ((*registers[15] + 4u) & 0xFFCu) && !((uintptr_t)data & 3u)) {
                        pipeline[0] = pipeline[1];
                        *registers[15] += 4;
                        pipeline[1] = U8TO32(pcData += 4, 0);
                        uint32_t value;
                        __builtin_memcpy(&value, data, 4);
#ifdef ENDIAN_BIG
                        value = __builtin_bswap32(value);
#endif
                        const unsigned shift = (address & 3u) * 8u;
                        if (shift) value = (value >> shift) | (value << (32u - shift));
                        *registers[(strictOp >> 12) & 15u] = value;
                        return 1;
                    }
                    pipeline[0] = pipeline[1];
                    pipeline[1] = (((*registers[15] += 4) & 0xFFC) && pcData) ? U8TO32(pcData += 4, 0) : getOpcode32();
                    return (this->*armInstrs[((strictOp >> 16) & 0xFF0) | ((strictOp >> 4) & 0xF)])(strictOp);
                }
            }
#endif
#if NOODS_STRICT_BRANCH_PRECHECK
            // The checked interpreter's branch refill performs a data-checked
            // read at its target. Only bypass rollback if the target is mapped,
            // both opcodes fit in the page, and that same data read is allowed.
            // For all other cases preserve the checked refill and abort path.
            const uint32_t brOp = pipeline[0];
            if (!(cpsr & BIT(5)) && condition[((brOp >> 24) & 0xF0) | (cpsr >> 28)] == 1) {
                const bool immBranch = (brOp & 0x0E000000u) == 0x0A000000u;
                const bool regBranch = (brOp & 0x0FFFFFF0u) == 0x012FFF10u && (brOp & 15u) != 15u;
                if (immBranch || regBranch) {
                    const uint32_t target = immBranch ? (*registers[15] + 4u + uint32_t(int32_t(brOp << 8) >> 6)) : *registers[brOp & 15u];
                    const bool thumbTarget = regBranch && (target & 1u);
                    const uint32_t alignedTarget = target & (thumbTarget ? ~1u : ~3u);
                    const uint32_t offset = alignedTarget & 0xFFFu;
                    uint8_t *const targetPage = core->memory.readMap9A[alignedTarget >> 12];
                    if (offset <= (thumbTarget ? 0xFFCu : 0xFF8u) && targetPage &&
                        core->cp15.checkAccessPage(alignedTarget, false, true, priv)) {
                        // With direct source and target fetches, the sequential
                        // pipeline read that the branch discards has no side
                        // effects. Refill exactly as flushPipeline would.
                        if (!core->gbaMode && !core->memory.mpuDataCheckActive && pcData &&
                            ((*registers[15] + 4u) & 0xFFCu)) {
                            if (immBranch && (brOp & BIT(24))) *registers[14] = *registers[15];
                            if (thumbTarget) cpsr |= BIT(5);
                            uint8_t *const targetData = targetPage + offset;
                            if (thumbTarget) {
                                *registers[15] = alignedTarget + 2;
                                pipeline[0] = U8TO16(targetData, 0);
                                pipeline[1] = U8TO16(targetData, 2);
                                pcData = targetData + 2;
                            } else {
                                *registers[15] = alignedTarget + 4;
                                pipeline[0] = U8TO32(targetData, 0);
                                pipeline[1] = U8TO32(targetData, 4);
                                pcData = targetData + 4;
                            }
                            return 3;
                        }
                        pipeline[0] = pipeline[1];
                        pipeline[1] = (((*registers[15] += 4) & 0xFFC) && pcData) ? U8TO32(pcData += 4, 0) : getOpcode32();
                        return (this->*armInstrs[((brOp >> 16) & 0xFF0) | ((brOp >> 4) & 0xF)])(brOp);
                    }
                }
            }
#endif
            uint32_t savedRegs[15];
            for (int i = 0; i < 15; i++)
                savedRegs[i] = *registers[i];

            uint32_t opcode = pipeline[0];
            pipeline[0] = pipeline[1];
            int cyc;
            if (cpsr & BIT(5)) {
                pipeline[1] = (((*registers[15] += 2) & 0xFFE) && pcData) ? U8TO16(pcData += 2, 0) : getOpcode16();
                core->memory.mpuDataCheckActive = true;
                dataAbort = false;
                cyc = (this->*thumbInstrs[(opcode >> 6) & 0x3FF])(opcode);
                core->memory.mpuDataCheckActive = false;
            }
            else {
                pipeline[1] = (((*registers[15] += 4) & 0xFFC) && pcData) ? U8TO32(pcData += 4, 0) : getOpcode32();
                switch (condition[((opcode >> 24) & 0xF0) | (cpsr >> 28)]) {
                    case 0: cyc = 1; break;
                    case 2:
                        core->memory.mpuDataCheckActive = true; dataAbort = false;
                        cyc = handleReserved(opcode);
                        core->memory.mpuDataCheckActive = false;
                        break;
                    default:
                        core->memory.mpuDataCheckActive = true; dataAbort = false;
                        cyc = (this->*armInstrs[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0xF)])(opcode);
                        core->memory.mpuDataCheckActive = false;
                        break;
                }
            }
            if (dataAbort) {
                dataAbort = false;
                for (int i = 0; i < 15; i++)
                    *registers[i] = savedRegs[i];
                return takeAbort(0x10, faultPc + 8);
            }
            return cyc;
        }
    }

    // Push the next opcode through the pipeline
    uint32_t opcode = pipeline[0];
    pipeline[0] = pipeline[1];

    int cyc;
    // Execute an instruction
    if (cpsr & BIT(5)) { // THUMB mode
        // Increment the program counter and fill the pipeline from pointer or fallback
        pipeline[1] = (((*registers[15] += 2) & 0xFFE) && pcData) ? U8TO16(pcData += 2, 0) : getOpcode16();

        // Execute a THUMB instruction
#if NOODS_JIT
        cyc = Settings::armJit ? ArmPpcJit::executeThumb(this, opcode, remIndep, remShared)
                               : (this->*thumbInstrs[(opcode >> 6) & 0x3FF])(opcode);
#else
        cyc = (this->*thumbInstrs[(opcode >> 6) & 0x3FF])(opcode);
#endif
    }
    else { // ARM mode
        // Increment the program counter and fill the pipeline from pointer or fallback
        pipeline[1] = (((*registers[15] += 4) & 0xFFC) && pcData) ? U8TO32(pcData += 4, 0) : getOpcode32();

        // Execute an ARM instruction based on its condition
        switch (condition[((opcode >> 24) & 0xF0) | (cpsr >> 28)]) {
            case 0: cyc = 1; break; // False
            case 2: cyc = handleReserved(opcode); break; // Reserved
#if NOODS_JIT
            default: cyc = Settings::armJit ? ArmPpcJit::executeArm(this, opcode, remIndep, remShared)
                                            : (this->*armInstrs[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0xF)])(opcode); break;
#else
            default: cyc = (this->*armInstrs[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0xF)])(opcode); break;
#endif
        }
    }
    if (arm7 && core->gbaMode)
        core->memory.clearGbaDmaOpenBus();
    return cyc;
}

void Interpreter::advanceGbaCycles(int c) {
    cycles = (core->globalCycles += c);
}

void Interpreter::stallGbaDmaCycles(uint32_t c) {
    if (!halted) {
        cycles = std::max(cycles, core->globalCycles) + c;
    }
}

#if NOODS_GBA_ARM_BURST >= 3 || NOODS_GBA_DEDICATED
__attribute__((noinline))
#endif
void Interpreter::runGbaOpcode() {
    uint32_t opcode = pipeline[0];
    pipeline[0] = pipeline[1];
    uint16_t waitCnt = core->memory.getWaitCnt();
    bool thumb = (cpsr & BIT(5)) != 0;

    // 1. Instruction fetch phase (pipeline[1])
    if (thumb) {
        pipeline[1] = (((*registers[15] += 2) & 0xFFE) && pcData) ? U8TO16(pcData += 2, 0) : getOpcode16();
#if NOODS_GBA_FETCH_FAST
        int fFast = core->gbaBus.fetchThumbRomSeq(*registers[15], waitCnt);
        int fCyc = (fFast >= 0) ? fFast : core->gbaBus.fetchOpcode(*registers[15], false, waitCnt);
#else
        int fCyc = core->gbaBus.fetchOpcode(*registers[15], false, waitCnt);
#endif
        cycles = (core->globalCycles += fCyc);
    }
    else {
        pipeline[1] = (((*registers[15] += 4) & 0xFFC) && pcData) ? U8TO32(pcData += 4, 0) : getOpcode32();
        int fCyc = core->gbaBus.fetchOpcode(*registers[15], true, waitCnt);
        cycles = (core->globalCycles += fCyc);
    }

    bool usedDuty = core->gbaBus.lastFetchUsedDuty;

    // 2. Allow any pending GBA DMA transfer ready by end of fetch to preempt bus before data phase,
    //    even if a scanline/audio task sits earlier in core->events!
    if (!halted && core->events.frontCycles <= cycles) {
        for (size_t idx = 0; idx < core->events.size() && core->events[idx].cycles <= cycles; ) {
            SchedTask t = core->events[idx].task;
            if (t >= DMA7_TRANSFER0 && t <= DMA7_TRANSFER3) {
                core->events.eraseAt(idx);
                core->globalCycles = cycles;
                core->tasks[t]();
                core->globalCycles = cycles;
                int sWait = GbaBus::romWaitS(*registers[15], waitCnt);
                if (usedDuty && ((core->dma[1].firstPakWasWrite && sWait == 3) ||
                                 (core->dma[1].firstPakWasRead && !(cpsr & BIT(5)) && sWait == 2))) {
                    cycles = (core->globalCycles += 1);
                    usedDuty = false;
                }
                idx = 0; // Restart scan in case higher-priority DMA became ready
                continue;
            }
            idx++;
        }
        waitCnt = core->memory.getWaitCnt();
    }

    // 3. Execute instruction (data accesses update cycles directly on bus read/write!)
    core->gbaBus.beginInstruction();
    core->memory.gbaCpuExecActive = true;
    int baseCyc;
    if (thumb) {
        baseCyc = (this->*thumbInstrs[(opcode >> 6) & 0x3FF])(opcode);
    }
    else {
        switch (condition[((opcode >> 24) & 0xF0) | (cpsr >> 28)]) {
            case 0: baseCyc = 1; break;
            case 2: baseCyc = handleReserved(opcode); break;
            default: baseCyc = (this->*armInstrs[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0xF)])(opcode); break;
        }
    }
    core->memory.gbaCpuExecActive = false;
    if (halted) {
        core->memory.clearGbaDmaOpenBus();
        return;
    }
    waitCnt = core->memory.getWaitCnt();

    // 4. Trailing internal cycles (1I after loads, multiply/shift internal ticks)
    if (core->gbaBus.branched) {
        // Note: if LDR PC / POP {PC} loaded from memory before branching, add 1I
        if (core->gbaBus.hadLoad) {
            int iCyc = core->gbaBus.stepInternal(1, waitCnt);
            cycles = (core->globalCycles += iCyc);
        }
    }
    else if (core->gbaBus.hadDataAccess) {
        if (core->gbaBus.hadLoad) {
            int iCyc = core->gbaBus.stepInternal(1, waitCnt);
            cycles = (core->globalCycles += iCyc);
        }
    }
    else {
        int internal = baseCyc - 1;
        if (internal > 0) {
            int iCyc = core->gbaBus.stepInternal(internal, waitCnt);
            cycles = (core->globalCycles += iCyc);
        }
    }

    core->memory.clearGbaDmaOpenBus();
}

uint16_t Interpreter::getOpcode16() {
    // Set the opcode pointer or fall back to a regular 16-bit opcode read
    if (!(pcData = (arm7 ? core->memory.readMap7 : core->memory.readMap9A)[*registers[15] >> 12])) {
        bool prev = core->memory.gbaCpuExecActive;
        core->memory.gbaCpuExecActive = false;
        uint16_t op = core->memory.read<uint16_t>(arm7, *registers[15]);
        core->memory.gbaCpuExecActive = prev;
        return op;
    }
    pcData += (*registers[15] & 0xFFE);
    return U8TO16(pcData, 0);
}

uint32_t Interpreter::getOpcode32() {
    // Set the opcode pointer or fall back to a regular 32-bit opcode read
    if (!(pcData = (arm7 ? core->memory.readMap7 : core->memory.readMap9A)[*registers[15] >> 12])) {
        bool prev = core->memory.gbaCpuExecActive;
        core->memory.gbaCpuExecActive = false;
        uint32_t op = core->memory.read<uint32_t>(arm7, *registers[15]);
        core->memory.gbaCpuExecActive = prev;
        return op;
    }
    pcData += (*registers[15] & 0xFFC);
    return U8TO32(pcData, 0);
}

void Interpreter::halt(int bit) {
    // Set a halt bit and disable the CPU if newly halted
    bool before = halted;
    halted |= BIT(bit);
    if (before) return;
    core->schedule(UPDATE_RUN, 0);
    cycles = 0xFFFFFFFF;
}

void Interpreter::unhalt(int bit) {
    // Clear a halt bit and enable the CPU if newly halted
    bool before = halted;
    halted &= ~BIT(bit);
    if (!before) return;
    core->schedule(UPDATE_RUN, 0);
    cycles = 0;
}

void Interpreter::sendInterrupt(int bit) {
    // Set the interrupt's request bit
    irf |= BIT(bit);

    // Trigger an interrupt if the conditions are met, or unhalt the CPU even if interrupts are disabled
    // The ARM9 additionally needs IME to be set for it to unhalt, but the ARM7 doesn't care
    if (ie & irf) {
        if (ime && !(cpsr & BIT(7)))
            core->schedule(SchedTask(ARM9_INTERRUPT + arm7), core->gbaMode ? 3 : ((arm7 && !core->gbaMode) + 1));
        else if (ime || arm7)
            unhalt(0);
    }
}

void Interpreter::interrupt() {
    // Trigger an interrupt and unhalt the CPU if the conditions still hold
    if (ime && (ie & irf) && !(cpsr & BIT(7))) {
        uint32_t irqStart = halted ? core->globalCycles : std::max(core->globalCycles, cycles);
        int cyc = exception(0x18);
        unhalt(0);
        if (core->gbaMode)
            cycles = (core->globalCycles = irqStart + cyc);
    }
}

int Interpreter::takeAbort(uint8_t vector, uint32_t lrVal) {
    setCpsr((cpsr & ~0x3F) | BIT(7) | 0x17, true); // ARM, IRQ off, Abort mode
    *registers[14] = lrVal;
    *registers[15] = (arm7 ? 0 : core->cp15.exceptionAddr) + vector;
    flushPipeline();
    return 3;
}

int Interpreter::exception(uint8_t vector) {
    // Forward the call to HLE BIOS if enabled, unless on ARM9 with the exception address changed
    if (bios && (arm7 || core->cp15.exceptionAddr))
        return bios->execute(vector, registers);

    // Switch the CPU mode, save the return address, and jump to the exception vector
    static const uint8_t modes[] = { 0x13, 0x1B, 0x13, 0x17, 0x17, 0x13, 0x12, 0x11 };
    setCpsr((cpsr & ~0x3F) | BIT(7) | modes[vector >> 2], true); // ARM, interrupts off, new mode
    *registers[14] = *registers[15] + ((*spsr & BIT(5)) >> 4);
    *registers[15] = (arm7 ? 0 : core->cp15.exceptionAddr) + vector;
    flushPipeline();
    return 3;
}

int Interpreter::undefinedException(bool thumb) {
    // Match Dust's ARM7/ARM9 undefined-instruction entry: preserve the old CPSR
    // in the Undefined SPSR bank, clear Thumb state, disable IRQs, and branch to +4.
    setCpsr((cpsr & ~0x3F) | BIT(7) | 0x1B, true);
    *registers[14] = *registers[15] - (thumb ? 2 : 4);
    *registers[15] = (arm7 ? 0 : core->cp15.exceptionAddr) | 0x04;
    flushPipeline();
    return 3;
}

void Interpreter::flushPipeline() {
    // v1.118: the usual case - a DS CPU jumping to code in a mapped page, with both pipeline
    // words in that page - is served from one page-table lookup. It leaves exactly the state
    // the general code below does (R15, pipeline, pcData).
    if (__builtin_expect(!core->gbaMode && !core->memory.mpuDataCheckActive, 1)) {
        uint8_t *const page = (arm7 ? core->memory.readMap7 : core->memory.readMap9A)[*registers[15] >> 12];
        if (__builtin_expect(page != nullptr, 1)) {
            if (cpsr & BIT(5)) {
                const uint32_t targetPc = *registers[15] & ~0x1u;
                if (__builtin_expect((targetPc & 0xFFE) != 0xFFE, 1)) {
                    const uint8_t *p = page + (targetPc & 0xFFE);
                    *registers[15] = targetPc + 2;
                    pipeline[0] = U8TO16(p, 0);
                    pipeline[1] = U8TO16(p, 2);
                    pcData = const_cast<uint8_t*>(p) + 2;
                    return;
                }
            } else {
                const uint32_t targetPc = *registers[15] & ~0x3u;
                if (__builtin_expect((targetPc & 0xFFC) != 0xFFC, 1)) {
                    const uint8_t *p = page + (targetPc & 0xFFC);
                    *registers[15] = targetPc + 4;
                    pipeline[0] = U8TO32(p, 0);
                    pipeline[1] = U8TO32(p, 4);
                    pcData = const_cast<uint8_t*>(p) + 4;
                    return;
                }
            }
        }
    }
    bool prevExec = core->memory.gbaCpuExecActive;
    core->memory.gbaCpuExecActive = false;
    uint32_t targetPc;
    // Adjust the program counter and refill the pipeline after a jump
    if (cpsr & BIT(5)) { // THUMB mode
        targetPc = *registers[15] & ~0x1u;
        *registers[15] = targetPc + 2;
        pipeline[0] = core->memory.read<uint16_t>(arm7, targetPc);
        pipeline[1] = getOpcode16();
    }
    else { // ARM mode
        targetPc = *registers[15] & ~0x3u;
        *registers[15] = targetPc + 4;
        pipeline[0] = core->memory.read<uint32_t>(arm7, targetPc);
        pipeline[1] = getOpcode32();
    }
    core->memory.gbaCpuExecActive = prevExec;
    if (prevExec && arm7 && core->gbaMode) {
        int refill = core->gbaBus.branchRefill(targetPc, !(cpsr & BIT(5)), core->memory.getWaitCnt());
        cycles = (core->globalCycles += refill);
    }
}

void Interpreter::swapRegisters(uint32_t value) {
    // Swap banked registers based on a CPU mode value
    switch (value & 0x0F) {
    case 0x00: // User
    case 0x04: // Reserved: System register bank, Abort SPSR bank
    case 0x05:
    case 0x06:
    case 0x08: // Reserved: System register bank, Undefined SPSR bank
    case 0x09:
    case 0x0A:
    case 0x0C: // Reserved and System modes share the System register bank
    case 0x0D:
    case 0x0E:
    case 0x0F: // System
        registers[8] = &registersUsr[8];
        registers[9] = &registersUsr[9];
        registers[10] = &registersUsr[10];
        registers[11] = &registersUsr[11];
        registers[12] = &registersUsr[12];
        registers[13] = &registersUsr[13];
        registers[14] = &registersUsr[14];
        if ((value & 0x0F) >= 0x04 && (value & 0x0F) <= 0x06)
            spsr = &spsrAbt;
        else if ((value & 0x0F) >= 0x08 && (value & 0x0F) <= 0x0A)
            spsr = &spsrUnd;
        else
            spsr = nullptr;
        break;

    case 0x01: // FIQ (mode field bits 3:0)
        registers[8] = &registersFiq[0];
        registers[9] = &registersFiq[1];
        registers[10] = &registersFiq[2];
        registers[11] = &registersFiq[3];
        registers[12] = &registersFiq[4];
        registers[13] = &registersFiq[5];
        registers[14] = &registersFiq[6];
        spsr = &spsrFiq;
        break;

    case 0x02: // IRQ (mode field bits 3:0)
        registers[8] = &registersUsr[8];
        registers[9] = &registersUsr[9];
        registers[10] = &registersUsr[10];
        registers[11] = &registersUsr[11];
        registers[12] = &registersUsr[12];
        registers[13] = &registersIrq[0];
        registers[14] = &registersIrq[1];
        spsr = &spsrIrq;
        break;

    case 0x03: // Supervisor (mode field bits 3:0)
        registers[8] = &registersUsr[8];
        registers[9] = &registersUsr[9];
        registers[10] = &registersUsr[10];
        registers[11] = &registersUsr[11];
        registers[12] = &registersUsr[12];
        registers[13] = &registersSvc[0];
        registers[14] = &registersSvc[1];
        spsr = &spsrSvc;
        break;

    case 0x07: // Abort (mode field bits 3:0)
        registers[8] = &registersUsr[8];
        registers[9] = &registersUsr[9];
        registers[10] = &registersUsr[10];
        registers[11] = &registersUsr[11];
        registers[12] = &registersUsr[12];
        registers[13] = &registersAbt[0];
        registers[14] = &registersAbt[1];
        spsr = &spsrAbt;
        break;

    case 0x0B: // Undefined (mode field bits 3:0)
        registers[8] = &registersUsr[8];
        registers[9] = &registersUsr[9];
        registers[10] = &registersUsr[10];
        registers[11] = &registersUsr[11];
        registers[12] = &registersUsr[12];
        registers[13] = &registersUnd[0];
        registers[14] = &registersUnd[1];
        spsr = &spsrUnd;
        break;

    default:
        LOG_CRIT("Unknown ARM%d CPU mode: 0x%X\n", arm7 ? 7 : 9, value & 0x0F);
        break;
    }
}

void Interpreter::setCpsr(uint32_t value, bool save) {
    // Update registers if the CPU mode changed
    if ((value & 0x0F) != (cpsr & 0x0F))
        swapRegisters(value);

    // Set the CPSR, saving the old value if requested
    if (save && spsr) *spsr = cpsr;
    cpsr = value;

    // Trigger an interrupt if the conditions are met
    if (ime && (ie & irf) && !(cpsr & BIT(7)))
        core->schedule(SchedTask(ARM9_INTERRUPT + arm7), (arm7 && !core->gbaMode) + 1);
}

int Interpreter::handleReserved(uint32_t opcode) {
    // The ARM9-exclusive BLX instruction uses the reserved condition code, so let it run
    if ((opcode & 0xE000000) == 0xA000000)
        return blx(opcode); // BLX label

    // If the special HLE BIOS opcode was jumped to, return from an HLE interrupt
    if (bios && opcode == 0xFF000000)
        return finishHleIrq();

    // If a DLDI function was jumped to, HLE it and return
    if (core->dldi.isPatched()) {
        uint32_t **r = registers;
        switch (opcode) {
            case DLDI_START: *r[0] = core->dldi.startup(); break;
            case DLDI_INSERT: *r[0] = core->dldi.isInserted(); break;
            case DLDI_READ: *r[0] = core->dldi.readSectors(arm7, *r[0], *r[1], *r[2]); break;
            case DLDI_WRITE: *r[0] = core->dldi.writeSectors(arm7, *r[0], *r[1], *r[2]); break;
            case DLDI_CLEAR: *r[0] = core->dldi.clearStatus(); break;
            case DLDI_STOP: *r[0] = core->dldi.shutdown(); break;
        }
        return bx(14);
    }

    // Treat anything else as an unknown opcode
    return unkArm(opcode);
}

int Interpreter::handleHleIrq() {
    // Switch to IRQ mode, save the return address, and push registers to the stack
    setCpsr((cpsr & ~0x3F) | BIT(7) | 0x12, true);
    *registers[14] = *registers[15] + ((*spsr & BIT(5)) ? 2 : 0);
    stmdbW((13 << 16) | BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(12) | BIT(14));

    // Set the return address to the special HLE BIOS opcode amd jump to the interrupt handler
    *registers[14] = arm7 ? 0x00000000 : 0xFFFF0000;
    *registers[15] = core->memory.read<uint32_t>(arm7, arm7 ? 0x3FFFFFC : (core->cp15.dtcmAddr + 0x3FFC));
    flushPipeline();
    return core->gbaMode ? 21 : 3;
}

int Interpreter::finishHleIrq() {
    bool wasIntrWait = bios->shouldCheck();
    // Update the wait flags if in the middle of an HLE IntrWait function
    if (wasIntrWait)
        bios->checkWaitFlags();

    // Pop registers from the stack, jump to the return address, and restore the mode
    ldmiaW((13 << 16) | BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(12) | BIT(14));
    *registers[15] = *registers[14] - 4;
    if (spsr) setCpsr(*spsr);
    flushPipeline();
    if (core->gbaMode) {
        core->memory.setGbaBiosOpenBus(0xE25EF004u);
        if (wasIntrWait && !bios->shouldCheck()) {
            // Returning out of SWI IntrWait / VBlankIntrWait BIOS body
            inGbaHaltSwi = false;
            advanceGbaCycles(42);
        }
        else if (inGbaHaltSwi) {
            // Returning out of SWI Halt BIOS body
            inGbaHaltSwi = false;
            advanceGbaCycles(23);
        }
    }
    return 3;
}

int Interpreter::unkArm(uint32_t opcode) {
    // Unknown ARM encodings enter Undefined mode rather than acting as NOPs.
    (void)opcode;
    return undefinedException(false);
}

int Interpreter::unkThumb(uint16_t opcode) {
    // Unknown Thumb encodings enter the same Undefined vector with a Thumb LR offset.
    (void)opcode;
    return undefinedException(true);
}

void Interpreter::writeIme(uint8_t value) {
    // Write to the IME register
    ime = value & 0x01;

    // Trigger an interrupt if the conditions are met
    if (ime && (ie & irf) && !(cpsr & BIT(7)))
        core->schedule(SchedTask(ARM9_INTERRUPT + arm7), core->gbaMode ? 3 : ((arm7 && !core->gbaMode) + 1));
}

void Interpreter::writeIe(uint32_t mask, uint32_t value) {
    // Write to the IE register
    mask &= (arm7 ? (core->gbaMode ? 0x3FFF : 0x01FF3FFF) : 0x003F3F7F);
    ie = (ie & ~mask) | (value & mask);

    // Trigger an interrupt if the conditions are met
    if (ime && (ie & irf) && !(cpsr & BIT(7)))
        core->schedule(SchedTask(ARM9_INTERRUPT + arm7), core->gbaMode ? 3 : ((arm7 && !core->gbaMode) + 1));
}

void Interpreter::writeIrf(uint32_t mask, uint32_t value) {
    // Write to the IF register
    // Setting a bit actually clears it to acknowledge an interrupt
    irf &= ~(value & mask);
}

void Interpreter::writePostFlg(uint8_t value) {
    // Write to the POSTFLG register
    // The first bit can be set, but never cleared
    // For some reason, the second bit is writable on the ARM9
    postFlg |= value & 0x01;
    if (!arm7) postFlg = (postFlg & ~0x02) | (value & 0x02);
}
