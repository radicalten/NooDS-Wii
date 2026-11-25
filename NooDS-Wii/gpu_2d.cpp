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

#ifndef NOODS_GBA_AFFINE_WINDOW
#define NOODS_GBA_AFFINE_WINDOW 0
#endif
#ifndef NOODS_GBA_AFFINE_VERIFY
#define NOODS_GBA_AFFINE_VERIFY 0
#endif
#ifndef NOODS_GBA_OBJ_MASK
#define NOODS_GBA_OBJ_MASK 0
#endif

#ifndef NOODS_OBJ_WORD_READS
#define NOODS_OBJ_WORD_READS 0
#endif
#ifndef NOODS_DS_OBJ_FAST
#define NOODS_DS_OBJ_FAST 0
#endif
#ifndef NOODS_DS_OBJ_VERIFY
#define NOODS_DS_OBJ_VERIFY 0
#endif
#ifndef NOODS_SOLO_TEXT
#define NOODS_SOLO_TEXT 0
#endif
#ifndef NOODS_SOLO_TEXT_VERIFY
#define NOODS_SOLO_TEXT_VERIFY 0
#endif
#ifndef NOODS_SOLID_FADE
#define NOODS_SOLID_FADE 1
#endif
#ifndef NOODS_OBJ_MASK
#define NOODS_OBJ_MASK 1
#endif
#ifndef NOODS_AFFINE_FAST
#define NOODS_AFFINE_FAST 1
#endif
#include <cstring>
#include "core.h"




#include "profile.h"

uint32_t g_gbaAffineStats[3]; // verified lines, mismatched lines, bypassed window checks in lines
#if NOODS_GBA_AFFINE_VERIFY
static bool s_affineReference = false;
#endif
uint32_t g_dsObjStats[8]; // 4bpp, 8bpp, affine, window, dual passes, wrong fields, extended, reserved
uint32_t g_soloTextStats[6]; // calls, 4bpp, 8bpp, raw, pixel mismatches, raw mismatches

Gpu2D::Gpu2D(Core *core, bool engine): core(core), engine(engine) {
    // Set up 2D GPU engine A or B
    bgVramAddr = 0x6000000 + engine * 0x200000;
    objVramAddr = 0x6400000 + engine * 0x200000;
    palette = core->memory.palette + engine * 0x400;
    oam = core->memory.oam + engine * 0x400;
    extPalettes = engine ? core->memory.engBExtPal : core->memory.engAExtPal;
}

void Gpu2D::saveState(FILE *file) {
    // Write state data to the file
    fwrite(winHFlip, sizeof(bool), sizeof(winHFlip) / sizeof(bool), file);
    fwrite(winVFlag, sizeof(bool), sizeof(winVFlag) / sizeof(bool), file);
    fwrite(&dispCnt, sizeof(dispCnt), 1, file);
    fwrite(bgCnt, 2, sizeof(bgCnt) / 2, file);
    fwrite(bgHOfs, 2, sizeof(bgHOfs) / 2, file);
    fwrite(bgVOfs, 2, sizeof(bgVOfs) / 2, file);
    fwrite(bgPA, 2, sizeof(bgPA) / 2, file);
    fwrite(bgPB, 2, sizeof(bgPB) / 2, file);
    fwrite(bgPC, 2, sizeof(bgPC) / 2, file);
    fwrite(bgPD, 2, sizeof(bgPD) / 2, file);
    fwrite(bgX, 4, sizeof(bgX) / 4, file);
    fwrite(bgY, 4, sizeof(bgY) / 4, file);
    fwrite(winX1, 2, sizeof(winX1) / 2, file);
    fwrite(winX2, 2, sizeof(winX2) / 2, file);
    fwrite(winY1, 2, sizeof(winY1) / 2, file);
    fwrite(winY2, 2, sizeof(winY2) / 2, file);
    fwrite(&winIn, sizeof(winIn), 1, file);
    fwrite(&winOut, sizeof(winOut), 1, file);
    fwrite(&bldCnt, sizeof(bldCnt), 1, file);
    fwrite(&mosaic, sizeof(mosaic), 1, file);
    fwrite(&bldAlpha, sizeof(bldAlpha), 1, file);
    fwrite(&bldY, sizeof(bldY), 1, file);
    fwrite(&masterBright, sizeof(masterBright), 1, file);
}

void Gpu2D::loadState(FILE *file) {
#if NOODS_GX_2D
    gx2dDiscard(engine,0); // queued snapshots do not survive a state restore
#endif
    // Read state data from the file
    fread(winHFlip, sizeof(bool), sizeof(winHFlip) / sizeof(bool), file);
    fread(winVFlag, sizeof(bool), sizeof(winVFlag) / sizeof(bool), file);
    fread(&dispCnt, sizeof(dispCnt), 1, file);
    fread(bgCnt, 2, sizeof(bgCnt) / 2, file);
    fread(bgHOfs, 2, sizeof(bgHOfs) / 2, file);
    fread(bgVOfs, 2, sizeof(bgVOfs) / 2, file);
    fread(bgPA, 2, sizeof(bgPA) / 2, file);
    fread(bgPB, 2, sizeof(bgPB) / 2, file);
    fread(bgPC, 2, sizeof(bgPC) / 2, file);
    fread(bgPD, 2, sizeof(bgPD) / 2, file);
    fread(bgX, 4, sizeof(bgX) / 4, file);
    fread(bgY, 4, sizeof(bgY) / 4, file);
    fread(winX1, 2, sizeof(winX1) / 2, file);
    fread(winX2, 2, sizeof(winX2) / 2, file);
    fread(winY1, 2, sizeof(winY1) / 2, file);
    fread(winY2, 2, sizeof(winY2) / 2, file);
    fread(&winIn, sizeof(winIn), 1, file);
    fread(&winOut, sizeof(winOut), 1, file);
    fread(&bldCnt, sizeof(bldCnt), 1, file);
    fread(&mosaic, sizeof(mosaic), 1, file);
    fread(&bldAlpha, sizeof(bldAlpha), 1, file);
    fread(&bldY, sizeof(bldY), 1, file);
    fread(&masterBright, sizeof(masterBright), 1, file);
}

namespace {
struct Gpu2DLuts {
    uint32_t rgb5To6[32768];
    Gpu2DLuts() {
        for (uint32_t c = 0; c < 32768; c++) {
            uint8_t r = ((c << 1) & 0x3E);
            uint8_t g = ((c >> 4) & 0x3E);
            uint8_t b = ((c >> 9) & 0x3E);
            rgb5To6[c] = ((uint32_t)b << 12) | ((uint32_t)g << 6) | r;
        }
    }
};
static const Gpu2DLuts s_gpu2DLuts;
} // namespace

// v1.80: shared object dimension table for the per-scanline active list.
static const uint8_t s_objectSizes[32] = {
    8, 8, 16, 16, 32, 32, 64, 64, // Square
    16, 8, 32, 8, 32, 16, 64, 32, // Horizontal
    8, 16, 8, 32, 16, 32, 32, 64, // Vertical
    0, 0, 0, 0, 0, 0, 0, 0       // Prohibited
};

uint32_t Gpu2D::rgb5ToRgb6(uint32_t color) {
    return (color & 0xFFFC0000u) | s_gpu2DLuts.rgb5To6[color & 0x7FFFu];
}

// v1.72: brightness LUTs for master-bright (avoid per-pixel div)
uint8_t Gpu2D::s_brightIncTable[64][17];
uint8_t Gpu2D::s_brightDecTable[64][17];
bool Gpu2D::s_brightTablesReady = false;
uint8_t Gpu2D::s_brightIncTable5[32][17];
uint8_t Gpu2D::s_brightDecTable5[32][17];
void Gpu2D::ensureBrightTables() {
    if (s_brightTablesReady) return;
    for (int c=0;c<64;c++) for(int f=0;f<17;f++) {
        s_brightIncTable[c][f] = c + (63 - c) * f / 16;
        s_brightDecTable[c][f] = c - c * f / 16;
    }
    for (int c=0;c<32;c++) for(int f=0;f<17;f++) {
        s_brightIncTable5[c][f] = c + (31 - c) * f / 16;
        s_brightDecTable5[c][f] = c - c * f / 16;
    }
    s_brightTablesReady = true;
}
// v1.73: alpha blend LUT for 6-bit (eva 0..16, evb=16-eva)
uint8_t Gpu2D::s_alphaLUT[17][4096];
bool Gpu2D::s_alphaLUTReady = false;
void Gpu2D::ensureAlphaLUT() {
    if (s_alphaLUTReady) return;
    for (int eva=0; eva<17; eva++) {
        int evb = 16 - eva;
        for (int src=0; src<64; src++) for (int dst=0; dst<64; dst++) {
            int idx = (src<<6) | dst;
            s_alphaLUT[eva][idx] = (src*eva + dst*evb)/16;
            if (s_alphaLUT[eva][idx] > 63) s_alphaLUT[eva][idx]=63;
        }
    }
    s_alphaLUTReady = true;
}
FORCE_INLINE uint8_t Gpu2D::alphaBlend6(uint8_t src, uint8_t dst, uint8_t eva) {
    if (eva>16) eva=16;
    return s_alphaLUT[eva][(src<<6)|dst];
}
FORCE_INLINE uint16_t Gpu2D::fastRead16(uint32_t addr) {
    uint8_t *p = core->memory.readMap9A[addr >> 12];
    if (__builtin_expect(p != nullptr, 1)) {
        uint16_t v; __builtin_memcpy(&v, p + (addr & 0xFFF), 2);
#ifdef ENDIAN_BIG
        return __builtin_bswap16(v);
#else
        return v;
#endif
    }
    return core->memory.read<uint16_t>(0, addr);
}
FORCE_INLINE uint32_t Gpu2D::fastRead32(uint32_t addr) {
    uint8_t *p = core->memory.readMap9A[addr >> 12];
    if (__builtin_expect(p != nullptr, 1)) {
        uint32_t v; __builtin_memcpy(&v, p + (addr & 0xFFF), 4);
#ifdef ENDIAN_BIG
        return __builtin_bswap32(v);
#else
        return v;
#endif
    }
    return core->memory.read<uint32_t>(0, addr);
}
template<bool GBA> FORCE_INLINE uint16_t Gpu2D::fastRead16T(uint32_t addr) {
    if constexpr(GBA) return core->memory.read<uint16_t>(true, addr);
    else return fastRead16(addr);
}
template<bool GBA> FORCE_INLINE uint32_t Gpu2D::fastRead32T(uint32_t addr) {
    if constexpr(GBA) return core->memory.read<uint32_t>(true, addr);
    else return fastRead32(addr);
}
template<bool GBA> FORCE_INLINE uint8_t Gpu2D::fastRead8T(uint32_t addr) {
    if constexpr(GBA) return core->memory.read<uint8_t>(true, addr);
    else return fastRead8(addr);
}
FORCE_INLINE uint8_t Gpu2D::fastRead8(uint32_t addr) {
    uint8_t *p = core->memory.readMap9A[addr >> 12];
    if (__builtin_expect(p != nullptr, 1)) {
        return *(p + (addr & 0xFFF));
    }
    return core->memory.read<uint8_t>(0, addr);
}
void Gpu2D::buildWindowMask(int line) {
    V180_PROFILE_SCOPE(19);
    if (!(dispCnt & 0xE000)) {
        // fast: all enabled (include blend bit 5)
        memset(winMaskCache, 0x3F, sizeof(winMaskCache));
        winMaskValid = true;
        return;
    }
    // v1.106: same classification (window 0, then window 1, then object window, then outside)
    // filled by ranges instead of evaluated per pixel.
    {
        const uint32_t dc = dispCnt;
        const uint8_t out = winOut & 0x3F;
        if (dc & BIT(15)) {
            const uint8_t outObj = (winOut >> 8) & 0x3F;
            const uint32_t *const fb = &framebuffer[line * 256];
            for (int x = 0; x < 256; x++)
                winMaskCache[x] = (fb[x] & BIT(24)) ? outObj : out;
        } else {
            memset(winMaskCache, out, 256);
        }
        for (int w = 1; w >= 0; w--) {
            if (!(dc & BIT(13 + w)) || !winVFlag[w]) continue;
            const uint8_t in = (w ? (winIn >> 8) : winIn) & 0x3F;
            // inside when (x >= x1 && x < x2) != flip
            int x1 = winX1[w], x2 = winX2[w];
            if (x1 < 0) x1 = 0;
            if (x2 > 256) x2 = 256;
            const bool empty = (x1 >= x2);
            if (!winHFlip[w]) {
                if (!empty) memset(&winMaskCache[x1], in, (size_t)(x2 - x1));
            } else if (empty) {
                memset(winMaskCache, in, 256);
            } else {
                if (x1 > 0) memset(winMaskCache, in, (size_t)x1);
                if (x2 < 256) memset(&winMaskCache[x2], in, (size_t)(256 - x2));
            }
        }
    }
    winMaskValid = true;
}

void Gpu2D::reloadRegisters() {
    // Reload internal registers at the start of a frame
    internalX[0] = bgX[0];
    internalX[1] = bgX[1];
    internalY[0] = bgY[0];
    internalY[1] = bgY[1];
}

void Gpu2D::updateWindows(int line) {
    // Enable or disable vertical window areas at the set scanlines
    for (int i = 0; i < 2; i++) {
        if (line == winY2[i])
            winVFlag[i] = false;
        else if (line == winY1[i])
            winVFlag[i] = true;
    }
}

int Gpu2D::buildObjectLine(int line) {
    V180_PROFILE_SCOPE(20);
    // v1.80: parse and cull OAM once per scanline. The renderer no longer
    // walks 128 entries twice or recomputes geometry for both object passes.
    int count = 0;
    for (int i = 0; i < 128; i++) {
        uint8_t byte = oam[i * 8 + 1];
        uint8_t type = (byte >> 2) & 0x3;
        if ((byte & 0x3) == 2)
            continue;

        const uint16_t object[3] = {
            U8TO16(oam, i * 8 + 0), U8TO16(oam, i * 8 + 2), U8TO16(oam, i * 8 + 4)
        };
        const uint8_t *size = &s_objectSizes[((object[0] >> 11) & 0x18) | ((object[1] >> 13) & 0x6)];
        const uint8_t width = size[0];
        const uint8_t height = size[1];
        const bool shift = ((object[0] & 0x300) == 0x300);
        const uint8_t width2 = width << shift;
        const uint8_t height2 = height << shift;

        int y = object[0] & 0xFF;
        if (y >= 192) y -= 256;
        const int spriteY = ((object[0] & BIT(12)) ?
            (line - (line % (((mosaic >> 12) & 0xF) + 1))) : line) - y;
        if (spriteY < 0 || spriteY >= height2)
            continue;

        int x = object[1] & 0x1FF;
        if (x >= 256) x -= 512;

        ObjectLine &dst = objectLine[count++];
        dst.object[0] = object[0];
        dst.object[1] = object[1];
        dst.object[2] = object[2];
        dst.x = (int16_t)x;
        dst.spriteY = (int16_t)spriteY;
        dst.width = width;
        dst.height = height;
        dst.width2 = width2;
        dst.height2 = height2;
        dst.priority = (int8_t)(((object[2] >> 10) & 0x3) - 1);
        dst.type = type;
    }
    return count;
}

// v1.123: a DS frame scans OAM for every line of both screens: 128 entries, 384 times. Games
// write OAM once a frame, so the answer to "which entries can touch this line" is kept as a
// bit mask per line and rebuilt only after OAM was written (Memory::oamGen; OAM is smaller
// than a page, so every write to it goes through Memory's fallback path). The mask is a
// superset: each selected entry still goes through the same tests as before, in OAM order.
void Gpu2D::buildObjectMask() {
    memset(objMask, 0, sizeof(objMask));
    for (int i = 0; i < 128; i++) {
        const uint8_t byte = oam[i * 8 + 1];
        if ((byte & 0x3) == 2)
            continue;
        const uint16_t obj0 = U8TO16(oam, i * 8 + 0), obj1 = U8TO16(oam, i * 8 + 2);
        int from = 0, to = 192;
        if (!(obj0 & BIT(12))) { // a mosaic sprite's row depends on the MOSAIC register: every line
            const uint8_t *size = &s_objectSizes[((obj0 >> 11) & 0x18) | ((obj1 >> 13) & 0x6)];
            const int height2 = size[1] << ((obj0 & 0x300) == 0x300);
            int y = obj0 & 0xFF;
            if (y >= 192) y -= 256;
            from = (y < 0) ? 0 : y;
            to = (y + height2 > 192) ? 192 : y + height2;
        }
        const uint32_t bit = 1u << (i & 31);
        for (int l = from; l < to; l++) objMask[l][i >> 5] |= bit;
    }
}

