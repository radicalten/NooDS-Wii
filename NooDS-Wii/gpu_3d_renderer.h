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
#include <vector>

extern "C" {
    #include <tuxedo/thread.h>
}

class Core;
struct Vertex;
struct _Polygon;

class Gpu3DRenderer {
public:
    Gpu3DRenderer(Core *core);
    ~Gpu3DRenderer();

    void saveState(FILE *file);
    void loadState(FILE *file);

    void drawScanline(int line);
    
    // Returns native 16-bit RGB5A3 pointers directly to avoid intermediate conversions
    uint16_t *getLine(int line);

    uint16_t readDisp3DCnt() { return disp3DCnt; }

    void writeDisp3DCnt(uint16_t mask, uint16_t value);
    void writeEdgeColor(int index, uint16_t mask, uint16_t value);
    void writeClearColor(uint32_t mask, uint32_t value);
    void writeClearDepth(uint16_t mask, uint16_t value);
    void writeFogColor(uint32_t mask, uint32_t value);
    void writeFogOffset(uint16_t mask, uint16_t value);
    void writeFogTable(int index, uint8_t value);
    void writeToonTable(int index, uint16_t mask, uint16_t value);

private:
    Core *core;

    bool resShift = false;
    uint16_t framebuffer[2][256 * 192 * 4] = {}; // Native RGB5A3
    int32_t depthBuffer[2][256 * 192 * 4] = {};
    uint32_t attribBuffer[2][256 * 192 * 4] = {};
    uint8_t stencilBuffer[256 * 192 * 4] = {};
    bool stencilClear[256 * 2] = {};

    int polygonTop[2048] = {};
    int polygonBot[2048] = {};

    // Tuxedo Thread Structures
    uint8_t activeThreads = 0;
    KThread threads[4]; 
    uint8_t* threadStacks[4] = {};
    volatile int ready[192 * 2]; 

    uint16_t disp3DCnt = 0;
    uint16_t edgeColor[8] = {};
    uint32_t clearColor = 0;
    uint16_t clearDepth = 0;
    uint32_t fogColor = 0;
    uint16_t fogOffset = 0;
    uint8_t fogTable[32] = {};
    uint16_t toonTable[32] = {};

    // Pack internal RGBA6 word -> RGB5A3 uint16_t
    static inline uint16_t rgba6ToRgb5a3(uint32_t c) {
        uint8_t a = (c >> 18) & 0x3F;
        uint8_t r = (c >>  0) & 0x3F;
        uint8_t g = (c >>  6) & 0x3F;
        uint8_t b = (c >> 12) & 0x3F;

        if (a >= 0x3F) {
            // Opaque: 1_RRRRR_GGGGG_BBBBB  (6-bit -> 5-bit)
            return static_cast<uint16_t>(
                0x8000u |
                ((r >> 1) << 10) |
                ((g >> 1) <<  5) |
                ((b >> 1) <<  0));
        } else {
            // Translucent: 0_AAA_RRRR_GGGG_BBBB (6->3 alpha, 6->4 color)
            return static_cast<uint16_t>(
                ((a >> 3) << 12) |
                ((r >> 2) <<  8) |
                ((g >> 2) <<  4) |
                ((b >> 2) <<  0));
        }
    }

    // Read framebuffer pixel back as RGBA6 for blending operations
    inline uint32_t fbGet(int layer, int idx) const {
        uint16_t p = framebuffer[layer][idx];
        if (p & 0x8000u) {
            uint8_t r5 = (p >> 10) & 0x1F;
            uint8_t g5 = (p >>  5) & 0x1F;
            uint8_t b5 = (p >>  0) & 0x1F;
            uint8_t r6 = (r5 << 1) | (r5 >> 4);
            uint8_t g6 = (g5 << 1) | (g5 >> 4);
            uint8_t b6 = (b5 << 1) | (b5 >> 4);
            return (0x3Fu << 18) | (b6 << 12) | (g6 << 6) | r6;
        } else {
            uint8_t a3 = (p >> 12) & 0x07;
            uint8_t r4 = (p >>  8) & 0x0F;
            uint8_t g4 = (p >>  4) & 0x0F;
            uint8_t b4 = (p >>  0) & 0x0F;
            uint8_t a6 = (a3 << 3) | (a3 >> 0); 
            uint8_t r6 = (r4 << 2) | (r4 >> 2); 
            uint8_t g6 = (g4 << 2) | (g4 >> 2);
            uint8_t b6 = (b4 << 2) | (b4 >> 2);
            return (a6 << 18) | (b6 << 12) | (g6 << 6) | r6;
        }
    }

    inline bool fbHasAlpha(int layer, int idx) const {
        uint16_t p = framebuffer[layer][idx];
        if (p & 0x8000u) return true;           
        return ((p >> 12) & 0x07) != 0;         
    }

    inline bool fbIsOpaque(int layer, int idx) const {
        return (framebuffer[layer][idx] & 0x8000u) != 0;
    }

    static uint32_t rgba5ToRgba6(uint32_t color);

    uint16_t *getLine1(int line);

    static sptr drawThreadedEntryPoint(void* arg);
    void drawThreaded(int thread);
    void drawScanline1(int line);
    void finishScanline(int line);

    uint8_t *getTexture(uint32_t address);
    uint8_t *getPalette(uint32_t address);

    static uint32_t interpolateLinear(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2);
    static uint32_t interpolateLinRev(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2);
    static uint32_t interpolateFactor(uint32_t factor, uint32_t shift, uint32_t v1, uint32_t v2);
    static uint32_t interpolateColor(uint32_t c1, uint32_t c2, uint32_t x1, uint32_t x, uint32_t x2);

    uint32_t readTexture(_Polygon *polygon, int s, int t);
    void drawPolygon(int line, int polygonIndex);
};
