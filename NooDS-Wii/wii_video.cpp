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

#include "wii_video.h"
#include <gccore.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>

extern "C" {
    #include <tuxedo/thread.h>
    #include <tuxedo/ppc/intrinsics.h>
}

extern float g_cursorX;
extern float g_cursorY;
extern bool  g_cursorShow;

#define FIFO_SIZE (256 * 1024)

static WiiVideoSystem  wiiVid;
static WiiDebugOverlay wiiDbg;
static void*           gp_fifo = nullptr;
static Mtx44           perspective;
static Mtx             modelView;
static GXRModeObj*     rmode   = nullptr;

static void* hw_mem2_align(size_t size, u32 alignment) {
    u8* lo = (u8*)SYS_GetArena2Lo();
    u8* hi = (u8*)SYS_GetArena2Hi();
    uptr aligned = ((uptr)lo + alignment - 1) & ~(uptr)(alignment - 1);
    u8*  newLo   = (u8*)aligned + size;
    if (newLo > hi) return nullptr;
    SYS_SetArena2Lo(newLo);
    return (void*)aligned;
}

static const u8 debugFont[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // '!'
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // '#'
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // '$'
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // '%'
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // '&'
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // '\''
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // '('
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // ','
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // '.'
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // '/'
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // '0'
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // '1'
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // '2'
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // '3'
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // '4'
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // '5'
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // '6'
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // '7'
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // '8'
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // '9'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // ':'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // ';'
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // '<'
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // '='
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // '>'
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // '?'
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // '@'
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // 'A'
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // 'B'
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // 'C'
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // 'D'
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // 'E'
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // 'F'
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // 'G'
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // 'H'
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 'I'
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // 'J'
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // 'K'
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // 'L'
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // 'M'
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // 'N'
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // 'O'
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // 'P'
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // 'Q'
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // 'R'
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // 'S'
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 'T'
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // 'U'
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 'V'
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 'W'
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // 'X'
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // 'Y'
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // 'Z'
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // '['
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // '\\'
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // ']'
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // '_'
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // '`'
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // 'a'
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // 'b'
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // 'c'
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, // 'd'
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, // 'e'
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00}, // 'f'
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // 'g'
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // 'h'
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // 'i'
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // 'j'
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // 'k'
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 'l'
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // 'm'
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // 'n'
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // 'o'
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // 'p'
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // 'q'
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // 'r'
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // 's'
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // 't'
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // 'u'
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 'v'
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // 'w'
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // 'x'
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // 'y'
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // 'z'
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // '{'
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // '|'
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // '}'
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // '~'
};

#define FONT_CHAR_W  8
#define FONT_CHAR_H  8
#define OVERLAY_COLS (DEBUG_OVERLAY_WIDTH  / FONT_CHAR_W)
#define OVERLAY_ROWS (DEBUG_OVERLAY_HEIGHT / FONT_CHAR_H)

static uint32_t overlayStaging[DEBUG_OVERLAY_WIDTH * DEBUG_OVERLAY_HEIGHT];
static bool overlayRowDirty[OVERLAY_ROWS];

static void DrawCharToStaging(int col, int row, char c,
                               uint32_t fgColor, uint32_t bgColor) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const u8* glyph = debugFont[c - 0x20];

    for (int py = 0; py < FONT_CHAR_H; py++) {
        u8 bits = glyph[py];
        for (int px = 0; px < FONT_CHAR_W; px++) {
            bool lit = (bits >> px) & 1;
            int idx  = (row * FONT_CHAR_H + py) * DEBUG_OVERLAY_WIDTH
                     + (col * FONT_CHAR_W + px);
            overlayStaging[idx] = lit ? fgColor : bgColor;
        }
    }
}

static void TileImageRGBA8_fromABGR(const uint32_t* src, u8* dest,
                                     int width, int height) {
    const int tilesX = width  >> 2;
    const int tilesY = height >> 2;

    for (int ty = 0; ty < tilesY; ty++) {
        for (int tx = 0; tx < tilesX; tx++) {
            u8* tile = dest + ((ty * tilesX + tx) << 6);
            u8* ar   = tile;
            u8* gb   = tile + 32;

            for (int py = 0; py < 4; py++) {
                const uint32_t* row = src + (ty * 4 + py) * width + tx * 4;
                for (int px = 0; px < 4; px++) {
                    uint32_t p = row[px];
                    u8 r = (u8)( p        & 0xFF);
                    u8 g = (u8)((p >>  8) & 0xFF);
                    u8 b = (u8)((p >> 16) & 0xFF);
                    u8 a = (u8)((p >> 24) & 0xFF);

                    int idx      = (py << 2) + px;
                    ar[idx * 2 + 0] = a;
                    ar[idx * 2 + 1] = r;
                    gb[idx * 2 + 0] = g;
                    gb[idx * 2 + 1] = b;
                }
            }
        }
    }
}

