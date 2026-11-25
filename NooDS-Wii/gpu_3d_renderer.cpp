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
#include "profile.h"
#include "settings.h"


#include <stdint.h>
// Exact equivalent of the software renderer's density calculation. depth is
// signed: retain C++ truncation toward zero when dividing it by 512.
static inline uint8_t noodsFogDensityFast(int32_t depth, uint16_t fogOffset,
                                          unsigned fogShift, const uint8_t *table) {
    int32_t offset = depth / 0x200 - fogOffset;
    uint8_t density;
    if (fogShift > 10) {
        density = table[(offset > 0) ? 31 : 0];
    } else {
        unsigned bits = 10 - fogShift;
        if (offset < (1 << bits)) {
            density = table[0];
        } else {
            unsigned n = ((unsigned)offset >> bits) - 1;
            if (n >= 31) {
                density = table[31];
            } else {
                unsigned m = (unsigned)offset & ((1u << bits) - 1);
                density = (table[n + 1] * m + table[n] * ((1u << bits) - m)) >> bits;
            }
        }
    }
    return density == 127 ? 128 : density;
}

#include <stdint.h>
// Exact legacy software-renderer semantics, including high flag bits.
static inline uint32_t noodsFogBlendFast(uint32_t pixel, uint32_t fog,
                                         unsigned density, bool alphaOnly) {
    if (density == 0)
        return alphaOnly ? pixel : ((pixel & 0x00ffffffu) | (1u << 26));
    unsigned a = (((fog >> 18) & 63u) * density + ((pixel >> 18) & 63u) * (128u - density)) / 128u;
    if (alphaOnly)
        return (pixel & ~(63u << 18)) | (a << 18);
    unsigned r = (((fog >> 0) & 63u) * density + ((pixel >> 0) & 63u) * (128u - density)) / 128u;
    unsigned g = (((fog >> 6) & 63u) * density + ((pixel >> 6) & 63u) * (128u - density)) / 128u;
    unsigned b = (((fog >> 12) & 63u) * density + ((pixel >> 12) & 63u) * (128u - density)) / 128u;
    return (1u << 26) | (a << 18) | (b << 12) | (g << 6) | r;
}
#if NOODS_FOG_BLEND_VERIFY
uint64_t g_fogBlendVerify[4] = {};
#endif
#if NOODS_FOG_VERIFY
uint64_t g_fogVerify[2] = {};
#endif

#ifndef NOODS_GX_SPAN_COVERAGE
#define NOODS_GX_SPAN_COVERAGE 0
#endif
#ifndef NOODS_GX_ACCURACY_FALLBACK
#define NOODS_GX_ACCURACY_FALLBACK 0
#endif
#ifndef NOODS_V160_GX_DELTA
#define NOODS_V160_GX_DELTA 1
#endif
#ifndef NOODS_GX_DELTA_MAX_POLYS
#define NOODS_GX_DELTA_MAX_POLYS 2048
#endif
#ifndef NOODS_GX_DELTA_MAX_CRITICAL
#define NOODS_GX_DELTA_MAX_CRITICAL 64
#endif
#ifndef NOODS_GX_DELTA_MAX_ROWS
#define NOODS_GX_DELTA_MAX_ROWS 160
#endif
#if NOODS_TEX5_VERIFY
uint64_t g_tex5Verify[2] = {};
#endif
uint32_t g_gxAccuracyRoutes[5]; // gx frames, software feature frames, aa/edge, wire/shadow, horizontal
extern uint32_t g_gxDeltaStats[12]; // frames, corrected pixels, critical polygons, skipped, rows, full, cached, misses, budget, max polys, max critical, max rows
#if NOODS_V160_GX_DELTA
// Snapshot of the GX framebuffer and a frame-keyed sparse correction overlay.
// Repeated GX frames reuse the overlay without re-running the CPU rasterizer.
static uint32_t s_gxDeltaBackup[256 * 192 * 4];
static const int MAX_GX_DELTA_PIXELS = 16384;
static uint32_t s_gxDeltaIndex[MAX_GX_DELTA_PIXELS], s_gxDeltaColor[MAX_GX_DELTA_PIXELS];
static uint32_t s_gxDeltaKey = 0, s_gxDeltaCount = 0;
static int s_gxDeltaY0 = 0, s_gxDeltaY1 = 0;
static bool s_gxDeltaValid = false;

static inline uint32_t gxDeltaHashWords(uint32_t h, const void *data, size_t words) {
    const uint8_t *p = (const uint8_t*)data;
    for (size_t i = 0; i < words; ++i) {
        uint32_t w;
        memcpy(&w, p + i * 4, 4);
        h = (h ^ w) * 0x01000193u;
        h = (h << 13) | (h >> 19);
    }
    return h;
}
#endif

#if NOODS_V161_GX_OVERLAY
static bool s_gxOverlayPreparing = false;
static bool s_gxOverlayValid = false;
static uint32_t s_gxOverlayKey = 0;
static uint32_t s_gxOverlayCachedRuns = 0;

static inline uint32_t gxOverlayHashWords(uint32_t h, const void *data, size_t words) {
    const uint8_t *p = (const uint8_t*)data;
    for (size_t i = 0; i < words; ++i) {
        uint32_t w;
        memcpy(&w, p + i * 4, 4);
        h = (h ^ w) * 0x01000193u;
        h = (h << 13) | (h >> 19);
    }
    return h;
}
#endif


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
#if NOODS_V161_GX_OVERLAY
    s_gxOverlayValid = false;
    s_gxOverlayCachedRuns = 0;
    g_gxOverlayActive = 0;
    g_gxOverlayRunCount = 0;
#endif
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
    uint8_t mod6[64 * 64]; // v1.101: ((a + 1) * (b + 1) - 1) >> 6 for 6-bit a, b
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
        for (uint32_t a = 0; a < 64; a++)
            for (uint32_t b = 0; b < 64; b++)
                mod6[(a << 6) | b] = (uint8_t)(((a + 1) * (b + 1) - 1) >> 6);
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
    V180_PROFILE_SCOPE(2);
    V180_PROFILE_INC(14);
    // v1.108: with hardware rendering the whole frame is drawn when its first line is due.
    if (line == 0) {
        hwFrame = Settings::gx3D && !Settings::highRes3D && activeThreads == 0 && !Settings::threaded3D;
#if NOODS_GX_ACCURACY_FALLBACK
        if (hwFrame && core->gpu3D.polygonCountOut) {
            // GX lacks DS edge/AA and line/wireframe rules. A whole-frame
            // fallback preserves depth, ordering and blending between polygons;
            // deciding per polygon would not. Never inspect a ROM or test ID.
            bool effect = (disp3DCnt & (BIT(4) | BIT(5))) != 0;
            bool wireOrShadow = false, horizontal = false;
            for (int i = 0; i < core->gpu3D.polygonCountOut; ++i) {
                const _Polygon &p = core->gpu3D.polygonsOut[i];
                if (p.alpha == 0 || p.mode == 3) wireOrShadow = true;
                if (p.size >= 3) {
                    const Vertex *v = &core->gpu3D.verticesOut[p.vertices];
                    bool sameY = true;
                    for (int j = 1; j < p.size; ++j) sameY &= v[j].y == v[0].y;
                    horizontal |= sameY;
                }
            }
            if (effect || wireOrShadow || horizontal) {
                hwFrame = false;
                ++g_gxAccuracyRoutes[1];
                g_gxAccuracyRoutes[2] += effect;
                g_gxAccuracyRoutes[3] += wireOrShadow;
                g_gxAccuracyRoutes[4] += horizontal;
            }
        }
#endif
        if (hwFrame) ++g_gxAccuracyRoutes[0];
#if NOODS_V161_GX_OVERLAY
        // Build the GX primitive overlay before drawFrameGx() copies the EFB.
        // A cache hit only replays GX geometry; the CPU reference pass is not
        // a displayed-frame fallback.
        g_gxOverlayActive = 0;
        g_gxOverlayBudgetSkip = 0;
        if (hwFrame && !s_gxOverlayPreparing) prepareGxOverlay();
#endif
    }
    if (hwFrame) {
        if (line == 0) {
            resShift = false;
            drawFrameGx();
#if NOODS_V160_GX_DELTA
            // v1.160: keep the GX frame active. Only NDS-only rasterization
            // features use a bounded CPU reference delta pass.
            applyGxDelta();
#endif
        }
        return;
    }
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

        for (int i = 0; i < polyCount; i++)
            polyInv[i] = computePolyInvariant(&core->gpu3D.polygonsOut[i]);

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

