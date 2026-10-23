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

#ifdef __cplusplus
extern "C" {
#endif

// Wii Flipper/Hollywood hardware AI DMA configuration:
// 1,024 big-endian 16-bit stereo frames per DMA buffer at 48,000 Hz
// -> 4,096 bytes/slice = 46.875 DMA callbacks/sec (21.3333 ms/slice).
// Exact NDS/GBA 32,768 Hz SPU frames per slice = 32768 / 46.875 = 699.0507.
#define WIIAUD_OUT_RATE           48000
#define WIIAUD_FRAMES_PER_BUF     1024
#define WIIAUD_BUF_BYTES_STEREO   (WIIAUD_FRAMES_PER_BUF * 4)   // 4096 bytes (multiple of 32B)
#define WIIAUD_BUF_BYTES_MONO     (WIIAUD_FRAMES_PER_BUF * 2)

#define WIIAUD_NDS_RATE              32768
#define WIIAUD_NDS_FRAMES_PER_VFRAME 699
#define WIIAUD_NDS_FRAMES            WIIAUD_NDS_FRAMES_PER_VFRAME
#define WIIAUD_MAX_SPU_FRAMES        704

#define WIIAUD_GBA_RATE              32768
#define WIIAUD_GBA_FRAMES_PER_VFRAME 699
#define WIIAUD_GBA_FRAMES            WIIAUD_GBA_FRAMES_PER_VFRAME

#ifdef __cplusplus
static_assert(WIIAUD_NDS_FRAMES == WIIAUD_GBA_FRAMES,
    "NDS and GBA frame counts must match");
#endif

#ifdef __cplusplus
}
#endif
