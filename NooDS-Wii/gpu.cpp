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

#include <cstring>
#include "core.h"

Gpu::Gpu(Core *core): core(core) {
    ready   = false;
    running = false;
    drawing = 0;
}

Gpu::~Gpu() {
    if (gpuThreadStack) {
        running = false;
        KThreadJoin(&gpuThread);
        delete[] gpuThreadStack;
    }

    while (!framebuffers.empty()) {
        Buffers &buffers = framebuffers.front();
        delete[] buffers.framebuffer;
        delete[] buffers.hiRes3D;
        framebuffers.pop();
    }
}

sptr Gpu::GpuThreadEntry(void *arg) {
    Gpu *gpuObj = (Gpu*)arg;
    if (gpuObj->core->gbaMode)
        gpuObj->drawGbaThreaded();
    else
        gpuObj->drawThreaded();
    return 0;
}

void Gpu::saveState(FILE *file) {
    fwrite(dispStat,    2, sizeof(dispStat) / 2, file);
    fwrite(&vCount,     sizeof(vCount),     1, file);
    fwrite(&dispCapCnt, sizeof(dispCapCnt), 1, file);
    fwrite(&powCnt1,    sizeof(powCnt1),    1, file);
}

void Gpu::loadState(FILE *file) {
    fread(dispStat,    2, sizeof(dispStat) / 2, file);
    fread(&vCount,     sizeof(vCount),     1, file);
    fread(&dispCapCnt, sizeof(dispCapCnt), 1, file);
    fread(&powCnt1,    sizeof(powCnt1),    1, file);
}

