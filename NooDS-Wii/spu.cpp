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

const int Spu::indexTable[] = {
    -1, -1, -1, -1, 2, 4, 6, 8
};

const int16_t Spu::adpcmTable[] = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x0010, 0x0011, 0x0013, 0x0015,
    0x0017, 0x0019, 0x001C, 0x001F, 0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
    0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F, 0x009D, 0x00AD, 0x00BE, 0x00D1,
    0x00E6, 0x00FD, 0x0117, 0x0133, 0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583, 0x0610, 0x06AB, 0x0756, 0x0812,
    0x08E0, 0x09C3, 0x0ABD, 0x0BD0, 0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
    0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B, 0x3BB9, 0x41B2, 0x4844, 0x4F7E,
    0x5771, 0x602F, 0x69CE, 0x7462, 0x7FFF
};

Spu::Spu(Core *core): core(core) {
    ready = false;
    memset(&waitQueue1, 0, sizeof(waitQueue1));
    memset(&waitQueue2, 0, sizeof(waitQueue2));
}

Spu::~Spu() {
    delete[] bufferIn;
    delete[] bufferOut;
}

void Spu::saveState(FILE *file) {
    fwrite(&gbaFrameSequencer, sizeof(gbaFrameSequencer), 1, file);
    fwrite(gbaSoundTimers, 4, sizeof(gbaSoundTimers) / 4, file);
    fwrite(gbaEnvelopes, 1, sizeof(gbaEnvelopes), file);
    fwrite(gbaEnvTimers, 1, sizeof(gbaEnvTimers), file);
    fwrite(&gbaSweepTimer, sizeof(gbaSweepTimer), 1, file);
    fwrite(&gbaWaveDigit, sizeof(gbaWaveDigit), 1, file);
    fwrite(&gbaNoiseValue, sizeof(gbaNoiseValue), 1, file);
    fwrite(gbaWaveRam, 1, sizeof(gbaWaveRam), file);
    fwrite(&gbaSampleA, sizeof(gbaSampleA), 1, file);
    fwrite(&gbaSampleB, sizeof(gbaSampleB), 1, file);
    fwrite(&enabled, sizeof(enabled), 1, file);
    fwrite(adpcmValue, 4, sizeof(adpcmValue) / 4, file);
    fwrite(adpcmLoopValue, 4, sizeof(adpcmLoopValue) / 4, file);
    fwrite(adpcmIndex, 1, sizeof(adpcmIndex), file);
    fwrite(adpcmLoopIndex, 1, sizeof(adpcmLoopIndex), file);
    fwrite(adpcmToggle, sizeof(bool), sizeof(adpcmToggle) / sizeof(bool), file);
    fwrite(dutyCycles, 1, sizeof(dutyCycles), file);
    fwrite(noiseValues, 2, sizeof(noiseValues) / 2, file);
    fwrite(soundCurrent, 4, sizeof(soundCurrent) / 4, file);
    fwrite(soundTimers, 2, sizeof(soundTimers) / 2, file);
    fwrite(sndCapCurrent, 4, sizeof(sndCapCurrent) / 4, file);
    fwrite(sndCapTimers, 2, sizeof(sndCapTimers) / 2, file);
    fwrite(gbaSoundCntL, 1, sizeof(gbaSoundCntL), file);
    fwrite(gbaSoundCntH, 2, sizeof(gbaSoundCntH) / 2, file);
    fwrite(gbaSoundCntX, 2, sizeof(gbaSoundCntX) / 2, file);
    fwrite(&gbaMainSoundCntL, sizeof(gbaMainSoundCntL), 1, file);
    fwrite(&gbaMainSoundCntH, sizeof(gbaMainSoundCntH), 1, file);
    fwrite(&gbaMainSoundCntX, sizeof(gbaMainSoundCntX), 1, file);
    fwrite(&gbaSoundBias, sizeof(gbaSoundBias), 1, file);
    fwrite(soundCnt, 4, sizeof(soundCnt) / 4, file);
    fwrite(soundSad, 4, sizeof(soundSad) / 4, file);
    fwrite(soundTmr, 2, sizeof(soundTmr) / 2, file);
    fwrite(soundPnt, 2, sizeof(soundPnt) / 2, file);
    fwrite(soundLen, 4, sizeof(soundLen) / 4, file);
    fwrite(&mainSoundCnt, sizeof(mainSoundCnt), 1, file);
    fwrite(&soundBias, sizeof(soundBias), 1, file);
    fwrite(sndCapCnt, 1, sizeof(sndCapCnt), file);
    fwrite(sndCapDad, 4, sizeof(sndCapDad) / 4, file);
    fwrite(sndCapLen, 2, sizeof(sndCapLen) / 2, file);

    for (int i = 0; i < 2; i++) {
        uint32_t count = gbaFifos[i].size();
        fwrite(&count, sizeof(count), 1, file);
        for (uint32_t j = 0; j < count; j++)
            fwrite(&gbaFifos[i][j], sizeof(gbaFifos[i][j]), 1, file);
    }
}

