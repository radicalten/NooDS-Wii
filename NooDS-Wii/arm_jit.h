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

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>

class Core;
class Interpreter;

#ifndef NOODS_JIT
#define NOODS_JIT 0
#endif

#ifndef NOODS_JIT_POLL_SKIP
#define NOODS_JIT_POLL_SKIP 0 // Joint fixed-point poll-loop skipping (ARM9 + ARM7); off by default
#endif
#ifndef NOODS_JIT_BLOCKS
#define NOODS_JIT_BLOCKS 0
#endif
#ifndef NOODS_JIT_STATS
#define NOODS_JIT_STATS 1
#endif
extern "C" int jit_enter(void *code, Interpreter *cpu, uint32_t maxIters = 1);
extern "C" void jit_exit(void);
extern "C" int jit_fallback_arm(Interpreter *cpu, uint32_t opcode);
extern "C" int jit_fallback_thumb(Interpreter *cpu, uint32_t opcode);

// Use namespace JitPpc — never PPC (libogc <tuxedo/ppc/intrinsics.h> defines #define PPC).
namespace JitPpc {

enum Reg : uint8_t {
    R0  = 0,  R1  = 1,  R2  = 2,  R3  = 3,
    R4  = 4,  R5  = 5,  R6  = 6,  R7  = 7,
    R8  = 8,  R9  = 9,  R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
    R16 = 16, R17 = 17, R18 = 18, R19 = 19,
    R20 = 20, R21 = 21, R22 = 22, R23 = 23,
    R24 = 24, R25 = 25, R26 = 26, R27 = 27,
    R28 = 28, R29 = 29, R30 = 30, R31 = 31
};

// Generic format builders (PowerPC 750CL / Broadway 32-bit MSB-first encoding)
constexpr inline uint32_t encodeD(uint32_t op, uint8_t rt, uint8_t ra, int16_t simm) {
    return ((op & 0x3Fu) << 26) |
           ((uint32_t)(rt & 0x1Fu) << 21) |
           ((uint32_t)(ra & 0x1Fu) << 16) |
           (uint16_t)simm;
}

constexpr inline uint32_t encodeDU(uint32_t op, uint8_t rs, uint8_t ra, uint16_t uimm) {
    return ((op & 0x3Fu) << 26) |
           ((uint32_t)(rs & 0x1Fu) << 21) |
           ((uint32_t)(ra & 0x1Fu) << 16) |
           (uint32_t)uimm;
}

constexpr inline uint32_t encodeXO(uint8_t rt, uint8_t ra, uint8_t rb, uint32_t xo, bool oe = false, bool rc = false) {
    return (31u << 26) |
           ((uint32_t)(rt & 0x1Fu) << 21) |
           ((uint32_t)(ra & 0x1Fu) << 16) |
           ((uint32_t)(rb & 0x1Fu) << 11) |
           ((oe ? 1u : 0u) << 10) |
           ((xo & 0x1FFu) << 1) |
           (rc ? 1u : 0u);
}

constexpr inline uint32_t encodeX(uint8_t rs_or_rt, uint8_t ra, uint8_t rb, uint32_t xo, bool rc = false) {
    return (31u << 26) |
           ((uint32_t)(rs_or_rt & 0x1Fu) << 21) |
           ((uint32_t)(ra & 0x1Fu) << 16) |
           ((uint32_t)(rb & 0x1Fu) << 11) |
           ((xo & 0x3FFu) << 1) |
           (rc ? 1u : 0u);
}

constexpr inline uint32_t encodeM(uint32_t op, uint8_t rs, uint8_t ra, uint8_t sh, uint8_t mb, uint8_t me, bool rc = false) {
    return ((op & 0x3Fu) << 26) |
           ((uint32_t)(rs & 0x1Fu) << 21) |
           ((uint32_t)(ra & 0x1Fu) << 16) |
           ((uint32_t)(sh & 0x1Fu) << 11) |
           ((uint32_t)(mb & 0x1Fu) << 6) |
           ((uint32_t)(me & 0x1Fu) << 1) |
           (rc ? 1u : 0u);
}

// 1-4: Immediate load / add (D-form; r0 base forbidden on addi/addis)
constexpr inline uint32_t li(uint8_t rd, int16_t simm)    { return encodeD(14u, rd, 0, simm); }
constexpr inline uint32_t lis(uint8_t rd, int16_t simm)   { return encodeD(15u, rd, 0, simm); }
inline uint32_t addi(uint8_t rd, uint8_t ra, int16_t simm)  { assert(ra != 0); return encodeD(14u, rd, ra, simm); }
inline uint32_t addis(uint8_t rd, uint8_t ra, int16_t simm) { assert(ra != 0); return encodeD(15u, rd, ra, simm); }

// 5-8: Carry-affecting immediate arithmetic & multiply immediate
constexpr inline uint32_t addic(uint8_t rd, uint8_t ra, int16_t simm)    { return encodeD(12u, rd, ra, simm); }
constexpr inline uint32_t addic_rc(uint8_t rd, uint8_t ra, int16_t simm) { return encodeD(13u, rd, ra, simm); }
constexpr inline uint32_t subfic(uint8_t rd, uint8_t ra, int16_t simm)   { return encodeD(8u,  rd, ra, simm); }
constexpr inline uint32_t mulli(uint8_t rd, uint8_t ra, int16_t simm)    { return encodeD(7u,  rd, ra, simm); }

// 9-15: Logical immediate & nop
constexpr inline uint32_t ori(uint8_t ra, uint8_t rs, uint16_t uimm)      { return encodeDU(24u, rs, ra, uimm); }
constexpr inline uint32_t oris(uint8_t ra, uint8_t rs, uint16_t uimm)     { return encodeDU(25u, rs, ra, uimm); }
constexpr inline uint32_t xori(uint8_t ra, uint8_t rs, uint16_t uimm)     { return encodeDU(26u, rs, ra, uimm); }
constexpr inline uint32_t xoris(uint8_t ra, uint8_t rs, uint16_t uimm)    { return encodeDU(27u, rs, ra, uimm); }
constexpr inline uint32_t andi_rc(uint8_t ra, uint8_t rs, uint16_t uimm)  { return encodeDU(28u, rs, ra, uimm); }
constexpr inline uint32_t andis_rc(uint8_t ra, uint8_t rs, uint16_t uimm) { return encodeDU(29u, rs, ra, uimm); }
constexpr inline uint32_t nop()                                           { return ori(0, 0, 0); }

// 16-19: Compare instructions
constexpr inline uint32_t cmpwi(uint8_t crfd, uint8_t ra, int16_t simm) {
    return (11u << 26) | ((uint32_t)(crfd & 7u) << 23) | ((uint32_t)(ra & 0x1Fu) << 16) | (uint16_t)simm;
}
constexpr inline uint32_t cmplwi(uint8_t crfd, uint8_t ra, uint16_t uimm) {
    return (10u << 26) | ((uint32_t)(crfd & 7u) << 23) | ((uint32_t)(ra & 0x1Fu) << 16) | (uint32_t)uimm;
}
constexpr inline uint32_t cmpw(uint8_t crfd, uint8_t ra, uint8_t rb) {
    return (31u << 26) | ((uint32_t)(crfd & 7u) << 23) | ((uint32_t)(ra & 0x1Fu) << 16) | ((uint32_t)(rb & 0x1Fu) << 11) | (0u << 1);
}
constexpr inline uint32_t cmplw(uint8_t crfd, uint8_t ra, uint8_t rb) {
    return (31u << 26) | ((uint32_t)(crfd & 7u) << 23) | ((uint32_t)(ra & 0x1Fu) << 16) | ((uint32_t)(rb & 0x1Fu) << 11) | (32u << 1);
}

// 20-31: Memory load/store (D-form with r0 base assertion + X-form byte-reversed/indexed)
inline uint32_t lwz(uint8_t rd, uint8_t ra, int16_t d)  { assert(ra != 0); return encodeD(32u, rd, ra, d); }
inline uint32_t lwzu(uint8_t rd, uint8_t ra, int16_t d) { assert(ra != 0); return encodeD(33u, rd, ra, d); }
inline uint32_t stw(uint8_t rs, uint8_t ra, int16_t d)  { assert(ra != 0); return encodeD(36u, rs, ra, d); }
inline uint32_t stwu(uint8_t rs, uint8_t ra, int16_t d) { assert(ra != 0); return encodeD(37u, rs, ra, d); }
inline uint32_t lhz(uint8_t rd, uint8_t ra, int16_t d)  { assert(ra != 0); return encodeD(40u, rd, ra, d); }
inline uint32_t lha(uint8_t rd, uint8_t ra, int16_t d)  { assert(ra != 0); return encodeD(42u, rd, ra, d); }
inline uint32_t sth(uint8_t rs, uint8_t ra, int16_t d)  { assert(ra != 0); return encodeD(44u, rs, ra, d); }
inline uint32_t lbz(uint8_t rd, uint8_t ra, int16_t d)  { assert(ra != 0); return encodeD(34u, rd, ra, d); }
inline uint32_t stb(uint8_t rs, uint8_t ra, int16_t d)  { assert(ra != 0); return encodeD(38u, rs, ra, d); }
inline uint32_t lmw(uint8_t rd, uint8_t ra, int16_t d)  { assert(ra != 0); return encodeD(46u, rd, ra, d); }
inline uint32_t stmw(uint8_t rs, uint8_t ra, int16_t d) { assert(ra != 0); return encodeD(47u, rs, ra, d); }
constexpr inline uint32_t lwzx(uint8_t rd, uint8_t ra, uint8_t rb)  { return encodeX(rd, ra, rb, 23u,  false); }
constexpr inline uint32_t stwx(uint8_t rs, uint8_t ra, uint8_t rb)  { return encodeX(rs, ra, rb, 151u, false); }
constexpr inline uint32_t lhbrx(uint8_t rd, uint8_t ra, uint8_t rb) { return encodeX(rd, ra, rb, 790u, false); }
constexpr inline uint32_t lwbrx(uint8_t rd, uint8_t ra, uint8_t rb) { return encodeX(rd, ra, rb, 534u, false); }
constexpr inline uint32_t sthbrx(uint8_t rs, uint8_t ra, uint8_t rb) { return encodeX(rs, ra, rb, 918u, false); }
constexpr inline uint32_t stwbrx(uint8_t rs, uint8_t ra, uint8_t rb) { return encodeX(rs, ra, rb, 662u, false); }

// 32-39: Integer arithmetic (XO-form)
constexpr inline uint32_t add(uint8_t rd, uint8_t ra, uint8_t rb, bool oe = false, bool rc = false)   { return encodeXO(rd, ra, rb, 266u, oe, rc); }
constexpr inline uint32_t addc(uint8_t rd, uint8_t ra, uint8_t rb, bool oe = false, bool rc = false)  { return encodeXO(rd, ra, rb, 10u,  oe, rc); }
constexpr inline uint32_t adde(uint8_t rd, uint8_t ra, uint8_t rb, bool oe = false, bool rc = false)  { return encodeXO(rd, ra, rb, 138u, oe, rc); }
constexpr inline uint32_t subf(uint8_t rd, uint8_t ra, uint8_t rb, bool oe = false, bool rc = false)  { return encodeXO(rd, ra, rb, 40u,  oe, rc); }
constexpr inline uint32_t subfc(uint8_t rd, uint8_t ra, uint8_t rb, bool oe = false, bool rc = false) { return encodeXO(rd, ra, rb, 8u,   oe, rc); }
constexpr inline uint32_t subfe(uint8_t rd, uint8_t ra, uint8_t rb, bool oe = false, bool rc = false) { return encodeXO(rd, ra, rb, 136u, oe, rc); }
constexpr inline uint32_t neg(uint8_t rd, uint8_t ra, bool oe = false, bool rc = false)               { return encodeXO(rd, ra, 0,  104u, oe, rc); }
constexpr inline uint32_t mullw(uint8_t rd, uint8_t ra, uint8_t rb, bool oe = false, bool rc = false) { return encodeXO(rd, ra, rb, 235u, oe, rc); }

// 40-50: Bitwise logical, shifts, sign-extend, count leading zeros (X-form)
constexpr inline uint32_t and_(uint8_t ra, uint8_t rs, uint8_t rb, bool rc = false)   { return encodeX(rs, ra, rb, 28u,  rc); }
constexpr inline uint32_t andc(uint8_t ra, uint8_t rs, uint8_t rb, bool rc = false)   { return encodeX(rs, ra, rb, 60u,  rc); }
constexpr inline uint32_t or_(uint8_t ra, uint8_t rs, uint8_t rb, bool rc = false)    { return encodeX(rs, ra, rb, 444u, rc); }
constexpr inline uint32_t orc(uint8_t ra, uint8_t rs, uint8_t rb, bool rc = false)    { return encodeX(rs, ra, rb, 412u, rc); }
constexpr inline uint32_t xor_(uint8_t ra, uint8_t rs, uint8_t rb, bool rc = false)   { return encodeX(rs, ra, rb, 316u, rc); }
constexpr inline uint32_t nand(uint8_t ra, uint8_t rs, uint8_t rb, bool rc = false)   { return encodeX(rs, ra, rb, 476u, rc); }
constexpr inline uint32_t nor(uint8_t ra, uint8_t rs, uint8_t rb, bool rc = false)    { return encodeX(rs, ra, rb, 124u, rc); }
constexpr inline uint32_t eqv(uint8_t ra, uint8_t rs, uint8_t rb, bool rc = false)    { return encodeX(rs, ra, rb, 284u, rc); }
constexpr inline uint32_t mr(uint8_t ra, uint8_t rs)                                  { return or_(ra, rs, rs, false); }
constexpr inline uint32_t not_(uint8_t ra, uint8_t rs, bool rc = false)               { return nor(ra, rs, rs, rc); }
constexpr inline uint32_t slw(uint8_t ra, uint8_t rs, uint8_t rb, bool rc = false)    { return encodeX(rs, ra, rb, 24u,  rc); }
constexpr inline uint32_t srw(uint8_t ra, uint8_t rs, uint8_t rb, bool rc = false)    { return encodeX(rs, ra, rb, 536u, rc); }
constexpr inline uint32_t sraw(uint8_t ra, uint8_t rs, uint8_t rb, bool rc = false)   { return encodeX(rs, ra, rb, 792u, rc); }
constexpr inline uint32_t srawi(uint8_t ra, uint8_t rs, uint8_t sh, bool rc = false)  { return encodeX(rs, ra, sh, 824u, rc); }
constexpr inline uint32_t extsb(uint8_t ra, uint8_t rs, bool rc = false)              { return encodeX(rs, ra, 0,  954u, rc); }
constexpr inline uint32_t extsh(uint8_t ra, uint8_t rs, bool rc = false)              { return encodeX(rs, ra, 0,  922u, rc); }
constexpr inline uint32_t cntlzw(uint8_t ra, uint8_t rs, bool rc = false)             { return encodeX(rs, ra, 0,  26u,  rc); }

// 51-53: Rotate & mask (M-form)
constexpr inline uint32_t rlwinm(uint8_t ra, uint8_t rs, uint8_t sh, uint8_t mb, uint8_t me, bool rc = false) {
    return encodeM(21u, rs, ra, sh, mb, me, rc);
}
constexpr inline uint32_t rlwimi(uint8_t ra, uint8_t rs, uint8_t sh, uint8_t mb, uint8_t me, bool rc = false) {
    return encodeM(20u, rs, ra, sh, mb, me, rc);
}
constexpr inline uint32_t rlwnm(uint8_t ra, uint8_t rs, uint8_t rb, uint8_t mb, uint8_t me, bool rc = false) {
    return encodeM(23u, rs, ra, rb, mb, me, rc);
}
constexpr inline uint32_t slwi(uint8_t ra, uint8_t rs, uint8_t sh, bool rc = false) {
    return rlwinm(ra, rs, sh, 0, (uint8_t)(31 - sh), rc);
}
constexpr inline uint32_t srwi(uint8_t ra, uint8_t rs, uint8_t sh, bool rc = false) {
    return rlwinm(ra, rs, (uint8_t)(32 - sh), sh, 31, rc);
}
constexpr inline uint32_t rotrwi(uint8_t ra, uint8_t rs, uint8_t sh, bool rc = false) {
    return rlwinm(ra, rs, (uint8_t)(32 - sh), 0, 31, rc);
}

// 54-60: SPR / CR moves (XFX-form)
constexpr inline uint32_t mtspr(uint16_t spr, uint8_t rs) {
    uint32_t sprEnc = ((uint32_t)(spr & 0x1Fu) << 5) | ((uint32_t)(spr >> 5) & 0x1Fu);
    return (31u << 26) | ((uint32_t)(rs & 0x1Fu) << 21) | (sprEnc << 11) | (467u << 1);
}
constexpr inline uint32_t mfspr(uint8_t rd, uint16_t spr) {
    uint32_t sprEnc = ((uint32_t)(spr & 0x1Fu) << 5) | ((uint32_t)(spr >> 5) & 0x1Fu);
    return (31u << 26) | ((uint32_t)(rd & 0x1Fu) << 21) | (sprEnc << 11) | (339u << 1);
}
constexpr inline uint32_t mtlr(uint8_t rs)  { return mtspr(8, rs); }
constexpr inline uint32_t mflr(uint8_t rd)  { return mfspr(rd, 8); }
constexpr inline uint32_t mtctr(uint8_t rs) { return mtspr(9, rs); }
constexpr inline uint32_t mfctr(uint8_t rd) { return mfspr(rd, 9); }
constexpr inline uint32_t mtxer(uint8_t rs) { return mtspr(1, rs); }
constexpr inline uint32_t mfxer(uint8_t rd) { return mfspr(rd, 1); }
constexpr inline uint32_t mfcr(uint8_t rd)  { return (31u << 26) | ((uint32_t)(rd & 0x1Fu) << 21) | (19u << 1); }
constexpr inline uint32_t mtcrf(uint8_t fxm, uint8_t rs) {
    return (31u << 26) | ((uint32_t)(rs & 0x1Fu) << 21) | ((uint32_t)fxm << 12) | (144u << 1);
}

// 61-65: Branches (I-form, B-form, XL-form)
constexpr inline uint32_t b(int32_t offsetBytes, bool aa = false, bool lk = false) {
    return (18u << 26) | ((uint32_t)offsetBytes & 0x03FFFFFCu) | (aa ? 2u : 0u) | (lk ? 1u : 0u);
}
constexpr inline uint32_t bl(int32_t offsetBytes) {
    return b(offsetBytes, false, true);
}
constexpr inline uint32_t bc(uint8_t bo, uint8_t bi, int16_t offsetBytes, bool aa = false, bool lk = false) {
    return (16u << 26) |
           ((uint32_t)(bo & 0x1Fu) << 21) |
           ((uint32_t)(bi & 0x1Fu) << 16) |
           ((uint32_t)offsetBytes & 0xFFFCu) |
           (aa ? 2u : 0u) |
           (lk ? 1u : 0u);
}
constexpr inline uint32_t beq(int16_t offsetBytes, uint8_t cr = 0) { return bc(12, (uint8_t)(cr * 4 + 2), offsetBytes); }
constexpr inline uint32_t bne(int16_t offsetBytes, uint8_t cr = 0) { return bc(4,  (uint8_t)(cr * 4 + 2), offsetBytes); }
constexpr inline uint32_t blt(int16_t offsetBytes, uint8_t cr = 0) { return bc(12, (uint8_t)(cr * 4 + 0), offsetBytes); }
constexpr inline uint32_t bge(int16_t offsetBytes, uint8_t cr = 0) { return bc(4,  (uint8_t)(cr * 4 + 0), offsetBytes); }
constexpr inline uint32_t bgt(int16_t offsetBytes, uint8_t cr = 0) { return bc(12, (uint8_t)(cr * 4 + 1), offsetBytes); }
constexpr inline uint32_t ble(int16_t offsetBytes, uint8_t cr = 0) { return bc(4,  (uint8_t)(cr * 4 + 1), offsetBytes); }
constexpr inline uint32_t blr(bool lk = false)   { return (19u << 26) | (20u << 21) | (16u << 1) | (lk ? 1u : 0u); }
constexpr inline uint32_t bctr(bool lk = false)  { return (19u << 26) | (20u << 21) | (528u << 1) | (lk ? 1u : 0u); }
constexpr inline uint32_t bctrl()                { return bctr(true); }

// 66-69: Cache / synchronization (Broadway Gekko / 750CL)
constexpr inline uint32_t dcbst(uint8_t ra, uint8_t rb) { return encodeX(0, ra, rb, 54u,  false); }
constexpr inline uint32_t icbi(uint8_t ra, uint8_t rb)  { return encodeX(0, ra, rb, 982u, false); }
constexpr inline uint32_t sync()                        { return (31u << 26) | (598u << 1); }
constexpr inline uint32_t isync()                       { return (19u << 26) | (150u << 1); }

} // namespace JitPpc

