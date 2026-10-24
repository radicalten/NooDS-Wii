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

namespace {
struct Gpu3DRenderLuts {
    uint32_t rgb5To6[32768];
    uint32_t alpha5To6[32];
    struct UDivEntry {
        uint32_t mul;
        uint8_t add;
        uint8_t shift;
    } udiv[1025];

    Gpu3DRenderLuts() {
        for (uint32_t c = 0; c < 32768; c++) {
            uint32_t r = ((c >> 0) & 0x1F) * 2; if (r > 0) r++;
            uint32_t g = ((c >> 5) & 0x1F) * 2; if (g > 0) g++;
            uint32_t b = ((c >> 10) & 0x1F) * 2; if (b > 0) b++;
            rgb5To6[c] = (b << 12) | (g << 6) | r;
        }
        for (uint32_t a = 0; a < 32; a++) {
            uint32_t a6 = a * 2; if (a6 > 0) a6++;
            alpha5To6[a] = a6 << 18;
        }
        udiv[0] = { 0, 2, 0 };
        for (uint32_t d = 1; d <= 1024; d++) {
            if ((d & (d - 1)) == 0) {
                uint8_t sh = 0;
                for (uint32_t t = d; t > 1; t >>= 1) sh++;
                udiv[d] = { 0, 2, sh };
            } else {
                uint64_t nc = ((uint64_t)(0xFFFFFFFFu / d) * d) - 1u;
                for (uint32_t p = 32; p < 64; p++) {
                    uint64_t twoP = 1ULL << p;
                    uint64_t rem = (twoP - 1u) % d;
                    if (twoP > nc * (d - 1u - rem)) {
                        uint64_t m = (twoP + d - 1u - rem) / d;
                        if (m < (1ULL << 32))
                            udiv[d] = { (uint32_t)m, 0, (uint8_t)(p - 32) };
                        else
                            udiv[d] = { (uint32_t)m, 1, (uint8_t)(p - 32) };
                        break;
                    }
                }
            }
        }
    }
};
static const Gpu3DRenderLuts s_3dLuts;

static inline uint32_t udivSpan(uint32_t n, uint32_t d) {
    if (__builtin_expect(d <= 1024, 1)) {
        const auto &e = s_3dLuts.udiv[d];
        if (e.add == 2) return n >> e.shift;
        uint32_t q = (uint32_t)(((uint64_t)n * e.mul) >> 32);
        if (e.add == 1) {
            q = ((n - q) >> 1) + q;
            return q >> (e.shift - 1);
        }
        return q >> e.shift;
    }
    return n / d;
}
} // namespace