void Spu::loadState(FILE *file) {
    fread(&gbaFrameSequencer, sizeof(gbaFrameSequencer), 1, file);
    fread(gbaSoundTimers, 4, sizeof(gbaSoundTimers) / 4, file);
    fread(gbaEnvelopes, 1, sizeof(gbaEnvelopes), file);
    fread(gbaEnvTimers, 1, sizeof(gbaEnvTimers), file);
    fread(&gbaSweepTimer, sizeof(gbaSweepTimer), 1, file);
    fread(&gbaWaveDigit, sizeof(gbaWaveDigit), 1, file);
    fread(&gbaNoiseValue, sizeof(gbaNoiseValue), 1, file);
    fread(gbaWaveRam, 1, sizeof(gbaWaveRam), file);
    fread(&gbaSampleA, sizeof(gbaSampleA), 1, file);
    fread(&gbaSampleB, sizeof(gbaSampleB), 1, file);
    fread(&enabled, sizeof(enabled), 1, file);
    fread(adpcmValue, 4, sizeof(adpcmValue) / 4, file);
    fread(adpcmLoopValue, 4, sizeof(adpcmLoopValue) / 4, file);
    fread(adpcmIndex, 1, sizeof(adpcmIndex), file);
    fread(adpcmLoopIndex, 1, sizeof(adpcmLoopIndex), file);
    fread(adpcmToggle, sizeof(bool), sizeof(adpcmToggle) / sizeof(bool), file);
    fread(dutyCycles, 1, sizeof(dutyCycles), file);
    fread(noiseValues, 2, sizeof(noiseValues) / 2, file);
    fread(soundCurrent, 4, sizeof(soundCurrent) / 4, file);
    fread(soundTimers, 2, sizeof(soundTimers) / 2, file);
    fread(sndCapCurrent, 4, sizeof(sndCapCurrent) / 4, file);
    fread(sndCapTimers, 2, sizeof(sndCapTimers) / 2, file);
    fread(gbaSoundCntL, 1, sizeof(gbaSoundCntL), file);
    fread(gbaSoundCntH, 2, sizeof(gbaSoundCntH) / 2, file);
    fread(gbaSoundCntX, 2, sizeof(gbaSoundCntX) / 2, file);
    fread(&gbaMainSoundCntL, sizeof(gbaMainSoundCntL), 1, file);
    fread(&gbaMainSoundCntH, sizeof(gbaMainSoundCntH), 1, file);
    fread(&gbaMainSoundCntX, sizeof(gbaMainSoundCntX), 1, file);
    fread(&gbaSoundBias, sizeof(gbaSoundBias), 1, file);
    fread(soundCnt, 4, sizeof(soundCnt) / 4, file);
    fread(soundSad, 4, sizeof(soundSad) / 4, file);
    fread(soundTmr, 2, sizeof(soundTmr) / 2, file);
    fread(soundPnt, 2, sizeof(soundPnt) / 2, file);
    fread(soundLen, 4, sizeof(soundLen) / 4, file);
    fread(&mainSoundCnt, sizeof(mainSoundCnt), 1, file);
    fread(&soundBias, sizeof(soundBias), 1, file);
    fread(sndCapCnt, 1, sizeof(sndCapCnt), file);
    fread(sndCapDad, 4, sizeof(sndCapDad) / 4, file);
    fread(sndCapLen, 2, sizeof(sndCapLen) / 2, file);

    for (int i = 0; i < 2; i++) {
        gbaFifos[i].clear();
        uint32_t count;
        int8_t value;
        fread(&count, sizeof(count), 1, file);
        for (uint32_t j = 0; j < count; j++) {
            fread(&value, sizeof(value), 1, file);
            gbaFifos[i].push_back(value);
        }
    }
}

