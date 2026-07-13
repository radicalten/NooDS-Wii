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

#ifndef WII_VIDEO_H
#define WII_VIDEO_H

#include <gctypes.h>
#include <ogc/gx.h>
#include <stdint.h>
#include <stdbool.h>

#define NDS_SCREEN_WIDTH  256
#define NDS_SCREEN_HEIGHT 192

#define GBA_SCREEN_WIDTH  240
#define GBA_SCREEN_HEIGHT 160

struct WiiVideoSystem {
    void*    xfbList[2];
    int      currentXfb;
    u8*      texDataTop;
    u8*      texDataBottom;
    GXTexObj texObjTop;
    GXTexObj texObjBottom;
    bool     lastGbaMode;
};

#define DEBUG_OVERLAY_WIDTH  256
#define DEBUG_OVERLAY_HEIGHT  64

struct WiiDebugOverlay {
    u8*      texData;
    GXTexObj texObj;
    bool     enabled;
};

void Wii_VideoInit();
void Wii_VideoRender(const uint16_t* srcTop, const uint16_t* srcBottom,
                     bool gbaMode = false);
void Wii_VideoFlushAsync();
void Wii_DrawCursor(float x, float y);
void Wii_DebugOverlayInit();
void Wii_DebugOverlayPrint(int line, const char* fmt, ...);
void Wii_DebugOverlaySetEnabled(bool enabled);

// ConsoleUI platform hook implementations
// createTexture: emulator framebuffer pixels (RGB5A3 uint16_t)
void* Wii_CreateTexture(uint16_t* data, int width, int height);

// createTextureRGBA8: UI art pixels (RGBA8 uint32_t — BMP icons, font)
void* Wii_CreateTextureRGBA8(uint32_t* data, int width, int height);

void  Wii_DestroyTexture(void* texture);

#endif // WII_VIDEO_H