void Wii_VideoInit() {
    VIDEO_Init();
    PAD_Init();

    rmode = VIDEO_GetPreferredMode(nullptr);

    wiiVid.xfbList[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    wiiVid.xfbList[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    wiiVid.currentXfb = 0;

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(wiiVid.xfbList[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    VIDEO_WaitVSync();

    gp_fifo = hw_mem2_align(FIFO_SIZE, 32);
    if (!gp_fifo) gp_fifo = memalign(32, FIFO_SIZE);
    memset(gp_fifo, 0, FIFO_SIZE);
    GX_Init(gp_fifo, FIFO_SIZE);

    GXColor background = { 0x18, 0x18, 0x18, 0xFF };
    GX_SetCopyClear(background, GX_MAX_Z24);

    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    f32 yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
    GX_SetDispCopyYScale(yscale);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, rmode->xfbHeight);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering,
        ((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));

    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetClipMode(GX_CLIP_ENABLE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);

    guOrtho(perspective, 0, rmode->efbHeight, 0, rmode->fbWidth, 0, 300);
    GX_LoadProjectionMtx(perspective, GX_ORTHOGRAPHIC);

    guMtxIdentity(modelView);
    guMtxTransApply(modelView, modelView, 0.0f, 0.0f, -100.0f);
    GX_LoadPosMtxImm(modelView, GX_PNMTX0);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);

    const size_t ndsBufSize = NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 4;
    wiiVid.texDataTop = (u8*)hw_mem2_align(ndsBufSize, 32);
    wiiVid.texDataBottom = (u8*)hw_mem2_align(ndsBufSize, 32);
    if (!wiiVid.texDataTop)    wiiVid.texDataTop    = (u8*)memalign(32, ndsBufSize);
    if (!wiiVid.texDataBottom) wiiVid.texDataBottom = (u8*)memalign(32, ndsBufSize);

    memset(wiiVid.texDataTop,    0, ndsBufSize);
    memset(wiiVid.texDataBottom, 0, ndsBufSize);
    DCFlushRange(wiiVid.texDataTop,    ndsBufSize);
    DCFlushRange(wiiVid.texDataBottom, ndsBufSize);

    GX_InitTexObj(&wiiVid.texObjTop,
                  wiiVid.texDataTop,
                  NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT,
                  GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObj(&wiiVid.texObjBottom,
                  wiiVid.texDataBottom,
                  NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT,
                  GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);

    GX_InitTexObjFilterMode(&wiiVid.texObjTop,    GX_NEAR, GX_NEAR);
    GX_InitTexObjFilterMode(&wiiVid.texObjBottom, GX_NEAR, GX_NEAR);

    wiiVid.lastGbaMode = false;

    Wii_DebugOverlayInit();
}

void Wii_DebugOverlayInit() {
    const size_t bufSize = DEBUG_OVERLAY_WIDTH * DEBUG_OVERLAY_HEIGHT * 4;
    wiiDbg.texData = (u8*)hw_mem2_align(bufSize, 32);
    if (!wiiDbg.texData) wiiDbg.texData = (u8*)memalign(32, bufSize);

    memset(wiiDbg.texData,   0, bufSize);
    memset(overlayStaging,   0, sizeof(overlayStaging));
    memset(overlayRowDirty,  0, sizeof(overlayRowDirty));
    DCFlushRange(wiiDbg.texData, bufSize);

    GX_InitTexObj(&wiiDbg.texObj,
                  wiiDbg.texData,
                  DEBUG_OVERLAY_WIDTH, DEBUG_OVERLAY_HEIGHT,
                  GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&wiiDbg.texObj, GX_NEAR, GX_NEAR);

    wiiDbg.enabled = true;
}

void Wii_DebugOverlaySetEnabled(bool enabled) {
    wiiDbg.enabled = enabled;
}

void Wii_DebugOverlayPrint(int line, const char* fmt, ...) {
    if (!wiiDbg.enabled || line < 0 || line >= OVERLAY_ROWS) return;

    char buf[OVERLAY_COLS + 1];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    int len = (int)strlen(buf);
    while (len < OVERLAY_COLS) buf[len++] = ' ';
    buf[OVERLAY_COLS] = '\0';

    const uint32_t fg = 0xFFFFFFFF;
    const uint32_t bg = 0xC0000000;

    for (int col = 0; col < OVERLAY_COLS; col++)
        DrawCharToStaging(col, line, buf[col], fg, bg);

    overlayRowDirty[line] = true;
}

static void Wii_DebugOverlayFlush() {
    if (!wiiDbg.enabled || !wiiDbg.texData) return;

    bool anyDirty = false;
    for (int row = 0; row < OVERLAY_ROWS; row++) {
        if (!overlayRowDirty[row]) continue;
        anyDirty = true;

        int py0 = row * FONT_CHAR_H;
        int py1 = py0 + FONT_CHAR_H;

        int tileRow0 = py0 >> 2;
        int tileRow1 = (py1 + 3) >> 2;

        for (int ty = tileRow0; ty < tileRow1; ty++) {
            for (int tx = 0; tx < (DEBUG_OVERLAY_WIDTH >> 2); tx++) {
                u8* tile = wiiDbg.texData + ((ty * (DEBUG_OVERLAY_WIDTH >> 2) + tx) << 6);
                u8* ar   = tile;
                u8* gb   = tile + 32;

                for (int tpy = 0; tpy < 4; tpy++) {
                    int srcY = ty * 4 + tpy;
                    if (srcY >= DEBUG_OVERLAY_HEIGHT) break;
                    const uint32_t* srcRow = overlayStaging
                        + srcY * DEBUG_OVERLAY_WIDTH + tx * 4;

                    for (int tpx = 0; tpx < 4; tpx++) {
                        uint32_t p = srcRow[tpx];
                        u8 a = (u8)((p >> 24) & 0xFF);
                        u8 r = (u8)((p >> 16) & 0xFF);
                        u8 g = (u8)((p >>  8) & 0xFF);
                        u8 b = (u8)( p        & 0xFF);

                        int idx         = (tpy << 2) + tpx;
                        ar[idx * 2 + 0] = a;
                        ar[idx * 2 + 1] = r;
                        gb[idx * 2 + 0] = g;
                        gb[idx * 2 + 1] = b;
                    }
                }
            }
        }

        int byteOffset = tileRow0 * (DEBUG_OVERLAY_WIDTH >> 2) * 64;
        int byteCount  = (tileRow1 - tileRow0) * (DEBUG_OVERLAY_WIDTH >> 2) * 64;
        DCFlushRange(wiiDbg.texData + byteOffset, byteCount);

        overlayRowDirty[row] = false;
    }
}

static void DrawScreenQuad(GXTexObj* texObj,
                            f32 x1, f32 y1, f32 x2, f32 y2,
                            bool isOverlay) {
    GX_LoadTexObj(texObj, GX_TEXMAP0);

    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0,
                   GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
                   GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);

    if (isOverlay) {
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    } else {
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    }

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position3f32(x1, y1, 0.0f); GX_Color4u8(0xFF,0xFF,0xFF,0xFF); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(x2, y1, 0.0f); GX_Color4u8(0xFF,0xFF,0xFF,0xFF); GX_TexCoord2f32(1.0f, 0.0f);
        GX_Position3f32(x2, y2, 0.0f); GX_Color4u8(0xFF,0xFF,0xFF,0xFF); GX_TexCoord2f32(1.0f, 1.0f);
        GX_Position3f32(x1, y2, 0.0f); GX_Color4u8(0xFF,0xFF,0xFF,0xFF); GX_TexCoord2f32(0.0f, 1.0f);
    GX_End();
}

void Wii_VideoRender(const uint32_t* srcTop, const uint32_t* srcBottom, bool gbaMode) {
    const int scrW = gbaMode ? GBA_SCREEN_WIDTH  : NDS_SCREEN_WIDTH;
    const int scrH = gbaMode ? GBA_SCREEN_HEIGHT : NDS_SCREEN_HEIGHT;
    const size_t bufSize = (size_t)scrW * scrH * 4;

    if (gbaMode != wiiVid.lastGbaMode) {
        GX_InitTexObj(&wiiVid.texObjTop,
                      wiiVid.texDataTop,
                      scrW, scrH,
                      GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&wiiVid.texObjTop, GX_NEAR, GX_NEAR);

        if (!gbaMode) {
            GX_InitTexObj(&wiiVid.texObjBottom,
                          wiiVid.texDataBottom,
                          scrW, scrH,
                          GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
            GX_InitTexObjFilterMode(&wiiVid.texObjBottom, GX_NEAR, GX_NEAR);
        }

        wiiVid.lastGbaMode = gbaMode;
    }

    if (srcTop) {
        TileImageRGBA8_fromABGR(srcTop, wiiVid.texDataTop, scrW, scrH);
        DCFlushRange(wiiVid.texDataTop, bufSize);
    }
    if (srcBottom && !gbaMode) {
        TileImageRGBA8_fromABGR(srcBottom, wiiVid.texDataBottom, scrW, scrH);
        DCFlushRange(wiiVid.texDataBottom, bufSize);
    }

    Wii_DebugOverlayFlush();

    GX_InvalidateTexAll();
    GX_LoadPosMtxImm(modelView, GX_PNMTX0);
    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);

    if (gbaMode) {
        const int scale   = 2;
        const int gbaW    = scrW * scale;
        const int gbaH    = scrH * scale;
        const int originX = ((int)rmode->fbWidth  - gbaW) / 2;
        const int originY = ((int)rmode->efbHeight - gbaH) / 2;

        DrawScreenQuad(&wiiVid.texObjTop,
                       (f32)originX,        (f32)originY,
                       (f32)(originX + gbaW), (f32)(originY + gbaH),
                       false);
    }
    else {
        const int gap     = 2;
        const int totalW  = scrW;
        const int totalH  = scrH * 2 + gap;
        const int originX = ((int)rmode->fbWidth  - totalW) / 2;
        const int originY = ((int)rmode->efbHeight - totalH) / 2;

        const f32 x1    = (f32)originX;
        const f32 x2    = (f32)(originX + scrW);
        const f32 topY1 = (f32)originY;
        const f32 topY2 = (f32)(originY + scrH);
        const f32 botY1 = (f32)(originY + scrH + gap);
        const f32 botY2 = (f32)(originY + scrH + gap + scrH);

        DrawScreenQuad(&wiiVid.texObjTop,    x1, topY1, x2, topY2, false);
        DrawScreenQuad(&wiiVid.texObjBottom, x1, botY1, x2, botY2, false);
    }

    if (!gbaMode && g_cursorShow) {
        Wii_DrawCursor(g_cursorX, g_cursorY);
    }

    if (wiiDbg.enabled && wiiDbg.texData) {
        DrawScreenQuad(&wiiDbg.texObj,
                       0.0f, 0.0f,
                       (f32)DEBUG_OVERLAY_WIDTH,
                       (f32)DEBUG_OVERLAY_HEIGHT,
                       true);
    }
}

void Wii_DrawCursor(float x, float y) {
    if (x < 0.0f || x > rmode->fbWidth  ||
        y < 0.0f || y > rmode->efbHeight) return;

    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0,
                   GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
                   GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    
    const float size = 3.0f;
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position3f32(x - size, y - size, 0.0f); GX_Color4u8(0x00, 0xFF, 0xFF, 0xE0); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(x + size, y - size, 0.0f); GX_Color4u8(0x00, 0xFF, 0xFF, 0xE0); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(x + size, y + size, 0.0f); GX_Color4u8(0x00, 0xFF, 0xFF, 0xE0); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(x - size, y + size, 0.0f); GX_Color4u8(0x00, 0xFF, 0xFF, 0xE0); GX_TexCoord2f32(0.0f, 0.0f);
    GX_End();
}

void Wii_VideoFlushAsync() {
    GX_DrawDone();
    wiiVid.currentXfb ^= 1;
    GX_CopyDisp(wiiVid.xfbList[wiiVid.currentXfb], GX_TRUE);
    GX_Flush();
    VIDEO_SetNextFramebuffer(wiiVid.xfbList[wiiVid.currentXfb]);
    VIDEO_Flush();
}
