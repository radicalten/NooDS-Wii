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
#include <vector>
#include <tuxedo/thread.h>

#include "core.h"

// Helper struct to pass the context to the flat thread entry point
struct RendererThreadArg {
    Gpu3DRenderer* renderer;
    int threadId;
};

Gpu3DRenderer::Gpu3DRenderer(Core *core): core(core) {
    // Mark the scanlines as ready to start
    for (int i = 0; i < 192 * 2; i++)
        ready[i] = 3;
}

Gpu3DRenderer::~Gpu3DRenderer() {
    // Clean up the threads
    for (int i = 0; i < activeThreads; i++) {
        KThreadJoin(&threads[i]);
        if (threadStacks[i]) {
            free(threadStacks[i]);
            threadStacks[i] = nullptr;
        }
    }
}

void Gpu3DRenderer::saveState(FILE *file) {
    // Write state data to the file
    fwrite(&disp3DCnt, sizeof(disp3DCnt), 1, file);
    fwrite(edgeColor, 2, sizeof(edgeColor) / 2, file);
    fwrite(&clearColor, sizeof(clearColor), 1, file);
    fwrite(&clearDepth, sizeof(clearDepth), 1, file);
    fwrite(&fogColor, sizeof(fogColor), 1, file);
    fwrite(&fogOffset, sizeof(fogOffset), 1, file);
    fwrite(fogTable, 1, sizeof(fogTable), file);
    fwrite(toonTable, 2, sizeof(toonTable) / 2, file);
}

void Gpu3DRenderer::loadState(FILE *file) {
    // Read state data from the file
    fread(&disp3DCnt, sizeof(disp3DCnt), 1, file);
    fread(edgeColor, 2, sizeof(edgeColor) / 2, file);
    fread(&clearColor, sizeof(clearColor), 1, file);
    fread(&clearDepth, sizeof(clearDepth), 1, file);
    fread(&fogColor, sizeof(fogColor), 1, file);
    fread(&fogOffset, sizeof(fogOffset), 1, file);
    fread(fogTable, 1, sizeof(fogTable), file);
    fread(toonTable, 2, sizeof(toonTable) / 2, file);
}

uint32_t Gpu3DRenderer::rgba5ToRgba6(uint32_t color) {
    // Convert an RGBA5 value to an RGBA6 value
    uint8_t r = ((color >> 0) & 0x1F) * 2; if (r > 0) r++;
    uint8_t g = ((color >> 5) & 0x1F) * 2; if (g > 0) g++;
    uint8_t b = ((color >> 10) & 0x1F) * 2; if (b > 0) b++;
    uint8_t a = ((color >> 15) & 0x1F) * 2; if (a > 0) a++;
    return (a << 18) | (b << 12) | (g << 6) | r;
}

uint32_t *Gpu3DRenderer::getLine(int line) {
    if (resShift) {
        uint32_t *data = getLine1(line * 2);
        getLine1(line * 2 + 1);
        return data;
    }
    return getLine1(line);
}

uint32_t *Gpu3DRenderer::getLine1(int line) {
    PPCIrqState st = PPCIrqLockByMsr();
    int currentReady = ready[line];
    PPCIrqUnlockByMsr(st);

    if (currentReady < 3 && line + activeThreads * 2 < (192 << resShift)) {
        int next = line + activeThreads * 2;
        
        st = PPCIrqLockByMsr();
        int oldVal = ready[next];
        ready[next] = 1;
        PPCIrqUnlockByMsr(st);

        switch (oldVal) {
        case 0:
            drawScanline1(next);
            st = PPCIrqLockByMsr();
            ready[next] = 2;
            PPCIrqUnlockByMsr(st);
            break;

        case 2:
            st = PPCIrqLockByMsr();
            if (ready[next] == 3) {
                // Keep completed state
            } else {
                ready[next] = 2;
            }
            PPCIrqUnlockByMsr(st);
            break;

        case 3:
            st = PPCIrqLockByMsr();
            ready[next] = 3;
            PPCIrqUnlockByMsr(st);
            break;
        }
    }

    // Wait until scanline is fully written (state == 3)
    st = PPCIrqLockByMsr();
    currentReady = ready[line];
    PPCIrqUnlockByMsr(st);

    while (currentReady < 3) {
        KThreadYield(); 
        st = PPCIrqLockByMsr();
        currentReady = ready[line];
        PPCIrqUnlockByMsr(st);
    }
    return &framebuffer[0][line * 256 * 2];
}

void Gpu3DRenderer::drawScanline(int line) {
    if (line == 0) {
        // Calculate the scanline bounds for each polygon
        for (int i = 0; i < core->gpu3D.polygonCountOut; i++) {
            polygonTop[i] = 192 * 2;
            polygonBot[i] = 0 * 2;

            _Polygon *polygon = &core->gpu3D.polygonsOut[i];
            for (int j = 0; j < polygon->size; j++) {
                Vertex *vertex = &core->gpu3D.verticesOut[polygon->vertices + j];
                if (vertex->y < polygonTop[i]) polygonTop[i] = vertex->y;
                if (vertex->y > polygonBot[i]) polygonBot[i] = vertex->y;
            }

            if (polygonTop[i] == polygonBot[i]) polygonBot[i]++;
        }

        resShift = Settings::highRes3D;

        // Clean up previous frame threads
        for (int i = 0; i < activeThreads; i++) {
            KThreadJoin(&threads[i]);
            if (threadStacks[i]) {
                free(threadStacks[i]);
                threadStacks[i] = nullptr;
            }
        }

        activeThreads = Settings::threaded3D & 0xF;
        if (activeThreads > 4) activeThreads = 4; // Absolute cap to prevent memory pressure

        if (activeThreads > 0) {
            // Mark the scanlines as not ready inside atomic-like locks
            PPCIrqState st = PPCIrqLockByMsr();
            for (int i = 0; i < (192 << resShift); i++)
                ready[i] = 0;
            PPCIrqUnlockByMsr(st);

            // Launch Tuxedo workers
            for (uint8_t i = 0; i < activeThreads; i++) {
                threadStacks[i] = (uint8_t*)malloc(16384);
                RendererThreadArg* args = (RendererThreadArg*)malloc(sizeof(RendererThreadArg));
                args->renderer = this;
                args->threadId = i;
                KThreadPrepare(&threads[i], drawThreadedEntryPoint, args, threadStacks[i] + 16384, KTHR_MAIN_PRIO);
                KThreadResume(&threads[i]);
            }
        }
    }

    if (activeThreads == 0) {
        if (resShift) {
            for (int i = line * 2; i < line * 2 + 2; i++) {
                drawScanline1(i);
                if (i > 0) finishScanline(i - 1);
                if (i == 383) finishScanline(383);
            }
        }
        else {
            drawScanline1(line);
            if (line > 0) finishScanline(line - 1);
            if (line == 191) finishScanline(191);
        }
    }
}

