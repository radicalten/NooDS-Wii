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

#define WIIAUD_OUT_RATE         48000
#define WIIAUD_FRAMES_PER_BUF   1024
#define WIIAUD_BUF_BYTES_STEREO (WIIAUD_FRAMES_PER_BUF * 4)
#define WIIAUD_BUF_BYTES_MONO   (WIIAUD_FRAMES_PER_BUF * 2)

#define WIIAUD_NDS_RATE              32768
#define WIIAUD_NDS_FRAMES_PER_VFRAME 548
#define WIIAUD_NDS_FRAMES            WIIAUD_NDS_FRAMES_PER_VFRAME

#define WIIAUD_GBA_RATE              32768
#define WIIAUD_GBA_FRAMES_PER_VFRAME 549
#define WIIAUD_GBA_FRAMES            WIIAUD_GBA_FRAMES_PER_VFRAME

#ifdef __cplusplus
}
#endif
