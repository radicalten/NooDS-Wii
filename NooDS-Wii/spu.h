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

#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>

extern "C" {
    #include <tuxedo/thread.h>
}

class Core;

class Spu {
public:
    explicit Spu(Core *core);
    ~Spu();

    void saveState(FILE *file);
    void loadState(FILE *file);

    // ---------------------------------------------------------------------------
    // Fill `dst` with `count` stereo samples packed as:
    //   bits 15.. 0  = left  channel (int16_t)
    //   bits 31..16  = right channel (int16_t)
    //
    // Returns true if fresh data was available, false if silence was substituted.
    // The caller owns `dst`; it must be at least count*4 bytes.
    // No heap allocation is performed inside this call.
    // ---------------------------------------------------------------------------
    bool getSamples(uint32_t *dst, int count);
    int  drainSamples(uint32_t *dst, int maxCount, uint32_t *availAfter = nullptr, int minCount = 1);

    void runGbaSample();
    void runSample();
    void gbaFifoTimer(int timer);
    void wakeSpuSchedule();

    // GBA register accessors
    uint8_t  readGbaSoundCntL(int channel);
    uint16_t readGbaSoundCntH(int channel);
    uint16_t readGbaSoundCntX(int channel);
    uint16_t readGbaMainSoundCntL()  { return gbaMainSoundCntL; }
    uint16_t readGbaMainSoundCntH()  { return gbaMainSoundCntH; }
    uint8_t  readGbaMainSoundCntX()  { return gbaMainSoundCntX; }
    uint16_t readGbaSoundBias()      { return gbaSoundBias;      }
    uint8_t  readGbaWaveRam(int index);

    // NDS register accessors
    uint32_t readSoundCnt(int channel)    { return soundCnt[channel]; }
    uint16_t readMainSoundCnt()           { return mainSoundCnt;      }
    uint16_t readSoundBias()              { return soundBias;          }
    uint8_t  readSndCapCnt(int channel)   { return sndCapCnt[channel]; }
    uint32_t readSndCapDad(int channel)   { return sndCapDad[channel]; }

    // GBA register writers
    void writeGbaSoundCntL(int channel, uint8_t value);
    void writeGbaSoundCntH(int channel, uint16_t mask, uint16_t value);
    void writeGbaSoundCntX(int channel, uint16_t mask, uint16_t value);
    void writeGbaMainSoundCntL(uint16_t mask, uint16_t value);
    void writeGbaMainSoundCntH(uint16_t mask, uint16_t value);
    void writeGbaMainSoundCntX(uint8_t value);
    void writeGbaSoundBias(uint16_t mask, uint16_t value);
    void writeGbaWaveRam(int index, uint8_t value);
    void writeGbaFifoA(uint32_t mask, uint32_t value);
    void writeGbaFifoB(uint32_t mask, uint32_t value);

    // NDS register writers
    void writeSoundCnt(int channel, uint32_t mask, uint32_t value);
    void writeSoundSad(int channel, uint32_t mask, uint32_t value);
    void writeSoundTmr(int channel, uint16_t mask, uint16_t value);
    void writeSoundPnt(int channel, uint16_t mask, uint16_t value);
    void writeSoundLen(int channel, uint32_t mask, uint32_t value);
    void writeMainSoundCnt(uint16_t mask, uint16_t value);
    void writeSoundBias(uint16_t mask, uint16_t value);
    void writeSndCapCnt(int channel, uint8_t value);
    void writeSndCapDad(int channel, uint32_t mask, uint32_t value);
    void writeSndCapLen(int channel, uint16_t mask, uint16_t value);

private:
    Core *core;