class ArmPpcJit {
public:
    static constexpr size_t STUB_TABLE_SIZE  = 32768; // Direct-mapped Phase 1/2 per-instruction cache per CPU
    static constexpr size_t BLOCK_TABLE_SIZE = 32768; // Direct-mapped Phase 2 multi-instruction block cache per CPU
    static constexpr size_t PAGE_EPOCH_SIZE  = 32768; // Collision-free 4 KB page slots covering 0x00000000..0x07FFFFFF
    static constexpr size_t CODE_LINE_WORDS  = 65536; // 64-byte line bitmap covering 0x00000000..0x07FFFFFF (256 KiB)
    static constexpr size_t DEFAULT_POOL_BYTES = 8 * 1024 * 1024; // 8 MiB code pool in MEM2
    static constexpr size_t DEFAULT_POOL_WORDS = DEFAULT_POOL_BYTES / sizeof(uint32_t);
    static constexpr size_t MAX_STUB_WORDS     = 224;
    static constexpr size_t MAX_BLOCK_WORDS    = 4096; // reserve for inline memory accesses and guarded MMIO paths
    static constexpr size_t MAX_BLOCK_INSTRS   = 24;

    struct StubEntry {
        uint32_t pc;
        uint32_t opcode;
        uint32_t epochAndFlags; // (epoch << 2) | (thumb ? 2 : 0) | 1
        void *code;
    };