sptr Gpu3DRenderer::drawThreadedEntryPoint(void* arg) {
    RendererThreadArg* actualArgs = (RendererThreadArg*)arg;
    actualArgs->renderer->drawThreaded(actualArgs->threadId);
    free(actualArgs); // Garbage-collect argument payload
    return 0;
}

void Gpu3DRenderer::drawThreaded(int thread) {
    int i, end = 192 << resShift;
    for (i = thread; i < end; i += activeThreads) {
        PPCIrqState st = PPCIrqLockByMsr();
        int oldVal = ready[i];
        ready[i] = 1;
        PPCIrqUnlockByMsr(st);

        switch (oldVal) {
        case 0:
            drawScanline1(i);
            st = PPCIrqLockByMsr();
            ready[i] = 2;
            PPCIrqUnlockByMsr(st);
            break;

        case 2:
            st = PPCIrqLockByMsr();
            ready[i] = 2;
            PPCIrqUnlockByMsr(st);
            break;
        }

        if (i < activeThreads) continue;
        int prev = i - activeThreads;

        // Synchronize on surrounding scanline progress
        st = PPCIrqLockByMsr();
        bool isPrevLeftReady = (prev > 0) ? (ready[prev - 1] < 2) : false;
        bool isPrevReady     = (ready[prev] < 2);
        bool isPrevRightReady = (ready[prev + 1] < 2);
        PPCIrqUnlockByMsr(st);

        while (isPrevLeftReady || isPrevReady || isPrevRightReady) {
            KThreadYield(); 
            st = PPCIrqLockByMsr();
            isPrevLeftReady = (prev > 0) ? (ready[prev - 1] < 2) : false;
            isPrevReady     = (ready[prev] < 2);
            isPrevRightReady = (ready[prev + 1] < 2);
            PPCIrqUnlockByMsr(st);
        }

        finishScanline(prev);
        st = PPCIrqLockByMsr();
        ready[prev] = 3;
        PPCIrqUnlockByMsr(st);
    }

    int prev = i - activeThreads;

    PPCIrqState st = PPCIrqLockByMsr();
    bool isPrevLeftReady  = (ready[prev - 1] < 2);
    bool isPrevReady      = (ready[prev] < 2);
    bool isPrevRightReady = (prev < 191) ? (ready[prev + 1] < 2) : false;
    PPCIrqUnlockByMsr(st);

    while (isPrevLeftReady || isPrevReady || isPrevRightReady) {
        KThreadYield(); 
        st = PPCIrqLockByMsr();
        isPrevLeftReady  = (ready[prev - 1] < 2);
        isPrevReady      = (ready[prev] < 2);
        isPrevRightReady = (prev < 191) ? (ready[prev + 1] < 2) : false;
        PPCIrqUnlockByMsr(st);
    }

    finishScanline(prev);
    st = PPCIrqLockByMsr();
    ready[prev] = 3;
    PPCIrqUnlockByMsr(st);
}

void Gpu3DRenderer::drawScanline1(int line) {
    uint32_t color = BIT(26) | rgba5ToRgba6(((clearColor & 0x001F0000) >> 1) | (clearColor & 0x00007FFF));
    int32_t depth = (clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9);
    uint32_t attrib = ((clearColor & BIT(15)) >> 2) | ((clearColor & 0x3F000000) >> 18) | ((clearColor & 0x3F000000) >> 24) |
        (0x3F << 15) | (((clearColor & 0x001F0000) && ((clearColor & 0x001F0000) >> 16) < 31) << 12);

    int start = line * 256 * 2, end = start + (256 << resShift);
    for (int i = start; i < end; i++) {
        framebuffer[0][i] = color;
        depthBuffer[0][i] = depth;
        attribBuffer[0][i] = attrib;
    }

    stencilClear[line] = false;

    std::vector<int> translucent;

    for (int i = 0; i < core->gpu3D.polygonCountOut; i++) {
        if (line < polygonTop[i] || line >= polygonBot[i])
            continue;

        _Polygon *polygon = &core->gpu3D.polygonsOut[i];
        if (polygon->alpha < 0x3F || polygon->textureFmt == 1 || polygon->textureFmt == 6)
            translucent.push_back(i);
        else
            drawPolygon(line, i);
    }

    for (unsigned int i = 0; i < translucent.size(); i++)
        drawPolygon(line, translucent[i]);
}