bool Gpu::getFrame(uint32_t *out, bool gbaCrop) {
    if (!ready)
        return false;

    Buffers &buffers = framebuffers.front();
    if (gbaCrop) {
        if (Settings::highRes3D || Settings::screenFilter == 1) {
            // High-res output: scale 1× pixels to 2×2 blocks, ABGR8 output
            for (int y = 0; y < 160; y++) {
                uint32_t *line = &out[y * 240 * 4];
                for (int x = 0; x < 240; x++) {
                    uint32_t abgr = rgb5a3ToAbgr8(
                        buffers.framebuffer[y * 256 + x]);
                    line[x * 2]     = abgr;
                    line[x * 2 + 1] = abgr;
                }
                memcpy(&line[240 * 2], line, 240 * 8);
            }
        }
        else {
            // Normal output: pass RGB5A3 words straight through.
            // wii_video::TileImageRGB5A3 reads bits[15:0] of each slot.
            for (int y = 0; y < 160; y++)
                for (int x = 0; x < 240; x++)
                    out[y * 240 + x] = buffers.framebuffer[y * 256 + x];
        }
    }
    else if (core->gbaMode) {
        int      offset   = (powCnt1 & BIT(15)) ? 0 : (256 * 192);
        uint32_t base     = 0x6800000 + gbaBlock * 0x20000;
        gbaBlock          = !gbaBlock;

        if (Settings::highRes3D || Settings::screenFilter == 1) {
            // High-res ABGR8 path
            for (int y = 0; y < 192; y++) {
                uint32_t *line = &out[offset * 4 + y * 256 * 4];
                for (int x = 0; x < 256; x++) {
                    uint32_t abgr;
                    if (x >= 8 && x < 248 && y >= 16 && y < 176) {
                        // Inner GBA pixels: decode from RGB5A3
                        abgr = rgb5a3ToAbgr8(
                            buffers.framebuffer[(y - 16) * 256 + x - 8]);
                    }
                    else {
                    // Border pixels: raw RGB5 from VRAM → GX RGB5A3
                    uint16_t raw = core->memory.read<uint16_t>(
                        0, base + (y * 256 + x) * 2);
                    // DS RGB5: bits[4:0]=R, bits[9:5]=G, bits[14:10]=B
                    uint8_t r5 = ( raw        & 0x1F);
                    uint8_t g5 = ((raw >>  5) & 0x1F);
                    uint8_t b5 = ((raw >> 10) & 0x1F);
                    // GX RGB5A3 opaque: 1_RRRRR_GGGGG_BBBBB
                        out[offset + y * 256 + x] = (uint32_t)(
                        0x8000u
                        | ((uint32_t)r5 << 10)   // R → bits[14:10]
                        | ((uint32_t)g5 <<  5)   // G → bits[9:5]
                        |  (uint32_t)b5);         // B → bits[4:0]
                    }
                    line[x * 2]     = abgr;
                    line[x * 2 + 1] = abgr;
                }
                memcpy(&line[256 * 2], line, 256 * 8);
            }
            memset(&out[(256 * 192 - offset) * 4], 0,
                   256 * 192 * 4 * sizeof(uint32_t));
        }
        else {
            // Normal path: pass RGB5A3 for inner pixels; border = black RGB5A3
            for (int y = 0; y < 192; y++) {
                for (int x = 0; x < 256; x++) {
                    if (x >= 8 && x < 248 && y >= 16 && y < 176) {
                        out[offset + y * 256 + x] =
                            buffers.framebuffer[(y - 16) * 256 + x - 8];
                    }
                    else {
                        // Border: read raw RGB5 from VRAM, pack as RGB5A3
                        uint16_t raw = core->memory.read<uint16_t>(
                            0, base + (y * 256 + x) * 2);
                        uint8_t r5 = ( raw        & 0x1F);
                        uint8_t g5 = ((raw >>  5) & 0x1F);
                        uint8_t b5 = ((raw >> 10) & 0x1F);
                        // GX RGB5A3 opaque: 1_RRRRR_GGGGG_BBBBB
                        out[offset + y * 256 + x] = (uint32_t)(
                            0x8000u
                            | ((uint32_t)r5 << 10)
                            | ((uint32_t)g5 <<  5)
                            | b5);
                    }
                }
            }
            memset(&out[256 * 192 - offset], 0,
                   256 * 192 * sizeof(uint32_t));
        }
    }
    else {
        if (Settings::highRes3D || Settings::screenFilter == 1) {
            if (buffers.hiRes3D) {
                // High-res 3D overlay: mix 2D (RGB5A3) with 3D (RGB6)
                // Output is ABGR8 because pixel arithmetic is needed
                for (int y = 0; y < 192 * 2; y++) {
                    for (int x = 0; x < 256; x++) {
                        uint32_t val2d = buffers.framebuffer[y * 256 + x];
                        int i = (y * 2) * (256 * 2) + (x * 2);

                        if (val2d & BIT(26)) {
                            // 3D pixel: decode hiRes3D (RGB6) if present
                            auto decode3D = [&](int idx) -> uint32_t {
                                uint32_t v3 = buffers.hiRes3D[
                                    idx % (256 * 192 * 4)];
                                return rgb6ToAbgr8(
                                    (v3 & 0xFC0000) ? v3 : val2d);
                            };
                            out[i + 0]   = decode3D(i + 0);
                            out[i + 1]   = decode3D(i + 1);
                            out[i + 512] = decode3D(i + 512);
                            out[i + 513] = decode3D(i + 513);
                        }
                        else {
                            // 2D pixel: decode from RGB5A3
                            uint32_t abgr = rgb5a3ToAbgr8(val2d);
                            out[i + 0]   = abgr;
                            out[i + 1]   = abgr;
                            out[i + 512] = abgr;
                            out[i + 513] = abgr;
                        }
                    }
                }
            }
            else {
                // High-res without 3D overlay: scale 2D RGB5A3 to ABGR8 2×2
                for (int y = 0; y < 192 * 2; y++) {
                    uint32_t *line = &out[y * 256 * 4];
                    for (int x = 0; x < 256; x++) {
                        uint32_t abgr = rgb5a3ToAbgr8(
                            buffers.framebuffer[y * 256 + x]);
                        line[x * 2]     = abgr;
                        line[x * 2 + 1] = abgr;
                    }
                    memcpy(&line[256 * 2], line, 256 * 8);
                }
            }
        }
        else {
            // Normal 1× path: pass RGB5A3 words straight through.
            // TileImageRGB5A3 in wii_video reads bits[15:0] of each slot.
            for (int i = 0; i < 256 * 192 * 2; i++)
                out[i] = buffers.framebuffer[i];
        }
    }

    if (Settings::screenGhost &&
        (Settings::highRes3D || Settings::screenFilter == 1)) {
        static uint32_t prev[256 * 192 * 8];
        uint32_t width  = (gbaCrop ? 240 : 256)
                        << (Settings::highRes3D || Settings::screenFilter == 1);
        uint32_t height = (gbaCrop ? 160 : (192 * 2))
                        << (Settings::highRes3D || Settings::screenFilter == 1);
        uint32_t size   = width * height;

        for (uint32_t i = 0; i < size; i++) {
            uint8_t r = (uint8_t)((((prev[i] >>  0) & 0xFF) +
                                   ((out[i]  >>  0) & 0xFF)) >> 1);
            uint8_t g = (uint8_t)((((prev[i] >>  8) & 0xFF) +
                                   ((out[i]  >>  8) & 0xFF)) >> 1);
            uint8_t b = (uint8_t)((((prev[i] >> 16) & 0xFF) +
                                   ((out[i]  >> 16) & 0xFF)) >> 1);
            prev[i] = out[i];
            out[i]  = 0xFF000000u | ((uint32_t)b << 16)
                    | ((uint32_t)g << 8) | r;
        }
    }

    delete[] buffers.framebuffer;
    delete[] buffers.hiRes3D;

    PPCIrqState st = PPCIrqLockByMsr();
    framebuffers.pop();
    ready = !framebuffers.empty();
    PPCIrqUnlockByMsr(st);
    return true;
}