#if NOODS_V160_GX_DELTA
void Gpu3DRenderer::applyGxDelta() {
    const int polyCount = core->gpu3D.polygonCountOut;
    if (polyCount <= 0) {
        ++g_gxDeltaStats[3];
        return;
    }
    if ((uint32_t)polyCount > g_gxDeltaStats[9])
        g_gxDeltaStats[9] = (uint32_t)polyCount;
    // Keep GX primary when a title submits more geometry than a bounded
    // correction can cover. A whole-frame CPU reference pass would violate
    // the release's no-software-fallback rule and harm real-ROM speed.
    if (polyCount > NOODS_GX_DELTA_MAX_POLYS) {
        ++g_gxDeltaStats[3];
        ++g_gxDeltaStats[8];
        return;
    }

    // GX handles the common cases. These are the NDS-only rasterization
    // features that a triangle/quad fan cannot reproduce exactly: AA/edge
    // marking/fog are global state, wireframes/shadows/toon are per polygon,
    // and horizontal line polygons obey special clipped-vertex rules.
    const bool globalEffect = (disp3DCnt & (BIT(4) | BIT(5) | BIT(7))) != 0;
    bool critical = globalEffect;
    int criticalPolys = 0, yMin = 192, yMax = 0;
    for (int i = 0; i < polyCount && i < 2048; ++i) {
        const _Polygon &p = core->gpu3D.polygonsOut[i];
        bool polyCritical = p.alpha == 0 || p.mode >= 2 || p.crossed ||
            (p.fog && (disp3DCnt & BIT(7))) ||
            (p.alpha < 0x3F && (p.textureFmt == 1 || p.textureFmt == 6));
        if (p.size >= 3) {
            const Vertex *v = &core->gpu3D.verticesOut[p.vertices];
            bool sameY = true;
            for (int j = 1; j < p.size; ++j)
                sameY &= v[j].y == v[0].y;
            polyCritical |= sameY;
        }
        if (polyCritical) {
            ++criticalPolys;
            critical = true;
        }
        // Global effects may change every polygon edge; otherwise only the
        // incompatible polygons need reference rasterization.
        if (globalEffect || polyCritical) {
            for (int j = 0; j < p.size; ++j) {
                int y = core->gpu3D.verticesOut[p.vertices + j].y;
                if (y < yMin) yMin = y;
                if (y > yMax) yMax = y;
            }
        }
    }
    if ((uint32_t)criticalPolys > g_gxDeltaStats[10])
        g_gxDeltaStats[10] = (uint32_t)criticalPolys;
    if (!critical || yMax < yMin) {
        ++g_gxDeltaStats[3];
        return;
    }
    if (yMin < 0) yMin = 0;
    if (yMin > 191) yMin = 191;
    if (yMax < 0) yMax = 0;
    if (yMax > 191) yMax = 191;
    if (yMax == yMin) ++yMax;
    const int y0 = (yMin > 1) ? yMin - 1 : 0;
    const int y1 = (yMax < 190) ? yMax + 2 : 192; // exclusive
    const int deltaRows = y1 - y0;
    if ((uint32_t)deltaRows > g_gxDeltaStats[11])
        g_gxDeltaStats[11] = (uint32_t)deltaRows;
    // Only a genuinely bounded overlay is worth the reference pass. Games
    // that enable a global effect across the whole screen keep the GX output
    // instead of paying for a full-frame software reconstruction.
    if (criticalPolys > NOODS_GX_DELTA_MAX_CRITICAL ||
        deltaRows > NOODS_GX_DELTA_MAX_ROWS ||
        (y0 == 0 && y1 == 192)) {
        ++g_gxDeltaStats[3];
        ++g_gxDeltaStats[8];
        return;
    }
    const int draw0 = (y0 > 0) ? y0 - 1 : 0;
    const int draw1 = (y1 < 192) ? y1 + 1 : 192; // exclusive
    ++g_gxDeltaStats[0];
    g_gxDeltaStats[2] += (uint32_t)criticalPolys;
    memcpy(s_gxDeltaBackup, framebuffer[0], sizeof(s_gxDeltaBackup));

    // Prepare the same immutable polygon list for the reference rasterizer.
    // Settings are switched only for preparation; GX stays the primary path.
    const int savedGx3D = Settings::gx3D;
    const int savedThreaded3D = Settings::threaded3D;
    const int savedHighRes3D = Settings::highRes3D;
    const bool savedResShift = resShift;
    const bool savedHwFrame = hwFrame;
    const uint8_t savedActiveThreads = activeThreads;

    // Key the correction by the immutable polygon list, raster state and the
    // GX pixels in the affected rectangle. Identical frames reuse the sparse
    // overlay and do no CPU rasterization at all.
    uint32_t state[8] = {
        (uint32_t)polyCount, (uint32_t)core->gpu3D.vertexCountOut,
        (uint32_t)disp3DCnt, clearColor,
        (uint32_t)clearDepth | ((uint32_t)fogOffset << 16), fogColor,
        (uint32_t)y0 | ((uint32_t)y1 << 16),
        (uint32_t)(savedGx3D & 0xFF) | ((uint32_t)(savedThreaded3D & 0xFF) << 8) |
        ((uint32_t)(savedHighRes3D & 0xFF) << 16) | ((uint32_t)savedResShift << 24)
    };
    uint32_t key = gxDeltaHashWords(0x811C9DC5u, state, 8);
    key = gxDeltaHashWords(key, edgeColor, sizeof(edgeColor) / 4);
    key = gxDeltaHashWords(key, fogTable, sizeof(fogTable) / 4);
    key = gxDeltaHashWords(key, toonTable, sizeof(toonTable) / 4);
    for (int i = 0; i < polyCount && i < 2048; ++i) {
        const _Polygon &p = core->gpu3D.polygonsOut[i];
        key = gxDeltaHashWords(key, &p, sizeof(_Polygon) / 4);
        if (p.size)
            key = gxDeltaHashWords(key, &core->gpu3D.verticesOut[p.vertices],
                (size_t)p.size * (sizeof(Vertex) / 4));
    }
    for (int y = y0; y < y1; ++y)
        key = gxDeltaHashWords(key, &s_gxDeltaBackup[y * 512], 256);

    if (s_gxDeltaValid && s_gxDeltaKey == key && s_gxDeltaY0 == y0 && s_gxDeltaY1 == y1) {
        for (uint32_t k = 0; k < s_gxDeltaCount; ++k)
            framebuffer[0][s_gxDeltaIndex[k]] = s_gxDeltaColor[k];
        g_gxDeltaStats[1] += s_gxDeltaCount;
        ++g_gxDeltaStats[6];
        return;
    }
    ++g_gxDeltaStats[7];
    g_gxDeltaStats[4] += (uint32_t)(draw1 - draw0);
    if (y0 == 0 && y1 == 192) ++g_gxDeltaStats[5];

    Settings::gx3D = 0;
    Settings::threaded3D = 0;
    Settings::highRes3D = 0;
    resShift = false;
    activeThreads = 0;
    hwFrame = false;

    // drawScanline(0) builds scanline lists, polygon invariants, texture
    // pointers and shared-edge flags exactly as the software renderer does.
    // Afterwards only the bounded rows are actually rasterized.
    drawScanline(0);
    for (int line = draw0; line < draw1; ++line)
        drawScanline1(line);
    for (int line = y0; line < y1; ++line)
        finishScanline(line);

    Settings::gx3D = savedGx3D;
    Settings::threaded3D = savedThreaded3D;
    Settings::highRes3D = savedHighRes3D;
    resShift = savedResShift;
    activeThreads = savedActiveThreads;
    hwFrame = savedHwFrame;

    // Keep GX pixels outside the affected rectangle and wherever GX already
    // matched the NDS reference. Store the disagreement as a sparse overlay
    // for subsequent identical GX frames.
    s_gxDeltaCount = 0;
    s_gxDeltaValid = true;
    s_gxDeltaKey = key;
    s_gxDeltaY0 = y0;
    s_gxDeltaY1 = y1;
    for (int y = 0; y < 192; ++y) {
        uint32_t *row = &framebuffer[0][y * 512];
        const uint32_t *bak = &s_gxDeltaBackup[y * 512];
        memcpy(row + 256, bak + 256, 256 * sizeof(uint32_t));
        if (y < y0 || y >= y1) {
            memcpy(row, bak, 256 * sizeof(uint32_t));
            continue;
        }
        for (int x = 0; x < 256; ++x) {
            const uint32_t idx = (uint32_t)(y * 512 + x);
            if (row[x] != bak[x]) {
                ++g_gxDeltaStats[1];
                if (s_gxDeltaCount < MAX_GX_DELTA_PIXELS) {
                    s_gxDeltaIndex[s_gxDeltaCount] = idx;
                    s_gxDeltaColor[s_gxDeltaCount] = row[x];
                    ++s_gxDeltaCount;
                } else {
                    s_gxDeltaValid = false;
                }
            } else {
                row[x] = bak[x];
            }
        }
    }
}
#endif

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
    V180_PROFILE_SCOPE(3);
    uint32_t color = BIT(26) | rgba5ToRgba6(((clearColor & 0x001F0000) >> 1) | (clearColor & 0x00007FFF));
    int32_t depth = (clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9);
    uint32_t attrib = ((clearColor & BIT(15)) >> 2) | ((clearColor & 0x3F000000) >> 18) | ((clearColor & 0x3F000000) >> 24) |
        (0x3F << 15) | (((clearColor & 0x001F0000) && ((clearColor & 0x001F0000) >> 16) < 31) << 12);

    int start = line * 256 * 2, end = start + (256 << resShift);
    const uint64_t clearStart = V180_PROFILE_TICK();
    for (int i = start; i < end; i++) {
        framebuffer[0][i] = color;
        depthBuffer[0][i] = depth;
        attribBuffer[0][i] = attrib;
    }
    V180_PROFILE_ADD(7, V180_PROFILE_TICK() - clearStart);

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
    V180_PROFILE_SCOPE(4);
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
        unsigned fogShift = (disp3DCnt & 0x0F00) >> 8;
