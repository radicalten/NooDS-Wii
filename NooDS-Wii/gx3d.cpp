/*
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

// v1.108: DS 3D drawn by the Wii's graphics processor (GX).
//
// The geometry engine is unchanged: it still produces a list of clipped polygons in screen
// coordinates. Instead of rasterising them scanline by scanline in software, the whole list
// is sent to GX once per 3D frame into a 256x192 corner of the embedded frame buffer, the
// result is copied back to main memory and unpacked into the line buffers the 2D compositor
// reads, so everything after the 3D layer works as before.
//
// Known differences from the software rasteriser (see README): approximate fog, no edge marking,
// anti-aliasing, shadow polygons or wireframe; toon/highlight shading is per vertex; vertex
// colours are interpolated without perspective correction; translucent pixels over an empty
// background are slightly darker.

#include <gccore.h>
#include <malloc.h>
/* GX texture cache index. The authoritative records and equality checks remain
 * in the renderer; the table only narrows the search. No deletion except clear.
 * 512 slots for at most 256 records guarantees an empty terminator. */
#include <stdint.h>
#include <string.h>

struct GxTextureIndex {
    uint16_t slots[512] = {}; // record index + 1; zero means empty

    void clear() { memset(slots, 0, sizeof(slots)); }

    template<typename P> static uint32_t hash(const P &p) {
        uint32_t h = (p.textureAddr >> 3) * 0x9e3779b1u;
        h ^= (p.paletteAddr >> 3) * 0x85ebca6bu;
        h ^= (uint32_t)p.sizeS * 0xc2b2ae35u;
        h ^= (uint32_t)p.sizeT * 0x27d4eb2du;
        h ^= (uint32_t)p.textureFmt * 0x165667b1u;
        h ^= (uint32_t)p.transparent0 * 0xd3a2646cu;
        h ^= h >> 16;
        return h & 511u;
    }

    template<typename P, typename E> static bool same(const P &p, const E &e) {
        return e.addr == p.textureAddr && e.pal == p.paletteAddr &&
            e.w == p.sizeS && e.h == p.sizeT && e.fmt == p.textureFmt &&
            e.trans0 == p.transparent0;
    }

    // Returns the record index or -1, and the existing/first empty slot.
    template<typename P, typename E> int find(const P &p, const E *entries, uint32_t &slot) const {
        slot = hash(p);
        while (slots[slot]) {
            int i = slots[slot] - 1;
            if (same(p, entries[i])) return i;
            slot = (slot + 1) & 511u;
        }
        return -1;
    }
    void insert(uint32_t slot, int index) { slots[slot] = (uint16_t)(index + 1); }
};


#ifndef NOODS_GX_TEX_INDEX
#define NOODS_GX_TEX_INDEX 1
#endif
#include <string.h>

#include "core.h"

#include "settings.h"

#ifndef NOODS_GX_EDGE_EXPERIMENT
#define NOODS_GX_EDGE_EXPERIMENT 0
#endif
#include "wii_video.h"

#ifndef NOODS_GX_INDEXED
#define NOODS_GX_INDEXED 0
#endif
#ifndef NOODS_GX_INDEXED_VERIFY
#define NOODS_GX_INDEXED_VERIFY 0
#endif

#ifndef NOODS_GX_EMPTY_FAST
#define NOODS_GX_EMPTY_FAST 1
#endif

#ifndef NOODS_GX_UNLOCK_WAIT
#define NOODS_GX_UNLOCK_WAIT 1
#endif

#ifndef NOODS_GX_SPAN_COVERAGE
#define NOODS_GX_SPAN_COVERAGE 0
#endif
#ifndef NOODS_V130_GX
#define NOODS_V130_GX 1
#endif

#ifndef GX3D_OFS_X
#define GX3D_OFS_X 0.0f
#define GX3D_OFS_Y 0.0f
#endif

uint32_t g_gxSpanStats[4]; // eligible polygons, reference rows, emitted rows, skipped eligibility
uint32_t g_gxIndexedStats[8]; // packed fmt2/3/4, binds, verified frames, wrong pixels, fallbacks, decoded texels
uint32_t g_gxDeltaStats[12]; // v1.160 frames, corrected pixels, critical polygons, skipped, rows, full, cached, misses, budget, max polys, max critical, max rows

namespace {
bool s_ciReference = false, s_ciVerifyActive = false;
#if GX3D_EMPTY_SELFTEST
bool s_emptyTestDone = false, s_emptyTestBypass = false;
#endif


struct TexEntry {
    uint32_t addr, pal, hash, lastFrame, bytes, gen;
    uint16_t w, h;
    uint8_t fmt;
    bool trans0;
    void *data;
    GXTexObj obj;
    GXTlutObj tlut;
    bool indexed;
};

const int MAX_TEX = 256;
const uint32_t MAX_TEX_BYTES = 6 * 1024 * 1024;
TexEntry s_tex[MAX_TEX];
#if NOODS_GX_TEX_INDEX
GxTextureIndex s_texIndex;
#endif
int s_texCount = 0;
uint32_t s_texBytes = 0;
uint32_t s_frame = 0;
uint8_t *s_copyBuf = nullptr;
uint8_t *s_scratch = nullptr;
uint8_t *s_idBuf = nullptr;   // v1.110: polygon IDs read back for shadows (R8, 8x4 tiles)
uint8_t *s_zBuf = nullptr;    // v1.110: depth read back for shadows (Z24X8, 4x4 tiles)
uint8_t s_stencil[256 * 192]; // shadow mask
uint8_t s_shadowId[256 * 192];// ID of the last shadow drawn on a pixel (0xFF = none)
bool s_texDirty = false;
#if NOODS_V130_GX
// Polygon-local vertices: strip vertices may have different alpha/texture scales
// in different polygons. The arrays stay immutable until GX_DrawDone below.
struct GxVertex { float pos[3], tex[3]; uint8_t color[4]; };
GxVertex *s_vertices = nullptr;
uint32_t s_vertexCapacity = 0;
uint16_t s_vertexBase[2048];
#endif

inline uint32_t hashBytes(uint32_t h, const uint8_t *p, uint32_t n) {
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);
        h = (h ^ w) * 0x9E3779B1u;
        h = (h << 13) | (h >> 19);
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x01000193u;
    return h;
}

void freeAllTextures() {
    for (int i = 0; i < s_texCount; i++)
        free(s_tex[i].data);
    s_texCount = 0;
#if NOODS_GX_TEX_INDEX
    s_texIndex.clear();
#endif
    s_texBytes = 0;
    s_texDirty = true;
}

inline uint8_t c6to8(uint32_t c) { return (uint8_t)((c << 2) | (c >> 4)); }

} // namespace

// Called after the renderer thread has stopped and its draw-done fences have
// completed. No CPU or GX command may retain these allocations at this point.
void gx3dReleaseCaches() {
    freeAllTextures();
    free(s_copyBuf); s_copyBuf = nullptr;
    free(s_scratch); s_scratch = nullptr;
    free(s_idBuf); s_idBuf = nullptr;
    free(s_zBuf); s_zBuf = nullptr;
#if NOODS_V130_GX
    free(s_vertices); s_vertices = nullptr; s_vertexCapacity = 0;
#endif
    s_frame = 0;
}

uint32_t g_gx3dDbg[8];
extern uint32_t g_vramGen; // memory.cpp: counts VRAM mapping changes

#if NOODS_V161_GX_OVERLAY
uint32_t g_gxOverlayActive = 0;
uint32_t g_gxOverlayBudgetSkip = 0;
uint32_t g_gxOverlayRunCount = 0;
GxOverlayRun g_gxOverlayRuns[MAX_GX_OVERLAY_RUNS];
uint32_t g_gxOverlayStats[8] = {};