    // ---------------------------------------------------------------------------
    // Lock-free double buffer.
    //
    // The emulator thread writes into buffers[writeIdx] via pushSample().
    // The audio thread reads from buffers[readIdx ^ 1] via getSamples().
    //
    // Swap protocol (emulator thread only):
    //   1. Finish filling buffers[writeIdx].
    //   2. Atomically flip writeIdx (PPCIrqLock).
    //   3. Signal ready.
    //
    // Read protocol (audio thread only):
    //   1. Check ready under lock.
    //   2. Copy from buffers[readIdx] (the buffer NOT currently being written).
    //   3. Clear ready under lock.
    // ---------------------------------------------------------------------------
    static constexpr uint32_t SPU_RING_SIZE      = 4096;
    static constexpr uint32_t SPU_RING_MASK      = SPU_RING_SIZE - 1;
    static constexpr uint32_t SPU_TARGET_LATENCY = 2048; // ~62 ms max buffered ahead
    static const int          SPU_BUF_FRAMES     = 699;

    uint32_t          ringBuf[SPU_RING_SIZE];
    volatile uint32_t ringWrite     = 0;
    volatile uint32_t ringRead      = 0;
    uint32_t          lastPopSample = 0;

    KThrQueue waitQueue1;   // emulator blocks here when audio ring is full
    KThrQueue waitQueue2;   // reserved

    // ---- GBA state ----
    int16_t  gbaFrameSequencer    = 0;
    int32_t  gbaSoundTimers[4]    = {};
    int8_t   gbaEnvelopes[3]      = {};
    int8_t   gbaEnvTimers[3]      = {};
    int8_t   gbaSweepTimer        = 0;
    int8_t   gbaWaveDigit         = 0;
    uint16_t gbaNoiseValue        = 0;

    struct GbaAudioFifo {
        int8_t data[64] = {};
        uint8_t head = 0;
        uint8_t sz = 0;
        inline size_t size() const { return sz; }
        inline bool empty() const { return sz == 0; }
        inline void clear() { head = 0; sz = 0; }
        inline int8_t front() const { return data[head & 63]; }
        inline void pop_front() { head = (head + 1) & 63; sz--; }
        inline void push_back(int8_t v) {
            if (sz < 64) { data[(head + sz) & 63] = v; sz++; }
        }
        inline const int8_t& operator[](size_t i) const { return data[(head + i) & 63]; }
    };

    uint8_t  gbaWaveRam[2][16]    = {};
    GbaAudioFifo gbaFifos[2];
    int8_t   gbaSampleA = 0, gbaSampleB = 0;
    uint32_t spuSilentNextCycle   = 0;
    uint32_t spuSilentBatch       = 1;

    uint16_t enabled              = 0;

    static const int     indexTable[8];
    static const int16_t adpcmTable[89];

    int32_t adpcmValue[16]     = {};
    int32_t adpcmLoopValue[16] = {};
    int8_t  adpcmIndex[16]     = {};
    int8_t  adpcmLoopIndex[16] = {};
    bool    adpcmToggle[16]    = {};

    uint8_t  dutyCycles[6]     = {};
    uint16_t noiseValues[2]    = {};
    uint32_t soundCurrent[16]  = {};
    uint16_t soundTimers[16]   = {};
    uint32_t sndCapCurrent[2]  = {};
    uint16_t sndCapTimers[2]   = {};

    uint8_t  gbaSoundCntL[2]   = {};
    uint16_t gbaSoundCntH[4]   = {};
    uint16_t gbaSoundCntX[4]   = {};
    uint16_t gbaMainSoundCntL  = 0;
    uint16_t gbaMainSoundCntH  = 0;
    uint8_t  gbaMainSoundCntX  = 0;
    uint16_t gbaSoundBias      = 0;

    uint32_t soundCnt[16]      = {};
    uint32_t soundSad[16]      = {};
    uint16_t soundTmr[16]      = {};
    uint16_t soundPnt[16]      = {};
    uint32_t soundLen[16]      = {};
    uint16_t mainSoundCnt      = 0;
    uint16_t soundBias         = 0;
    uint8_t  sndCapCnt[2]      = {};
    uint32_t sndCapDad[2]      = {};
    uint16_t sndCapLen[2]      = {};

    void pushSample(int16_t sampleLeft, int16_t sampleRight);
    void startChannel(int channel);
};