    // 32-byte L1-cache-line-sized block descriptor:
    //   instrCount:   bits[7:0]=count, bit[8]=gbaPureAlu, bit[9]=hasBranch, bits[11:10]=loopKind, bit[15]=pollPure
    //   prefixCycles: bits[7:0]=prefixCycles, bits[15:8]=loopCycles
    struct alignas(32) BlockEntry {
        uint32_t pc;
        uint32_t opcode0;
        uint32_t opcode1;
        uint32_t tailHash;
        uint32_t epochAndFlags; // (poolEpoch << 2) | (thumb << 1) | valid
        uint32_t pageEpoch;
        uint16_t instrCount;
        uint16_t prefixCycles;
        void *code;
    };

    struct Stats {
        uint32_t dispatches[2];
        uint32_t stubHits[2];
        uint32_t stubCompiles[2];
        uint32_t nativeInstrs[2];
        uint32_t fallbackInstrs[2];
        uint32_t blockHits[2];
        uint32_t pollSkips[2];
        uint64_t pollSkipIters[2];
        uint32_t blockCompiles[2];
        uint32_t blockCompileFails[2];
        uint32_t fbClassArm[2][8];
        uint32_t fbOpc7[256]; // ARM7 fallback histogram by opcode bits 27:20
        uint32_t fbLast7[256]; // last full ARM7 fallback opcode per bucket (diagnostics)
        uint32_t fbClassThumb[2][8];
        uint32_t blockInstrsExecuted[2];
        uint32_t cp15Exits[2];
        uint32_t smcInvalidations[2];
        uint32_t mapInvalidations[2];
        uint32_t poolWraps;
        size_t   poolWordsUsed;
        size_t   poolLimitWords;
        const uint32_t *firstStubAddr;
        size_t   firstStubWords;
        uint32_t firstStubPc;
        uint32_t firstStubOpcode;
        const uint32_t *firstBlockAddr;
        size_t   firstBlockWords;
        uint32_t firstBlockPc;
    };