void Gpu::gbaScanline240() {
    if (vCount < 160) {
        if (gpuThreadStack) {
            while (drawing != 0)
                KThreadYield(); 
        }
        else if (frames == 0) {
            core->gpu2D[0].drawGbaScanline(vCount);
        }

        core->dma[1].trigger(2);
    }

    dispStat[1] |= BIT(1);

    if (dispStat[1] & BIT(4))
        core->interpreter[1].sendInterrupt(1);

    core->schedule(GBA_SCANLINE240, 308 * 4);
}

void Gpu::gbaScanline308() {
    dispStat[1] &= ~BIT(1);

    switch (++vCount) {
    case 160:
        if (gpuThreadStack) {
            running = false;
            KThreadJoin(&gpuThread);
            delete[] gpuThreadStack;
            gpuThreadStack = nullptr;
        }

        dispStat[1] |= BIT(0);

        if (dispStat[1] & BIT(3))
            core->interpreter[1].sendInterrupt(0);

        core->dma[1].trigger(1);

        if (frames == 0 && framebuffers.size() < 2) {
            Buffers buffers;
            // RGB5A3 words: 256×160 slots
            buffers.framebuffer = new uint32_t[256 * 160];
            memcpy(buffers.framebuffer,
                   core->gpu2D[0].getFramebuffer(),
                   256 * 160 * sizeof(uint32_t));

            PPCIrqState st = PPCIrqLockByMsr();
            framebuffers.push(buffers);
            ready = true;
            PPCIrqUnlockByMsr(st);
        }

        if (frames++ >= Settings::frameskip)
            frames = 0;

        core->endFrame();
        break;

    case 227:
        dispStat[1] &= ~BIT(0);
        break;

    case 228:
        vCount = 0;
        core->gpu2D[0].reloadRegisters();

        if (Settings::threaded2D && frames == 0 && !gpuThreadStack) {
            running       = true;
            gpuThreadStack = new u8[16384];
            KThreadPrepare(&gpuThread, GpuThreadEntry, this,
                           gpuThreadStack + 16384, 15);
            KThreadResume(&gpuThread);
        }
        break;
    }

    core->gpu2D[0].updateWindows(vCount);

    if (vCount < 160 && gpuThreadStack)
        drawing = 1;

    if (vCount == (dispStat[1] >> 8)) {
        dispStat[1] |= BIT(2);
        if (dispStat[1] & BIT(5))
            core->interpreter[1].sendInterrupt(2);
    }
    else if (dispStat[1] & BIT(2)) {
        dispStat[1] &= ~BIT(2);
    }

    core->schedule(GBA_SCANLINE308, 308 * 4);
}

