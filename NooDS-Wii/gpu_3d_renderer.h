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
    uint32_t *getLine(int line);

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
    uint32_t framebuffer[2][256 * 192 * 4] = {};
    int32_t depthBuffer[2][256 * 192 * 4] = {};
    uint32_t attribBuffer[2][256 * 192 * 4] = {};
    uint8_t stencilBuffer[256 * 192 * 4] = {};
    bool stencilClear[256 * 2] = {};

    int polygonTop[2048] = {};
    int polygonBot[2048] = {};

    // Tuxedo Thread Structures
    uint8_t activeThreads = 0;
    KThread threads[4]; // Supporting up to 4 parallel threads on the platform
    uint8_t* threadStacks[4] = {};
    volatile int ready[192 * 2]; // Thread safe synchronization indicators

    uint16_t disp3DCnt = 0;
    uint16_t edgeColor[8] = {};
    uint32_t clearColor = 0;
    uint16_t clearDepth = 0;
    uint32_t fogColor = 0;
    uint16_t fogOffset = 0;
    uint8_t fogTable[32] = {};
    uint16_t toonTable[32] = {};

    static uint32_t rgba5ToRgba6(uint32_t color);

    uint32_t *getLine1(int line);

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