#if NOODS_FOG_VERIFY
        int fogStep = 0x400 >> fogShift;
#endif

        for (int layer = 0; layer < ((disp3DCnt & BIT(4)) ? 2 : 1); layer++) {
            int start = line * 256 * 2, end = start + (256 << resShift);
            for (int i = start; i < end; i++) {
                if (attribBuffer[layer][i] & BIT(13)) {
                    uint8_t density = noodsFogDensityFast(depthBuffer[layer][i], fogOffset, fogShift, fogTable);
#if NOODS_FOG_VERIFY
                    int32_t offset = ((depthBuffer[layer][i] / 0x200) - fogOffset);
                    int n = (fogStep > 0) ? (offset / fogStep - 1) : ((offset > 0) ? 31 : 0);

                    uint8_t originalDensity;
                    if (n >= 31) {
                        originalDensity = fogTable[31];
                    }
                    else if (n < 0 || fogStep == 0) {
                        originalDensity = fogTable[0];
                    }
                    else {
                        int m = offset % fogStep;
                        originalDensity = ((m >= 0) ? ((fogTable[n + 1] * m + fogTable[n] * (fogStep - m)) / fogStep) : fogTable[0]);
                    }

                    if (originalDensity == 127)
                        originalDensity++;

                    ++g_fogVerify[0];
                    if (density != originalDensity) ++g_fogVerify[1];
#endif

#if NOODS_FOG_BLEND_VERIFY
                    const uint32_t before = framebuffer[layer][i];
#endif
                    framebuffer[layer][i] = noodsFogBlendFast(framebuffer[layer][i], fog, density, (disp3DCnt & BIT(6)) != 0);
#if NOODS_FOG_BLEND_VERIFY
                    const uint32_t computed = framebuffer[layer][i];
                    framebuffer[layer][i] = before;
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
                    ++g_fogBlendVerify[0];
                    if (density == 0) ++g_fogBlendVerify[1];
                    if (disp3DCnt & BIT(6)) ++g_fogBlendVerify[2];
                    if (framebuffer[layer][i] != computed) ++g_fogBlendVerify[3];
                    framebuffer[layer][i] = computed;
#endif
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
    // No v1 == v2 special case: the v1 <= v2 path below returns v1 exactly.
    if (v1 <= v2)
        return v1 + udivSpan((v2 - v1) * (x - x1), x2 - x1);
    else
        return v2 + udivSpan((v1 - v2) * (x2 - x), x2 - x1);
}
FORCE_INLINE uint32_t Gpu3DRenderer::interpolateLinRev(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2) {
    if (__builtin_expect(x <= x1, 0)) return v1;
    if (__builtin_expect(x >= x2, 0)) return v2;
    if (v1 <= v2)
        return v2 - udivSpan((v2 - v1) * (x2 - x), x2 - x1);
    else
        return v1 - udivSpan((v1 - v2) * (x - x1), x2 - x1);
}
FORCE_INLINE uint32_t Gpu3DRenderer::interpolateFactor(uint32_t factor, uint32_t shift, uint32_t v1, uint32_t v2) {
    // No v1 == v2 special case: a zero delta already yields exactly v1 below.
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
#if NOODS_TEX5_MIX
        // Opaque 6-bit channels: exact floor of weighted channel sum.
        // The baseline interpolateColor's ascending and descending cases
        // give this same value for denominators 2 and 8.
        auto mix5 = [&](uint32_t a, uint32_t b, uint32_t w) -> uint32_t {
            const uint32_t inv = 8u - w;
            const uint32_t r = (((a & 63u) * inv + (b & 63u) * w) >> 3);
            const uint32_t g = ((((a >> 6) & 63u) * inv + ((b >> 6) & 63u) * w) >> 3);
            const uint32_t bl = ((((a >> 12) & 63u) * inv + ((b >> 12) & 63u) * w) >> 3);
            const uint32_t v = (63u << 18) | (bl << 12) | (g << 6) | r;
#if NOODS_TEX5_VERIFY
            const uint32_t ref = interpolateColor(a, b, 0, w, 8);
            ++g_tex5Verify[0];
            if (v != ref) ++g_tex5Verify[1];
#endif
            return v;
        };
#endif
        switch ((palBase & 0xC000) >> 14) {
        case 0:
            if (index == 3) return 0;
            return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));

        case 1:
            switch (index) {
            case 2:
                c1 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 0));
                c2 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 2));
#if NOODS_TEX5_MIX
                return mix5(c1, c2, 4u);
#else
                return interpolateColor(c1, c2, 0, 1, 2);
#endif

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
#if NOODS_TEX5_MIX
                return mix5(c1, c2, 3u);
#else
                return interpolateColor(c1, c2, 0, 3, 8);
#endif

            case 3:
                c1 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 0));
                c2 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 2));
#if NOODS_TEX5_MIX
                return mix5(c1, c2, 5u);
#else
                return interpolateColor(c1, c2, 0, 5, 8);
#endif

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

// v1.85: line-independent part of drawPolygon. Depends only on the polygon and its
// vertices, which are fixed for the frame; computed once per polygon at line 0.
Gpu3DRenderer::PolyInvariant Gpu3DRenderer::computePolyInvariant(_Polygon *polygon) {
    PolyInvariant inv = {};
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
        if (area == 0) { inv.skip = true; return inv; }

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

    bool horizontalLine = polygon->size >= 3;
    for (int i = 1; i < polygon->size; i++)
        if (vertices[i]->y != vertices[0]->y) horizontalLine = false;
    inv.shallowXMajorEdge = shallowXMajorEdge;
    inv.duplicateAdjacent = duplicateAdjacent;
    inv.thinTriangle = thinTriangle;
    inv.horizontalLine = horizontalLine;
    inv.start = start;
    return inv;
}

// v1.101: interpolateColor(c1, c2, 0, a, 63) with the divide-by-63 as a compile-time
// constant (multiply + shift) instead of three calls into the generic span divider.
// floor(n / 63) is what udivSpan(n, 63) returns, so the result is identical.
static FORCE_INLINE uint32_t blendChan63(uint32_t v1, uint32_t v2, uint32_t a) {
    if (v1 == v2) return v1;
    if (__builtin_expect(a == 0, 0)) return v1;
    if (__builtin_expect(a >= 63, 0)) return v2;
    if (v1 <= v2) return v1 + ((v2 - v1) * a) / 63u;
    return v2 + ((v1 - v2) * (63u - a)) / 63u;
}
static FORCE_INLINE uint32_t blendColor63(uint32_t c1, uint32_t c2, uint32_t a) {
    const uint32_t r = blendChan63((c1 >> 0) & 0x3F, (c2 >> 0) & 0x3F, a);
    const uint32_t g = blendChan63((c1 >> 6) & 0x3F, (c2 >> 6) & 0x3F, a);
    const uint32_t b = blendChan63((c1 >> 12) & 0x3F, (c2 >> 12) & 0x3F, a);
    const uint32_t a1 = (c1 >> 18) & 0x3F, a2 = (c2 >> 18) & 0x3F;
    return ((a1 > a2 ? a1 : a2) << 18) | (b << 12) | (g << 6) | r;
}

// v1.101: per-pixel edge anti-aliasing coverage (verbatim from the generic loop in
// drawPolygon), kept out of line so the hot span loop stays small.
__attribute__((noinline)) uint32_t Gpu3DRenderer::spanEdgeAlpha(const SpanCtx &c, uint32_t x) {
    const uint32_t x1 = c.x1, x2 = c.x2, x3 = c.x3, x4 = c.x4;
    const uint32_t x1a = c.x1a, x2a = c.x2a, x3a = c.x3a, x4a = c.x4a;
    const bool aaRightXMajor = c.aaRightXMajor, swappedEdges = c.swappedEdges;
    const int aaRightX0 = c.aaRightX0, aaRightY0 = c.aaRightY0, aaRightX1 = c.aaRightX1, aaRightY1 = c.aaRightY1;
    const int line = c.line;
    const auto pAlpha = c.polygon->alpha;
    const auto pSize = c.polygon->size;
    uint8_t edgeAlpha = 0x3F;
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
                    pAlpha == 63 && pSize > 4) {
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
    return edgeAlpha;
}

