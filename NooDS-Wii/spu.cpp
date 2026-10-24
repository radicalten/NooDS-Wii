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

#include <cstring>
#include <algorithm>

extern "C" {
    #include <tuxedo/thread.h>
    #include <tuxedo/ppc/intrinsics.h>
    #include <tuxedo/ppc/clock.h>
}

#include "core.h"

// ---------------------------------------------------------------------------
// Static tables
// ---------------------------------------------------------------------------
const int Spu::indexTable[] = {
    -1, -1, -1, -1, 2, 4, 6, 8
};

const int16_t Spu::adpcmTable[] = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
    0x0010, 0x0011, 0x0013, 0x0015, 0x0017, 0x0019, 0x001C, 0x001F,
    0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
    0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F,
    0x009D, 0x00AD, 0x00BE, 0x00D1, 0x00E6, 0x00FD, 0x0117, 0x0133,
    0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583,
    0x0610, 0x06AB, 0x0756, 0x0812, 0x08E0, 0x09C3, 0x0ABD, 0x0BD0,
    0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
    0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B,
    0x3BB9, 0x41B2, 0x4844, 0x4F7E, 0x5771, 0x602F, 0x69CE, 0x7462,
    0x7FFF
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
Spu::Spu(Core *core) : core(core) {
    ringWrite     = 0;
    ringRead      = 0;
    lastPopSample = 0;
    memset(ringBuf,     0, sizeof(ringBuf));
    memset(&waitQueue1, 0, sizeof(waitQueue1));
    memset(&waitQueue2, 0, sizeof(waitQueue2));
}

Spu::~Spu() {
    // buffers[] are inline arrays — nothing to free.
}

// ---------------------------------------------------------------------------
// saveState
// ---------------------------------------------------------------------------
void Spu::saveState(FILE *file) {
    fwrite(&gbaFrameSequencer, sizeof(gbaFrameSequencer), 1, file);
    fwrite(gbaSoundTimers,     sizeof(int32_t),  4,  file);
    fwrite(gbaEnvelopes,       sizeof(int8_t),   3,  file);
    fwrite(gbaEnvTimers,       sizeof(int8_t),   3,  file);
    fwrite(&gbaSweepTimer,     sizeof(gbaSweepTimer),  1, file);
    fwrite(&gbaWaveDigit,      sizeof(gbaWaveDigit),   1, file);
    fwrite(&gbaNoiseValue,     sizeof(gbaNoiseValue),  1, file);
    fwrite(gbaWaveRam,         sizeof(gbaWaveRam),     1, file);
    fwrite(&gbaSampleA,        sizeof(gbaSampleA),     1, file);
    fwrite(&gbaSampleB,        sizeof(gbaSampleB),     1, file);
    fwrite(&enabled,           sizeof(enabled),        1, file);

    fwrite(adpcmValue,     sizeof(int32_t), 16, file);
    fwrite(adpcmLoopValue, sizeof(int32_t), 16, file);
    fwrite(adpcmIndex,     sizeof(int8_t),  16, file);
    fwrite(adpcmLoopIndex, sizeof(int8_t),  16, file);
    fwrite(adpcmToggle,    sizeof(bool),    16, file);

    fwrite(dutyCycles,    sizeof(uint8_t),  6,  file);
    fwrite(noiseValues,   sizeof(uint16_t), 2,  file);
    fwrite(soundCurrent,  sizeof(uint32_t), 16, file);
    fwrite(soundTimers,   sizeof(uint16_t), 16, file);
    fwrite(sndCapCurrent, sizeof(uint32_t), 2,  file);
    fwrite(sndCapTimers,  sizeof(uint16_t), 2,  file);

    fwrite(gbaSoundCntL,      sizeof(uint8_t),  2, file);
    fwrite(gbaSoundCntH,      sizeof(uint16_t), 4, file);
    fwrite(gbaSoundCntX,      sizeof(uint16_t), 4, file);
    fwrite(&gbaMainSoundCntL, sizeof(gbaMainSoundCntL), 1, file);
    fwrite(&gbaMainSoundCntH, sizeof(gbaMainSoundCntH), 1, file);
    fwrite(&gbaMainSoundCntX, sizeof(gbaMainSoundCntX), 1, file);
    fwrite(&gbaSoundBias,     sizeof(gbaSoundBias),     1, file);

    fwrite(soundCnt,  sizeof(uint32_t), 16, file);
    fwrite(soundSad,  sizeof(uint32_t), 16, file);
    fwrite(soundTmr,  sizeof(uint16_t), 16, file);
    fwrite(soundPnt,  sizeof(uint16_t), 16, file);
    fwrite(soundLen,  sizeof(uint32_t), 16, file);
    fwrite(&mainSoundCnt, sizeof(mainSoundCnt), 1, file);
    fwrite(&soundBias,    sizeof(soundBias),    1, file);
    fwrite(sndCapCnt, sizeof(uint8_t),  2, file);
    fwrite(sndCapDad, sizeof(uint32_t), 2, file);
    fwrite(sndCapLen, sizeof(uint16_t), 2, file);

    for (int i = 0; i < 2; i++) {
        uint32_t count = (uint32_t)gbaFifos[i].size();
        fwrite(&count, sizeof(count), 1, file);
        for (uint32_t j = 0; j < count; j++)
            fwrite(&gbaFifos[i][j], sizeof(int8_t), 1, file);
    }
}