uint32_t *Spu::getSamples(int count) {
    if (bufferSize != count) {
        delete[] bufferIn;
        delete[] bufferOut;
        bufferIn  = new uint32_t[count];
        bufferOut = new uint32_t[count];
        bufferSize    = count;
        bufferPointer = 0;
        ready         = false;
    }

    uint32_t *out = new uint32_t[count];
    bool wait = false;

    if (Settings::fpsLimiter == 2) {
        u64 startTime = PPCGetTickCount();
        u64 timeout   = PPCUsToTicks(1000000 / 60);
        while (true) {
            PPCIrqState st = PPCIrqLockByMsr();
            bool isReady = ready;
            PPCIrqUnlockByMsr(st);

            if (isReady) break;

            if (PPCGetTickCount() - startTime > timeout) {
                wait = true;
                break;
            }
            KThreadYield();
        }
    }
    else if (Settings::fpsLimiter == 1) {
        PPCIrqState st = PPCIrqLockByMsr();
        bool isReady = ready;
        PPCIrqUnlockByMsr(st);

        if (!isReady) {
            KThrQueueBlock(&waitQueue2, 1);
        }
    }
    else {
        PPCIrqState st = PPCIrqLockByMsr();
        if (!ready) wait = true;
        PPCIrqUnlockByMsr(st);
    }

    if (wait) {
        uint32_t lastSample = (bufferOut && bufferSize > 0) ? bufferOut[bufferSize - 1] : 0;
        for (int i = 0; i < count; i++)
            out[i] = lastSample;
    }
    else {
        memcpy(out, bufferOut, count * sizeof(uint32_t));
    }

    {
        PPCIrqState st = PPCIrqLockByMsr();
        ready = false;
        PPCIrqUnlockByMsr(st);
    }

    if (Settings::fpsLimiter == 1) {
        KThrQueueUnblockAllByValue(&waitQueue1, 1);
    }

    return out;
}