// v1.105: the commonest span of all - constant W (so the interpolation factor is the plain
// floor(((x - x1) << 8) / spanLen)), W-buffered (so depth is one value for the whole span)
// and a directly readable 256-colour texture. Only the factor itself is stepped; texture
// coordinates and colours are derived from it for the pixels that pass the depth test,
// with the same mod-2^32 products interpolateFactor() forms, so every value is identical.
__attribute__((noinline)) void Gpu3DRenderer::drawSpanFlat4(const SpanCtx &c) {
    const _Polygon *const polygon = c.polygon;
    const uint32_t x1 = c.x1, x4 = c.x4;
    const uint32_t spanLen = x4 - x1;
    const uint32_t xEnd = (x4 < 256u) ? x4 : 256u;
    const uint32_t fq = 256u / spanLen, fr = 256u % spanLen;
    const int pWShift = polygon->wShift;
    int32_t depthV = (int32_t)c.w0;
    if (pWShift > 0) depthV <<= pWShift;
    else if (pWShift < 0) depthV >>= -pWShift;
    const int32_t depth = depthV;
    const uint32_t d3c = disp3DCnt;
    const bool aaOn = (d3c & BIT(4)) != 0;
    const bool blendOn = (d3c & BIT(3)) != 0;
    const uint32_t pAlpha = polygon->alpha;
    const uint32_t aSel = pAlpha ? pAlpha : 0x3F;
    const bool colConst = (c.r0 == c.r1 && c.g0 == c.g1 && c.b0 == c.b1);
    // channel(x) = base + ((k0 + k1 * factor) >> 8)
    struct Chan { uint32_t base, k0, k1; };
    auto mk = [](const uint32_t v1, const uint32_t v2) -> Chan {
        if (v1 <= v2) return Chan{ v1, 0u, v2 - v1 };
        return Chan{ v2, (v1 - v2) << 8, 0u - (v1 - v2) };
    };
    const Chan chS = mk(c.sB0, c.sB1), chT = mk(c.tB0, c.tB1);
    const Chan chR = mk(c.r0, c.r1), chG = mk(c.g0, c.g1), chB = mk(c.b0, c.b1);
    const int polygonIndex = c.polygonIndex;
    const uint8_t *const texBase = cachedTexSlot0[polygonIndex];
    const uint8_t *const palBase = cachedPalSlot[polygonIndex];
    const int tSizeS = polygon->sizeS, tSizeT = polygon->sizeT;
    const bool tRepS = polygon->repeatS, tRepT = polygon->repeatT;
    const bool tFlipS = polygon->flipS, tFlipT = polygon->flipT;
    const bool tTrans0 = polygon->transparent0;
    const bool sPow2 = tSizeS > 0 && (tSizeS & (tSizeS - 1)) == 0;
    const uint32_t sShift = sPow2 ? (uint32_t)__builtin_ctz((unsigned)tSizeS) : 0;
    const uint8_t *const m6 = s_3dLuts.mod6;
    const uint32_t opaque5 = s_3dLuts.alpha5To6[0x1F];
    const uint32_t *const rgb56 = s_3dLuts.rgb5To6;
    const uint32_t colR = c.r0 >> 3, colG = c.g0 >> 3, colB = c.b0 >> 3;
    const int rowBase = c.line * 256 * 2;
    uint32_t *const fb0 = &framebuffer[0][rowBase], *const fb1 = &framebuffer[1][rowBase];
    int32_t *const db0 = &depthBuffer[0][rowBase], *const db1 = &depthBuffer[1][rowBase];
    uint32_t *const ab0 = &attribBuffer[0][rowBase], *const ab1 = &attribBuffer[1][rowBase];
    const uint32_t x1e = c.x1e, x4e = c.x4e, x2 = c.x2, x3 = c.x3;
    const bool horizontal = c.horizontal;
    const uint32_t pId = polygon->id;
    const uint32_t pFog = polygon->fog;
    const bool pTransNewDepth = polygon->transNewDepth;
    // Three short passes instead of one long loop body, so that each keeps its working set
    // in registers. A pixel only ever reads and writes its own column, and the texel caches
    // are pure functions of their inputs, so splitting the work does not change any result.
    uint32_t list[256];  // x | layer << 8 | factor << 16
    uint32_t cols[256];
    uint32_t n = 0;

    // Pass 1: step the factor, depth test.
    {
        uint32_t f = 0, ferr = 0;
        if (aaOn) {
            for (uint32_t x = x1; x < xEnd; x++) {
                const uint32_t fx = f;
                f += fq; ferr += fr;
                if (ferr >= spanLen) { ferr -= spanLen; f++; }
                if (db0[x] > depth) list[n++] = x | (fx << 16);
                else if ((ab0[x] & BIT(14)) && (db1[x] > depth)) list[n++] = x | 0x100u | (fx << 16);
            }
        } else {
            for (uint32_t x = x1; x < xEnd; x++) {
                const uint32_t fx = f;
                f += fq; ferr += fr;
                if (ferr >= spanLen) { ferr -= spanLen; f++; }
                if (db0[x] > depth) list[n++] = x | (fx << 16);
            }
        }
    }
    if (n == 0) return;

    // Pass 2: texel, modulate, alpha / edge rejection. Survivors: list[i] = x | layer << 8.
    uint32_t m = 0;
    {
        const uint32_t sK0 = chS.k0, sK1 = chS.k1, sBase = chS.base - 0xFFFF;
        const uint32_t tK0 = chT.k0, tK1 = chT.k1, tBase = chT.base - 0xFFFF;
        const bool edgeKeepTrans = blendOn;
        const uint32_t vcolC = (colB << 12) | (colG << 6) | colR;
        auto shade = [&](const bool simpleWrap, const bool flatCol) __attribute__((always_inline)) {
            const uint32_t maskS = (uint32_t)tSizeS - 1u, maskT = (uint32_t)tSizeT - 1u;
            int lastS = 0xFFFF, lastT = 0xFFFF;
            uint32_t texel = 0, texA = 0, lastCol = 0xFFFFFFFFu, modC = 0;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t e = list[i];
                const uint32_t fx = e >> 16;
                const uint32_t x = e & 0xFFu;
                const int s = (int)(sBase + ((sK0 + sK1 * fx) >> 8)) >> 4;
                const int t = (int)(tBase + ((tK0 + tK1 * fx) >> 8)) >> 4;
                bool texChanged = false;
                if (s != lastS || t != lastT) {
                    lastS = s; lastT = t; texChanged = true;
                    uint32_t index;
                    if (simpleWrap) {
                        index = texBase[((((uint32_t)t & maskT)) << sShift) + ((uint32_t)s & maskS)];
                    } else {
                        int cs = s, ct = t;
                        if (tRepS) {
                            if (tFlipS && (cs & tSizeS)) cs = -1 - cs;
                            cs &= tSizeS - 1;
                        } else {
                            cs = cs < 0 ? 0 : (cs >= tSizeS ? tSizeS - 1 : cs);
                        }
                        if (tRepT) {
                            if (tFlipT && (ct & tSizeT)) ct = -1 - ct;
                            ct &= tSizeT - 1;
                        } else {
                            ct = ct < 0 ? 0 : (ct >= tSizeT ? tSizeT - 1 : ct);
                        }
                        index = texBase[(sPow2 ? (ct << sShift) : (ct * tSizeS)) + cs];
                    }
                    texel = (tTrans0 && index == 0) ? 0 :
                        (opaque5 | rgb56[U8TO16(palBase, index * 2) & 0x7FFF]);
                    texA = m6[((texel >> 12) & 0xFC0) | aSel];
                }
                if (texA == 0) continue; // modulated alpha 0: discarded
                if ((x < x1e || x >= x4e) && (texA == 0x3F || !edgeKeepTrans)) continue;
                uint32_t vcol = vcolC;
                if (!flatCol) {
                    const uint32_t vr = (chR.base + ((chR.k0 + chR.k1 * fx) >> 8)) >> 3;
                    const uint32_t vg = (chG.base + ((chG.k0 + chG.k1 * fx) >> 8)) >> 3;
                    const uint32_t vb = (chB.base + ((chB.k0 + chB.k1 * fx) >> 8)) >> 3;
                    vcol = (vb << 12) | (vg << 6) | vr;
                }
                if (texChanged || vcol != lastCol) {
                    lastCol = vcol;
                    const uint32_t r = m6[((texel << 6) & 0xFC0) | ((vcol >> 0) & 0x3F)];
                    const uint32_t g = m6[((texel >> 0) & 0xFC0) | ((vcol >> 6) & 0x3F)];
                    const uint32_t b = m6[((texel >> 6) & 0xFC0) | ((vcol >> 12) & 0x3F)];
                    modC = (texA << 18) | (b << 12) | (g << 6) | r;
                }
                list[m] = e & 0x1FFu;
                cols[m] = modC;
                m++;
            }
        };
        const bool simpleWrap = sPow2 && tSizeT > 0 && (tSizeT & (tSizeT - 1)) == 0 && tRepS && tRepT && !tFlipS && !tFlipT;
        if (simpleWrap) { if (colConst) shade(true, true); else shade(true, false); }
        else            { if (colConst) shade(false, true); else shade(false, false); }
    }

    // Pass 3: write.
    for (uint32_t i = 0; i < m; i++) {
        const uint32_t x = list[i] & 0xFFu;
        const unsigned layer = list[i] >> 8;
        const uint32_t color = cols[i];
        uint32_t *const fbp = layer ? fb1 : fb0;
        int32_t *const dbp = layer ? db1 : db0;
        uint32_t *const abp = layer ? ab1 : ab0;
        if ((color >> 18) == 0x3F) {
            uint32_t edgeAlpha = 0x3F;
            const uint32_t edge = (x <= x2 || x >= x3 || horizontal);
            if (aaOn && layer == 0 && edge) {
                fb1[x] = fb0[x];
                db1[x] = db0[x];
                ab1[x] = ab0[x];
                edgeAlpha = spanEdgeAlpha(c, x);
            }
            fbp[x] = BIT(26) | color;
            dbp[x] = depth;
            abp[x] = (abp[x] & 0x0FC0) | (edgeAlpha << 15) | (edge << 14) | (pFog << 13) | pId;
        }
        else if (!(abp[x] & BIT(12)) || ((abp[x] >> 6) & 0x3F) != pId) {
            fbp[x] = BIT(26) | ((blendOn && (fbp[x] & 0xFC0000)) ?
                blendColor63(fbp[x], color, color >> 18) : color);
            if (pTransNewDepth) dbp[x] = depth;
            abp[x] = (abp[x] & (0x1FC03F | (pFog << 13))) | BIT(12) | (pId << 6);

            if (aaOn && layer == 0 && (ab0[x] & BIT(14))) {
                fb1[x] = BIT(26) | ((blendOn && (fb1[x] & 0xFC0000)) ?
                    blendColor63(fb1[x], color, color >> 18) : color);
                if (pTransNewDepth) db1[x] = depth;
                ab1[x] = (ab1[x] & (0x1FC03F | (pFog << 13))) | BIT(12) | (pId << 6);
            }
        }
    }
}