int Gpu2D::buildObjectLineDs(int line) {
    if (!NOODS_OBJ_MASK || (unsigned)line >= 192u) return buildObjectLine(line);
    if (objMaskGen != core->memory.oamGen) {
        objMaskGen = core->memory.oamGen;
        buildObjectMask();
    }
    int count = 0;
    for (int w = 0; w < 4; w++) {
      for (uint32_t m = objMask[line][w]; m; m &= m - 1) {
        const int i = w * 32 + __builtin_ctz(m);
        uint8_t byte = oam[i * 8 + 1];
        uint8_t type = (byte >> 2) & 0x3;
        if ((byte & 0x3) == 2)
            continue;

        const uint16_t object[3] = {
            U8TO16(oam, i * 8 + 0), U8TO16(oam, i * 8 + 2), U8TO16(oam, i * 8 + 4)
        };
        const uint8_t *size = &s_objectSizes[((object[0] >> 11) & 0x18) | ((object[1] >> 13) & 0x6)];
        const uint8_t width = size[0];
        const uint8_t height = size[1];
        const bool shift = ((object[0] & 0x300) == 0x300);
        const uint8_t width2 = width << shift;
        const uint8_t height2 = height << shift;

        int y = object[0] & 0xFF;
        if (y >= 192) y -= 256;
        const int spriteY = ((object[0] & BIT(12)) ?
            (line - (line % (((mosaic >> 12) & 0xF) + 1))) : line) - y;
        if (spriteY < 0 || spriteY >= height2)
            continue;

        int x = object[1] & 0x1FF;
        if (x >= 256) x -= 512;

        ObjectLine &dst = objectLine[count++];
        dst.object[0] = object[0];
        dst.object[1] = object[1];
        dst.object[2] = object[2];
        dst.x = (int16_t)x;
        dst.spriteY = (int16_t)spriteY;
        dst.width = width;
        dst.height = height;
        dst.width2 = width2;
        dst.height2 = height2;
        dst.priority = (int8_t)(((object[2] >> 10) & 0x3) - 1);
        dst.type = type;
      }
    }
    return count;
}

void Gpu2D::drawGbaScanline(int line) {
    l1Needed = true;
    V180_PROFILE_SCOPE(1);
    V180_PROFILE_INC(13);
    // Clear layers with the backdrop (first palette index)
    uint32_t backdrop = U8TO16(palette, 0) & ~BIT(15);
    // v1.114: brightness effect with BLDY = 0 is a no-op (see drawScanline)
    const uint8_t rawBldMode = (bldCnt >> 6) & 0x3;
    const uint8_t bldMode = (rawBldMode >= 2 && bldY == 0) ? 0 : rawBldMode;

    // Fast path when no BG or OBJ layers are enabled on this scanline
    if ((dispCnt & 0x1F00) == 0 && bldMode == 0) {
        uint32_t *dst = &framebuffer[line * 256];
        for (int i = 0; i < 240; i++) dst[i] = backdrop;
        return;
    }

    const bool needBlendLayers = (bldMode != 0) || ((dispCnt & BIT(12)) != 0);
    for (int i = 0; i < 240; i++) layers[0][i] = backdrop;
    memset(priorities, 4, sizeof(priorities));
    if (needBlendLayers) {
        memcpy(layers[1], layers[0], 240 * sizeof(uint32_t));
        memset(blendBits, 5, sizeof(blendBits));
    }

    // v1.80: build the active-object list once, then classify windows once
    // after the object-window pass (so OBJ-window bits are current).
    winMaskValid = false;
    if (dispCnt & BIT(12)) {
        objectLineCount = NOODS_GBA_OBJ_MASK ? buildObjectLineDs(line) : buildObjectLine(line);
        if (dispCnt & BIT(15)) drawObjects<1>(line, true);
        if (dispCnt & 0xE000) buildWindowMask(line);
        drawObjects<1>(line, false);
    }
    else {
        objectLineCount = 0;
        if (dispCnt & 0xE000) buildWindowMask(line);
    }

    // Draw the background layers depending on the BG mode
    switch (uint8_t mode = (dispCnt & 0x7)) {
    case 0:
        if (dispCnt & BIT(11)) drawText<1>(3, line);
        if (dispCnt & BIT(10)) drawText<1>(2, line);
        if (dispCnt & BIT(9)) drawText<1>(1, line);
        if (dispCnt & BIT(8)) drawText<1>(0, line);
        break;

    case 1:
        if (dispCnt & BIT(10)) drawAffine<1>(2, line);
        if (dispCnt & BIT(9)) drawText<1>(1, line);
        if (dispCnt & BIT(8)) drawText<1>(0, line);
        break;

    case 2:
        if (dispCnt & BIT(11)) drawAffine<1>(3, line);
        if (dispCnt & BIT(10)) drawAffine<1>(2, line);
        break;

    case 3: case 4: case 5:
        if (dispCnt & BIT(10)) drawExtendedGba(2, line);
        break;

    default:
        LOG_CRIT("Unknown GBA BG mode: %d\n", mode);
        break;
    }

    // Fast path when mode == 0 and no semi-transparent sprite pixels exist on the line
    if (bldMode == 0) {
        if (!needBlendLayers) {
            memcpy(&framebuffer[line * 256], layers[0], 240 * sizeof(uint32_t));
            return;
        }
        uint32_t orBits = 0;
        for (int i = 0; i < 240; i++)
            orBits |= layers[0][i];
        if (!(orBits & BIT(25))) {
            memcpy(&framebuffer[line * 256], layers[0], 240 * sizeof(uint32_t));
            return;
        }
    }

    // v1.80: winMaskCache was built once for this line before BG rendering.
    // Rebuilding it here would repeat 256 window classifications per line.
    // Blend the layers to form the final image
    // v1.106: same decisions per pixel as before, with the registers read once per line and the
    // alpha blend done on two packed channel groups (red+blue, green) instead of three
    // separate channels. Each channel sum is at most 31 * 16 + 31 * 16 = 992 < 1024, so the
    // groups cannot carry into each other and the results are identical.
    {
        V180_PROFILE_SCOPE(22);
        const uint32_t bc = bldCnt;
        const int mode = (bc >> 6) & 0x3;
        const bool winOn = (dispCnt & 0xE000) != 0;
        const uint32_t eva = std::min((bldAlpha >> 0) & 0x1F, 16);
        const uint32_t evb = std::min((bldAlpha >> 8) & 0x1F, 16);
        const uint32_t by = bldY;
        if (mode >= 2 && by) ensureBrightTables();
        uint32_t *const l0 = layers[0];
        const uint32_t *const l1 = layers[1];
        const int8_t *const bb0 = blendBits[0], *const bb1 = blendBits[1];
        for (int i = 0; i < 240; i++) {
            const uint32_t top = l0[i];
            bool alpha = false;
            if (top & BIT(25)) { // Semi-transparent pixel
                // Force alpha blending if possible, otherwise allow brightness blending or do nothing
                if (bc & BIT(8 + bb1[i])) // Below pixel is blendable
                    alpha = true;
                else if (mode < 2 || !(bc & BIT(bb0[i])))
                    continue;
            }
            else if (mode == 0 || !(bc & BIT(bb0[i])) || (mode == 1 && !(bc & BIT(8 + bb1[i])))) {
                // Do nothing if blending is disabled or not possible
                continue;
            }

            if (!alpha) {
                // Skip blending if the pixel is in the bounds of a window that has it disabled
                if (winOn && !(winMaskCache[i] & BIT(5))) continue;
                alpha = (mode == 1);
            }

            if (alpha) {
                const uint32_t below = l1[i];
                uint32_t rb = ((top & 0x7C1Fu) * eva + (below & 0x7C1Fu) * evb) >> 4;
                uint32_t g = ((top & 0x03E0u) * eva + (below & 0x03E0u) * evb) >> 4;
                uint32_t r = rb & 0x3Fu, b = (rb >> 10) & 0x3Fu;
                g = (g >> 5) & 0x3Fu;
                if (r > 31u) r = 31u;
                if (g > 31u) g = 31u;
                if (b > 31u) b = 31u;
                l0[i] = (b << 10) | (g << 5) | r;
            }
            else if (by) {
                if (mode == 2) { // Brightness increase
                    uint8_t r = s_brightIncTable5[(top>>0)&0x1F][by];
                    uint8_t g = s_brightIncTable5[(top>>5)&0x1F][by];
                    uint8_t b = s_brightIncTable5[(top>>10)&0x1F][by];
                    l0[i] = (b<<10)|(g<<5)|r;
                } else {         // Brightness decrease
                    uint8_t r = s_brightDecTable5[(top>>0)&0x1F][by];
                    uint8_t g = s_brightDecTable5[(top>>5)&0x1F][by];
                    uint8_t b = s_brightDecTable5[(top>>10)&0x1F][by];
                    l0[i] = (b<<10)|(g<<5)|r;
                }
            }
        }
    }

    // Copy the final image to the framebuffer
    memcpy(&framebuffer[line * 256], layers[0], 240 * sizeof(uint32_t));
}

// DS BG mode 0, one text layer, no objects/windows/effective blending.
// Compose straight into the visible line; transparency selects the backdrop.
bool Gpu2D::soloTextLine(int bg, int line, uint32_t backdrop, bool raw) {
    const uint16_t cnt = bgCnt[bg];
    if ((cnt & BIT(6)) && (mosaic & 15)) return false;
    const int hofs = bgHOfs[bg];
    const int y = (((cnt & BIT(6)) ? line - line % (((mosaic >> 4) & 15) + 1) : line) + bgVOfs[bg]) & 511;
    uint32_t map = bgVramAddr + ((dispCnt >> 11) & 0x70000) + ((cnt << 3) & 0x0F800) + ((y & 0xF8) << 3);
    if (y >= 256 && (cnt & BIT(15))) map += (cnt & BIT(14)) ? 0x1000 : 0x800;
    const uint32_t data = bgVramAddr + ((dispCnt >> 8) & 0x70000) + ((cnt << 12) & 0x3C000);
    const bool eight = (cnt & BIT(7)) != 0;
    uint8_t *const ext = (eight && (dispCnt & BIT(30))) ?
        extPalettes[bg + ((bg < 2 && (cnt & BIT(13))) ? 2 : 0)] : nullptr;
    const uint32_t back = rgb5ToRgb6(backdrop);
    uint32_t *const out = &framebuffer[line * 256];
    const int width = 256 + (hofs & 7);
    for (int i = 0; i < width; i += 8) {
        const int xo = (i + hofs) & 511;
        const uint16_t tile = fastRead16(map + ((xo & 0xF8) >> 2) + ((xo >= 256 && (cnt & BIT(14))) ? 0x800 : 0));
        const int row = (tile & BIT(11)) ? 7 - (y & 7) : (y & 7);
        uint8_t *const pal = eight ? (ext ? ext + ((tile >> 3) & 0x1E00) : palette) : palette + ((tile & 0xF000) >> 7);
        const uint32_t address = data + (tile & 0x3FF) * (eight ? 64 : 32) + row * (eight ? 8 : 4);
        const uint32_t low = fastRead32(address);
        const uint32_t high = eight ? fastRead32(address + 4) : 0;
        const int origin = i - (xo & 7);
        const int first = origin < 0 ? -origin : 0;
        const int last = origin + 8 > 256 ? 256 - origin : 8;
        for (int j = first; j < last; ++j) {
            const int lane = (tile & BIT(10)) ? 7 - j : j;
            const uint32_t index = eight ? (((lane < 4 ? low : high) >> ((lane & 3) * 8)) & 255) : ((low >> (lane * 4)) & 15);
            out[origin + j] = index ? rgb5ToRgb6(U8TO16(pal, index * 2)) : back;
        }
    }
    if (raw) memcpy(layers[0], out, 256 * sizeof(uint32_t));
    ++g_soloTextStats[0]; ++g_soloTextStats[eight ? 2 : 1];
    if (raw) ++g_soloTextStats[3];
    return true;
}

void Gpu2D::finishGx2D() {
#if NOODS_GX_2D
    V180_PROFILE_SCOPE(0); // include GX submission, wait and readback in renderer cost
    gx2dFinish(engine, framebuffer);
#endif
}

bool Gpu2D::queueGxBitmapLine(int line) {
#if NOODS_GX_2D_BITMAP
    if (!NoodsGx2D::eligibleBitmap(engine,dispCnt,bgCnt[3],bldCnt,bldY,masterBright)) return false;
    GX2D_PROFILE_QUEUE();
    Gx2DLine *r=gx2dBegin(engine,line);
    if (!r) return false;
    r->bitmap=true;r->enabled=0;r->backdrop=U8TO16(palette,0)&0x7FFF;
    const uint16_t cnt=bgCnt[3];
    static const unsigned sizes[4][2]={{128,128},{256,256},{512,256},{512,512}};
    const unsigned width=sizes[cnt>>14][0],height=sizes[cnt>>14][1];
    const uint32_t base=bgVramAddr+((cnt<<6)&0x7C000u);
    const bool wrap=cnt&BIT(13);
    const int pa=bgPA[1],pc=bgPC[1];
    int xx=internalX[1],yy=internalY[1];
    bool packed=false;
    const int sx0=xx>>8,sy0=yy>>8;
    if (pa==256 && pc==0 && sx0>=0 && (unsigned)sx0+256<=width &&
        sy0>=0 && (unsigned)sy0<height && !(sx0&1)) {
        uint32_t addr=base+(sy0*width+sx0)*2;
        uint8_t **map=core->memory.readMap9A;
        if (map[addr>>12] && map[(addr+511)>>12]) {
            uint8_t *dst=r->indices[0];
            unsigned remaining=512;
            while (remaining) {
                const unsigned bytes=std::min(4096u-(addr&4095u),remaining);
                const uint8_t *src=map[addr>>12]+(addr&4095u);
                for (unsigned i=0;i<bytes;i+=4) {
                    uint32_t raw;memcpy(&raw,src+i,4);
#ifdef ENDIAN_BIG
                    raw=__builtin_bswap32(raw);
#endif
                    NoodsGx2D::storeBe32(dst+i,NoodsGx2D::bitmapPair(raw));
                }
                addr+=bytes;dst+=bytes;remaining-=bytes;
            }
            packed=true;
        }
    }
    if (!packed) for (unsigned x=0;x<256;++x,xx+=pa,yy+=pc) {
        int sx=xx>>8,sy=yy>>8;
        if (wrap) {sx&=width-1;sy&=height-1;}
        uint16_t pixel=0;
        if ((unsigned)sx<width && (unsigned)sy<height)
            pixel=NoodsGx2D::bitmapPixel(fastRead16(base+(sy*width+sx)*2));
        r->indices[0][x*2]=pixel>>8;r->indices[0][x*2+1]=pixel;
    }
    ++g_gxBitmapStats[0];
    return true;
#else
    (void)line;return false;
#endif
}