uint32_t Gpu3DRenderer::rgba5ToRgba6(uint32_t color) {
    return s_3dLuts.alpha5To6[(color >> 15) & 0x1Fu] | s_3dLuts.rgb5To6[color & 0x7FFFu];
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
        const int polyCount = core->gpu3D.polygonCountOut;
        const bool edgeMark = (disp3DCnt & BIT(5)) != 0;
        for (int i = 0; i < polyCount; i++) {
            polygonTop[i] = 192 * 2;
            polygonBot[i] = 0 * 2;
            polygonSharesSameIdOpaqueEdge[i] = false;

            _Polygon *polygon = &core->gpu3D.polygonsOut[i];
            for (int j = 0; j < polygon->size; j++) {
                Vertex *vertex = &core->gpu3D.verticesOut[polygon->vertices + j];
                if (vertex->y < polygonTop[i]) polygonTop[i] = vertex->y;
                if (vertex->y > polygonBot[i]) polygonBot[i] = vertex->y;
            }

            if (polygonTop[i] == polygonBot[i]) polygonBot[i]++;
            // v1.72: cache texture/palette base pointers for fast per-pixel fetch (safe for dynamic scenes — recomputed each frame)
            _Polygon *poly = &core->gpu3D.polygonsOut[i];
            if (poly->textureFmt != 0) {
                uint8_t *texSlot = core->memory.tex3D[poly->textureAddr >> 17];
                uint32_t texOff = poly->textureAddr & 0x1FFFF;
                cachedTexSlot0[i] = texSlot ? texSlot + texOff : nullptr;
                uint32_t endAddr = poly->textureAddr + poly->sizeS * poly->sizeT * (poly->textureFmt==7?2:1);
                uint32_t endSlot = endAddr >> 17;
                if (endSlot != (poly->textureAddr>>17) && endSlot < 4)
                    cachedTexSlot1[i] = core->memory.tex3D[endSlot];
                else
                    cachedTexSlot1[i] = nullptr;
                uint8_t *palSlot = (poly->paletteAddr < (6<<14)) ? core->memory.pal3D[poly->paletteAddr >> 14] : nullptr;
                cachedPalSlot[i] = palSlot ? palSlot + (poly->paletteAddr & 0x3FFF) : nullptr;
                cachedUseFastTex[i] = (cachedTexSlot0[i] && (endSlot == (poly->textureAddr>>17) || !cachedTexSlot1[i]) && cachedPalSlot[i]);
                if (poly->textureFmt==5) {
                    uint32_t palTexAddr = 0x20000 + (poly->textureAddr % 0x20000)/2 + ((poly->textureAddr/0x20000==2)?0x10000:0);
                    uint8_t *p = core->memory.tex3D[palTexAddr>>17];
                    cachedTexPalBase[i] = p ? p + (palTexAddr & 0x1FFFF) : nullptr;
                } else cachedTexPalBase[i]=nullptr;
            } else {
                cachedTexSlot0[i]=cachedTexSlot1[i]=cachedPalSlot[i]=cachedTexPalBase[i]=nullptr;
                cachedUseFastTex[i]=false;
            }
        }

        if (edgeMark) {
            for (int i = 0; i < polyCount; i++) {
                _Polygon *polygon = &core->gpu3D.polygonsOut[i];
                if (polygon->size != 3 || polygon->alpha != 63 || polygonSharesSameIdOpaqueEdge[i])
                    continue;
                for (int otherIndex = i + 1; otherIndex < polyCount; otherIndex++) {
                    _Polygon *other = &core->gpu3D.polygonsOut[otherIndex];
                    if (other->size < 3 || other->alpha != 63 || other->id != polygon->id)
                        continue;
                    bool shared = false;
                    for (int a = 0; a < polygon->size && !shared; a++) {
                        Vertex *a0 = &core->gpu3D.verticesOut[polygon->vertices + a];
                        Vertex *a1 = &core->gpu3D.verticesOut[polygon->vertices + ((a + 1) % polygon->size)];
                        for (int b = 0; b < other->size; b++) {
                            Vertex *b0 = &core->gpu3D.verticesOut[other->vertices + b];
                            Vertex *b1 = &core->gpu3D.verticesOut[other->vertices + ((b + 1) % other->size)];
                            if ((a0->x == b0->x && a0->y == b0->y && a1->x == b1->x && a1->y == b1->y) ||
                                (a0->x == b1->x && a0->y == b1->y && a1->x == b0->x && a1->y == b0->y)) {
                                shared = true;
                                break;
                            }
                        }
                    }
                    if (shared) {
                        polygonSharesSameIdOpaqueEdge[i] = true;
                        if (other->size == 3)
                            polygonSharesSameIdOpaqueEdge[otherIndex] = true;
                    }
                }
            }
        }

        resShift = Settings::highRes3D;
        // v1.73: build per-scanline active polygon lists (single pass, O(poly*height) once vs per-line scan)
        {
            int h = 192 << resShift;
            for (int y=0;y<h;y++) { linePolyCount[y]=0; linePolyOverflow[y]=false; }
            for (int i=0;i<polyCount;i++) {
                int top = polygonTop[i]; if (top<0) top=0; if (top>=h) continue;
                int bot = polygonBot[i]; if (bot>h) bot=h; if (bot<0) continue;
                for (int y=top; y<bot; y++) {
                    int c = linePolyCount[y];
                    if (c < 256) linePolyIndices[y][c] = i;
                    else linePolyOverflow[y]=true;
                    linePolyCount[y]++;
                }
            }
        }

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

    int translucent[2048];
    int translucentCount = 0;
    // v1.73: use prebuilt per-line lists (built once at line 0)
    int cnt = linePolyCount[line];
    if (!linePolyOverflow[line]) {
        for (int k=0;k<cnt;k++) {
            int i = linePolyIndices[line][k];
            _Polygon *polygon = &core->gpu3D.polygonsOut[i];
            if (polygon->alpha < 0x3F || polygon->textureFmt == 1 || polygon->textureFmt == 6)
                translucent[translucentCount++] = i;
            else
                drawPolygon(line, i);
        }
    } else {
        // overflow fallback: original scan (rare, >256 polys in one line)
        for (int i = 0; i < core->gpu3D.polygonCountOut; i++) {
            if (line < polygonTop[i] || line >= polygonBot[i]) continue;
            _Polygon *polygon = &core->gpu3D.polygonsOut[i];
            if (polygon->alpha < 0x3F || polygon->textureFmt == 1 || polygon->textureFmt == 6)
                translucent[translucentCount++] = i;
            else
                drawPolygon(line, i);
        }
    }
    for (int i = 0; i < translucentCount; i++)
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

FORCE_INLINE uint32_t Gpu3DRenderer::interpolateLinear(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2) {
    if (__builtin_expect(x <= x1, 0)) return v1;
    if (__builtin_expect(x >= x2, 0)) return v2;
    if (v1 == v2) return v1;
    if (v1 <= v2)
        return v1 + udivSpan((v2 - v1) * (x - x1), x2 - x1);
    else
        return v2 + udivSpan((v1 - v2) * (x2 - x), x2 - x1);
}
FORCE_INLINE uint32_t Gpu3DRenderer::interpolateLinRev(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2) {
    if (__builtin_expect(x <= x1, 0)) return v1;
    if (__builtin_expect(x >= x2, 0)) return v2;
    if (v1 == v2) return v1;
    if (v1 <= v2)
        return v2 - udivSpan((v2 - v1) * (x2 - x), x2 - x1);
    else
        return v1 - udivSpan((v1 - v2) * (x - x1), x2 - x1);
}
FORCE_INLINE uint32_t Gpu3DRenderer::interpolateFactor(uint32_t factor, uint32_t shift, uint32_t v1, uint32_t v2) {
    if (v1 == v2) return v1;
    if (v1 <= v2)
        return v1 + (((v2 - v1) * factor) >> shift);
    else
        return v2 + (((v1 - v2) * ((1 << shift) - factor)) >> shift);
}
FORCE_INLINE uint32_t Gpu3DRenderer::interpolateColor(uint32_t c1, uint32_t c2, uint32_t x1, uint32_t x, uint32_t x2) {
    // v1.72: fast path when colors equal on that channel
    uint32_t r1=(c1>>0)&0x3F, r2=(c2>>0)&0x3F;
    uint32_t g1=(c1>>6)&0x3F, g2=(c2>>6)&0x3F;
    uint32_t b1=(c1>>12)&0x3F,b2=(c2>>12)&0x3F;
    uint32_t r = (r1==r2)?r1:interpolateLinear(r1,r2,x1,x,x2);
    uint32_t g = (g1==g2)?g1:interpolateLinear(g1,g2,x1,x,x2);
    uint32_t b = (b1==b2)?b1:interpolateLinear(b1,b2,x1,x,x2);
    uint32_t a = (((c1 >> 18) & 0x3F) > ((c2 >> 18) & 0x3F)) ? ((c1 >> 18) & 0x3F) : ((c2 >> 18) & 0x3F);
    return (a << 18) | (b << 12) | (g << 6) | r;
}

uint32_t Gpu3DRenderer::readTexture(_Polygon *polygon, int s, int t) {
    return readTextureCached(-1, polygon, s, t);
}
uint32_t Gpu3DRenderer::readTextureCached(int polyIdx, _Polygon *polygon, int s, int t) {
    // v1.72: branchless clamp/wrap using bit ops where possible
    if (polygon->repeatS) {
        if (polygon->flipS && (s & polygon->sizeS))
            s = -1 - s;
        s &= polygon->sizeS - 1;
    } else {
        // branchless clamp: s = max(0, min(s, sizeS-1))
        s = s < 0 ? 0 : (s >= polygon->sizeS ? polygon->sizeS - 1 : s);
    }
    if (polygon->repeatT) {
        if (polygon->flipT && (t & polygon->sizeT))
            t = -1 - t;
        t &= polygon->sizeT - 1;
    } else {
        t = t < 0 ? 0 : (t >= polygon->sizeT ? polygon->sizeT - 1 : t);
    }

    switch (polygon->textureFmt) {
    case 1: {
        uint8_t index;
        if (polyIdx >=0 && cachedUseFastTex[polyIdx]) {
            index = cachedTexSlot0[polyIdx][t * polygon->sizeS + s];
        } else {
            uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s);
            uint8_t *data = getTexture(address);
            if (!data) return 0;
            index = *data;
        }
        uint8_t *palette = (polyIdx>=0 && cachedPalSlot[polyIdx]) ? cachedPalSlot[polyIdx] : getPalette(polygon->paletteAddr);
        if (!palette) return 0;
        uint16_t color = U8TO16(palette, (index & 0x1F) * 2) & ~BIT(15);
        uint8_t alpha = (index >> 5) * 4 + (index >> 5) / 2;
        return rgba5ToRgba6((alpha << 15) | color);
    }

    case 2: {
        uint8_t index;
        if (polyIdx >=0 && cachedUseFastTex[polyIdx]) {
            uint32_t off = (t * polygon->sizeS + s) / 4;
            uint8_t data = cachedTexSlot0[polyIdx][off];
            index = (data >> ((s % 4) * 2)) & 0x03;
        } else {
            uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s) / 4;
            uint8_t *data = getTexture(address);
            if (!data) return 0;
            index = (*data >> ((s % 4) * 2)) & 0x03;
        }
        if (polygon->transparent0 && index == 0)
            return 0;
        uint8_t *palette = (polyIdx>=0 && cachedPalSlot[polyIdx]) ? cachedPalSlot[polyIdx] : getPalette(polygon->paletteAddr);
        if (!palette) return 0;
        return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
    }

    case 3: {
        uint8_t index;
        if (polyIdx >=0 && cachedUseFastTex[polyIdx]) {
            uint32_t off = (t * polygon->sizeS + s) / 2;
            uint8_t data = cachedTexSlot0[polyIdx][off];
            index = (data >> ((s % 2) * 4)) & 0x0F;
        } else {
            uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s) / 2;
            uint8_t *data = getTexture(address);
            if (!data) return 0;
            index = (*data >> ((s % 2) * 4)) & 0x0F;
        }
        if (polygon->transparent0 && index == 0)
            return 0;
        uint8_t *palette = (polyIdx>=0 && cachedPalSlot[polyIdx]) ? cachedPalSlot[polyIdx] : getPalette(polygon->paletteAddr);
        if (!palette) return 0;
        return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
    }

    case 4: {
        uint8_t index;
        if (polyIdx >=0 && cachedUseFastTex[polyIdx]) {
            index = cachedTexSlot0[polyIdx][t * polygon->sizeS + s];
        } else {
            uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s);
            uint8_t *data = getTexture(address);
            if (!data) return 0;
            index = *data;
        }
        if (polygon->transparent0 && index == 0)
            return 0;
        uint8_t *palette = (polyIdx>=0 && cachedPalSlot[polyIdx]) ? cachedPalSlot[polyIdx] : getPalette(polygon->paletteAddr);
        if (!palette) return 0;
        return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
    }

    case 5: {
        int tile = (t / 4) * (polygon->sizeS / 4) + (s / 4);
        uint32_t address = polygon->textureAddr + (tile * 4 + t % 4);
        uint8_t *data;
        uint8_t index;
        if (polyIdx>=0 && cachedTexSlot0[polyIdx]) {
            // texture data is contiguous from slot base; tile addressing falls inside slot0 for size <=64KB
            data = cachedTexSlot0[polyIdx] + ((address - polygon->textureAddr) & 0x1FFFF);
            if (!data) return 0;
            index = (*data >> ((s % 4) * 2)) & 0x03;
            // palTex uses separate slot
            if (cachedTexPalBase[polyIdx]) data = cachedTexPalBase[polyIdx] + (tile * 2);
            else { address = 0x20000 + (polygon->textureAddr % 0x20000) / 2 + ((polygon->textureAddr / 0x20000 == 2) ? 0x10000 : 0); if (!(data = getTexture(address + tile * 2))) return 0; }
        } else {
            if (!(data = getTexture(address))) return 0;
            index = (*data >> ((s % 4) * 2)) & 0x03;
            address = 0x20000 + (polygon->textureAddr % 0x20000) / 2 + ((polygon->textureAddr / 0x20000 == 2) ? 0x10000 : 0);
            if (!(data = getTexture(address + tile * 2))) return 0;
        }
        uint16_t palBase = U8TO16(data, 0);
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
        uint8_t index;
        if (polyIdx >=0 && cachedUseFastTex[polyIdx]) {
            index = cachedTexSlot0[polyIdx][t * polygon->sizeS + s];
        } else {
            uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s);
            uint8_t *data = getTexture(address);
            if (!data) return 0;
            index = *data;
        }
        uint8_t *palette = (polyIdx>=0 && cachedPalSlot[polyIdx]) ? cachedPalSlot[polyIdx] : getPalette(polygon->paletteAddr);
        if (!palette) return 0;
        uint16_t color = U8TO16(palette, (index & 0x07) * 2) & ~BIT(15);
        uint8_t alpha = index >> 3;
        return rgba5ToRgba6((alpha << 15) | color);
    }

    default: {
        uint8_t *data;
        if (polyIdx>=0 && cachedUseFastTex[polyIdx]) {
            data = cachedTexSlot0[polyIdx] - (polygon->textureAddr & 0x1FFFF) + (polygon->textureAddr & 0x1FFFF); // base already offset, but offset for default is different (direct color)
            // For default (direct color) the tex is at base address, we need base+ (t*sizeS+s)*2
            // cachedTexSlot0 is base at texAddr, so offset*2
            uint32_t off = (t * polygon->sizeS + s) * 2;
            uint16_t color = U8TO16(data, off);
            uint8_t alpha = (color & BIT(15)) ? 0x1F : 0;
            return rgba5ToRgba6((alpha << 15) | color);
        } else {
            data = getTexture(polygon->textureAddr);
            if (!data) return 0;
            uint16_t color = U8TO16(data, (t * polygon->sizeS + s) * 2);
            uint8_t alpha = (color & BIT(15)) ? 0x1F : 0;
            return rgba5ToRgba6((alpha << 15) | color);
        }
    }}
}