// Replay a CPU-derived final-image primitive list as GX quads. The displayed
// EFB is written by GX; no CPU pixel writes or software framebuffer fallback.
static void gx3d_submitOverlay() {
    const uint32_t count = g_gxOverlayRunCount;
    if (!g_gxOverlayActive || !count) return;

    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.0f, 1.0f, (GXColor){0, 0, 0, 0});
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_PixModeSync();

    // GX_Begin's vertex count is 16-bit; cap each batch at 4096 quads.
    for (uint32_t base = 0; base < count;) {
        const uint32_t n = (count - base > 4096) ? 4096 : (count - base);
        GX_Begin(GX_QUADS, GX_VTXFMT1, n * 4);
        for (uint32_t k = 0; k < n; ++k) {
            const GxOverlayRun &r = g_gxOverlayRuns[base + k];
            const uint32_t c = r.color;
            const uint8_t red = c6to8(c & 0x3F);
            const uint8_t green = c6to8((c >> 6) & 0x3F);
            const uint8_t blue = c6to8((c >> 12) & 0x3F);
            const uint8_t alpha = c6to8((c >> 18) & 0x3F);
            const float x0 = (float)r.x0, x1 = (float)r.x1;
            const float y0 = (float)r.y, y1 = y0 + 1.0f;
            GX_Position3f32(x0, y0, 0.0f); GX_Normal3f32(0, 0, 1); GX_Color4u8(red, green, blue, alpha);
            GX_Position3f32(x1, y0, 0.0f); GX_Normal3f32(0, 0, 1); GX_Color4u8(red, green, blue, alpha);
            GX_Position3f32(x1, y1, 0.0f); GX_Normal3f32(0, 0, 1); GX_Color4u8(red, green, blue, alpha);
            GX_Position3f32(x0, y1, 0.0f); GX_Normal3f32(0, 0, 1); GX_Color4u8(red, green, blue, alpha);
        }
        GX_End();
        base += n;
    }
    GX_SetNumTexGens(1);
}
#endif

// Hash of the guest memory a texture is decoded from, so that a changed texture is re-decoded.
uint32_t Gpu3DRenderer::gxTextureHash(_Polygon *p) {
    uint32_t texels = (uint32_t)p->sizeS * p->sizeT, texBytes, palBytes;
    switch (p->textureFmt) {
        case 1: texBytes = texels; palBytes = 64; break;
        case 2: texBytes = texels / 4; palBytes = 8; break;
        case 3: texBytes = texels / 2; palBytes = 32; break;
        case 4: texBytes = texels; palBytes = 512; break;
        case 5: texBytes = texels / 4; palBytes = 0; break;
        case 6: texBytes = texels; palBytes = 16; break;
        default: texBytes = texels * 2; palBytes = 0; break;
    }
    uint32_t h = 0x811C9DC5u;
    const uint32_t off = p->textureAddr & 0x1FFFF;
    if (const uint8_t *slot = core->memory.tex3D[(p->textureAddr >> 17) & 3]) {
        const uint32_t n = (off + texBytes > 0x20000) ? (0x20000 - off) : texBytes;
        h = hashBytes(h, slot + off, n);
    }
    if (p->textureFmt == 5) {
        // 4x4 compressed: the per-block palette words live in slot 1
        const uint32_t idxAddr = 0x20000 + off / 2 + (((p->textureAddr >> 17) == 2) ? 0x10000 : 0);
        if (const uint8_t *slot = core->memory.tex3D[(idxAddr >> 17) & 3]) {
            const uint32_t o = idxAddr & 0x1FFFF, n0 = texels / 8;
            h = hashBytes(h, slot + o, (o + n0 > 0x20000) ? (0x20000 - o) : n0);
        }
    }
    if (palBytes && p->paletteAddr < (6u << 14)) {
        if (const uint8_t *slot = core->memory.pal3D[p->paletteAddr >> 14]) {
            const uint32_t o = p->paletteAddr & 0x3FFF;
            h = hashBytes(h, slot + o, (o + palBytes > 0x4000) ? (0x4000 - o) : palBytes);
        }
    }
    return h;
}

