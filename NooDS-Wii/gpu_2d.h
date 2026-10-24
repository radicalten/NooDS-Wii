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
    void buildWindowMask(int line);
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
    void drawExtended(int bg, int line);
    void drawExtendedGba(int bg, int line);
    void drawLarge(int bg, int line);
    template <bool gbaMode> void drawObjects(int line, bool window);
};