    static void init();
    static void reset();
    static void setPoolLimitWords(size_t maxWords);

    // Execute one instruction (or superblock / bounded hardware loop up to remIndep/remShared cycles)
    static int executeArm(Interpreter *cpu, uint32_t opcode, uint32_t remIndep = 1, uint32_t remShared = 1);
    static int executeThumb(Interpreter *cpu, uint32_t opcode, uint32_t remIndep = 1, uint32_t remShared = 1);
    static bool tryExecuteGbaFast(Interpreter *cpu, uint16_t waitCnt, uint32_t targetCycles);

    static inline bool hasCodeAtAddr(uint32_t addr) {
        return (codeLineBits[(addr >> 11) & (CODE_LINE_WORDS - 1)] & (1u << ((addr >> 6) & 31u))) != 0;
    }

    // SMC & Memory map invalidation hooks
    static void invalidateAddr(bool arm7, uint32_t addr);
    static void invalidateRange(bool arm7, uint32_t startAddr, uint32_t endAddr);
    static void invalidateMapRange(bool arm7, uint32_t startAddr, uint32_t endAddr);
    static void invalidateSharedAddr(uint32_t addr);
    static void invalidateCpu(bool arm7);
    static void invalidateAll();

    // Broadway D-cache / I-cache synchronization
    static void flushCodeRange(void *start, size_t byteCount);

