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


#include "core.h"
#ifndef NOODS_DMA_GXFIFO_VERIFY
#define NOODS_DMA_GXFIFO_VERIFY 0
#endif
#if NOODS_DMA_GXFIFO_VERIFY && !NOODS_DMA_GXFIFO_STATS
#error "DMA_GXFIFO_VERIFY requires DMA_GXFIFO_STATS"
#endif
#ifndef NOODS_DMA_GXFIFO_MAP
#define NOODS_DMA_GXFIFO_MAP 0
#endif
#ifndef NOODS_DMA_GXFIFO_FAST
#define NOODS_DMA_GXFIFO_FAST 0
#endif
#ifndef NOODS_DMA_GXFIFO_STATS
#define NOODS_DMA_GXFIFO_STATS 0
#endif
#if NOODS_DMA_GXFIFO_STATS
uint32_t g_dmaGxStats[12]; // mode7 calls, mode7 words, eligible calls, fast words, not-fixed, not-gxfifo, not-word, checks-active
#endif

void Dma::saveState(FILE *file) {
    // Write state data to the file
    fwrite(srcAddrs, 4, sizeof(srcAddrs) / 4, file);
    fwrite(dstAddrs, 4, sizeof(dstAddrs) / 4, file);
    fwrite(wordCounts, 4, sizeof(wordCounts) / 4, file);
    fwrite(dmaLatch, 4, sizeof(dmaLatch) / 4, file);
    fwrite(dmaSad, 4, sizeof(dmaSad) / 4, file);
    fwrite(dmaDad, 4, sizeof(dmaDad) / 4, file);
    fwrite(dmaCnt, 4, sizeof(dmaCnt) / 4, file);
}

void Dma::loadState(FILE *file) {
    // Read state data from the file
    fread(srcAddrs, 4, sizeof(srcAddrs) / 4, file);
    fread(dstAddrs, 4, sizeof(dstAddrs) / 4, file);
    fread(wordCounts, 4, sizeof(wordCounts) / 4, file);
    fread(dmaLatch, 4, sizeof(dmaLatch) / 4, file);
    fread(dmaSad, 4, sizeof(dmaSad) / 4, file);
    fread(dmaDad, 4, sizeof(dmaDad) / 4, file);
    fread(dmaCnt, 4, sizeof(dmaCnt) / 4, file);
}