void Spu::runGbaSample() {
    core->schedule(GBA_SPU_SAMPLE, 512);
    if (!Settings::emulateAudio) return pushSample(0, 0);

    int32_t sampleLeft = 0, sampleRight = 0;
    if (gbaMainSoundCntX & BIT(7)) {
        int32_t data[4] = {};
        for (int i = 0; i < 2; i++) {
            if (!(gbaMainSoundCntX & BIT(i)))
                continue;

            if (i == 0 && gbaFrameSequencer % 256 == 128 && (gbaSoundCntL[i] & 0x70) && --gbaSweepTimer <= 0) {
                uint16_t frequency = (gbaSoundCntX[i] & 0x07FF);
                int sweep = frequency >> (gbaSoundCntL[i] & 0x07);
                if (gbaSoundCntL[i] & BIT(3)) sweep = -sweep;

                frequency += sweep;

                if (frequency < 0x800) {
                    gbaSoundCntX[i] = (gbaSoundCntX[i] & ~0x07FF) | frequency;
                    gbaSweepTimer = (gbaSoundCntL[i] & 0x70) >> 4;
                }
                else {
                    gbaMainSoundCntX &= ~BIT(i);
                    continue;
                }
            }

            gbaSoundTimers[i] -= 4;
            while ((gbaSoundTimers[i]) <= 0)
                gbaSoundTimers[i] += 2048 - (gbaSoundCntX[i] & 0x07FF);

            int duty;
            switch ((gbaSoundCntH[i] & 0x00C0) >> 6) {
                case 0: duty = (2048 - (gbaSoundCntX[i] & 0x07FF)) * 7 / 8; break;
                case 1: duty = (2048 - (gbaSoundCntX[i] & 0x07FF)) * 6 / 8; break;
                case 2: duty = (2048 - (gbaSoundCntX[i] & 0x07FF)) * 4 / 8; break;
                case 3: duty = (2048 - (gbaSoundCntX[i] & 0x07FF)) * 2 / 8; break;
                default: duty = 0; break;
            }

            data[i] = (gbaSoundTimers[i] < duty) ? -0x80 : 0x80;

            if (gbaFrameSequencer % 128 == 0 && (gbaSoundCntX[i] & BIT(14)) && (gbaSoundCntH[i] & 0x003F)) {
                gbaSoundCntH[i] = (gbaSoundCntH[i] & ~0x003F) | ((gbaSoundCntH[i] & 0x003F) - 1);

                if ((gbaSoundCntH[i] & 0x003F) == 0)
                    gbaMainSoundCntX &= ~BIT(i);
            }

            if (gbaFrameSequencer == 448 && --gbaEnvTimers[i] <= 0) {
                if (gbaEnvTimers[i] == 0) {
                    if ((gbaSoundCntH[i] & BIT(11)) && gbaEnvelopes[i] < 15)
                        gbaEnvelopes[i]++;
                    else if (!(gbaSoundCntH[i] & BIT(11)) && gbaEnvelopes[i] > 0)
                        gbaEnvelopes[i]--;
                }
                else {
                    gbaEnvelopes[i] = (gbaSoundCntH[i] & 0xF000) >> 12;
                }

                gbaEnvTimers[i] = (gbaSoundCntH[i] & 0x0700) >> 8;
            }

            data[i] = data[i] * gbaEnvelopes[i] / 15;
        }

        if ((gbaMainSoundCntX & BIT(2)) && (gbaSoundCntL[1] & BIT(7))) {
            gbaSoundTimers[2] -= 64;
            while ((gbaSoundTimers[2]) <= 0) {
                gbaSoundTimers[2] += (2048 - (gbaSoundCntX[2] & 0x07FF));
                gbaWaveDigit = (gbaWaveDigit + 1) % 64;
            }

            int bank = (gbaSoundCntL[1] & BIT(6)) >> 6;
            if ((gbaSoundCntL[1] & BIT(5)) && gbaWaveDigit >= 32)
                bank = !bank;

            data[2] = gbaWaveRam[bank][(gbaWaveDigit % 32) / 2];
            if (gbaWaveDigit & 1)
                data[2] &= 0x0F;
            else
                data[2] >>= 4;

            if (gbaFrameSequencer % 128 == 0 && (gbaSoundCntX[2] & BIT(14)) && (gbaSoundCntH[2] & 0x00FF)) {
                gbaSoundCntH[2] = (gbaSoundCntH[2] & ~0x00FF) | ((gbaSoundCntH[2] & 0x00FF) - 1);

                if ((gbaSoundCntH[2] & 0x00FF) == 0)
                    gbaMainSoundCntX &= ~BIT(2);
            }

            switch ((gbaSoundCntH[2] & 0xE000) >> 13) {
                case 0: data[2] >>= 4; break;
                case 1: data[2] >>= 0; break;
                case 2: data[2] >>= 1; break;
                case 3: data[2] >>= 2; break;
                default: data[2] = data[2] * 3 / 4; break;
            }

            data[2] = (data[2] * 0x100 / 0xF);
        }

        if (gbaMainSoundCntX & BIT(3)) {
            gbaSoundTimers[3] -= 16;
            while ((gbaSoundTimers[3]) <= 0) {
                int divisor = (gbaSoundCntX[3] & 0x0007) * 16;
                if (divisor == 0) divisor = 8;
                gbaSoundTimers[3] += (divisor << ((gbaSoundCntX[3] & 0x00F0) >> 4));

                gbaNoiseValue &= ~BIT(15);
                if (gbaNoiseValue & BIT(0))
                    gbaNoiseValue = BIT(15) | ((gbaNoiseValue >> 1) ^ ((gbaSoundCntH[3] & BIT(3)) ? 0x60 : 0x6000));
                else
                    gbaNoiseValue >>= 1;
            }

            data[3] = (gbaNoiseValue & BIT(15)) ? 0x80 : -0x80;

            if (gbaFrameSequencer % 128 == 0 && (gbaSoundCntX[3] & BIT(14)) && (gbaSoundCntH[3] & 0x003F)) {
                gbaSoundCntH[3] = (gbaSoundCntH[3] & ~0x003F) | ((gbaSoundCntH[3] & 0x003F) - 1);

                if ((gbaSoundCntH[3] & 0x003F) == 0)
                    gbaMainSoundCntX &= ~BIT(3);
            }

            if (gbaFrameSequencer == 448 && --gbaEnvTimers[2] <= 0) {
                if (gbaEnvTimers[2] == 0) {
                    if ((gbaSoundCntH[3] & BIT(11)) && gbaEnvelopes[2] < 15)
                        gbaEnvelopes[2]++;
                    else if (!(gbaSoundCntH[3] & BIT(11)) && gbaEnvelopes[2] > 0)
                        gbaEnvelopes[2]--;
                }
                else {
                    gbaEnvelopes[2] = (gbaSoundCntH[3] & 0xF000) >> 12;
                }

                gbaEnvTimers[2] = (gbaSoundCntH[3] & 0x0700) >> 8;
            }

            data[3] = data[3] * gbaEnvelopes[2] / 15;
        }

        for (int i = 0; i < 4; i++) {
            switch (gbaMainSoundCntH & 0x0003) {
                case 0: data[i] >>= 2; break;
                case 1: data[i] >>= 1; break;
                case 2: data[i] >>= 0; break;
            }

            if (gbaMainSoundCntL & BIT(12 + i))
                sampleLeft += data[i] * ((gbaMainSoundCntL & 0x0070) >> 4) / 7;
            if (gbaMainSoundCntL & BIT(8 + i))
                sampleRight += data[i] * (gbaMainSoundCntL & 0x0007) / 7;
        }

        if (gbaMainSoundCntH & BIT(9))
            sampleLeft += gbaSampleA << ((gbaMainSoundCntH & BIT(2)) ? 2 : 1);
        if (gbaMainSoundCntH & BIT(8))
            sampleRight += gbaSampleA << ((gbaMainSoundCntH & BIT(2)) ? 2 : 1);

        if (gbaMainSoundCntH & BIT(13))
            sampleLeft += gbaSampleB << ((gbaMainSoundCntH & BIT(3)) ? 2 : 1);
        if (gbaMainSoundCntH & BIT(12))
            sampleRight += gbaSampleB << ((gbaMainSoundCntH & BIT(3)) ? 2 : 1);

        gbaFrameSequencer = (gbaFrameSequencer + 1) % 512;
    }

    sampleLeft  = (std::max(0, std::min<int32_t>(0x3FF, sampleLeft  + (gbaSoundBias & 0x3FF))) - 0x200) << 6;
    sampleRight = (std::max(0, std::min<int32_t>(0x3FF, sampleRight + (gbaSoundBias & 0x3FF))) - 0x200) << 6;
    pushSample(sampleLeft, sampleRight);
}