// v1.101: specialised perspective span loop. Covers the overwhelmingly common case
// (texture mode 0, normal depth test, non-wireframe, native resolution, spans that
// are not on the exact-linear DDA path). All arithmetic is the same 32-bit wrap-around
// math as the generic loop in drawPolygon, only strength-reduced: the factor
// numerator/denominator are stepped instead of re-multiplied, loop-invariant
// branches are hoisted, format-4 texels are read inline and the modulate uses a
// 4 KB table. Living in its own function matters: inside the 50 KB drawPolygon every
// loop variable was spilled to the stack.
__attribute__((noinline)) void Gpu3DRenderer::drawSpanPersp(const SpanCtx &c) {
    _Polygon *const polygon = c.polygon;
    const int polygonIndex = c.polygonIndex;
#ifndef NOODS_NO_SPAN_FLAT4
    if (c.w0 == c.w1 && polygon->wBuffer && polygon->textureFmt == 4 && cachedUseFastTex[polygonIndex]) {
        drawSpanFlat4(c);
        return;
    }
#endif
    const uint32_t x1 = c.x1, x2 = c.x2, x3 = c.x3, x4 = c.x4, x1e = c.x1e, x4e = c.x4e;
    const bool horizontal = c.horizontal;
    const uint32_t w0 = c.w0, w1 = c.w1;
    const uint32_t cz0 = c.z0, cz1 = c.z1, cr0 = c.r0, cr1 = c.r1, cg0 = c.g0, cg1 = c.g1, cb0 = c.b0, cb1 = c.b1;
    const uint32_t sB0 = c.sB0, sB1 = c.sB1, tB0 = c.tB0, tB1 = c.tB1;
    const uint32_t pId = polygon->id;
    const int pWShift = polygon->wShift;
    const uint32_t pFog = polygon->fog;
    const uint32_t pAlpha = polygon->alpha;
    const bool pTransNewDepth = polygon->transNewDepth;
    const bool pWBuffer = polygon->wBuffer;
    const uint32_t pTextureFmt = polygon->textureFmt;
    const uint32_t d3c = disp3DCnt;
    const bool aaOn = (d3c & BIT(4)) != 0;
    const bool blendOn = (d3c & BIT(3)) != 0;
    const bool constW = (w0 == w1);
    const uint32_t spanLen = x4 - x1;
    const uint32_t xEnd = (x4 < 256u) ? x4 : 256u;
    // Constant W: factor = floor(((x - x1) << 8) / spanLen), stepped exactly.
    const uint32_t fq = constW ? (256u / spanLen) : 0, fr = constW ? (256u % spanLen) : 0;
    uint32_t ferr = 0;
    // Otherwise: factor = ((w0 * (x - x1)) << 8) / (w1 * (x4 - x) + w0 * (x - x1)), mod 2^32.
    uint32_t num = 0, den = w1 * spanLen;
    const uint32_t denStep = w0 - w1;
    int32_t depthConst = 0;
    if (pWBuffer && constW) {
        depthConst = (int32_t)w0;
        if (pWShift > 0) depthConst <<= pWShift;
        else if (pWShift < 0) depthConst >>= -pWShift;
    }
    const bool depthIsConst = pWBuffer && constW;
    const bool colConst = (cr0 == cr1 && cg0 == cg1 && cb0 == cb1);
    const uint32_t aBits = (uint32_t)(pAlpha ? pAlpha : 0x3F) << 18;
    const uint32_t colC = aBits | ((cb0 >> 3) << 12) | ((cg0 >> 3) << 6) | (cr0 >> 3);
    const bool fast4 = (pTextureFmt == 4) && cachedUseFastTex[polygonIndex];
    const uint8_t *const texBase = cachedTexSlot0[polygonIndex];
    const uint8_t *const palBase = cachedPalSlot[polygonIndex];
    const int tSizeS = polygon->sizeS, tSizeT = polygon->sizeT;
    const bool tRepS = polygon->repeatS, tRepT = polygon->repeatT;
    const bool tFlipS = polygon->flipS, tFlipT = polygon->flipT;
    const bool tTrans0 = polygon->transparent0;
    const int rowBase = c.line * 256 * 2;
    uint32_t *const fbL[2] = { &framebuffer[0][rowBase], &framebuffer[1][rowBase] };
    int32_t *const dbL[2] = { &depthBuffer[0][rowBase], &depthBuffer[1][rowBase] };
    uint32_t *const abL[2] = { &attribBuffer[0][rowBase], &attribBuffer[1][rowBase] };
    int32_t *const db0 = dbL[0], *const db1 = dbL[1];
    uint32_t *const ab0 = abL[0];
    int lastS = 0xFFFF, lastT = 0xFFFF;
    uint32_t texel = 0;
    uint32_t texA = 0; // texel alpha modulated by the (constant) polygon alpha
    uint32_t modC = 0; // colConst: texel modulated by the constant vertex colour
    const uint8_t *const m6 = s_3dLuts.mod6;
    const uint32_t aSel = (aBits >> 18) & 0x3F;
    const uint32_t opaque5 = s_3dLuts.alpha5To6[0x1F];
    const uint32_t *const rgb56 = s_3dLuts.rgb5To6;
    // Texture rows are powers of two (8 << n); index with a shift when that holds.
    const bool sPow2 = tSizeS > 0 && (tSizeS & (tSizeS - 1)) == 0;
    const uint32_t sShift = sPow2 ? (uint32_t)__builtin_ctz((unsigned)tSizeS) : 0;

    // Texture + blend + write for one pixel that passed the depth test. `color` is the
    // interpolated vertex colour with the polygon alpha in bits 18-23.
    auto emit = [&](const uint32_t x, const int32_t depth, const unsigned layer, uint32_t color, const int s, const int t) __attribute__((always_inline)) {
        if (pTextureFmt != 0) {
            const bool texChanged = (s != lastS || t != lastT);
            if (texChanged) {
                lastS = s; lastT = t;
                if (fast4) {
                    int cs = s, ct = t;
                    if (tRepS) {
                        if (tFlipS && (cs & tSizeS)) cs = -1 - cs;
                        cs &= tSizeS - 1;
                    } else {
                        cs = cs < 0 ? 0 : (cs >= tSizeS ? tSizeS - 1 : cs);
                    }
                    if (tRepT) {
                        if (tFlipT && (ct & tSizeT)) ct = -1 - ct;
                        ct &= tSizeT - 1;
                    } else {
                        ct = ct < 0 ? 0 : (ct >= tSizeT ? tSizeT - 1 : ct);
                    }
                    const uint8_t index = texBase[(sPow2 ? (ct << sShift) : (ct * tSizeS)) + cs];
                    texel = (tTrans0 && index == 0) ? 0 :
                        (opaque5 | rgb56[U8TO16(palBase, index * 2) & 0x7FFF]);
                } else {
                    texel = readTextureCached(polygonIndex, polygon, s, t);
                }
                texA = m6[((texel >> 12) & 0xFC0) | aSel];
            }
            // Modulated alpha 0: the pixel is discarded (same test as below, taken early).
            if (texA == 0) return;
            if (colConst && !texChanged) {
                color = modC;
            } else {
                uint32_t r = m6[((texel << 6) & 0xFC0) | ((color >> 0) & 0x3F)];
                uint32_t g = m6[((texel >> 0) & 0xFC0) | ((color >> 6) & 0x3F)];
                uint32_t b = m6[((texel >> 6) & 0xFC0) | ((color >> 12) & 0x3F)];
                color = (texA << 18) | (b << 12) | (g << 6) | r;
                modC = color;
            }
        }

        if (!(color & 0xFC0000) || ((x < x1e || x >= x4e) && ((color >> 18) == 0x3F || !blendOn)))
            return;

        uint32_t *const fbp = fbL[layer];
        int32_t *const dbp = dbL[layer];
        uint32_t *const abp = abL[layer];
        if ((color >> 18) == 0x3F) {
            uint32_t edgeAlpha = 0x3F;
            const uint32_t edge = (x <= x2 || x >= x3 || horizontal);
            if (aaOn && layer == 0 && edge) {
                fbL[1][x] = fbL[0][x];
                dbL[1][x] = dbL[0][x];
                abL[1][x] = abL[0][x];
                edgeAlpha = spanEdgeAlpha(c, x);
            }
            fbp[x] = BIT(26) | color;
            dbp[x] = depth;
            abp[x] = (abp[x] & 0x0FC0) | (edgeAlpha << 15) | (edge << 14) | (pFog << 13) | pId;
        }
        else if (!(abp[x] & BIT(12)) || ((abp[x] >> 6) & 0x3F) != pId) {
            fbp[x] = BIT(26) | ((blendOn && (fbp[x] & 0xFC0000)) ?
                blendColor63(fbp[x], color, color >> 18) : color);
            if (pTransNewDepth) dbp[x] = depth;
            abp[x] = (abp[x] & (0x1FC03F | (pFog << 13))) | BIT(12) | (pId << 6);

            if (aaOn && layer == 0 && (abL[0][x] & BIT(14))) {
                fbL[1][x] = BIT(26) | ((blendOn && (fbL[1][x] & 0xFC0000)) ?
                    blendColor63(fbL[1][x], color, color >> 18) : color);
                if (pTransNewDepth) dbL[1][x] = depth;
                abL[1][x] = (abL[1][x] & (0x1FC03F | (pFog << 13))) | BIT(12) | (pId << 6);
            }
        }
    };

    if (constW) {
        // Constant W: factor(x) = floor(((x - x1) << 8) / spanLen) advances by fq or fq + 1 per
        // pixel, so each channel's product delta * factor (or delta * (256 - factor) for a
        // descending channel) is carried forward by additions. interpolateFactor() computes
        // base + (product >> 8) with the same mod-2^32 product, so values are identical.
        struct Chan { uint32_t base, acc, step, cstep; };
        auto initChan = [&](Chan &ch, const uint32_t v1, const uint32_t v2) {
            if (v1 <= v2) {
                const uint32_t d = v2 - v1;
                ch.base = v1; ch.acc = 0; ch.step = d * fq; ch.cstep = d;
            } else {
                const uint32_t d = v1 - v2;
                ch.base = v2; ch.acc = d << 8; ch.step = 0u - d * fq; ch.cstep = 0u - d;
            }
        };
        Chan chR, chG, chB, chS, chT;
        initChan(chR, cr0, cr1); initChan(chG, cg0, cg1); initChan(chB, cb0, cb1);
        initChan(chS, sB0, sB1); initChan(chT, tB0, tB1);
        uint32_t accR = chR.acc, accG = chG.acc, accB = chB.acc, accS = chS.acc, accT = chT.acc;
        const uint32_t stR = chR.step, stG = chG.step, stB = chB.step, stS = chS.step, stT = chT.step;
        const uint32_t csR = chR.cstep, csG = chG.cstep, csB = chB.cstep, csS = chS.cstep, csT = chT.cstep;
        const uint32_t bsR = chR.base, bsG = chG.base, bsB = chB.base, bsS = chS.base, bsT = chT.base;
        const bool hasTex = pTextureFmt != 0;
        for (uint32_t x = x1; x < xEnd; x++) {
            const int32_t depth = depthIsConst ? depthConst : (int32_t)interpolateLinear(cz0, cz1, x1, x, x4);
            unsigned layer = 0;
            bool pass = true;
            if (!(db0[x] > depth)) {
                if (aaOn && (ab0[x] & BIT(14)) && (db1[x] > depth)) layer = 1;
                else pass = false;
            }
            if (pass) {
                uint32_t color;
                if (colConst) {
                    color = colC;
                } else {
                    const uint32_t rv = (bsR + (accR >> 8)) >> 3;
                    const uint32_t gv = (bsG + (accG >> 8)) >> 3;
                    const uint32_t bv = (bsB + (accB >> 8)) >> 3;
                    color = aBits | (bv << 12) | (gv << 6) | rv;
                }
                int s = 0, t = 0;
                if (hasTex) {
                    s = (int)(bsS + (accS >> 8) - 0xFFFF) >> 4;
                    t = (int)(bsT + (accT >> 8) - 0xFFFF) >> 4;
                }
                emit(x, depth, layer, color, s, t);
            }
            ferr += fr;
            if (ferr >= spanLen) {
                ferr -= spanLen;
                if (!colConst) { accR += stR + csR; accG += stG + csG; accB += stB + csB; }
                if (hasTex) { accS += stS + csS; accT += stT + csT; }
            } else {
                if (!colConst) { accR += stR; accG += stG; accB += stB; }
                if (hasTex) { accS += stS; accT += stT; }
            }
        }
        return;
    }

    for (uint32_t x = x1; x < xEnd; x++) {
        // factor = ((w0 * (x - x1)) << 8) / (w1 * (x4 - x) + w0 * (x - x1)), mod 2^32
        const uint32_t factor = (x == x1) ? 0 : ((num << 8) / den);
        num += w0; den += denStep;

        int32_t depth;
        if (pWBuffer) {
            depth = interpolateFactor(factor, 8, w0, w1);
            if (pWShift > 0) depth <<= pWShift;
            else if (pWShift < 0) depth >>= -pWShift;
        } else {
            depth = interpolateLinear(cz0, cz1, x1, x, x4);
        }

        unsigned layer = 0;
        if (!(db0[x] > depth)) {
            if (!(aaOn && (ab0[x] & BIT(14)) && (db1[x] > depth)))
                continue;
            layer = 1;
        }

        uint32_t color;
        if (colConst) {
            color = colC;
        } else {
            uint32_t rv = interpolateFactor(factor, 8, cr0, cr1) >> 3;
            uint32_t gv = interpolateFactor(factor, 8, cg0, cg1) >> 3;
            uint32_t bv = interpolateFactor(factor, 8, cb0, cb1) >> 3;
            color = aBits | (bv << 12) | (gv << 6) | rv;
        }
        int s = 0, t = 0;
        if (pTextureFmt != 0) {
            s = (int)(interpolateFactor(factor, 8, sB0, sB1) - 0xFFFF) >> 4;
            t = (int)(interpolateFactor(factor, 8, tB0, tB1) - 0xFFFF) >> 4;
        }
        emit(x, depth, layer, color, s, t);
    }
}