void Dma::transfer(int channel) {
    int dstAddrCnt = (dmaCnt[channel] & 0x00600000) >> 21;
    int srcAddrCnt = (dmaCnt[channel] & 0x01800000) >> 23;
    int mode = (dmaCnt[channel] & 0x38000000) >> 27;
    int gxFifoCount = 0;

    if (core->gbaMode) {
        const uint32_t srcMask = (channel == 0) ? 0x07FFFFFFu : 0x0FFFFFFFu;
        const uint32_t dstMask = (channel == 3) ? 0x0FFFFFFFu : 0x07FFFFFFu;
        const bool isFifo = (mode == 6 && (channel == 1 || channel == 2));
        const bool isWord = isFifo || ((dmaCnt[channel] & BIT(26)) != 0);
        const uint32_t alignMask = isWord ? ~3u : ~1u;
        const uint32_t step = isWord ? 4u : 2u;
        const uint32_t count = isFifo ? 4u : wordCounts[channel];
        const uint16_t waitCnt = core->memory.getWaitCnt();
        bool prevExec = core->memory.gbaCpuExecActive;
        core->memory.gbaCpuExecActive = false;
        bool dmaPakSeq = false;
        firstPakWasWrite = false;
        firstPakWasRead = false;
        uint32_t dmaCycles = 0;

        // v1.106: work RAM -> OAM transfers (per-scanline sprite tables) without the generic
        // per-unit bus and write dispatch. Neither side is on the cartridge bus, so every unit
        // costs the same; the prefetcher just sees that many cycles pass (advancePrefetchOnly is
        // additive), and an OAM write is a plain little-endian store. Same result as the loop below.
        {
            const uint32_t s0 = srcAddrs[channel] & srcMask, d0 = dstAddrs[channel] & dstMask;
            const uint32_t span = step * count;
            const bool srcInc = (srcAddrCnt == 0 || srcAddrCnt == 3), dstInc = (dstAddrCnt == 0 || dstAddrCnt == 3);
            if (!isFifo && count > 1 && ((s0 >> 24) == 2 || (s0 >> 24) == 3) && (d0 >> 24) == 7 &&
                (srcInc || srcAddrCnt == 2) && (dstInc || dstAddrCnt == 2) &&
                ((s0 + span - 1) >> 24) == (s0 >> 24) && ((d0 + span - 1) >> 24) == 7) {
                GbaBus &bus = core->gbaBus;
                const uint32_t cs = (uint32_t)bus.rawAccessCycles(s0, step, false, waitCnt);
                const uint32_t cd = (uint32_t)bus.rawAccessCycles(d0, step, false, waitCnt);
                dmaCycles += (uint32_t)bus.dataAccess(s0 & alignMask, step, false, waitCnt, true); // first unit: as before
                bus.advancePrefetchOnly((int)(count * (cs + cd) - cs), waitCnt);
                dmaCycles += count * (cs + cd) - cs;
                uint8_t *const oam = core->memory.gbaOamData();

                uint32_t sA = s0, dA = d0, latch = dmaLatch[channel];
                if (isWord) {
                    for (uint32_t i = 0; i < count; i++) {
                        const uint32_t val = core->memory.read<uint32_t>(cpu, sA & alignMask, false);
                        latch = val;
                        uint8_t *const o = &oam[dA & 0x3FCu];
#if NOODS_GBA_OBJ_MASK
                        if (GbaObjMask::changesMembership(oam,dA,val,4)) ++core->memory.oamGen;
#endif
                        o[0] = (uint8_t)val; o[1] = (uint8_t)(val >> 8); o[2] = (uint8_t)(val >> 16); o[3] = (uint8_t)(val >> 24);
                        if (srcInc) sA += 4u;
                        if (dstInc) dA += 4u;
                    }
                } else {
                    for (uint32_t i = 0; i < count; i++) {
                        const uint16_t val = core->memory.read<uint16_t>(cpu, sA & alignMask, false);
                        latch = uint32_t(val) * 0x00010001u;
                        uint8_t *const o = &oam[dA & 0x3FEu];
#if NOODS_GBA_OBJ_MASK
                        if (GbaObjMask::changesMembership(oam,dA,val,2)) ++core->memory.oamGen;
#endif
                        o[0] = (uint8_t)val; o[1] = (uint8_t)(val >> 8);
                        if (srcInc) sA += 2u;
                        if (dstInc) dA += 2u;
                    }
                }
                dmaLatch[channel] = latch;
                core->memory.setGbaDmaLatch(latch);
                if (srcInc) srcAddrs[channel] = sA & srcMask;
                if (dstInc) dstAddrs[channel] = dA & dstMask;
                goto gbaDmaDone;
            }
        }

        for (uint32_t i = 0; i < count; i++) {
            uint32_t sAddr = (srcAddrs[channel] & srcMask) & alignMask;
            uint32_t dAddr = (dstAddrs[channel] & dstMask) & alignMask;

            bool sPak = (sAddr >= 0x08000000u && sAddr < 0x0E000000u);
            if (sPak && !dmaPakSeq) firstPakWasRead = true;
            dmaCycles += core->gbaBus.dataAccess(sAddr, step, sPak ? dmaPakSeq : (i > 0), waitCnt, true);
            if (sPak) dmaPakSeq = true;

            if (isWord) {
                uint32_t val;
                if (sAddr >= 0x02000000u && sAddr < 0x10000000u) {
                    val = core->memory.read<uint32_t>(cpu, sAddr, false);
                    dmaLatch[channel] = val;
                } else {
                    val = dmaLatch[channel];
                }
                core->memory.setGbaDmaLatch(dmaLatch[channel]);
                core->memory.write<uint32_t>(cpu, dAddr, val, false);
            } else {
                uint16_t val;
                if (sAddr >= 0x02000000u && sAddr < 0x10000000u) {
                    val = core->memory.read<uint16_t>(cpu, sAddr, false);
                    dmaLatch[channel] = uint32_t(val) * 0x00010001u;
                } else {
                    val = uint16_t(dmaLatch[channel] >> ((dAddr & 2u) * 8u));
                }
                core->memory.setGbaDmaLatch(dmaLatch[channel]);
                core->memory.write<uint16_t>(cpu, dAddr, val, false);
            }

            bool dPak = (dAddr >= 0x08000000u && dAddr < 0x0E000000u);
            if (dPak && !dmaPakSeq) firstPakWasWrite = true;
            dmaCycles += core->gbaBus.dataAccess(dAddr, step, dPak ? dmaPakSeq : (i > 0), waitCnt, true);
            if (dPak) dmaPakSeq = true;

            // GamePak ROM bus (0x08000000..0x0DFFFFFF) always increments internal burst counter!
            if (sAddr >= 0x08000000u && sAddr < 0x0E000000u) {
                srcAddrs[channel] = (srcAddrs[channel] + step) & srcMask;
            } else if (srcAddrCnt == 0 || srcAddrCnt == 3) {
                srcAddrs[channel] = (srcAddrs[channel] + step) & srcMask;
            } else if (srcAddrCnt == 1) {
                srcAddrs[channel] = (srcAddrs[channel] - step) & srcMask;
            }

            if (!isFifo) {
                if (dstAddrCnt == 0 || dstAddrCnt == 3)
                    dstAddrs[channel] = (dstAddrs[channel] + step) & dstMask;
                else if (dstAddrCnt == 1)
                    dstAddrs[channel] = (dstAddrs[channel] - step) & dstMask;
            }
        }

    gbaDmaDone:
        dmaCycles += core->gbaBus.stepInternal(2, waitCnt);
        core->memory.gbaCpuExecActive = prevExec;
        core->interpreter[1].stallGbaDmaCycles(dmaCycles);

        if ((dmaCnt[channel] & BIT(25)) && mode != 0) {
            uint32_t rawCnt = dmaCnt[channel] & ((channel == 3) ? 0xFFFFu : 0x3FFFu);
            wordCounts[channel] = rawCnt ? rawCnt : ((channel == 3) ? 0x10000u : 0x4000u);
            if (dstAddrCnt == 3 && !isFifo)
                dstAddrs[channel] = dmaDad[channel] & dstMask;
        } else {
            dmaCnt[channel] &= ~BIT(31);
        }

        if (dmaCnt[channel] & BIT(30))
            core->interpreter[cpu].sendInterrupt(8 + channel);
        return;
    }

    // An ARM9 32-bit DMA to a fixed, unmapped packed-GXFIFO port normally
    // goes through Memory::writeFallback -> ioWrite9 for every word. The same
    // operation is the exact writeGxFifo call used by that switch, with a
    // full mask. Keep per-word reads, FIFO updates and the 112-entry break;
    // never coalesce event scheduling or FIFO transitions.
#if NOODS_DMA_GXFIFO_STATS
    if (!cpu && mode == 7) {
        ++g_dmaGxStats[0];
        g_dmaGxStats[1] += wordCounts[channel] < 112 ? wordCounts[channel] : 112;
        if (dstAddrCnt != 2) ++g_dmaGxStats[4];
        if (((dstAddrs[channel] & ~3u) - 0x04000400u) >= 0x40u) ++g_dmaGxStats[5];
        if (!(dmaCnt[channel] & BIT(26))) ++g_dmaGxStats[6];
        if (core->memory.mpuDataCheckActive) ++g_dmaGxStats[7];
    }
#endif
#if NOODS_DMA_GXFIFO_FAST
    if (!cpu && mode == 7 && (dmaCnt[channel] & BIT(26)) && dstAddrCnt == 2 &&
        ((dstAddrs[channel] & ~3u) - 0x04000400u) < 0x40u &&
        !core->memory.writeMap9B[dstAddrs[channel] >> 12] &&
        !core->memory.mpuDataCheckActive) {
#if NOODS_DMA_GXFIFO_STATS
        ++g_dmaGxStats[2];
#endif
#if NOODS_DMA_GXFIFO_MAP
        // Direct main-RAM reads for mapped 4-KiB pages with suitably aligned backing.
        // Accesses remain ordered one at a time, interleaved with each exact FIFO write.
        const uint32_t amount = wordCounts[channel] < 112u ? wordCounts[channel] : 112u;
        const uint32_t src = srcAddrs[channel];
        if (amount && srcAddrCnt == 0 && !core->dsiMode && (src >> 24) == 2 && (src & 3u) == 0u) {
            uint32_t curSrc = src;
            uint32_t remaining = amount;
            bool fastOk = true;
            while (remaining > 0) {
                uint8_t *const mapped = core->memory.readMap9B[curSrc >> 12];
                if (!mapped || (((uintptr_t)mapped & 3u) != 0u)) {
                    fastOk = false;
                    break;
                }
                const uint32_t pageOff = curSrc & 0xFFCu;
                const uint32_t pageAvailWords = (0x1000u - pageOff) >> 2;
                const uint32_t chunk = (remaining < pageAvailWords) ? remaining : pageAvailWords;
                uint32_t off = pageOff;
                for (uint32_t i = 0; i < chunk; ++i, off += 4) {
                    const uint32_t value = U8TO32(mapped, off);
                    core->gpu3D.writeGxFifo(0xFFFFFFFFu, value);
                }
                curSrc += chunk * 4u;
                remaining -= chunk;
            }
            if (fastOk) {
                srcAddrs[channel] = curSrc;
                gxFifoCount = amount;
                goto ndsDmaTransferred;
            }
        }
#endif
        for (unsigned int i = 0; i < wordCounts[channel]; ++i) {
            const uint32_t value = core->memory.read<uint32_t>(false, srcAddrs[channel], false);
            core->gpu3D.writeGxFifo(0xFFFFFFFFu, value);
            if (srcAddrCnt == 0) srcAddrs[channel] += 4;
            else if (srcAddrCnt == 1) srcAddrs[channel] -= 4;
#if NOODS_DMA_GXFIFO_STATS
            ++g_dmaGxStats[3];
#endif
            if (++gxFifoCount == 112) break;
        }
        goto ndsDmaTransferred;
    }
#endif
    // Perform the transfer
    if (core->gbaMode && mode == 6 && (channel == 1 || channel == 2)) { // GBA sound DMA
        LOG_INFO("ARM%d DMA channel %d transferring 4 words from 0x%X to 0x%X in mode %d\n",
            cpu ? 7 : 9, channel, srcAddrs[channel], dstAddrs[channel], mode);
        for (unsigned int i = 0; i < 4; i++) {
            // Transfer a word
            // GBA sound DMAs always transfer 4 words and never adjust the destination address
            uint32_t value = core->memory.read<uint32_t>(cpu, srcAddrs[channel], false);
            core->memory.write<uint32_t>(cpu, dstAddrs[channel], value, false);

            // Adjust the source address
            if (srcAddrCnt == 0) // Increment
                srcAddrs[channel] += 4;
            else if (srcAddrCnt == 1) // Decrement
                srcAddrs[channel] -= 4;
        }
    }
    else if (dmaCnt[channel] & BIT(26)) { // Whole word transfer
        LOG_INFO("ARM%d DMA channel %d transferring %d words from 0x%X to 0x%X in mode %d\n",
            cpu ? 7 : 9, channel, wordCounts[channel], srcAddrs[channel], dstAddrs[channel], mode);
        for (unsigned int i = 0; i < wordCounts[channel]; i++) {
            // Transfer a word
            uint32_t value = core->memory.read<uint32_t>(cpu, srcAddrs[channel], false);
            core->memory.write<uint32_t>(cpu, dstAddrs[channel], value, false);

            // Adjust the source address
            if (srcAddrCnt == 0) // Increment
                srcAddrs[channel] += 4;
            else if (srcAddrCnt == 1) // Decrement
                srcAddrs[channel] -= 4;

            // Adjust the destination address
            if (dstAddrCnt == 0 || dstAddrCnt == 3) // Increment
                dstAddrs[channel] += 4;
            else if (dstAddrCnt == 1) // Decrement
                dstAddrs[channel] -= 4;

            // In GXFIFO mode, only send 112 words at a time
            if (mode == 7 && ++gxFifoCount == 112)
                break;
        }
    }
    else { // Half-word transfer
        LOG_INFO("ARM%d DMA channel %d transferring %d half-words from 0x%X to 0x%X in mode %d\n",
            cpu ? 7 : 9, channel, wordCounts[channel], srcAddrs[channel], dstAddrs[channel], mode);
        for (unsigned int i = 0; i < wordCounts[channel]; i++) {
            // Transfer a half-word
            uint16_t value = core->memory.read<uint16_t>(cpu, srcAddrs[channel], false);
            core->memory.write<uint16_t>(cpu, dstAddrs[channel], value, false);

            // Adjust the source address
            if (srcAddrCnt == 0) // Increment
                srcAddrs[channel] += 2;
            else if (srcAddrCnt == 1) // Decrement
                srcAddrs[channel] -= 2;

            // Adjust the destination address
            if (dstAddrCnt == 0 || dstAddrCnt == 3) // Increment
                dstAddrs[channel] += 2;
            else if (dstAddrCnt == 1) // Decrement
                dstAddrs[channel] -= 2;

            // In GXFIFO mode, only send 112 words at a time
            if (mode == 7 && ++gxFifoCount == 112)
                break;
        }
    }

#if NOODS_DMA_GXFIFO_FAST
ndsDmaTransferred:
#endif
    if (mode == 7) {
        // Don't end a GXFIFO transfer if there are still words left
        wordCounts[channel] -= gxFifoCount;
        if (wordCounts[channel] > 0) {
            // Schedule another transfer immediately if the FIFO is still half empty
            if (core->gpu3D.readGxStat() & BIT(25))
                core->schedule(SchedTask(DMA9_TRANSFER0 + (cpu << 2) + channel), 1);
            return;
        }
    }

    if ((dmaCnt[channel] & BIT(25)) && mode != 0) { // Repeat
        // Reload the internal registers on repeat
        wordCounts[channel] = dmaCnt[channel] & 0x001FFFFF;
        if (dstAddrCnt == 3) // Increment and reload
            dstAddrs[channel] = dmaDad[channel];

        // In GXFIFO mode, schedule another transfer immediately if the FIFO is still half empty
        if (mode == 7 && core->gpu3D.readGxStat() & BIT(25))
            core->schedule(SchedTask(DMA9_TRANSFER0 + (cpu << 2) + channel), 1);
    }
    else {
        // End the transfer
        dmaCnt[channel] &= ~BIT(31);
    }

    // Trigger an end of transfer IRQ if enabled
    if (dmaCnt[channel] & BIT(30))
        core->interpreter[cpu].sendInterrupt(8 + channel);
}

