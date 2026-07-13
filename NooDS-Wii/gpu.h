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

    // out[] receives uint16_t RGB5A3 words packed into uint32_t slots
    // (bits[15:0] valid; bits[31:16] zero) in normal mode,
    // or full ABGR8 uint32_t in high-res / screen-filter mode.
    bool getFrame(uint16_t *out, bool gbaCrop);

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
        // 2D engine output: native RGB5A3 uint16_t pixels
        uint16_t *framebuffer = nullptr;

        // 3D renderer hi-res output: RGB5A3 uint16_t pixels
        // (Gpu3DRenderer::framebuffer is already uint16_t)
        uint16_t *hiRes3D     = nullptr;

        bool top3D = false;
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

    // Unpack RGB5A3 → ABGR8 for high-res / screen-ghost paths
    static inline uint32_t rgb5a3ToAbgr8(uint16_t px) {
        if (px & 0x8000u) {
            // Opaque: 1_RRRRR_GGGGG_BBBBB
            uint8_t r5 = (px >> 10) & 0x1F;
            uint8_t g5 = (px >>  5) & 0x1F;
            uint8_t b5 =  px        & 0x1F;
            uint8_t r8 = (r5 << 3) | (r5 >> 2);
            uint8_t g8 = (g5 << 3) | (g5 >> 2);
            uint8_t b8 = (b5 << 3) | (b5 >> 2);
            return (0xFFu << 24) | ((uint32_t)b8 << 16)
                 | ((uint32_t)g8 << 8) | r8;
        } else {
            // Translucent: 0_AAA_RRRR_GGGG_BBBB
            uint8_t a3 = (px >> 12) & 0x07;
            uint8_t r4 = (px >>  8) & 0x0F;
            uint8_t g4 = (px >>  4) & 0x0F;
            uint8_t b4 =  px        & 0x0F;
            uint8_t a8 = (a3 << 5) | (a3 << 2) | (a3 >> 1);
            uint8_t r8 = (r4 << 4) | r4;
            uint8_t g8 = (g4 << 4) | g4;
            uint8_t b8 = (b4 << 4) | b4;
            return ((uint32_t)a8 << 24) | ((uint32_t)b8 << 16)
                 | ((uint32_t)g8 << 8) | r8;
        }
    }

    // RGB6 (3D internal) → ABGR8, used only by hi-res 3D compositing
    static inline uint32_t rgb6ToAbgr8(uint32_t color) {
        uint8_t r = (uint8_t)(((color >>  0) & 0x3F) * 255 / 63);
        uint8_t g = (uint8_t)(((color >>  6) & 0x3F) * 255 / 63);
        uint8_t b = (uint8_t)(((color >> 12) & 0x3F) * 255 / 63);
        return (0xFFu << 24) | ((uint32_t)b << 16)
             | ((uint32_t)g << 8) | r;
    }

    // RGB6 → RGB5 for display capture
    static inline uint16_t rgb6ToRgb5(uint32_t color) {
        uint8_t r = (uint8_t)((color >>  1) & 0x1F);
        uint8_t g = (uint8_t)((color >>  7) & 0x1F);
        uint8_t b = (uint8_t)((color >> 13) & 0x1F);
        return (uint16_t)(BIT(15) | (b << 10) | (g << 5) | r);
    }

    // Unpack stored RGB5A3 pixel (from uint16_t fb slot) back to RGB6
    // for display-capture blending.  Only called when the source is
    // a 2D layer line (getRawLine) which is still uint32_t RGB5/RGB6.
    // This helper is used when source IS the 3D renderer line (uint16_t).
    static inline uint16_t rgb5a3ToRgb5(uint16_t px) {
        // Opaque pixel (bit15=1): already has R5 G5 B5 in the right positions
        // for a DS RGB5 word — just return it as-is with bit15 set.
        if (px & 0x8000u) return px; // 1_RRRRR_GGGGG_BBBBB
        // Translucent: recover 4-bit channels, shift to 5-bit
        uint8_t r = ((px >>  8) & 0x0F) << 1;
        uint8_t g = ((px >>  4) & 0x0F) << 1;
        uint8_t b = ( px        & 0x0F) << 1;
        return (uint16_t)(BIT(15) | (b << 10) | (g << 5) | r);
    }

    void drawGbaThreaded();
    void drawThreaded();

    static sptr GpuThreadEntry(void *arg);
};