// ---------------------------------------------------------------------------
// loadState
// ---------------------------------------------------------------------------
void Spu::loadState(FILE *file) {
    fread(&gbaFrameSequencer, sizeof(gbaFrameSequencer), 1, file);
    fread(gbaSoundTimers,     sizeof(int32_t),  4,  file);
    fread(gbaEnvelopes,       sizeof(int8_t),   3,  file);
    fread(gbaEnvTimers,       sizeof(int8_t),   3,  file);
    fread(&gbaSweepTimer,     sizeof(gbaSweepTimer),  1, file);
    fread(&gbaWaveDigit,      sizeof(gbaWaveDigit),   1, file);
    fread(&gbaNoiseValue,     sizeof(gbaNoiseValue),  1, file);
    fread(gbaWaveRam,         sizeof(gbaWaveRam),     1, file);
    fread(&gbaSampleA,        sizeof(gbaSampleA),     1, file);
    fread(&gbaSampleB,        sizeof(gbaSampleB),     1, file);
    fread(&enabled,           sizeof(enabled),        1, file);

    fread(adpcmValue,     sizeof(int32_t), 16, file);
    fread(adpcmLoopValue, sizeof(int32_t), 16, file);
    fread(adpcmIndex,     sizeof(int8_t),  16, file);
    fread(adpcmLoopIndex, sizeof(int8_t),  16, file);
    fread(adpcmToggle,    sizeof(bool),    16, file);

    fread(dutyCycles,    sizeof(uint8_t),  6,  file);
    fread(noiseValues,   sizeof(uint16_t), 2,  file);
    fread(soundCurrent,  sizeof(uint32_t), 16, file);
    fread(soundTimers,   sizeof(uint16_t), 16, file);
    fread(sndCapCurrent, sizeof(uint32_t), 2,  file);
    fread(sndCapTimers,  sizeof(uint16_t), 2,  file);

    fread(gbaSoundCntL,      sizeof(uint8_t),  2, file);
    fread(gbaSoundCntH,      sizeof(uint16_t), 4, file);
    fread(gbaSoundCntX,      sizeof(uint16_t), 4, file);
    fread(&gbaMainSoundCntL, sizeof(gbaMainSoundCntL), 1, file);
    fread(&gbaMainSoundCntH, sizeof(gbaMainSoundCntH), 1, file);
    fread(&gbaMainSoundCntX, sizeof(gbaMainSoundCntX), 1, file);
    fread(&gbaSoundBias,     sizeof(gbaSoundBias),     1, file);

    fread(soundCnt,  sizeof(uint32_t), 16, file);
    fread(soundSad,  sizeof(uint32_t), 16, file);
    fread(soundTmr,  sizeof(uint16_t), 16, file);
    fread(soundPnt,  sizeof(uint16_t), 16, file);
    fread(soundLen,  sizeof(uint32_t), 16, file);
    fread(&mainSoundCnt, sizeof(mainSoundCnt), 1, file);
    fread(&soundBias,    sizeof(soundBias),    1, file);
    fread(sndCapCnt, sizeof(uint8_t),  2, file);
    fread(sndCapDad, sizeof(uint32_t), 2, file);
    fread(sndCapLen, sizeof(uint16_t), 2, file);

    for (int i = 0; i < 2; i++) {
        gbaFifos[i].clear();
        uint32_t count = 0;
        fread(&count, sizeof(count), 1, file);
        for (uint32_t j = 0; j < count; j++) {
            int8_t value = 0;
            fread(&value, sizeof(value), 1, file);
            gbaFifos[i].push_back(value);
        }
    }

    // Restore the ring buffer to a clean state so drainSamples() does not
    // serve stale pre-load data.
    PPCIrqState st = PPCIrqLockByMsr();
    memset(ringBuf, 0, sizeof(ringBuf));
    ringWrite     = 0;
    ringRead      = 0;
    lastPopSample = 0;
    PPCIrqUnlockByMsr(st);
}

// ---------------------------------------------------------------------------
// getSamples
//
// Called from the audio thread.
// dst  : caller-owned buffer, must hold at least `count` uint32_t words.
// count: number of stereo sample pairs requested (≤ SPU_BUF_FRAMES).
//
// Each word is packed:  bits 15..0  = left  (int16_t)
//                       bits 31..16 = right (int16_t)
//
// Returns true  → dst filled with fresh emulator audio.
// Returns false → dst filled with the last known sample repeated (silence).
// ---------------------------------------------------------------------------
int Spu::drainSamples(uint32_t *dst, int maxCount, uint32_t *availAfter, int minCount) {
    if (maxCount <= 0) {
        if (availAfter) *availAfter = ringWrite - ringRead;
        return 0;
    }

    uint32_t n = 0;
    {
        PPCIrqState st = PPCIrqLockByMsr();
        uint32_t rd    = ringRead;
        uint32_t avail = ringWrite - rd;
        if (avail >= (uint32_t)minCount) {
            n = (avail < (uint32_t)maxCount) ? avail : (uint32_t)maxCount;
            for (uint32_t i = 0; i < n; i++)
                dst[i] = ringBuf[(rd + i) & SPU_RING_MASK];
            if (n > 0) {
                ringRead      = rd + n;
                lastPopSample = dst[n - 1];
            }
        }
        if (availAfter) *availAfter = avail - n;
        PPCIrqUnlockByMsr(st);
    }

    if (n > 0 && Settings::fpsLimiter == 1)
        KThrQueueUnblockAllByValue(&waitQueue1, 1);

    return (int)n;
}

