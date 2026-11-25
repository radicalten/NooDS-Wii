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

#pragma once

#include <cstdint>
#include <cstdio>
#include "defines.h"
#include "gpu.h"

class Core;

class Gpu2D {
public:
    Gpu2D(Core *core, bool engine);
    void saveState(FILE *file);
    void loadState(FILE *file);

    void reloadRegisters();
    void updateWindows(int line);
    void drawGbaScanline(int line);
    void drawScanline(int line);

    void finishGx2D();

    uint32_t *getFramebuffer() { return framebuffer; }
    uint32_t *getRawLine() { return layers[0]; }

    uint32_t readDispCnt() { return dispCnt; }
    uint16_t readBgCnt(int bg) { return bgCnt[bg]; }
    uint16_t readWinIn() { return winIn; }
    uint16_t readWinOut() { return winOut; }
    uint16_t readBldCnt() { return bldCnt; }
    uint16_t readBldAlpha() { return bldAlpha; }
    uint16_t readMasterBright() { return masterBright; }

    void writeDispCnt(uint32_t mask, uint32_t value);
    void writeBgCnt(int bg, uint16_t mask, uint16_t value);
    void writeBgHOfs(int bg, uint16_t mask, uint16_t value);
    void writeBgVOfs(int bg, uint16_t mask, uint16_t value);
    void writeBgPA(int bg, uint16_t mask, uint16_t value);
    void writeBgPB(int bg, uint16_t mask, uint16_t value);
    void writeBgPC(int bg, uint16_t mask, uint16_t value);
    void writeBgPD(int bg, uint16_t mask, uint16_t value);
    void writeBgX(int bg, uint32_t mask, uint32_t value);
    void writeBgY(int bg, uint32_t mask, uint32_t value);
    void writeWinH(int win, uint16_t mask, uint16_t value);
    void writeWinV(int win, uint16_t mask, uint16_t value);
    void writeWinIn(uint16_t mask, uint16_t value);
    void writeWinOut(uint16_t mask, uint16_t value);
    void writeMosaic(uint16_t mask, uint16_t value);
    void writeBldCnt(uint16_t mask, uint16_t value);
    void writeBldAlpha(uint16_t mask, uint16_t value);
    void writeBldY(uint8_t value);
    void writeMasterBright(uint16_t mask, uint16_t value);

private:
    bool queueGxTextLine(int line);
    bool queueGxBitmapLine(int line);
    Core *core;
    bool engine;

    uint32_t bgVramAddr, objVramAddr;
    uint8_t *palette, *oam;
    uint8_t **extPalettes;

    uint32_t framebuffer[256 * 192] = {};
    uint32_t layers[2][256] = {};
    int8_t priorities[2][256] = {};
    int8_t blendBits[2][256] = {};

    int internalX[2] = {};
    int internalY[2] = {};
    bool winHFlip[2] = {};
    bool winVFlag[2] = {};

    uint32_t dispCnt = 0;
    uint16_t bgCnt[4] = {};
    uint16_t bgHOfs[4] = {};
    uint16_t bgVOfs[4] = {};
    int16_t bgPA[2] = {};
    int16_t bgPB[2] = {};
    int16_t bgPC[2] = {};
    int16_t bgPD[2] = {};
    int32_t bgX[2] = {};
    int32_t bgY[2] = {};
    uint16_t winX1[2] = {};
    uint16_t winX2[2] = {};
    uint16_t winY1[2] = {};
    uint16_t winY2[2] = {};
    uint16_t winIn = 0;
    uint16_t winOut = 0;
    uint16_t bldCnt = 0;
    uint16_t mosaic = 0;
    uint16_t bldAlpha = 0;
    uint8_t bldY = 0;
    uint16_t masterBright = 0;

    static uint32_t rgb5ToRgb6(uint32_t color);