void Gpu3DRenderer::finishScanline(int line) {
    if (disp3DCnt & BIT(5)) {
        int offset = line * 256 * 2;
        int w = (256 << resShift) - 1;
        int h = (192 << resShift) - 1;

        for (int i = offset; i <= offset + w; i++) {
            if (attribBuffer[0][i] & BIT(14)) { // Edge bit
                uint32_t id[4] = {
                    (((i & w) > 0) ? attribBuffer[0][i - 1] : (clearColor >> 24)) & 0x3F, // Left
                    (((i & w) < w) ? attribBuffer[0][i + 1] : (clearColor >> 24)) & 0x3F, // Right
                    ((line > 0) ? attribBuffer[0][i - 512] : (clearColor >> 24)) & 0x3F, // Up
                    ((line < h) ? attribBuffer[0][i + 512] : (clearColor >> 24)) & 0x3F // Down
                };

                int32_t depth[4] = {
                    (((i & w) > 0) ? depthBuffer[0][i - 1] : ((clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9))), // Left
                    (((i & w) < w) ? depthBuffer[0][i + 1] : ((clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9))), // Right
                    ((line > 0) ? depthBuffer[0][i - 512] : ((clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9))), // Up
                    ((line < h) ? depthBuffer[0][i + 512] : ((clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9))) // Down
                };

                for (int j = 0; j < 4; j++) {
                    if ((attribBuffer[0][i] & 0x3F) != id[j] && depthBuffer[0][i] < depth[j]) {
                        framebuffer[0][i] = BIT(26) | rgba5ToRgba6((0x1F << 15) | edgeColor[(attribBuffer[0][i] & 0x3F) >> 3]);
                        attribBuffer[0][i] = (attribBuffer[0][i] & ~(0x3F << 15)) | (0x20 << 15);
                        break;
                    }
                }
            }
        }
    }

    if (disp3DCnt & BIT(7)) {
        uint32_t fog = rgba5ToRgba6(((fogColor & 0x001F0000) >> 1) | (fogColor & 0x00007FFF));
        int fogStep = 0x400 >> ((disp3DCnt & 0x0F00) >> 8);

        for (int layer = 0; layer < ((disp3DCnt & BIT(4)) ? 2 : 1); layer++) {
            int start = line * 256 * 2, end = start + (256 << resShift);
            for (int i = start; i < end; i++) {
                if (attribBuffer[layer][i] & BIT(13)) {
                    int32_t offset = ((depthBuffer[layer][i] / 0x200) - fogOffset);
                    int n = (fogStep > 0) ? (offset / fogStep - 1) : ((offset > 0) ? 31 : 0);

                    uint8_t density;
                    if (n >= 31) {
                        density = fogTable[31];
                    }
                    else if (n < 0 || fogStep == 0) {
                        density = fogTable[0];
                    }
                    else {
                        int m = offset % fogStep;
                        density = ((m >= 0) ? ((fogTable[n + 1] * m + fogTable[n] * (fogStep - m)) / fogStep) : fogTable[0]);
                    }

                    if (density == 127)
                        density++;

                    uint8_t a = (((fog >> 18) & 0x3F) * density + ((framebuffer[layer][i] >> 18) & 0x3F) * (128 - density)) / 128;
                    if (disp3DCnt & BIT(6)) {
                        framebuffer[layer][i] = (framebuffer[layer][i] & ~(0x3F << 18)) | (a << 18);
                    }
                    else {
                        uint8_t r = (((fog >> 0) & 0x3F) * density + ((framebuffer[layer][i] >> 0) & 0x3F) * (128 - density)) / 128;
                        uint8_t g = (((fog >> 6) & 0x3F) * density + ((framebuffer[layer][i] >> 6) & 0x3F) * (128 - density)) / 128;
                        uint8_t b = (((fog >> 12) & 0x3F) * density + ((framebuffer[layer][i] >> 12) & 0x3F) * (128 - density)) / 128;
                        framebuffer[layer][i] = BIT(26) | (a << 18) | (b << 12) | (g << 6) | r;
                    }
                }
            }
        }
    }

    if (disp3DCnt & BIT(4)) {
        int start = line * 256 * 2, end = start + (256 << resShift);
        for (int i = start; i < end; i++) {
            if (((attribBuffer[0][i] >> 15) & 0x3F) < 0x3F) {
                if ((framebuffer[1][i] >> 18) & 0x3F)
                    framebuffer[0][i] = BIT(26) | interpolateColor(framebuffer[1][i], framebuffer[0][i], 0, ((attribBuffer[0][i] >> 15) & 0x3F), 0x3F);
                else
                    framebuffer[0][i] = (framebuffer[0][i] & ~0xFC0000) | ((attribBuffer[0][i] & 0x1F8000) << 3);
            }
        }
    }
}

uint8_t *Gpu3DRenderer::getTexture(uint32_t address) {
    uint8_t *slot = core->memory.tex3D[address >> 17];
    return slot ? &slot[address & 0x1FFFF] : nullptr;
}

uint8_t *Gpu3DRenderer::getPalette(uint32_t address) {
    uint8_t *slot = core->memory.pal3D[address >> 14];
    return slot ? &slot[address & 0x3FFF] : nullptr;
}

uint32_t Gpu3DRenderer::interpolateLinear(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2) {
    if (x <= x1) return v1;
    if (x >= x2) return v2;

    if (v1 <= v2)
        return v1 + (v2 - v1) * (x - x1) / (x2 - x1);
    else
        return v2 + (v1 - v2) * (x2 - x) / (x2 - x1);
}

uint32_t Gpu3DRenderer::interpolateLinRev(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2) {
    if (x <= x1) return v1;
    if (x >= x2) return v2;

    if (v1 <= v2)
        return v2 - (v2 - v1) * (x2 - x) / (x2 - x1);
    else
        return v1 - (v1 - v2) * (x - x1) / (x2 - x1);
}

uint32_t Gpu3DRenderer::interpolateFactor(uint32_t factor, uint32_t shift, uint32_t v1, uint32_t v2) {
    if (v1 <= v2)
        return v1 + (((v2 - v1) * factor) >> shift);
    else
        return v2 + (((v1 - v2) * ((1 << shift) - factor)) >> shift);
}

uint32_t Gpu3DRenderer::interpolateColor(uint32_t c1, uint32_t c2, uint32_t x1, uint32_t x, uint32_t x2) {
    uint32_t r = interpolateLinear((c1 >> 0) & 0x3F, (c2 >> 0) & 0x3F, x1, x, x2);
    uint32_t g = interpolateLinear((c1 >> 6) & 0x3F, (c2 >> 6) & 0x3F, x1, x, x2);
    uint32_t b = interpolateLinear((c1 >> 12) & 0x3F, (c2 >> 12) & 0x3F, x1, x, x2);
    uint32_t a = (((c1 >> 18) & 0x3F) > ((c2 >> 18) & 0x3F)) ? ((c1 >> 18) & 0x3F) : ((c2 >> 18) & 0x3F);
    return (a << 18) | (b << 12) | (g << 6) | r;
}