void Gpu3DRenderer::drawPolygon(int line, int polygonIndex) {
    _Polygon *polygon = &core->gpu3D.polygonsOut[polygonIndex];

    Vertex *vertices[10];
    for (int i = 0; i < polygon->size; i++)
        vertices[i] = &core->gpu3D.verticesOut[polygon->vertices + i];

    if (polygon->crossed)
        SWAP(vertices[2], vertices[3]);

    bool shallowXMajorEdge = false;
    if (polygon->size == 4) {
        for (int edge = 0; edge < 4; edge++) {
            Vertex *a = vertices[edge];
            Vertex *b = vertices[(edge + 1) & 3];
            if (abs(b->x - a->x) > abs(b->y - a->y) * 4)
                shallowXMajorEdge = true;
        }
    }

    bool duplicateAdjacent = false;
    bool allSameY = true;
    bool thinTriangle = false;
    for (int i = 0; i < polygon->size; i++) {
        int next = (i + 1) % polygon->size;
        if (vertices[i]->x == vertices[next]->x && vertices[i]->y == vertices[next]->y)
            duplicateAdjacent = true;
        if (vertices[i]->y != vertices[0]->y)
            allSameY = false;
    }
    if (polygon->size == 3 && !duplicateAdjacent && !allSameY) {
        int64_t dx1 = int64_t(vertices[1]->x) - vertices[0]->x;
        int64_t dy1 = int64_t(vertices[1]->y) - vertices[0]->y;
        int64_t dx2 = int64_t(vertices[2]->x) - vertices[0]->x;
        int64_t dy2 = int64_t(vertices[2]->y) - vertices[0]->y;
        int64_t area = dx1 * dy2 - dy1 * dx2;
        if (area == 0) return;

        int64_t maxDelta = abs(vertices[1]->x - vertices[0]->x);
        if (abs(vertices[1]->y - vertices[0]->y) > maxDelta) maxDelta = abs(vertices[1]->y - vertices[0]->y);
        if (abs(vertices[2]->x - vertices[0]->x) > maxDelta) maxDelta = abs(vertices[2]->x - vertices[0]->x);
        if (abs(vertices[2]->y - vertices[0]->y) > maxDelta) maxDelta = abs(vertices[2]->y - vertices[0]->y);
        if (abs(vertices[2]->x - vertices[1]->x) > maxDelta) maxDelta = abs(vertices[2]->x - vertices[1]->x);
        if (abs(vertices[2]->y - vertices[1]->y) > maxDelta) maxDelta = abs(vertices[2]->y - vertices[1]->y);

        thinTriangle = (area > -maxDelta && area < maxDelta);
    }

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

    // Degenerate horizontal polygons use one of their surviving line segments,
    // rather than the two independently selected scanline chains above.
    bool horizontalLine = polygon->size >= 3;
    for (int i = 1; i < polygon->size; i++)
        if (vertices[i]->y != vertices[0]->y) horizontalLine = false;
    if (horizontalLine && line == vertices[0]->y) {
        int left = -1, right = -1;
        bool clippedAtLeft = false;
        for (int i = 0; i < polygon->size; i++)
            if (vertices[i]->x == 0) clippedAtLeft = true;

        if (clippedAtLeft) {
            for (int i = 0; i < polygon->size; i++) {
                if (vertices[i]->x == 0 && left < 0) left = i;
                if (vertices[i]->x > 0 && (right < 0 || vertices[i]->x < vertices[right]->x)) right = i;
            }
        }
        else {
            for (int i = 0; i < polygon->size; i++) {
                if (polygon->size == 4 && i == 2) continue;
                if (left < 0 || vertices[i]->x < vertices[left]->x) left = i;
                if (right < 0 || vertices[i]->x > vertices[right]->x) right = i;
            }
        }

        if (left >= 0 && right >= 0) {
            v[0] = left; v[1] = right;
            v[2] = left; v[3] = right;
        }
    }

    uint32_t x1, x2, x3, x4;
    uint32_t x1a = 0x3F, x2a = 0x3F, x3a = 0x3F, x4a = 0x3F;
    bool aaRightXMajor = false;
    int aaRightX0 = 0, aaRightY0 = 0, aaRightX1 = 0, aaRightY1 = 0;

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
        int dx = abs(vertices[v[1]]->x - vertices[v[0]]->x);
        int dy = vertices[v[1]]->y - vertices[v[0]]->y;
        if (vertices[v[0]]->x > vertices[v[1]]->x) {
            x1 = interpolateLinRev(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6, vertices[v[0]]->y, line, vertices[v[1]]->y);
            if (dx == dy || line == vertices[v[0]]->y || (x1 & 0x3F) != 0) x1--;
        }
        else {
            x1 = interpolateLinear(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6, vertices[v[0]]->y, line, vertices[v[1]]->y);
            if (dx > 0 && dx < dy && line > vertices[v[0]]->y && (x1 & 0x3F) == 0 && x1 >= 0x40)
                x1 -= 0x40;
        }

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
        aaRightXMajor = true;
        aaRightX0 = vertices[v[2]]->x;
        aaRightY0 = vertices[v[2]]->y;
        aaRightX1 = vertices[v[3]]->x;
        aaRightY1 = vertices[v[3]]->y;
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
        int dx = abs(vertices[v[3]]->x - vertices[v[2]]->x);
        int dy = vertices[v[3]]->y - vertices[v[2]]->y;
        if (vertices[v[2]]->x > vertices[v[3]]->x) {
            x3 = interpolateLinRev(vertices[v[2]]->x << 6, vertices[v[3]]->x << 6, vertices[v[2]]->y, line, vertices[v[3]]->y);
            if (dx == dy || line == vertices[v[2]]->y || (x3 & 0x3F) != 0) x3--;
        }
        else {
            x3 = interpolateLinear(vertices[v[2]]->x << 6, vertices[v[3]]->x << 6, vertices[v[2]]->y, line, vertices[v[3]]->y);
            if (dx > 0 && dx < dy && line > vertices[v[2]]->y && (x3 & 0x3F) == 0 && x3 >= 0x40)
                x3 -= 0x40;
        }

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

    bool oldRightEdgeWasVertical = (vertices[v[2]]->x == vertices[v[3]]->x);
    bool swappedEdges = (x1 > x4);
    if (swappedEdges) {
        SWAP(v[0], v[2]);
        SWAP(v[1], v[3]);
        SWAP(x1, x3);
        SWAP(x2, x4);
        SWAP(x1a, x3a);
        SWAP(x2a, x4a);
    }

    // The vertical-right-edge shift follows the edge after chain reordering,
    // but must not be applied twice to an edge that was already vertical.
    if (swappedEdges && !oldRightEdgeWasVertical && polygon->alpha == 63 && !(disp3DCnt & BIT(4)) &&
        vertices[v[2]]->x == vertices[v[3]]->x &&
        (vertices[v[0]]->x != vertices[v[1]]->x || vertices[v[0]]->x != vertices[v[2]]->x) && x3 > 0) {
        x3--;
        x4--;
    }

    // Correct the exact-boundary bias on reordered opaque quad Y-major edges.
    if (polygon->size == 4 && polygon->alpha == 63 && !(disp3DCnt & BIT(4))) {
        if (swappedEdges) {
            int leftDx = abs(vertices[v[1]]->x - vertices[v[0]]->x);
            int leftDy = vertices[v[1]]->y - vertices[v[0]]->y;
            if (leftDx > 0 && leftDx < leftDy && vertices[v[0]]->x > vertices[v[1]]->x &&
                line > vertices[v[0]]->y) {
                uint32_t leftRaw = interpolateLinRev(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6,
                    vertices[v[0]]->y, line, vertices[v[1]]->y);
                if ((leftRaw & 0x3F) == 0 && x1 > 0) x1--;
            }

            int rightDx = abs(vertices[v[3]]->x - vertices[v[2]]->x);
            int rightDy = vertices[v[3]]->y - vertices[v[2]]->y;
            if (rightDx > 0 && rightDx < rightDy && vertices[v[2]]->x < vertices[v[3]]->x &&
                line > vertices[v[2]]->y) {
                uint32_t rightRaw = interpolateLinear(vertices[v[2]]->x << 6, vertices[v[3]]->x << 6,
                    vertices[v[2]]->y, line, vertices[v[3]]->y);
                if ((rightRaw & 0x3F) == 0) { x3++; x4++; }
            }
        }
        else if (x1 == x4 && vertices[v[0]]->x < vertices[v[1]]->x) {
            int leftDx = abs(vertices[v[1]]->x - vertices[v[0]]->x);
            int leftDy = vertices[v[1]]->y - vertices[v[0]]->y;
            if (leftDx > 0 && leftDx < leftDy && line == vertices[v[0]]->y + 1 && x1 > 0) {
                uint32_t leftRaw = interpolateLinear(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6,
                    vertices[v[0]]->y, line, vertices[v[1]]->y);
                if ((leftRaw & 0x3F) != 0) x1--;
            }
        }
    }

    // A translucent blended quad can also need the exact-boundary correction
    // after its original vertical right edge moves to a Y-major right span.
    if (polygon->size == 4 && polygon->alpha > 0 && polygon->alpha < 63 &&
        (disp3DCnt & BIT(3)) && !(disp3DCnt & BIT(4)) &&
        swappedEdges && oldRightEdgeWasVertical) {
        int rightDx = abs(vertices[v[3]]->x - vertices[v[2]]->x);
        int rightDy = vertices[v[3]]->y - vertices[v[2]]->y;
        if (rightDx > 0 && rightDx < rightDy && vertices[v[2]]->x < vertices[v[3]]->x &&
            line > vertices[v[2]]->y) {
            uint32_t rightRaw = interpolateLinear(vertices[v[2]]->x << 6, vertices[v[3]]->x << 6,
                vertices[v[2]]->y, line, vertices[v[3]]->y);
            if ((rightRaw & 0x3F) == 0) { x3++; x4++; }
        }
    }

    // X-major translucent triangle edges use the DS's quantized 18-bit
    // slope accumulator rather than a fresh exact division on every row.
    if (polygon->size == 3 && polygon->alpha > 0 && polygon->alpha < 63 &&
        !(disp3DCnt & (BIT(4) | BIT(5)))) {
        int32_t edge0 = v[0], edge1 = v[1];
        int32_t x0 = vertices[edge0]->x, y0 = vertices[edge0]->y;
        int32_t x1v = vertices[edge1]->x, y1v = vertices[edge1]->y;
        if (y0 > y1v) {
            int32_t tmp = x0; x0 = x1v; x1v = tmp;
            tmp = y0; y0 = y1v; y1v = tmp;
        }
        int32_t edgeDx = x1v - x0;
        int32_t edgeDy = y1v - y0;
        int32_t edgeXLen = abs(edgeDx);
        if (edgeDy > 0 && edgeXLen > edgeDy) {
            int32_t reciprocal = (1 << 18) / edgeDy;
            int32_t increment = abs(edgeDx * reciprocal);
            bool negative = (edgeDx < 0);
            bool xMajor = increment > 0x40000;
            if (xMajor) {
                int32_t dx;
                bool rightSide = swappedEdges;
                if (rightSide)
                    dx = negative ? 0x60000 : increment - 0x20000;
                else
                    dx = negative ? increment + 0x20000 : 0x20000;
                dx += (line - y0) * increment;

                int32_t edgeStart = negative ? x0 - (dx >> 18) : x0 + (dx >> 18);
                int32_t edgeMin = (x1v < x0) ? x1v : x0;
                int32_t edgeMax = ((x1v > x0) ? x1v : x0) - 1;
                if (edgeStart < edgeMin) edgeStart = edgeMin;
                if (edgeStart > edgeMax) edgeStart = edgeMax;

                int32_t edgeLength;
                if (rightSide ^ negative)
                    edgeLength = (dx >> 18) - ((dx - increment) >> 18);
                else
                    edgeLength = ((dx + increment) >> 18) - (dx >> 18);

                if (edgeLength > 0 && edgeStart >= 0 && edgeStart < 256) {
                    x1 = (uint32_t)edgeStart;
                    x2 = (uint32_t)(edgeStart + edgeLength - 1);
                }
            }
        }
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

    if (polygon->size == 3 && duplicateAdjacent)
        hideLeft = false;

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

    if (thinTriangle && line >= polygonTop[polygonIndex] +
        (polygonBot[polygonIndex] - polygonTop[polygonIndex] + 1) / 2)
        hideRight = true;

    // When chain reordering moves an originally vertical right edge, the DS
    // keeps its always-filled edge decision for a non-X-major right span.
    if (polygon->size == 4 && swappedEdges && oldRightEdgeWasVertical &&
        abs(vertices[v[3]]->x - vertices[v[2]]->x) <= abs(vertices[v[3]]->y - vertices[v[2]]->y))
        hideRight = false;

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

    const bool sharesSameIdOpaqueEdge = polygonSharesSameIdOpaqueEdge[polygonIndex];

    uint32_t x1e = x1, x4e = ++x4;

    if (polygon->alpha != 0 && !(disp3DCnt & BIT(4)) &&
        (!(disp3DCnt & BIT(5)) || (polygon->size == 3 && sharesSameIdOpaqueEdge))) {
        if (hideLeft) x1e = x2 + 1;
        if (hideRight) x4e = x3;
        if (!(hideLeft && hideRight) && x4e <= x1e)
            x4e = x1e + 1;
    }

    // A duplicated top vertex in a line-shaped quad emits the far sample of
    // the collapsed first span, not the whole zero-width start.
    if (polygon->size == 4 && polygon->alpha == 63 &&
        !(disp3DCnt & (BIT(4) | BIT(5))) && !swappedEdges && hideLeft &&
        line == polygonTop[polygonIndex] &&
        vertices[v[0]]->x == vertices[v[2]]->x && vertices[v[0]]->y == vertices[v[2]]->y &&
        x1 == x2 && x2 == x3 && x4 > x1 + 2)
        x1e = x4 - 1;

    // Once both edge chains collapse onto the same line, keep its leftmost
    // sample; ordinary left-edge hiding otherwise drops it on every row.
    if (polygon->size == 4 && polygon->alpha == 63 &&
        !(disp3DCnt & (BIT(4) | BIT(5))) && !swappedEdges && hideLeft &&
        vertices[v[0]]->x == vertices[v[2]]->x && vertices[v[0]]->y == vertices[v[2]]->y &&
        vertices[v[1]]->x == vertices[v[3]]->x && vertices[v[1]]->y == vertices[v[3]]->y &&
        line > vertices[v[0]]->y) {
        x1e = x1;
        const uint32_t lineRaw = interpolateLinear(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6,
            vertices[v[0]]->y, line, vertices[v[1]]->y);
        const uint32_t nextRaw = interpolateLinear(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6,
            vertices[v[0]]->y, line + 1, vertices[v[1]]->y);
        // Exact half-pixel endpoints belong to the lower sample on this shared line.
        if ((lineRaw & 0x3F) == 0x20 && x1 > 0) {
            x1--;
            x1e = x1;
        }
        if ((nextRaw & 0x3F) == 0x20 && x4 > 0) {
            x4--;
            x4e = x4;
        }
    }

    // Shared-start quads retain the next fixed-point sample at this specific
    // Q6 edge phase; include x2 instead of discarding the last covered pixel.
    if (polygon->size == 4 && polygon->alpha == 63 &&
        !(disp3DCnt & (BIT(4) | BIT(5))) && !swappedEdges && hideLeft &&
        vertices[v[0]]->x == vertices[v[2]]->x && vertices[v[0]]->y == vertices[v[2]]->y &&
        (vertices[v[1]]->x != vertices[v[3]]->x || vertices[v[1]]->y != vertices[v[3]]->y) &&
        vertices[v[0]]->x < vertices[v[1]]->x && vertices[v[0]]->y < vertices[v[1]]->y &&
        x2 > x1) {
        const uint32_t leftRaw = interpolateLinear(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6,
            vertices[v[0]]->y, line, vertices[v[1]]->y);
        if ((leftRaw & 0x3F) == 0x31)
            x1e = x2;
    }

    // On a positive X-major left edge, the edge pixel at x2 is included
    // on scanlines where the quantized edge advance equals the integer
    // slope. The bottom-most scanline also includes a collapsed edge pixel.
    if (polygon->size == 4 && polygon->alpha == 63 &&
        !(disp3DCnt & (BIT(4) | BIT(5))) && !swappedEdges && hideLeft &&
        vertices[v[0]]->x < vertices[v[1]]->x &&
        vertices[v[0]]->y < vertices[v[1]]->y &&
        vertices[v[1]]->y == vertices[v[2]]->y) {
        const int32_t leftDx = vertices[v[1]]->x - vertices[v[0]]->x;
        const int32_t leftDy = vertices[v[1]]->y - vertices[v[0]]->y;
        if (leftDx > leftDy && leftDy > 0 && x2 >= x1 &&
            (int32_t)(x2 - x1) == leftDx / leftDy)
            x1e = x2;
        if (line == polygonBot[polygonIndex] - 1 && vertices[v[1]]->y == vertices[v[2]]->y) {
            if (x1 == x2)
                x1e = x1;
            if (x3 + 1 == x4)
                x4e = x4;
        }
    }

    // When the bottom edge is clipped at the screen boundary, include both
    // scanline edge spans rather than dropping their endpoint pixels.
    if (polygon->size == 4 && polygon->alpha == 63 &&
        !(disp3DCnt & (BIT(4) | BIT(5))) && !swappedEdges &&
        line == polygonBot[polygonIndex] - 1 &&
        vertices[v[1]]->y >= 192 && vertices[v[2]]->y >= 192) {
        if (x1 > 0) x1--;
        x1e = x1;
        x4e = x4;
    }

    bool horizontal = (line == polygonTop[polygonIndex] || line == polygonBot[polygonIndex] - 1);

    // A flat-bottom opaque quad with an X-major down-right edge has one
    // exclusive sample on swapped rows, but keeps its full closing endpoint.
    const bool flatBottomXMajorQuad = polygon->size == 4 && polygon->alpha == 63 &&
        !(disp3DCnt & (BIT(4) | BIT(5))) &&
        vertices[0]->y < vertices[1]->y && vertices[1]->y == vertices[2]->y &&
        vertices[0]->y < vertices[3]->y && vertices[3]->y < vertices[1]->y &&
        vertices[3]->x < vertices[0]->x && vertices[0]->x < vertices[1]->x &&
        vertices[1]->x < vertices[2]->x &&
        vertices[1]->x - vertices[0]->x > vertices[1]->y - vertices[0]->y;
    if (flatBottomXMajorQuad && x2 > x1) {
        if (swappedEdges && !horizontal && x1e == x1)
            x1e = x2;
        else if (!swappedEdges && horizontal && line == polygonBot[polygonIndex] - 1 && x1e > x1)
            x1e = x1;
    }

    // A wireframe quad's advancing X-major left edge carries the next
    // endpoint onto the following scanline instead of drawing it early.
    if (polygon->alpha == 0 && polygon->size == 4 && !horizontal && !swappedEdges &&
        vertices[v[0]]->x < vertices[v[1]]->x &&
        abs(vertices[v[1]]->x - vertices[v[0]]->x) > vertices[v[1]]->y - vertices[v[0]]->y &&
        line > vertices[v[0]]->y && line <= vertices[v[1]]->y) {
        const uint32_t rowX1 = x1;
        const bool deferEnd = (x2 > x1);
        uint32_t previousX1 = interpolateLinear(vertices[v[0]]->x << 1, vertices[v[1]]->x << 1,
            vertices[v[0]]->y, line - 1, vertices[v[1]]->y) + 1;
        uint32_t previousX2 = interpolateLinear(vertices[v[0]]->x << 1, vertices[v[1]]->x << 1,
            vertices[v[0]]->y, line, vertices[v[1]]->y) + 1;
        previousX1 >>= 1;
        previousX2 >>= 1;
        if (previousX2 > previousX1) previousX2--;
        if (previousX2 > previousX1 && previousX2 < x1) {
            x1 = previousX2;
            x1e = previousX2;
        }
        if (deferEnd)
            x2 = rowX1;
    }

    int lastS = 0xFFFF, lastT = 0xFFFF;
    uint32_t texel;

    // v1.75: DDA for linear spans (factor==-1) — avoid per-pixel divisions
    struct DDA { int32_t cur; int32_t q; int32_t r; int32_t err; bool inc; int w; };
    auto initDDA = [](DDA &d, uint32_t v0, uint32_t v1, int spanW){
        d.w = spanW;
        if (v0 <= v1) {
            uint32_t delta = v1 - v0;
            d.q = delta / (uint32_t)spanW;
            d.r = delta % (uint32_t)spanW;
            d.cur = (int32_t)v0;
            d.err = 0;
            d.inc = true;
        } else {
            uint32_t delta = v0 - v1;
            d.q = delta / (uint32_t)spanW;
            d.r = delta % (uint32_t)spanW;
            d.cur = (int32_t)v0;
            d.err = spanW - 1;
            d.inc = false;
        }
    };
    bool isLinear = (we[0] == we[1] && !(we[0] & 0x7F) && polygon->alpha != 0);
    int spanW = (int)(x4 - x1); if (spanW <= 0) spanW = 1;
    DDA dDepth, dR, dG, dB, dS, dT;
    if (isLinear) {
        if (polygon->wBuffer) initDDA(dDepth, we[0], we[1], spanW);
        else initDDA(dDepth, ze[0], ze[1], spanW);
        initDDA(dR, re[0], re[1], spanW);
        initDDA(dG, ge[0], ge[1], spanW);
        initDDA(dB, be[0], be[1], spanW);
        if (polygon->textureFmt != 0) {
            initDDA(dS, se[0]+0xFFFF, se[1]+0xFFFF, spanW);
            initDDA(dT, te[0]+0xFFFF, te[1]+0xFFFF, spanW);
        }
    }
    struct DDAGuard {
        DDA *a,*b,*c,*d,*e,*f;
        DDAGuard(DDA *aa,DDA *bb,DDA *cc,DDA *dd,DDA *ee,DDA *ff):a(aa),b(bb),c(cc),d(dd),e(ee),f(ff){}
        ~DDAGuard(){ auto s=[](DDA *p){ if(!p) return; if(p->inc){ p->cur+=p->q; p->err+=p->r; if(p->err>=p->w){p->cur++; p->err-=p->w;}} else { p->cur-=p->q; p->err+=p->r; if(p->err>=p->w){p->cur--; p->err-=p->w;}}}; s(a); s(b); s(c); s(d); s(e); s(f); }
    };
    if (isLinear) {
    for (uint32_t x = x1; x < x4; x++) {
        DDAGuard _guard(&dDepth, &dR, &dG, &dB, polygon->textureFmt? &dS:nullptr, polygon->textureFmt? &dT:nullptr);
        if (!horizontal && polygon->alpha == 0 && x == x2 + 1 && x3 > x2)
            x = x3;

        if (x >= (256 << resShift))
            break;

        bool layer = 0;
        int i = line * 256 * 2 + x;

        // v1.75 linear fast path — DDA (no per-pixel div)
        int32_t depth = dDepth.cur;
        if (polygon->wBuffer) {
            if (polygon->wShift > 0) depth <<= polygon->wShift;
            else if (polygon->wShift < 0) depth >>= -polygon->wShift;
        }

        bool depthPass[2];
        if (polygon->depthTestEqual) {
            uint32_t margin = (polygon->wBuffer ? 0xFF : 0x200);
            depthPass[0] = (depthBuffer[0][i] >= depth - margin && depthBuffer[0][i] <= depth + margin);
            depthPass[1] = (disp3DCnt & BIT(4)) && (attribBuffer[0][i] & BIT(14)) &&
                (depthBuffer[1][i] >= depth - margin && depthBuffer[1][i] <= depth + margin);
        }
        else {
            bool topQuadOutlineEdge = polygon->size == 4 && polygon->alpha == 63 &&
                (disp3DCnt & BIT(5)) && shallowXMajorEdge &&
                line == polygonTop[polygonIndex] + 1 && x >= (int)x3;
            bool sameIdOutlineEdge = topQuadOutlineEdge && (attribBuffer[0][i] & BIT(14)) &&
                ((attribBuffer[0][i] & 0x3F) == polygon->id);
            uint32_t pixelDepth24 = (uint32_t)depth & 0x00FFFFFF;
            bool wrappedDepthPass = (((uint32_t)depthBuffer[0][i] & 0x00FFFFFF) > pixelDepth24);
            depthPass[0] = (depthBuffer[0][i] > depth) || (sameIdOutlineEdge && wrappedDepthPass);
            depthPass[1] = (disp3DCnt & BIT(4)) && (attribBuffer[0][i] & BIT(14)) &&
                (depthBuffer[1][i] > depth);
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

        uint32_t rv = (uint32_t)dR.cur >> 3;
        uint32_t gv = (uint32_t)dG.cur >> 3;
        uint32_t bv = (uint32_t)dB.cur >> 3;
        uint32_t color = ((polygon->alpha ? polygon->alpha : 0x3F) << 18) | (bv << 12) | (gv << 6) | rv;

        if (polygon->textureFmt != 0) {
            int s = (int)(dS.cur - 0xFFFF) >> 4;
            int t = (int)(dT.cur - 0xFFFF) >> 4;

            if (s != lastS || t != lastT) {
                lastS = s; lastT = t;
                texel = readTextureCached(polygonIndex, polygon, s, t);
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

                if (x <= x2 && x >= x3) {
                    uint8_t leftAlpha = interpolateLinear(x1a, x2a, x1, x, x2);
                    uint8_t rightAlpha = interpolateLinear(x3a, x4a, x3, x, x4);
                    edgeAlpha = (leftAlpha > rightAlpha) ? leftAlpha : rightAlpha;
                }
                else if (x <= x2)
                    edgeAlpha = interpolateLinear(x1a, x2a, x1, x, x2);
                else if (x >= x3)
                    edgeAlpha = interpolateLinear(x3a, x4a, x3, x, x4);

                // DS edge coverage uses a truncated 18-bit reciprocal. A few
                // X-major endpoint samples therefore have exactly zero coverage
                // even when the linear edge-alpha interpolation rounds above zero.
                if (aaRightXMajor && !swappedEdges && x > x2 && x >= x3 &&
                    polygon->alpha == 63 && polygon->size > 4) {
                    int edgeDx = abs(aaRightX1 - aaRightX0);
                    int edgeDy = aaRightY1 - aaRightY0;
                    if (edgeDx > 0 && edgeDy > 0) {
                        int increment = edgeDx * ((1 << 18) / edgeDy);
                        if (increment > 0x40000) {
                            bool negative = aaRightX1 < aaRightX0;
                            int dx = negative ? 0x60000 : (increment - 0x20000);
                            dx += (line - aaRightY0) * increment;
                            int dxWhole = dx >> 18;
                            int length = (negative ? ((dx + increment) >> 18) - dxWhole :
                                dxWhole - ((dx - increment) >> 18));
                            int startX = negative ? edgeDx - dxWhole : dxWhole;
                            startX = startX - length + 1;
                            int xMin = (aaRightX0 < aaRightX1) ? aaRightX0 : aaRightX1;
                            int xMax = ((aaRightX0 > aaRightX1) ? aaRightX0 : aaRightX1) - 1;
                            int edgeEnd = negative ? aaRightX0 - dxWhole : aaRightX0 + dxWhole;
                            if (edgeEnd < xMin) edgeEnd = xMin;
                            if (edgeEnd > xMax) edgeEnd = xMax;
                            int edgeStart = xMin + startX;
                            if ((int)x >= edgeStart && (int)x <= edgeEnd) {
                                int startCov = (((startX << 10) + 0x1FF) * edgeDy) / edgeDx;
                                int xCov = (startCov & 0x3FF) + ((int)x - edgeStart) * ((edgeDy << 10) / edgeDx);
                                int coverage = 31 - (xCov >> 5);
                                if (coverage <= 0) edgeAlpha = 0;
                            }
                        }
                    }
                }
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
    } else {
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
            bool topQuadOutlineEdge = polygon->size == 4 && polygon->alpha == 63 &&
                (disp3DCnt & BIT(5)) && shallowXMajorEdge &&
                line == polygonTop[polygonIndex] + 1 && x >= (int)x3;
            bool sameIdOutlineEdge = topQuadOutlineEdge && (attribBuffer[0][i] & BIT(14)) &&
                ((attribBuffer[0][i] & 0x3F) == polygon->id);
            uint32_t pixelDepth24 = (uint32_t)depth & 0x00FFFFFF;
            bool wrappedDepthPass = (((uint32_t)depthBuffer[0][i] & 0x00FFFFFF) > pixelDepth24);
            depthPass[0] = (depthBuffer[0][i] > depth) || (sameIdOutlineEdge && wrappedDepthPass);
            depthPass[1] = (disp3DCnt & BIT(4)) && (attribBuffer[0][i] & BIT(14)) &&
                (depthBuffer[1][i] > depth);
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
                texel = readTextureCached(polygonIndex, polygon, s, t);
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

                if (x <= x2 && x >= x3) {
                    uint8_t leftAlpha = interpolateLinear(x1a, x2a, x1, x, x2);
                    uint8_t rightAlpha = interpolateLinear(x3a, x4a, x3, x, x4);
                    edgeAlpha = (leftAlpha > rightAlpha) ? leftAlpha : rightAlpha;
                }
                else if (x <= x2)
                    edgeAlpha = interpolateLinear(x1a, x2a, x1, x, x2);
                else if (x >= x3)
                    edgeAlpha = interpolateLinear(x3a, x4a, x3, x, x4);

                // DS edge coverage uses a truncated 18-bit reciprocal. A few
                // X-major endpoint samples therefore have exactly zero coverage
                // even when the linear edge-alpha interpolation rounds above zero.
                if (aaRightXMajor && !swappedEdges && x > x2 && x >= x3 &&
                    polygon->alpha == 63 && polygon->size > 4) {
                    int edgeDx = abs(aaRightX1 - aaRightX0);
                    int edgeDy = aaRightY1 - aaRightY0;
                    if (edgeDx > 0 && edgeDy > 0) {
                        int increment = edgeDx * ((1 << 18) / edgeDy);
                        if (increment > 0x40000) {
                            bool negative = aaRightX1 < aaRightX0;
                            int dx = negative ? 0x60000 : (increment - 0x20000);
                            dx += (line - aaRightY0) * increment;
                            int dxWhole = dx >> 18;
                            int length = (negative ? ((dx + increment) >> 18) - dxWhole :
                                dxWhole - ((dx - increment) >> 18));
                            int startX = negative ? edgeDx - dxWhole : dxWhole;
                            startX = startX - length + 1;
                            int xMin = (aaRightX0 < aaRightX1) ? aaRightX0 : aaRightX1;
                            int xMax = ((aaRightX0 > aaRightX1) ? aaRightX0 : aaRightX1) - 1;
                            int edgeEnd = negative ? aaRightX0 - dxWhole : aaRightX0 + dxWhole;
                            if (edgeEnd < xMin) edgeEnd = xMin;
                            if (edgeEnd > xMax) edgeEnd = xMax;
                            int edgeStart = xMin + startX;
                            if ((int)x >= edgeStart && (int)x <= edgeEnd) {
                                int startCov = (((startX << 10) + 0x1FF) * edgeDy) / edgeDx;
                                int xCov = (startCov & 0x3FF) + ((int)x - edgeStart) * ((edgeDy << 10) / edgeDx);
                                int coverage = 31 - (xCov >> 5);
                                if (coverage <= 0) edgeAlpha = 0;
                            }
                        }
                    }
                }
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