void Gpu::scanline256() {
    if (vCount < 192) {
        if (gpuThreadStack) {
            while (drawing == 1)
                KThreadYield(); 

            PPCIrqState st        = PPCIrqLockByMsr();
            int current_drawing   = drawing;
            drawing               = 3;
            PPCIrqUnlockByMsr(st);

            switch (current_drawing) {
            case 2:
                core->gpu2D[1].drawScanline(vCount);
                // fall through
            case 3:
                while (drawing != 0)
                    KThreadYield(); 
                break;
            }
        }
        else if (frames == 0) {
            core->gpu2D[0].drawScanline(vCount);
            core->gpu2D[1].drawScanline(vCount);
        }

        core->dma[0].trigger(2);

        if (vCount == 0 && (dispCapCnt & BIT(31)))
            displayCapture = true;

        if (displayCapture) {
            static const uint16_t sizes[] = {
                128, 128, 256, 64, 256, 128, 256, 192
            };
            const uint16_t *size   = &sizes[(dispCapCnt >> 19) & 0x6];
            const uint16_t  width  = size[0];
            const uint16_t  height = size[1];

            uint32_t base        = 0x6800000 +
                                   ((dispCapCnt & 0x00030000) >> 16) * 0x20000;
            uint32_t writeOffset = ((dispCapCnt & 0x000C0000) >> 3) +
                                   vCount * width * 2;

            switch ((dispCapCnt & 0x60000000) >> 29) {
            case 0: {
                // Source is either 3D renderer (RGB6) or 2D raw line (RGB5/RGB6)
                // getRawLine() returns layers[0] which is pre-conversion RGB5/RGB6
                uint32_t *source = (dispCapCnt & BIT(24))
                    ? core->gpu3DRenderer.getLine(vCount)
                    : core->gpu2D[0].getRawLine();
                bool resShift = (Settings::highRes3D && (dispCapCnt & BIT(24)));

                for (int i = 0; i < width; i++)
                    core->memory.write<uint16_t>(
                        0,
                        base + ((writeOffset + i * 2) & 0x1FFFF),
                        rgb6ToRgb5(source[i << resShift]));
                break;
            }

            case 1: {
                if (dispCapCnt & BIT(25)) {
                    LOG_CRIT("Unimplemented display capture source: "
                             "display FIFO\n");
                    break;
                }

                uint32_t readOffset = ((dispCapCnt & 0x0C000000) >> 11) +
                                      vCount * width * 2;

                for (int i = 0; i < width; i++) {
                    uint16_t color = core->memory.read<uint16_t>(
                        0, base + ((readOffset + i * 2) & 0x1FFFF));
                    core->memory.write<uint16_t>(
                        0,
                        base + ((writeOffset + i * 2) & 0x1FFFF),
                        color);
                }
                break;
            }

            default: {
                if (dispCapCnt & BIT(25)) {
                    LOG_CRIT("Unimplemented display capture source: "
                             "display FIFO\n");
                    break;
                }

                uint32_t *source = (dispCapCnt & BIT(24))
                    ? core->gpu3DRenderer.getLine(vCount)
                    : core->gpu2D[0].getRawLine();
                bool resShift = (Settings::highRes3D && (dispCapCnt & BIT(24)));

                uint32_t readOffset = ((dispCapCnt & 0x0C000000) >> 11) +
                                      vCount * width * 2;

                uint8_t eva = (uint8_t)std::min((dispCapCnt >> 0) & 0x1F, 16U);
                uint8_t evb = (uint8_t)std::min((dispCapCnt >> 8) & 0x1F, 16U);

                for (int i = 0; i < width; i++) {
                    uint16_t c1 = rgb6ToRgb5(source[i << resShift]);
                    uint16_t c2 = core->memory.read<uint16_t>(
                        0, base + ((readOffset + i * 2) & 0x1FFFF));

                    uint8_t r = (uint8_t)std::min(
                        (((c1 >> 0) & 0x1F) * eva +
                         ((c2 >> 0) & 0x1F) * evb) / 16, 31);
                    uint8_t g = (uint8_t)std::min(
                        (((c1 >> 5) & 0x1F) * eva +
                         ((c2 >> 5) & 0x1F) * evb) / 16, 31);
                    uint8_t b = (uint8_t)std::min(
                        (((c1 >> 10) & 0x1F) * eva +
                         ((c2 >> 10) & 0x1F) * evb) / 16, 31);

                    uint16_t color = (uint16_t)(BIT(15) | (b << 10) |
                                                (g << 5) | r);
                    core->memory.write<uint16_t>(
                        0,
                        base + ((writeOffset + i * 2) & 0x1FFFF),
                        color);
                }
                break;
            }}

            if (vCount + 1 == height) {
                displayCapture = false;
                dispCapCnt    &= ~BIT(31);
            }
        }
    }

    if (frames == 0 && dirty3D &&
        (core->gpu2D[0].readDispCnt() & BIT(3)) &&
        ((vCount + 48) % 263) < 192) {
        if (vCount == 215) dirty3D = BIT(1);
        core->gpu3DRenderer.drawScanline((vCount + 48) % 263);
        if (vCount == 143) dirty3D &= ~BIT(1);
    }

    for (int i = 0; i < 2; i++) {
        dispStat[i] |= BIT(1);
        if (dispStat[i] & BIT(4))
            core->interpreter[i].sendInterrupt(1);
    }

    core->schedule(NDS_SCANLINE256, 355 * 6);
}

