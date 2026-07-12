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

class Core;

// Output pixel format: GX RGB5A3 (native Wii texture word, big-endian)
// Opaque pixel  (bit 15 = 1): 1_RRRRR_GGGGG_BBBBB
// Transparent   (bit 15 = 0): 0_AAA_RRRR_GGGG_BBBB

// We always emit opaque pixels from the 2D engine, so bit 15 is always 1.
// The 18-bit RGB6 internal colour  (b[17:12] g[11:6] r[5:0]) is converted by 
// taking the top 5 bits of each channel: R5 = r6 >> 1,  G5 = g6 >> 1,  B5 = b6 >> 1

// The resulting 16-bit value is stored big-endian inside a uint16_t because
// GX textures are big-endian; on the PowerPC this is naturally correct when
// we write a uint16_t straight to the tiled texture buffer.

class Gpu2D {
public:
    Gpu2D(Core *core, bool engine);
    void saveState(FILE *file);
    void loadState(FILE *file);

    void reloadRegisters();
    void updateWindows(int line);
    void drawGbaScanline(int line);
    void drawScanline(int line);

    // Framebuffer now holds RGB5A3 words (uint16_t packed into uint32_t
    // slots for alignment – upper 16 bits unused / zero).
    // We keep uint32_t so that the rest of the emulator core (gpu.cpp,
    // gpu3D, etc.) that calls getFramebuffer() keeps compiling without
    // changes; the actual useful data is in bits [15:0] of every word.

    uint32_t *getFramebuffer() { return framebuffer; }
    uint32_t *getRawLine()     { return layers[0]; }

    uint32_t readDispCnt()         { return dispCnt; }
    uint16_t readBgCnt(int bg)     { return bgCnt[bg]; }
    uint16_t readWinIn()           { return winIn; }
    uint16_t readWinOut()          { return winOut; }
    uint16_t readBldCnt()          { return bldCnt; }
    uint16_t readBldAlpha()        { return bldAlpha; }
    uint16_t readMasterBright()    { return masterBright; }

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
    Core *core;
    bool engine;

    uint32_t bgVramAddr, objVramAddr;
    uint8_t *palette, *oam;
    uint8_t **extPalettes;

    // Internal framebuffer: each slot stores one RGB5A3 word in bits[15:0].
    uint32_t framebuffer[256 * 192] = {};

    // Working layers still use the old RGB6 internal format during rendering;
    // conversion to RGB5A3 happens once at the end of each scanline.
    uint32_t layers[2][256]    = {};
    int8_t   priorities[2][256] = {};
    int8_t   blendBits[2][256]  = {};

    int  internalX[2] = {};
    int  internalY[2] = {};
    bool winHFlip[2]  = {};
    bool winVFlag[2]  = {};

    uint32_t dispCnt    = 0;
    uint16_t bgCnt[4]   = {};
    uint16_t bgHOfs[4]  = {};
    uint16_t bgVOfs[4]  = {};
    int16_t  bgPA[2]    = {};
    int16_t  bgPB[2]    = {};
    int16_t  bgPC[2]    = {};
    int16_t  bgPD[2]    = {};
    int32_t  bgX[2]     = {};
    int32_t  bgY[2]     = {};
    uint16_t winX1[2]   = {};
    uint16_t winX2[2]   = {};
    uint16_t winY1[2]   = {};
    uint16_t winY2[2]   = {};
    uint16_t winIn      = 0;
    uint16_t winOut     = 0;
    uint16_t bldCnt     = 0;
    uint16_t mosaic     = 0;
    uint16_t bldAlpha   = 0;
    uint8_t  bldY       = 0;
    uint16_t masterBright = 0;

    // -----------------------------------------------------------------------
    // Colour helpers
    // -----------------------------------------------------------------------

    // Keep the original RGB5→RGB6 converter for internal blending arithmetic.
    static uint32_t rgb5ToRgb6(uint32_t color);

    // Convert a finalised RGB6 pixel (bits 17:0 = b6 g6 r6) to an RGB5A3
    // opaque word suitable for direct DMA into a GX_TF_RGB5A3 texture.
    //   RGB5A3 opaque: bit15=1, bits14:10=R5, bits9:5=G5, bits4:0=B5
    static inline uint16_t rgb6ToRgb5A3(uint32_t rgb6) {
        uint8_t r5 = (uint8_t)((rgb6 >>  1) & 0x1F); // r6[5:1]
        uint8_t g5 = (uint8_t)((rgb6 >>  7) & 0x1F); // g6[11:7]  (g starts at bit 6)
        uint8_t b5 = (uint8_t)((rgb6 >> 13) & 0x1F); // b6[17:13] (b starts at bit 12)
        return (uint16_t)(0x8000u | ((uint16_t)r5 << 10) | ((uint16_t)g5 << 5) | b5);
    }

    // Convert a raw RGB5 palette word (DS/GBA 16-bit colour, bit15 = opaque
    // flag inside the emulator – NOT the GX opaque bit) to RGB5A3.
    // The DS stores colours as  0_bbbbb_ggggg_rrrrr  (bit15 used as flag).
    static inline uint16_t rgb5PalToRgb5A3(uint32_t palColor) {
        // palColor lower 15 bits: bits14:10=b, bits9:5=g, bits4:0=r  (DS order)
        uint8_t r5 = (uint8_t)( palColor        & 0x1F);
        uint8_t g5 = (uint8_t)((palColor >>  5) & 0x1F);
        uint8_t b5 = (uint8_t)((palColor >> 10) & 0x1F);
        // GX RGB5A3 opaque: 1_RRRRR_GGGGG_BBBBB
        return (uint16_t)(0x8000u | ((uint16_t)r5 << 10) | ((uint16_t)g5 << 5) | b5);
    }

    // GBA blending works entirely in RGB5 space; convert the finished RGB5
    // blended value (same bit layout as a palette word, no flag bits) to RGB5A3.
    static inline uint16_t rgb5BlendToRgb5A3(uint32_t blended) {
        // blended: bits14:10=b, bits9:5=g, bits4:0=r
        uint8_t r5 = (uint8_t)( blended        & 0x1F);
        uint8_t g5 = (uint8_t)((blended >>  5) & 0x1F);
        uint8_t b5 = (uint8_t)((blended >> 10) & 0x1F);
        return (uint16_t)(0x8000u | ((uint16_t)r5 << 10) | ((uint16_t)g5 << 5) | b5);
    }

    void drawBgPixel(int bg, int line, int x, uint32_t pixel);
    void drawObjPixel(int line, int x, uint32_t pixel, int8_t priority);

    template <bool gbaMode> void drawText(int bg, int line);
    template <bool gbaMode> void drawAffine(int bg, int line);
    void drawExtended(int bg, int line);
    void drawExtendedGba(int bg, int line);
    void drawLarge(int bg, int line);
    template <bool gbaMode> void drawObjects(int line, bool window);
};