void Spu::runSample() {
    core->schedule(NDS_SPU_SAMPLE, 512 * 2);
    if (!Settings::emulateAudio) return pushSample(0, 0);

    int64_t mixerLeft = 0, mixerRight = 0;
    int64_t channelsLeft[2] = {}, channelsRight[2] = {};
    for (int i = 0; enabled >> i; i++) {
        if (!(enabled & BIT(i))) continue;
        uint8_t format = (soundCnt[i] >> 29) & 0x3;
        int64_t data = 0;

        switch (format) {
        case 0:
            data = (int8_t)core->memory.read<uint8_t>(1, soundCurrent[i]) << 8;
            break;

        case 1:
            data = (int16_t)core->memory.read<uint16_t>(1, soundCurrent[i]);
            break;

        case 2:
            data = adpcmValue[i];
            break;

        case 3:
            if (i >= 8 && i <= 13) {
                uint8_t duty = 7 - ((soundCnt[i] & 0x07000000) >> 24);
                data = (dutyCycles[i - 8] < duty) ? -0x7FFF : 0x7FFF;
            }
            else if (i >= 14) {
                data = (noiseValues[i - 14] & BIT(15)) ? -0x7FFF : 0x7FFF;
            }
            break;
        }

        soundTimers[i] += 512;
        bool overflow = (soundTimers[i] < 512);

        while (overflow) {
            soundTimers[i] += soundTmr[i];
            overflow = (soundTimers[i] < soundTmr[i]);

            switch (format) {
            case 0: case 1:
                soundCurrent[i] += 1 + format;
                break;

            case 2: {
                if (soundCurrent[i] == soundSad[i] + soundPnt[i] * 4 && !adpcmToggle[i]) {
                    adpcmLoopValue[i] = adpcmValue[i];
                    adpcmLoopIndex[i] = adpcmIndex[i];
                }

                uint8_t adpcmData = core->memory.read<uint8_t>(1, soundCurrent[i]);
                adpcmData = adpcmToggle[i] ? ((adpcmData & 0xF0) >> 4) : (adpcmData & 0x0F);

                int32_t diff = adpcmTable[adpcmIndex[i]] / 8;
                if (adpcmData & BIT(0)) diff += adpcmTable[adpcmIndex[i]] / 4;
                if (adpcmData & BIT(1)) diff += adpcmTable[adpcmIndex[i]] / 2;
                if (adpcmData & BIT(2)) diff += adpcmTable[adpcmIndex[i]] / 1;

                if (adpcmData & BIT(3)) {
                    adpcmValue[i] += diff;
                    if (adpcmValue[i] > 0x7FFF) adpcmValue[i] = 0x7FFF;
                }
                else {
                    adpcmValue[i] -= diff;
                    if (adpcmValue[i] < -0x7FFF) adpcmValue[i] = -0x7FFF;
                }

                adpcmIndex[i] += indexTable[adpcmData & 0x7];
                if (adpcmIndex[i] < 0)  adpcmIndex[i] = 0;
                if (adpcmIndex[i] > 88) adpcmIndex[i] = 88;

                adpcmToggle[i] = !adpcmToggle[i];
                if (!adpcmToggle[i]) soundCurrent[i]++;

                break;
            }

            case 3:
                if (i >= 8 && i <= 13) {
                    dutyCycles[i - 8] = (dutyCycles[i - 8] + 1) % 8;
                }
                else if (i >= 14) {
                    noiseValues[i - 14] &= ~BIT(15);

                    if (noiseValues[i - 14] & BIT(0))
                        noiseValues[i - 14] = BIT(15) | ((noiseValues[i - 14] >> 1) ^ 0x6000);
                    else
                        noiseValues[i - 14] >>= 1;
                }
                break;
            }

            if (format != 3 && soundCurrent[i] >= soundSad[i] + (soundPnt[i] + soundLen[i]) * 4) {
                if ((soundCnt[i] & 0x18000000) >> 27 == 1) {
                    soundCurrent[i] = soundSad[i] + soundPnt[i] * 4;

                    if (format == 2) {
                        adpcmValue[i]  = adpcmLoopValue[i];
                        adpcmIndex[i]  = adpcmLoopIndex[i];
                        adpcmToggle[i] = false;
                    }
                }
                else {
                    soundCnt[i] &= ~BIT(31);
                    enabled     &= ~BIT(i);
                    break;
                }
            }
        }

        int divShift = (soundCnt[i] & 0x00000300) >> 8;
        if (divShift == 3) divShift++;
        data <<= 4 - divShift;

        int mulFactor = (soundCnt[i] & 0x0000007F);
        if (mulFactor == 127) mulFactor++;
        data = (data << 7) * mulFactor / 128;

        int panValue = (soundCnt[i] & 0x007F0000) >> 16;
        if (panValue == 127) panValue++;
        int64_t dataLeft  = (data * (128 - panValue) / 128) >> 3;
        int64_t dataRight = (data *          panValue / 128) >> 3;

        if (i == 1 || i == 3) {
            channelsLeft[i >> 1]  = dataLeft;
            channelsRight[i >> 1] = dataRight;
            if (mainSoundCnt & BIT(12 + (i >> 1)))
                continue;
        }

        mixerLeft  += dataLeft;
        mixerRight += dataRight;
    }

    for (int i = 0; i < 2; i++) {
        if (!(sndCapCnt[i] & BIT(7)))
            continue;

        sndCapTimers[i] += 512;
        bool overflow = (sndCapTimers[i] < 512);

        while (overflow) {
            sndCapTimers[i] += soundTmr[1 + (i << 1)];
            overflow = (sndCapTimers[i] < soundTmr[1 + (i << 1)]);

            int64_t sample = ((i == 0) ? mixerLeft : mixerRight);
            if (sample >  0x7FFFFF) sample =  0x7FFFFF;
            if (sample < -0x800000) sample = -0x800000;

            if (sndCapCnt[i] & BIT(3)) {
                core->memory.write<uint8_t>(1, sndCapCurrent[i], sample >> 16);
                sndCapCurrent[i]++;
            }
            else {
                core->memory.write<uint16_t>(1, sndCapCurrent[i], sample >> 8);
                sndCapCurrent[i] += 2;
            }

            if (sndCapCurrent[i] >= sndCapDad[i] + sndCapLen[i] * 4) {
                if (sndCapCnt[i] & BIT(2)) {
                    sndCapCnt[i] &= ~BIT(7);
                    continue;
                }
                else {
                    sndCapCurrent[i] = sndCapDad[i];
                }
            }
        }
    }

    int64_t sampleLeft;
    switch ((mainSoundCnt & 0x0300) >> 8) {
        case 0: sampleLeft = mixerLeft;                              break;
        case 1: sampleLeft = channelsLeft[0];                       break;
        case 2: sampleLeft = channelsLeft[1];                       break;
        case 3: sampleLeft = channelsLeft[0] + channelsLeft[1];     break;
        default: sampleLeft = mixerLeft; break;
    }

    int64_t sampleRight;
    switch ((mainSoundCnt & 0x0C00) >> 10) {
        case 0: sampleRight = mixerRight;                             break;
        case 1: sampleRight = channelsRight[0];                      break;
        case 2: sampleRight = channelsRight[1];                      break;
        case 3: sampleRight = channelsRight[0] + channelsRight[1];   break;
        default: sampleRight = mixerRight; break;
    }

    int masterVol = (mainSoundCnt & 0x007F);
    if (masterVol == 127) masterVol++;
    sampleLeft  = (sampleLeft  * masterVol / 128) >> 8;
    sampleRight = (sampleRight * masterVol / 128) >> 8;

    if (Settings::audio16Bit) {
        sampleLeft  = (std::max(0, std::min<int32_t>(0xFFFF, (int32_t)sampleLeft  + (soundBias << 6))) - 0x8000);
        sampleRight = (std::max(0, std::min<int32_t>(0xFFFF, (int32_t)sampleRight + (soundBias << 6))) - 0x8000);
    }
    else {
        sampleLeft  = (std::max(0, std::min<int32_t>(0x3FF, (int32_t)(sampleLeft  >> 6) + soundBias)) - 0x200) << 6;
        sampleRight = (std::max(0, std::min<int32_t>(0x3FF, (int32_t)(sampleRight >> 6) + soundBias)) - 0x200) << 6;
    }
    pushSample((int16_t)sampleLeft, (int16_t)sampleRight);
}