bool Gpu2D::queueGxTextLine(int line) {
#if NOODS_GX_2D
    // Snapshot a strictly ordinary text-composition line before guest execution
    // can change VRAM, palettes or scroll registers. Engine A never defers,
    // avoiding capture enable changes between drawing and the capture phase.
    if (!NoodsGx2D::eligible(engine,dispCnt,bldCnt,masterBright)) return false;
    for (int bg=0;bg<4;++bg)
        if ((dispCnt & BIT(8+bg)) && (bgCnt[bg] & BIT(6))) return false;
    GX2D_PROFILE_QUEUE();
    Gx2DLine *r=gx2dBegin(engine,line);
    if (!r) return false;
    r->enabled=(dispCnt>>8)&15;
    r->backdrop=U8TO16(palette,0)&0x7FFF;
    for (int bg=0;bg<4;++bg) if (r->enabled & BIT(bg)) {
        ++g_gx2dStats[1];
        const bool eight=bgCnt[bg]&BIT(7);
        ++g_gx2dStats[eight ? 6 : 5];
        r->priority[bg]=bgCnt[bg]&3;
#if NOODS_GX_2D_IO >= 4
        if (!gx2dSetPalette(r,engine,bg,palette,eight)) {
            NoodsGx::packPalette(palette,r->palettes[bg],256,true);
            if (!eight) for (unsigned bank=1;bank<16;++bank)
                r->palettes[bg][bank*32]=r->palettes[bg][bank*32+1]=0;
        }
#elif NOODS_GX_2D_PAL_CACHE
        static NoodsGx2D::PaletteCache paletteCache[2];
        memcpy(r->palettes[bg],paletteCache[engine].get(palette,eight),512);
#else
        NoodsGx::packPalette(palette,r->palettes[bg],256,true);
        if (!eight) for (int bank=1;bank<16;++bank)
            r->palettes[bg][bank*32]=r->palettes[bg][bank*32+1]=0;
#endif
        const int hofs=bgHOfs[bg];
#if NOODS_GX_2D_IO >= 5
        // Decode scrolled tiles with aligned word stores; clip once afterward.
        // All 33 source tiles remain snapshots of this emulated scanline.
        alignas(4) uint8_t linear[264];
        const unsigned shift=hofs&7;
#endif
        const int yy=(line+bgVOfs[bg])&511;
        uint32_t map=bgVramAddr+((dispCnt>>11)&0x70000)+((bgCnt[bg]<<3)&0xF800)+((yy&0xF8)<<3);
        const uint32_t base=bgVramAddr+((dispCnt>>8)&0x70000)+((bgCnt[bg]<<12)&0x3C000);
        if (yy>=256 && (bgCnt[bg]&BIT(15))) map+=(bgCnt[bg]&BIT(14))?0x1000:0x800;
        for (int left=-(hofs&7);left<256;left+=8) {
            const int xx=(left+hofs)&511;
            const uint16_t tile=fastRead16(map+((xx&0xF8)>>2)+((xx>=256 && (bgCnt[bg]&BIT(14)))?0x800:0));
            const int y=(tile&BIT(11))?7-(yy&7):(yy&7);
            const uint32_t addr=base+(tile&1023)*(eight?64:32)+y*(eight?8:4);
            const uint32_t lo=fastRead32(addr),hi=eight?fastRead32(addr+4):0;
#if NOODS_GX_2D_IO >= 5
            if (shift) NoodsGx2D::linearTileRow(linear+left+shift,tile,lo,hi,eight);
            else
#endif
            NoodsGx2D::tileRow(r->indices[bg],left,tile,lo,hi,eight);
        }
#if NOODS_GX_2D_IO >= 5
        if (shift) for (unsigned x=0;x<256;x+=8)
            memcpy(r->indices[bg]+x*4,linear+x+shift,8);
#endif
    }
    return true;
#else
    (void)line;
    return false;
#endif
}

void Gpu2D::drawScanline(int line) {
    V180_PROFILE_SCOPE(0);
    V180_PROFILE_INC(12);
#if NOODS_GX_2D
    gx2dDiscard(engine,line);
#if NOODS_GX_2D_BITMAP
    if (queueGxBitmapLine(line)) {
#if !NOODS_GX_2D_VERIFY
        winMaskValid=false;objectLineCount=0;l1Needed=false;
        internalX[1]+=bgPB[1];internalY[1]+=bgPD[1];
        return;
#endif
    }
#endif
    if (queueGxTextLine(line)) {
#if !NOODS_GX_2D_VERIFY
        return; // vblank publishes completed GX readback before frame consumers
#endif
    }
#endif
#if NOODS_SOLO_TEXT_VERIFY
    bool verifySolo = false;
    uint32_t expectedSolo[256];
#endif
    const uint8_t dispMode = (dispCnt >> 16) & 0x3;
    const uint8_t bgMode = dispCnt & 0x7;
    // v1.118: the raw composed line (layers[0]) is only ever read back by display capture,
    // and only from engine A. The one-pass paths below skip that second store otherwise.
    const bool rawNeeded = engine == 0 && core->gpu.captureMayRead();

    // Fast early bypass when display mode is not 2D layer composition (mode 0 = off, mode 2 = direct VRAM)
    // and display capture is not sampling Engine A's raw 2D composition output (layers[0]).
    if (dispMode != 1 && (engine == 1 || !(core->gpu.readDispCapCnt() & BIT(31)))) {
        // Keep internal affine reference registers advancing if an affine BG is enabled
        if (bgMode >= 1 && (dispCnt & 0x0C00)) {
            if ((dispCnt & BIT(11)) && (bgMode <= 5)) {
                internalX[1] += bgPB[1];
                internalY[1] += bgPD[1];
            }
            if ((dispCnt & BIT(10)) && (bgMode == 2 || bgMode == 4 || bgMode == 5 || bgMode == 6)) {
                internalX[0] += bgPB[0];
                internalY[0] += bgPD[0];
            }
        }
        if (dispMode == 0) {
            memset(&framebuffer[line * 256], 0xFF, 256 * sizeof(uint32_t));
        } else if (dispMode == 2) {
            uint32_t addr = 0x6800000 + ((dispCnt >> 1) & 0x60000) + line * 256 * 2;
            uint32_t *dst = &framebuffer[line * 256];
            if (uint8_t *vramPtr = core->memory.readMap9A[addr >> 12]) {
                vramPtr += (addr & 0xFFE);
                for (int i = 0; i < 256; i++)
                    dst[i] = rgb5ToRgb6(U8TO16(vramPtr, i * 2));
            } else {
                for (int i = 0; i < 256; i++)
                    dst[i] = rgb5ToRgb6(fastRead16(addr + i * 2));
            }
        } else {
            LOG_CRIT("Unimplemented engine %c display mode: display FIFO\n", (engine == 0) ? 'A' : 'B');
        }
        goto apply_master_bright;
    }

    // v1.80: fused 3D-only composite path.
    //
    // When BG0 alone carries 3D with no other backgrounds, sprites, windows,
    // blending or high-res 3D, the general path performs per-pixel window and
    // priority arbitration across six arrays and then re-reads them in a blend
    // loop that can never blend. The visible result is exactly "3D pixel if
    // lit, backdrop otherwise", so compose it in one pass that writes both the
    // raw layer (used by display capture) and the framebuffer.
    if (engine == 0 && dispMode == 1 && bgMode == 0 && !Settings::highRes3D &&
        (dispCnt & (BIT(3) | BIT(8))) == (BIT(3) | BIT(8)) &&
        (dispCnt & (0x0F00 | BIT(12) | 0xE000)) == BIT(8) &&
        ((bldCnt & 0xC0) == 0 || ((bldCnt & 0x80) && bldY == 0))) {
        // v1.115: also taken with a brightness effect of strength 0 (a no-op that games leave
        // on). The only blend left is a translucent 3D pixel over the backdrop, when the
        // backdrop is a second target.
        uint32_t backdrop = U8TO16(palette, 0) & ~BIT(15);
        uint32_t backdrop6 = rgb5ToRgb6(backdrop);
        uint32_t *line3D = core->gpu3DRenderer.getLine(line);
        uint32_t *dst = &framebuffer[line * 256];
        if (bldCnt & BIT(13)) {
            const uint32_t br = backdrop6 & 0x3F, bg = (backdrop6 >> 6) & 0x3F, bb = (backdrop6 >> 12) & 0x3F;
            for (int i = 0; i < 256; i++) {
                uint32_t pixel = line3D[i];
                if (!(pixel & 0xFC0000)) {
                    pixel = backdrop6;
                } else {
                    const uint32_t eva = ((pixel >> 18) & 0x3F) + 1;
                    if (eva != 64) {
                        const uint32_t evb = 64 - eva;
                        const uint32_t r = std::min((((pixel >> 0) & 0x3F) * eva + br * evb) / 64, 63U);
                        const uint32_t g = std::min((((pixel >> 6) & 0x3F) * eva + bg * evb) / 64, 63U);
                        const uint32_t b = std::min((((pixel >> 12) & 0x3F) * eva + bb * evb) / 64, 63U);
                        pixel = (b << 12) | (g << 6) | r;
                    }
                }
                dst[i] = pixel;
            }
        } else {
            for (int i = 0; i < 256; i++) {
                const uint32_t p3 = line3D[i];
                dst[i] = (p3 & 0xFC0000) ? p3 : backdrop6;
            }
        }
        if (rawNeeded) memcpy(layers[0], dst, 256 * sizeof(uint32_t));
        goto apply_master_bright;
    }

    {
    // Clear layers with the backdrop (first palette index)
    uint32_t backdrop = U8TO16(palette, 0) & ~BIT(15);
    // v1.114: a brightness effect with BLDY = 0 changes nothing (games leave it switched on
    // between fades), so the line is composed as if blending were off.
    const uint8_t rawBldMode = (bldCnt >> 6) & 0x3;
    // v1.115: alpha blending always needs a second target; with none selected, an alpha
    // effect, a semi-transparent sprite pixel and a 3D pixel all leave the line unchanged.
    const bool noTargets2 = (bldCnt & 0x3F00) == 0;
    const uint8_t bldMode = ((rawBldMode >= 2 && bldY == 0) || (rawBldMode == 1 && noTargets2)) ? 0 : rawBldMode;
    const bool needBlendLayers = (bldMode != 0) || ((dispCnt & BIT(12)) != 0) || ((dispCnt & BIT(3)) && (dispCnt & BIT(8)));

    if ((dispCnt & 0x1F00) == 0 && bldMode == 0) {
        uint32_t col = rgb5ToRgb6(backdrop);
        for (int i = 0; i < 256; i++) layers[0][i] = col;
        goto copy_display_mode;
    }

    // v1.122: a brightness effect at full strength (BLDY = 16) that covers every layer and
    // the backdrop, with no window to exempt pixels and no second target (which would let a
    // 3D or semi-transparent sprite pixel take the alpha path instead), turns every pixel of
    // the line black or white whatever is drawn. Games hold exactly this state while they
    // load behind a faded-out screen. The line is filled directly; the layers and sprites
    // are not drawn. Only the per-line step of the affine backgrounds is kept.
    if (NOODS_SOLID_FADE && rawBldMode >= 2 && bldY == 16 && (bldCnt & 0x3F3F) == 0x003F && (dispCnt & 0xE000) == 0) {
        const uint32_t col = (rawBldMode == 2) ? 0x3FFFFu : 0u;
        for (int i = 0; i < 256; i++) layers[0][i] = col;
        winMaskValid = false;
        objectLineCount = 0;
        l1Needed = false;
        const uint8_t m = bgMode;
        if ((dispCnt & BIT(11)) && m >= 1 && m <= 5) { internalX[1] += bgPB[1]; internalY[1] += bgPD[1]; }
        if ((dispCnt & BIT(10)) && (m == 2 || m >= 4) && m <= 6) { internalX[0] += bgPB[0]; internalY[0] += bgPD[0]; }
        goto copy_display_mode;
    }

#if NOODS_SOLO_TEXT
    {
    const uint32_t only = dispCnt & 0x0F00;
    if (dispMode == 1 && bgMode == 0 && bldMode == 0 && only && !(only & (only - 1u)) &&
        !(dispCnt & (BIT(12) | 0xE000)) && !((only == BIT(8)) && (dispCnt & BIT(3)))) {
        const int bg = __builtin_ctz(only) - 8;
        if (soloTextLine(bg, line, backdrop, rawNeeded)) {
#if NOODS_SOLO_TEXT_VERIFY
            memcpy(expectedSolo, &framebuffer[line * 256], sizeof(expectedSolo));
            verifySolo = true; // also run the unchanged reference path below
#else
            winMaskValid = false;
            objectLineCount = 0;
            l1Needed = false;
            goto apply_master_bright;
#endif
        }
    }
    }
#endif

    // v1.118: a lone full-screen bitmap on BG3 with nothing that could blend or cover it is
    // converted and written to the frame in one pass, without clearing or filling the layer
    // buffers first (v1.115 did this from inside drawExtended, after the clears).
    if (dispMode == 1 && bldMode == 0 && bgMode >= 3 && bgMode <= 5 &&
        (dispCnt & (0x1F00 | 0xE000)) == 0x0800 && soloBitmapLine(3, line, backdrop, rawNeeded)) {
        winMaskValid = false;
        objectLineCount = 0;
        l1Needed = false;
        internalX[1] += bgPB[1];
        internalY[1] += bgPD[1];
        goto apply_master_bright;
    }

    for (int i = 0; i < 256; i++) layers[0][i] = backdrop;
    memset(priorities, 4, sizeof(priorities));
    if (needBlendLayers)
        memset(blendBits[0], 5, sizeof(blendBits[0]));

    // v1.80: one active-object list and one current-line window mask.
    winMaskValid = false;
    if (dispCnt & BIT(12)) {
        objectLineCount = buildObjectLineDs(line);
        if (dispCnt & BIT(15)) drawObjects<0>(line, true);
        if (dispCnt & 0xE000) buildWindowMask(line);
        drawObjects<0>(line, false);
    }
    else {
        objectLineCount = 0;
        if (dispCnt & 0xE000) buildWindowMask(line);
    }

    // v1.114: the second layer (the pixel underneath) is only ever read for alpha blending:
    // by a blend effect, under a 3D pixel, or under a semi-transparent object pixel. Objects
    // are drawn first and only into the top layer, so it is known here whether any of that
    // can happen on this line. If not, the backgrounds keep just the top layer: two stores
    // per pixel instead of six.
    {
        bool need = (bldMode != 0) || (!noTargets2 && (dispCnt & BIT(3)) && (dispCnt & BIT(8)));
        if (!need && !noTargets2 && (dispCnt & BIT(12))) {
            uint32_t orBits = 0;
            for (int i = 0; i < 256; i++) orBits |= layers[0][i];
            need = (orBits & BIT(25)) != 0;
        }
        l1Needed = need;
        if (need) {
            for (int i = 0; i < 256; i++) layers[1][i] = backdrop;
            memset(blendBits[1], 5, sizeof(blendBits[1]));
        }
    }

    // Draw the background layers depending on the BG mode
    switch (uint8_t mode = bgMode) {
    case 0:
        if (dispCnt & BIT(11)) drawText<0>(3, line);
        if (dispCnt & BIT(10)) drawText<0>(2, line);
        if (dispCnt & BIT(9)) drawText<0>(1, line);
        if (dispCnt & BIT(8)) drawText<0>(0, line);
        break;

    case 1:
        if (dispCnt & BIT(11)) drawAffine<0>(3, line);
        if (dispCnt & BIT(10)) drawText<0>(2, line);
        if (dispCnt & BIT(9)) drawText<0>(1, line);
        if (dispCnt & BIT(8)) drawText<0>(0, line);
        break;

    case 2:
        if (dispCnt & BIT(11)) drawAffine<0>(3, line);
        if (dispCnt & BIT(10)) drawAffine<0>(2, line);
        if (dispCnt & BIT(9)) drawText<0>(1, line);
        if (dispCnt & BIT(8)) drawText<0>(0, line);
        break;

    case 3:
        if (dispCnt & BIT(11)) drawExtended(3, line);
        if (dispCnt & BIT(10)) drawText<0>(2, line);
        if (dispCnt & BIT(9)) drawText<0>(1, line);
        if (dispCnt & BIT(8)) drawText<0>(0, line);
        break;

    case 4:
        if (dispCnt & BIT(11)) drawExtended(3, line);
        if (dispCnt & BIT(10)) drawAffine<0>(2, line);
        if (dispCnt & BIT(9)) drawText<0>(1, line);
        if (dispCnt & BIT(8)) drawText<0>(0, line);
        break;

    case 5:
        if (dispCnt & BIT(11)) drawExtended(3, line);
        if (dispCnt & BIT(10)) drawExtended(2, line);
        if (dispCnt & BIT(9)) drawText<0>(1, line);
        if (dispCnt & BIT(8)) drawText<0>(0, line);
        break;

    case 6:
        if (dispCnt & BIT(10)) drawLarge(2, line);
        break;

    default:
        LOG_CRIT("Unknown engine %c BG mode: %d\n", (engine == 0) ? 'A' : 'B', mode);
        break;
    }

    if (!l1Needed) {
        if ((dispCnt & BIT(3)) && (dispCnt & BIT(8))) {
            // v1.115: 3D pixels are 18-bit already and are kept as they are
            uint32_t *dst = (dispMode == 1) ? &framebuffer[line * 256] : layers[1];
            if (dispMode == 1 && !rawNeeded) {
                for (int i = 0; i < 256; i++) {
                    const uint32_t px = layers[0][i];
                    dst[i] = (px & BIT(26)) ? px : rgb5ToRgb6(px);
                }
                goto apply_master_bright;
            }
            for (int i = 0; i < 256; i++) {
                const uint32_t px = layers[0][i];
                const uint32_t color = (px & BIT(26)) ? px : rgb5ToRgb6(px);
                layers[0][i] = color;
                dst[i] = color;
            }
            if (dispMode == 1) goto apply_master_bright;
            goto copy_display_mode;
        }
        if (dispMode == 1) {
            // v1.80: raw line and visible framebuffer are identical here.
            // Convert once and write both destinations, avoiding a 1 KB copy.
            uint32_t *dst = &framebuffer[line * 256];
            if (!rawNeeded) {
                for (int i = 0; i < 256; i++) dst[i] = rgb5ToRgb6(layers[0][i]);
                goto apply_master_bright;
            }
            for (int i = 0; i < 256; i++) {
                uint32_t color = rgb5ToRgb6(layers[0][i]);
                layers[0][i] = color;
                dst[i] = color;
            }
            goto apply_master_bright;
        }
        for (int i = 0; i < 256; i++)
            layers[0][i] = rgb5ToRgb6(layers[0][i]);
        goto copy_display_mode;
    }

    // v1.80: when blending is off and no pixel needs the forced alpha paths,
    // convert the raw line and emit the framebuffer in one pass. This removes
    // both the per-pixel blend dispatch and the later 1 KB memcpy.
    if (bldMode == 0) {
        uint32_t orBits = 0;
        for (int i = 0; i < 256; i++)
            orBits |= layers[0][i];
        if (!(orBits & (BIT(25) | BIT(26)))) {
            if (dispMode == 1) {
                uint32_t *dst = &framebuffer[line * 256];
                if (!rawNeeded) {
                    for (int i = 0; i < 256; i++) dst[i] = rgb5ToRgb6(layers[0][i]);
                    goto apply_master_bright;
                }
                for (int i = 0; i < 256; i++) {
                    uint32_t color = rgb5ToRgb6(layers[0][i]);
                    layers[0][i] = color;
                    dst[i] = color;
                }
                goto apply_master_bright;
            }
            for (int i = 0; i < 256; i++)
                layers[0][i] = rgb5ToRgb6(layers[0][i]);
            goto copy_display_mode;
        }
    }

    // v1.80: winMaskCache is already current for this line (built immediately
    // after the object-window pass and before BG/object rasterization).
    // Blend the layers to form the final image
    // v1.112: the visible line is written as each pixel is finished, instead of a separate
    // 1 KB copy afterwards ("continue" below leaves the inner do-while, not the for loop).
    {
    uint32_t *const lineOut = &framebuffer[line * 256];
    // registers held in locals: the per-pixel stores could alias them, so the compiler
    // otherwise reloads each one for every pixel
    const int bc = bldCnt, ba = bldAlpha, by = bldY;
    const uint32_t dc = dispCnt;
    for (int mode = (bc >> 6) & 0x3, i = 0; i < 256; i++) {
      do {
        // Check if blending can/should be performed
        if (layers[0][i] & BIT(26)) { // 3D pixel
            if (bc & BIT(8 + blendBits[1][i])) { // Below pixel is blendable
                // Override the default blending rules and apply special alpha blending
                // If 3D alpha is max, skip blending; high-res 3D is transposed on these pixels
                uint8_t eva = ((layers[0][i] >> 18) & 0x3F) + 1;
                if (eva == 64) continue;
                uint32_t blend = rgb5ToRgb6(layers[1][i]);
                uint8_t evb = 64 - eva;
                uint8_t r = std::min((((layers[0][i] >> 0) & 0x3F) * eva + ((blend >> 0) & 0x3F) * evb) / 64, 63U);
                uint8_t g = std::min((((layers[0][i] >> 6) & 0x3F) * eva + ((blend >> 6) & 0x3F) * evb) / 64, 63U);
                uint8_t b = std::min((((layers[0][i] >> 12) & 0x3F) * eva + ((blend >> 12) & 0x3F) * evb) / 64, 63U);
                layers[0][i] = (b << 12) | (g << 6) | r;
                continue;
            }
            else if (mode < 2 || !(bc & BIT(blendBits[0][i]))) {
                // Do nothing unless brightness blending is possible as a fallback
                continue;
            }
        }
        else {
            // Convert the pixel to 18-bit since it isn't an 18-bit 3D pixel
            layers[0][i] = rgb5ToRgb6(layers[0][i]);

            if (layers[0][i] & BIT(25)) { // Semi-transparent pixel
                // Force alpha blending if possible, otherwise allow brightness blending or do nothing
                if (bc & BIT(8 + blendBits[1][i])) // Below pixel is blendable
                    goto alpha;
                else if (mode < 2 || !(bc & BIT(blendBits[0][i])))
                    continue;
            }
            else if (mode == 0 || !(bc & BIT(blendBits[0][i])) || (mode == 1 && !(bc & BIT(8 + blendBits[1][i])))) {
                // Do nothing if blending is disabled or not possible
                continue;
            }
        }

        // Skip blending if the pixel is in the bounds of a window that has it disabled — v1.74 via prebuilt mask
        if (dc & 0xE000) {
            if (!(winMaskCache[i] & BIT(5))) continue;
        }

        // Apply blending, using 18-bit colors in DS mode — v1.73 alpha LUT fast path, v1.74 brightness LUT
        switch (mode) {
        case 1: // Alpha blending
        alpha: {
            uint32_t blend = (layers[1][i] & BIT(26)) ? layers[1][i] : rgb5ToRgb6(layers[1][i]);
            uint8_t eva = std::min((ba >> 0) & 0x1F, 16);
            uint8_t evb = std::min((ba >> 8) & 0x1F, 16);
            // Fast LUT when eva+evb==16 (common), otherwise fallback
            if (eva + evb <= 16) {
                // v1.112 (v1.114: also for eva+evb == 16, replacing a 70 KB table that missed the cache):
                // no channel can exceed 63 here, so red+blue and green are blended as
                // packed fields with two multiplies each and no clamping.
                const uint32_t a = layers[0][i];
                const uint32_t rb = ((a & 0x3F03Fu) * eva + (blend & 0x3F03Fu) * evb) >> 4;
                const uint32_t g = ((a & 0xFC0u) * eva + (blend & 0xFC0u) * evb) >> 4;
                layers[0][i] = (rb & 0x3F03Fu) | (g & 0xFC0u);
            } else {
                uint8_t r = std::min((((layers[0][i] >> 0) & 0x3F) * eva + ((blend >> 0) & 0x3F) * evb) / 16, 63U);
                uint8_t g = std::min((((layers[0][i] >> 6) & 0x3F) * eva + ((blend >> 6) & 0x3F) * evb) / 16, 63U);
                uint8_t b = std::min((((layers[0][i] >> 12) & 0x3F) * eva + ((blend >> 12) & 0x3F) * evb) / 16, 63U);
                layers[0][i] = (b << 12) | (g << 6) | r;
            }
            continue;
        }

        case 2: // Brightness increase — v1.74 LUT (no div)
            if (by) {
                const uint32_t a = layers[0][i], rb = a & 0x3F03Fu, g = a & 0xFC0u;
                layers[0][i] = (rb + ((((0x3F03Fu - rb) * by) >> 4) & 0x3F03Fu)) |
                               (g + ((((0xFC0u - g) * by) >> 4) & 0xFC0u));
            }
            continue;

        case 3: // Brightness decrease — v1.74 LUT
            if (by) {
                const uint32_t a = layers[0][i], rb = a & 0x3F03Fu, g = a & 0xFC0u;
                layers[0][i] = (rb - (((rb * by) >> 4) & 0x3F03Fu)) | (g - (((g * by) >> 4) & 0xFC0u));
            }
            continue;
        }
      } while (0);
      lineOut[i] = layers[0][i];
    }
    }
    if (dispMode == 1)
        goto apply_master_bright;

copy_display_mode:
    // Copy the final image to the framebuffer
    switch (dispMode) {
    case 0:
        memset(&framebuffer[line * 256], 0xFF, 256 * sizeof(uint32_t));
        break;
    case 1:
        memcpy(&framebuffer[line * 256], layers[0], 256 * sizeof(uint32_t));
        break;
    case 2: {
        uint32_t addr = 0x6800000 + ((dispCnt >> 1) & 0x60000) + line * 256 * 2;
        uint32_t *dst = &framebuffer[line * 256];
        for (int i = 0; i < 256; i++)
            dst[i] = rgb5ToRgb6(fastRead16(addr + i * 2));
        break;
    }
    default:
        LOG_CRIT("Unimplemented engine %c display mode: display FIFO\n", (engine == 0) ? 'A' : 'B');
        break;
    }
    }

apply_master_bright:
#if NOODS_SOLO_TEXT_VERIFY
    if (verifySolo) {
        for (int x = 0; x < 256; ++x) {
            g_soloTextStats[4] += expectedSolo[x] != framebuffer[line * 256 + x];
            if (rawNeeded) g_soloTextStats[5] += expectedSolo[x] != layers[0][x];
        }
    }
#endif
    // Apply master brightness (DS-only, 18-bit).
    // v1.114: red+blue and green are scaled as packed fields (c + (63-c)*f/16 and c - c*f/16
    // per channel, the same rounding as the tables used before), two multiplies per pixel
    // instead of three table loads.
    switch ((masterBright >> 14) & 0x3) {
    case 1: {
        const uint32_t factor = std::min<uint8_t>(masterBright & 0x1F, 16);
        if (factor) {
            uint32_t *px = &framebuffer[line * 256];
            for (int i = 0; i < 256; i++) {
                const uint32_t rb = px[i] & 0x3F03Fu, g = px[i] & 0xFC0u;
                px[i] = (rb + ((((0x3F03Fu - rb) * factor) >> 4) & 0x3F03Fu)) |
                        (g + ((((0xFC0u - g) * factor) >> 4) & 0xFC0u));
            }
        }
        break;
    }
    case 2: {
        const uint32_t factor = std::min<uint8_t>(masterBright & 0x1F, 16);
        if (factor) {
            uint32_t *px = &framebuffer[line * 256];
            for (int i = 0; i < 256; i++) {
                const uint32_t rb = px[i] & 0x3F03Fu, g = px[i] & 0xFC0u;
                px[i] = (rb - (((rb * factor) >> 4) & 0x3F03Fu)) |
                        (g - (((g * factor) >> 4) & 0xFC0u));
            }
        }
        break;
    }
    }
}