void Dma::trigger(int mode, uint8_t channels) {
    // ARM7 DMAs don't use the lowest mode bit, so adjust accordingly
    if (cpu == 1) mode <<= 1;

    // Schedule a transfer on channels that are enabled and set to the triggered mode
    for (int i = 0; i < 4; i++) {
        if ((channels & BIT(i)) && (dmaCnt[i] & BIT(31)) && ((dmaCnt[i] & 0x38000000) >> 27) == mode)
            core->schedule(SchedTask(DMA9_TRANSFER0 + (cpu << 2) + i), core->gbaMode ? 2 : 1);
    }
}

void Dma::writeDmaSad(int channel, uint32_t mask, uint32_t value) {
    // Write to one of the DMASAD registers
    mask &= ((cpu == 0 || channel != 0) ? 0x0FFFFFFF : 0x07FFFFFF);
    dmaSad[channel] = (dmaSad[channel] & ~mask) | (value & mask);
}

void Dma::writeDmaDad(int channel, uint32_t mask, uint32_t value) {
    // Write to one of the DMADAD registers
    mask &= ((cpu == 0 || channel == 3) ? 0x0FFFFFFF : 0x07FFFFFF);
    dmaDad[channel] = (dmaDad[channel] & ~mask) | (value & mask);
}