void Spu::pushSample(int16_t sampleLeft, int16_t sampleRight) {
    if (!bufferSize) return;
    bufferIn[bufferPointer++] = ((uint32_t)(uint16_t)sampleRight << 16)
                              | ((uint32_t)(uint16_t)sampleLeft  & 0xFFFF);
    if (bufferPointer != bufferSize) return;

    if (Settings::fpsLimiter == 2) {
        u64 startTime = PPCGetTickCount();
        u64 timeout   = PPCUsToTicks(1000000);
        while (true) {
            PPCIrqState st = PPCIrqLockByMsr();
            bool isReady = ready;
            PPCIrqUnlockByMsr(st);

            if (!isReady) break;

            if (PPCGetTickCount() - startTime > timeout) break;

            KThreadYield();
        }
    }
    else if (Settings::fpsLimiter == 1) {
        PPCIrqState st = PPCIrqLockByMsr();
        bool isReady = ready;
        PPCIrqUnlockByMsr(st);

        if (isReady) {
            KThrQueueBlock(&waitQueue1, 1);
        }
    }

    std::swap(bufferIn, bufferOut);
    bufferPointer = 0;

    {
        PPCIrqState st = PPCIrqLockByMsr();
        ready = true;
        PPCIrqUnlockByMsr(st);
    }

    if (Settings::fpsLimiter == 1) {
        KThrQueueUnblockAllByValue(&waitQueue2, 1);
    }
}