FORCE_INLINE void Gpu2D::drawBgPixel(int bg, int line, int x, uint32_t pixel) {
    // v1.72: use precomputed window mask when valid, otherwise fast window check
    if (__builtin_expect(dispCnt & 0xE000, 0)) {
        if (winMaskValid) {
            if (!(winMaskCache[x] & BIT(bg))) return;
        } else {
            uint16_t enabled;
            if ((dispCnt & BIT(13)) && winVFlag[0] && (x >= winX1[0] && x < winX2[0]) != winHFlip[0])
                enabled = winIn;
            else if ((dispCnt & BIT(14)) && winVFlag[1] && (x >= winX1[1] && x < winX2[1]) != winHFlip[1])
                enabled = winIn >> 8;
            else if ((dispCnt & BIT(15)) && (framebuffer[line * 256 + x] & BIT(24)))
                enabled = winOut >> 8;
            else
                enabled = winOut;
            if (!(enabled & BIT(bg))) return;
        }
    }
    int prio = bgCnt[bg] & 0x3;
    if (__builtin_expect(!l1Needed, 0)) {
        if (prio <= priorities[0][x]) {
            layers[0][x] = pixel;
            priorities[0][x] = prio;
        }
        return;
    }
    if (__builtin_expect(prio <= priorities[0][x], 1)) {
        layers[1][x] = layers[0][x];
        priorities[1][x] = priorities[0][x];
        blendBits[1][x] = blendBits[0][x];
        layers[0][x] = pixel;
        priorities[0][x] = prio;
        blendBits[0][x] = bg;
    } else if (prio <= priorities[1][x]) {
        layers[1][x] = pixel;
        priorities[1][x] = prio;
        blendBits[1][x] = bg;
    }
}
FORCE_INLINE void Gpu2D::drawObjPixel(int line, int x, uint32_t pixel, int8_t priority) {
    if (__builtin_expect(dispCnt & 0xE000, 0)) {
        if (winMaskValid) {
            if (!(winMaskCache[x] & BIT(4))) return;
        } else {
            uint16_t enabled;
            if ((dispCnt & BIT(13)) && winVFlag[0] && (x >= winX1[0] && x < winX2[0]) != winHFlip[0])
                enabled = winIn;
            else if ((dispCnt & BIT(14)) && winVFlag[1] && (x >= winX1[1] && x < winX2[1]) != winHFlip[1])
                enabled = winIn >> 8;
            else if ((dispCnt & BIT(15)) && (framebuffer[line * 256 + x] & BIT(24)))
                enabled = winOut >> 8;
            else
                enabled = winOut;
            if (!(enabled & BIT(4))) return;
        }
    }
    if (__builtin_expect(!(layers[0][x] & BIT(15)) || priority < priorities[0][x], 1)) {
        layers[0][x] = pixel;
        priorities[0][x] = priority;
        blendBits[0][x] = 4;
    }
}

