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
#include <queue>

extern "C" {
    #include <tuxedo/thread.h>
}

#include "defines.h"

class Core;

class Gpu {
public:
    Gpu(Core *core);
    ~Gpu();

    void saveState(FILE *file);
    void loadState(FILE *file);

    bool getFrame(uint32_t *out, bool gbaCrop);
    void invalidate3D() { dirty3D |= BIT(0); }

    void gbaScanline240();
    void gbaScanline308();
    void scanline256();
    void scanline355();

    uint16_t readDispStat(bool cpu) { return dispStat[cpu]; }
    uint16_t readVCount()           { return vCount; }
    uint32_t readDispCapCnt()       { return dispCapCnt; }
    uint16_t readPowCnt1()          { return powCnt1; }

    void writeDispStat(bool cpu, uint16_t mask, uint16_t value);
    void writeDispCapCnt(uint32_t mask, uint32_t value);
    void writePowCnt1(uint16_t mask, uint16_t value);

private:
    Core *core;

    struct Buffers {
        uint32_t *framebuffer = nullptr; // RGB5A3 words from Gpu2D
        uint32_t *hiRes3D     = nullptr; // RGB6 words from Gpu3DRenderer
        bool      top3D       = false;
    };

    std::queue<Buffers> framebuffers;
    volatile bool ready;

    volatile bool running;
    volatile int  drawing;
    KThread gpuThread;
    u8     *gpuThreadStack = nullptr;

    int  frames         = 0;
    bool gbaBlock       = true;
    bool displayCapture = false;
    uint8_t dirty3D     = 0;

    uint16_t dispStat[2] = {};
    uint16_t vCount      = 0;
    uint32_t dispCapCnt  = 0;
    uint16_t powCnt1     = 0;

    static inline uint32_t rgb5a3ToAbgr8(uint32_t rgb5a3) {
        uint16_t px = (uint16_t)(rgb5a3 & 0xFFFF);
        uint8_t r5  = (px >> 10) & 0x1F;
        uint8_t g5  = (px >>  5) & 0x1F;
        uint8_t b5  =  px        & 0x1F;
        // Scale 5-bit → 8-bit: replicate top bits into low bits
        uint8_t r8  = (r5 << 3) | (r5 >> 2);
        uint8_t g8  = (g5 << 3) | (g5 >> 2);
        uint8_t b8  = (b5 << 3) | (b5 >> 2);
        return (0xFFu << 24) | ((uint32_t)b8 << 16) | ((uint32_t)g8 << 8) | r8;
    }

    // Convert RGB6 (3D renderer internal) → ABGR8, used only by hiRes3D path.
    static inline uint32_t rgb6ToAbgr8(uint32_t color) {
        uint8_t r = (uint8_t)(((color >>  0) & 0x3F) * 255 / 63);
        uint8_t g = (uint8_t)(((color >>  6) & 0x3F) * 255 / 63);
        uint8_t b = (uint8_t)(((color >> 12) & 0x3F) * 255 / 63);
        return (0xFFu << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    }

    // Convert RGB6 → RGB5 for display capture (reads from 3D renderer line,
    // which is still RGB6; Gpu2D::getRawLine() also still returns RGB5/RGB6).
    static inline uint16_t rgb6ToRgb5(uint32_t color) {
        uint8_t r = (uint8_t)((color >>  1) & 0x1F);
        uint8_t g = (uint8_t)((color >>  7) & 0x1F);
        uint8_t b = (uint8_t)((color >> 13) & 0x1F);
        return (uint16_t)(BIT(15) | (b << 10) | (g << 5) | r);
    }

    void drawGbaThreaded();
    void drawThreaded();

    static sptr GpuThreadEntry(void *arg);
};