    // v1.72: inline hot pixel helpers and precomputed window/brightness caches
    FORCE_INLINE void drawBgPixel(int bg, int line, int x, uint32_t pixel);
    FORCE_INLINE void drawObjPixel(int line, int x, uint32_t pixel, int8_t priority);
    FORCE_INLINE uint16_t fastRead16(uint32_t addr);
    FORCE_INLINE uint32_t fastRead32(uint32_t addr);
    FORCE_INLINE uint8_t  fastRead8 (uint32_t addr);
    template<bool GBA> FORCE_INLINE uint16_t fastRead16T(uint32_t addr);
    template<bool GBA> FORCE_INLINE uint32_t fastRead32T(uint32_t addr);
    template<bool GBA> FORCE_INLINE uint8_t  fastRead8T (uint32_t addr);
    // precomputed per-scanline window mask (valid after updateWindows)
    uint8_t winMaskCache[256] = {};
    bool winMaskValid = false;
    bool soloTextLine(int bg, int line, uint32_t backdrop, bool raw);
    bool soloBitmapLine(int bg, int line, uint32_t backdrop, bool raw); // v1.118
    bool l1Needed = true; // v1.114: this line needs the second (underneath) layer
    void buildWindowMask(int line);

    // v1.80: one parsed active-object list per scanline. It replaces the old
    // OAM rescan in each of the two object passes while preserving OAM order.
    struct ObjectLine {
        uint16_t object[3];
        int16_t x;
        int16_t spriteY;
        uint8_t width;
        uint8_t height;
        uint8_t width2;
        uint8_t height2;
        int8_t priority;
        uint8_t type;
    } __attribute__((aligned(4)));
    ObjectLine objectLine[128] = {};
    int objectLineCount = 0;
    int buildObjectLine(int line);
    // v1.123: which OAM entries can touch each line, rebuilt when OAM is written
    uint32_t objMask[192][4] = {};
    uint32_t objMaskGen = 0;
    void buildObjectMask();
    int buildObjectLineDs(int line);
    static uint8_t s_brightIncTable[64][17];
    static uint8_t s_brightDecTable[64][17];
    static bool s_brightTablesReady;
    static void ensureBrightTables();
    // v1.74: GBA 5-bit brightness LUTs (reuse same ensure)
    static uint8_t s_brightIncTable5[32][17];
    static uint8_t s_brightDecTable5[32][17];
    // v1.73: alpha-blend LUT for common eva+evb==16 case (17*4096)
    static uint8_t s_alphaLUT[17][4096];
    static bool s_alphaLUTReady;
    static void ensureAlphaLUT();
    FORCE_INLINE static uint8_t alphaBlend6(uint8_t src, uint8_t dst, uint8_t eva);

    template <bool gbaMode> void drawText(int bg, int line);
    template <bool gbaMode> void drawAffine(int bg, int line);
    template <bool gbaMode> void drawAffineImpl(int bg, int line);
    void drawExtended(int bg, int line);
    void drawExtendedGba(int bg, int line);
    void drawLarge(int bg, int line);
    bool objReference = false; // diagnostic dual-render only
    template <bool gbaMode> void drawObjects(int line, bool window);
    template <bool gbaMode> void drawObjectsImpl(int line, bool window);
};

#include <stdint.h>
// buildWindowMask chooses a control byte from these four. If each byte has
// the BG bit, the resulting mask necessarily has it for every x.
static inline bool gbaAffineAllWindowsAllow(uint16_t winIn, uint16_t winOut, uint8_t bgBit) {
    return (winIn & (winIn >> 8) & winOut & (winOut >> 8) & bgBit) != 0;
}