template <bool gbaMode> void Gpu2D::drawText(int bg, int line) {
    V180_PROFILE_SCOPE_IF(18, gbaMode);
    // If 3D is enabled, override BG0 in text mode
    if (!gbaMode && bg == 0 && (dispCnt & BIT(3))) {
        // In high-res 3D mode, skip every other pixel
        uint32_t *data = core->gpu3DRenderer.getLine(line);
        bool resShift = Settings::highRes3D;

        // Draw a scanline of 3D pixels
        for (int i = 0; i < 256; i++)
            if (data[i << resShift] & 0xFC0000)
                drawBgPixel(bg, line, i, data[i << resShift]);
        return;
    }

    // Get the base data addresses
    uint32_t tileBase = bgVramAddr + ((dispCnt >> 11) & 0x70000) + ((bgCnt[bg] << 3) & 0x0F800);
    uint32_t indexBase = bgVramAddr + ((dispCnt >> 8) & 0x70000) + ((bgCnt[bg] << 12) & 0x3C000);

    // Adjust the Y-coordinate within the background based on vertical mosaic
    int yOffset = (((bgCnt[bg] & BIT(6)) ? (line - (line % (((mosaic >> 4) & 0xF) + 1))) : line) + bgVOfs[bg]) & 0x1FF;
    tileBase += (yOffset & 0xF8) << 3;

    // If the Y-offset exceeds 256 and the background is 512 pixels tall, move to the next 256x256 section
    // If the background is 512 pixels wide, move 2 sections to skip the second X section
    if (yOffset >= 256 && (bgCnt[bg] & BIT(15)))
        tileBase += (bgCnt[bg] & BIT(14)) ? 0x1000 : 0x800;

    // With mosaic enabled and a horizontal period above 1, the fetched pixel
    // repeats across each horizontal mosaic block (the vertical snap is
    // already in yOffset). Fetch once per block and replicate the pixel,
    // stepping by whole blocks so no per-pixel modulo or redundant VRAM
    // reads are needed. A period of 1 is a visual no-op, so those layers
    // keep the fast tile path below.
    if ((bgCnt[bg] & BIT(6)) && (mosaic & 0xF)) {
        int periodH = (mosaic & 0xF) + 1;
        int width = gbaMode ? 240 : 256;
        uint8_t *extPal = (bgCnt[bg] & BIT(7)) ?
            ((dispCnt & BIT(30)) ? extPalettes[bg + (bg < 2 && (bgCnt[bg] & BIT(13))) * 2] : nullptr) : nullptr;
        for (int x = 0; x < width; x += periodH) {
            // Move the tile address to the current tile; x is a block start,
            // so the snapped sample offset is x itself
            int xOffset = (bgHOfs[bg] + x) & 0x1FF;
            uint32_t tileAddr = tileBase + ((xOffset & 0xF8) >> 2);

            // If the X-offset exceeds 256 and the background is 512 pixels wide, move to the next 256x256 section
            if (xOffset >= 256 && (bgCnt[bg] & BIT(14)))
                tileAddr += 0x800;

            // Get the current tile and the pixel offsets within it, accounting for flips
            uint16_t tile = fastRead16T<gbaMode>(tileAddr);
            int px = (tile & BIT(10)) ? (7 - (xOffset & 7)) : (xOffset & 7);
            int py = (tile & BIT(11)) ? (7 - (yOffset & 7)) : (yOffset & 7);

            // Fetch the block's pixel once
            uint16_t color;
            if (bgCnt[bg] & BIT(7)) { // 8-bit
                uint8_t *pal = extPal ? &extPal[(tile >> 3) & 0x1E00] : palette;
                uint8_t index = fastRead8T<gbaMode>(indexBase + (tile & 0x3FF) * 64 + py * 8 + px);
                if (!index)
                    continue;
                color = U8TO16(pal, index * 2) | BIT(15);
            }
            else { // 4-bit
                uint8_t *pal = &palette[(tile & 0xF000) >> 7];
                uint32_t indices = fastRead32T<gbaMode>(indexBase + (tile & 0x3FF) * 32 + py * 4);
                uint8_t index = (indices >> (px * 4)) & 0xF;
                if (!index)
                    continue;
                color = U8TO16(pal, index * 2) | BIT(15);
            }

            // Replicate the pixel across the rest of the block
            int end = std::min(x + periodH, width);
            for (int i = x; i < end; i++)
                drawBgPixel(bg, line, i, color);
        }
        return;
    }

    // v1.116: the registers and layer pointers are read once per line, and the pixel write is
    // done here with them held in locals. (The stores into the layer arrays may alias the
    // registers, so the compiler otherwise reloads each one for every tile and pixel.)
    const int hofs = bgHOfs[bg];
    const bool wide = (bgCnt[bg] & BIT(14)) != 0;
    const bool winOn = (dispCnt & 0xE000) != 0;
    const bool slowPut = winOn && !winMaskValid;
    const uint8_t bgBit = BIT(bg);
    const int prio = bgCnt[bg] & 0x3;
    const bool l1n = gbaMode || l1Needed;
    const uint8_t *const wm = winMaskCache;
    uint32_t *const l0 = layers[0], *const l1 = layers[1];
    int8_t *const p0 = priorities[0], *const p1 = priorities[1];
    int8_t *const b0 = blendBits[0], *const b1 = blendBits[1];
    auto put = [&](int x, uint32_t pixel) __attribute__((always_inline)) {
        if (__builtin_expect(slowPut, 0)) { drawBgPixel(bg, line, x, pixel); return; }
        if (winOn && !(wm[x] & bgBit)) return;
        if (!l1n) {
            if (prio <= p0[x]) { l0[x] = pixel; p0[x] = prio; }
            return;
        }
        if (__builtin_expect(prio <= p0[x], 1)) {
            l1[x] = l0[x];
            p1[x] = p0[x];
            b1[x] = b0[x];
            l0[x] = pixel;
            p0[x] = prio;
            b0[x] = bg;
        } else if (prio <= p1[x]) {
            l1[x] = pixel;
            p1[x] = prio;
            b1[x] = bg;
        }
    };

    // Draw a line
    int width = (gbaMode ? 240 : 256) + (hofs & 0x7);
    if (bgCnt[bg] & BIT(7)) { // 8-bit
        // Set the extended palette slot if enabled; BG 0/1 can optionally use slot 2/3
        uint8_t *extPal = (dispCnt & BIT(30)) ? extPalettes[bg + (bg < 2 && (bgCnt[bg] & BIT(13))) * 2] : nullptr;
        for (int i = 0; i < width; i += 8) {
            // Move the tile address to the current tile
            int xOffset = (i + hofs) & 0x1FF;
            uint32_t tileAddr = tileBase + ((xOffset & 0xF8) >> 2);

            // If the X-offset exceeds 256 and the background is 512 pixels wide, move to the next 256x256 section
            if (xOffset >= 256 && wide)
                tileAddr += 0x800;

            // Get the current tile and its palette
            uint16_t tile = fastRead16T<gbaMode>(tileAddr);
            uint8_t *pal = extPal ? &extPal[(tile >> 3) & 0x1E00] : palette;

            // Get the palette indices for the current line of the tile, flipped vertically if enabled
            uint32_t indexAddr = indexBase + (tile & 0x3FF) * 64 + ((tile & BIT(11)) ? (7 - (yOffset & 7)) : (yOffset & 7)) * 8;
            uint64_t indices = fastRead32T<gbaMode>(indexAddr) |
                ((uint64_t)fastRead32T<gbaMode>(indexAddr + 4) << 32);

            // Draw the current line of the tile, flipped horizontally if enabled
            for (int x = i - (xOffset & 7) + ((tile & BIT(10)) ? 7 : 0); indices != 0; ((tile & BIT(10)) ? x-- : x++), indices >>= 8) {
                // Draw a pixel if it's visible
                if (x >= 0 && x < (gbaMode ? 240 : 256) && (indices & 0xFF))
                    put(x, U8TO16(pal, (indices & 0xFF) * 2) | BIT(15));
            }
        }
    }
    else { // 4-bit
        for (int i = 0; i < width; i += 8) {
            // Move the tile address to the current tile
            int xOffset = (i + hofs) & 0x1FF;
            uint32_t tileAddr = tileBase + ((xOffset & 0xF8) >> 2);

            // If the X-offset exceeds 256 and the background is 512 pixels wide, move to the next 256x256 section
            if (xOffset >= 256 && wide)
                tileAddr += 0x800;

            // Get the current tile
            uint16_t tile = fastRead16T<gbaMode>(tileAddr);

            // Get the tile's palette
            // In 4-bit mode, the tile can select from multiple 16-color palettes
            uint8_t *pal = &palette[(tile & 0xF000) >> 7];

            // Get the palette indices for the current line of the tile, flipped vertically if enabled
            uint32_t indexAddr = indexBase + (tile & 0x3FF) * 32 + ((tile & BIT(11)) ? (7 - (yOffset & 7)) : (yOffset & 7)) * 4;
            uint32_t indices = fastRead32T<gbaMode>(indexAddr);

            // Draw the current line of the tile, flipped horizontally if enabled
            for (int x = i - (xOffset & 7) + ((tile & BIT(10)) ? 7 : 0); indices != 0; ((tile & BIT(10)) ? x-- : x++), indices >>= 4) {
                // Draw a pixel if it's visible
                if (x >= 0 && x < (gbaMode ? 240 : 256) && (indices & 0xF))
                    put(x, U8TO16(pal, (indices & 0xF) * 2) | BIT(15));
            }
        }
    }
}

template <bool gbaMode> void Gpu2D::drawAffine(int bg, int line) {
#if NOODS_GBA_AFFINE_WINDOW && NOODS_GBA_AFFINE_VERIFY
    if constexpr (gbaMode) {
        // Verify the production fast path against the original ordering on the
        // same input state. Restore candidate state after comparing every layer.
        struct State { uint32_t layers[2][256]; int8_t priorities[2][256], blendBits[2][256]; int x[2], y[2]; } before, candidate;
        auto save = [&](State &s) { memcpy(s.layers, layers, sizeof(layers)); memcpy(s.priorities, priorities, sizeof(priorities)); memcpy(s.blendBits, blendBits, sizeof(blendBits)); memcpy(s.x, internalX, sizeof(internalX)); memcpy(s.y, internalY, sizeof(internalY)); };
        auto restore = [&](const State &s) { memcpy(layers, s.layers, sizeof(layers)); memcpy(priorities, s.priorities, sizeof(priorities)); memcpy(blendBits, s.blendBits, sizeof(blendBits)); memcpy(internalX, s.x, sizeof(internalX)); memcpy(internalY, s.y, sizeof(internalY)); };
        save(before);
        drawAffineImpl<gbaMode>(bg, line);
        save(candidate);
        restore(before); s_affineReference = true;
        drawAffineImpl<gbaMode>(bg, line);
        s_affineReference = false;
        bool mismatch = memcmp(candidate.layers, layers, sizeof(layers)) || memcmp(candidate.priorities, priorities, sizeof(priorities)) || memcmp(candidate.blendBits, blendBits, sizeof(blendBits)) || memcmp(candidate.x, internalX, sizeof(internalX)) || memcmp(candidate.y, internalY, sizeof(internalY));
        ++g_gbaAffineStats[0]; if (mismatch) ++g_gbaAffineStats[1];
        restore(candidate);
        return;
    }
#endif
    drawAffineImpl<gbaMode>(bg, line);
}

template <bool gbaMode> void Gpu2D::drawAffineImpl(int bg, int line) {
    V180_PROFILE_SCOPE_IF(16, gbaMode);
    // Calculate the base data addresses
    uint32_t tileBase = bgVramAddr + ((bgCnt[bg] << 3) & 0x0F800) + ((dispCnt >> 11) & 0x70000);
    uint32_t indexBase = bgVramAddr + ((bgCnt[bg] << 12) & 0x3C000) + ((dispCnt >> 8) & 0x70000);

    // Set the initial rotscale coordinates
    int rotscaleX = internalX[bg - 2] - bgPA[bg - 2];
    int rotscaleY = internalY[bg - 2] - bgPC[bg - 2];

    // Get the background size
    int size = 128 << ((bgCnt[bg] & 0xC000) >> 14);

    // v1.101: GBA fast path. Same per-pixel work as the loop below with every loop-invariant
    // (affine deltas, size, wrap flag, priority, window mode, page table) held in locals; the
    // generic loop reloads them each pixel because the layer stores may alias the object.
    // Taken only when a VRAM read is a plain page-table read (no CPU bus accounting active)
    // and the window test is either off or served by the precomputed per-line mask.
    if (gbaMode && !core->memory.gbaCpuExecActive && (!(dispCnt & 0xE000) || winMaskValid)) {
        const int pa = bgPA[bg - 2], pc = bgPC[bg - 2];
        const bool wrap = (bg < 2 || (bgCnt[bg] & BIT(13)));
        const int sizeMask = size - 1;
        const uint32_t rowShift = 4 + ((bgCnt[bg] & 0xC000) >> 14); // log2(size / 8)
        const uint8_t bgBit = BIT(bg);
        const bool winOn = (dispCnt & 0xE000) != 0;
        bool useWin = winOn;
#if NOODS_GBA_AFFINE_WINDOW
        // The prebuilt window mask picks exactly one of these four control
        // bytes. If all permit this BG, every pixel passes without a lookup.
        if (gbaAffineAllWindowsAllow(winIn, winOut, bgBit)
#if NOODS_GBA_AFFINE_VERIFY
            && !s_affineReference
#endif
        ) useWin = false;
#if NOODS_GBA_AFFINE_VERIFY
        if (winOn && !useWin) ++g_gbaAffineStats[2];
#endif
#endif
        const int prio = bgCnt[bg] & 0x3;
        const uint8_t *const pal = palette;
        uint8_t **const map = core->memory.readMap7;
        uint32_t *const l0 = layers[0], *const l1 = layers[1];
        int8_t *const p0 = priorities[0], *const p1 = priorities[1];
        const bool l1n = gbaMode || l1Needed;
        int8_t *const b0 = blendBits[0], *const b1 = blendBits[1];
        uint32_t lastTileAddr = 0xFFFFFFFFu, lastTile = 0;
        for (int i = 0; i < 240; i++) {
            int x = (rotscaleX += pa) >> 8;
            int y = (rotscaleY += pc) >> 8;
            if (wrap) {
                x &= sizeMask;
                y &= sizeMask;
            }
            else if ((uint32_t)x >= (uint32_t)size || (uint32_t)y >= (uint32_t)size) {
                continue;
            }
            const uint32_t tileAddr = tileBase + ((uint32_t)(y >> 3) << rowShift) + (x >> 3);
            // v1.106: neighbouring pixels usually fall in the same map cell; reuse its tile number
            if (tileAddr != lastTileAddr) {
                const uint8_t *tp = map[tileAddr >> 12];
                lastTile = tp ? tp[tileAddr & 0xFFF] : core->memory.read<uint8_t>(true, tileAddr);
                lastTileAddr = tileAddr;
            }
            const uint32_t indexAddr = indexBase + lastTile * 64 + (y & 7) * 8 + (x & 7);
            const uint8_t *ip = map[indexAddr >> 12];
            const uint8_t index = ip ? ip[indexAddr & 0xFFF] : core->memory.read<uint8_t>(true, indexAddr);
            if (!index) continue;
            if (useWin && !(winMaskCache[i] & bgBit)) continue;
            const uint32_t pixel = U8TO16(pal, index * 2) | BIT(15);
            if (!l1n) {
                if (prio <= p0[i]) { l0[i] = pixel; p0[i] = prio; }
                continue;
            }
            if (__builtin_expect(prio <= p0[i], 1)) {
                l1[i] = l0[i];
                p1[i] = p0[i];
                b1[i] = b0[i];
                l0[i] = pixel;
                p0[i] = prio;
                b0[i] = bg;
            } else if (prio <= p1[i]) {
                l1[i] = pixel;
                p1[i] = prio;
                b1[i] = bg;
            }
        }
        internalX[bg - 2] += bgPB[bg - 2];
        internalY[bg - 2] += bgPD[bg - 2];
        return;
    }

    // v1.122: the same loop for a DS screen. Everything that does not change along the line
    // is held in locals, a map cell's tile number is reused while the pixels stay inside
    // it, and the layer stores are written out for the two cases (top layer only, or both).
    if (NOODS_AFFINE_FAST && !gbaMode && (!(dispCnt & 0xE000) || winMaskValid)) {
        const int pa = bgPA[bg - 2], pc = bgPC[bg - 2];
        const bool wrap = (bg < 2 || (bgCnt[bg] & BIT(13)));
        const int sizeMask = size - 1;
        const uint32_t rowShift = 4 + ((bgCnt[bg] & 0xC000) >> 14); // log2(size / 8)
        const bool useWin = (dispCnt & 0xE000) != 0;
        const uint8_t bgBit = BIT(bg);
        const int prio = bgCnt[bg] & 0x3;
        const uint8_t *const pal = palette;
        uint32_t *const l0 = layers[0], *const l1 = layers[1];
        int8_t *const p0 = priorities[0], *const p1 = priorities[1];
        const bool l1n = l1Needed;
        int8_t *const b0 = blendBits[0], *const b1 = blendBits[1];
        uint32_t lastTileAddr = 0xFFFFFFFFu, lastTile = 0;
        for (int i = 0; i < 256; i++) {
            int x = (rotscaleX += pa) >> 8;
            int y = (rotscaleY += pc) >> 8;
            if (wrap) {
                x &= sizeMask;
                y &= sizeMask;
            }
            else if ((uint32_t)x >= (uint32_t)size || (uint32_t)y >= (uint32_t)size) {
                continue;
            }
            const uint32_t tileAddr = tileBase + ((uint32_t)(y >> 3) << rowShift) + (x >> 3);
            if (tileAddr != lastTileAddr) {
                lastTile = fastRead8T<false>(tileAddr);
                lastTileAddr = tileAddr;
            }
            const uint8_t index = fastRead8T<false>(indexBase + lastTile * 64 + (y & 7) * 8 + (x & 7));
            if (!index) continue;
            if (useWin && !(winMaskCache[i] & bgBit)) continue;
            const uint32_t pixel = U8TO16(pal, index * 2) | BIT(15);
            if (!l1n) {
                if (prio <= p0[i]) { l0[i] = pixel; p0[i] = prio; }
                continue;
            }
            if (__builtin_expect(prio <= p0[i], 1)) {
                l1[i] = l0[i];
                p1[i] = p0[i];
                b1[i] = b0[i];
                l0[i] = pixel;
                p0[i] = prio;
                b0[i] = bg;
            } else if (prio <= p1[i]) {
                l1[i] = pixel;
                p1[i] = prio;
                b1[i] = bg;
            }
        }
        internalX[bg - 2] += bgPB[bg - 2];
        internalY[bg - 2] += bgPD[bg - 2];
        return;
    }

    // Draw a line
    for (int i = 0; i < (gbaMode ? 240 : 256); i++) {
        // Increment the rotscaled coordinates and remove the fraction
        int x = (rotscaleX += bgPA[bg - 2]) >> 8;
        int y = (rotscaleY += bgPC[bg - 2]) >> 8;

        // Handle display area overflow
        if (bg < 2 || (bgCnt[bg] & BIT(13))) { // Wraparound
            x &= size - 1;
            y &= size - 1;
        }
        else if (x < 0 || x >= size || y < 0 || y >= size) { // Transparent
            continue;
        }

        // Read the current tile
        uint32_t tileAddr = tileBase + (y / 8) * (size / 8) + (x / 8);
        uint8_t tile = fastRead8T<gbaMode>(tileAddr);

        // Read the palette index for the current pixel of the tile
        uint32_t indexAddr = indexBase + tile * 64 + (y & 7) * 8 + (x & 7);
        uint8_t index = fastRead8T<gbaMode>(indexAddr);

        // Draw a pixel if it isn't transparent
        if (index)
            drawBgPixel(bg, line, i, U8TO16(palette, index * 2) | BIT(15));
    }

    // Increment the internal registers at the end of the scanline
    internalX[bg - 2] += bgPB[bg - 2];
    internalY[bg - 2] += bgPD[bg - 2];
}