bool Spu::getSamples(uint32_t *dst, int count) {
    if (count <= 0) return false;
    int got = drainSamples(dst, count, nullptr, count);
    if (got == 0) {
        uint32_t hold = lastPopSample;
        for (int i = 0; i < count; i++)
            dst[i] = hold;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// pushSample
//
// Called from the emulator thread (runSample / runGbaSample).
// Enqueues one packed stereo sample into the 4096-sample ring buffer and
// applies back-pressure only if the ring exceeds SPU_TARGET_LATENCY (~62 ms).
// ---------------------------------------------------------------------------
void Spu::pushSample(int16_t sampleLeft, int16_t sampleRight) {
    // v1.76: when audio emulation is off, do not enqueue or throttle — previously
    // the ring filled (audio thread never drains) and fpsLimiter==1 caused
    // 30 ms stalls per sample, dropping displayed FPS to 0 after ~62 ms.
    if (!Settings::emulateAudio)
        return;

    const uint32_t packed =
        ((uint32_t)(uint16_t)sampleRight << 16) |
        ((uint32_t)(uint16_t)sampleLeft  & 0xFFFF);

    extern volatile bool g_wiiAudioHardwareActive;
    if ((Settings::fpsLimiter == 1 && g_wiiAudioHardwareActive) || Settings::fpsLimiter == 2) {
        if ((ringWrite - ringRead) >= SPU_TARGET_LATENCY) {
            u64 start   = PPCGetTickCount();
            u64 timeout = PPCUsToTicks(Settings::fpsLimiter == 2 ? 1000000 : 30000);
            while ((ringWrite - ringRead) >= SPU_TARGET_LATENCY) {
                if (PPCGetTickCount() - start > timeout) break;
                KThreadSleepUs(250);
            }
        }
    }

    PPCIrqState st = PPCIrqLockByMsr();
    uint32_t wr = ringWrite;
    if ((wr - ringRead) >= SPU_RING_SIZE)
        ringRead = wr - (SPU_RING_SIZE - 1);
    ringBuf[wr & SPU_RING_MASK] = packed;
    ringWrite = wr + 1;
    PPCIrqUnlockByMsr(st);
}

// ---------------------------------------------------------------------------
// runGbaSample
//
// Scheduled every 512 GBA cycles (≈ 32768 Hz).
// Mixes all four legacy GBA sound channels plus the two DMA FIFOs and
// calls pushSample() with the result.
// ---------------------------------------------------------------------------
void Spu::wakeSpuSchedule() {
    if (spuSilentBatch <= 1) return;
    const SchedTask targetTask = core->gbaMode ? GBA_SPU_SAMPLE : NDS_SPU_SAMPLE;
    const uint32_t step = core->gbaMode ? 512u : 1024u;
    const uint32_t shift = core->gbaMode ? 9u : 10u;

    for (size_t idx = 0; idx < core->events.size(); idx++) {
        if (core->events[idx].task == targetTask) {
            core->events.eraseAt(idx);
            uint32_t elapsed = (core->globalCycles >= spuSilentNextCycle - spuSilentBatch * step)
                ? (core->globalCycles - (spuSilentNextCycle - spuSilentBatch * step))
                : 0u;
            uint32_t done = std::min<uint32_t>(spuSilentBatch - 1u, elapsed >> shift);
            int16_t dc = 0;
            if (Settings::emulateAudio) {
                if (core->gbaMode) {
                    int32_t bias = gbaSoundBias & 0x3FF;
                    dc = (int16_t)((std::max(0, std::min(0x3FF, bias)) - 0x200) << 6);
                } else if (Settings::audio16Bit) {
                    dc = (int16_t)(std::max(0, std::min<int32_t>(0xFFFF, soundBias << 6)) - 0x8000);
                } else {
                    dc = (int16_t)((std::max(0, std::min<int32_t>(0x3FF, soundBias)) - 0x200) << 6);
                }
            }
            for (uint32_t k = 0; k < done; k++)
                pushSample(dc, dc);
            uint32_t base = spuSilentNextCycle - spuSilentBatch * step;
            uint32_t nextCyc = base + (done + 1u) * step;
            uint32_t delta = (nextCyc > core->globalCycles) ? (nextCyc - core->globalCycles) : 1u;
            spuSilentBatch = 1;
            core->schedule(targetTask, delta);
            return;
        }
    }
    spuSilentBatch = 1;
}

void Spu::runGbaSample() {
    if (!(gbaMainSoundCntX & BIT(7))) {
        const uint32_t prevBatch = spuSilentBatch;
        spuSilentBatch = 32;
        spuSilentNextCycle = core->globalCycles + 32u * 512u;
        core->schedule(GBA_SPU_SAMPLE, 32u * 512u);

        int16_t dc = 0;
        if (Settings::emulateAudio) {
            int32_t bias = gbaSoundBias & 0x3FF;
            dc = (int16_t)((std::max(0, std::min(0x3FF, bias)) - 0x200) << 6);
        }
        for (uint32_t k = 0; k < prevBatch; k++)
            pushSample(dc, dc);
        return;
    }

    spuSilentBatch = 1;
    // Reschedule immediately so the next sample fires on time.
    core->schedule(GBA_SPU_SAMPLE, 512);

    if (!Settings::emulateAudio)
        return pushSample(0, 0);

    int32_t sampleLeft  = 0;
    int32_t sampleRight = 0;

    // Only mix when the master sound enable bit is set.
    if (gbaMainSoundCntX & BIT(7)) {

        int32_t data[4] = {};

        // ---- Channels 0 and 1: Square-wave with envelope and optional sweep ----
        for (int i = 0; i < 2; i++) {
            if (!(gbaMainSoundCntX & BIT(i)))
                continue;

            // Sweep (channel 0 only).
            if (i == 0 &&
                gbaFrameSequencer % 256 == 128 &&
                (gbaSoundCntL[0] & 0x70) &&
                --gbaSweepTimer <= 0)
            {
                uint16_t freq  = gbaSoundCntX[0] & 0x07FF;
                int      shift = gbaSoundCntL[0] & 0x07;
                int      delta = freq >> shift;
                if (gbaSoundCntL[0] & BIT(3)) delta = -delta;

                freq += delta;

                if (freq < 0x800) {
                    gbaSoundCntX[0] = (gbaSoundCntX[0] & ~0x07FF) | freq;
                    gbaSweepTimer   = (gbaSoundCntL[0] & 0x70) >> 4;
                } else {
                    // Overflow → silence this channel.
                    gbaMainSoundCntX &= ~BIT(0);
                    continue;
                }
            }

            // Advance the frequency timer (counts up from 0; overflows at 2048).
            gbaSoundTimers[i] -= 4;
            while (gbaSoundTimers[i] <= 0)
                gbaSoundTimers[i] += 2048 - (gbaSoundCntX[i] & 0x07FF);

            // Duty cycle threshold (in timer ticks above zero).
            int period = 2048 - (gbaSoundCntX[i] & 0x07FF);
            int duty;
            switch ((gbaSoundCntH[i] & 0x00C0) >> 6) {
                case 0:  duty = period * 7 / 8; break;
                case 1:  duty = period * 6 / 8; break;
                case 2:  duty = period * 4 / 8; break;
                case 3:  duty = period * 2 / 8; break;
                default: duty = 0;              break;
            }

            data[i] = (gbaSoundTimers[i] < duty) ? -0x80 : 0x80;

            // Length counter.
            if (gbaFrameSequencer % 128 == 0 &&
                (gbaSoundCntX[i] & BIT(14)) &&
                (gbaSoundCntH[i] & 0x003F))
            {
                gbaSoundCntH[i] = (gbaSoundCntH[i] & ~0x003F) |
                                  ((gbaSoundCntH[i] & 0x003F) - 1);
                if ((gbaSoundCntH[i] & 0x003F) == 0)
                    gbaMainSoundCntX &= ~BIT(i);
            }

            // Volume envelope (ticks at frame-sequencer step 448).
            if (gbaFrameSequencer == 448 && --gbaEnvTimers[i] <= 0) {
                if (gbaEnvTimers[i] == 0) {
                    if ((gbaSoundCntH[i] & BIT(11)) && gbaEnvelopes[i] < 15)
                        gbaEnvelopes[i]++;
                    else if (!(gbaSoundCntH[i] & BIT(11)) && gbaEnvelopes[i] > 0)
                        gbaEnvelopes[i]--;
                } else {
                    // Reload envelope volume from initial-volume field.
                    gbaEnvelopes[i] = (gbaSoundCntH[i] & 0xF000) >> 12;
                }
                gbaEnvTimers[i] = (gbaSoundCntH[i] & 0x0700) >> 8;
            }

            data[i] = data[i] * gbaEnvelopes[i] / 15;
        }

        // ---- Channel 2: Wave ----
        if ((gbaMainSoundCntX & BIT(2)) && (gbaSoundCntL[1] & BIT(7))) {
            gbaSoundTimers[2] -= 64;
            while (gbaSoundTimers[2] <= 0) {
                gbaSoundTimers[2] += 2048 - (gbaSoundCntX[2] & 0x07FF);
                gbaWaveDigit = (gbaWaveDigit + 1) % 64;
            }

            // Select which wave-RAM bank to read from.
            int bank = (gbaSoundCntL[1] & BIT(6)) >> 6;
            if ((gbaSoundCntL[1] & BIT(5)) && gbaWaveDigit >= 32)
                bank = !bank;

            // Each byte in wave-RAM holds two 4-bit samples.
            data[2] = gbaWaveRam[bank][(gbaWaveDigit % 32) / 2];
            if (gbaWaveDigit & 1)
                data[2] &= 0x0F;
            else
                data[2] >>= 4;

            // Length counter.
            if (gbaFrameSequencer % 128 == 0 &&
                (gbaSoundCntX[2] & BIT(14)) &&
                (gbaSoundCntH[2] & 0x00FF))
            {
                gbaSoundCntH[2] = (gbaSoundCntH[2] & ~0x00FF) |
                                  ((gbaSoundCntH[2] & 0x00FF) - 1);
                if ((gbaSoundCntH[2] & 0x00FF) == 0)
                    gbaMainSoundCntX &= ~BIT(2);
            }

            // Volume shift.
            switch ((gbaSoundCntH[2] & 0xE000) >> 13) {
                case 0:  data[2] >>= 4; break;   // mute
                case 1:  data[2] >>= 0; break;   // 100 %
                case 2:  data[2] >>= 1; break;   //  50 %
                case 3:  data[2] >>= 2; break;   //  25 %
                default: data[2]  = data[2] * 3 / 4; break;  // 75 %
            }

            // Scale to same range as the square channels.
            data[2] = data[2] * 0x100 / 0xF;
        }

        // ---- Channel 3: Noise ----
        if (gbaMainSoundCntX & BIT(3)) {
            gbaSoundTimers[3] -= 16;
            while (gbaSoundTimers[3] <= 0) {
                int divisor = (gbaSoundCntX[3] & 0x0007) * 16;
                if (divisor == 0) divisor = 8;
                gbaSoundTimers[3] +=
                    divisor << ((gbaSoundCntX[3] & 0x00F0) >> 4);

                // LFSR step.
                gbaNoiseValue &= ~BIT(15);
                if (gbaNoiseValue & BIT(0)) {
                    uint16_t xorMask = (gbaSoundCntH[3] & BIT(3)) ? 0x60 : 0x6000;
                    gbaNoiseValue = (uint16_t)(BIT(15) |
                                   ((gbaNoiseValue >> 1) ^ xorMask));
                } else {
                    gbaNoiseValue >>= 1;
                }
            }

            data[3] = (gbaNoiseValue & BIT(15)) ? 0x80 : -0x80;

            // Length counter.
            if (gbaFrameSequencer % 128 == 0 &&
                (gbaSoundCntX[3] & BIT(14)) &&
                (gbaSoundCntH[3] & 0x003F))
            {
                gbaSoundCntH[3] = (gbaSoundCntH[3] & ~0x003F) |
                                  ((gbaSoundCntH[3] & 0x003F) - 1);
                if ((gbaSoundCntH[3] & 0x003F) == 0)
                    gbaMainSoundCntX &= ~BIT(3);
            }

            // Volume envelope (shares gbaEnvelopes[2] and gbaEnvTimers[2]).
            if (gbaFrameSequencer == 448 && --gbaEnvTimers[2] <= 0) {
                if (gbaEnvTimers[2] == 0) {
                    if ((gbaSoundCntH[3] & BIT(11)) && gbaEnvelopes[2] < 15)
                        gbaEnvelopes[2]++;
                    else if (!(gbaSoundCntH[3] & BIT(11)) && gbaEnvelopes[2] > 0)
                        gbaEnvelopes[2]--;
                } else {
                    gbaEnvelopes[2] = (gbaSoundCntH[3] & 0xF000) >> 12;
                }
                gbaEnvTimers[2] = (gbaSoundCntH[3] & 0x0700) >> 8;
            }

            data[3] = data[3] * gbaEnvelopes[2] / 15;
        }

        // ---- Mix legacy channels into left/right ----
        for (int i = 0; i < 4; i++) {
            // Master volume shift for legacy channels.
            switch (gbaMainSoundCntH & 0x0003) {
                case 0: data[i] >>= 2; break;   // 25 %
                case 1: data[i] >>= 1; break;   // 50 %
                case 2: data[i] >>= 0; break;   // 100 %
                // case 3: prohibited by hardware — leave as-is
            }

            if (gbaMainSoundCntL & BIT(12 + i))
                sampleLeft  += data[i] * ((gbaMainSoundCntL & 0x0070) >> 4) / 7;
            if (gbaMainSoundCntL & BIT(8 + i))
                sampleRight += data[i] * ( gbaMainSoundCntL & 0x0007)       / 7;
        }

        // ---- Mix DMA FIFO A ----
        if (gbaMainSoundCntH & BIT(9))
            sampleLeft  += gbaSampleA << ((gbaMainSoundCntH & BIT(2)) ? 2 : 1);
        if (gbaMainSoundCntH & BIT(8))
            sampleRight += gbaSampleA << ((gbaMainSoundCntH & BIT(2)) ? 2 : 1);

        // ---- Mix DMA FIFO B ----
        if (gbaMainSoundCntH & BIT(13))
            sampleLeft  += gbaSampleB << ((gbaMainSoundCntH & BIT(3)) ? 2 : 1);
        if (gbaMainSoundCntH & BIT(12))
            sampleRight += gbaSampleB << ((gbaMainSoundCntH & BIT(3)) ? 2 : 1);

        // Advance the frame sequencer (0–511).
        gbaFrameSequencer = (gbaFrameSequencer + 1) % 512;
    }

    // Apply bias, clamp to 10-bit range, then scale to 16-bit.
    int32_t bias = gbaSoundBias & 0x3FF;
    sampleLeft  = (std::max(0, std::min(0x3FF, sampleLeft  + bias)) - 0x200) << 6;
    sampleRight = (std::max(0, std::min(0x3FF, sampleRight + bias)) - 0x200) << 6;

    pushSample((int16_t)sampleLeft, (int16_t)sampleRight);
}

// ---------------------------------------------------------------------------
// runSample
//
// Scheduled every 1024 ARM7 cycles (512 * 2) → 32768 Hz.
// Mixes all 16 NDS SPU channels, runs the two capture units, applies master
// volume and bias, and calls pushSample() with the final stereo pair.
// ---------------------------------------------------------------------------
void Spu::runSample() {
    if (!enabled && !(sndCapCnt[0] & BIT(7)) && !(sndCapCnt[1] & BIT(7))) {
        const uint32_t prevBatch = spuSilentBatch;
        spuSilentBatch = 32;
        spuSilentNextCycle = core->globalCycles + 32u * 1024u;
        core->schedule(NDS_SPU_SAMPLE, 32u * 1024u);

        int16_t dc = 0;
        if (Settings::emulateAudio) {
            if (Settings::audio16Bit)
                dc = (int16_t)(std::max(0, std::min<int32_t>(0xFFFF, soundBias << 6)) - 0x8000);
            else
                dc = (int16_t)((std::max(0, std::min<int32_t>(0x3FF, soundBias)) - 0x200) << 6);
        }
        for (uint32_t k = 0; k < prevBatch; k++)
            pushSample(dc, dc);
        return;
    }

    spuSilentBatch = 1;
    core->schedule(NDS_SPU_SAMPLE, 512 * 2);

    if (!Settings::emulateAudio)
        return pushSample(0, 0);

    int64_t mixerLeft  = 0, mixerRight = 0;
    int64_t channelsLeft[2]  = {};
    int64_t channelsRight[2] = {};

    // ---- Per-channel mixing ----
    for (int i = 0; enabled >> i; i++) {
        if (!(enabled & BIT(i))) continue;

        uint8_t format = (soundCnt[i] >> 29) & 0x3;
        int64_t data   = 0;

        // Read current sample for this channel.
        switch (format) {
            case 0:  // 8-bit PCM
                data = (int8_t)core->memory.read<uint8_t>(1, soundCurrent[i]) << 8;
                break;
            case 1:  // 16-bit PCM
                data = (int16_t)core->memory.read<uint16_t>(1, soundCurrent[i]);
                break;
            case 2:  // IMA-ADPCM (current decoded value updated below)
                data = adpcmValue[i];
                break;
            case 3:  // PSG duty / noise (no memory read needed)
                if (i >= 8 && i <= 13) {
                    uint8_t duty = 7 - ((soundCnt[i] & 0x07000000) >> 24);
                    data = (dutyCycles[i - 8] < duty) ? -0x7FFF : 0x7FFF;
                } else if (i >= 14) {
                    data = (noiseValues[i - 14] & BIT(15)) ? -0x7FFF : 0x7FFF;
                }
                break;
        }

        // Advance the channel timer; each overflow fetches the next data unit.
        soundTimers[i] += 512;
        bool overflow   = (soundTimers[i] < 512);   // wrapped past 0xFFFF

        while (overflow) {
            soundTimers[i] += soundTmr[i];
            overflow         = (soundTimers[i] < soundTmr[i]);

            switch (format) {
                case 0:  // 8-bit: advance by 1 byte
                    soundCurrent[i] += 1;
                    break;

                case 1:  // 16-bit: advance by 2 bytes
                    soundCurrent[i] += 2;
                    break;

                case 2: {  // ADPCM: decode next nibble
                    // Save loop-point state at the loop address boundary.
                    if (soundCurrent[i] == soundSad[i] + soundPnt[i] * 4 &&
                        !adpcmToggle[i])
                    {
                        adpcmLoopValue[i] = adpcmValue[i];
                        adpcmLoopIndex[i] = adpcmIndex[i];
                    }

                    uint8_t byte = core->memory.read<uint8_t>(1, soundCurrent[i]);
                    uint8_t nibble = adpcmToggle[i]
                                     ? ((byte & 0xF0) >> 4)
                                     : ( byte & 0x0F);

                    // IMA step.
                    int32_t step = adpcmTable[adpcmIndex[i]];
                    int32_t diff = step / 8;
                    if (nibble & BIT(0)) diff += step / 4;
                    if (nibble & BIT(1)) diff += step / 2;
                    if (nibble & BIT(2)) diff += step;

                    if (nibble & BIT(3)) {
                        adpcmValue[i] -= diff;
                        if (adpcmValue[i] < -0x7FFF) adpcmValue[i] = -0x7FFF;
                    } else {
                        adpcmValue[i] += diff;
                        if (adpcmValue[i] >  0x7FFF) adpcmValue[i] =  0x7FFF;
                    }

                    adpcmIndex[i] += indexTable[nibble & 0x7];
                    if (adpcmIndex[i] <  0) adpcmIndex[i] =  0;
                    if (adpcmIndex[i] > 88) adpcmIndex[i] = 88;

                    adpcmToggle[i] = !adpcmToggle[i];
                    if (!adpcmToggle[i]) soundCurrent[i]++;   // consumed both nibbles
                    break;
                }

                case 3:  // PSG: step the generator
                    if (i >= 8 && i <= 13) {
                        dutyCycles[i - 8] = (dutyCycles[i - 8] + 1) % 8;
                    } else if (i >= 14) {
                        noiseValues[i - 14] &= ~BIT(15);
                        if (noiseValues[i - 14] & BIT(0))
                            noiseValues[i - 14] = (uint16_t)(
                                BIT(15) | ((noiseValues[i - 14] >> 1) ^ 0x6000));
                        else
                            noiseValues[i - 14] >>= 1;
                    }
                    break;
            }

            // End-of-data check (not applicable to PSG channels).
            if (format != 3 &&
                soundCurrent[i] >= soundSad[i] + (soundPnt[i] + soundLen[i]) * 4)
            {
                uint8_t repeat = (soundCnt[i] & 0x18000000) >> 27;
                if (repeat == 1) {
                    // Loop: jump back to loop-start address.
                    soundCurrent[i] = soundSad[i] + soundPnt[i] * 4;
                    if (format == 2) {
                        adpcmValue[i]  = adpcmLoopValue[i];
                        adpcmIndex[i]  = adpcmLoopIndex[i];
                        adpcmToggle[i] = false;
                    }
                } else {
                    // One-shot: disable this channel.
                    soundCnt[i] &= ~BIT(31);
                    enabled     &= ~BIT(i);
                    break;
                }
            }
        }

        // Apply volume divider (bits 9-8 of soundCnt).
        int divShift = (soundCnt[i] & 0x00000300) >> 8;
        if (divShift == 3) divShift++;          // value 3 → shift by 4
        data <<= (4 - divShift);

        // Apply per-channel volume multiplier (bits 6-0 of soundCnt).
        int mulFactor = soundCnt[i] & 0x0000007F;
        if (mulFactor == 127) mulFactor++;      // 127 → 128 (full volume)
        data = (data << 7) * mulFactor / 128;

        // Apply panning (bits 22-16 of soundCnt).
        int pan = (soundCnt[i] & 0x007F0000) >> 16;
        if (pan == 127) pan++;                  // 127 → 128 (full right)
        int64_t dataLeft  = (data * (128 - pan) / 128) >> 3;
        int64_t dataRight = (data *          pan / 128) >> 3;

        // Channels 1 and 3 feed the capture units and can be excluded from
        // the main mixer if the capture-add bit is clear.
        if (i == 1 || i == 3) {
            channelsLeft[i >> 1]  = dataLeft;
            channelsRight[i >> 1] = dataRight;
            if (mainSoundCnt & BIT(12 + (i >> 1)))
                continue;   // capture-add: don't also add to main mixer
        }

        mixerLeft  += dataLeft;
        mixerRight += dataRight;
    }

    // ---- Capture units ----
    for (int i = 0; i < 2; i++) {
        if (!(sndCapCnt[i] & BIT(7))) continue;

        sndCapTimers[i] += 512;
        bool overflow    = (sndCapTimers[i] < 512);

        while (overflow) {
            sndCapTimers[i] += soundTmr[1 + (i << 1)];
            overflow          = (sndCapTimers[i] < soundTmr[1 + (i << 1)]);

            int64_t sample = (i == 0) ? mixerLeft : mixerRight;
            if (sample >  0x7FFFFF) sample =  0x7FFFFF;
            if (sample < -0x800000) sample = -0x800000;

            if (sndCapCnt[i] & BIT(3)) {
                // 8-bit capture.
                core->memory.write<uint8_t>(1, sndCapCurrent[i],
                                            (uint8_t)(sample >> 16));
                sndCapCurrent[i] += 1;
            } else {
                // 16-bit capture.
                core->memory.write<uint16_t>(1, sndCapCurrent[i],
                                             (uint16_t)(sample >> 8));
                sndCapCurrent[i] += 2;
            }

            if (sndCapCurrent[i] >= sndCapDad[i] + sndCapLen[i] * 4) {
                if (sndCapCnt[i] & BIT(2)) {
                    // One-shot: stop.
                    sndCapCnt[i] &= ~BIT(7);
                    break;
                } else {
                    // Loop: wrap to start.
                    sndCapCurrent[i] = sndCapDad[i];
                }
            }
        }
    }

    // ---- Final output selection (mainSoundCnt bits 11-8) ----
    int64_t sampleLeft;
    switch ((mainSoundCnt & 0x0300) >> 8) {
        case 0:  sampleLeft = mixerLeft;                            break;
        case 1:  sampleLeft = channelsLeft[0];                      break;
        case 2:  sampleLeft = channelsLeft[1];                      break;
        case 3:  sampleLeft = channelsLeft[0] + channelsLeft[1];    break;
        default: sampleLeft = mixerLeft;                            break;
    }

    int64_t sampleRight;
    switch ((mainSoundCnt & 0x0C00) >> 10) {
        case 0:  sampleRight = mixerRight;                           break;
        case 1:  sampleRight = channelsRight[0];                     break;
        case 2:  sampleRight = channelsRight[1];                     break;
        case 3:  sampleRight = channelsRight[0] + channelsRight[1];  break;
        default: sampleRight = mixerRight;                           break;
    }

    // ---- Master volume ----
    int masterVol = mainSoundCnt & 0x007F;
    if (masterVol == 127) masterVol++;
    sampleLeft  = (sampleLeft  * masterVol / 128) >> 8;
    sampleRight = (sampleRight * masterVol / 128) >> 8;

    // ---- Bias and clamp ----
    if (Settings::audio16Bit) {
        sampleLeft  = std::max(0, std::min<int32_t>(0xFFFF,
                        (int32_t)sampleLeft  + (soundBias << 6))) - 0x8000;
        sampleRight = std::max(0, std::min<int32_t>(0xFFFF,
                        (int32_t)sampleRight + (soundBias << 6))) - 0x8000;
    } else {
        sampleLeft  = (std::max(0, std::min<int32_t>(0x3FF,
                        (int32_t)(sampleLeft  >> 6) + soundBias)) - 0x200) << 6;
        sampleRight = (std::max(0, std::min<int32_t>(0x3FF,
                        (int32_t)(sampleRight >> 6) + soundBias)) - 0x200) << 6;
    }

    pushSample((int16_t)sampleLeft, (int16_t)sampleRight);
}

// ---------------------------------------------------------------------------
// startChannel
//
// Initialises per-channel state when a channel is enabled via soundCnt or
// mainSoundCnt.  Called from writeSoundCnt, writeSoundSad, writeMainSoundCnt.
// ---------------------------------------------------------------------------
void Spu::startChannel(int channel) {
    soundCurrent[channel] = soundSad[channel];
    soundTimers[channel]  = soundTmr[channel];

    switch ((soundCnt[channel] & 0x60000000) >> 29) {
        case 2: {
            // ADPCM: read the 4-byte header at the start address.
            uint32_t header      = core->memory.read<uint32_t>(1, soundSad[channel]);
            adpcmValue[channel]  = (int16_t)(header & 0xFFFF);
            adpcmIndex[channel]  = (header & 0x007F0000) >> 16;
            if (adpcmIndex[channel] > 88) adpcmIndex[channel] = 88;
            adpcmToggle[channel] = false;
            soundCurrent[channel] += 4;   // skip the header word
            break;
        }
        case 3:
            if (channel >= 8 && channel <= 13)
                dutyCycles[channel - 8]   = 0;
            else if (channel >= 14)
                noiseValues[channel - 14] = 0x7FFF;
            break;
    }

    enabled |= BIT(channel);
    wakeSpuSchedule();
}

// ---------------------------------------------------------------------------
// gbaFifoTimer
//
// Called by the timer overflow logic when timer 0 or 1 fires.
// Advances the FIFO sample pointers and triggers DMA refill as needed.
// ---------------------------------------------------------------------------
void Spu::gbaFifoTimer(int timer) {
    // FIFO A is driven by the timer selected in bit 10 of gbaMainSoundCntH.
    if (((gbaMainSoundCntH & BIT(10)) >> 10) == timer) {
        if (!gbaFifos[0].empty()) {
            gbaSampleA = gbaFifos[0].front();
            gbaFifos[0].pop_front();
        }
        if (gbaFifos[0].size() <= 16)
            core->dma[1].trigger(3, 0x02);
    }

    // FIFO B is driven by the timer selected in bit 14 of gbaMainSoundCntH.
    if (((gbaMainSoundCntH & BIT(14)) >> 14) == timer) {
        if (!gbaFifos[1].empty()) {
            gbaSampleB = gbaFifos[1].front();
            gbaFifos[1].pop_front();
        }
        if (gbaFifos[1].size() <= 16)
            core->dma[1].trigger(3, 0x04);
    }
}

// ---------------------------------------------------------------------------
// GBA register writers
// ---------------------------------------------------------------------------
void Spu::writeGbaSoundCntL(int channel, uint8_t value) {
    if (!(gbaMainSoundCntX & BIT(7))) return;
    // Channel 0 exposes all 7 bits; channel 1 only the wave-RAM control bits.
    uint8_t mask = (channel == 0) ? 0x7F : 0xE0;
    gbaSoundCntL[channel / 2] = (gbaSoundCntL[channel / 2] & ~mask) | (value & mask);
    if (channel != 0 && !(gbaSoundCntL[1] & BIT(7)))
        gbaMainSoundCntX &= ~BIT(2);
}

void Spu::writeGbaSoundCntH(int channel, uint16_t mask, uint16_t value) {
    if (!(gbaMainSoundCntX & BIT(7))) return;
    switch (channel) {
        case 2: mask &= 0xE0FF; break;   // wave: no duty, no envelope
        case 3: mask &= 0xFF3F; break;   // noise: no frequency
    }
    gbaSoundCntH[channel] = (gbaSoundCntH[channel] & ~mask) | (value & mask);
    if (channel != 2 && !(gbaSoundCntH[channel] & 0xF800))
        gbaMainSoundCntX &= ~BIT(channel);
}

void Spu::writeGbaSoundCntX(int channel, uint16_t mask, uint16_t value) {
    if (!(gbaMainSoundCntX & BIT(7))) return;
    mask &= (channel == 3) ? 0x40FF : 0x47FF;
    gbaSoundCntX[channel] = (gbaSoundCntX[channel] & ~mask) | (value & mask);

    if (!(value & BIT(15))) return;

    bool dacOn = (channel == 2) ? ((gbaSoundCntL[1] & BIT(7)) != 0) : ((gbaSoundCntH[channel] & 0xF800) != 0);
    if (dacOn)
        gbaMainSoundCntX |= BIT(channel);
    switch (channel) {
        case 0:
            gbaSweepTimer   = (gbaSoundCntL[0] & 0x70) >> 4;
            // fall through
        case 1:
            gbaEnvelopes[channel] = (gbaSoundCntH[channel] & 0xF000) >> 12;
            gbaEnvTimers[channel] = (gbaSoundCntH[channel] & 0x0700) >> 8;
            gbaSoundTimers[channel] = 2048 - (gbaSoundCntX[channel] & 0x07FF);
            break;
        case 2:
            gbaWaveDigit    = 0;
            gbaSoundTimers[2] = 2048 - (gbaSoundCntX[2] & 0x07FF);
            break;
        case 3: {
            gbaNoiseValue   = (gbaSoundCntH[3] & BIT(3)) ? 0x40 : 0x4000;
            gbaEnvelopes[2] = (gbaSoundCntH[3] & 0xF000) >> 12;
            gbaEnvTimers[2] = (gbaSoundCntH[3] & 0x0700) >> 8;
            int divisor     = (gbaSoundCntX[3] & 0x0007) * 16;
            if (divisor == 0) divisor = 8;
            gbaSoundTimers[3] = divisor << ((gbaSoundCntX[3] & 0x00F0) >> 4);
            break;
        }
    }
}

void Spu::writeGbaMainSoundCntL(uint16_t mask, uint16_t value) {
    if (!(gbaMainSoundCntX & BIT(7))) return;
    mask &= 0xFF77;
    gbaMainSoundCntL = (gbaMainSoundCntL & ~mask) | (value & mask);
}

void Spu::writeGbaMainSoundCntH(uint16_t mask, uint16_t value) {
    mask &= 0x770F;
    gbaMainSoundCntH = (gbaMainSoundCntH & ~mask) | (value & mask);
    if (value & BIT(11)) gbaFifos[0].clear();
    if (value & BIT(15)) gbaFifos[1].clear();
}

void Spu::writeGbaMainSoundCntX(uint8_t value) {
    const bool wasEnabled = (gbaMainSoundCntX & BIT(7)) != 0;
    gbaMainSoundCntX = (gbaMainSoundCntX & ~0x80) | (value & 0x80);

    if (!(gbaMainSoundCntX & BIT(7))) {
        // Power off: silence all legacy channels.
        for (int i = 0; i < 2; i++) gbaSoundCntL[i] = 0;
        for (int i = 0; i < 4; i++) { gbaSoundCntH[i] = 0; gbaSoundCntX[i] = 0; }
        gbaMainSoundCntL  = 0;
        gbaMainSoundCntX &= ~0x0F;   // clear channel-active bits
        gbaFrameSequencer = 0;
    } else if (!wasEnabled) {
        wakeSpuSchedule();
    }
}

void Spu::writeGbaSoundBias(uint16_t mask, uint16_t value) {
    mask &= 0xC3FE;
    gbaSoundBias = (gbaSoundBias & ~mask) | (value & mask);
}

void Spu::writeGbaWaveRam(int index, uint8_t value) {
    // Write to the bank that is NOT currently playing.
    gbaWaveRam[!(gbaSoundCntL[1] & BIT(6))][index] = value;
}

void Spu::writeGbaFifoA(uint32_t mask, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        if (gbaFifos[0].size() < 32 && (mask & (0xFF << shift)))
            gbaFifos[0].push_back((int8_t)(value >> shift));
}

void Spu::writeGbaFifoB(uint32_t mask, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        if (gbaFifos[1].size() < 32 && (mask & (0xFF << shift)))
            gbaFifos[1].push_back((int8_t)(value >> shift));
}

// ---------------------------------------------------------------------------
// NDS register writers
// ---------------------------------------------------------------------------
void Spu::writeSoundCnt(int channel, uint32_t mask, uint32_t value) {
    bool enable = !(soundCnt[channel] & BIT(31)) && (value & mask & BIT(31));

    mask &= 0xFF7F837F;
    soundCnt[channel] = (soundCnt[channel] & ~mask) | (value & mask);

    if (enable && (mainSoundCnt & BIT(15)) &&
        (soundSad[channel] != 0 ||
         ((soundCnt[channel] & 0x60000000) >> 29) == 3))
    {
        startChannel(channel);
    }
    else if (!(soundCnt[channel] & BIT(31))) {
        enabled &= ~BIT(channel);
    }
}

void Spu::writeSoundSad(int channel, uint32_t mask, uint32_t value) {
    mask &= 0x07FFFFFC;
    soundSad[channel] = (soundSad[channel] & ~mask) | (value & mask);

    // For non-PSG channels: starting the channel requires a valid address.
    if (((soundCnt[channel] & 0x60000000) >> 29) != 3) {
        if (soundSad[channel] != 0 &&
            (mainSoundCnt & BIT(15)) &&
            (soundCnt[channel] & BIT(31)))
        {
            startChannel(channel);
        } else {
            enabled &= ~BIT(channel);
        }
    }
}

void Spu::writeSoundTmr(int channel, uint16_t mask, uint16_t value) {
    soundTmr[channel] = (soundTmr[channel] & ~mask) | (value & mask);
}

void Spu::writeSoundPnt(int channel, uint16_t mask, uint16_t value) {
    soundPnt[channel] = (soundPnt[channel] & ~mask) | (value & mask);
}

void Spu::writeSoundLen(int channel, uint32_t mask, uint32_t value) {
    mask &= 0x003FFFFF;
    soundLen[channel] = (soundLen[channel] & ~mask) | (value & mask);
}

void Spu::writeMainSoundCnt(uint16_t mask, uint16_t value) {
    bool enable = !(mainSoundCnt & BIT(15)) && (value & BIT(15));
    mask &= 0xBF7F;
    mainSoundCnt = (mainSoundCnt & ~mask) | (value & mask);

    if (enable) {
        // Start all channels that were already configured.
        for (int i = 0; i < 16; i++) {
            if ((soundCnt[i] & BIT(31)) &&
                (soundSad[i] != 0 ||
                 ((soundCnt[i] & 0x60000000) >> 29) == 3))
            {
                startChannel(i);
            }
        }
    } else if (!(mainSoundCnt & BIT(15))) {
        enabled = 0;
    }
}

void Spu::writeSoundBias(uint16_t mask, uint16_t value) {
    mask &= 0x03FF;
    soundBias = (soundBias & ~mask) | (value & mask);
}

void Spu::writeSndCapCnt(int channel, uint8_t value) {
    if (!(sndCapCnt[channel] & BIT(7)) && (value & BIT(7))) {
        // Starting capture: reset the capture address and timer.
        sndCapCurrent[channel] = sndCapDad[channel];
        sndCapTimers[channel]  = soundTmr[1 + (channel << 1)];
        sndCapCnt[channel] = value & 0x8F;
        wakeSpuSchedule();
        return;
    }
    sndCapCnt[channel] = value & 0x8F;
}

void Spu::writeSndCapDad(int channel, uint32_t mask, uint32_t value) {
    mask &= 0x07FFFFFC;
    sndCapDad[channel] = (sndCapDad[channel] & ~mask) | (value & mask);
    // Reset current pointer whenever the base address changes.
    sndCapCurrent[channel] = sndCapDad[channel];
    sndCapTimers[channel]  = soundTmr[1 + (channel << 1)];
}

void Spu::writeSndCapLen(int channel, uint16_t mask, uint16_t value) {
    sndCapLen[channel] = (sndCapLen[channel] & ~mask) | (value & mask);
}

// ---------------------------------------------------------------------------
// GBA register readers
// ---------------------------------------------------------------------------
uint8_t Spu::readGbaSoundCntL(int channel) {
    return gbaSoundCntL[channel / 2];
}

uint16_t Spu::readGbaSoundCntH(int channel) {
    // Write-only fields return 0.
    uint16_t mask = (channel == 2) ? ~0x00FFu : ~0x003Fu;
    return gbaSoundCntH[channel] & mask;
}

uint16_t Spu::readGbaSoundCntX(int channel) {
    // Frequency bits (10-0) are write-only on channels 0-2; channel 3 noise clock bits (7-0) are readable.
    return gbaSoundCntX[channel] & (uint16_t)((channel == 3) ? 0x40FFu : 0x4000u);
}

uint8_t Spu::readGbaWaveRam(int index) {
    // Read from the bank that is NOT currently playing.
    return gbaWaveRam[!(gbaSoundCntL[1] & BIT(6))][index];
}
