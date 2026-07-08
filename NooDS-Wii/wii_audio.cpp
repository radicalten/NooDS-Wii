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

#include "wii_audio.h"
#include <string.h>
#include <gccore.h>

extern "C" {
#include <tuxedo/ppc/intrinsics.h>
}

alignas(32) static int16_t s_slots[3][WIIAUD_FRAMES_PER_BUF * 2];

static volatile int  s_writeSlot = 0;
static volatile int  s_readySlot = 1;
static volatile int  s_playSlot  = 2;
static volatile bool s_freshFlag = false;

void WiiAudio_Init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    DCFlushRange(s_slots, sizeof(s_slots));
    s_writeSlot = 0;
    s_readySlot = 1;
    s_playSlot  = 2;
    s_freshFlag = false;
}

void WiiAudio_Submit(const int16_t *samples, int count)
{
    int ws = s_writeSlot;
    int frames = count < WIIAUD_FRAMES_PER_BUF ? count : WIIAUD_FRAMES_PER_BUF;
    
    memcpy(s_slots[ws], samples, frames * 4);

    if (frames < WIIAUD_FRAMES_PER_BUF) {
        memset(s_slots[ws] + frames * 2, 0, (WIIAUD_FRAMES_PER_BUF - frames) * 4);
    }

    DCFlushRange(s_slots[ws], WIIAUD_BUF_BYTES);

    PPCIrqState st = PPCIrqLockByMsr();
    int tmp     = s_readySlot;
    s_readySlot = ws;
    s_writeSlot = tmp;
    s_freshFlag = true;
    PPCIrqUnlockByMsr(st);
}

int16_t *WiiAudio_ConsumeBuffer(void)
{
    PPCIrqState st = PPCIrqLockByMsr();
    if (s_freshFlag) {
        int tmp     = s_playSlot;
        s_playSlot  = s_readySlot;
        s_readySlot = tmp;
        s_freshFlag = false;
    }
    int ps = s_playSlot;
    PPCIrqUnlockByMsr(st);

    return s_slots[ps];
}