uint32_t Gpu3DRenderer::readTexture(_Polygon *polygon, int s, int t) {
    if (polygon->repeatS) {
        if (polygon->flipS && (s & polygon->sizeS))
            s = -1 - s;
        s &= polygon->sizeS - 1;
    }
    else if (s < 0) {
        s = 0;
    }
    else if (s >= polygon->sizeS) {
        s = polygon->sizeS - 1;
    }

    if (polygon->repeatT) {
        if (polygon->flipT && (t & polygon->sizeT))
            t = -1 - t;
        t &= polygon->sizeT - 1;
    }
    else if (t < 0) {
        t = 0;
    }
    else if (t >= polygon->sizeT) {
        t = polygon->sizeT - 1;
    }

    switch (polygon->textureFmt) {
    case 1: {
        uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s);
        uint8_t *data = getTexture(address);
        if (!data) return 0;
        uint8_t index = *data;

        uint8_t *palette = getPalette(polygon->paletteAddr);
        if (!palette) return 0;

        uint16_t color = U8TO16(palette, (index & 0x1F) * 2) & ~BIT(15);
        uint8_t alpha = (index >> 5) * 4 + (index >> 5) / 2;
        return rgba5ToRgba6((alpha << 15) | color);
    }

    case 2: {
        uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s) / 4;
        uint8_t *data = getTexture(address);
        if (!data) return 0;
        uint8_t index = (*data >> ((s % 4) * 2)) & 0x03;

        if (polygon->transparent0 && index == 0)
            return 0;

        uint8_t *palette = getPalette(polygon->paletteAddr);
        if (!palette) return 0;

        return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
    }

    case 3: {
        uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s) / 2;
        uint8_t *data = getTexture(address);
        if (!data) return 0;
        uint8_t index = (*data >> ((s % 2) * 4)) & 0x0F;

        if (polygon->transparent0 && index == 0)
            return 0;

        uint8_t *palette = getPalette(polygon->paletteAddr);
        if (!palette) return 0;

        return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
    }

    case 4: {
        uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s);
        uint8_t *data = getTexture(address);
        if (!data) return 0;
        uint8_t index = *data;

        if (polygon->transparent0 && index == 0)
            return 0;

        uint8_t *palette = getPalette(polygon->paletteAddr);
        if (!palette) return 0;

        return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
    }

    case 5: {
        int tile = (t / 4) * (polygon->sizeS / 4) + (s / 4);
        uint32_t address = polygon->textureAddr + (tile * 4 + t % 4);
        uint8_t *data = getTexture(address);
        if (!data) return 0;
        uint8_t index = (*data >> ((s % 4) * 2)) & 0x03;

        address = 0x20000 + (polygon->textureAddr % 0x20000) / 2 + ((polygon->textureAddr / 0x20000 == 2) ? 0x10000 : 0);
        if (!(data = getTexture(address))) return 0;
        uint16_t palBase = U8TO16(data, tile * 2);
        uint8_t *palette = getPalette(polygon->paletteAddr + (palBase & 0x3FFF) * 4);
        if (!palette) return 0;

        uint32_t c1, c2;
        switch ((palBase & 0xC000) >> 14) {
        case 0:
            if (index == 3) return 0;
            return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));

        case 1:
            switch (index) {
            case 2:
                c1 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 0));
                c2 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 2));
                return interpolateColor(c1, c2, 0, 1, 2);

            case 3:
                return 0;

            default:
                return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
            }

        case 2:
            return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));

        case 3:
            switch (index) {
            case 2:
                c1 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 0));
                c2 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 2));
                return interpolateColor(c1, c2, 0, 3, 8);

            case 3:
                c1 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 0));
                c2 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 2));
                return interpolateColor(c1, c2, 0, 5, 8);

            default:
                return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
            }
        }
    }

    case 6: {
        uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s);
        uint8_t *data = getTexture(address);
        if (!data) return 0;
        uint8_t index = *data;

        uint8_t *palette = getPalette(polygon->paletteAddr);
        if (!palette) return 0;

        uint16_t color = U8TO16(palette, (index & 0x07) * 2) & ~BIT(15);
        uint8_t alpha = index >> 3;
        return rgba5ToRgba6((alpha << 15) | color);
    }

    default: {
        uint8_t *data = getTexture(polygon->textureAddr);
        if (!data) return 0;

        uint16_t color = U8TO16(data, (t * polygon->sizeS + s) * 2);
        uint8_t alpha = (color & BIT(15)) ? 0x1F : 0;
        return rgba5ToRgba6((alpha << 15) | color);
    }}
}