// v1.118: one line of an unscaled, unrotated bitmap background that is the only thing on
// its screen: read, convert to 18-bit and write to the frame in one pass. Returns false
// (having drawn nothing) when the line is not that simple.
bool Gpu2D::soloBitmapLine(int bg, int line, uint32_t backdrop, bool raw) {
    if (!(bgCnt[bg] & BIT(7)) || bgPA[bg - 2] != 0x100 || bgPC[bg - 2] != 0) return false;
    static const uint16_t sizes[] = { 128, 128, 256, 256, 512, 256, 512, 512 };
    const uint16_t *size = &sizes[((bgCnt[bg] >> 13) & 0x6)];
    const int sizeX = size[0], sizeY = size[1];
    const int x0 = internalX[bg - 2] >> 8, y = internalY[bg - 2] >> 8;
    if (x0 < 0 || x0 + 256 > sizeX || y < 0 || y >= sizeY) return false;
    const bool direct = (bgCnt[bg] & BIT(2)) != 0;
    const uint32_t bpp = direct ? 2 : 1;
    const uint32_t xShift = (uint32_t)__builtin_ctz((unsigned)sizeX);
    uint32_t addr = bgVramAddr + ((bgCnt[bg] << 6) & 0x7C000) + ((((uint32_t)y << xShift) + x0) * bpp);
    uint8_t **const map = core->memory.readMap9A;
    for (uint32_t a = addr >> 12; a <= (addr + 256 * bpp - 1) >> 12; a++)
        if (!map[a]) return false;

    uint32_t *out = &framebuffer[line * 256];
    const uint32_t back6 = rgb5ToRgb6(backdrop);
    const uint8_t *const pal = palette;
    // The row is walked one 4 KB page at a time (a row crosses at most one page boundary)
    int left = 256;
    while (left > 0) {
        const uint8_t *p = map[addr >> 12] + (addr & 0xFFF);
        int n = (int)((0x1000 - (addr & 0xFFF)) / bpp);
        if (n > left) n = left;
        if (n == 0) n = 1; // a 16-bit pixel never straddles a page (addresses are even)
        if (direct) {
            for (int i = 0; i < n; i++) {
                const uint16_t v = U8TO16(p, i * 2);
                out[i] = (v & BIT(15)) ? rgb5ToRgb6(v) : back6;
            }
        } else {
            for (int i = 0; i < n; i++) {
                const uint8_t index = p[i];
                out[i] = index ? rgb5ToRgb6(U8TO16(pal, index * 2) | BIT(15)) : back6;
            }
        }
        out += n; addr += n * bpp; left -= n;
    }
    if (raw) memcpy(layers[0], &framebuffer[line * 256], 256 * sizeof(uint32_t));
    return true;
}

void Gpu2D::drawExtended(int bg, int line) {
    // Set the initial rotscale coordinates
    int rotscaleX = internalX[bg - 2] - bgPA[bg - 2];
    int rotscaleY = internalY[bg - 2] - bgPC[bg - 2];

    if (bgCnt[bg] & BIT(7)) { // Bitmap
        // Calculate the base data address
        uint32_t dataBase = bgVramAddr + ((bgCnt[bg] << 6) & 0x7C000);

        // Get the bitmap size
        static const uint16_t sizes[] = { 128, 128, 256, 256, 512, 256, 512, 512 };
        const uint16_t *size = &sizes[((bgCnt[bg] >> 13) & 0x6)];
        const uint16_t sizeX = size[0];
        const uint16_t sizeY = size[1];

        // v1.102: bitmap fast path. Same per-pixel work as the loops below with the
        // loop-invariants (affine deltas, size, wrap flag, priority, window mode, page table)
        // held in locals; the generic loops reload them every pixel because the layer stores
        // may alias the object. Used when the window test is off or served by the per-line mask.
        if (!(dispCnt & 0xE000) || winMaskValid) {
            const int pa = bgPA[bg - 2], pc = bgPC[bg - 2];
            const bool wrap = (bgCnt[bg] & BIT(13)) != 0;
            const bool direct = (bgCnt[bg] & BIT(2)) != 0;
            const uint32_t xShift = (uint32_t)__builtin_ctz((unsigned)sizeX); // sizeX is 128/256/512
            const bool useWin = (dispCnt & 0xE000) != 0;
            const uint8_t bgBit = BIT(bg);
            const int prio = bgCnt[bg] & 0x3;
            const uint8_t *const pal = palette;
            uint8_t **const map = core->memory.readMap9A;
            uint32_t *const l0 = layers[0], *const l1 = layers[1];
            int8_t *const p0 = priorities[0], *const p1 = priorities[1];
            const bool l1n = l1Needed;
            int8_t *const b0 = blendBits[0], *const b1 = blendBits[1];
            // v1.112: unscaled, unrotated bitmap whose 256 pixels lie inside the image (the
            // usual full-screen picture): walk the source row with a pointer, looking up the
            // page table once per 4 KB page instead of once per pixel.
            bool rowDone = false;
            if (pa == 0x100 && pc == 0) {
                const int x0 = (rotscaleX + 0x100) >> 8, y = rotscaleY >> 8;
                if (x0 >= 0 && x0 + 256 <= sizeX && y >= 0 && y < sizeY) {
                    const uint32_t bpp = direct ? 2 : 1;
                    uint32_t addr = dataBase + ((((uint32_t)y << xShift) + x0) * bpp);
                    bool ok = true;
                    for (uint32_t a = addr >> 12; a <= (addr + 256 * bpp - 1) >> 12; a++)
                        if (!map[a]) ok = false;
                    if (ok) {
                        for (int i = 0; i < 256; i++, addr += bpp) {
                            const uint8_t *p = map[addr >> 12] + (addr & 0xFFF);
                            uint32_t pixel;
                            if (direct) {
                                const uint16_t v = U8TO16(p, 0);
                                if (!(v & BIT(15))) continue;
                                pixel = v;
                            } else {
                                const uint8_t index = *p;
                                if (!index) continue;
                                pixel = U8TO16(pal, index * 2) | BIT(15);
                            }
                            if (useWin && !(winMaskCache[i] & bgBit)) continue;
                            if (!l1n) {
                                if (prio <= p0[i]) { l0[i] = pixel; p0[i] = prio; }
                                continue;
                            }
                            if (__builtin_expect(prio <= p0[i], 1)) {
                                l1[i] = l0[i];
                                p1[i] = p0[i];
                                b1[i] = b0[i];
                                l0[i] = pixel;
                                p0[i] = prio;
                                b0[i] = bg;
                            } else if (prio <= p1[i]) {
                                l1[i] = pixel;
                                p1[i] = prio;
                                b1[i] = bg;
                            }
                        }
                        rowDone = true;
                    }
                }
            }
            if (!rowDone)
            for (int i = 0; i < 256; i++) {
                int x = (rotscaleX += pa) >> 8;
                int y = (rotscaleY += pc) >> 8;
                if (wrap) {
                    x &= sizeX - 1;
                    y &= sizeY - 1;
                }
                else if ((uint32_t)x >= (uint32_t)sizeX || (uint32_t)y >= (uint32_t)sizeY) {
                    continue;
                }
                uint32_t pixel;
                if (direct) {
                    const uint32_t addr = dataBase + ((((uint32_t)y << xShift) + x) << 1);
                    const uint8_t *p = map[addr >> 12];
                    const uint16_t v = p ? U8TO16(p, addr & 0xFFF) : core->memory.read<uint16_t>(0, addr);
                    if (!(v & BIT(15))) continue;
                    pixel = v;
                } else {
                    const uint32_t addr = dataBase + ((uint32_t)y << xShift) + x;
                    const uint8_t *p = map[addr >> 12];
                    const uint8_t index = p ? p[addr & 0xFFF] : core->memory.read<uint8_t>(0, addr);
                    if (!index) continue;
                    pixel = U8TO16(pal, index * 2) | BIT(15);
                }
                if (useWin && !(winMaskCache[i] & bgBit)) continue;
                if (!l1n) {
                    if (prio <= p0[i]) { l0[i] = pixel; p0[i] = prio; }
                    continue;
                }
                if (__builtin_expect(prio <= p0[i], 1)) {
                    l1[i] = l0[i];
                    p1[i] = p0[i];
                    b1[i] = b0[i];
                    l0[i] = pixel;
                    p0[i] = prio;
                    b0[i] = bg;
                } else if (prio <= p1[i]) {
                    l1[i] = pixel;
                    p1[i] = prio;
                    b1[i] = bg;
                }
            }
        }
        else if (bgCnt[bg] & BIT(2)) { // Direct color bitmap
            // Draw a line
            for (int i = 0; i < 256; i++) {
                // Increment the rotscaled coordinates and remove the fraction
                int x = (rotscaleX += bgPA[bg - 2]) >> 8;
                int y = (rotscaleY += bgPC[bg - 2]) >> 8;

                // Handle display area overflow
                if (bgCnt[bg] & BIT(13)) { // Wraparound
                    x &= sizeX - 1;
                    y &= sizeY - 1;
                }
                else if (x < 0 || x >= sizeX || y < 0 || y >= sizeY) { // Transparent
                    continue;
                }

                // Draw a pixel if it isn't transparent
                uint16_t pixel = fastRead16(dataBase + (y * sizeX + x) * 2);
                if (pixel & BIT(15))
                    drawBgPixel(bg, line, i, pixel);
            }
        }
        else { // 256 color bitmap
            // Draw a line
            for (int i = 0; i < 256; i++) {
                // Increment the rotscaled coordinates and remove the fraction
                int x = (rotscaleX += bgPA[bg - 2]) >> 8;
                int y = (rotscaleY += bgPC[bg - 2]) >> 8;

                // Handle display area overflow
                if (bgCnt[bg] & BIT(13)) { // Wraparound
                    x &= sizeX - 1;
                    y &= sizeY - 1;
                }
                else if (x < 0 || x >= sizeX || y < 0 || y >= sizeY) { // Transparent
                    continue;
                }

                // Read the palette index for the current pixel
                uint8_t index = fastRead8(dataBase + y * sizeX + x);

                // Draw a pixel if it isn't transparent
                if (index)
                    drawBgPixel(bg, line, i, U8TO16(palette, index * 2) | BIT(15));
            }
        }
    }
    else { // Extended affine
        uint32_t tileBase = bgVramAddr + ((bgCnt[bg] << 3) & 0x0F800) + ((dispCnt >> 11) & 0x70000);
        uint32_t indexBase = bgVramAddr + ((bgCnt[bg] << 12) & 0x3C000) + ((dispCnt >> 8) & 0x70000);
        size_t size = 128 << ((bgCnt[bg] >> 14) & 0x3);

        // In the non-threaded renderer, CPU and DMA cannot change the tile
        // map during this function. Cache the last *mapped* tile and hold
        // scanline invariants in locals. The reference path remains for
        // threaded rendering or a window without a precomputed mask.
        if (!Settings::threaded2D && !core->gpu.has2DWorker() && (!(dispCnt & 0xE000) || winMaskValid)) {
            const int pa = bgPA[bg - 2], pc = bgPC[bg - 2];
            const bool wrap = (bgCnt[bg] & BIT(13)) != 0;
            const bool useWin = (dispCnt & 0xE000) != 0;
            const bool extEnabled = (dispCnt & BIT(30)) != 0;
            const uint8_t *const ext = extPalettes[bg];
            const uint8_t *const standardPal = palette;
            uint8_t **const map = core->memory.readMap9A;
            const uint8_t bgBit = BIT(bg);
            const int prio = bgCnt[bg] & 3;
            const bool l1n = l1Needed;
            uint32_t *const l0 = layers[0], *const l1 = layers[1];
            int8_t *const p0 = priorities[0], *const p1 = priorities[1];
            int8_t *const b0 = blendBits[0], *const b1 = blendBits[1];
            // Common extended-palette, unwrapped, no-window composition.
            // All derived tile addresses are stable for this single scanline:
            // the worker is absent and the guest cannot write VRAM here.
            if (!wrap && extEnabled && !useWin && l1n) {
                if (ext) {
                    uint32_t cachedAddr = 0xFFFFFFFFu;
                    uint32_t indexBaseCached = 0;
                    const uint8_t *tilePixels = nullptr, *tilePalette = nullptr;
                    int xFlip = 0, yFlip = 0;
                    for (int i = 0; i < 256; i++) {
                        int x = (rotscaleX += pa) >> 8;
                        int y = (rotscaleY += pc) >> 8;
                        if ((uint32_t)x >= size || (uint32_t)y >= size) continue;
                        uint32_t tileKey = (((uint32_t)y & ~7u) << 4) | ((uint32_t)x >> 3);
                        if (tileKey != cachedAddr) {
                            uint32_t tileAddr = tileBase + (((uint32_t)(y >> 3) * (size >> 3) + (x >> 3)) << 1);
                            const uint8_t *tilePage = map[tileAddr >> 12];
                            uint16_t tile = tilePage ? U8TO16(tilePage, tileAddr & 0xFFF) : fastRead16(tileAddr);
                            cachedAddr = tilePage ? tileKey : 0xFFFFFFFFu;
                            indexBaseCached = indexBase + ((tile & 0x3FF) << 6);
                            const uint8_t *indexPage = map[indexBaseCached >> 12];
                            tilePixels = indexPage ? indexPage + (indexBaseCached & 0xFFF) : nullptr;
                            tilePalette = ext + ((tile >> 3) & 0x1E00);
                            xFlip = (tile & BIT(10)) ? 7 : 0;
                            yFlip = (tile & BIT(11)) ? 7 : 0;
                        }
                        unsigned off = (((y ^ yFlip) & 7) << 3) | ((x ^ xFlip) & 7);
                        uint8_t index = tilePixels ? tilePixels[off] : fastRead8(indexBaseCached + off);
                        if (!index) continue;
                        uint32_t pixel = U8TO16(tilePalette, index * 2) | BIT(15);
                        if (prio <= p0[i]) {
                            l1[i] = l0[i]; p1[i] = p0[i]; b1[i] = b0[i];
                            l0[i] = pixel; p0[i] = prio; b0[i] = bg;
                        } else if (prio <= p1[i]) {
                            l1[i] = pixel; p1[i] = prio; b1[i] = bg;
                        }
                    }
                }
            } else {
            uint32_t cachedAddr = 0xFFFFFFFFu;
            uint16_t cachedTile = 0;
            for (int i = 0; i < 256; i++) {
                int x = (rotscaleX += pa) >> 8;
                int y = (rotscaleY += pc) >> 8;
                if (wrap) { x &= size - 1; y &= size - 1; }
                else if ((uint32_t)x >= size || (uint32_t)y >= size) continue;
                uint32_t tileAddr = tileBase + (((uint32_t)(y >> 3) * (size >> 3) + (x >> 3)) << 1);
                uint16_t tile;
                if (tileAddr == cachedAddr) tile = cachedTile;
                else {
                    const uint8_t *tilePage = map[tileAddr >> 12];
                    if (tilePage) {
                        tile = U8TO16(tilePage, tileAddr & 0xFFF);
                        cachedAddr = tileAddr;
                        cachedTile = tile;
                    } else {
                        tile = fastRead16(tileAddr);
                        cachedAddr = 0xFFFFFFFFu;
                    }
                }
                if (extEnabled && !ext) continue;
                const uint8_t *pal = extEnabled ? ext + ((tile >> 3) & 0x1E00) : standardPal;
                uint32_t indexAddr = indexBase + (tile & 0x3FF) * 64 +
                    (((tile & BIT(11)) ? (7 - y) : y) & 7) * 8 +
                    (((tile & BIT(10)) ? (7 - x) : x) & 7);
                const uint8_t *indexPage = map[indexAddr >> 12];
                const uint8_t index = indexPage ? indexPage[indexAddr & 0xFFF] : fastRead8(indexAddr);
                if (!index) continue;
                uint32_t pixel = U8TO16(pal, index * 2) | BIT(15);
                if (useWin && !(winMaskCache[i] & bgBit)) continue;
                if (!l1n) {
                    if (prio <= p0[i]) { l0[i] = pixel; p0[i] = prio; }
                } else if (prio <= p0[i]) {
                    l1[i] = l0[i]; p1[i] = p0[i]; b1[i] = b0[i];
                    l0[i] = pixel; p0[i] = prio; b0[i] = bg;
                } else if (prio <= p1[i]) {
                    l1[i] = pixel; p1[i] = prio; b1[i] = bg;
                }
            }
            }
        } else {
        // Use the standard palette by default
        uint8_t *pal = palette;

        // Draw a line of the layer
        for (int i = 0; i < 256; i++) {
            // Increment the rotscaled coordinates and remove the fraction
            int x = (rotscaleX += bgPA[bg - 2]) >> 8;
            int y = (rotscaleY += bgPC[bg - 2]) >> 8;

            // Handle display area overflow
            if (bgCnt[bg] & BIT(13)) { // Wraparound
                x &= size - 1;
                y &= size - 1;
            }
            else if (x < 0 || x >= size || y < 0 || y >= size) { // Transparent
                continue;
            }

            // Read the current tile
            uint32_t tileAddr = tileBase + ((y / 8) * (size / 8) + (x / 8)) * 2;
            uint16_t tile = fastRead16(tileAddr);

            // Switch to an extended palette selected by the tile if enabled
            if (dispCnt & BIT(30)) {
                if (!extPalettes[bg]) continue;
                pal = &extPalettes[bg][(tile >> 3) & 0x1E00];
            }

            // Read the palette index for the current pixel
            uint32_t indexAddr = indexBase + (tile & 0x3FF) * 64 + // Tile offset
                (((tile & BIT(11)) ? (7 - y) : y) & 7) * 8 + // Vertical offset, flipped if enabled
                (((tile & BIT(10)) ? (7 - x) : x) & 7); // Horizontal offset, flipped if enabled
            uint8_t index = fastRead8(indexAddr);

            // Draw the pixel if it isn't transparent
            if (index)
                drawBgPixel(bg, line, i, U8TO16(pal, index * 2) | BIT(15));
        }
    }

    }

    // Increment the internal registers at the end of the scanline
    internalX[bg - 2] += bgPB[bg - 2];
    internalY[bg - 2] += bgPD[bg - 2];
}

