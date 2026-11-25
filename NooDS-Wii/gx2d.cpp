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

    Optional conservative NDS text-layer compositor. 
*/

#include <cstdint>
#include "gpu_2d.h"

#ifdef NOODS_V180_PROFILE
uint64_t g_gx2dTicks[8];
uint32_t g_gx2dCommands[4]; // TLUT uploads, texture loads, backdrop quads, TEV orders
#define GX2D_COMMAND(i) (++g_gx2dCommands[i])
#else
#define GX2D_COMMAND(i) do {} while(0)
#endif
#if NOODS_GX_2D_BITMAP
uint32_t g_gxBitmapStats[4];
#endif
uint32_t g_gx2dStats[8];
uint32_t g_gx2dSelfTest[2];
uint32_t g_gx2dBatchStats[2]; // vertical state runs and textured quads
#if NOODS_GX_2D
#include <gccore.h>
#include <malloc.h>
#include <cstring>
#include <cstdio>
#include <new>
#include "wii_video.h"


namespace {
Gx2DLine *rows[2];
bool pending[2][192];
bool isText(unsigned engine,unsigned y) {
#if NOODS_GX_2D_BITMAP
    return pending[engine][y] && !rows[engine][y].bitmap;
#else
    return pending[engine][y];
#endif
}
#if NOODS_GX_2D_IO >= 4
NoodsGx2D::PalettePool *palettePools[2];
#endif
const uint8_t *paletteFor(const Gx2DLine &r,unsigned bg) {
#if NOODS_GX_2D_IO >= 4
    if (r.paletteRefs[bg]) return r.paletteRefs[bg];
#endif
    return r.palettes[bg];
}
#if NOODS_GX_2D_BATCH
alignas(32) uint8_t images[4][256*192];
#endif
#if NOODS_GX_2D_BITMAP
alignas(32) uint8_t bitmapImage[256*192*2];
#endif
alignas(32) uint8_t copy[256*192*(NOODS_GX_2D_IO >= 2 ? 2 : 4)];
alignas(32) uint8_t scratch[256*192/2];
void quad(float y, float end, GXColor c) {
    GX_Begin(GX_QUADS, GX_VTXFMT2, 4);
    GX_Position3f32(0,y,0); GX_Color4u8(c.r,c.g,c.b,c.a); GX_TexCoord2f32(0,NOODS_GX_2D_BATCH ? y/192.0f : 0.125f);
    GX_Position3f32(256,y,0); GX_Color4u8(c.r,c.g,c.b,c.a); GX_TexCoord2f32(1,NOODS_GX_2D_BATCH ? y/192.0f : 0.125f);
    GX_Position3f32(256,end,0); GX_Color4u8(c.r,c.g,c.b,c.a); GX_TexCoord2f32(1,NOODS_GX_2D_BATCH ? end/192.0f : 0.125f);
    GX_Position3f32(0,end,0); GX_Color4u8(c.r,c.g,c.b,c.a); GX_TexCoord2f32(0,NOODS_GX_2D_BATCH ? end/192.0f : 0.125f);
    GX_End();
}
uint8_t expand5(unsigned c) { return (c<<3)|(c>>2); }
#if NOODS_GX_2D_IO >= 7 && NOODS_GX_2D_BATCH
#if NOODS_GX_2D_IO >= 9
void quadSlice(float y,float end,GXColor c,float top,float bottom) {
    GX_Begin(GX_QUADS,GX_VTXFMT2,4);
    GX_Position3f32(0,y,0);GX_Color4u8(c.r,c.g,c.b,c.a);GX_TexCoord2f32(0,top);
    GX_Position3f32(256,y,0);GX_Color4u8(c.r,c.g,c.b,c.a);GX_TexCoord2f32(1,top);
    GX_Position3f32(256,end,0);GX_Color4u8(c.r,c.g,c.b,c.a);GX_TexCoord2f32(1,bottom);
    GX_Position3f32(0,end,0);GX_Color4u8(c.r,c.g,c.b,c.a);GX_TexCoord2f32(0,bottom);
    GX_End();
}
#endif
void layeredRows(unsigned engine) {
#if NOODS_GX_2D_IO >= 8
    NoodsGx2D::LayerTlutSlots slots;
#else
    NoodsGx2D::TlutSlots slots;
#endif
    GXTexObj textures[4];
    unsigned boundSlot[4]={16,16,16,16},previousLayout=0;
#if NOODS_GX_2D_IO >= 9
    unsigned boundBase[4]={192,192,192,192},boundHeight[4]={};
#endif
    for (unsigned bg=0;bg<4;++bg) {
        GX_InitTexObjCI(&textures[bg],images[bg],256,192,GX_TF_CI8,GX_CLAMP,GX_CLAMP,GX_FALSE,GX_TLUT0);
        GX_InitTexObjLOD(&textures[bg],GX_NEAR,GX_NEAR,0,0,0,GX_FALSE,GX_FALSE,GX_ANISO_1);
    }
    GX_SetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
    GX_SetNumIndStages(0);
    GX_SetTevSwapModeTable(GX_TEV_SWAP0,GX_CH_RED,GX_CH_GREEN,GX_CH_BLUE,GX_CH_ALPHA);
    for (unsigned y=0;y<192;++y) if (isText(engine,y)) {
        const Gx2DLine &r=rows[engine][y];
        unsigned end=y+1;
        for (;end<192 && isText(engine,end);++end) {
            const Gx2DLine &n=rows[engine][end];
            bool same=r.enabled==n.enabled && r.backdrop==n.backdrop;
            for (unsigned bg=0;bg<4 && same;++bg) if (r.enabled&(1u<<bg))
                same=r.priority[bg]==n.priority[bg] &&
                    (paletteFor(r,bg)==paletteFor(n,bg) || !memcmp(paletteFor(r,bg),paletteFor(n,bg),512));
            if (!same) break;
        }
        ++g_gx2dBatchStats[0];
#if NOODS_GX_2D_IO >= 9
        // CI8 memory is contiguous in four-row blocks. Bind only the blocks
        // sampled by this state run, not all 192 atlas rows for a one-line draw.
        const auto slice=NoodsGx2D::atlasSlice(y,end);
#endif
#if NOODS_GX_2D_IO >= 8
        slots.begin();
#else
        unsigned pinned=0;
#endif
        for (unsigned bg=0;bg<4;++bg) if (r.enabled&(1u<<bg)) {
            bool upload;
            const uint8_t *palette=paletteFor(r,bg);
#if NOODS_GX_2D_IO >= 8
            const unsigned slot=slots.get(bg,palette,upload);
#else
            const unsigned slot=slots.get(palette,upload,pinned);
            pinned|=1u<<slot;
#endif
            if (upload) {
                DCFlushRange((void*)palette,512);
                GXTlutObj tlut;
                GX_InitTlutObj(&tlut,(void*)palette,GX_TL_RGB5A3,256);
                GX_LoadTlut(&tlut,GX_TLUT0+slot);GX2D_COMMAND(0);
            }
#if NOODS_GX_2D_IO >= 9
            if (boundSlot[bg]!=slot || boundBase[bg]!=slice.base || boundHeight[bg]!=slice.height) {
                GX_InitTexObjCI(&textures[bg],images[bg]+slice.base*256,256,slice.height,
                                GX_TF_CI8,GX_CLAMP,GX_CLAMP,GX_FALSE,GX_TLUT0+slot);
                GX_InitTexObjLOD(&textures[bg],GX_NEAR,GX_NEAR,0,0,0,GX_FALSE,GX_FALSE,GX_ANISO_1);
                boundBase[bg]=slice.base;boundHeight[bg]=slice.height;
#else
            if (boundSlot[bg]!=slot) {
                GX_InitTexObjTlut(&textures[bg],GX_TLUT0+slot);
#endif
                GX_LoadTexObj(&textures[bg],GX_TEXMAP0+bg);
                GX2D_COMMAND(1);boundSlot[bg]=slot;
            }
        }
        uint8_t order[4];
        const unsigned count=NoodsGx2D::orderedLayers(r.enabled,r.priority,order);
        unsigned layout=1;
        for (unsigned stage=0;stage<count;++stage) layout=(layout<<2)|order[stage];
        if (layout!=previousLayout) {
            GX_SetNumTevStages(count);
            for (unsigned stage=0;stage<count;++stage) {
                GX_SetTevOrder(stage,GX_TEXCOORD0,GX_TEXMAP0+order[stage],GX_COLOR0A0);
                GX2D_COMMAND(3);
                // All eligible palette alphas are exactly 0 or 255. Thus this
                // is a binary selector, not approximated DS alpha blending:
                // previous*(1-texAlpha)+texColor*texAlpha. Raster is backdrop.
                GX_SetTevColorIn(stage,stage?GX_CC_CPREV:GX_CC_RASC,GX_CC_TEXC,GX_CC_TEXA,GX_CC_ZERO);
                GX_SetTevColorOp(stage,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
                GX_SetTevAlphaIn(stage,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_RASA);
                GX_SetTevAlphaOp(stage,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
                GX_SetTevSwapMode(stage,GX_TEV_SWAP0,GX_TEV_SWAP0);
                GX_SetTevDirect(stage);
            }
            previousLayout=layout;
        }
        const unsigned c=r.backdrop;
#if NOODS_GX_2D_IO >= 9
        quadSlice(y,end,{expand5(c&31),expand5((c>>5)&31),expand5((c>>10)&31),255},
                  float(y-slice.base)/slice.height,float(end-slice.base)/slice.height);
#else
        quad(y,end,{expand5(c&31),expand5((c>>5)&31),expand5((c>>10)&31),255});
#endif
        ++g_gx2dBatchStats[1];
        y=end-1;
    }
}
#endif
#if NOODS_GX_2D_BITMAP
void bitmapRows(unsigned engine) {
    bool any=false;
    for (unsigned y=0;y<192;++y) any|=pending[engine][y] && rows[engine][y].bitmap;
    if (!any) return;
    ++g_gxBitmapStats[3];
    GXTexObj tex;
    GX_InitTexObj(&tex,bitmapImage,256,192,GX_TF_RGB5A3,GX_CLAMP,GX_CLAMP,GX_FALSE);
    GX_InitTexObjLOD(&tex,GX_NEAR,GX_NEAR,0,0,0,GX_FALSE,GX_FALSE,GX_ANISO_1);
    GX_LoadTexObj(&tex,GX_TEXMAP0);GX2D_COMMAND(1);
    GX_SetNumTevStages(1);GX_SetNumIndStages(0);GX_SetTevDirect(0);
    GX_SetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
    GX_SetTevOrder(0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0);GX2D_COMMAND(3);
    GX_SetTevSwapModeTable(GX_TEV_SWAP0,GX_CH_RED,GX_CH_GREEN,GX_CH_BLUE,GX_CH_ALPHA);
    GX_SetTevSwapModeTable(GX_TEV_SWAP1,GX_CH_BLUE,GX_CH_GREEN,GX_CH_RED,GX_CH_ALPHA);
    GX_SetTevSwapMode(0,GX_TEV_SWAP0,GX_TEV_SWAP1);
    GX_SetTevColorIn(0,GX_CC_RASC,GX_CC_TEXC,GX_CC_TEXA,GX_CC_ZERO);
    GX_SetTevColorOp(0,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
    GX_SetTevAlphaIn(0,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_RASA);
    GX_SetTevAlphaOp(0,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
    for (unsigned y=0;y<192;++y) if (pending[engine][y] && rows[engine][y].bitmap) {
        const unsigned c=rows[engine][y].backdrop;
        unsigned end=y+1;
        while (end<192 && pending[engine][end] && rows[engine][end].bitmap && rows[engine][end].backdrop==c) ++end;
        quad(y,end,{expand5(c&31),expand5((c>>5)&31),expand5((c>>10)&31),255});
        ++g_gx2dBatchStats[1];y=end-1;
    }
    // Do not leak the BGR texture swizzle into presentation or later GX 3D.
    GX_SetTevSwapMode(0,GX_TEV_SWAP0,GX_TEV_SWAP0);
}
#endif
}
static void selfTest();
void gx2dReleaseCaches() {
    for (unsigned engine = 0; engine < 2; ++engine) {
        memset(pending[engine], 0, sizeof(pending[engine]));
        free(rows[engine]); rows[engine] = nullptr;
#if NOODS_GX_2D_IO >= 4
        if (palettePools[engine]) {
            palettePools[engine]->~PalettePool();
            free(palettePools[engine]);
            palettePools[engine] = nullptr;
        }
#endif
    }
}
Gx2DLine *gx2dBegin(unsigned engine, unsigned line) {
#if GX2D_SELFTEST
    static bool tested=false;
    if (!tested) { tested=true; selfTest(); }
#endif
    if (!rows[engine]) {
        rows[engine] = (Gx2DLine*)memalign(32,sizeof(Gx2DLine)*192);
        if (!rows[engine]) { ++g_gx2dStats[7]; return nullptr; }
        memset(rows[engine],0,sizeof(Gx2DLine)*192);
    }
#if NOODS_GX_2D_IO >= 4
    for (unsigned bg=0;bg<4;++bg) rows[engine][line].paletteRefs[bg]=nullptr;
#endif
#if NOODS_GX_2D_BITMAP
    rows[engine][line].bitmap=false;
#endif
    pending[engine][line] = true;
    ++g_gx2dStats[0];
    return &rows[engine][line];
}
bool gx2dSetPalette(Gx2DLine *row,unsigned engine,unsigned bg,const uint8_t *src,bool eight) {
#if NOODS_GX_2D_IO >= 4
    if (!palettePools[engine]) {
        void *p=memalign(32,sizeof(NoodsGx2D::PalettePool));
        if (!p) {++g_gx2dStats[7];return false;}
        palettePools[engine]=new(p) NoodsGx2D::PalettePool;
    }
    row->paletteRefs[bg]=palettePools[engine]->get(src,eight);
    return row->paletteRefs[bg]!=nullptr;
#else
    (void)row;(void)engine;(void)bg;(void)src;(void)eight;return false;
#endif
}
void gx2dDiscard(unsigned engine, unsigned line) {
    if (line==0) {
        memset(pending[engine],0,sizeof(pending[engine]));
#if NOODS_GX_2D_IO >= 4
        if (palettePools[engine]) palettePools[engine]->reset();
#endif
    }
    pending[engine][line]=false;
}
void gx2dFinish(unsigned engine, uint32_t *fb) {
    bool any=false;
    for (bool p: pending[engine]) any |= p;
    if (!any) return;
    ++g_gx2dStats[2];
    GX2D_PROFILE_BEGIN();
    // Every queued payload is immutable until the end-of-transaction fence.
#if NOODS_GX_2D_IO == 0
    DCFlushRange(rows[engine],sizeof(Gx2DLine)*192);
#endif
    GX2D_PROFILE_MARK(1);
#if NOODS_GX_2D_BATCH
    // Coalesce only identical per-line state. Pixel indices remain snapshots
    // from their own scanline, even when scroll/VRAM changes within a run.
    for (unsigned y=0;y<192;++y) if (isText(engine,y))
        for (unsigned bg=0;bg<4;++bg) if (rows[engine][y].enabled&(1u<<bg))
            for (unsigned tx=0;tx<32;++tx)
                memcpy(images[bg]+((y/4)*32+tx)*32+(y%4)*8,
                       rows[engine][y].indices[bg]+tx*32,8);
#if NOODS_GX_2D_BITMAP
    bool bitmapActive=false;
    for (unsigned y=0;y<192;++y) if (pending[engine][y] && rows[engine][y].bitmap) {
        bitmapActive=true;
        for (unsigned x=0;x<256;x+=4)
            memcpy(bitmapImage+((y/4)*64+x/4)*32+(y%4)*8,rows[engine][y].indices[0]+x*2,8);
    }
#endif
    GX2D_PROFILE_MARK(2);
#if NOODS_GX_2D_IO >= 1
    unsigned active=0;
    for (unsigned y=0;y<192;++y) if (isText(engine,y)) active |= rows[engine][y].enabled;
    for (unsigned bg=0;bg<4;++bg) if (active&(1u<<bg)) DCFlushRange(images[bg],sizeof(images[bg]));
#else
    DCFlushRange(images,sizeof(images));
#endif
#endif
    DCInvalidateRange(copy,sizeof(copy));
#if NOODS_GX_2D_BITMAP
    if (bitmapActive) DCFlushRange(bitmapImage,sizeof(bitmapImage));
#endif
    GX2D_PROFILE_MARK(3);
    Wii_GxLock();
    GX2D_PROFILE_MARK(7);
    GX_SetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);
    GX_SetViewport(0,0,256,192,0,1);
    GX_SetScissor(0,0,256,192);
    Mtx44 proj; guOrtho(proj,0,192,0,256,0,1);
    Mtx ident; guMtxIdentity(ident);
    GX_LoadProjectionMtx(proj,GX_ORTHOGRAPHIC);
    GX_LoadPosMtxImm(ident,GX_PNMTX0); GX_SetCurrentMtx(GX_PNMTX0);
    GX_SetZMode(GX_FALSE,GX_ALWAYS,GX_FALSE); GX_SetZCompLoc(GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE); GX_SetClipMode(GX_CLIP_DISABLE);
    GX_SetDither(GX_FALSE); GX_SetColorUpdate(GX_TRUE); GX_SetAlphaUpdate(GX_TRUE);
    GX_SetDstAlpha(GX_DISABLE,0);
    GX_SetBlendMode(GX_BM_NONE,GX_BL_ONE,GX_BL_ZERO,GX_LO_CLEAR);
    GX_SetFog(GX_FOG_NONE,0,1,0,1,{0,0,0,0});
    Wii_GxCopyFilter(false);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0,GX_DISABLE,GX_SRC_REG,GX_SRC_VTX,GX_LIGHTNULL,GX_DF_NONE,GX_AF_NONE);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,GX_DIRECT); GX_SetVtxDesc(GX_VA_CLR0,GX_DIRECT); GX_SetVtxDesc(GX_VA_TEX0,GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT2,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
    GX_SetVtxAttrFmt(GX_VTXFMT2,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
    GX_SetVtxAttrFmt(GX_VTXFMT2,GX_VA_TEX0,GX_TEX_ST,GX_F32,0);
    GX_SetNumTexGens(1); GX_SetTexCoordGen(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY);
    GX_SetNumTevStages(1); GX_InvalidateTexAll();
#if NOODS_GX_2D_IO >= 7 && NOODS_GX_2D_BATCH
    layeredRows(engine);
#else
#if NOODS_GX_2D_IO >= 6 && NOODS_GX_2D_BATCH
    // Backdrop is independent of palette/priority state. Draw it in one pass
    // before any BG, merging equal-color contiguous pending rows only.
    GX_SetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
    GX_SetTevOrder(GX_TEVSTAGE0,GX_TEXCOORDNULL,GX_TEXMAP_NULL,GX_COLOR0A0);
    GX2D_COMMAND(3);
    GX_SetTevOp(GX_TEVSTAGE0,GX_PASSCLR);
    for (unsigned y=0;y<192;++y) if (isText(engine,y)) {
        const unsigned c=rows[engine][y].backdrop;
        unsigned end=y+1;
        while (end<192 && isText(engine,end) && rows[engine][end].backdrop==c) ++end;
        quad(y,end,{expand5(c&31),expand5((c>>5)&31),expand5((c>>10)&31),255});
        GX2D_COMMAND(2);y=end-1;
    }
    GX_SetAlphaCompare(GX_GREATER,0,GX_AOP_AND,GX_ALWAYS,0);
    GX_SetTevOp(GX_TEVSTAGE0,GX_REPLACE);
    NoodsGx2D::TlutSlots tlutSlots; // never survives another GX owner's work
    GXTexObj atlasTextures[4];
    unsigned boundSlot[4]={16,16,16,16};
    for (unsigned bg=0;bg<4;++bg) {
        GX_InitTexObjCI(&atlasTextures[bg],images[bg],256,192,GX_TF_CI8,GX_CLAMP,GX_CLAMP,GX_FALSE,GX_TLUT0);
        GX_InitTexObjLOD(&atlasTextures[bg],GX_NEAR,GX_NEAR,0,0,0,GX_FALSE,GX_FALSE,GX_ANISO_1);
    }
#endif
    for (unsigned y=0;y<192;++y) if (isText(engine,y)) {
        Gx2DLine &r=rows[engine][y];
        unsigned end=y+1;
#if NOODS_GX_2D_BATCH
        for (;end<192 && isText(engine,end);++end) {
            const Gx2DLine &next=rows[engine][end];
            bool same=r.enabled==next.enabled && r.backdrop==next.backdrop;
            for (unsigned bg=0;bg<4 && same;++bg) if (r.enabled&(1u<<bg))
                same=r.priority[bg]==next.priority[bg] &&
                    (paletteFor(r,bg)==paletteFor(next,bg) || !memcmp(paletteFor(r,bg),paletteFor(next,bg),512));
            if (!same) break;
        }
#endif
        ++g_gx2dBatchStats[0];
#if !(NOODS_GX_2D_IO >= 6 && NOODS_GX_2D_BATCH)
        const unsigned c=r.backdrop;
        GX_SetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
        GX_SetTevOrder(GX_TEVSTAGE0,GX_TEXCOORDNULL,GX_TEXMAP_NULL,GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0,GX_PASSCLR);
        quad(y,end,{expand5(c&31),expand5((c>>5)&31),expand5((c>>10)&31),255});
        GX2D_COMMAND(2);GX2D_COMMAND(3);
        GX_SetAlphaCompare(GX_GREATER,0,GX_AOP_AND,GX_ALWAYS,0);
        GX_SetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0,GX_REPLACE);
        GX2D_COMMAND(3);
#endif
        // Lower BG number wins ties; lower numeric priority is in front.
        for (int priority=3;priority>=0;--priority) for (int bg=3;bg>=0;--bg)
            if ((r.enabled&(1u<<bg)) && r.priority[bg]==priority) {
#if NOODS_GX_2D_IO >= 6 && NOODS_GX_2D_BATCH
                bool upload;
                const uint8_t *palette=paletteFor(r,bg);
                const unsigned slot=tlutSlots.get(palette,upload);
                if (upload) {
                    DCFlushRange((void*)palette,512);
                    GXTlutObj tlut;
                    GX_InitTlutObj(&tlut,(void*)palette,GX_TL_RGB5A3,256);
                    GX_LoadTlut(&tlut,GX_TLUT0+slot);GX2D_COMMAND(0);
                }
                if (boundSlot[bg]!=slot) {
                    GX_InitTexObjTlut(&atlasTextures[bg],GX_TLUT0+slot);
                    GX_LoadTexObj(&atlasTextures[bg],GX_TEXMAP0+bg);
                    GX2D_COMMAND(1);boundSlot[bg]=slot;
                }
                GX_SetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0+bg,GX_COLOR0A0);
                GX2D_COMMAND(3);
#else
                GXTexObj tex; GXTlutObj tlut;
#if NOODS_GX_2D_IO >= 1
                // Only this immutable palette and the referenced texture reach GX.
                DCFlushRange((void*)paletteFor(r,bg),512);
#if !NOODS_GX_2D_BATCH
                DCFlushRange(r.indices[bg],1024);
#endif
#endif
                GX_InitTlutObj(&tlut,(void*)paletteFor(r,bg),GX_TL_RGB5A3,256);
                GX_LoadTlut(&tlut,GX_TLUT0);GX2D_COMMAND(0);
#if NOODS_GX_2D_BATCH
                GX_InitTexObjCI(&tex,images[bg],256,192,GX_TF_CI8,GX_CLAMP,GX_CLAMP,GX_FALSE,GX_TLUT0);
#else
                GX_InitTexObjCI(&tex,r.indices[bg],256,4,GX_TF_CI8,GX_CLAMP,GX_CLAMP,GX_FALSE,GX_TLUT0);
#endif
                GX_InitTexObjLOD(&tex,GX_NEAR,GX_NEAR,0,0,0,GX_FALSE,GX_FALSE,GX_ANISO_1);
                GX_LoadTexObj(&tex,GX_TEXMAP0);GX2D_COMMAND(1);
#endif
                quad(y,end,{255,255,255,255});
                ++g_gx2dBatchStats[1];
            }
        y=end-1;
    }
#endif // single-quad layered path versus per-layer draws
#if NOODS_GX_2D_BITMAP
    bitmapRows(engine);
#endif
    GX_SetTexCopySrc(0,0,256,192); GX_SetTexCopyDst(256,192,NOODS_GX_2D_IO >= 2 ? GX_TF_RGB565 : GX_TF_RGBA8,GX_FALSE);
    GX_CopyTex(copy,GX_FALSE); GX_PixModeSync();
    Wii_GxRestoreDisplayState();
    GX_SetTexCopySrc(0,0,256,192); GX_SetTexCopyDst(256,192,GX_TF_I4,GX_FALSE);
    GX_CopyTex(scratch,GX_TRUE); GX_PixModeSync();
    GX_SetDrawDone(); Wii_GxUnlock();
    GX2D_PROFILE_MARK(4);
    GX_WaitDrawDone();
    GX2D_PROFILE_MARK(5);
#if NOODS_GX_2D_IO >= 2
    const uint16_t *tile=(const uint16_t*)copy;
    for (unsigned ty=0;ty<192;ty+=4) {
        const unsigned mask=(pending[engine][ty]?1u:0u)|(pending[engine][ty+1]?2u:0u)|
                            (pending[engine][ty+2]?4u:0u)|(pending[engine][ty+3]?8u:0u);
        for (unsigned tx=0;tx<256;tx+=4,tile+=16)
            for (unsigned y=0;y<4;++y) if (mask&(1u<<y)) {
                uint32_t *dst=fb+(ty+y)*256+tx;
                for (unsigned x=0;x<4;++x) {
                    const uint32_t c=NoodsGx2D::unpack565(tile[y*4+x]);
#if NOODS_GX_2D_VERIFY
                    ++g_gx2dStats[3]; g_gx2dStats[4] += c != dst[x];
#if NOODS_GX_2D_BITMAP
                    if (rows[engine][ty+y].bitmap) {++g_gxBitmapStats[1];g_gxBitmapStats[2]+=c!=dst[x];}
#endif
#else
                    dst[x]=c;
#endif
                }
            }
    }
#else
    for (unsigned y=0;y<192;++y) if (pending[engine][y]) {
        for (unsigned x=0;x<256;++x) {
            const unsigned t=((y/4)*64+x/4)*64+(y%4)*8+(x%4)*2;
            // Recover original 5-bit channels, then use DS 5->6 (not GX bit replication).
            const uint32_t c=((copy[t+1]>>3)<<1)|((copy[t+32]>>3)<<7)|((copy[t+33]>>3)<<13);
#if NOODS_GX_2D_VERIFY
#if GX2D_DUMP_FIRST && NOODS_GX_2D_BATCH
            if (c != fb[y*256+x] && !g_gx2dStats[4]) {
                FILE *f=fopen("sd:/gx2d-first.bin","wb");
                if (f) {
                    uint32_t header[]={g_gx2dStats[2],engine,y,x,(uint32_t)sizeof(Gx2DLine)};
                    fwrite(header,4,5,f); fwrite(fb,4,256*192,f);
                    fwrite(copy,1,sizeof(copy),f); fwrite(images,1,sizeof(images),f);
                    fwrite(rows[engine],1,sizeof(Gx2DLine)*192,f); fclose(f);
                }
            }
#endif
            ++g_gx2dStats[3]; g_gx2dStats[4] += c != fb[y*256+x];
#else
            fb[y*256+x]=c;
#endif
        }
    }
#endif
    for (unsigned y=0;y<192;++y) pending[engine][y]=false;
    GX2D_PROFILE_MARK(6);
}
#if GX2D_SELFTEST && NOODS_GX_2D_VERIFY
static void selfTest() {
    // Exercise the actual CI/TLUT draw, alpha discard, priority ordering and
    // RGB8 readback with independent scalar composition. No guest state used.
    static uint32_t expected[256*192];
    const uint32_t before=g_gx2dStats[4],beforeCompared=g_gx2dStats[3];
    for (unsigned phase=0;phase<3;++phase) {
    for (unsigned y=0;y<192;++y) {
        const unsigned sy=phase==1 ? y/16 : y;
        // Constant backdrop with independently changing palettes/priorities,
        // interrupted by software-only rows; also drives TLUT eviction/reuse.
        if (phase==2 && y%13==0) {gx2dDiscard(1,y);continue;}
        Gx2DLine *r=gx2dBegin(1,y);
        if (!r) return;
        r->enabled=(sy%15)+1;
        r->backdrop=phase==2 ? 0x294A : (sy*173)&32767;
        for (unsigned bg=0;bg<4;++bg) {
            uint8_t source[512];
            r->priority[bg]=((sy>>(bg%3))+bg)%4;
            for (unsigned k=0;k<256;++k) {
                const unsigned c=(sy*307+bg*8191+k*131)&32767;
                source[k*2]=c; source[k*2+1]=c>>8;
                const bool transparent=(sy&1)?k==0:(k%16)==0;
                const uint16_t v=NoodsGx::paletteRgb5A3(c,transparent);
                r->palettes[bg][k*2]=v>>8; r->palettes[bg][k*2+1]=v;
            }
#if NOODS_GX_2D_IO >= 4
            gx2dSetPalette(r,1,bg,source,sy&1);
#endif
            for (unsigned x=0;x<256;++x)
                r->indices[bg][(x/8)*32+x%8]=(x*29+y*7+bg*16)&255;
        }
        for (unsigned x=0;x<256;++x) {
            unsigned chosen=r->backdrop, best=4;
            // Reverse BG order means the smaller BG wins a priority tie.
            for (int bg=3;bg>=0;--bg) if (r->enabled&(1u<<bg)) {
                const unsigned k=(x*29+y*7+bg*16)&255;
                const bool transparent=(sy&1)?k==0:(k%16)==0;
                if (!transparent && r->priority[bg]<=best) {
                    chosen=(sy*307+bg*8191+k*131)&32767; best=r->priority[bg];
                }
            }
            expected[y*256+x]=((chosen&31)<<1)|((chosen&0x3E0)<<2)|((chosen&0x7C00)<<3);
        }
    }
    gx2dFinish(1,expected);
#if NOODS_GX_2D_IO >= 4
    if (palettePools[1]) palettePools[1]->reset(); // fence completed above
#endif
    }
    g_gx2dSelfTest[0]=g_gx2dStats[3]-beforeCompared;
    g_gx2dSelfTest[1]=g_gx2dStats[4]-before;
}
#else
static void selfTest() {}
#endif

#else
Gx2DLine *gx2dBegin(unsigned,unsigned) { return nullptr; }
bool gx2dSetPalette(Gx2DLine*,unsigned,unsigned,const uint8_t*,bool) {return false;}
void gx2dDiscard(unsigned,unsigned) {}
void gx2dFinish(unsigned,uint32_t*) {}
void gx2dReleaseCaches() {}
#endif