void Spu::startChannel(int channel) {
    soundCurrent[channel] = soundSad[channel];
    soundTimers[channel]  = soundTmr[channel];

    switch ((soundCnt[channel] & 0x60000000) >> 29) {
    case 2: {
        uint32_t header = core->memory.read<uint32_t>(1, soundSad[channel]);
        adpcmValue[channel]  = (int16_t)header;
        adpcmIndex[channel]  = (header & 0x007F0000) >> 16;
        if (adpcmIndex[channel] > 88) adpcmIndex[channel] = 88;
        adpcmToggle[channel] = false;
        soundCurrent[channel] += 4;
        break;
    }

    case 3:
        if (channel >= 8 && channel <= 13)
            dutyCycles[channel - 8] = 0;
        else if (channel >= 14)
            noiseValues[channel - 14] = 0x7FFF;
        break;
    }

    enabled |= BIT(channel);
}

void Spu::gbaFifoTimer(int timer) {
    if (((gbaMainSoundCntH & BIT(10)) >> 10) == timer) {
        if (!gbaFifos[0].empty()) {
            gbaSampleA = gbaFifos[0].front();
            gbaFifos[0].pop_front();
        }

        if (gbaFifos[0].size() <= 16)
            core->dma[1].trigger(3, 0x02);
    }

    if (((gbaMainSoundCntH & BIT(14)) >> 14) == timer) {
        if (!gbaFifos[1].empty()) {
            gbaSampleB = gbaFifos[1].front();
            gbaFifos[1].pop_front();
        }

        if (gbaFifos[1].size() <= 16)
            core->dma[1].trigger(3, 0x04);
    }
}

void Spu::writeGbaSoundCntL(int channel, uint8_t value) {
    if (!(gbaMainSoundCntX & BIT(7))) return;
    uint8_t mask = (channel == 0) ? 0x7F : 0xE0;
    gbaSoundCntL[channel / 2] = (gbaSoundCntL[channel / 2] & ~mask) | (value & mask);
}

void Spu::writeGbaSoundCntH(int channel, uint16_t mask, uint16_t value) {
    if (!(gbaMainSoundCntX & BIT(7))) return;
    switch (channel) {
        case 2: mask &= 0xE0FF; break;
        case 3: mask &= 0xFF3F; break;
    }
    gbaSoundCntH[channel] = (gbaSoundCntH[channel] & ~mask) | (value & mask);
}

