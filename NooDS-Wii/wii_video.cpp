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
#include "settings.h"
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
static char overlayLastText[OVERLAY_ROWS][OVERLAY_COLS + 1];


static void DrawCharToStaging(int col, int row, char c,
                               uint32_t fgColor, uint32_t bgColor) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const u8* glyph = debugFont[c - 0x20];

    for (int py = 0; py < FONT_CHAR_H; py++) {
        u8 bits = glyph[py];
        uint32_t* dst = overlayStaging + (row * FONT_CHAR_H + py) * DEBUG_OVERLAY_WIDTH
                      + (col * FONT_CHAR_W);
        dst[0] = (bits & 0x01) ? fgColor : bgColor;
        dst[1] = (bits & 0x02) ? fgColor : bgColor;
        dst[2] = (bits & 0x04) ? fgColor : bgColor;
        dst[3] = (bits & 0x08) ? fgColor : bgColor;
        dst[4] = (bits & 0x10) ? fgColor : bgColor;
        dst[5] = (bits & 0x20) ? fgColor : bgColor;
        dst[6] = (bits & 0x40) ? fgColor : bgColor;
        dst[7] = (bits & 0x80) ? fgColor : bgColor;
    }
}

static void TileImageRGBA8_fromABGR(const uint32_t* __restrict__ src,
                                     u8* __restrict__ dest,
                                     int width, int height) {
    const int tilesX = width  >> 2;
    const int tilesY = height >> 2;

    for (int ty = 0; ty < tilesY; ty++) {
        const uint32_t* row0 = src + (ty * 4 + 0) * width;
        const uint32_t* row1 = row0 + width;
        const uint32_t* row2 = row1 + width;
        const uint32_t* row3 = row2 + width;
        u32* tilePtr = (u32*)(dest + ((ty * tilesX) << 6));

        for (int tx = 0; tx < tilesX; tx++, tilePtr += 16) {
            const int x4 = tx << 2;
            #define PACK_ROW(ROW, BASE) do { \
                const uint32_t p0 = (ROW)[x4 + 0]; \
                const uint32_t p1 = (ROW)[x4 + 1]; \
                const uint32_t p2 = (ROW)[x4 + 2]; \
                const uint32_t p3 = (ROW)[x4 + 3]; \
                tilePtr[(BASE) + 0] = ((p0 & 0xFF000000u) | ((p0 & 0xFFu) << 16)) \
                                    | (((p1 & 0xFF000000u) >> 16) | (p1 & 0xFFu)); \
                tilePtr[(BASE) + 1] = ((p2 & 0xFF000000u) | ((p2 & 0xFFu) << 16)) \
                                    | (((p3 & 0xFF000000u) >> 16) | (p3 & 0xFFu)); \
                tilePtr[(BASE) + 8] = (((p0 & 0x0000FF00u) << 16) | (p0 & 0x00FF0000u)) \
                                    | ((p1 & 0x0000FF00u) | ((p1 & 0x00FF0000u) >> 16)); \
                tilePtr[(BASE) + 9] = (((p2 & 0x0000FF00u) << 16) | (p2 & 0x00FF0000u)) \
                                    | ((p3 & 0x0000FF00u) | ((p3 & 0x00FF0000u) >> 16)); \
            } while (0)
            PACK_ROW(row0, 0);
            PACK_ROW(row1, 2);
            PACK_ROW(row2, 4);
            PACK_ROW(row3, 6);
            #undef PACK_ROW
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
    memset(overlayLastText,  0, sizeof(overlayLastText));
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

bool Wii_DebugOverlayHasDirty() {
    if (!wiiDbg.enabled) return false;
    for (int row = 0; row < OVERLAY_ROWS; row++) {
        if (overlayRowDirty[row]) return true;
    }
    return false;
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

    if (memcmp(buf, overlayLastText[line], OVERLAY_COLS) == 0)
        return;
    memcpy(overlayLastText[line], buf, OVERLAY_COLS + 1);

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

    if (anyDirty) {
        static u8 s_overlayTexGen = 0;
        wiiDbg.texData[0] = (u8)(0xC0u | (++s_overlayTexGen & 1u));
        DCFlushRange(wiiDbg.texData, 32);
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

static struct {
    f32  left, top, right, bottom;
    bool visible;
} s_lastTouchRect = { 192.0f, 241.0f, 448.0f, 433.0f, true };

bool Wii_GetTouchScreenRect(float* outLeft, float* outTop,
                            float* outRight, float* outBottom) {
    if (outLeft)   *outLeft   = s_lastTouchRect.left;
    if (outTop)    *outTop    = s_lastTouchRect.top;
    if (outRight)  *outRight  = s_lastTouchRect.right;
    if (outBottom) *outBottom = s_lastTouchRect.bottom;
    return s_lastTouchRect.visible;
}

struct WiiScreenRects {
    f32  topX1, topY1, topX2, topY2;
    bool topVis;
    f32  botX1, botY1, botX2, botY2;
    bool botVis;
};

static void Wii_ComputeScreenRects(int fbW, int fbH, bool gbaMode, WiiScreenRects* out) {
    static const float kSizeScales[10] = {
        0.0f,  // 0: Auto
        0.50f, // 1: 0.50x
        0.75f, // 2: 0.75x
        1.00f, // 3: 1.00x
        1.25f, // 4: 1.25x
        1.50f, // 5: 1.50x
        1.75f, // 6: 1.75x
        2.00f, // 7: 2.00x
        2.50f, // 8: 2.50x
        -1.0f  // 9: Off
    };
    static const int kGapPixels[5] = { 0, 2, 8, 16, 32 };

    int s1     = (Settings::screen1Size  >= 0 && Settings::screen1Size  <= 9) ? Settings::screen1Size  : 0;
    int s2     = (Settings::screen2Size  >= 0 && Settings::screen2Size  <= 9) ? Settings::screen2Size  : 0;
    int layout = (Settings::screenLayout >= 0 && Settings::screenLayout <= 5) ? Settings::screenLayout : 0;
    int swap   = (Settings::screenSwap != 0) ? 1 : 0;
    int gap    = kGapPixels[(Settings::wiiScreenGap >= 0 && Settings::wiiScreenGap <= 4) ? Settings::wiiScreenGap : 1];

    memset(out, 0, sizeof(*out));

    if (gbaMode) {
        if (s1 == 9) return;
        float sc = (s1 == 0) ? 2.0f : kSizeScales[s1];
        float w  = (float)GBA_SCREEN_WIDTH  * sc;
        float h  = (float)GBA_SCREEN_HEIGHT * sc;
        float fit = 1.0f;
        if (w > (float)fbW) fit = (float)fbW / w;
        if (h * fit > (float)fbH) fit = (float)fbH / h;
        w *= fit;
        h *= fit;
        out->topX1  = ((float)fbW - w) * 0.5f;
        out->topY1  = ((float)fbH - h) * 0.5f;
        out->topX2  = out->topX1 + w;
        out->topY2  = out->topY1 + h;
        out->topVis = true;
        return;
    }

    bool vis1 = (s1 != 9) && (layout != 5);
    bool vis2 = (s2 != 9) && (layout != 4);
    if (!vis1 && !vis2) {
        vis1 = true;
        s1   = 3; // 1.00x fallback so the screen never goes completely black
    }

    // Single-screen mode (either layout = Scr1/Scr2 Only, or the other screen is Off).
    if (vis1 != vis2) {
        bool onlyTop = vis1;
        int  sOpt    = onlyTop ? s1 : s2;
        if (swap) {
            onlyTop = !onlyTop;
            sOpt    = onlyTop ? s1 : s2;
            if (sOpt == 9) sOpt = 0;
        }
        float sc = (sOpt == 0) ? 2.50f : kSizeScales[sOpt];
        float w  = (float)NDS_SCREEN_WIDTH  * sc;
        float h  = (float)NDS_SCREEN_HEIGHT * sc;
        float fit = 1.0f;
        if (w > (float)fbW) fit = (float)fbW / w;
        if (h * fit > (float)fbH) fit = (float)fbH / h;
        w *= fit;
        h *= fit;
        f32 x1 = ((float)fbW - w) * 0.5f;
        f32 y1 = ((float)fbH - h) * 0.5f;
        if (onlyTop) {
            out->topX1 = x1; out->topY1 = y1; out->topX2 = x1 + w; out->topY2 = y1 + h;
            out->topVis = true;
        } else {
            out->botX1 = x1; out->botY1 = y1; out->botX2 = x1 + w; out->botY2 = y1 + h;
            out->botVis = true;
        }
        return;
    }

    // Dual-screen modes: compute desired scales for Screen 1 (Top) and Screen 2 (Bottom).
    float auto1 = 1.0f, auto2 = 1.0f;
    if (layout == 1) { // Side-by-Side
        auto1 = auto2 = ((float)fbW - (float)gap) / (2.0f * (float)NDS_SCREEN_WIDTH);
    } else if (layout == 2) { // Hybrid (Big Screen 1 on Left, Compact Screen 2 on Right)
        auto1 = 1.75f;
        auto2 = 0.70f;
    } else if (layout == 3) { // Hybrid (Big Screen 2 on Left, Compact Screen 1 on Right)
        auto1 = 0.70f;
        auto2 = 1.75f;
    }

    float sc1 = (s1 == 0) ? auto1 : kSizeScales[s1];
    float sc2 = (s2 == 0) ? auto2 : kSizeScales[s2];

    float w1 = (float)NDS_SCREEN_WIDTH  * sc1;
    float h1 = (float)NDS_SCREEN_HEIGHT * sc1;
    float w2 = (float)NDS_SCREEN_WIDTH  * sc2;
    float h2 = (float)NDS_SCREEN_HEIGHT * sc2;

    // Determine slot order: in HybridBot, Screen 2 starts on the left unless swapped.
    bool firstIsTop = (layout == 3) ? (swap != 0) : (swap == 0);
    float wA = firstIsTop ? w1 : w2;
    float hA = firstIsTop ? h1 : h2;
    float wB = firstIsTop ? w2 : w1;
    float hB = firstIsTop ? h2 : h1;

    float fit = 1.0f;
    if (layout == 0) { // Vertical stack (A over B)
        float maxW = (wA > wB) ? wA : wB;
        float totH = hA + (float)gap + hB;
        if (maxW > (float)fbW) fit = (float)fbW / maxW;
        if (totH * fit > (float)fbH) {
            float availH = (float)fbH - (float)gap;
            if (availH < 32.0f) availH = 32.0f;
            float f2 = availH / (hA + hB);
            if (f2 < fit) fit = f2;
        }
        wA *= fit; hA *= fit; wB *= fit; hB *= fit;
        float startY = ((float)fbH - (hA + (float)gap + hB)) * 0.5f;
        f32 xA = ((float)fbW - wA) * 0.5f, yA = startY;
        f32 xB = ((float)fbW - wB) * 0.5f, yB = startY + hA + (float)gap;
        if (firstIsTop) {
            out->topX1 = xA; out->topY1 = yA; out->topX2 = xA + wA; out->topY2 = yA + hA;
            out->botX1 = xB; out->botY1 = yB; out->botX2 = xB + wB; out->botY2 = yB + hB;
        } else {
            out->botX1 = xA; out->botY1 = yA; out->botX2 = xA + wA; out->botY2 = yA + hA;
            out->topX1 = xB; out->topY1 = yB; out->topX2 = xB + wB; out->topY2 = yB + hB;
        }
    } else { // Horizontal arrangement (A left of B)
        float maxH = (hA > hB) ? hA : hB;
        float totW = wA + (float)gap + wB;
        if (maxH > (float)fbH) fit = (float)fbH / maxH;
        if (totW * fit > (float)fbW) {
            float availW = (float)fbW - (float)gap;
            if (availW < 32.0f) availW = 32.0f;
            float f2 = availW / (wA + wB);
            if (f2 < fit) fit = f2;
        }
        wA *= fit; hA *= fit; wB *= fit; hB *= fit;
        float startX = ((float)fbW - (wA + (float)gap + wB)) * 0.5f;
        f32 xA = startX,                   yA = ((float)fbH - hA) * 0.5f;
        f32 xB = startX + wA + (float)gap, yB = ((float)fbH - hB) * 0.5f;
        if (firstIsTop) {
            out->topX1 = xA; out->topY1 = yA; out->topX2 = xA + wA; out->topY2 = yA + hA;
            out->botX1 = xB; out->botY1 = yB; out->botX2 = xB + wB; out->botY2 = yB + hB;
        } else {
            out->botX1 = xA; out->botY1 = yA; out->botX2 = xA + wA; out->botY2 = yA + hA;
            out->topX1 = xB; out->topY1 = yB; out->topX2 = xB + wB; out->topY2 = yB + hB;
        }
    }
    out->topVis = true;
    out->botVis = true;
}

void Wii_VideoRender(const uint32_t* srcTop, const uint32_t* srcBottom,
                     bool gbaMode, bool uploadTextures) {
    const int scrW = gbaMode ? GBA_SCREEN_WIDTH  : NDS_SCREEN_WIDTH;
    const int scrH = gbaMode ? GBA_SCREEN_HEIGHT : NDS_SCREEN_HEIGHT;
    const size_t bufSize = (size_t)scrW * scrH * 4;
    const u8 filterMode = (Settings::screenFilter != 0) ? GX_LINEAR : GX_NEAR;
    static u8 s_lastFilterMode = 0xFF;

    if (gbaMode != wiiVid.lastGbaMode || filterMode != s_lastFilterMode) {
        GX_InitTexObj(&wiiVid.texObjTop,
                      wiiVid.texDataTop,
                      scrW, scrH,
                      GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&wiiVid.texObjTop, filterMode, filterMode);

        if (!gbaMode) {
            GX_InitTexObj(&wiiVid.texObjBottom,
                          wiiVid.texDataBottom,
                          scrW, scrH,
                          GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
            GX_InitTexObjFilterMode(&wiiVid.texObjBottom, filterMode, filterMode);
        }

        if (gbaMode != wiiVid.lastGbaMode)
            uploadTextures = true;
        wiiVid.lastGbaMode = gbaMode;
        s_lastFilterMode   = filterMode;
        GX_InvalidateTexAll();
    }

    static u8 s_screenTexGen = 0;
    bool anyTexUpdated = false;
    if (uploadTextures && srcTop) {
        TileImageRGBA8_fromABGR(srcTop, wiiVid.texDataTop, scrW, scrH);
        wiiVid.texDataTop[0] = ++s_screenTexGen;
        DCFlushRange(wiiVid.texDataTop, bufSize);
        anyTexUpdated = true;
    }
    if (uploadTextures && srcBottom && !gbaMode) {
        TileImageRGBA8_fromABGR(srcBottom, wiiVid.texDataBottom, scrW, scrH);
        wiiVid.texDataBottom[0] = ++s_screenTexGen;
        DCFlushRange(wiiVid.texDataBottom, bufSize);
        anyTexUpdated = true;
    }

    if (Wii_DebugOverlayHasDirty()) {
        Wii_DebugOverlayFlush();
        anyTexUpdated = true;
    }

    if (anyTexUpdated)
        GX_InvalidateTexAll();
    GX_LoadPosMtxImm(modelView, GX_PNMTX0);
    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);

    WiiScreenRects rects;
    Wii_ComputeScreenRects((int)rmode->fbWidth, (int)rmode->efbHeight, gbaMode, &rects);

    s_lastTouchRect.left    = rects.botX1;
    s_lastTouchRect.top     = rects.botY1;
    s_lastTouchRect.right   = rects.botX2;
    s_lastTouchRect.bottom  = rects.botY2;
    s_lastTouchRect.visible = (!gbaMode && rects.botVis);

    if (rects.topVis)
        DrawScreenQuad(&wiiVid.texObjTop, rects.topX1, rects.topY1, rects.topX2, rects.topY2, false);
    if (!gbaMode && rects.botVis)
        DrawScreenQuad(&wiiVid.texObjBottom, rects.botX1, rects.botY1, rects.botX2, rects.botY2, false);

    if (wiiDbg.enabled && wiiDbg.texData) {
        DrawScreenQuad(&wiiDbg.texObj,
                       0.0f, 0.0f,
                       (f32)DEBUG_OVERLAY_WIDTH,
                       (f32)DEBUG_OVERLAY_HEIGHT,
                       true);
    }

    if (!gbaMode && g_cursorShow) {
        Wii_DrawCursor(g_cursorX, g_cursorY);
    }

    // 2x2 near-black vertex-colored corner pixel so host presenters with duplicate-XFB
    // filtering always count true 60 Hz VSync frames during static/frameskip hold frames
    // with zero texture uploads or cache invalidations.
    static u8 s_vsyncBeat = 0;
    const u8 c  = (++s_vsyncBeat & 1u) ? 0x04 : 0x00;
    const f32 bx = (f32)(rmode->fbWidth  - 2);
    const f32 by = (f32)(rmode->efbHeight - 2);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0,
                   GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
                   GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position3f32(bx,        by,        0.0f); GX_Color4u8(c,c,c,0xFF); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(bx + 2.0f, by,        0.0f); GX_Color4u8(c,c,c,0xFF); GX_TexCoord2f32(1.0f, 0.0f);
        GX_Position3f32(bx + 2.0f, by + 2.0f, 0.0f); GX_Color4u8(c,c,c,0xFF); GX_TexCoord2f32(1.0f, 1.0f);
        GX_Position3f32(bx,        by + 2.0f, 0.0f); GX_Color4u8(c,c,c,0xFF); GX_TexCoord2f32(0.0f, 1.0f);
    GX_End();
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
    wiiVid.currentXfb ^= 1;
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(wiiVid.xfbList[wiiVid.currentXfb], GX_TRUE);
    GX_Flush();
    VIDEO_SetNextFramebuffer(wiiVid.xfbList[wiiVid.currentXfb]);
    VIDEO_Flush();
}