    static const Stats &getStats() { return stats; }
    static const uint32_t *getCodePool() { return codePool; }
    static void dumpStats(FILE *f);

    static int fallbackArm(Interpreter *cpu, uint32_t opcode);
    static int fallbackThumb(Interpreter *cpu, uint32_t opcode);
    static uintptr_t resolveArmHandler(uint32_t opcode);
    static uintptr_t resolveThumbHandler(uint16_t opcode);
    static int condBranchArm(Interpreter *cpu, uint32_t opcode);
    static int execBranchArm(Interpreter *cpu, uint32_t opcode, int32_t remDelta, int32_t totalDelta, int32_t prefixCycles);
    static int execBranchThumb(Interpreter *cpu, uint32_t opcode, int32_t remDelta, int32_t totalDelta, int32_t prefixCycles);

private:
    static uint32_t *codePool;
    static size_t poolWordsUsed;
    static size_t poolLimitWords;
    static uint32_t currentEpoch;
    static bool initialized;

    static StubEntry stubTable[2][STUB_TABLE_SIZE];
    static BlockEntry blockTable[2][BLOCK_TABLE_SIZE];
    static uint32_t pageEpoch[2][PAGE_EPOCH_SIZE];
    static uint32_t pageHasBlockBits[2][PAGE_EPOCH_SIZE / 32];
    static uint32_t codeLineBits[CODE_LINE_WORDS];
    static Stats stats;