// Find or build the GX texture for a polygon. Returns an index into s_tex, or -1.
int Gpu3DRenderer::gxGetTexture(_Polygon *p) {
    // Restrict hardware palette lookup to fully mapped contiguous input spans.
    const bool paletteFmt = p->textureFmt >= 2 && p->textureFmt <= 4;
    const uint32_t entries = p->textureFmt == 2 ? 4u : (p->textureFmt == 3 ? 16u : 256u);
    const uint32_t texBytes = (p->sizeS * p->sizeT) >> (p->textureFmt == 2 ? 2 : (p->textureFmt == 3 ? 1 : 0));
    const uint8_t *ciTex = nullptr, *ciPal = nullptr;
#if NOODS_GX_INDEXED
    if (paletteFmt && !s_ciReference && p->textureAddr < 0x80000u &&
        (p->textureAddr & 0x1FFFFu) + texBytes <= 0x20000u &&
        p->paletteAddr < 0x18000u && (p->paletteAddr & 0x3FFFu) + entries * 2 <= 0x4000u) {
        ciTex = getTexture(p->textureAddr);
        ciPal = getPalette(p->paletteAddr);
    }
#endif
    const bool indexed = ciTex && ciPal;
    // The guest memory behind a texture is hashed once per 3D frame, however many polygons use it.
    int found = -1;
#if NOODS_GX_TEX_INDEX
    uint32_t indexSlot;
    found = s_texIndex.find(*p, s_tex, indexSlot);
#else
    for (int i = 0; i < s_texCount; i++) {
        if (GxTextureIndex::same(*p, s_tex[i])) { found = i; break; }
    }
#endif
    if (found >= 0) {
        TexEntry &e = s_tex[found];
        if (e.indexed == indexed) {
            if (e.lastFrame == s_frame) return found;
            // Texture-mapped VRAM cannot be written until remapped. Preserve
            // the original generation/hash validation, including capture invalidation.
            if (e.gen == g_vramGen) { e.lastFrame = s_frame; return found; }
        }
    }
    const uint32_t hash = gxTextureHash(p);
    if (found >= 0 && s_tex[found].indexed == indexed && s_tex[found].hash == hash) {
        s_tex[found].lastFrame = s_frame;
        s_tex[found].gen = g_vramGen;
        return found;
    }

    const bool alphaFmt = (p->textureFmt == 1 || p->textureFmt == 6);
    const uint32_t w = p->sizeS, h = p->sizeT;
    // Keep capacity sufficient for either representation across VRAM remaps.
    // Palette storage remains immutable while queued GX commands reference it.
    const uint32_t bytes = w * h * (alphaFmt ? 4 : 2) + ((NOODS_GX_INDEXED && paletteFmt) ? 512u : 0u);
    if (found < 0) {
        if (s_texCount >= MAX_TEX || s_texBytes + bytes > MAX_TEX_BYTES)
            return -1;
        void *data = memalign(32, (bytes + 31) & ~31u);
        if (!data) return -1;
        found = s_texCount++;
        s_texBytes += bytes;
        TexEntry &e = s_tex[found];
        e.addr = p->textureAddr; e.pal = p->paletteAddr; e.w = w; e.h = h;
        e.fmt = p->textureFmt; e.trans0 = p->transparent0; e.data = data; e.bytes = bytes;
#if NOODS_GX_TEX_INDEX
        s_texIndex.insert(indexSlot, found);
#endif
    }
    TexEntry &e = s_tex[found];
    e.hash = hash;
    e.lastFrame = s_frame;
    e.gen = g_vramGen;
    e.indexed = indexed;
    if (indexed) {
        uint8_t *pal = (uint8_t*)e.data + w * h * 2; // 32-byte aligned
        memset(pal, 0, 512);
        NoodsGx::packIndices(ciTex, (uint8_t*)e.data, w, h, p->textureFmt);
        NoodsGx::packPalette(ciPal, pal, entries, p->transparent0);
        DCFlushRange(e.data, w * h);
        DCFlushRange(pal, 512);
        GX_InitTlutObj(&e.tlut, pal, GX_TL_RGB5A3, 256);
        GX_InitTexObjCI(&e.obj, e.data, w, h, GX_TF_CI8, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0);
        GX_InitTexObjLOD(&e.obj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        ++g_gxIndexedStats[p->textureFmt - 2];
        g_gxIndexedStats[7] += w * h;
        s_texDirty = true;
        return found;
    }
    if (NOODS_GX_INDEXED && paletteFmt && !s_ciReference) ++g_gxIndexedStats[6];

    // Decode with the software texel reader, without wrapping, into GX's 4x4-tiled layout.
    _Polygon tmp = *p;
    tmp.repeatS = tmp.repeatT = tmp.flipS = tmp.flipT = false;
    uint8_t *dst = (uint8_t*)e.data;
    for (uint32_t ty = 0; ty < h; ty += 4) {
        for (uint32_t tx = 0; tx < w; tx += 4) {
            for (uint32_t y = 0; y < 4; y++) {
                for (uint32_t x = 0; x < 4; x++) {
                    const uint32_t c = readTexture(&tmp, tx + x, ty + y); // a6 b6 g6 r6
                    const uint32_t r = c & 0x3F, g = (c >> 6) & 0x3F, b = (c >> 12) & 0x3F, a = (c >> 18) & 0x3F;
                    const uint32_t i = y * 4 + x;
                    if (alphaFmt) {
                        dst[i * 2] = c6to8(a); dst[i * 2 + 1] = c6to8(r);
                        dst[32 + i * 2] = c6to8(g); dst[32 + i * 2 + 1] = c6to8(b);
                    } else {
                        const uint16_t v = a ? (0x8000 | ((r >> 1) << 10) | ((g >> 1) << 5) | (b >> 1)) : 0;
                        dst[i * 2] = v >> 8; dst[i * 2 + 1] = v & 0xFF;
                    }
                }
            }
            dst += alphaFmt ? 64 : 32;
        }
    }
    DCFlushRange(e.data, (bytes + 31) & ~31u);
    GX_InitTexObj(&e.obj, e.data, w, h, alphaFmt ? GX_TF_RGBA8 : GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&e.obj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    s_texDirty = true;
    return found;
}

// v1.110: shadow polygons, drawn on the CPU over the image read back from GX.
// DS rules: a shadow polygon with ID 0 is a mask and marks the pixels where it is hidden
// behind the scene; a shadow polygon with another ID is drawn where it is visible, the mask
// is set, and the pixel does not belong to a polygon with the same ID (so an object does not
// shadow itself). A pixel takes one shadow per ID.
void Gpu3DRenderer::gxDrawShadows() {
    Gpu3D &g3 = core->gpu3D;
    const int polyCount = g3.polygonCountOut;
    uint32_t *const fb = framebuffer[0];
    memset(s_shadowId, 0xFF, sizeof(s_shadowId));
    memset(s_stencil, 0, sizeof(s_stencil));
    bool lastWasMask = false;
    const bool blendOn = (disp3DCnt & BIT(3)) != 0;

    auto one = [&](const _Polygon *p, const Vertex *verts) {
        if (p->mode != 3 || p->size < 3) return;
        const bool mask = (p->id == 0);
        if (mask && !lastWasMask)
            memset(s_stencil, 0, sizeof(s_stencil));
        lastWasMask = mask;
        if (!mask && p->alpha == 0) return;

        // vertices: screen position and the same depth value that was sent to GX
        const int n = p->size;
        float vx[10], vy[10], vd[10];
        int yMin = 192, yMax = 0;
        for (int j = 0; j < n; j++) {
            const Vertex *v = &verts[j];
            const float fw = v->w ? (float)(uint32_t)v->w : 1.0f;
            float d = p->wBuffer ? (1.0f - gxWMin / fw) : (float)(uint32_t)v->z * (1.0f / 16777216.0f);
            if (d < 0.0f) d = 0.0f;
            if (d > 0.99998f) d = 0.99998f;
            vx[j] = (float)v->x; vy[j] = (float)v->y; vd[j] = d * 16777215.0f;
            if (v->y < yMin) yMin = v->y;
            if (v->y > yMax) yMax = v->y;
        }
        if (p->crossed && n >= 4) {
            float t;
            t = vx[2]; vx[2] = vx[3]; vx[3] = t;
            t = vy[2]; vy[2] = vy[3]; vy[3] = t;
            t = vd[2]; vd[2] = vd[3]; vd[3] = t;
        }
        if (yMin < 0) yMin = 0;
        if (yMax > 192) yMax = 192;

        const uint32_t col = verts[0].color;
        const uint32_t sr = col & 0x3F, sg = (col >> 6) & 0x3F, sb = (col >> 12) & 0x3F, sa = p->alpha;

        for (int y = yMin; y < yMax; y++) {
            // span of the convex polygon on this line, sampled at the pixel centre row
            const float fy = (float)y + 0.5f;
            float xl = 1e9f, xr = -1e9f, dl = 0, dr = 0;
            for (int j = 0; j < n; j++) {
                const int k = (j + 1 == n) ? 0 : j + 1;
                const float y0 = vy[j], y1 = vy[k];
                if (y0 == y1) continue;
                if ((fy < y0) == (fy < y1)) continue;
                const float t = (fy - y0) / (y1 - y0);
                const float x = vx[j] + (vx[k] - vx[j]) * t;
                const float d = vd[j] + (vd[k] - vd[j]) * t;
                if (x < xl) { xl = x; dl = d; }
                if (x > xr) { xr = x; dr = d; }
            }
            if (xr <= xl) continue;
            int x0 = (int)(xl + 0.5f), x1 = (int)(xr + 0.5f);
            const float dStep = (dr - dl) / (xr - xl);
            if (x0 < 0) x0 = 0;
            if (x1 > 256) x1 = 256;
            float d = dl + dStep * ((float)x0 + 0.5f - xl);
            const uint8_t *zrow4 = s_zBuf + ((y >> 2) * 64) * 64; // 64 tiles per row, 64 bytes each
            const uint8_t *irow8 = s_idBuf + ((y >> 2) * 32) * 32; // 32 tiles per row, 32 bytes each
            for (int x = x0; x < x1; x++, d += dStep) {
                const uint8_t *zt = zrow4 + (x >> 2) * 64 + (y & 3) * 8 + (x & 3) * 2;
                const uint32_t zb = ((uint32_t)zt[1] << 16) | ((uint32_t)zt[32] << 8) | zt[33];
                const int idx = y * 256 + x;
                const bool pass = d < (float)zb;
                if (mask) {
                    if (!pass) s_stencil[idx] = 1;
                    continue;
                }
                if (!pass || !s_stencil[idx]) continue;
                const uint32_t dstId = irow8[(x >> 3) * 32 + (y & 3) * 8 + (x & 7)] >> 2;
                if (dstId == p->id || s_shadowId[idx] == p->id) continue;
                s_shadowId[idx] = p->id;
                uint32_t &px = fb[y * 512 + x];
                const uint32_t c = px;
                uint32_t r = c & 0x3F, g = (c >> 6) & 0x3F, b = (c >> 12) & 0x3F, a = (c >> 18) & 0x3F;
                if (blendOn && a) {
                    r = (sr * (sa + 1) + r * (63 - sa)) >> 6;
                    g = (sg * (sa + 1) + g * (63 - sa)) >> 6;
                    b = (sb * (sa + 1) + b * (63 - sa)) >> 6;
                    if (sa > a) a = sa;
                } else {
                    r = sr; g = sg; b = sb; a = sa;
                }
                px = BIT(26) | (a << 18) | (b << 12) | (g << 6) | r;
                g_gx3dDbg[1]++;
            }
        }
        g_gx3dDbg[0]++;
    };
    for (int i = 0; i < polyCount && i < 2048; i++)
        one(&g3.polygonsOut[i], &g3.verticesOut[g3.polygonsOut[i].vertices]);
#ifdef GX3D_FORCE_SHADOW
    {   // self-test: a mask far behind the scene and a half-black shadow in front, ID 5
        _Polygon q; q.mode = 3; q.size = 4; q.alpha = 0x20;
        Vertex v[4];
        const int xs[4] = {40, 200, 200, 40}, ys[4] = {60, 60, 170, 170};
        for (int j = 0; j < 4; j++) { v[j].x = xs[j]; v[j].y = ys[j]; v[j].w = 1; v[j].color = 0; }
        q.id = 0; for (int j = 0; j < 4; j++) v[j].z = 0xF00000; one(&q, v);
        q.id = 5; for (int j = 0; j < 4; j++) v[j].z = 0x000100; one(&q, v);
    }
#endif
    // debug: one depth sample from the top-left pixel and from the centre
    g_gx3dDbg[2] = ((uint32_t)s_zBuf[1] << 16) | ((uint32_t)s_zBuf[32] << 8) | s_zBuf[33];
    g_gx3dDbg[3] = s_idBuf[0];
    g_gx3dDbg[4] = ((uint32_t)s_zBuf[0] << 24) | ((uint32_t)s_zBuf[1] << 16) | ((uint32_t)s_zBuf[32] << 8) | s_zBuf[33];
}

void Gpu3DRenderer::drawFrameGx() {
    Gpu3D &g3 = core->gpu3D;
    const int polyCount = g3.polygonCountOut;
#if NOODS_GX_INDEXED_VERIFY
    // Compare actual GX EFB readbacks for the SAME immutable polygon list,
    // first with the original expanded texture, then with CI8 + hardware TLUT.
    // Both complete their fences. This is deliberately not a speed build.
    static unsigned verifyCandidates = 0;
    bool hasPalette = false;
    if (!s_ciVerifyActive)
        for (int i = 0; i < polyCount && i < 2048; ++i)
            hasPalette |= g3.polygonsOut[i].textureFmt >= 2 && g3.polygonsOut[i].textureFmt <= 4;
    if (!s_ciVerifyActive && hasPalette && (++verifyCandidates % 13u) == 1u && g_gxIndexedStats[4] < 48) {
        static uint32_t reference[256 * 192];
        s_ciVerifyActive = true;
        s_ciReference = true;
        freeAllTextures();
        drawFrameGx();
        for (int y = 0; y < 192; ++y)
            memcpy(reference + y * 256, framebuffer[0] + y * 512, 256 * 4);
        s_ciReference = false;
        freeAllTextures();
        drawFrameGx();
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 256; ++x)
                g_gxIndexedStats[5] += reference[y * 256 + x] != framebuffer[0][y * 512 + x];
        ++g_gxIndexedStats[4];
        s_ciVerifyActive = false;
        return;
    }
#endif
    s_frame++;
    ++g_gx3dDbg[6];
#if NOODS_V161_GX_OVERLAY
    // Fast path: when the cached primitive list covers the full 3D image,
    // GX can draw it directly. Skip normal polygon submission, texture
    // resolution, span capture, depth setup, and the CPU shadow pass.
    if (g_gxOverlayActive && g_gxOverlayRunCount) {
        if (!s_copyBuf)
            s_copyBuf = (uint8_t*)memalign(32, 256 * 192 * 4);
        if (!s_scratch)
            s_scratch = (uint8_t*)memalign(32, 256 * 192 / 2);
        Wii_GxLock();
        GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
        GX_SetViewport(0, 0, 256, 192, 0, 1);
        GX_SetScissor(0, 0, 256, 192);
        Mtx44 proj;
        guOrtho(proj, 0, 192, 0, 256, 0, 1);
        GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
        Mtx ident;
        guMtxIdentity(ident);
        GX_LoadPosMtxImm(ident, GX_PNMTX0);
        GX_SetCurrentMtx(GX_PNMTX0);
        GX_SetZCompLoc(GX_FALSE);
        GX_SetCullMode(GX_CULL_NONE);
        GX_SetClipMode(GX_CLIP_DISABLE);
        GX_SetDither(GX_FALSE);
        GX_SetColorUpdate(GX_TRUE);
        GX_SetAlphaUpdate(GX_TRUE);
        GX_SetDstAlpha(GX_DISABLE, 0);
        GX_SetNumChans(1);
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GX_SetNumTexGens(0);
        GX_SetNumTevStages(1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.0f, 1.0f, (GXColor){0, 0, 0, 0});
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        GX_PixModeSync();
        gx3d_submitOverlay();

        Wii_GxCopyFilter(false);
        DCInvalidateRange(s_copyBuf, 256 * 192 * 4);
        GX_SetTexCopySrc(0, 0, 256, 192);
        GX_SetTexCopyDst(256, 192, GX_TF_RGBA8, GX_FALSE);
        GX_CopyTex(s_copyBuf, GX_FALSE);
        GX_PixModeSync();
        Wii_GxRestoreDisplayState();
        if (s_scratch) {
            GX_SetTexCopySrc(0, 0, 256, 192);
            GX_SetTexCopyDst(256, 192, GX_TF_I4, GX_FALSE);
            GX_CopyTex(s_scratch, GX_TRUE);
            GX_PixModeSync();
        }
#if NOODS_GX_UNLOCK_WAIT
        GX_SetDrawDone();
        Wii_GxUnlock();
        GX_WaitDrawDone();
#else
        GX_Flush();
        GX_DrawDone();
        Wii_GxUnlock();
#endif
        uint32_t *const fb = framebuffer[0];
        const uint8_t *tile = s_copyBuf;
        for (int ty = 0; ty < 192; ty += 4) {
            for (int tx = 0; tx < 256; tx += 4) {
                for (int y = 0; y < 4; y++) {
                    uint32_t *row = &fb[(ty + y) * 512 + tx];
                    const uint16_t *ar = (const uint16_t*)(tile + y * 8), *gb = (const uint16_t*)(tile + 32 + y * 8);
                    for (int x = 0; x < 4; x++) {
                        const uint32_t p = ar[x], q = gb[x];
                        row[x] = BIT(26) | ((p & 0xFC00u) << 8) | ((p & 0xFCu) >> 2) |
                                 ((q & 0xFC00u) >> 4) | ((q & 0xFCu) << 10);
                    }
                }
                tile += 64;
            }
        }
        PPCIrqState st = PPCIrqLockByMsr();
        for (int i = 0; i < 192 * 2; i++)
            ready[i] = 3;
        PPCIrqUnlockByMsr(st);
        return;
    }
#endif
#if GX3D_EMPTY_SELFTEST
    if (!s_emptyTestDone && polyCount == 0) {
        s_emptyTestDone = s_emptyTestBypass = true;
        const uint32_t savedColor = clearColor;
        uint32_t bad = 0, tested = 0;
        FILE *test = fopen("sd:/empty-selftest.log", "w");
        // Exercise every 5-bit input value in each independent color/alpha
        // channel against the actual GX RGBA6 clear/copy/unpack path.
        for (uint32_t i = 0; i < 40; ++i) {
            uint32_t c = ((i * 7) & 31) | (((i * 13) & 31) << 5) |
                         (((i * 23) & 31) << 10) | ((i & 31) << 16);
            if (i >= 32) c = 0x7FFFu | (((i - 32) * 4u) << 16);
            clearColor = c;
            drawFrameGx(); // forced reference path; fence completes before read
            const uint32_t a5 = (c >> 16) & 31u;
            const uint32_t expected = BIT(26) | rgba5ToRgba6(c & 0x7FFFu) |
                                      ((a5 ? a5 * 2u + 1u : 0u) << 18);
            uint32_t mismatches = 0;
            for (int y = 0; y < 192; ++y)
                for (int x = 0; x < 256; ++x)
                    mismatches += framebuffer[0][y * 512 + x] != expected;
            bad += mismatches != 0;
            ++tested;
            if (test) fprintf(test, "clear=%08X expected=%08X actual=%08X mismatched_pixels=%u\n",
                              c, expected, framebuffer[0][0], mismatches);
        }
        if (test) { fprintf(test, "SUMMARY tested=%u failed=%u\n", tested, bad); fclose(test); }
        clearColor = savedColor;
        s_emptyTestBypass = false;
    }
#endif
#if NOODS_GX_EMPTY_FAST
    // With no polygons, no clear-image mode and no injected shadow test, the
    // exact result is the constant RGBA6 clear value. Match the GX clear
    // quad's 5-to-6 expansion, including its special zero-alpha mapping.
    // This still processes the frame and publishes every ready scanline;
    // only an unnecessary EFB clear/copy/fence/unpack round trip is removed.
#ifndef GX3D_FORCE_SHADOW
    if (polyCount == 0 && !(disp3DCnt & BIT(14))
#if GX3D_EMPTY_SELFTEST
        && !s_emptyTestBypass
#endif
    ) {
        const uint32_t a5 = (clearColor >> 16) & 31u;
        const uint32_t color = BIT(26) | rgba5ToRgba6(clearColor & 0x7FFFu) |
                               ((a5 ? a5 * 2u + 1u : 0u) << 18);
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 256; ++x)
                framebuffer[0][y * 512 + x] = color;
        PPCIrqState irq = PPCIrqLockByMsr();
        for (int i = 0; i < 192 * 2; ++i) ready[i] = 3;
        PPCIrqUnlockByMsr(irq);
        ++g_gx3dDbg[5];
        return;
    }
#endif
#endif

    if (!s_copyBuf)
        s_copyBuf = (uint8_t*)memalign(32, 256 * 192 * 4);

    // CPU-only preparation does not own the display/GX mutex. The previous
    // emulation draw completed before its arrays/textures can be reused.
    // Drop the texture cache when it is full; it refills from the polygons in use.
    if (s_texCount >= MAX_TEX - 16 || s_texBytes > MAX_TEX_BYTES - (512 * 1024))
        freeAllTextures();

    // Resolve textures first so that the cache state is known before any drawing.
    static int16_t texIdx[2048];
    {
        // consecutive polygons usually share a texture: remember the last lookup
        const _Polygon *lp = nullptr;
        int lastIdx = -1;
        for (int i = 0; i < polyCount && i < 2048; i++) {
            _Polygon *p = &g3.polygonsOut[i];
            if (p->textureFmt == 0 || p->sizeS < 8 || p->sizeT < 8) { texIdx[i] = -1; continue; }
            if (lp && lp->textureAddr == p->textureAddr && lp->paletteAddr == p->paletteAddr && lp->sizeS == p->sizeS &&
                lp->sizeT == p->sizeT && lp->textureFmt == p->textureFmt && lp->transparent0 == p->transparent0) {
                texIdx[i] = lastIdx;
                continue;
            }
            texIdx[i] = lastIdx = gxGetTexture(p);
            lp = p;
        }
    }

    // Depth: the z-buffer value is linear in screen space and used as is. In w-buffer mode
    // 1 - wMin / w is used: it is monotonic in w and, unlike w, linear in screen space.
    bool wBuf = false;
    float wMin = 1.0f;
    {
        uint32_t m = 0xFFFFFFFFu;
        for (int i = 0; i < polyCount; i++) {
            _Polygon *p = &g3.polygonsOut[i];
            if (p->wBuffer) wBuf = true;
            for (int j = 0; j < p->size; j++) {
                const uint32_t w = (uint32_t)g3.verticesOut[p->vertices + j].w;
                if (w && w < m) m = w;
            }
        }
        if (m != 0xFFFFFFFFu) wMin = (float)m;
    }
    gxWMin = wMin;

#if NOODS_V130_GX
    uint32_t vertexCount = 0;
    for (int i = 0; i < polyCount && i < 2048; ++i) {
        const _Polygon *p = &g3.polygonsOut[i];
        s_vertexBase[i] = (uint16_t)vertexCount;
        if (p->mode != 3 && p->size >= 3 && p->alpha != 0)
            vertexCount += p->size;
    }
    if (vertexCount > s_vertexCapacity) {
        // Old data was consumed by the previous frame's DrawDone. Keep the old
        // allocation on failure and fall back to direct submission this frame.
        const uint32_t capacity = (vertexCount + 255u) & ~255u;
        GxVertex *data = (GxVertex*)memalign(32, (capacity * sizeof(GxVertex) + 31u) & ~31u);
        if (data) { free(s_vertices); s_vertices = data; s_vertexCapacity = capacity; }
    }
    const bool indexed = vertexCount != 0 && vertexCount <= s_vertexCapacity;
    if (indexed) {
        for (int i = 0; i < polyCount && i < 2048; ++i) {
            _Polygon *p = &g3.polygonsOut[i];
            const int n = p->size, ti = texIdx[i];
            if (p->mode == 3 || n < 3 || p->alpha == 0) continue;
            Vertex *vs[10];
            for (int j = 0; j < n; ++j) vs[j] = &g3.verticesOut[p->vertices + j];
            if (p->crossed && n >= 4) { Vertex *t = vs[2]; vs[2] = vs[3]; vs[3] = t; }
            uint32_t wMax = 1;
            for (int j = 0; j < n; ++j)
                if ((uint32_t)vs[j]->w > wMax) wMax = (uint32_t)vs[j]->w;
            const float fwMax = (float)wMax;
            const float sScale = ti >= 0 ? 1.0f / (16.0f * p->sizeS) : 0.0f;
            const float tScale = ti >= 0 ? 1.0f / (16.0f * p->sizeT) : 0.0f;
            const uint8_t a8 = c6to8(p->alpha);
#if NOODS_GX_EDGE_EXPERIMENT
            // EXPERIMENT ONLY: outward bias of opaque polygon coverage. Not DS accurate.
            int sx = 0, sy = 0;
            const bool expCoverage = (disp3DCnt & (BIT(4) | BIT(5))) && p->alpha == 63 && p->textureFmt != 1 && p->textureFmt != 6 && !p->crossed;
            if (expCoverage) for (int k = 0; k < n; ++k) { sx += vs[k]->x; sy += vs[k]->y; }
#endif
            for (int j = 0; j < n; ++j) {
                const Vertex *v = vs[j];
                const float fw = v->w ? (float)(uint32_t)v->w : 1.0f;
                float d = p->wBuffer ? (1.0f - wMin / fw) : (float)(uint32_t)v->z * (1.0f / 16777216.0f);
                if (d < 0.0f) d = 0.0f;
                if (d > 0.99998f) d = 0.99998f;
                const float q = fwMax / fw;
                uint32_t col = v->color;
                if (p->mode == 2) col = rgba5ToRgba6(toonTable[(col & 0x3F) / 2]);
                GxVertex &out = s_vertices[s_vertexBase[i] + j];
                out.pos[0] = (float)v->x + GX3D_OFS_X;
                out.pos[1] = (float)v->y + GX3D_OFS_Y;
#if NOODS_GX_EDGE_EXPERIMENT
                if (expCoverage) {
                    out.pos[0] += v->x * n > sx ? 0.5f : (v->x * n < sx ? -0.5f : 0.0f);
#if NOODS_GX_EDGE_EXPERIMENT == 1
                    out.pos[1] += v->y * n > sy ? 0.5f : (v->y * n < sy ? -0.5f : 0.0f);
#endif
                }
#endif
                out.pos[2] = -d;
                out.tex[0] = (float)v->s * sScale * q;
                out.tex[1] = (float)v->t * tScale * q;
                out.tex[2] = q;
                out.color[0] = c6to8(col & 0x3F);
                out.color[1] = c6to8((col >> 6) & 0x3F);
                out.color[2] = c6to8((col >> 12) & 0x3F);
                out.color[3] = a8;
            }
        }
        DCFlushRange(s_vertices, (vertexCount * sizeof(GxVertex) + 31u) & ~31u);
    }
#endif

    int shadowCount = 0;
    for (int i = 0; i < polyCount && i < 2048; i++)
        if (g3.polygonsOut[i].mode == 3 && g3.polygonsOut[i].size >= 3) shadowCount++;
#ifdef GX3D_FORCE_SHADOW
    shadowCount++;
#endif
    if (shadowCount) {
        if (!s_idBuf) s_idBuf = (uint8_t*)memalign(32, 256 * 192);
        if (!s_zBuf) s_zBuf = (uint8_t*)memalign(32, 256 * 192 * 4);
    }
    const bool doShadows = !g_gxOverlayActive && shadowCount && s_idBuf && s_zBuf;

    if (!s_scratch)
        s_scratch = (uint8_t*)memalign(32, 256 * 192 / 2);
    Wii_GxLock();
    if (s_texDirty) {
        GX_InvalidateTexAll();
        s_texDirty = false;
    }

    // ---- state ----
    GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
    GX_SetViewport(0, 0, 256, 192, 0, 1);
    GX_SetScissor(0, 0, 256, 192);
    Mtx44 proj;
    guOrtho(proj, 0, 192, 0, 256, 0, 1);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    Mtx ident;
    guMtxIdentity(ident);
    GX_LoadPosMtxImm(ident, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_LoadTexMtxImm(ident, GX_TEXMTX0, GX_MTX3x4);
    GX_SetZCompLoc(GX_FALSE); // depth after the alpha test, so cut-out texels leave no depth
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetClipMode(GX_CLIP_DISABLE);
    GX_SetDither(GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetDstAlpha(GX_DISABLE, 0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    // Texture coordinates travel in the normal as (s/w, t/w, 1/w); the 3x4 texgen makes the
    // hardware divide per pixel, which gives perspective-correct texturing under an
    // orthographic projection.
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_NRM, GX_TEXMTX0);
    GX_SetNumTevStages(1);

    // ---- clear ----
    {
        const uint32_t cc = clearColor;
        const uint8_t r = c6to8(rgba5ToRgba6(cc & 0x7FFF) & 0x3F), g = c6to8((rgba5ToRgba6(cc & 0x7FFF) >> 6) & 0x3F),
            b = c6to8((rgba5ToRgba6(cc & 0x7FFF) >> 12) & 0x3F);
        const uint32_t a5 = (cc >> 16) & 0x1F;
        const uint8_t a = c6to8(a5 ? (a5 * 2 + 1) : 0);
        float d = (wBuf || clearDepth == 0x7FFF) ? 0.99999f : (float)(clearDepth << 9) * (1.0f / 16777216.0f);
        if (d > 0.99999f) d = 0.99999f;
        GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
        GX_Position3f32(0, 0, -d);     GX_Normal3f32(0, 0, 1); GX_Color4u8(r, g, b, a);
        GX_Position3f32(256, 0, -d);   GX_Normal3f32(0, 0, 1); GX_Color4u8(r, g, b, a);
        GX_Position3f32(256, 192, -d); GX_Normal3f32(0, 0, 1); GX_Color4u8(r, g, b, a);
        GX_Position3f32(0, 192, -d);   GX_Normal3f32(0, 0, 1); GX_Color4u8(r, g, b, a);
        GX_End();
    }

#if NOODS_V130_GX
    if (indexed) {
        GX_InvVtxCache();
        GX_SetArray(GX_VA_POS, s_vertices[0].pos, sizeof(GxVertex));
        GX_SetArray(GX_VA_NRM, s_vertices[0].tex, sizeof(GxVertex));
        GX_SetArray(GX_VA_CLR0, s_vertices[0].color, sizeof(GxVertex));
    }
    auto vertexMode = [&](bool useIndices, bool idColor) {
        GX_SetVtxDesc(GX_VA_POS, useIndices ? GX_INDEX16 : GX_DIRECT);
        GX_SetVtxDesc(GX_VA_NRM, useIndices ? GX_INDEX16 : GX_DIRECT);
        GX_SetVtxDesc(GX_VA_CLR0, (useIndices && !idColor) ? GX_INDEX16 : GX_DIRECT);
    };
    vertexMode(indexed, false);
#endif

    // ---- polygons ----
    const bool blendOn = (disp3DCnt & BIT(3)) != 0;
    const bool toonMode = !(disp3DCnt & BIT(1));
    (void)toonMode;
    int curTex = -2, curTev = -1;

    // v1.109: fog, approximated by one linear ramp between the depth where the DS density
    // table starts to rise and the depth where it reaches its maximum. Not done for
    // alpha-only fog or w-buffered scenes.
    bool fogOk = false;
    int curFog = 0;
    float fogStart = 0.0f, fogEnd = 1.0f;
    GXColor fogCol = {0, 0, 0, 0};
    if ((disp3DCnt & BIT(7)) && !(disp3DCnt & BIT(6)) && !wBuf) {
        const int step = 0x400 >> ((disp3DCnt & 0x0F00) >> 8);
        int lo = 0, hi = 31, dmax = fogTable[31];
        while (lo < 31 && fogTable[lo + 1] <= fogTable[0]) lo++;
        while (hi > lo + 1 && fogTable[hi - 1] >= dmax) hi--;
        if (dmax > 0 && step > 0) {
            const float d0 = (float)(fogOffset + (lo + 1) * step) * (1.0f / 32768.0f);
            float d1 = (float)(fogOffset + (hi + 1) * step) * (1.0f / 32768.0f);
            d1 = d0 + (d1 - d0) * (127.0f / (float)dmax);
            if (d1 > d0) {
                const uint32_t fc = rgba5ToRgba6(fogColor & 0x7FFF);
                fogCol = (GXColor){ c6to8(fc & 0x3F), c6to8((fc >> 6) & 0x3F), c6to8((fc >> 12) & 0x3F), 0xFF };
                fogStart = d0; fogEnd = d1; fogOk = true;
            }
        }
    }
    GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);

    bool idPass = false;
    int curTexKey = -1; // texture and wrap mode currently loaded
    auto submit = [&](const int i) {
        _Polygon *p = &g3.polygonsOut[i];
        const int n = p->size;
        const int ti = texIdx[i];
        // texture + combiner
        if (idPass) {
            // colour = polygon ID from the vertex colour, alpha = texture alpha (cut-outs)
            if (ti >= 0) {
                TexEntry &e = s_tex[ti];
                const uint8_t ws = p->repeatS ? (p->flipS ? GX_MIRROR : GX_REPEAT) : GX_CLAMP;
                const uint8_t wt = p->repeatT ? (p->flipT ? GX_MIRROR : GX_REPEAT) : GX_CLAMP;
                const int key = (ti << 4) | (ws << 2) | wt;
                if (key != curTexKey) {
                    GX_InitTexObjWrapMode(&e.obj, ws, wt);
                    if (e.indexed) { GX_LoadTlut(&e.tlut, GX_TLUT0); ++g_gxIndexedStats[3]; }
                    GX_LoadTexObj(&e.obj, GX_TEXMAP0);
                    curTexKey = key;
                }
                if (curTev != 3) {
                    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
                    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
                    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
                    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                    curTev = 3;
                }
            } else if (curTev != 0) {
                GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
                GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
                curTev = 0;
            }
        } else if (ti >= 0) {
            TexEntry &e = s_tex[ti];
            const uint8_t ws = p->repeatS ? (p->flipS ? GX_MIRROR : GX_REPEAT) : GX_CLAMP;
            const uint8_t wt = p->repeatT ? (p->flipT ? GX_MIRROR : GX_REPEAT) : GX_CLAMP;
            const int key = (ti << 4) | (ws << 2) | wt;
            if (key != curTexKey) {
                GX_InitTexObjWrapMode(&e.obj, ws, wt);
                if (e.indexed) { GX_LoadTlut(&e.tlut, GX_TLUT0); ++g_gxIndexedStats[3]; }
                GX_LoadTexObj(&e.obj, GX_TEXMAP0);
                curTexKey = key;
            }
            curTex = ti;
            const int tev = (p->mode == 1) ? 2 : 1;
            if (tev != curTev) {
                GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
                GX_SetTevOp(GX_TEVSTAGE0, (tev == 2) ? GX_DECAL : GX_MODULATE);
                curTev = tev;
            }
        } else if (curTev != 0) {
            GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
            curTev = 0;
        }

        const int wantFog = (fogOk && p->fog && !idPass) ? 1 : 0;
        if (wantFog != curFog) {
            if (wantFog) GX_SetFog(GX_FOG_ORTHO_LIN, fogStart, fogEnd, 0.0f, 1.0f, fogCol);
            else         GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.0f, 1.0f, fogCol);
            curFog = wantFog;
        }

#if NOODS_GX_SPAN_COVERAGE
        // Large commercial frames already use the normal GX path; do not pay
        // for per-polygon DS span-capture eligibility checks on those frames.
        if (!g_gxOverlayBudgetSkip) {
        // Use the software renderer's actual DS edge coverage, but GX still
        // fills, depth-tests and composites these flat constant attributes.
        // Varying attributes need proper DS interpolation and stay unchanged.
        bool spanEligible = !idPass && n >= 3 && p->textureFmt == 0 &&
            p->alpha != 0 && p->mode < 2 && !p->depthTestEqual &&
            !(disp3DCnt & (BIT(4) | BIT(5))) && !(fogOk && p->fog);
        const Vertex *v0 = &g3.verticesOut[p->vertices];
        for (int j = 1; spanEligible && j < n; ++j) {
            const Vertex *v = &g3.verticesOut[p->vertices + j];
            spanEligible = v->z == v0->z && v->w == v0->w && v->color == v0->color;
        }
        if (spanEligible && v0->w > 0) {
            int top = 192, bottom = 0;
            for (int j = 0; j < n; ++j) {
                int y = g3.verticesOut[p->vertices + j].y;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
            if (top == bottom) ++bottom;
            polygonTop[i] = top; polygonBot[i] = bottom;
            polygonSharesSameIdOpaqueEdge[i] = false;
            polyInv[i] = computePolyInvariant(p);
            uint16_t left[192], right[192];
            int y0 = top < 0 ? 0 : top, y1 = bottom > 192 ? 192 : bottom;
            if (y0 > 192) y0 = 192;
            if (y1 < y0) y1 = y0;
            int rows = 0;
            gxSpanCapture = true;
            for (int y = y0; y < y1; ++y) {
                gxSpanBegin = gxSpanEnd = 0;
                drawPolygon(y, i);
                ++g_gxSpanStats[1];
                left[y] = gxSpanBegin; right[y] = gxSpanEnd;
                if (gxSpanEnd > gxSpanBegin) ++rows;
            }
            gxSpanCapture = false;
            ++g_gxSpanStats[0];
            g_gxSpanStats[2] += rows;
            if (rows) {
#if NOODS_V130_GX
                if (indexed) vertexMode(false, false);
#endif
                float fw = (float)(uint32_t)v0->w;
                float d = p->wBuffer ? (1.0f - wMin / fw) : (float)(uint32_t)v0->z * (1.0f / 16777216.0f);
                if (d < 0.0f) d = 0.0f;
                if (d > 0.99998f) d = 0.99998f;
                uint32_t col = v0->color;
                const uint8_t r = c6to8(col & 0x3F), g = c6to8((col >> 6) & 0x3F), b = c6to8((col >> 12) & 0x3F), alpha = c6to8(p->alpha);
                GX_Begin(GX_QUADS, GX_VTXFMT1, rows * 4);
                for (int y = y0; y < y1; ++y) if (right[y] > left[y]) {
                    const float l = left[y], rgt = right[y], t = y, bot = y + 1;
                    GX_Position3f32(l,t,-d); GX_Normal3f32(0,0,1); GX_Color4u8(r,g,b,alpha);
                    GX_Position3f32(rgt,t,-d); GX_Normal3f32(0,0,1); GX_Color4u8(r,g,b,alpha);
                    GX_Position3f32(rgt,bot,-d); GX_Normal3f32(0,0,1); GX_Color4u8(r,g,b,alpha);
                    GX_Position3f32(l,bot,-d); GX_Normal3f32(0,0,1); GX_Color4u8(r,g,b,alpha);
                }
                GX_End();
#if NOODS_V130_GX
                if (indexed) vertexMode(true, false);
#endif
            }
            return;
        }
        ++g_gxSpanStats[3];
        }
#endif
#if NOODS_V130_GX
        if (indexed) {
            GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT1, n);
            for (int j = 0; j < n; ++j) {
                const uint16_t index = s_vertexBase[i] + j;
                GX_Position1x16(index);
                GX_Normal1x16(index);
                if (idPass) GX_Color4u8((uint8_t)(p->id << 2), 0, 0, 0xFF);
                else GX_Color1x16(index);
            }
            GX_End();
            return;
        }
#endif
        Vertex *vs[10];
        for (int j = 0; j < n; j++)
            vs[j] = &g3.verticesOut[p->vertices + j];
        if (p->crossed && n >= 4) { Vertex *t = vs[2]; vs[2] = vs[3]; vs[3] = t; }

        uint32_t wMax = 1;
        for (int j = 0; j < n; j++)
            if ((uint32_t)vs[j]->w > wMax) wMax = (uint32_t)vs[j]->w;
        const float fwMax = (float)wMax;
        const float sScale = (ti >= 0) ? 1.0f / (16.0f * p->sizeS) : 0.0f;
        const float tScale = (ti >= 0) ? 1.0f / (16.0f * p->sizeT) : 0.0f;
        const uint8_t a8 = c6to8(p->alpha);
#if NOODS_GX_EDGE_EXPERIMENT
        int sx = 0, sy = 0;
        const bool expCoverage = (disp3DCnt & (BIT(4) | BIT(5))) && p->alpha == 63 && p->textureFmt != 1 && p->textureFmt != 6 && !p->crossed;
        if (expCoverage) for (int k = 0; k < n; ++k) { sx += vs[k]->x; sy += vs[k]->y; }
#endif

        GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT1, n);
        for (int j = 0; j < n; j++) {
            const Vertex *v = vs[j];
            const float fw = v->w ? (float)(uint32_t)v->w : 1.0f;
            float d = p->wBuffer ? (1.0f - wMin / fw) : (float)(uint32_t)v->z * (1.0f / 16777216.0f);
            if (d < 0.0f) d = 0.0f;
            if (d > 0.99998f) d = 0.99998f;
            const float q = fwMax / fw;
            uint32_t col = v->color;
            if (p->mode == 2)
                col = rgba5ToRgba6(toonTable[(col & 0x3F) / 2]);
            float xp = (float)v->x + GX3D_OFS_X, yp = (float)v->y + GX3D_OFS_Y;
#if NOODS_GX_EDGE_EXPERIMENT
            if (expCoverage) {
                xp += v->x * n > sx ? 0.5f : (v->x * n < sx ? -0.5f : 0.0f);
#if NOODS_GX_EDGE_EXPERIMENT == 1
                yp += v->y * n > sy ? 0.5f : (v->y * n < sy ? -0.5f : 0.0f);
#endif
            }
#endif
            GX_Position3f32(xp, yp, -d);
            GX_Normal3f32((float)v->s * sScale * q, (float)v->t * tScale * q, q);
            if (idPass) GX_Color4u8((uint8_t)(p->id << 2), 0, 0, 0xFF);
            else GX_Color4u8(c6to8(col & 0x3F), c6to8((col >> 6) & 0x3F), c6to8((col >> 12) & 0x3F), a8);
        }
        GX_End();
    };
    (void)curTex;

    // Opaque polygons first, as the software renderer does.
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    int zState = -1;
    for (int i = 0; i < polyCount && i < 2048; i++) {
        _Polygon *p = &g3.polygonsOut[i];
        if (p->alpha < 0x3F || p->textureFmt == 1 || p->textureFmt == 6 || p->mode == 3 || p->size < 3)
            continue;
        const int zs = p->depthTestEqual ? 1 : 0;
        if (zs != zState) {
            GX_SetZMode(GX_TRUE, zs ? GX_LEQUAL : GX_LESS, GX_TRUE);
            zState = zs;
        }
        submit(i);
    }

    // Translucent polygons in list order: colour pass, then a pass that accumulates coverage
    // in the alpha channel (the 2D compositor uses it to blend the 3D layer).
    for (int i = 0; i < polyCount && i < 2048; i++) {
        _Polygon *p = &g3.polygonsOut[i];
        if (!(p->alpha < 0x3F || p->textureFmt == 1 || p->textureFmt == 6) || p->mode == 3 || p->size < 3)
            continue;
        if (p->alpha == 0)
            continue; // wireframe: not drawn
        if (blendOn) {
            GX_SetZMode(GX_TRUE, p->depthTestEqual ? GX_LEQUAL : GX_LESS, GX_FALSE);
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
            GX_SetAlphaUpdate(GX_FALSE);
            submit(i);
            GX_SetZMode(GX_TRUE, GX_LEQUAL, p->transNewDepth ? GX_TRUE : GX_FALSE);
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_INVDSTALPHA, GX_BL_ONE, GX_LO_CLEAR);
            GX_SetColorUpdate(GX_FALSE);
            GX_SetAlphaUpdate(GX_TRUE);
            submit(i);
            GX_SetColorUpdate(GX_TRUE);
        } else {
            GX_SetZMode(GX_TRUE, p->depthTestEqual ? GX_LEQUAL : GX_LESS, p->transNewDepth ? GX_TRUE : GX_FALSE);
            GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
            submit(i);
        }
    }