void Spu::writeGbaSoundCntX(int channel, uint16_t mask, uint16_t value) {
    if (!(gbaMainSoundCntX & BIT(7))) return;
    mask &= (channel == 3) ? 0x40FF : 0x47FF;
    gbaSoundCntX[channel] = (gbaSoundCntX[channel] & ~mask) | (value & mask);

    if (!Settings::emulateAudio || !(value & BIT(15))) return;
    gbaMainSoundCntX |= BIT(channel);
    if (channel < 2) {
        if (channel == 0) gbaSweepTimer = (gbaSoundCntL[0] & 0x70) >> 4;
        gbaEnvelopes[channel] = (gbaSoundCntH[channel] & 0xF000) >> 12;
        gbaEnvTimers[channel] = (gbaSoundCntH[channel] & 0x0700) >> 8;
        gbaSoundTimers[channel] = 2048 - (gbaSoundCntX[channel] & 0x07FF);
    }
    else if (channel == 2) {
        gbaWaveDigit = 0;
        gbaSoundTimers[2] = 2048 - (gbaSoundCntX[2] & 0x07FF);
    }
    else {
        gbaNoiseValue = (gbaSoundCntH[3] & BIT(3)) ? 0x40 : 0x4000;
        gbaEnvelopes[2] = (gbaSoundCntH[3] & 0xF000) >> 12;
        gbaEnvTimers[2] = (gbaSoundCntH[3] & 0x0700) >> 8;

        int divisor = (gbaSoundCntX[3] & 0x0007) * 16;
        if (divisor == 0) divisor = 8;
        gbaSoundTimers[3] = (divisor << ((gbaSoundCntX[3] & 0x00F0) >> 4));
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
    gbaMainSoundCntX = (gbaMainSoundCntX & ~0x80) | (value & 0x80);

    if (!(gbaMainSoundCntX & BIT(7))) {
        for (int i = 0; i < 4; i++) {
            if (i < 2) gbaSoundCntL[i] = 0;
            gbaSoundCntH[i] = 0;
            gbaSoundCntX[i] = 0;
        }
        gbaMainSoundCntL  = 0;
        gbaMainSoundCntX &= ~0x0F;
        gbaFrameSequencer = 0;
    }
}

void Spu::writeGbaSoundBias(uint16_t mask, uint16_t value) {
    mask &= 0xC3FE;
    gbaSoundBias = (gbaSoundBias & ~mask) | (value & mask);
}

void Spu::writeGbaWaveRam(int index, uint8_t value) {
    gbaWaveRam[!(gbaSoundCntL[1] & BIT(6))][index] = value;
}

void Spu::writeGbaFifoA(uint32_t mask, uint32_t value) {
    for (int i = 0; i < 32; i += 8)
        if (gbaFifos[0].size() < 32 && (mask & (0xFF << i)))
            gbaFifos[0].push_back(value >> i);
}

void Spu::writeGbaFifoB(uint32_t mask, uint32_t value) {
    for (int i = 0; i < 32; i += 8)
        if (gbaFifos[1].size() < 32 && (mask & (0xFF << i)))
            gbaFifos[1].push_back(value >> i);
}

void Spu::writeSoundCnt(int channel, uint32_t mask, uint32_t value) {
    if (!Settings::emulateAudio) value &= ~BIT(31);
    bool enable = (!(soundCnt[channel] & BIT(31)) && (value & mask & BIT(31)));

    mask &= 0xFF7F837F;
    soundCnt[channel] = (soundCnt[channel] & ~mask) | (value & mask);

    if (enable && (mainSoundCnt & BIT(15)) &&
        (soundSad[channel] != 0 || ((soundCnt[channel] & 0x60000000) >> 29) == 3))
        startChannel(channel);
    else if (!(soundCnt[channel] & BIT(31)))
        enabled &= ~BIT(channel);
}

void Spu::writeSoundSad(int channel, uint32_t mask, uint32_t value) {
    mask &= 0x07FFFFFC;
    soundSad[channel] = (soundSad[channel] & ~mask) | (value & mask);

    if (((soundCnt[channel] & 0x60000000) >> 29) != 3) {
        if (soundSad[channel] != 0 && (mainSoundCnt & BIT(15)) &&
            (soundCnt[channel] & BIT(31)))
            startChannel(channel);
        else
            enabled &= ~BIT(channel);
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
    bool enable = (!(mainSoundCnt & BIT(15)) && (value & BIT(15)));
    mask &= 0xBF7F;
    mainSoundCnt = (mainSoundCnt & ~mask) | (value & mask);

    if (enable) {
        for (int i = 0; i < 16; i++) {
            if ((soundCnt[i] & BIT(31)) &&
                (soundSad[i] != 0 || ((soundCnt[i] & 0x60000000) >> 29) == 3))
                startChannel(i);
        }
    }
    else if (!(mainSoundCnt & BIT(15))) {
        enabled = 0;
    }
}

void Spu::writeSoundBias(uint16_t mask, uint16_t value) {
    mask &= 0x03FF;
    soundBias = (soundBias & ~mask) | (value & mask);
}

void Spu::writeSndCapCnt(int channel, uint8_t value) {
    if (!(sndCapCnt[channel] & BIT(7)) && (value & BIT(7))) {
        sndCapCurrent[channel] = sndCapDad[channel];
        sndCapTimers[channel]  = soundTmr[1 + (channel << 1)];
    }
    sndCapCnt[channel] = (value & 0x8F);
}

void Spu::writeSndCapDad(int channel, uint32_t mask, uint32_t value) {
    mask &= 0x07FFFFFC;
    sndCapDad[channel] = (sndCapDad[channel] & ~mask) | (value & mask);

    sndCapCurrent[channel] = sndCapDad[channel];
    sndCapTimers[channel]  = soundTmr[1 + (channel << 1)];
}

void Spu::writeSndCapLen(int channel, uint16_t mask, uint16_t value) {
    sndCapLen[channel] = (sndCapLen[channel] & ~mask) | (value & mask);
}

uint8_t Spu::readGbaSoundCntL(int channel) {
    return gbaSoundCntL[channel / 2];
}

uint16_t Spu::readGbaSoundCntH(int channel) {
    return gbaSoundCntH[channel] & ~((channel == 2) ? 0x00FF : 0x003F);
}

uint16_t Spu::readGbaSoundCntX(int channel) {
    return gbaSoundCntX[channel] & ~((channel == 3) ? 0x0000 : 0x07FF);
}

uint8_t Spu::readGbaWaveRam(int index) {
    return gbaWaveRam[!(gbaSoundCntL[1] & BIT(6))][index];
}