#include <cstdint>
#ifdef NOODS_V180_PROFILE
#include <tuxedo/ppc/clock.h>
#include <tuxedo/ppc/intrinsics.h>
extern uint64_t g_gx2dTicks[8];
struct Gx2DProfileScope {
    unsigned index;
    uint64_t start;
    explicit Gx2DProfileScope(unsigned i):index(i),start(PPCGetTickCount()) {}
    ~Gx2DProfileScope() {g_gx2dTicks[index]+=PPCGetTickCount()-start;}
};
#define GX2D_PROFILE_QUEUE() Gx2DProfileScope gx2dQueueScope(0)
#define GX2D_PROFILE_BEGIN() uint64_t gx2dPhaseStart=PPCGetTickCount()
#define GX2D_PROFILE_MARK(i) do {uint64_t now=PPCGetTickCount();g_gx2dTicks[i]+=now-gx2dPhaseStart;gx2dPhaseStart=now;} while(0)
#else
#define GX2D_PROFILE_QUEUE() do {} while(0)
#define GX2D_PROFILE_BEGIN() do {} while(0)
#define GX2D_PROFILE_MARK(i) do {} while(0)
#endif

#include <cstdint>
#ifndef NOODS_GX_2D
#define NOODS_GX_2D 0
#endif
#ifndef NOODS_GX_2D_IO
#define NOODS_GX_2D_IO 0
#endif
#ifndef NOODS_GX_2D_BATCH
#define NOODS_GX_2D_BATCH 0
#endif
#ifndef NOODS_GX_2D_PAL_CACHE
#define NOODS_GX_2D_PAL_CACHE 0
#endif
#ifndef NOODS_GX_2D_VERIFY
#define NOODS_GX_2D_VERIFY 0
#endif
#ifndef NOODS_GX_2D_BITMAP
#define NOODS_GX_2D_BITMAP 0
#endif
#if NOODS_GX_2D_BITMAP && (!NOODS_GX_2D || !NOODS_GX_2D_BATCH || NOODS_GX_2D_IO < 7)
#error GX_2D_BITMAP requires GX_2D=1, GX_2D_BATCH=1 and GX_2D_IO>=7
#endif
// Snapshot at the emulated scanline, not at end-of-frame: no guest VRAM or
// palette pointers survive into the queued draw. CI8 images are 256x4; only
// row zero is sampled (at its texel center).
struct alignas(32) Gx2DLine {
    uint8_t indices[4][1024];
    uint8_t palettes[4][512];
#if NOODS_GX_2D_BITMAP
    bool bitmap; // indices[0][0..511] is a linear big-endian RGB5A3 row
#endif
    uint16_t backdrop;
    uint8_t enabled, priority[4];
#if NOODS_GX_2D_IO >= 4
    const uint8_t *paletteRefs[4];
#endif
};
Gx2DLine *gx2dBegin(unsigned engine, unsigned line);
bool gx2dSetPalette(Gx2DLine *row,unsigned engine,unsigned bg,const uint8_t *src,bool eight);
void gx2dDiscard(unsigned engine, unsigned line);
void gx2dFinish(unsigned engine, uint32_t *framebuffer);
// Call only after the old core and its GX submissions have stopped.
void gx2dReleaseCaches();
extern uint32_t g_gx2dStats[8]; // queued lines, BGs, passes, compared px, wrong px, 4bpp, 8bpp, alloc failures

#if NOODS_GX_2D_BITMAP
extern uint32_t g_gxBitmapStats[4]; // lines, compared pixels, wrong pixels, finish passes
#endif

#include <cstdint>
#include <cstring>

