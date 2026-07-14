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
    ready = false;
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
    if (gpuObj->core->gbaMode) {
        gpuObj->drawGbaThreaded();
    } else {
        gpuObj->drawThreaded();
    }
    return 0;
}

void Gpu::saveState(FILE *file) {
    fwrite(dispStat, 2, sizeof(dispStat) / 2, file);
    fwrite(&vCount, sizeof(vCount), 1, file);
    fwrite(&dispCapCnt, sizeof(dispCapCnt), 1, file);
    fwrite(&powCnt1, sizeof(powCnt1), 1, file);
}

void Gpu::loadState(FILE *file) {
    fread(dispStat, 2, sizeof(dispStat) / 2, file);
    fread(&vCount, sizeof(vCount), 1, file);
    fread(&dispCapCnt, sizeof(dispCapCnt), 1, file);
    fread(&powCnt1, sizeof(powCnt1), 1, file);
}

uint32_t Gpu::rgb5ToRgb8(uint32_t color) {
    uint8_t r = (((color >> 0) & 0x1F) << 1) * 255 / 63;
    uint8_t g = (((color >> 5) & 0x1F) << 1) * 255 / 63;
    uint8_t b = (((color >> 10) & 0x1F) << 1) * 255 / 63;
    return (0xFF << 24) | (b << 16) | (g << 8) | r;
}

uint32_t Gpu::rgb6ToRgb8(uint32_t color) {
    uint8_t r = ((color >> 0) & 0x3F) * 255 / 63;
    uint8_t g = ((color >> 6) & 0x3F) * 255 / 63;
    uint8_t b = ((color >> 12) & 0x3F) * 255 / 63;
    return (0xFF << 24) | (b << 16) | (g << 8) | r;
}

uint16_t Gpu::rgb6ToRgb5(uint32_t color) {
    uint8_t r = ((color >> 0) & 0x3F) / 2;
    uint8_t g = ((color >> 6) & 0x3F) / 2;
    uint8_t b = ((color >> 12) & 0x3F) / 2;
    return BIT(15) | (b << 10) | (g << 5) | r;
}

