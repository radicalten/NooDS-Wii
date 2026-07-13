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
        Buffers &b = framebuffers.front();
        delete[] b.framebuffer;
        delete[] b.hiRes3D;
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

bool Gpu::getFrame(uint16_t *out, bool gbaCrop) {
    if (!ready) return false;

    Buffers &buffers = framebuffers.front();

    if (gbaCrop) {
        // GBA crop: 240×160 from the 256×160 framebuffer (offset 0)
        for (int y = 0; y < 160; y++)
            for (int x = 0; x < 240; x++)
                out[y * 240 + x] = buffers.framebuffer[y * 256 + x];
    }
    else if (core->gbaMode) {
        // Full GBA display with NDS border fill
        int      offset = (powCnt1 & BIT(15)) ? 0 : (256 * 192);
        uint32_t base   = 0x6800000 + gbaBlock * 0x20000;
        gbaBlock = !gbaBlock;

        for (int y = 0; y < 192; y++) {
            for (int x = 0; x < 256; x++) {
                if (x >= 8 && x < 248 && y >= 16 && y < 176) {
                    out[offset + y * 256 + x] =
                        buffers.framebuffer[(y - 16) * 256 + (x - 8)];
                }
                else {
                    // Border: raw DS RGB5 from VRAM → RGB5A3
                    uint16_t raw = core->memory.read<uint16_t>(
                        0, base + (y * 256 + x) * 2);
                    uint8_t r5 = ( raw        & 0x1F);
                    uint8_t g5 = ((raw >>  5) & 0x1F);
                    uint8_t b5 = ((raw >> 10) & 0x1F);
                    out[offset + y * 256 + x] = (uint16_t)(
                        0x8000u
                        | ((uint16_t)r5 << 10)
                        | ((uint16_t)g5 <<  5)
                        |  (uint16_t)b5);
                }
            }
        }
        // Clear the unused screen half
        memset(&out[256 * 192 - offset], 0,
               256 * 192 * sizeof(uint16_t));
    }
    else {
        // Normal NDS mode: copy both screens (top + bottom) as RGB5A3
        for (int i = 0; i < 256 * 192 * 2; i++)
            out[i] = buffers.framebuffer[i];
    }

    // Screen-ghost effect (operates on uint16_t RGB5A3 words)
    if (Settings::screenGhost) {
        static uint16_t prev[256 * 192 * 2];
        int pixels = gbaCrop ? (240 * 160)
                   : (core->gbaMode ? (256 * 192)
                                    : (256 * 192 * 2));

        for (int i = 0; i < pixels; i++) {
            // Average the R5, G5, B5 channels between frames
            uint16_t a = prev[i];
            uint16_t b = out[i];

            // Both must be opaque (bit15=1) for a meaningful average
            // If either is transparent, just pass through current frame
            if ((a & 0x8000u) && (b & 0x8000u)) {
                uint8_t r = (uint8_t)((((a >> 10) & 0x1F)
                           + ((b >> 10) & 0x1F)) >> 1);
                uint8_t g = (uint8_t)((((a >>  5) & 0x1F)
                           + ((b >>  5) & 0x1F)) >> 1);
                uint8_t ch = (uint8_t)((( a        & 0x1F)
                           + ( b        & 0x1F)) >> 1);
                prev[i] = out[i];
                out[i]  = (uint16_t)(0x8000u
                         | ((uint16_t)r << 10)
                         | ((uint16_t)g <<  5)
                         | ch);
            } else {
                prev[i] = out[i];
            }
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
            while (drawing != 0) KThreadYield();
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
            // 256×160 RGB5A3 pixels
            buffers.framebuffer = new uint16_t[256 * 160];
            memcpy(buffers.framebuffer,
                   core->gpu2D[0].getFramebuffer(),
                   256 * 160 * sizeof(uint16_t));

            PPCIrqState st = PPCIrqLockByMsr();
            framebuffers.push(buffers);
            ready = true;
            PPCIrqUnlockByMsr(st);
        }

        if (frames++ >= Settings::frameskip) frames = 0;
        core->endFrame();
        break;

    case 227:
        dispStat[1] &= ~BIT(0);
        break;

    case 228:
        vCount = 0;
        core->gpu2D[0].reloadRegisters();

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

    if (vCount < 160 && gpuThreadStack) drawing = 1;

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
            while (drawing == 1) KThreadYield();

            PPCIrqState st      = PPCIrqLockByMsr();
            int current_drawing = drawing;
            drawing             = 3;
            PPCIrqUnlockByMsr(st);

            switch (current_drawing) {
            case 2:
                core->gpu2D[1].drawScanline(vCount);
                // fall through
            case 3:
                while (drawing != 0) KThreadYield();
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
                // Source: 3D renderer (now uint16_t*) or 2D raw line (uint32_t*)
                bool use3D    = (dispCapCnt & BIT(24)) != 0;
                bool resShift = (Settings::highRes3D && use3D);

                if (use3D) {
                    uint16_t *src3d =
                        core->gpu3DRenderer.getLine(vCount);
                    for (int i = 0; i < width; i++) {
                        // Convert RGB5A3 → RGB5 for capture memory
                        uint16_t rgb5 = rgb5a3ToRgb5(src3d[i << resShift]);
                        core->memory.write<uint16_t>(
                            0,
                            base + ((writeOffset + i * 2) & 0x1FFFF),
                            rgb5);
                    }
                }
                else {
                    uint32_t *src2d = core->gpu2D[0].getRawLine();
                    for (int i = 0; i < width; i++) {
                        core->memory.write<uint16_t>(
                            0,
                            base + ((writeOffset + i * 2) & 0x1FFFF),
                            rgb6ToRgb5(src2d[i]));
                    }
                }
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

                bool use3D    = (dispCapCnt & BIT(24)) != 0;
                bool resShift = (Settings::highRes3D && use3D);

                uint32_t readOffset = ((dispCapCnt & 0x0C000000) >> 11) +
                    vCount * width * 2;

                uint8_t eva = (uint8_t)std::min(
                    (dispCapCnt >> 0) & 0x1F, 16U);
                uint8_t evb = (uint8_t)std::min(
                    (dispCapCnt >> 8) & 0x1F, 16U);

                for (int i = 0; i < width; i++) {
                    uint16_t c1;
                    if (use3D) {
                        uint16_t *src3d =
                            core->gpu3DRenderer.getLine(vCount);
                        c1 = rgb5a3ToRgb5(src3d[i << resShift]);
                    }
                    else {
                        c1 = rgb6ToRgb5(core->gpu2D[0].getRawLine()[i]);
                    }

                    uint16_t c2 = core->memory.read<uint16_t>(
                        0, base + ((readOffset + i * 2) & 0x1FFFF));

                    uint8_t r = (uint8_t)std::min(
                        (((c1 >>  0) & 0x1F) * eva +
                         ((c2 >>  0) & 0x1F) * evb) / 16, 31);
                    uint8_t g = (uint8_t)std::min(
                        (((c1 >>  5) & 0x1F) * eva +
                         ((c2 >>  5) & 0x1F) * evb) / 16, 31);
                    uint8_t b = (uint8_t)std::min(
                        (((c1 >> 10) & 0x1F) * eva +
                         ((c2 >> 10) & 0x1F) * evb) / 16, 31);

                    uint16_t color = (uint16_t)(BIT(15)
                        | (b << 10) | (g << 5) | r);
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
            // 256×192×2 RGB5A3 pixels (top + bottom screen)
            buffers.framebuffer = new uint16_t[256 * 192 * 2];

            if (powCnt1 & BIT(0)) {
                if (powCnt1 & BIT(15)) {
                    memcpy(&buffers.framebuffer[0],
                           core->gpu2D[0].getFramebuffer(),
                           256 * 192 * sizeof(uint16_t));
                    memcpy(&buffers.framebuffer[256 * 192],
                           core->gpu2D[1].getFramebuffer(),
                           256 * 192 * sizeof(uint16_t));
                }
                else {
                    memcpy(&buffers.framebuffer[0],
                           core->gpu2D[1].getFramebuffer(),
                           256 * 192 * sizeof(uint16_t));
                    memcpy(&buffers.framebuffer[256 * 192],
                           core->gpu2D[0].getFramebuffer(),
                           256 * 192 * sizeof(uint16_t));
                }
            }
            else {
                // Both LCDs off: opaque black RGB5A3
                for (int i = 0; i < 256 * 192 * 2; i++)
                    buffers.framebuffer[i] = 0x8000u;
            }

            // hi-res 3D: getLine() now returns uint16_t*
            if (Settings::highRes3D &&
                (core->gpu2D[0].readDispCnt() & BIT(3))) {
                // 256×192×4 pixels at full hi-res
                const int hiResPixels = 256 * 192 * 4;
                buffers.hiRes3D = new uint16_t[hiResPixels];
                uint16_t *line0 = core->gpu3DRenderer.getLine(0);
                memcpy(buffers.hiRes3D, line0,
                       hiResPixels * sizeof(uint16_t));
                buffers.top3D = (powCnt1 & BIT(15));
            }

            PPCIrqState st = PPCIrqLockByMsr();
            framebuffers.push(buffers);
            ready = true;
            PPCIrqUnlockByMsr(st);
        }

        if (frames++ >= Settings::frameskip) frames = 0;
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

    if (vCount < 192 && gpuThreadStack) drawing = 1;

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

        PPCIrqState st = PPCIrqLockByMsr();
        int expected   = 2;
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
    mask         &= 0xFFB8;
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