void Gpu3DRenderer::drawPolygon(int line, int polygonIndex) {
    V180_PROFILE_SCOPE(5);
    const uint64_t edgeStart = V180_PROFILE_TICK();
    _Polygon *polygon = &core->gpu3D.polygonsOut[polygonIndex];

    Vertex *vertices[10];
    for (int i = 0; i < polygon->size; i++)
        vertices[i] = &core->gpu3D.verticesOut[polygon->vertices + i];

    if (polygon->crossed)
        SWAP(vertices[2], vertices[3]);

    const PolyInvariant &inv = polyInv[polygonIndex];
    if (inv.skip) return;
    const bool shallowXMajorEdge = inv.shallowXMajorEdge;
    const bool duplicateAdjacent = inv.duplicateAdjacent;
    const bool thinTriangle = inv.thinTriangle;
    const bool horizontalLine = inv.horizontalLine;
    const int start = inv.start;
    // v1.102: the vertex index always stays in [0, size), so stepping it by one needs a
    // compare instead of a hardware modulo (several 40-cycle divides per polygon row).
    const int pSizeN = polygon->size;
    auto nextIdx = [pSizeN](int i) { return (i + 1 >= pSizeN) ? (i + 1 - pSizeN) : (i + 1); };
    auto prevIdx = [pSizeN](int i) { return (i - 1 < 0) ? (i - 1 + pSizeN) : (i - 1); };
    int v[4] = {
        start, nextIdx(start),
        start, prevIdx(start)
    };

    while (vertices[v[1]]->y <= line) {
        v[0] = v[1];
        v[1] = nextIdx(v[1]);
        if (v[0] == start) break;
    }

    while (vertices[v[3]]->y <= line) {
        v[2] = v[3];
        v[3] = prevIdx(v[3]);
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
                // v1.102: the 64-bit divide (__udivdi3) is only needed when the numerator
                // does not fit in 32 bits after the shift; otherwise the same quotient
                // comes from one 32-bit divide.
                const uint32_t fNum = (ws[i2] >> 1) * (xe[i] - xe1[i]);
                const uint32_t fDen = (ws[i2 + 1] >> 1) * (xe2[i] - xe[i]) + wa * (xe[i] - xe1[i]);
                if (__builtin_expect(fNum < (1u << 23) && fDen != 0, 1))
                    factor = (fNum << 9) / fDen;
                else
                    factor = (uint64_t(fNum) << 9) / fDen;
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

#if NOODS_GX_SPAN_COVERAGE
    if (gxSpanCapture) {
        // Blended translucent polygons keep all edge samples; other
        // non-AA polygons use the reference edge-mask intersection.
        // The caller restricted the polygon to flat color/depth.
        const bool blendedTranslucent = polygon->alpha < 63 && (disp3DCnt & BIT(3));
        gxSpanBegin = blendedTranslucent ? x1 : (x1 > x1e ? x1 : x1e);
        gxSpanEnd = blendedTranslucent ? x4 : (x4 < x4e ? x4 : x4e);
        if (gxSpanEnd > 256) gxSpanEnd = 256;
        if (gxSpanBegin >= gxSpanEnd) gxSpanBegin = gxSpanEnd = 0;
        return;
    }
#endif
    // v1.80: hoist the loop-invariant half of the outline-edge test.
    const bool topQuadOutlineRow = polygon->size == 4 && polygon->alpha == 63 &&
        (disp3DCnt & BIT(5)) && shallowXMajorEdge && line == polygonTop[polygonIndex] + 1;

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
    V180_PROFILE_ADD(6, V180_PROFILE_TICK() - edgeStart);
    V180_PROFILE_INC(8);
    if (isLinear) V180_PROFILE_INC(9);
    else V180_PROFILE_INC(11);
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
            bool topQuadOutlineEdge = topQuadOutlineRow && x >= (int)x3;
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
    // Field loads hoisted out of the per-pixel span loop: the compiler cannot
    // prove the framebuffer/depth/attrib stores don't alias the polygon object.
    const auto pId = polygon->id;
    const auto pWShift = polygon->wShift;
    const auto pMode = polygon->mode;
    const auto pFog = polygon->fog;
    const auto pAlpha = polygon->alpha;
    const auto pTransNewDepth = polygon->transNewDepth;
    const auto pWBuffer = polygon->wBuffer;
    const auto pTextureFmt = polygon->textureFmt;
    const auto pSize = polygon->size;
    const auto pDepthTestEqual = polygon->depthTestEqual;
    const uint32_t seB0 = se[0] + 0xFFFF, seB1 = se[1] + 0xFFFF;
    const uint32_t teB0 = te[0] + 0xFFFF, teB1 = te[1] + 0xFFFF;
    // v1.101: the common perspective span case runs in a separate, register-friendly routine.
    if (!resShift && pAlpha != 0 && pMode == 0 && !pDepthTestEqual && !topQuadOutlineRow &&
        !(we[0] == we[1] && !(we[0] & 0x7F)) && x4 > x1) {
        SpanCtx c;
        c.polygon = polygon; c.polygonIndex = polygonIndex; c.line = line;
        c.x1 = x1; c.x2 = x2; c.x3 = x3; c.x4 = x4; c.x1e = x1e; c.x4e = x4e;
        c.x1a = x1a; c.x2a = x2a; c.x3a = x3a; c.x4a = x4a;
        c.w0 = we[0]; c.w1 = we[1]; c.z0 = ze[0]; c.z1 = ze[1];
        c.r0 = re[0]; c.r1 = re[1]; c.g0 = ge[0]; c.g1 = ge[1]; c.b0 = be[0]; c.b1 = be[1];
        c.sB0 = seB0; c.sB1 = seB1; c.tB0 = teB0; c.tB1 = teB1;
        c.aaRightX0 = aaRightX0; c.aaRightY0 = aaRightY0; c.aaRightX1 = aaRightX1; c.aaRightY1 = aaRightY1;
        c.horizontal = horizontal; c.aaRightXMajor = aaRightXMajor; c.swappedEdges = swappedEdges;
        drawSpanPersp(c);
        return;
    }
    for (uint32_t x = x1; x < x4; x++) {
        if (!horizontal && pAlpha == 0 && x == x2 + 1 && x3 > x2)
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
        // Constant W across the span: 256*W*a / (W*(x4-x1)) == (a << 8) / (x4-x1)
        // exactly, so the divisor is just the span length and the magic-multiply
        // divider applies (udivSpan) instead of a hardware divide per pixel.
        else if (we[0] == we[1])
            factor = udivSpan((x - x1) << 8, x4 - x1);
        else if (resShift && ((x - x1) >> 8))
            factor = (uint64_t(we[0] * (x - x1)) << 8) / (we[1] * (x4 - x) + we[0] * (x - x1));
        else
            factor = ((we[0] * (x - x1)) << 8) / (we[1] * (x4 - x) + we[0] * (x - x1));

        int32_t depth;
        if (pWBuffer) {
            depth = (factor == -1) ? interpolateLinear(we[0], we[1], x1, x, x4) :
                interpolateFactor(factor, 8, we[0], we[1]);
            if (pWShift > 0)
                depth <<= pWShift;
            else if (pWShift < 0)
                depth >>= -pWShift;
        }
        else {
            depth = interpolateLinear(ze[0], ze[1], x1, x, x4);
        }

        bool depthPass[2];
        if (pDepthTestEqual) {
            uint32_t margin = (pWBuffer ? 0xFF : 0x200);
            depthPass[0] = (depthBuffer[0][i] >= depth - margin && depthBuffer[0][i] <= depth + margin);
            depthPass[1] = (disp3DCnt & BIT(4)) && (attribBuffer[0][i] & BIT(14)) &&
                (depthBuffer[1][i] >= depth - margin && depthBuffer[1][i] <= depth + margin);
        }
        else {
            bool topQuadOutlineEdge = topQuadOutlineRow && x >= (int)x3;
            bool sameIdOutlineEdge = topQuadOutlineEdge && (attribBuffer[0][i] & BIT(14)) &&
                ((attribBuffer[0][i] & 0x3F) == pId);
            uint32_t pixelDepth24 = (uint32_t)depth & 0x00FFFFFF;
            bool wrappedDepthPass = (((uint32_t)depthBuffer[0][i] & 0x00FFFFFF) > pixelDepth24);
            depthPass[0] = (depthBuffer[0][i] > depth) || (sameIdOutlineEdge && wrappedDepthPass);
            depthPass[1] = (disp3DCnt & BIT(4)) && (attribBuffer[0][i] & BIT(14)) &&
                (depthBuffer[1][i] > depth);
        }

        if (pMode == 3) {
            if (pId == 0) {
                if (!depthPass[0]) stencilBuffer[i] |= BIT(0);
                if (!depthPass[1]) stencilBuffer[i] |= BIT(1);
                continue;
            }
            else if (!depthPass[0] || !(stencilBuffer[i] & BIT(0)) || (attribBuffer[0][i] & 0x3F) == pId) {
                if (!depthPass[1] || !(stencilBuffer[i] & BIT(1)) || (attribBuffer[1][i] & 0x3F) == pId)
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
        uint32_t color = ((pAlpha ? pAlpha : 0x3F) << 18) | (bv << 12) | (gv << 6) | rv;

        if (pTextureFmt != 0) {
            int s, t;
            if (factor == -1) {
                s = (int)(interpolateLinear(seB0, seB1, x1, x, x4) - 0xFFFF) >> 4;
                t = (int)(interpolateLinear(teB0, teB1, x1, x, x4) - 0xFFFF) >> 4;
            }
            else {
                s = (int)(interpolateFactor(factor, 8, seB0, seB1) - 0xFFFF) >> 4;
                t = (int)(interpolateFactor(factor, 8, teB0, teB1) - 0xFFFF) >> 4;
            }

            if (s != lastS || t != lastT) {
                lastS = s; lastT = t;
                texel = readTextureCached(polygonIndex, polygon, s, t);
            }

            switch (pMode) {
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
        else if (pMode == 2) {
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
                    pAlpha == 63 && pSize > 4) {
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
                (edge << 14) | (pFog << 13) | pId;
        }
        else if (!(attribBuffer[layer][i] & BIT(12)) || ((attribBuffer[layer][i] >> 6) & 0x3F) != pId) {
            framebuffer[layer][i] = BIT(26) | (((disp3DCnt & BIT(3)) && (framebuffer[layer][i] & 0xFC0000)) ?
                interpolateColor(framebuffer[layer][i], color, 0, color >> 18, 63) : color);
            if (pTransNewDepth) depthBuffer[layer][i] = depth;
            attribBuffer[layer][i] = (attribBuffer[layer][i] & (0x1FC03F | (pFog << 13))) | BIT(12) | (pId << 6);

            if ((disp3DCnt & BIT(4)) && layer == 0 && (attribBuffer[0][i] & BIT(14))) {
                framebuffer[1][i] = BIT(26) | (((disp3DCnt & BIT(3)) && (framebuffer[1][i] & 0xFC0000)) ?
                    interpolateColor(framebuffer[1][i], color, 0, color >> 18, 63) : color);
                if (pTransNewDepth) depthBuffer[1][i] = depth;
                attribBuffer[1][i] = (attribBuffer[1][i] & (0x1FC03F | (pFog << 13))) | BIT(12) | (pId << 6);
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

#if NOODS_V161_GX_OVERLAY
// Build a final-image primitive list for NDS-only rasterization features.
// The software rasterizer writes only into this object's scratch line buffers;
// the displayed 3D framebuffer is subsequently written by GX quads only.
void Gpu3DRenderer::prepareGxOverlay() {
    if (s_gxOverlayPreparing) return;
    g_gxOverlayActive = 0;
    g_gxOverlayBudgetSkip = 0;

    const int polyCount = core->gpu3D.polygonCountOut;
    if (polyCount <= 0 || polyCount > NOODS_GX_OVERLAY_MAX_POLYS ||
        polyCount > NOODS_GX_DELTA_MAX_POLYS) {
        g_gxOverlayBudgetSkip = 1;
        ++g_gxOverlayStats[5];
        return;
    }

    const bool globalEffect = (disp3DCnt & (BIT(4) | BIT(5) | BIT(7))) != 0;
    bool critical = globalEffect;
    bool unsupported = false;
    int criticalPolys = 0;
    for (int i = 0; i < polyCount; ++i) {
        const _Polygon &p = core->gpu3D.polygonsOut[i];
        // Dynamic texture state is not part of the immutable geometry key.
        // Keep GX's normal path for those frames instead of a stale overlay.
        if (p.textureFmt != 0 || p.mode == 3 || p.size > 10) {
            unsupported = true;
            break;
        }
        bool polyCritical = p.alpha == 0 || p.mode >= 2 || p.crossed ||
            (p.fog && (disp3DCnt & BIT(7))) ||
            (p.alpha < 0x3F && (p.textureFmt == 1 || p.textureFmt == 6));
        if (p.size >= 3) {
            const Vertex *v = &core->gpu3D.verticesOut[p.vertices];
            bool sameY = true;
            for (int j = 1; j < p.size; ++j)
                sameY &= v[j].y == v[0].y;
            polyCritical |= sameY;
        }
        if (polyCritical) {
            ++criticalPolys;
            critical = true;
        }
    }
    if (!critical) {
        ++g_gxOverlayStats[7];
        return;
    }
    if (unsupported || criticalPolys > NOODS_GX_DELTA_MAX_CRITICAL) {
        g_gxOverlayBudgetSkip = 1;
        ++g_gxOverlayStats[5];
        return;
    }

    const int savedGx3D = Settings::gx3D;
    const int savedThreaded3D = Settings::threaded3D;
    const int savedHighRes3D = Settings::highRes3D;
    const bool savedResShift = resShift;
    const bool savedHwFrame = hwFrame;
    const uint8_t savedActiveThreads = activeThreads;

    uint32_t state[8] = {
        (uint32_t)polyCount, (uint32_t)core->gpu3D.vertexCountOut,
        (uint32_t)disp3DCnt, clearColor,
        (uint32_t)clearDepth | ((uint32_t)fogOffset << 16), fogColor,
        (uint32_t)(savedGx3D & 0xFF) | ((uint32_t)(savedThreaded3D & 0xFF) << 8) |
        ((uint32_t)(savedHighRes3D & 0xFF) << 16) | ((uint32_t)savedResShift << 24),
        (uint32_t)savedActiveThreads | ((uint32_t)savedHwFrame << 8)
    };
    uint32_t key = gxOverlayHashWords(0x811C9DC5u, state, 8);
    key = gxOverlayHashWords(key, edgeColor, sizeof(edgeColor) / 4);
    key = gxOverlayHashWords(key, fogTable, sizeof(fogTable) / 4);
    key = gxOverlayHashWords(key, toonTable, sizeof(toonTable) / 4);
    for (int i = 0; i < polyCount; ++i) {
        const _Polygon &p = core->gpu3D.polygonsOut[i];
        key = gxOverlayHashWords(key, &p, sizeof(_Polygon) / 4);
        if (p.size)
            key = gxOverlayHashWords(key, &core->gpu3D.verticesOut[p.vertices],
                (size_t)p.size * (sizeof(Vertex) / 4));
    }

    if (s_gxOverlayValid && s_gxOverlayKey == key && s_gxOverlayCachedRuns > 0) {
        g_gxOverlayActive = 1;
        g_gxOverlayRunCount = s_gxOverlayCachedRuns;
        ++g_gxOverlayStats[0];
        ++g_gxOverlayStats[3];
        return;
    }

    Settings::gx3D = 0;
    Settings::threaded3D = 0;
    Settings::highRes3D = 0;
    resShift = false;
    activeThreads = 0;
    hwFrame = false;
    gxSpanCapture = false;
    s_gxOverlayPreparing = true;

    // Recursive call performs the normal scanline setup and draws row 0 in
    // software scratch. The guard above prevents another overlay preparation.
    drawScanline(0);
    for (int line = 1; line < 192; ++line) {
        drawScanline1(line);
        finishScanline(line - 1);
        if (line == 191) finishScanline(191);
    }

    uint32_t count = 0, pixels = 0;
    bool overflow = false;
    for (int y = 0; y < 192 && !overflow; ++y) {
        uint32_t color = framebuffer[0][y * 512];
        int runStart = 0;
        for (int x = 1; x <= 256; ++x) {
            const uint32_t next = (x < 256) ? framebuffer[0][y * 512 + x] : 0xFFFFFFFFu;
            if (next != color) {
                if (count >= MAX_GX_OVERLAY_RUNS) {
                    overflow = true;
                    break;
                }
                GxOverlayRun &r = g_gxOverlayRuns[count++];
                r.y = (uint16_t)y;
                r.x0 = (uint16_t)runStart;
                r.x1 = (uint16_t)x; // exclusive
                r.reserved = 0;
                r.color = color;
                pixels += (uint32_t)(x - runStart);
                runStart = x;
                color = next;
            }
        }
    }

    s_gxOverlayPreparing = false;
    Settings::gx3D = savedGx3D;
    Settings::threaded3D = savedThreaded3D;
    Settings::highRes3D = savedHighRes3D;
    resShift = savedResShift;
    activeThreads = savedActiveThreads;
    hwFrame = savedHwFrame;
    gxSpanCapture = false;

    if (overflow || count == 0) {
        s_gxOverlayValid = false;
        g_gxOverlayActive = 0;
        g_gxOverlayBudgetSkip = 1;
        g_gxOverlayRunCount = 0;
        ++g_gxOverlayStats[5];
        return;
    }

    s_gxOverlayValid = true;
    s_gxOverlayKey = key;
    s_gxOverlayCachedRuns = count;
    g_gxOverlayActive = 1;
    g_gxOverlayRunCount = count;
    ++g_gxOverlayStats[0];
    ++g_gxOverlayStats[4];
    g_gxOverlayStats[1] += count;
    g_gxOverlayStats[2] += pixels;
    if (count > g_gxOverlayStats[6]) g_gxOverlayStats[6] = count;
}
#endif