    // Joint poll-loop skipping (NOODS_JIT_POLL_SKIP). See arm_jit.cpp for the invariants.
    struct PollTrack {
        uint32_t head;        // loop head PC being tracked
        uint32_t prevPC;      // PC of this CPU's previous ARM dispatch
        uint32_t snap[19];    // post-fetch state at the last head arrival: r0..r15, cpsr, pipeline[0..1]
        uint32_t snapStart;   // global time of that arrival
        uint32_t snapEpoch;   // pollEpoch at that arrival
        uint32_t period;      // measured iteration length (global cycles)
        uint32_t validEpoch;  // pollEpoch at which the loop was validated
        bool armed;
        bool valid;
    };
    static PollTrack pollTrack[2];
public:
    static uint32_t pollEpoch;   // bumped by every impure dispatch and every scheduled event
    template <bool Track> static int executeArmCore(Interpreter *cpu, uint32_t opcode, bool *outPure, uint32_t remIndep = 1, uint32_t remShared = 1);
    static int pollExecArm(Interpreter *cpu, uint32_t opcode);
    static int pollJointSkip(Interpreter *cpu, uint32_t S, int r, int ci);
    static void pollSnapshot(PollTrack &t, Interpreter *cpu, uint32_t S);
private:

    static void ensureSpace(size_t wordsNeeded);
    static void wrapPool();