#if NOODS_V161_GX_OVERLAY
    // Last-in-order GX quads reproduce DS-only edge/AA/wireframe effects.
    // They overwrite the normal GX polygon result before the EFB read-back.
    if (g_gxOverlayActive) gx3d_submitOverlay();
#endif

    // ---- read back ----
    Wii_GxCopyFilter(false);
    DCInvalidateRange(s_copyBuf, 256 * 192 * 4);
    GX_SetTexCopySrc(0, 0, 256, 192);
    GX_SetTexCopyDst(256, 192, GX_TF_RGBA8, GX_FALSE);
    GX_CopyTex(s_copyBuf, GX_FALSE);
    GX_PixModeSync();
    GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.0f, 1.0f, (GXColor){0, 0, 0, 0});

    // v1.110: shadows. GX has no stencil buffer, so shadow polygons are drawn on the CPU after
    // the read-back. They need the depth and the polygon ID of every pixel: the depth buffer is
    // copied out, and the IDs come from drawing the opaque polygons once more with their ID as
    // the colour. Only done in frames that contain shadow polygons.
    if (doShadows) {
        curFog = 0;
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        GX_SetColorUpdate(GX_TRUE);
        GX_SetAlphaUpdate(GX_FALSE);
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        curTev = 0;
        const uint8_t cid = (uint8_t)(((clearColor >> 24) & 0x3F) << 2);
#if NOODS_V130_GX
        vertexMode(false, true);
#endif
        GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
        GX_Position3f32(0, 0, 0);     GX_Normal3f32(0, 0, 1); GX_Color4u8(cid, 0, 0, 0xFF);
        GX_Position3f32(256, 0, 0);   GX_Normal3f32(0, 0, 1); GX_Color4u8(cid, 0, 0, 0xFF);
        GX_Position3f32(256, 192, 0); GX_Normal3f32(0, 0, 1); GX_Color4u8(cid, 0, 0, 0xFF);
        GX_Position3f32(0, 192, 0);   GX_Normal3f32(0, 0, 1); GX_Color4u8(cid, 0, 0, 0xFF);
        GX_End();
        GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
        idPass = true;
#if NOODS_V130_GX
        vertexMode(indexed, true);
#endif
        // v1.111: only polygons that share an ID with a shadow matter (the ID is used to stop
        // an object shadowing itself), so only those are drawn again.
        uint64_t shadowIds = 0;
        for (int i = 0; i < polyCount && i < 2048; i++)
            if (g3.polygonsOut[i].mode == 3 && g3.polygonsOut[i].id != 0)
                shadowIds |= 1ull << (g3.polygonsOut[i].id & 63);
#ifdef GX3D_FORCE_SHADOW
        shadowIds |= 1ull << 5;
#endif
        for (int i = 0; i < polyCount && i < 2048; i++) {
            _Polygon *p = &g3.polygonsOut[i];
            if (p->alpha < 0x3F || p->textureFmt == 1 || p->textureFmt == 6 || p->mode == 3 || p->size < 3)
                continue;
            if (!((shadowIds >> (p->id & 63)) & 1))
                continue;
            submit(i);
        }
        idPass = false;
        DCInvalidateRange(s_idBuf, 256 * 192);
        DCInvalidateRange(s_zBuf, 256 * 192 * 4);
        GX_SetTexCopySrc(0, 0, 256, 192);
        GX_SetTexCopyDst(256, 192, GX_CTF_R8, GX_FALSE);
        GX_CopyTex(s_idBuf, GX_FALSE);
        GX_SetTexCopyDst(256, 192, GX_TF_Z24X8, GX_FALSE);
        GX_CopyTex(s_zBuf, GX_FALSE);
        GX_PixModeSync();
    }

    // v1.109: put the corner of the frame buffer back to the display's background, otherwise
    // the 3D image stays there and shows up on screen beside the DS screens. A second copy of
    // the same area with "clear" set does it without touching any drawing state.
    Wii_GxRestoreDisplayState();
    if (s_scratch) {
        GX_SetTexCopySrc(0, 0, 256, 192);
        GX_SetTexCopyDst(256, 192, GX_TF_I4, GX_FALSE);
        GX_CopyTex(s_scratch, GX_TRUE);
        GX_PixModeSync();
    }