void Dma::writeDmaCnt(int channel, uint32_t mask, uint32_t value) {
    uint32_t old = dmaCnt[channel];

    // Write to one of the DMACNT registers
    if (cpu == 0)
        mask &= 0xFFFFFFFF;
    else if (core->gbaMode)
        mask &= (channel == 3) ? 0xFFE0FFFF : 0xF7E03FFF;
    else
        mask &= (channel == 3) ? 0xF7E0FFFF : 0xF7E03FFF;
    dmaCnt[channel] = (dmaCnt[channel] & ~mask) | (value & mask);

    // In GXFIFO mode, schedule a transfer on the channel immediately if the FIFO is already half empty
    // All other modes are only triggered at the moment when the event happens
    // For example, if a word from the DS cart is ready before starting a DMA, the DMA will not be triggered
    if ((dmaCnt[channel] & BIT(31)) && ((dmaCnt[channel] & 0x38000000) >> 27) == 7 && (core->gpu3D.readGxStat() & BIT(25)))
        core->schedule(SchedTask(DMA9_TRANSFER0 + (cpu << 2) + channel), 1);

    // Don't reload the internal registers unless the enable bit changed from 0 to 1
    if ((old & BIT(31)) || !(dmaCnt[channel] & BIT(31)))
        return;

    // Reload the internal registers
    dstAddrs[channel] = dmaDad[channel];
    srcAddrs[channel] = dmaSad[channel];
    if (core->gbaMode) {
        uint32_t rawCnt = dmaCnt[channel] & ((channel == 3) ? 0xFFFFu : 0x3FFFu);
        wordCounts[channel] = rawCnt ? rawCnt : ((channel == 3) ? 0x10000u : 0x4000u);
    } else {
        wordCounts[channel] = dmaCnt[channel] & 0x001FFFFF;
    }

    // Schedule a transfer on the channel if it's set to immediate mode
    // Reloading seems to be the only trigger for this, so an enabled channel changed to immediate will never transfer
    // This also means that repeating doesn't work; in this case, the enabled bit is cleared after only one transfer
    // v1.107: with relaxed GBA timing a JIT block may run two more instructions before the
    // scheduler is consulted, so an immediate transfer is performed inside the register write.
    if (((dmaCnt[channel] & 0x38000000) >> 27) == 0) {
        if (Settings::gbaFastTiming && core->gbaMode)
            core->tasks[DMA9_TRANSFER0 + (cpu << 2) + channel](); // run it now, inside the write
        else
            core->schedule(SchedTask(DMA9_TRANSFER0 + (cpu << 2) + channel), core->gbaMode ? 2 : 1);
    }
}

uint32_t Dma::readDmaCnt(int channel) {
    // Read from one of the DMACNT registers
    // The lower half-word isn't readable in GBA mode
    return dmaCnt[channel] & ~(core->gbaMode ? 0x0000FFFF : 0x00000000);
}