void Gpu::scanline355() {
    switch (++vCount) {
    case 192:
        if (gpuThreadStack) {
            running = false;
            KThreadJoin(&gpuThread);
            delete[] gpuThreadStack;
            gpuThreadStack = nullptr;
        }

        for (int i = 0; i < 2; i++) {
            dispStat[i] |= BIT(0);
            if (dispStat[i] & BIT(3))
                core->interpreter[i].sendInterrupt(0);
            core->dma[i].trigger(1);
        }

        if (core->gpu3D.shouldSwap())
            core->gpu3D.swapBuffers();

        if (frames == 0 && framebuffers.size() < 2) {
            Buffers buffers;
            // RGB5A3 words: 256×192×2 slots (top + bottom screen)
            buffers.framebuffer = new uint32_t[256 * 192 * 2];

            if (powCnt1 & BIT(0)) {
                if (powCnt1 & BIT(15)) {
                    memcpy(&buffers.framebuffer[0],
                           core->gpu2D[0].getFramebuffer(),
                           256 * 192 * sizeof(uint32_t));
                    memcpy(&buffers.framebuffer[256 * 192],
                           core->gpu2D[1].getFramebuffer(),
                           256 * 192 * sizeof(uint32_t));
                }
                else {
                    memcpy(&buffers.framebuffer[0],
                           core->gpu2D[1].getFramebuffer(),
                           256 * 192 * sizeof(uint32_t));
                    memcpy(&buffers.framebuffer[256 * 192],
                           core->gpu2D[0].getFramebuffer(),
                           256 * 192 * sizeof(uint32_t));
                }
            }
            else {
                // Both LCDs off: fill with opaque black RGB5A3
                for (int i = 0; i < 256 * 192 * 2; i++)
                    buffers.framebuffer[i] = 0x8000u; // 1_00000_00000_00000
            }

            // hiRes3D buffer remains RGB6 (Gpu3DRenderer unchanged)
            if (Settings::highRes3D &&
                (core->gpu2D[0].readDispCnt() & BIT(3))) {
                buffers.hiRes3D = new uint32_t[256 * 192 * 4];
                memcpy(buffers.hiRes3D,
                       core->gpu3DRenderer.getLine(0),
                       256 * 192 * 4 * sizeof(uint32_t));
                buffers.top3D = (powCnt1 & BIT(15));
            }

            PPCIrqState st = PPCIrqLockByMsr();
            framebuffers.push(buffers);
            ready = true;
            PPCIrqUnlockByMsr(st);
        }

        if (frames++ >= Settings::frameskip)
            frames = 0;

        core->actionReplay.applyCheats();
        core->endFrame();
        break;

    case 262:
        dispStat[0] &= ~BIT(0);
        dispStat[1] &= ~BIT(0);
        break;

    case 263:
        vCount = 0;
        core->gpu2D[0].reloadRegisters();
        core->gpu2D[1].reloadRegisters();

        if (Settings::threaded2D && frames == 0 && !gpuThreadStack) {
            running        = true;
            gpuThreadStack = new u8[16384];
            KThreadPrepare(&gpuThread, GpuThreadEntry, this,
                           gpuThreadStack + 16384, 15);
            KThreadResume(&gpuThread);
        }
        break;
    }

    core->gpu2D[0].updateWindows(vCount);
    core->gpu2D[1].updateWindows(vCount);

    if (vCount < 192 && gpuThreadStack)
        drawing = 1;

    for (int i = 0; i < 2; i++) {
        if (vCount == ((dispStat[i] >> 8) |
                       ((dispStat[i] & BIT(7)) << 1))) {
            dispStat[i] |= BIT(2);
            if (dispStat[i] & BIT(5))
                core->interpreter[i].sendInterrupt(2);
        }
        else if (dispStat[i] & BIT(2)) {
            dispStat[i] &= ~BIT(2);
        }

        dispStat[i] &= ~BIT(1);
    }

    core->schedule(NDS_SCANLINE355, 355 * 6);
}