bool Gpu::getFrame(uint32_t *out, bool gbaCrop) {
    if (!ready)
        return false;

    Buffers &buffers = framebuffers.front();

    if (gbaCrop) {
        if (Settings::highRes3D || Settings::screenFilter == 1) {
            for (int y = 0; y < 160; y++) {
                uint32_t *line = &out[y * 240 * 4];
                for (int x = 0; x < 240; x++)
                    line[x * 2] = line[x * 2 + 1] = rgb5ToRgb8(buffers.framebuffer[y * 256 + x]);
                memcpy(&line[240 * 2], line, 240 * 8);
            }
        }
        else {
            for (int y = 0; y < 160; y++)
                for (int x = 0; x < 240; x++)
                    out[y * 240 + x] = rgb5ToRgb8(buffers.framebuffer[y * 256 + x]);
        }
    }
    else if (core->gbaMode) {
        int offset = (powCnt1 & BIT(15)) ? 0 : (256 * 192);
        uint32_t base = 0x6800000 + gbaBlock * 0x20000;
        gbaBlock = !gbaBlock;

        if (Settings::highRes3D || Settings::screenFilter == 1) {
            for (int y = 0; y < 192; y++) {
                uint32_t *line = &out[offset * 4 + y * 256 * 4];
                for (int x = 0; x < 256; x++)
                    line[x * 2] = line[x * 2 + 1] = rgb5ToRgb8((x >= 8 && x < 248 && y >= 16 && y < 176) ? buffers.
                        framebuffer[(y - 16) * 256 + x - 8] : core->memory.read<uint16_t>(0, base + (y * 256 + x) * 2));
                memcpy(&line[256 * 2], line, 256 * 8);
            }

            memset(&out[(256 * 192 - offset) * 4], 0, 256 * 192 * 4 * sizeof(uint32_t));
        }
        else {
            for (int y = 0; y < 192; y++)
                for (int x = 0; x < 256; x++)
                    out[offset + y * 256 + x] = rgb5ToRgb8((x >= 8 && x < 248 && y >= 16 && y < 176) ? buffers.
                        framebuffer[(y - 16) * 256 + x - 8] : core->memory.read<uint16_t>(0, base + (y * 256 + x) * 2));

            memset(&out[256 * 192 - offset], 0, 256 * 192 * sizeof(uint32_t));
        }
    }
    else {
        if (Settings::highRes3D || Settings::screenFilter == 1) {
            if (buffers.hiRes3D) {
                for (int y = 0; y < 192 * 2; y++) {
                    for (int x = 0; x < 256; x++) {
                        uint32_t value = buffers.framebuffer[y * 256 + x];
                        int i = (y * 2) * (256 * 2) + (x * 2);
                        if (value & BIT(26)) {
                            uint32_t value2 = buffers.hiRes3D[(i + 0) % (256 * 192 * 4)];
                            out[i + 0] = rgb6ToRgb8((value2 & 0xFC0000) ? value2 : value);
                            value2 = buffers.hiRes3D[(i + 1) % (256 * 192 * 4)];
                            out[i + 1] = rgb6ToRgb8((value2 & 0xFC0000) ? value2 : value);
                            value2 = buffers.hiRes3D[(i + 512) % (256 * 192 * 4)];
                            out[i + 512] = rgb6ToRgb8((value2 & 0xFC0000) ? value2 : value);
                            value2 = buffers.hiRes3D[(i + 513) % (256 * 192 * 4)];
                            out[i + 513] = rgb6ToRgb8((value2 & 0xFC0000) ? value2 : value);
                        }
                        else {
                            uint32_t color = rgb6ToRgb8(value);
                            out[i + 0] = out[i + 1] = color;
                            out[i + 512] = out[i + 513] = color;
                        }
                    }
                }
            }
            else {
                for (int y = 0; y < 192 * 2; y++) {
                    uint32_t *line = &out[y * 256 * 4];
                    for (int x = 0; x < 256; x++)
                        line[x * 2] = line[x * 2 + 1] = rgb6ToRgb8(buffers.framebuffer[y * 256 + x]);
                    memcpy(&line[256 * 2], line, 256 * 8);
                }
            }
        }
        else {
            for (int i = 0; i < 256 * 192 * 2; i++)
                out[i] = rgb6ToRgb8(buffers.framebuffer[i]);
        }
    }

    delete[] buffers.framebuffer;
    delete[] buffers.hiRes3D;

    if (Settings::screenGhost) {
        static uint32_t prev[256 * 192 * 8];
        uint32_t width = (gbaCrop ? 240 : 256) << (Settings::highRes3D || Settings::screenFilter == 1);
        uint32_t height = (gbaCrop ? 160 : (192 * 2)) << (Settings::highRes3D || Settings::screenFilter == 1);
        uint32_t size = width * height;

        for (uint32_t i = 0; i < size; i++) {
            uint8_t r = (((prev[i] >> 0) & 0xFF) + ((out[i] >> 0) & 0xFF)) >> 1;
            uint8_t g = (((prev[i] >> 8) & 0xFF) + ((out[i] >> 8) & 0xFF)) >> 1;
            uint8_t b = (((prev[i] >> 16) & 0xFF) + ((out[i] >> 16) & 0xFF)) >> 1;
            prev[i] = out[i];
            out[i] = 0xFF000000 | (b << 16) | (g << 8) | r;
        }
    }

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
            buffers.framebuffer = new uint32_t[256 * 160];
            memcpy(buffers.framebuffer, core->gpu2D[0].getFramebuffer(), 256 * 160 * sizeof(uint32_t));

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
            running = true;
            gpuThreadStack = new u8[16384];
            KThreadPrepare(&gpuThread, GpuThreadEntry, this, gpuThreadStack + 16384, 15);
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

            PPCIrqState st = PPCIrqLockByMsr();
            int current_drawing = drawing;
            drawing = 3;
            PPCIrqUnlockByMsr(st);

            switch (current_drawing) {
            case 2:
                core->gpu2D[1].drawScanline(vCount);

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
            static const uint16_t sizes[] = { 128, 128, 256, 64, 256, 128, 256, 192 };
            const uint16_t *size = &sizes[(dispCapCnt >> 19) & 0x6];
            const uint16_t width = size[0];
            const uint16_t height = size[1];

            uint32_t base = 0x6800000 + ((dispCapCnt & 0x00030000) >> 16) * 0x20000;
            uint32_t writeOffset = ((dispCapCnt & 0x000C0000) >> 3) + vCount * width * 2;

            switch ((dispCapCnt & 0x60000000) >> 29) {
            case 0: {
                uint32_t *source = (dispCapCnt & BIT(24)) ? core->gpu3DRenderer.getLine(vCount) : core->gpu2D[0].getRawLine();
                bool resShift = (Settings::highRes3D && (dispCapCnt & BIT(24)));

                for (int i = 0; i < width; i++)
                    core->memory.write<uint16_t>(0, base + ((writeOffset + i * 2) & 0x1FFFF), rgb6ToRgb5(source[i << resShift]));
                break;
            }

            case 1: {
                if (dispCapCnt & BIT(25)) {
                    LOG_CRIT("Unimplemented display capture source: display FIFO\n");
                    break;
                }

                uint32_t readOffset = ((dispCapCnt & 0x0C000000) >> 11) + vCount * width * 2;

                for (int i = 0; i < width; i++) {
                    uint16_t color = core->memory.read<uint16_t>(0, base + ((readOffset + i * 2) & 0x1FFFF));
                    core->memory.write<uint16_t>(0, base + ((writeOffset + i * 2) & 0x1FFFF), color);
                }
                break;
            }

            default: {
                if (dispCapCnt & BIT(25)) {
                    LOG_CRIT("Unimplemented display capture source: display FIFO\n");
                    break;
                }

                uint32_t *source = (dispCapCnt & BIT(24)) ? core->gpu3DRenderer.getLine(vCount) : core->gpu2D[0].getRawLine();
                bool resShift = (Settings::highRes3D && (dispCapCnt & BIT(24)));

                uint32_t readOffset = ((dispCapCnt & 0x0C000000) >> 11) + vCount * width * 2;

                uint8_t eva = std::min((dispCapCnt >> 0) & 0x1F, 16U);
                uint8_t evb = std::min((dispCapCnt >> 8) & 0x1F, 16U);

                for (int i = 0; i < width; i++) {
                    uint16_t c1 = rgb6ToRgb5(source[i << resShift]);
                    uint16_t c2 = core->memory.read<uint16_t>(0, base + ((readOffset + i * 2) & 0x1FFFF));

                    uint8_t r = std::min((((c1 >> 0) & 0x1F) * eva + ((c2 >> 0) & 0x1F) * evb) / 16, 31);
                    uint8_t g = std::min((((c1 >> 5) & 0x1F) * eva + ((c2 >> 5) & 0x1F) * evb) / 16, 31);
                    uint8_t b = std::min((((c1 >> 10) & 0x1F) * eva + ((c2 >> 10) & 0x1F) * evb) / 16, 31);

                    uint16_t color = BIT(15) | (b << 10) | (g << 5) | r;
                    core->memory.write<uint16_t>(0, base + ((writeOffset + i * 2) & 0x1FFFF), color);
                }
                break;
            }}

            if (vCount + 1 == height) {
                displayCapture = false;
                dispCapCnt &= ~BIT(31);
            }
        }
    }

    if (frames == 0 && dirty3D && (core->gpu2D[0].readDispCnt() & BIT(3)) && ((vCount + 48) % 263) < 192) {
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
            buffers.framebuffer = new uint32_t[256 * 192 * 2];
            if (powCnt1 & BIT(0)) {
                if (powCnt1 & BIT(15)) {
                    memcpy(&buffers.framebuffer[0], core->gpu2D[0].getFramebuffer(), 256 * 192 * sizeof(uint32_t));
                    memcpy(&buffers.framebuffer[256 * 192], core->gpu2D[1].getFramebuffer(), 256 * 192 * sizeof(uint32_t));
                }
                else {
                    memcpy(&buffers.framebuffer[0], core->gpu2D[1].getFramebuffer(), 256 * 192 * sizeof(uint32_t));
                    memcpy(&buffers.framebuffer[256 * 192], core->gpu2D[0].getFramebuffer(), 256 * 192 * sizeof(uint32_t));
                }
            }
            else {
                memset(buffers.framebuffer, 0, 256 * 192 * 2 * sizeof(uint32_t));
            }

            if (Settings::highRes3D && (core->gpu2D[0].readDispCnt() & BIT(3))) {
                buffers.hiRes3D = new uint32_t[256 * 192 * 4];
                memcpy(buffers.hiRes3D, core->gpu3DRenderer.getLine(0), 256 * 192 * 4 * sizeof(uint32_t));
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
            running = true;
            gpuThreadStack = new u8[16384];
            KThreadPrepare(&gpuThread, GpuThreadEntry, this, gpuThreadStack + 16384, 15);
            KThreadResume(&gpuThread);
        }
        break;
    }

    core->gpu2D[0].updateWindows(vCount);
    core->gpu2D[1].updateWindows(vCount);

    if (vCount < 192 && gpuThreadStack)
        drawing = 1;

    for (int i = 0; i < 2; i++) {
        if (vCount == ((dispStat[i] >> 8) | ((dispStat[i] & BIT(7)) << 1))) {
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
        int expected = 2;
        if (drawing == expected) {
            drawing = 3;
            PPCIrqUnlockByMsr(st);
            core->gpu2D[1].drawScanline(vCount);
        } else {
            PPCIrqUnlockByMsr(st);
        }

        drawing = 0;
    }
}

void Gpu::writeDispStat(bool cpu, uint16_t mask, uint16_t value) {
    mask &= 0xFFB8;
    dispStat[cpu] = (dispStat[cpu] & ~mask) | (value & mask);
}

void Gpu::writeDispCapCnt(uint32_t mask, uint32_t value) {
    mask &= 0xEF3F1F1F;
    dispCapCnt = (dispCapCnt & ~mask) | (value & mask);
}

void Gpu::writePowCnt1(uint16_t mask, uint16_t value) {
    mask &= 0x820F;
    powCnt1 = (powCnt1 & ~mask) | (value & mask);
}