    static void *compileStub(Interpreter *cpu, uint32_t instrPC, uint32_t opcode, bool thumb);
    static bool emitNativeThumb(uint32_t *&p, Interpreter *cpu, uint16_t opcode, int &outCycles, bool emitFlags = true);
    static bool emitNativeArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, int &outCycles, bool emitFlags = true);
    static bool emitNativeLoadThumb(uint32_t *&p, Interpreter *cpu, uint16_t opcode, int &outCycles);
    static bool emitNativeStoreThumb(uint32_t *&p, Interpreter *cpu, uint16_t opcode, int &outCycles);
    static bool emitNativePushPop(uint32_t *&p, Interpreter *cpu, uint16_t regMask, bool isLoad, uint32_t rawOpcode, bool isThumb, int &outCycles);
    static bool emitNativeLoadArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, int &outCycles);
    static bool emitNativeStoreArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, int &outCycles);
    static void emitGuardedPollHalfLoad(uint32_t *&p, Interpreter *cpu, uint8_t rd,
                                        void (*storeFn)(uint32_t *&, uint8_t, uint8_t),
                                        uint32_t **outJumpSlots, int &jumpCount);
    static void emitHelperR15Sync(uint32_t *&p, bool enter);
    static bool emitInlineBranchArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, uint32_t instrPC, int32_t totalDelta, int32_t prefixCycles, bool helperInBlock, uint32_t *loopTop = nullptr);
    static bool emitInlineBranchThumb(uint32_t *&p, Interpreter *cpu, uint16_t opcode, uint32_t instrPC, int32_t totalDelta, int32_t prefixCycles, bool helperInBlock, uint32_t *loopTop = nullptr);
    static bool emitInlineBxArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, uint32_t instrPC, int32_t totalDelta, int32_t prefixCycles, bool helperInBlock);
    static bool emitInlineBxThumb(uint32_t *&p, Interpreter *cpu, uint16_t opcode, uint32_t instrPC, int32_t totalDelta, int32_t prefixCycles, bool helperInBlock);
    static bool emitPcDestLoadTerminatorArm(uint32_t *&p, Interpreter *cpu, uint32_t opcode, uint32_t instrPC, int32_t totalDelta, int32_t prefixCycles, bool helperInBlock);

    static void emitLoadGuestReg(uint32_t *&p, uint8_t dstPpc, uint8_t guestReg);
    static void emitStoreGuestReg(uint32_t *&p, uint8_t srcPpc, uint8_t guestReg);
    static void emitUpdateNZ(uint32_t *&p, uint8_t resReg);
    static void emitUpdateAddNZCV(uint32_t *&p);
    static void emitUpdateSubNZCV(uint32_t *&p);

    template <bool Track = false> static int tryExecuteBlock(Interpreter *cpu, uint32_t instrPC, uint32_t opcode, bool thumb, bool &outIsNative0, bool *outPure = nullptr, uint32_t remIndep = 1, uint32_t remShared = 1);
    static bool compileBlock(Interpreter *cpu, uint32_t instrPC, uint32_t opcode0, bool thumb, BlockEntry &entry);
};
