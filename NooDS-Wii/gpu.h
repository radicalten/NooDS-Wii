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
#include <atomic>

extern "C" {
    #include <tuxedo/thread.h>
}

#include "defines.h"

class Core;

class Gpu {
    friend class ArmPpcJit;
public:
    Gpu(Core *core);
    ~Gpu();

    void saveState(FILE *file);
    void loadState(FILE *file);

    bool getFrame(uint32_t *out, bool gbaCrop);
    void invalidate3D() { dirty3D |= BIT(0); }
    // v1.112: finished DS frame written directly as two GX RGB565 textures (4x4 tiles)
    bool getFrameTex565(uint8_t *top, uint8_t *bottom);
    void peekFrame(uint32_t *out); // current 2D output as ABGR8 (test harness)

    void gbaHblankDma();
    void gbaScanline240();
    void gbaScanline308();
    void scanline256();
    void scanline355();

    uint16_t readDispStat(bool cpu) { return dispStat[cpu]; }
    uint16_t readVCount() { return vCount; }
    uint32_t readDispCapCnt() { return dispCapCnt; }
    bool captureMayRead() { return (dispCapCnt & (1u << 31)) != 0 || displayCapture; } // v1.118
    // A setting can change while a frame's worker is still winding down.
    bool has2DWorker() const { return gpuThreadStack != nullptr; }
    uint16_t readPowCnt1() { return powCnt1; }

    void writeDispStat(bool cpu, uint16_t mask, uint16_t value);
    void writeDispCapCnt(uint32_t mask, uint32_t value);
    void writePowCnt1(uint16_t mask, uint16_t value);

private:
    friend class ArmPpcJit; // guarded MMIO fast path reads the live DISPSTAT field
    Core *core;

    struct Buffers {
        uint32_t *framebuffer = nullptr;
        uint32_t *hiRes3D = nullptr;
        bool top3D = false;
        bool direct = false, topIsA = false; // v1.111: convert from the 2D engines directly
    };

    uint32_t *framePool[4] = {};
    uint8_t framePoolIdx = 0;
    // At most two frames are queued. A fixed ring avoids std::deque growth
    // and its large size() arithmetic on every deque block allocation.
    // Push/pop remain inside the existing Wii IRQ-protected regions.
    class FrameQueue {
    public:
        bool empty() const { return size() == 0; }
        unsigned size() const { return count.load(std::memory_order_relaxed); }
        Buffers &front() { return slots[head]; }
        void push(const Buffers &v) {
            const unsigned n = size();
            slots[(head + n) & 1u] = v;
            count.store(n + 1, std::memory_order_relaxed);
        }
        void pop() {
            const unsigned n = size();
            head = (head + 1) & 1u;
            count.store(n - 1, std::memory_order_relaxed);
        }
    private:
        Buffers slots[2]{};
        unsigned head = 0;
        std::atomic<unsigned> count{0};
    };
    static_assert(std::atomic<unsigned>::is_always_lock_free, "frame count must be lock-free");
    FrameQueue framebuffers;
    volatile bool ready;

    volatile bool running;
    volatile int drawing;
    KThread gpuThread;
    u8 *gpuThreadStack = nullptr;

    int frames = 0;
    bool gbaBlock = true;
    bool displayCapture = false;
    uint8_t dirty3D = 0;

    uint16_t dispStat[2] = {};
    uint16_t vCount = 0;
    uint32_t dispCapCnt = 0;
    uint16_t powCnt1 = 0;

    static uint32_t rgb5ToRgb8(uint32_t color);
    static uint32_t rgb6ToRgb8(uint32_t color);
    static uint16_t rgb6ToRgb5(uint32_t color);

    void drawGbaThreaded();
    void drawThreaded();

    static sptr GpuThreadEntry(void *arg);
};


namespace NoodsGx {
inline uint16_t paletteRgb5A3(uint16_t ds, bool transparent) {
    return transparent ? 0 : (uint16_t)(0x8000u | ((ds & 31u) << 10) |
                                      (ds & 0x3E0u) | ((ds >> 10) & 31u));
}
inline void packPalette(const uint8_t *src, uint8_t *dst, unsigned entries, bool trans0) {
    for (unsigned i = 0; i < entries; ++i) {
        const uint16_t v = paletteRgb5A3((uint16_t)(src[i*2] | (src[i*2+1] << 8)), trans0 && i == 0);
        dst[i*2] = v >> 8; dst[i*2+1] = v;
    }
}
inline void packIndices(const uint8_t *src, uint8_t *dst, unsigned w, unsigned h, unsigned fmt) {
    const unsigned shift = fmt == 2 ? 2 : (fmt == 3 ? 1 : 0);
    const unsigned bits = 8u >> shift, mask = (1u << bits) - 1u;
    for (unsigned ty = 0; ty < h; ty += 4)
        for (unsigned tx = 0; tx < w; tx += 8)
            for (unsigned y = 0; y < 4; ++y)
                for (unsigned x = 0; x < 8; ++x) {
                    const unsigned pos = (ty+y)*w+tx+x;
                    *dst++ = (src[pos >> shift] >> ((pos & ((1u << shift)-1u))*bits)) & mask;
                }
}
}

