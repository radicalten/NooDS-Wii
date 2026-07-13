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

class Gpu2D {
public:
    Gpu2D(Core *core, bool engine);
    void saveState(FILE *file);
    void loadState(FILE *file);

    void reloadRegisters();
    void updateWindows(int line);
    void drawGbaScanline(int line);
    void drawScanline(int line);

    // Framebuffer now stores native RGB5A3 uint16_t pixels directly
    uint16_t *getFramebuffer() { return framebuffer; }

    // getRawLine() still returns uint32_t* (layers[0] in RGB5/RGB6 internal format)
    // Used only by gpu.cpp display-capture path before final conversion
    uint32_t *getRawLine() { return layers[0]; }

    uint32_t readDispCnt()      { return dispCnt; }
    uint16_t readBgCnt(int bg)  { return bgCnt[bg]; }
    uint16_t readWinIn()        { return winIn; }
    uint16_t readWinOut()       { return winOut; }
    uint16_t readBldCnt()       { return bldCnt; }
    uint16_t readBldAlpha()     { return bldAlpha; }
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
    Core *core;
    bool engine;

    uint32_t bgVramAddr, objVramAddr;
    uint8_t *palette, *oam;
    uint8_t **extPalettes;

    // Native RGB5A3 framebuffer — each pixel is a uint16_t GX word
    uint16_t framebuffer[256 * 192] = {};

    // Separate object-window flag buffer replaces the old BIT(24) trick
    // that relied on spare bits in the uint32_t framebuffer.
    // Set to non-zero for pixels covered by an OBJ-window sprite.
    uint8_t objWinBuffer[256] = {};

    // Working layers remain uint32_t (RGB5 / RGB6 internal format)
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

    // RGB5 → RGB6 for internal blending (DS 2D engine pipeline)
    static uint32_t rgb5ToRgb6(uint32_t color);

    // RGB6 (18-bit internal) → RGB5A3 opaque GX word
    // Input:  bits[17:12]=b6, bits[11:6]=g6, bits[5:0]=r6
    // Output: 1_RRRRR_GGGGG_BBBBB
    static inline uint16_t rgb6ToRgb5A3(uint32_t rgb6) {
        uint8_t r5 = (uint8_t)((rgb6 >>  1) & 0x1F);
        uint8_t g5 = (uint8_t)((rgb6 >>  7) & 0x1F);
        uint8_t b5 = (uint8_t)((rgb6 >> 13) & 0x1F);
        return (uint16_t)(0x8000u
            | ((uint16_t)r5 << 10)
            | ((uint16_t)g5 <<  5)
            | b5);
    }

    // Raw DS palette word (0_bbbbb_ggggg_rrrrr) → RGB5A3 opaque GX word
    static inline uint16_t rgb5PalToRgb5A3(uint32_t palColor) {
        uint8_t r5 = (uint8_t)( palColor        & 0x1F);
        uint8_t g5 = (uint8_t)((palColor >>  5) & 0x1F);
        uint8_t b5 = (uint8_t)((palColor >> 10) & 0x1F);
        return (uint16_t)(0x8000u
            | ((uint16_t)r5 << 10)
            | ((uint16_t)g5 <<  5)
            | b5);
    }

    // GBA blended RGB5 value (same bit layout as palette, no flag bits) → RGB5A3
    static inline uint16_t rgb5BlendToRgb5A3(uint32_t blended) {
        uint8_t r5 = (uint8_t)( blended        & 0x1F);
        uint8_t g5 = (uint8_t)((blended >>  5) & 0x1F);
        uint8_t b5 = (uint8_t)((blended >> 10) & 0x1F);
        return (uint16_t)(0x8000u
            | ((uint16_t)r5 << 10)
            | ((uint16_t)g5 <<  5)
            | b5);
    }

    // Unpack an RGB5A3 GX word back to a 5-bit-per-channel value for
    // master-brightness arithmetic (only called after output stage)
    static inline uint16_t rgb5a3BrightnessUp(uint16_t px, uint8_t factor) {
        uint8_t r = (px >> 10) & 0x1F;
        uint8_t g = (px >>  5) & 0x1F;
        uint8_t b =  px        & 0x1F;
        r = (uint8_t)(r + (uint8_t)((31 - r) * factor / 16));
        g = (uint8_t)(g + (uint8_t)((31 - g) * factor / 16));
        b = (uint8_t)(b + (uint8_t)((31 - b) * factor / 16));
        return (uint16_t)(0x8000u | ((uint16_t)r << 10) | ((uint16_t)g << 5) | b);
    }

    static inline uint16_t rgb5a3BrightnessDown(uint16_t px, uint8_t factor) {
        uint8_t r = (px >> 10) & 0x1F;
        uint8_t g = (px >>  5) & 0x1F;
        uint8_t b =  px        & 0x1F;
        r = (uint8_t)(r - (uint8_t)(r * factor / 16));
        g = (uint8_t)(g - (uint8_t)(g * factor / 16));
        b = (uint8_t)(b - (uint8_t)(b * factor / 16));
        return (uint16_t)(0x8000u | ((uint16_t)r << 10) | ((uint16_t)g << 5) | b);
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