void Gpu2D::drawExtendedGba(int bg, int line) {
    V180_PROFILE_SCOPE(21);
    // Calculate the base data address
    // Modes 4 and 5 have two bitmaps that can be selected with DISPCNT bit 4
    uint8_t mode = dispCnt & 0x7;
    uint32_t dataBase = bgVramAddr + ((bgCnt[bg] << 3) & 0xF800) +
        ((mode > 3 && (dispCnt & BIT(4))) ? 0xA000 : 0);

    // Set the initial rotscale coordinates
    int rotscaleX = internalX[bg - 2] - bgPA[bg - 2];
    int rotscaleY = internalY[bg - 2] - bgPC[bg - 2];

    if (mode == 4) { // 256 color bitmap
        // Draw a line of the layer
        for (int i = 0; i < 240; i++) {
            // Increment the rotscaled coordinates and remove the fraction
            int x = (rotscaleX += bgPA[bg - 2]) >> 8;
            int y = (rotscaleY += bgPC[bg - 2]) >> 8;

            // Don't draw anything on display area overflow
            if (x < 0 || x >= 240 || y < 0 || y >= 160)
                continue;

            // Draw a pixel if it has a non-transparent palette index
            if (uint8_t index = core->memory.read<uint8_t>(1, dataBase + y * 240 + x))
                drawBgPixel(bg, line, i, U8TO16(palette, index * 2) | BIT(15));
        }
    }
    else { // Direct color bitmap
        // Get the bitmap size
        uint8_t sizeX = (mode == 5) ? 160 : 240;
        uint8_t sizeY = (mode == 5) ? 128 : 160;

        // Draw a line of the layer
        for (int i = 0; i < 240; i++) {
            // Increment the rotscaled coordinates and remove the fraction
            int x = (rotscaleX += bgPA[bg - 2]) >> 8;
            int y = (rotscaleY += bgPC[bg - 2]) >> 8;

            // Don't draw anything on display area overflow
            if (x < 0 || x >= sizeX || y < 0 || y >= sizeY)
                continue;

            // Draw a pixel, ignoring transparency
            uint16_t pixel = core->memory.read<uint16_t>(1, dataBase + (y * sizeX + x) * 2);
            drawBgPixel(bg, line, i, pixel | BIT(15));
        }
    }

    // Increment the internal registers at the end of the scanline
    internalX[bg - 2] += bgPB[bg - 2];
    internalY[bg - 2] += bgPD[bg - 2];
}

void Gpu2D::drawLarge(int bg, int line) {
    // Set the initial rotscale coordinates
    int rotscaleX = internalX[bg - 2] - bgPA[bg - 2];
    int rotscaleY = internalY[bg - 2] - bgPC[bg - 2];

    // Get the bitmap size
    int sizeX = ((bgCnt[bg] >> 14) & 0x3) ? 1024 : 512;
    int sizeY = ((bgCnt[bg] >> 14) & 0x3) ? 512 : 1024;

    // Draw a line of the layer
    for (int i = 0; i < 256; i++) {
        // Increment the rotscaled coordinates and remove the fraction
        int x = (rotscaleX += bgPA[bg - 2]) >> 8;
        int y = (rotscaleY += bgPC[bg - 2]) >> 8;

        // Handle display area overflow
        if (bgCnt[bg] & BIT(13)) { // Wraparound
            x &= sizeX - 1;
            y &= sizeY - 1;
        }
        else if (x < 0 || x >= sizeX || y < 0 || y >= sizeY) { // Transparent
            continue;
        }

        // A full large bitmap requires 512KB of VRAM, but engine B can only use 128KB
        // For engine B, wrap the 128KB bitmap 4 times to cover the full area
        if (engine == 1)
            y &= (sizeY / 4) - 1;

        // Read the palette index for the current pixel
        uint8_t index = fastRead8(bgVramAddr + y * sizeX + x);

        // Draw a pixel if it isn't transparent
        if (index)
            drawBgPixel(bg, line, i, U8TO16(palette, index * 2) | BIT(15));
    }

    // Increment the internal registers at the end of the scanline
    internalX[bg - 2] += bgPB[bg - 2];
    internalY[bg - 2] += bgPD[bg - 2];
}

template <bool gbaMode> void Gpu2D::drawObjects(int line, bool window) {
    V180_PROFILE_SCOPE_IF(17, gbaMode);
#if NOODS_DS_OBJ_VERIFY
    if (!gbaMode || NOODS_OBJ_WORD_READS) {
        struct ObjState { uint32_t pixels[256], fb[256]; int8_t prio[256], bits[256]; };
        ObjState before, expected;
        auto save = [&](ObjState &s) { memcpy(s.pixels,layers[0],sizeof(s.pixels)); memcpy(s.fb,&framebuffer[line*256],sizeof(s.fb)); memcpy(s.prio,priorities[0],256); memcpy(s.bits,blendBits[0],256); };
        auto load = [&](const ObjState &s) { memcpy(layers[0],s.pixels,sizeof(s.pixels)); memcpy(&framebuffer[line*256],s.fb,sizeof(s.fb)); memcpy(priorities[0],s.prio,256); memcpy(blendBits[0],s.bits,256); };
        save(before);
        objReference = false;
        drawObjectsImpl<gbaMode>(line, window);
        save(expected); load(before);
        objReference = true;
        drawObjectsImpl<gbaMode>(line, window);
        objReference = false;
        ++g_dsObjStats[4];
        if (gbaMode) ++g_dsObjStats[7];
        for (int x=0;x<256;++x) {
            g_dsObjStats[5] += expected.pixels[x] != layers[0][x];
            g_dsObjStats[5] += expected.fb[x] != framebuffer[line*256+x];
            g_dsObjStats[5] += expected.prio[x] != priorities[0][x];
            g_dsObjStats[5] += expected.bits[x] != blendBits[0][x];
        }
        return;
    }
#endif
    drawObjectsImpl<gbaMode>(line, window);
}