void Gpu::drawGbaThreaded() {
    while (running) {
        while (drawing != 1) {
            if (!running) return;
            KThreadYield(); 
        }
        core->gpu2D[0].drawGbaScanline(vCount);
        drawing = 0;
    }
}

void Gpu::drawThreaded() {
    while (running) {
        while (drawing != 1) {
            if (!running) return;
            KThreadYield(); 
        }

        drawing = 2;
        core->gpu2D[0].drawScanline(vCount);

        PPCIrqState st  = PPCIrqLockByMsr();
        int expected    = 2;
        if (drawing == expected) {
            drawing = 3;
            PPCIrqUnlockByMsr(st);
            core->gpu2D[1].drawScanline(vCount);
        }
        else {
            PPCIrqUnlockByMsr(st);
        }

        drawing = 0;
    }
}

void Gpu::writeDispStat(bool cpu, uint16_t mask, uint16_t value) {
    mask       &= 0xFFB8;
    dispStat[cpu] = (dispStat[cpu] & ~mask) | (value & mask);
}

void Gpu::writeDispCapCnt(uint32_t mask, uint32_t value) {
    mask      &= 0xEF3F1F1F;
    dispCapCnt = (dispCapCnt & ~mask) | (value & mask);
}

void Gpu::writePowCnt1(uint16_t mask, uint16_t value) {
    mask    &= 0x820F;
    powCnt1  = (powCnt1 & ~mask) | (value & mask);
}
