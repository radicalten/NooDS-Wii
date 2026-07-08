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

#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void WiiAudio_Init(void);
void WiiAudio_Submit(const int16_t *samples, int count);
int16_t *WiiAudio_ConsumeBuffer(void);

// ASND output parameters
#define WIIAUD_FRAMES_PER_BUF   1024
#define WIIAUD_BUF_BYTES        (WIIAUD_FRAMES_PER_BUF * 4)  // stereo 16-bit
#define WIIAUD_OUT_RATE         48000

// NDS SPU parameters
#define WIIAUD_NDS_RATE         32768
#define WIIAUD_NDS_FRAMES_PER_VIDEO_FRAME   547
#define WIIAUD_NDS_FRAMES       WIIAUD_NDS_FRAMES_PER_VIDEO_FRAME

#ifdef __cplusplus
}
#endif