namespace NoodsGx2D {
inline uint32_t unpack565(uint16_t p) {
    // Original RGB5 colors survived GX RGB8 expansion and RGB565 copying.
    // Drop the replicated low green bit, then use DS's even 5->6 expansion.
    return ((p>>10)&0x3Eu)|((p&0x7C0u)<<1)|((p&31u)<<13);
}

// Inspect all palette bytes on each call: no generation-counter assumption.
// A changed guest byte invalidates both converted variants before reuse.
struct PaletteCache {
    uint8_t raw[512] = {}, converted[2][512] = {};
    unsigned valid = 0, conversions = 0;
    const uint8_t *get(const uint8_t *src, bool eight) {
        if (!valid || std::memcmp(raw,src,512)) {
            std::memcpy(raw,src,512); valid=0;
        }
        const unsigned kind=eight?1:0;
        if (!(valid & (1u<<kind))) {
            NoodsGx::packPalette(raw,converted[kind],256,true);
            if (!eight) for (unsigned bank=1;bank<16;++bank)
                converted[kind][bank*32]=converted[kind][bank*32+1]=0;
            valid |= 1u<<kind; ++conversions;
        }
        return converted[kind];
    }
};
// Frame-local immutable storage; reset only after the previous GX fence.
struct alignas(32) PalettePool {
    static constexpr unsigned CAPACITY=768;
    uint8_t raw[512], values[CAPACITY][512];
    const uint8_t *current[2]={nullptr,nullptr};
    unsigned count=0;
    bool valid=false;
    void reset() {count=0;valid=false;current[0]=current[1]=nullptr;}
    const uint8_t *get(const uint8_t *src, bool eight) {
        if (!valid || std::memcmp(raw,src,512)) {
            std::memcpy(raw,src,512);valid=true;current[0]=current[1]=nullptr;
        }
        const unsigned k=eight?1:0;
        if (current[k]) return current[k];
        if (count==CAPACITY) return nullptr; // caller retains embedded fallback
        uint8_t *dst=values[count++];
        NoodsGx::packPalette(raw,dst,256,true);
        if (!eight) for (unsigned bank=1;bank<16;++bank) dst[bank*32]=dst[bank*32+1]=0;
        return current[k]=dst;
    }
};
// Transaction-local GX small-TLUT residency. Pointer identity is safe only
// because every palette snapshot stays immutable until the completion fence.
struct TlutSlots {
    static constexpr unsigned CAPACITY=16;
    const uint8_t *resident[CAPACITY]={};
    unsigned next=0;
    unsigned get(const uint8_t *palette,bool &upload,unsigned pinned=0) {
        for (unsigned i=0;i<CAPACITY;++i) if (resident[i]==palette) {
            upload=false;return i;
        }
        // A layered draw pins at most four of the sixteen slots. Do not evict
        // an earlier layer while resolving the remaining layers for one quad.
        while (pinned&(1u<<next)) next=(next+1)%CAPACITY;
        const unsigned slot=next;
        next=(next+1)%CAPACITY;resident[slot]=palette;upload=true;return slot;
    }
};
// Stable per-BG TLUT names avoid rebinding atlas textures for changing palette
// snapshots. Equal pointers may share an earlier active BG's slot this draw.
struct LayerTlutSlots {
    const uint8_t *resident[4]={},*selected[4]={};
    unsigned chosen[4]={};
    void begin() {for (unsigned i=0;i<4;++i) selected[i]=nullptr;}
    unsigned get(unsigned bg,const uint8_t *palette,bool &upload) {
        unsigned slot=bg;
        for (unsigned i=0;i<bg;++i) if (selected[i]==palette) {slot=chosen[i];break;}
        selected[bg]=palette;chosen[bg]=slot;
        upload=resident[slot]!=palette;resident[slot]=palette;
        return slot;
    }
};
struct AtlasSlice {unsigned base,height;};
inline AtlasSlice atlasSlice(unsigned y,unsigned end) {
    const unsigned base=y&~3u;
    return {base,((end+3u)&~3u)-base};
}
inline unsigned orderedLayers(unsigned enabled,const uint8_t *priority,uint8_t *order) {
    unsigned count=0;
    for (int p=3;p>=0;--p) for (int bg=3;bg>=0;--bg)
        if ((enabled&(1u<<bg)) && priority[bg]==p) order[count++]=bg;
    return count;
}
inline bool eligibleBitmap(bool engineB,uint32_t dc,uint16_t cnt,uint16_t blend,uint8_t by,uint16_t master) {
    const unsigned mode=dc&7;
    const unsigned effect=(blend>>6)&3;
    return engineB && ((dc>>16)&3)==1 && mode>=3 && mode<=5 &&
        (dc&0x1F00u)==0x800u && !(dc&((1u<<3)|0xE000u)) &&
        (cnt&0x84u)==0x84u && !(cnt&(1u<<6)) && !(master&0xC000u) &&
        (effect==0 || (effect>=2 && by==0));
}
inline uint32_t bitmapPair(uint32_t little) {
    const uint32_t a=(little&0xFFFFu)&(0u-((little>>15)&1u));
    const uint32_t b=(little>>16)&(0u-(little>>31));
    return (a<<16)|b;
}
inline uint16_t bitmapPixel(uint16_t pixel) {
    // RGB5A3 bit15=0 would otherwise expose partial alpha, unlike DS bit15=0.
    // Keep DS BGR5 channel order; the dedicated GX stage swaps texture R/B.
    return (pixel&0x8000u)?pixel:0;
}
inline bool eligible(bool engineB, uint32_t dc, uint16_t blend, uint16_t master) {
    return engineB && ((dc>>16)&3)==1 && !(dc&7) &&
        !(dc&((1u<<3)|(1u<<12)|0xE000u|(1u<<30))) &&
        (dc&0xF00u) && !(blend&0xC0u) && !(master&0xC000u);
}
inline uint32_t fourPixels(uint32_t v, bool reverse) {
    if (reverse) return ((v&0xF000u)<<12)|((v&0xF00u)<<8)|((v&0xF0u)<<4)|(v&15u);
    return ((v&15u)<<24)|((v&0xF0u)<<12)|(v&0xF00u)|((v>>12)&15u);
}
inline void storeBe32(uint8_t *dst, uint32_t value) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    value=__builtin_bswap32(value);
#endif
    std::memcpy(dst,&value,4);
}
// Output a contiguous eight-pixel row. The caller owns clipping/CI8 tiling.
inline void linearTileRow(uint8_t *out, uint16_t tile, uint32_t lo, uint32_t hi, bool eight) {
    const bool flip=tile&(1u<<10);
    if (eight) {
        storeBe32(out,flip?hi:__builtin_bswap32(lo));
        storeBe32(out+4,flip?lo:__builtin_bswap32(hi));
    } else {
        const uint32_t bank=((tile>>8)&0xF0u)*0x01010101u;
        storeBe32(out,bank|fourPixels(flip?lo>>16:lo,flip));
        storeBe32(out+4,bank|fourPixels(flip?lo:lo>>16,flip));
    }
}
inline void tileRow(uint8_t *dst, int left, uint16_t tile, uint32_t lo, uint32_t hi, bool eight) {
#if NOODS_GX_2D_IO >= 3
    // A full aligned row stays inside one GX CI8 tile: two word stores replace
    // eight indexed byte stores. Edge/clipped/unaligned rows keep scalar code.
    if ((unsigned)left<=248u && !(left&7)) {
        uint8_t *out=dst+(left/8)*32;
        const bool flip=tile&(1u<<10);
        if (eight) {
            storeBe32(out,flip?hi:__builtin_bswap32(lo));
            storeBe32(out+4,flip?lo:__builtin_bswap32(hi));
        } else {
            const uint32_t bank=((tile>>8)&0xF0u)*0x01010101u;
            storeBe32(out,bank|fourPixels(flip?lo>>16:lo,flip));
            storeBe32(out+4,bank|fourPixels(flip?lo:lo>>16,flip));
        }
        return;
    }
#endif
    for (int k=0;k<8;++k) {
        const int x=left+k;
        if ((unsigned)x>=256u) continue;
        const int sx=(tile&(1u<<10))?7-k:k;
        const unsigned idx=eight?((sx<4?lo:hi)>>((sx&3)*8))&255u:(lo>>(sx*4))&15u;
        dst[(x/8)*32+(x&7)]=eight?idx:((tile>>8)&0xF0u)|idx;
    }
}
}