void Gpu3DRenderer::drawPolygon(int line, int polygonIndex) {
    _Polygon *polygon = &core->gpu3D.polygonsOut[polygonIndex];

    Vertex *vertices[10];
    for (int i = 0; i < polygon->size; i++)
        vertices[i] = &core->gpu3D.verticesOut[polygon->vertices + i];

    if (polygon->crossed)
        SWAP(vertices[2], vertices[3]);

    int start = 0;
    for (int i = 0; i < polygon->size; i++) {
        if (vertices[start]->y > vertices[i]->y)
            start = i;
    }

    int v[4] = {
        start, (start + 1) % polygon->size,
        start, (start - 1 + polygon->size) % polygon->size
    };

    while (vertices[v[1]]->y <= line) {
        v[0] = v[1];
        v[1] = (v[1] + 1) % polygon->size;
        if (v[0] == start) break;
    }

    while (vertices[v[3]]->y <= line) {
        v[2] = v[3];
        v[3] = (v[3] - 1 + polygon->size) % polygon->size;
        if (v[2] == start) break;
    }

    if (polygon->clockwise) {
        SWAP(v[0], v[2]);
        SWAP(v[1], v[3]);
    }

    if (vertices[v[0]]->y > vertices[v[1]]->y) SWAP(v[0], v[1]);
    if (vertices[v[2]]->y > vertices[v[3]]->y) SWAP(v[2], v[3]);

    uint32_t x1, x2, x3, x4;
    uint32_t x1a = 0x3F, x2a = 0x3F, x3a = 0x3F, x4a = 0x3F;

    if (vertices[v[0]]->y == vertices[v[1]]->y) {
        x1 = vertices[v[0]]->x;
        x2 = vertices[v[1]]->x;

        if (vertices[v[0]]->x > vertices[v[1]]->x) {
            SWAP(v[0], v[1]);
            SWAP(x1, x2);
        }
    }
    else if (abs(vertices[v[1]]->x - vertices[v[0]]->x) > vertices[v[1]]->y - vertices[v[0]]->y) {
        x1 = interpolateLinear(vertices[v[0]]->x << 1, vertices[v[1]]->x << 1, vertices[v[0]]->y, line, vertices[v[1]]->y) + 1;
        x2 = interpolateLinear(vertices[v[0]]->x << 1, vertices[v[1]]->x << 1, vertices[v[0]]->y, line + 1, vertices[v[1]]->y) + 1;

        bool negative = (vertices[v[0]]->x > vertices[v[1]]->x);

        if (negative) {
            SWAP(v[0], v[1]);
            SWAP(x1, x2);
        }

        if (disp3DCnt & BIT(4)) {
            x1a = interpolateLinear(vertices[v[0]]->y << 6, vertices[v[1]]->y << 6, vertices[v[0]]->x << 1, x1, vertices[v[1]]->x << 1) & 0x3F;
            x2a = interpolateLinear(vertices[v[0]]->y << 6, vertices[v[1]]->y << 6, vertices[v[0]]->x << 1, x2 - 2, vertices[v[1]]->x << 1) & 0x3F;

            if (negative) {
                x1a = 0x3F - x1a;
                x2a = 0x3F - x2a;
            }
        }

        x1 >>= 1;
        x2 >>= 1;
    }
    else {
        if (vertices[v[0]]->x > vertices[v[1]]->x)
            x1 = interpolateLinRev(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6, vertices[v[0]]->y, line, vertices[v[1]]->y) - 1;
        else
            x1 = interpolateLinear(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6, vertices[v[0]]->y, line, vertices[v[1]]->y);

        if (disp3DCnt & BIT(4)) {
            if (abs(vertices[v[1]]->x - vertices[v[0]]->x) == vertices[v[1]]->y - vertices[v[0]]->y)
                x2a = x1a = 0x20;
            else
                x2a = x1a = 0x3F - (x1 & 0x3F);
        }

        x2 = x1 >>= 6;
    }

    if (vertices[v[2]]->y == vertices[v[3]]->y) {
        x3 = vertices[v[2]]->x;
        x4 = vertices[v[3]]->x;

        if (vertices[v[2]]->x > vertices[v[3]]->x) {
            SWAP(v[2], v[3]);
            SWAP(x3, x4);
        }
    }
    else if (vertices[v[2]]->x == vertices[v[3]]->x) {
        x3 = vertices[v[2]]->x;
        if ((vertices[v[0]]->x != vertices[v[1]]->x || vertices[v[0]]->x != vertices[v[2]]->x) && x3 > 0) x3--;
        x4 = x3;
    }
    else if (abs(vertices[v[3]]->x - vertices[v[2]]->x) > vertices[v[3]]->y - vertices[v[2]]->y) {
        x3 = interpolateLinear(vertices[v[2]]->x << 1, vertices[v[3]]->x << 1, vertices[v[2]]->y, line, vertices[v[3]]->y) + 1;
        x4 = interpolateLinear(vertices[v[2]]->x << 1, vertices[v[3]]->x << 1, vertices[v[2]]->y, line + 1, vertices[v[3]]->y) + 1;

        bool negative = (vertices[v[2]]->x > vertices[v[3]]->x);

        if (negative) {
            SWAP(v[2], v[3]);
            SWAP(x3, x4);
        }

        if (disp3DCnt & BIT(4)) {
            x3a = interpolateLinear(vertices[v[2]]->y << 6, vertices[v[3]]->y << 6, vertices[v[2]]->x << 1, x3, vertices[v[3]]->x << 1) & 0x3F;
            x4a = interpolateLinear(vertices[v[2]]->y << 6, vertices[v[3]]->y << 6, vertices[v[2]]->x << 1, x4 - 2, vertices[v[3]]->x << 1) & 0x3F;

            if (!negative) {
                x3a = 0x3F - x3a;
                x4a = 0x3F - x4a;
            }
        }

        x3 >>= 1;
        x4 >>= 1;
    }
    else {
        if (vertices[v[2]]->x > vertices[v[3]]->x)
            x3 = interpolateLinRev(vertices[v[2]]->x << 6, vertices[v[3]]->x << 6, vertices[v[2]]->y, line, vertices[v[3]]->y) - 1;
        else
            x3 = interpolateLinear(vertices[v[2]]->x << 6, vertices[v[3]]->x << 6, vertices[v[2]]->y, line, vertices[v[3]]->y);

        if (disp3DCnt & BIT(4)) {
            if (abs(vertices[v[3]]->x - vertices[v[2]]->x) == vertices[v[3]]->y - vertices[v[2]]->y)
                x4a = x3a = 0x20;
            else
                x4a = x3a = x3 & 0x3F;
        }

        x4 = x3 >>= 6;
    }

    if (x2 > x1) x2--;
    if (x4 > x3) x4--;

    if (x2 > x3) { x2 = x3; }
    if (x1 > x2) { x2 = x1; x1a = 0x3F - x1a; x2a = 0x3F - x2a; }
    if (x2 > x3) { x3 = x2; }
    if (x3 > x4) { x3 = x4; x3a = 0x3F - x3a; x4a = 0x3F - x4a; }

    if (x1 > x4) {
        SWAP(v[0], v[2]);
        SWAP(v[1], v[3]);
        SWAP(x1, x3);
        SWAP(x2, x4);
        SWAP(x1a, x3a);
        SWAP(x2a, x4a);
    }

    uint32_t xe1[2], xe[2], xe2[2];
    bool hideLeft, hideRight;

    if (abs(vertices[v[1]]->x - vertices[v[0]]->x) > abs(vertices[v[1]]->y - vertices[v[0]]->y)) {
        xe1[0] = vertices[v[0]]->x;
        xe[0] = x1;
        xe2[0] = vertices[v[1]]->x;
        hideLeft = (vertices[v[0]]->y < vertices[v[1]]->y);
    }
    else {
        xe1[0] = vertices[v[0]]->y;
        xe[0] = line;
        xe2[0] = vertices[v[1]]->y;
        hideLeft = false;
    }

    if (abs(vertices[v[3]]->x - vertices[v[2]]->x) > abs(vertices[v[3]]->y - vertices[v[2]]->y)) {
        xe1[1] = vertices[v[2]]->x;
        xe[1] = x4;
        xe2[1] = vertices[v[3]]->x;
        hideRight = (vertices[v[2]]->y > vertices[v[3]]->y);
    }
    else {
        xe1[1] = vertices[v[2]]->y;
        xe[1] = line;
        xe2[1] = vertices[v[3]]->y;
        hideRight = (vertices[v[2]]->x != vertices[v[3]]->x);
    }

    uint32_t ws[4];
    if (polygon->wShift >= 0) {
        for (int i = 0; i < 4; i++)
            ws[i] = vertices[v[i]]->w >> polygon->wShift;
    }
    else {
        for (int i = 0; i < 4; i++)
            ws[i] = vertices[v[i]]->w << -polygon->wShift;
    }

    uint32_t ze[2], we[2];
    uint32_t re[2], ge[2], be[2];
    uint32_t se[2], te[2];

    for (int i = 0; i < 2; i++) {
        int i2 = i * 2;
        ze[i] = interpolateLinear(vertices[v[i2]]->z, vertices[v[i2 + 1]]->z, xe1[i], xe[i], xe2[i]);
        
        if (ws[i2] == ws[i2 + 1] && !(ws[i2] & 0x00FE)) {
            we[i] = interpolateLinear(ws[i2], ws[i2 + 1], xe1[i], xe[i], xe2[i]);
            re[i] = interpolateLinear(((vertices[v[i2]]->color >> 0) & 0x3F) << 3, ((vertices[v[i2 + 1]]->color >> 0) & 0x3F) << 3, xe1[i], xe[i], xe2[i]);
            ge[i] = interpolateLinear(((vertices[v[i2]]->color >> 6) & 0x3F) << 3, ((vertices[v[i2 + 1]]->color >> 6) & 0x3F) << 3, xe1[i], xe[i], xe2[i]);
            be[i] = interpolateLinear(((vertices[v[i2]]->color >> 12) & 0x3F) << 3, ((vertices[v[i2 + 1]]->color >> 12) & 0x3F) << 3, xe1[i], xe[i], xe2[i]);
            se[i] = interpolateLinear((int32_t)vertices[v[i2]]->s + 0xFFFF, (int32_t)vertices[v[i2 + 1]]->s + 0xFFFF, xe1[i], xe[i], xe2[i]) - 0xFFFF;
            te[i] = interpolateLinear((int32_t)vertices[v[i2]]->t + 0xFFFF, (int32_t)vertices[v[i2 + 1]]->t + 0xFFFF, xe1[i], xe[i], xe2[i]) - 0xFFFF;
        }
        else {
            uint32_t factor;
            if (xe[i] <= xe1[i]) {
                factor = 0;
            }
            else if (xe[i] >= xe2[i]) {
                factor = (1 << 9);
            }
            else {
                uint32_t wa = (ws[i2] >> 1) + ((ws[i2] & 1) && !(ws[i2 + 1] & 1));
                factor = (uint64_t((ws[i2] >> 1) * (xe[i] - xe1[i])) << 9) /
                    ((ws[i2 + 1] >> 1) * (xe2[i] - xe[i]) + wa * (xe[i] - xe1[i]));
            }

            we[i] = interpolateFactor(factor, 9, ws[i2], ws[i2 + 1]);
            re[i] = interpolateFactor(factor, 9, ((vertices[v[i2]]->color >> 0) & 0x3F) << 3, ((vertices[v[i2 + 1]]->color >> 0) & 0x3F) << 3);
            ge[i] = interpolateFactor(factor, 9, ((vertices[v[i2]]->color >> 6) & 0x3F) << 3, ((vertices[v[i2 + 1]]->color >> 6) & 0x3F) << 3);
            be[i] = interpolateFactor(factor, 9, ((vertices[v[i2]]->color >> 12) & 0x3F) << 3, ((vertices[v[i2 + 1]]->color >> 12) & 0x3F) << 3);
            se[i] = interpolateFactor(factor, 9, (int32_t)vertices[v[i2]]->s + 0xFFFF, (int32_t)vertices[v[i2 + 1]]->s + 0xFFFF) - 0xFFFF;
            te[i] = interpolateFactor(factor, 9, (int32_t)vertices[v[i2]]->t + 0xFFFF, (int32_t)vertices[v[i2 + 1]]->t + 0xFFFF) - 0xFFFF;
        }
    }

    if (polygon->mode == 3 && polygon->id == 0) {
        if (!stencilClear[line]) {
            memset(&stencilBuffer[line * 256 * 2], 0, 256 << resShift);
            stencilClear[line] = true;
        }
    }
    else {
        stencilClear[line] = false;
    }

    uint32_t x1e = x1, x4e = ++x4;

    if (polygon->alpha != 0 && !(disp3DCnt & (BIT(4) | BIT(5)))) {
        if (hideLeft) x1e = x2 + 1;
        if (hideRight) x4e = x3;
        if (!(hideLeft && hideRight) && x4e <= x1e)
            x4e = x1e + 1;
    }

    bool horizontal = (line == polygonTop[polygonIndex] || line == polygonBot[polygonIndex] - 1);

    int lastS = 0xFFFF, lastT = 0xFFFF;
    uint32_t texel;

    for (uint32_t x = x1; x < x4; x++) {
        if (!horizontal && polygon->alpha == 0 && x == x2 + 1 && x3 > x2)
            x = x3;

        if (x >= (256 << resShift))
            break;

        bool layer = 0;
        int i = line * 256 * 2 + x;

        uint32_t factor;
        if (we[0] == we[1] && !(we[0] & 0x7F))
            factor = -1;
        else if (x <= x1)
            factor = 0;
        else if (x >= x4)
            factor = (1 << 8);
        else if (resShift && ((x - x1) >> 8))
            factor = (uint64_t(we[0] * (x - x1)) << 8) / (we[1] * (x4 - x) + we[0] * (x - x1));
        else
            factor = ((we[0] * (x - x1)) << 8) / (we[1] * (x4 - x) + we[0] * (x - x1));

        int32_t depth;
        if (polygon->wBuffer) {
            depth = (factor == -1) ? interpolateLinear(we[0], we[1], x1, x, x4) :
                interpolateFactor(factor, 8, we[0], we[1]);
            if (polygon->wShift > 0)
                depth <<= polygon->wShift;
            else if (polygon->wShift < 0)
                depth >>= -polygon->wShift;
        }
        else {
            depth = interpolateLinear(ze[0], ze[1], x1, x, x4);
        }

        bool depthPass[2];
        if (polygon->depthTestEqual) {
            uint32_t margin = (polygon->wBuffer ? 0xFF : 0x200);
            depthPass[0] = (depthBuffer[0][i] >= depth - margin && depthBuffer[0][i] <= depth + margin);
            depthPass[1] = (disp3DCnt & BIT(4)) && (attribBuffer[0][i] & BIT(14)) &&
                (depthBuffer[1][i] >= depth - margin && depthBuffer[1][i] <= depth + margin);
        }
        else {
            depthPass[0] = (depthBuffer[0][i] > depth);
            depthPass[1] = (disp3DCnt & BIT(4)) && (attribBuffer[0][i] & BIT(14)) && (depthBuffer[1][i] > depth);
        }

        if (polygon->mode == 3) {
            if (polygon->id == 0) {
                if (!depthPass[0]) stencilBuffer[i] |= BIT(0);
                if (!depthPass[1]) stencilBuffer[i] |= BIT(1);
                continue;
            }
            else if (!depthPass[0] || !(stencilBuffer[i] & BIT(0)) || (attribBuffer[0][i] & 0x3F) == polygon->id) {
                if (!depthPass[1] || !(stencilBuffer[i] & BIT(1)) || (attribBuffer[1][i] & 0x3F) == polygon->id)
                    continue;
                layer = 1;
            }
        }
        else if (!depthPass[0]) {
            if (!depthPass[1])
                continue;
            layer = 1;
        }

        uint32_t rv, gv, bv;
        if (factor == -1) {
            rv = interpolateLinear(re[0], re[1], x1, x, x4) >> 3;
            gv = interpolateLinear(ge[0], ge[1], x1, x, x4) >> 3;
            bv = interpolateLinear(be[0], be[1], x1, x, x4) >> 3;
        }
        else {
            rv = interpolateFactor(factor, 8, re[0], re[1]) >> 3;
            gv = interpolateFactor(factor, 8, ge[0], ge[1]) >> 3;
            bv = interpolateFactor(factor, 8, be[0], be[1]) >> 3;
        }
        uint32_t color = ((polygon->alpha ? polygon->alpha : 0x3F) << 18) | (bv << 12) | (gv << 6) | rv;

        if (polygon->textureFmt != 0) {
            int s, t;
            if (factor == -1) {
                s = (int)(interpolateLinear(se[0] + 0xFFFF, se[1] + 0xFFFF, x1, x, x4) - 0xFFFF) >> 4;
                t = (int)(interpolateLinear(te[0] + 0xFFFF, te[1] + 0xFFFF, x1, x, x4) - 0xFFFF) >> 4;
            }
            else {
                s = (int)(interpolateFactor(factor, 8, se[0] + 0xFFFF, se[1] + 0xFFFF) - 0xFFFF) >> 4;
                t = (int)(interpolateFactor(factor, 8, te[0] + 0xFFFF, te[1] + 0xFFFF) - 0xFFFF) >> 4;
            }

            if (s != lastS || t != lastT) {
                lastS = s; lastT = t;
                texel = readTexture(polygon, s, t);
            }

            switch (polygon->mode) {
            case 0: {
                uint8_t r = ((((texel >> 0) & 0x3F) + 1) * (((color >> 0) & 0x3F) + 1) - 1) / 64;
                uint8_t g = ((((texel >> 6) & 0x3F) + 1) * (((color >> 6) & 0x3F) + 1) - 1) / 64;
                uint8_t b = ((((texel >> 12) & 0x3F) + 1) * (((color >> 12) & 0x3F) + 1) - 1) / 64;
                uint8_t a = ((((texel >> 18) & 0x3F) + 1) * (((color >> 18) & 0x3F) + 1) - 1) / 64;
                color = (a << 18) | (b << 12) | (g << 6) | r;
                break;
            }

            case 1:
            case 3: {
                uint8_t at = ((texel >> 18) & 0x3F);
                uint8_t r = (((texel >> 0) & 0x3F) * at + ((color >> 0) & 0x3F) * (63 - at)) / 64;
                uint8_t g = (((texel >> 6) & 0x3F) * at + ((color >> 6) & 0x3F) * (63 - at)) / 64;
                uint8_t b = (((texel >> 12) & 0x3F) * at + ((color >> 12) & 0x3F) * (63 - at)) / 64;
                uint8_t a = ((color >> 18) & 0x3F);
                color = (a << 18) | (b << 12) | (g << 6) | r;
                break;
            }

            case 2: {
                uint32_t toon = rgba5ToRgba6(toonTable[(color & 0x3F) / 2]);
                uint8_t r, g, b;

                if (disp3DCnt & BIT(1)) {
                    r = ((((texel >> 0) & 0x3F) + 1) * (((color >> 0) & 0x3F) + 1) - 1) / 64;
                    g = ((((texel >> 6) & 0x3F) + 1) * (((color >> 6) & 0x3F) + 1) - 1) / 64;
                    b = ((((texel >> 12) & 0x3F) + 1) * (((color >> 12) & 0x3F) + 1) - 1) / 64;
                    r += ((toon >> 0) & 0x3F); if (r > 63) r = 63;
                    g += ((toon >> 6) & 0x3F); if (g > 63) g = 63;
                    b += ((toon >> 12) & 0x3F); if (b > 63) b = 63;
                }
                else {
                    r = ((((texel >> 0) & 0x3F) + 1) * (((toon >> 0) & 0x3F) + 1) - 1) / 64;
                    g = ((((texel >> 6) & 0x3F) + 1) * (((toon >> 6) & 0x3F) + 1) - 1) / 64;
                    b = ((((texel >> 12) & 0x3F) + 1) * (((toon >> 12) & 0x3F) + 1) - 1) / 64;
                }

                uint8_t a = ((((texel >> 18) & 0x3F) + 1) * (((color >> 18) & 0x3F) + 1) - 1) / 64;
                color = (a << 18) | (b << 12) | (g << 6) | r;
                break;
            }}
        }
        else if (polygon->mode == 2) {
            uint32_t toon = rgba5ToRgba6(toonTable[(color & 0x3F) / 2]);
            uint8_t r, g, b;

            if (disp3DCnt & BIT(1)) {
                r = ((color >> 0) & 0x3F) + ((toon >> 0) & 0x3F); if (r > 63) r = 63;
                g = ((color >> 6) & 0x3F) + ((toon >> 6) & 0x3F); if (g > 63) g = 63;
                b = ((color >> 12) & 0x3F) + ((toon >> 12) & 0x3F); if (b > 63) b = 63;
            }
            else {
                r = ((toon >> 0) & 0x3F);
                g = ((toon >> 6) & 0x3F);
                b = ((toon >> 12) & 0x3F);
            }

            color = (color & 0xFC0000) | (b << 12) | (g << 6) | r;
        }

        if (!(color & 0xFC0000) || ((x < x1e || x >= x4e) && ((color >> 18) == 0x3F || !(disp3DCnt & BIT(3)))))
            continue;

        if ((color >> 18) == 0x3F) {
            uint8_t edgeAlpha = 0x3F;
            bool edge = (x <= x2 || x >= x3 || horizontal);

            if ((disp3DCnt & BIT(4)) && layer == 0 && edge) {
                framebuffer[1][i] = framebuffer[0][i];
                depthBuffer[1][i] = depthBuffer[0][i];
                attribBuffer[1][i] = attribBuffer[0][i];

                if (x <= x2)
                    edgeAlpha = interpolateLinear(x1a, x2a, x1, x, x2);
                else if (x >= x3)
                    edgeAlpha = interpolateLinear(x3a, x4a, x3, x, x4);
            }

            framebuffer[layer][i] = BIT(26) | color;
            depthBuffer[layer][i] = depth;
            attribBuffer[layer][i] = (attribBuffer[layer][i] & 0x0FC0) | (edgeAlpha << 15) |
                (edge << 14) | (polygon->fog << 13) | polygon->id;
        }
        else if (!(attribBuffer[layer][i] & BIT(12)) || ((attribBuffer[layer][i] >> 6) & 0x3F) != polygon->id) {
            framebuffer[layer][i] = BIT(26) | (((disp3DCnt & BIT(3)) && (framebuffer[layer][i] & 0xFC0000)) ?
                interpolateColor(framebuffer[layer][i], color, 0, color >> 18, 63) : color);
            if (polygon->transNewDepth) depthBuffer[layer][i] = depth;
            attribBuffer[layer][i] = (attribBuffer[layer][i] & (0x1FC03F | (polygon->fog << 13))) | BIT(12) | (polygon->id << 6);

            if ((disp3DCnt & BIT(4)) && layer == 0 && (attribBuffer[0][i] & BIT(14))) {
                framebuffer[1][i] = BIT(26) | (((disp3DCnt & BIT(3)) && (framebuffer[1][i] & 0xFC0000)) ?
                    interpolateColor(framebuffer[1][i], color, 0, color >> 18, 63) : color);
                if (polygon->transNewDepth) depthBuffer[1][i] = depth;
                attribBuffer[1][i] = (attribBuffer[1][i] & (0x1FC03F | (polygon->fog << 13))) | BIT(12) | (polygon->id << 6);
            }
        }
    }
}