template <bool gbaMode> void Gpu2D::drawObjectsImpl(int line, bool window) {
    // v1.80: walk only sprites that passed this scanline's OAM cull.
    for (int i = 0; i < objectLineCount; i++) {
        const ObjectLine &entry = objectLine[i];
        const uint8_t type = entry.type;
        if ((type == 2) != window)
            continue;

        const uint16_t *object = entry.object;
        const uint8_t width = entry.width;
        const uint8_t height = entry.height;
        const uint8_t width2 = entry.width2;
        const uint8_t height2 = entry.height2;
        const int x = entry.x;
        const int spriteY = entry.spriteY;
        const int8_t priority = entry.priority;

        // v1.106: GBA fast path. Same per-pixel decisions as the loops below, with the loop
        // invariants in locals, the rotscale products stepped by addition instead of two
        // multiplies per pixel, and VRAM read through the page table directly. Used when a
        // VRAM read is a plain page-table read and the window test is off or precomputed.
        if (((gbaMode && !core->memory.gbaCpuExecActive && !(dispCnt & BIT(31))) ||
             (!gbaMode && NOODS_DS_OBJ_FAST && !objReference && type != 3)) &&
            (!(dispCnt & 0xE000) || winMaskValid)) {
            if (!gbaMode) {
                ++g_dsObjStats[(object[0] & BIT(13)) ? 1 : 0];
                if (object[0] & BIT(8)) ++g_dsObjStats[2];
                if (window) ++g_dsObjStats[3];
            }
            const int screenWidth = gbaMode ? 240 : 256;
            const bool useWin = (dispCnt & 0xE000) != 0;
            const bool map1D = (dispCnt & BIT(gbaMode ? 6 : 4)) != 0;
            const bool is8 = (object[0] & BIT(13)) != 0;
            const bool objWin = (type == 2);
            const uint32_t semi = (uint32_t)(type == 1) << 25;
            uint8_t **const map = gbaMode ? core->memory.readMap7 : core->memory.readMap9A;
            uint32_t *const l0 = layers[0];
            int8_t *const p0 = priorities[0];
            int8_t *const b0 = blendBits[0];
            uint32_t *const fbLine = &framebuffer[line * 256];
            const uint8_t *pal = is8 ? &palette[0x200] : &palette[0x200 + ((object[2] & 0xF000) >> 12) * 32];
            if (!gbaMode && is8 && (dispCnt & BIT(31))) {
                if (!extPalettes[4]) continue;
                pal = &extPalettes[4][(object[2] & 0xF000) >> 3];
                ++g_dsObjStats[6];
            }
            const uint32_t boundary = gbaMode ? 32 : (map1D ? (32 << ((dispCnt >> 20) & 3)) : 32);
            uint32_t tb = (gbaMode ? 0x6010000 : objVramAddr) + (object[2] & 0x3FF) * boundary;
            auto rd8 = [&](const uint32_t addr) __attribute__((always_inline)) -> uint8_t {
                const uint8_t *const p = map[addr >> 12];
                return p ? p[addr & 0xFFF] : core->memory.read<uint8_t>(gbaMode, addr);
            };
            auto put = [&](const int offset, const uint32_t index) __attribute__((always_inline)) {
                if (index && objWin) {
                    fbLine[offset] |= BIT(24);
                    return;
                }
                if (index && (!useWin || (winMaskCache[offset] & BIT(4)))) {
                    if (__builtin_expect(!(l0[offset] & BIT(15)) || priority < p0[offset], 1)) {
                        l0[offset] = semi | BIT(15) | U8TO16(pal, index * 2);
                        p0[offset] = priority;
                        b0[offset] = 4;
                    }
                }
                // Update the priority even if the pixel is transparent (GBA quirk)
                if (gbaMode && priority < p0[offset] && b0[offset] == 4)
                    p0[offset] = priority;
            };
            if (object[0] & BIT(8)) { // Rotscale
                const uint32_t pBase = ((object[1] >> 9) & 0x1F) * 0x20;
                const int pa = (int16_t)U8TO16(oam, pBase + 0x06), pb = (int16_t)U8TO16(oam, pBase + 0x0E);
                const int pc = (int16_t)U8TO16(oam, pBase + 0x16), pd = (int16_t)U8TO16(oam, pBase + 0x1E);
                const int mapWidth = map1D ? width : (is8 ? 128 : 256);
                const int j0 = (x < 0) ? -x : 0;
                const int j1 = (x + width2 > screenWidth) ? (screenWidth - x) : width2;
                int ax = pa * (j0 - width2 / 2) + pb * (spriteY - height2 / 2);
                int ay = pc * (j0 - width2 / 2) + pd * (spriteY - height2 / 2);
                const int halfW = width / 2, halfH = height / 2;
                for (int j = j0; j < j1; j++, ax += pa, ay += pc) {
                    const int rx = (ax >> 8) + halfW;
                    if ((unsigned)rx >= (unsigned)width) continue;
                    const int ry = (ay >> 8) + halfH;
                    if ((unsigned)ry >= (unsigned)height) continue;
                    uint32_t index;
                    if (is8) {
                        index = rd8(tb + ((ry >> 3) * mapWidth + (ry & 7)) * 8 + (rx >> 3) * 64 + (rx & 7));
                    } else {
                        index = rd8(tb + ((ry >> 3) * mapWidth + (ry & 7)) * 4 + (rx >> 3) * 32 + ((rx & 7) >> 1));
                        index = (rx & 1) ? (index >> 4) : (index & 0x0F);
                    }
                    put(x + j, index);
                }
            } else {
                const int mapWidth = map1D ? width : (is8 ? 128 : 256);
                const bool hflip = (object[1] & BIT(12)) != 0;
                const int unit = is8 ? 8 : 4;
                if (object[1] & BIT(13)) // Vertical flip
                    tb += (7 - (spriteY % 8) + ((height - 1 - spriteY) / 8) * mapWidth) * unit;
                else
                    tb += ((spriteY % 8) + (spriteY / 8) * mapWidth) * unit;
                uint32_t lastWordAddr = 0xFFFFFFFFu, word = 0;
                for (int j = 0; j < width; j++) {
                    if (NOODS_OBJ_WORD_READS && !objReference) {
                        const uint32_t step = is8 ? 4 : 8;
                        if ((j & (step - 1)) == 0) {
                            const uint32_t addr = tb + (j >> 3) * (is8 ? 64 : 32) + (is8 ? (j & 4) : 0);
                            word = fastRead32T<gbaMode>(addr);
                            lastWordAddr = addr;
                            if (!gbaMode && !objWin && word == 0) {
                                j += step - 1;
                                continue;
                            }
                        }
                    }
                    const int offset = hflip ? (x + width - j - 1) : (x + j);
                    if ((unsigned)offset >= (unsigned)screenWidth) continue;
                    uint32_t index;
                    if (NOODS_OBJ_WORD_READS && !objReference) {
                        // Tile rows are aligned and never cross a 4KB map page.
                        // Read each 32-bit word once, then extract 4 bytes/8 nibbles.
                        index = (word >> (is8 ? ((j & 3) * 8) : ((j & 7) * 4))) & (is8 ? 255u : 15u);
                    } else if (is8) {
                        index = rd8(tb + (j >> 3) * 64 + (j & 7));
                    } else {
                        index = rd8(tb + (j >> 3) * 32 + ((j & 7) >> 1));
                        index = (j & 1) ? (index >> 4) : (index & 0x0F);
                    }
                    put(offset, index);
                }
            }
            continue;
        }

        // Draw bitmap objects
        if (!gbaMode && type == 3) {
            uint32_t dataBase;
            int bitmapWidth;

            // Determine the address and width of the bitmap
            if (dispCnt & BIT(6)) { // 1D mapping
                dataBase = objVramAddr + (object[2] & 0x3FF) * ((dispCnt & BIT(22)) ? 256 : 128);
                bitmapWidth = width;
            }
            else { // 2D mapping
                uint8_t xMask = (dispCnt & BIT(5)) ? 0x1F : 0x0F;
                dataBase = objVramAddr + (object[2] & xMask) * 0x10 + (object[2] & 0x3FF & ~xMask) * 0x80;
                bitmapWidth = (dispCnt & BIT(5)) ? 256 : 128;
            }

            if (object[0] & BIT(8)) { // Rotscale
                // Get the rotscale parameters
                int16_t params[4] = {
                    (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x06),
                    (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x0E),
                    (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x16),
                    (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x1E)
                };

                // Draw a line of the object
                for (int j = 0; j < width2; j++) {
                    int offset = x + j;
                    if (offset < 0 || offset >= (gbaMode ? 240 : 256)) continue;

                    // Calculate the rotscaled X coordinate relative to the object
                    int rotscaleX = ((params[0] * (j - width2 / 2) + params[1] * (spriteY - height2 / 2)) >> 8) + width / 2;
                    if (rotscaleX < 0 || rotscaleX >= width) continue;

                    // Calculate the rotscaled Y coordinate relative to the object
                    int rotscaleY = ((params[2] * (j - width2 / 2) + params[3] * (spriteY - height2 / 2)) >> 8) + height / 2;
                    if (rotscaleY < 0 || rotscaleY >= height) continue;

                    // Draw a pixel
                    uint16_t pixel = fastRead16(dataBase + (rotscaleY * bitmapWidth + rotscaleX) * 2);
                    if (pixel & BIT(15))
                        drawObjPixel(line, offset, pixel, priority);
                }
            }
            else {
                // Adjust the bitmap offset based on whether or not the sprite is vertically flipped
                dataBase += ((object[1] & BIT(13)) ? (height - spriteY - 1) : spriteY) * bitmapWidth * 2;

                // Draw a line of the object
                for (int j = 0; j < width; j++) {
                    // Determine the horizontal pixel offset based on whether or not the sprite is horizontally flipped
                    int offset = (object[1] & BIT(12)) ? (x + width - j - 1) : (x + j);
                    if (offset < 0 || offset >= (gbaMode ? 240 : 256)) continue;

                    // Draw a pixel
                    uint16_t pixel = fastRead16(dataBase + j * 2);
                    if (pixel & BIT(15))
                        drawObjPixel(line, offset, pixel, priority);
                }
            }
            continue;
        }

        // Get the base tile address
        uint32_t tileBase;
        if (gbaMode) {
            tileBase = 0x6010000 + (object[2] & 0x3FF) * 32;
        }
        else {
            // On the DS, the boundary between tiles can be 32, 64, 128, or 256 bytes for 1D tile mapping
            uint16_t bound = (dispCnt & BIT(4)) ? (32 << ((dispCnt >> 20) & 0x3)) : 32;
            tileBase = objVramAddr + (object[2] & 0x3FF) * bound;
        }

        if (object[0] & BIT(8)) { // Rotscale
            // Get the rotscale parameters
            int16_t params[4] = {
                (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x06),
                (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x0E),
                (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x16),
                (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x1E)
            };

            if (object[0] & BIT(13)) { // 8-bit
                int mapWidth = (dispCnt & BIT(gbaMode ? 6 : 4)) ? width : 128;

                // Get the object's palette
                uint8_t *pal;
                if (dispCnt & BIT(31)) { // Extended palette
                    // In extended palette mode, the object can select from multiple 256-color palettes
                    if (!extPalettes[4]) continue;
                    pal = &extPalettes[4][(object[2] & 0xF000) >> 3];
                }
                else { // Standard palette
                    pal = &palette[0x200];
                }

                // Draw a line of the object
                for (int j = 0; j < width2; j++) {
                    int offset = x + j;
                    if (offset < 0 || offset >= (gbaMode ? 240 : 256)) continue;

                    // Calculate the rotscaled X coordinate relative to the object
                    int rotscaleX = ((params[0] * (j - width2 / 2) + params[1] * (spriteY - height2 / 2)) >> 8) + width / 2;
                    if (rotscaleX < 0 || rotscaleX >= width) continue;

                    // Calculate the rotscaled Y coordinate relative to the object
                    int rotscaleY = ((params[2] * (j - width2 / 2) + params[3] * (spriteY - height2 / 2)) >> 8) + height / 2;
                    if (rotscaleY < 0 || rotscaleY >= height) continue;

                    // Get the palette index for the current pixel
                    uint8_t index = fastRead8T<gbaMode>( tileBase +
                        ((rotscaleY / 8) * mapWidth + rotscaleY % 8) * 8 + (rotscaleX / 8) * 64 + rotscaleX % 8);

                    if (index && type == 2) { // Object window
                        // Mark object window pixels with an extra bit, and don't draw anything
                        framebuffer[line * 256 + offset] |= BIT(24);
                    }
                    else {
                        // Draw a pixel, marking semi-transparent pixels with an extra bit
                        if (index)
                            drawObjPixel(line, offset, ((type == 1) << 25) | BIT(15) | U8TO16(pal, index * 2), priority);

                        // Update the priority even if the pixel is transparent
                        // This is a quirky behavior of the GBA, but it seems to have been fixed on the DS
                        if (gbaMode && priority < priorities[0][offset] && blendBits[0][offset] == 4)
                            priorities[0][offset] = priority;
                    }
                }
            }
            else { // 4-bit
                int mapWidth = (dispCnt & BIT(gbaMode ? 6 : 4)) ? width : 256;

                // Get the object's palette
                // In 4-bit mode, the object can select from multiple 16-color palettes
                uint8_t *pal = &palette[0x200 + ((object[2] & 0xF000) >> 12) * 32];

                // Draw a line of the object
                for (int j = 0; j < width2; j++) {
                    int offset = x + j;
                    if (offset < 0 || offset >= (gbaMode ? 240 : 256)) continue;

                    // Calculate the rotscaled X coordinate relative to the object
                    int rotscaleX = ((params[0] * (j - width2 / 2) + params[1] * (spriteY - height2 / 2)) >> 8) + width / 2;
                    if (rotscaleX < 0 || rotscaleX >= width) continue;

                    // Calculate the rotscaled Y coordinate relative to the object
                    int rotscaleY = ((params[2] * (j - width2 / 2) + params[3] * (spriteY - height2 / 2)) >> 8) + height / 2;
                    if (rotscaleY < 0 || rotscaleY >= height) continue;

                    // Get the palette index for the current pixel
                    uint8_t index = fastRead8T<gbaMode>( tileBase +
                        ((rotscaleY / 8) * mapWidth + rotscaleY % 8) * 4 + (rotscaleX / 8) * 32 + (rotscaleX % 8) / 2);
                    index = (rotscaleX & 1) ? ((index & 0xF0) >> 4) : (index & 0x0F);

                    if (index && type == 2) { // Object window
                        // Mark object window pixels with an extra bit, and don't draw anything
                        framebuffer[line * 256 + offset] |= BIT(24);
                    }
                    else {
                        // Draw a pixel, marking semi-transparent pixels with an extra bit
                        if (index)
                            drawObjPixel(line, offset, ((type == 1) << 25) | BIT(15) | U8TO16(pal, index * 2), priority);

                        // Update the priority even if the pixel is transparent
                        // This is a quirky behavior of the GBA, but it seems to have been fixed on the DS
                        if (gbaMode && priority < priorities[0][offset] && blendBits[0][offset] == 4)
                            priorities[0][offset] = priority;
                    }
                }
            }
        }
        else if (object[0] & BIT(13)) { // 8-bit
            // Adjust the current tile to align with the current Y coordinate relative to the object
            int mapWidth = (dispCnt & BIT(gbaMode ? 6 : 4)) ? width : 128;
            if (object[1] & BIT(13)) // Vertical flip
                tileBase += (7 - (spriteY % 8) + ((height - 1 - spriteY) / 8) * mapWidth) * 8;
            else
                tileBase += ((spriteY % 8) + (spriteY / 8) * mapWidth) * 8;

            // Get the object's palette
            uint8_t *pal;
            if (dispCnt & BIT(31)) { // Extended palette
                // In extended palette mode, the object can select from multiple 256-color palettes
                if (!extPalettes[4]) continue;
                pal = &extPalettes[4][(object[2] & 0xF000) >> 3];
            }
            else { // Standard palette
                pal = &palette[0x200];
            }

            // Draw a line of the object
            for (int j = 0; j < width; j++) {
                // Determine the horizontal pixel offset based on whether or not the sprite is horizontally flipped
                int offset = (object[1] & BIT(12)) ? (x + width - j - 1) : (x + j);
                if (offset < 0 || offset >= (gbaMode ? 240 : 256)) continue;

                // Get the palette index for the current pixel
                uint8_t index = fastRead8T<gbaMode>( tileBase + (j / 8) * 64 + j % 8);

                if (index && type == 2) { // Object window
                    // Mark object window pixels with an extra bit, and don't draw anything
                    framebuffer[line * 256 + offset] |= BIT(24);
                }
                else {
                    // Draw a pixel, marking semi-transparent pixels with an extra bit
                    if (index)
                        drawObjPixel(line, offset, ((type == 1) << 25) | BIT(15) | U8TO16(pal, index * 2), priority);

                    // Update the priority even if the pixel is transparent
                    // This is a quirky behavior of the GBA, but it seems to have been fixed on the DS
                    if (gbaMode && priority < priorities[0][offset] && blendBits[0][offset] == 4)
                        priorities[0][offset] = priority;
                }
            }
        }
        else { // 4-bit
            // Adjust the current tile to align with the current Y coordinate relative to the object
            int mapWidth = (dispCnt & BIT(gbaMode ? 6 : 4)) ? width : 256;
            if (object[1] & BIT(13)) // Vertical flip
                tileBase += (7 - (spriteY % 8) + ((height - 1 - spriteY) / 8) * mapWidth) * 4;
            else
                tileBase += ((spriteY % 8) + (spriteY / 8) * mapWidth) * 4;

            // Get the palette of the object
            // In 4-bit mode, the object can select from multiple 16-color palettes
            uint8_t *pal = &palette[0x200 + ((object[2] & 0xF000) >> 12) * 32];

            // Draw a line of the object
            for (int j = 0; j < width; j++) {
                // Determine the horizontal pixel offset based on whether or not the sprite is horizontally flipped
                int offset = (object[1] & BIT(12)) ? (x + width - j - 1) : (x + j);
                if (offset < 0 || offset >= (gbaMode ? 240 : 256)) continue;

                // Get the palette index for the current pixel
                uint8_t index = fastRead8T<gbaMode>( tileBase + (j / 8) * 32 + (j % 8) / 2);
                index = (j & 1) ? ((index & 0xF0) >> 4) : (index & 0x0F);

                if (index && type == 2) { // Object window
                    // Mark object window pixels with an extra bit, and don't draw anything
                    framebuffer[line * 256 + offset] |= BIT(24);
                }
                else {
                    // Draw a pixel, marking semi-transparent pixels with an extra bit
                    if (index)
                        drawObjPixel(line, offset, ((type == 1) << 25) | BIT(15) | U8TO16(pal, index * 2), priority);

                    // Update the priority even if the pixel is transparent
                    // This is a quirky behavior of the GBA, but it seems to have been fixed on the DS
                    if (gbaMode && priority < priorities[0][offset] && blendBits[0][offset] == 4)
                        priorities[0][offset] = priority;
                }
            }
        }
    }
}

void Gpu2D::writeDispCnt(uint32_t mask, uint32_t value) {
    // Write to the DISPCNT register
    mask &= ((engine == 0) ? 0xFFFFFFFF : 0xC0B1FFF7);
    dispCnt = (dispCnt & ~mask) | (value & mask);
    if (core->gbaMode) dispCnt &= 0xFFFF;
}

void Gpu2D::writeBgCnt(int bg, uint16_t mask, uint16_t value) {
    // Write to one of the BGCNT registers
    if (core->gbaMode && bg < 2) mask &= 0xDFFF;
    bgCnt[bg] = (bgCnt[bg] & ~mask) | (value & mask);
}

void Gpu2D::writeBgHOfs(int bg, uint16_t mask, uint16_t value) {
    // Write to one of the BGHOFS registers
    mask &= 0x01FF;
    bgHOfs[bg] = (bgHOfs[bg] & ~mask) | (value & mask);
}

void Gpu2D::writeBgVOfs(int bg, uint16_t mask, uint16_t value) {
    // Write to one of the BGVOFS registers
    mask &= 0x01FF;
    bgVOfs[bg] = (bgVOfs[bg] & ~mask) | (value & mask);
}

void Gpu2D::writeBgPA(int bg, uint16_t mask, uint16_t value) {
    // Write to one of the BGPA registers
    bgPA[bg - 2] = (bgPA[bg - 2] & ~mask) | (value & mask);
}

void Gpu2D::writeBgPB(int bg, uint16_t mask, uint16_t value) {
    // Write to one of the BGPB registers
    bgPB[bg - 2] = (bgPB[bg - 2] & ~mask) | (value & mask);
}

void Gpu2D::writeBgPC(int bg, uint16_t mask, uint16_t value) {
    // Write to one of the BGPC registers
    bgPC[bg - 2] = (bgPC[bg - 2] & ~mask) | (value & mask);
}

void Gpu2D::writeBgPD(int bg, uint16_t mask, uint16_t value) {
    // Write to one of the BGPD registers
    bgPD[bg - 2] = (bgPD[bg - 2] & ~mask) | (value & mask);
}

void Gpu2D::writeBgX(int bg, uint32_t mask, uint32_t value) {
    // Write to one of the BGX registers
    mask &= 0x0FFFFFFF;
    bgX[bg - 2] = (bgX[bg - 2] & ~mask) | (value & mask);

    // Extend the sign to 32 bits
    if (bgX[bg - 2] & BIT(27)) bgX[bg - 2] |= 0xF0000000; else bgX[bg - 2] &= ~0xF0000000;

    // Reload the internal register
    internalX[bg - 2] = bgX[bg - 2];
}

void Gpu2D::writeBgY(int bg, uint32_t mask, uint32_t value) {
    // Write to one of the BGY registers
    mask &= 0x0FFFFFFF;
    bgY[bg - 2] = (bgY[bg - 2] & ~mask) | (value & mask);

    // Extend the sign to 32 bits
    if (bgY[bg - 2] & BIT(27)) bgY[bg - 2] |= 0xF0000000; else bgY[bg - 2] &= ~0xF0000000;

    // Reload the internal register
    internalY[bg - 2] = bgY[bg - 2];
}


void Gpu2D::writeWinH(int win, uint16_t mask, uint16_t value) {
    // Write to one of the WINH registers
    if (mask & 0x00FF) winX2[win] = (value & 0x00FF) >> 0;
    if (mask & 0xFF00) winX1[win] = (value & 0xFF00) >> 8;

    // Invert the window if X1 exceeds X2
    if (winHFlip[win] = (winX1[win] > winX2[win]))
        SWAP(winX1[win], winX2[win]);
}

void Gpu2D::writeWinV(int win, uint16_t mask, uint16_t value) {
    // Write to one of the WINV registers
    if (mask & 0x00FF) winY2[win] = (value & 0x00FF) >> 0;
    if (mask & 0xFF00) winY1[win] = (value & 0xFF00) >> 8;
}

void Gpu2D::writeWinIn(uint16_t mask, uint16_t value) {
    // Write to the WININ register
    mask &= 0x3F3F;
    winIn = (winIn & ~mask) | (value & mask);
}

void Gpu2D::writeWinOut(uint16_t mask, uint16_t value) {
    // Write to the WINOUT register
    mask &= 0x3F3F;
    winOut = (winOut & ~mask) | (value & mask);
}

void Gpu2D::writeMosaic(uint16_t mask, uint16_t value) {
    // Write to the MOSAIC register
    mosaic = (mosaic & ~mask) | (value & mask);
}

void Gpu2D::writeBldCnt(uint16_t mask, uint16_t value) {
    // Write to the BLDCNT register
    mask &= 0x3FFF;
    bldCnt = (bldCnt & ~mask) | (value & mask);
}

void Gpu2D::writeBldAlpha(uint16_t mask, uint16_t value) {
    // Write to the BLDALPHA register
    mask &= 0x1F1F;
    bldAlpha = (bldAlpha & ~mask) | (value & mask);
}

void Gpu2D::writeBldY(uint8_t value) {
    // Write to the BLDY register
    bldY = value & 0x1F;
    if (bldY > 16) bldY = 16;
}

void Gpu2D::writeMasterBright(uint16_t mask, uint16_t value) {
    // Write to the MASTER_BRIGHT register
    mask &= 0xC01F;
    masterBright = (masterBright & ~mask) | (value & mask);
}
