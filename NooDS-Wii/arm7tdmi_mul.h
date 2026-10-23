/*
    ARM7TDMI Radix-4 Booth Multiplier Carry Flag Model

    Adapted for C++ / NooDS-Wii from zaydlang's reference implementation
    (https://github.com/zaydlang/multiplication-algorithm), used under the
    zlib license:

    Copyright (c) 2024 zaydlang

    This software is provided 'as-is', without any express or implied warranty.
    In no event will the authors be held liable for any damages arising from the
    use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
       claim that you wrote the original software. If you use this software in a
       product, an acknowledgment in the product documentation would be
       appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.
*/

#pragma once

#include <cstdint>

namespace Arm7TdmiMul {

enum Flavor {
    SHORT,
    LONG_SIGNED,
    LONG_UNSIGNED,
};

struct Result {
    uint64_t output;
    bool carry;
    int ticks;
};

struct BoothRecode {
    uint64_t recoded;
    bool carry;
};

struct CSAOut {
    uint64_t output;
    uint64_t carry;
};

struct U128 {
    uint64_t lo;
    uint64_t hi;
};

static inline uint64_t maskRange(int lo, int hi) {
    uint64_t size = (uint64_t)(hi - lo);
    return ((1ULL << size) - 1ULL) << lo;
}

static inline bool bitAt(uint64_t x, int n) {
    return (x >> n) & 1ULL;
}

static inline uint64_t signExtend(uint64_t value, int fromSize, int toSize) {
    if (bitAt(value, fromSize - 1))
        value |= maskRange(fromSize, toSize);
    return value;
}

static inline uint64_t asrN(uint64_t value, int shift, int size) {
    int64_t se = (int64_t)signExtend(value, size, 64);
    se >>= shift;
    return (uint64_t)se & maskRange(0, size);
}

static inline U128 u128Ror(U128 in, int shift) {
    return {
        (in.lo >> shift) | (in.hi << (64 - shift)),
        (in.hi >> shift) | (in.lo << (64 - shift)),
    };
}

static inline BoothRecode boothRecode(uint64_t input, uint8_t chunk) {
    BoothRecode out = { 0, false };
    switch (chunk) {
        case 0: out = {            0ULL, false }; break;
        case 1: out = {        input,    false }; break;
        case 2: out = {        input,    false }; break;
        case 3: out = {    2 * input,    false }; break;
        case 4: out = { ~(2 * input),    true  }; break;
        case 5: out = {       ~input,    true  }; break;
        case 6: out = {       ~input,    true  }; break;
        case 7: out = {            0ULL, false }; break;
    }
    out.recoded &= 0x3FFFFFFFFULL;
    return out;
}

static inline CSAOut csaStep(uint64_t a, uint64_t b, uint64_t c) {
    return { a ^ b ^ c, (a & b) | (b & c) | (c & a) };
}

static inline Result execute(Flavor flavor, uint32_t rm, uint32_t rs, uint64_t acc = 0) {
    const bool isSigned = (flavor == LONG_SIGNED || flavor == SHORT);
    const bool isLong   = (flavor == LONG_SIGNED || flavor == LONG_UNSIGNED);

    uint64_t multiplicand = isSigned ? signExtend(rm, 32, 34) : ((uint64_t)rm & 0x1FFFFFFFFULL);
    uint64_t multiplier   = isSigned ? signExtend(rs, 32, 34) : ((uint64_t)rs & 0x1FFFFFFFFULL);
    bool aluCarryIn       = (multiplier & 1ULL) != 0;

    CSAOut csa = { acc, (multiplier & 1ULL) ? ~multiplicand : 0ULL };
    uint64_t accShift = acc >> 34;

    U128 pSum   = { csa.output & 1ULL, 0ULL };
    U128 pCarry = { csa.carry  & 1ULL, 0ULL };
    csa.output >>= 1;
    csa.carry  >>= 1;
    pSum   = u128Ror(pSum, 1);
    pCarry = u128Ror(pCarry, 1);

    int iters = 0;
    do {
        BoothRecode addends[4];
        for (int i = 0; i < 4; i++)
            addends[i] = boothRecode(multiplicand, (uint8_t)((multiplier >> (2 * i)) & 7ULL));

        CSAOut cur = csa;
        CSAOut fin = { 0ULL, 0ULL };
        for (int i = 0; i < 4; i++) {
            cur.output &= 0x1FFFFFFFFULL;
            cur.carry  &= 0x1FFFFFFFFULL;
            CSAOut r = csaStep(cur.output, addends[i].recoded & 0x1FFFFFFFFULL, cur.carry);
            r.carry = (r.carry << 1) | (uint64_t)addends[i].carry;
            fin.output |= (r.output & 3ULL) << (2 * i);
            fin.carry  |= (r.carry  & 3ULL) << (2 * i);
            r.output >>= 2;
            r.carry  >>= 2;
            uint64_t magic = (uint64_t)bitAt(accShift, 0) + (uint64_t)!bitAt(cur.carry, 32) + (uint64_t)!bitAt(addends[i].recoded, 33);
            r.output |= magic << 31;
            r.carry  |= (uint64_t)!bitAt(accShift, 1) << 32;
            accShift >>= 2;
            cur = r;
        }
        fin.output |= cur.output << 8;
        fin.carry  |= cur.carry  << 8;
        csa = fin;

        pSum.lo   |= csa.output & 0xFFULL;
        pCarry.lo |= csa.carry  & 0xFFULL;
        csa.output >>= 8;
        csa.carry  >>= 8;
        pSum   = u128Ror(pSum, 8);
        pCarry = u128Ror(pCarry, 8);
        multiplier = asrN(multiplier, 8, 33);
        iters++;
    } while (!(isSigned ? (multiplier == 0x1FFFFFFFFULL || multiplier == 0ULL) : (multiplier == 0ULL)));

    pSum.lo   |= csa.output;
    pCarry.lo |= csa.carry;

    const int corr = (iters == 4) ? 31 : (31 - 8 * iters);
    pSum   = u128Ror(pSum, corr);
    pCarry = u128Ror(pCarry, corr);

    auto add32 = [](uint32_t a, uint32_t b, bool cIn, bool &cOut) -> uint32_t {
        uint64_t full = (uint64_t)a + (uint64_t)b + (uint64_t)cIn;
        cOut = (full >> 32) != 0;
        return (uint32_t)full;
    };

    if (isLong) {
        bool cLo = false, cHi = false;
        if (iters == 4) {
            uint32_t lo = add32((uint32_t)pSum.hi, (uint32_t)pCarry.hi, aluCarryIn, cLo);
            uint32_t hi = add32((uint32_t)(pSum.hi >> 32), (uint32_t)(pCarry.hi >> 32), cLo, cHi);
            return { ((uint64_t)hi << 32) | lo, ((pCarry.hi >> 63) & 1ULL) != 0, iters };
        } else {
            uint32_t lo = add32((uint32_t)(pSum.hi >> 32), (uint32_t)(pCarry.hi >> 32), aluCarryIn, cLo);
            int sh = 2 + 8 * iters;
            pCarry.lo = signExtend(pCarry.lo, sh, 64);
            pSum.lo  |= accShift << sh;
            uint32_t hi = add32((uint32_t)pSum.lo, (uint32_t)pCarry.lo, cLo, cHi);
            return { ((uint64_t)hi << 32) | lo, ((pCarry.hi >> 63) & 1ULL) != 0, iters };
        }
    } else {
        bool cOut = false;
        if (iters == 4) {
            uint32_t out = add32((uint32_t)pSum.hi, (uint32_t)pCarry.hi, aluCarryIn, cOut);
            return { out, ((pCarry.hi >> 31) & 1ULL) != 0, iters };
        } else {
            uint32_t out = add32((uint32_t)(pSum.hi >> 32), (uint32_t)(pCarry.hi >> 32), aluCarryIn, cOut);
            return { out, ((pCarry.hi >> 63) & 1ULL) != 0, iters };
        }
    }
}

static inline int signedMulTicks(uint32_t rs) {
    for (int m = 1; m < 4; m++) {
        uint32_t top = rs >> (m * 8);
        uint32_t ones = 0xFFFFFFFFu >> (m * 8);
        if (top == 0 || top == ones) return m;
    }
    return 4;
}

static inline int unsignedMulTicks(uint32_t rs) {
    for (int m = 1; m < 4; m++) {
        if ((rs >> (m * 8)) == 0) return m;
    }
    return 4;
}

} // namespace Arm7TdmiMul