void Gpu3DRenderer::writeDisp3DCnt(uint16_t mask, uint16_t value) {
    if (value & BIT(12)) disp3DCnt &= ~BIT(12);
    if (value & BIT(13)) disp3DCnt &= ~BIT(13);

    mask &= 0x4FFF;
    if ((value & mask) == (disp3DCnt & mask)) return;
    disp3DCnt = (disp3DCnt & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeEdgeColor(int index, uint16_t mask, uint16_t value) {
    mask &= 0x7FFF;
    if ((value & mask) == (edgeColor[index] & mask)) return;
    edgeColor[index] = (edgeColor[index] & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeClearColor(uint32_t mask, uint32_t value) {
    mask &= 0x3F1FFFFF;
    if ((value & mask) == (clearColor & mask)) return;
    clearColor = (clearColor & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeClearDepth(uint16_t mask, uint16_t value) {
    mask &= 0x7FFF;
    if ((value & mask) == (clearDepth & mask)) return;
    clearDepth = (clearDepth & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeToonTable(int index, uint16_t mask, uint16_t value) {
    mask &= 0x7FFF;
    if ((value & mask) == (toonTable[index] & mask)) return;
    toonTable[index] = (toonTable[index] & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeFogColor(uint32_t mask, uint32_t value) {
    mask &= 0x001F7FFF;
    if ((value & mask) == (fogColor & mask)) return;
    fogColor = (fogColor & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeFogOffset(uint16_t mask, uint16_t value) {
    mask &= 0x7FFF;
    if ((value & mask) == (fogOffset & mask)) return;
    fogOffset = (fogOffset & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeFogTable(int index, uint8_t value) {
    if ((value & 0x7F) == (fogTable[index] & 0x7F)) return;
    fogTable[index] = value & 0x7F;
    core->gpu.invalidate3D();
}