#if NOODS_GX_UNLOCK_WAIT
    // The read-back and EFB restore are FIFO-ordered before this fence.
    // Only the emulation thread uses DrawDone; the display uses CopyDisp.
    // Release the command-stream mutex before sleeping on GPU completion so
    // the display can queue a 60 Hz present behind our completed transaction.
    // CPU unpack and reuse of frame-local arrays still wait for this fence.
    GX_SetDrawDone();
    Wii_GxUnlock();
    GX_WaitDrawDone();
#else
    GX_Flush();
    GX_DrawDone();
    Wii_GxUnlock();
#endif

    // Unpack the 4x4 tiles (AR pairs, then GB pairs) into the 3D line buffers.
    uint32_t *const fb = framebuffer[0];
    const uint8_t *tile = s_copyBuf;
    for (int ty = 0; ty < 192; ty += 4) {
        for (int tx = 0; tx < 256; tx += 4) {
            for (int y = 0; y < 4; y++) {
                uint32_t *row = &fb[(ty + y) * 512 + tx];
                // v1.115: one 16-bit load per pair instead of two byte loads (big-endian host)
                const uint16_t *ar = (const uint16_t*)(tile + y * 8), *gb = (const uint16_t*)(tile + 32 + y * 8);
                for (int x = 0; x < 4; x++) {
                    const uint32_t p = ar[x], q = gb[x]; // p = A:R, q = G:B
                    row[x] = BIT(26) | ((p & 0xFC00u) << 8) | ((p & 0xFCu) >> 2) |
                             ((q & 0xFC00u) >> 4) | ((q & 0xFCu) << 10);
                }
            }
            tile += 64;
        }
    }

    if (doShadows)
        gxDrawShadows();

    PPCIrqState st = PPCIrqLockByMsr();
    for (int i = 0; i < 192 * 2; i++)
        ready[i] = 3;
    PPCIrqUnlockByMsr(st);
}
