/*
    Copyright (C) 2026 radicalten

    Cycle-accurate GBA Memory Controller, Waitstate Decoder, and 8-Halfword
    GamePak Prefetch Buffer model for NooDS-Wii.
*/

#pragma once

#include <cstdint>
#include <cstdio>

struct GbaBus {
    uint32_t headAddr = 0;
    uint32_t tailAddr = 0;
    uint8_t  count    = 0;
    uint8_t  duty     = 0;
    bool     active   = false;
    bool     nextSeq  = false;

    // Per-instruction tracking flags
    bool     inSeqAccess = false;
    bool     hadDataAccess = false;
    bool     hadLoad = false;
    bool     branched = false;
    bool     lastFetchUsedDuty = false;
    int      preSwiSaved = 0;

    void reset() {
        headAddr = tailAddr = 0;
        count = duty = 0;
        active = false;
        nextSeq = false;
        inSeqAccess = hadDataAccess = hadLoad = branched = false;
    }

    void saveState(FILE *file) {
        fwrite(&headAddr, sizeof(headAddr), 1, file);
        fwrite(&tailAddr, sizeof(tailAddr), 1, file);
        fwrite(&count, sizeof(count), 1, file);
        fwrite(&duty, sizeof(duty), 1, file);
        fwrite(&active, sizeof(active), 1, file);
        fwrite(&nextSeq, sizeof(nextSeq), 1, file);
    }

    void loadState(FILE *file) {
        fread(&headAddr, sizeof(headAddr), 1, file);
        fread(&tailAddr, sizeof(tailAddr), 1, file);
        fread(&count, sizeof(count), 1, file);
        fread(&duty, sizeof(duty), 1, file);
        fread(&active, sizeof(active), 1, file);
        fread(&nextSeq, sizeof(nextSeq), 1, file);
    }

    void beginInstruction() {
        inSeqAccess = false;
        hadDataAccess = false;
        hadLoad = false;
        branched = false;
    }

    static inline int sramWait(uint16_t waitCnt) {
        static const uint8_t tbl[4] = { 5, 4, 3, 9 };
        return tbl[waitCnt & 3u];
    }

    static inline int romWaitN(uint32_t addr, uint16_t waitCnt) {
        static const uint8_t tbl[4] = { 5, 4, 3, 9 };
        uint8_t region = (addr >> 24) & 0xFFu;
        if (region < 0x0Au) return tbl[(waitCnt >> 2) & 3u];
        if (region < 0x0Cu) return tbl[(waitCnt >> 5) & 3u];
        return tbl[(waitCnt >> 8) & 3u];
    }

    static inline int romWaitS(uint32_t addr, uint16_t waitCnt) {
        uint8_t region = (addr >> 24) & 0xFFu;
        if (region < 0x0Au) return (waitCnt & (1u << 4)) ? 2 : 3;
        if (region < 0x0Cu) return (waitCnt & (1u << 7)) ? 2 : 5;
        return (waitCnt & (1u << 10)) ? 2 : 9;
    }

    inline int prefetcherTargetCycles(uint16_t waitCnt) const {
        if ((tailAddr & 0x1FFFEu) == 0 && count == 0 && duty == 0 && !nextSeq)
            return romWaitN(tailAddr, waitCnt);
        return (tailAddr & 0x1FFFEu) == 0 ? romWaitN(tailAddr, waitCnt) : romWaitS(tailAddr, waitCnt);
    }

    inline void advancePrefetchOnly(int n, uint16_t waitCnt) {
        if (!(waitCnt & (1u << 14)) || !active)
            return;
        while (n > 0) {
            if (count >= 8) {
                duty = 0;
                return;
            }
            int req = prefetcherTargetCycles(waitCnt);
            int rem = req - duty;
            if (n >= rem) {
                n -= rem;
                duty = 0;
                count++;
                tailAddr += 2;
            } else {
                duty += (uint8_t)n;
                n = 0;
            }
        }
    }

    inline int stepInternal(int n, uint16_t waitCnt) {
        if (n <= 0)
            return 0;
        advancePrefetchOnly(n, waitCnt);
        nextSeq = false;
        return n;
    }

    inline int fetchHalfwordRom(uint32_t addr, uint16_t waitCnt) {
        addr &= ~1u;
        if (waitCnt & (1u << 14)) {
            if (active && count > 0 && addr == headAddr) {
                // Buffered hit: 1 CPU cycle while prefetcher simultaneously steps 1 cycle!
                headAddr += 2;
                count--;
                advancePrefetchOnly(1, waitCnt);
                nextSeq = true;
                return 1;
            }
            if (active && count == 0 && addr == tailAddr) {
                // Currently in-flight halfword!
                int req = prefetcherTargetCycles(waitCnt);
                int wait = req - duty;
                if (duty > 0)
                    lastFetchUsedDuty = true;
                duty = 0;
                headAddr = tailAddr = addr + 2;
                nextSeq = true;
                return wait;
            }
            // Buffer miss: restart prefetcher after non-sequential fetch
            int wait = (nextSeq && (addr & 0x1FFFEu) != 0) ? romWaitS(addr, waitCnt) : romWaitN(addr, waitCnt);
            headAddr = tailAddr = addr + 2;
            count = 0;
            duty = 0;
            active = true;
            nextSeq = true;
            return wait;
        }
        else {
            active = false;
            count = 0;
            duty = 0;
            int wait = (nextSeq && (addr & 0x1FFFEu) != 0) ? romWaitS(addr, waitCnt) : romWaitN(addr, waitCnt);
            nextSeq = true;
            return wait;
        }
    }

    inline int fetchOpcode(uint32_t addr, bool isArm, uint16_t waitCnt) {
        lastFetchUsedDuty = false;
        uint8_t region = (addr >> 24) & 0xFFu;
        if (region >= 0x08u && region < 0x0Eu) {
            if (isArm) {
                uint32_t wAddr = addr & ~3u;
                // 32-bit output bus from prefetch buffer pops 2 ready halfwords in 1 CPU cycle,
                // or if 1 halfword is buffered and the 2nd is in-flight, only waits remaining cycles of the 2nd!
                if ((waitCnt & (1u << 14)) && active && wAddr == headAddr) {
                    if (count >= 2) {
                        headAddr += 4;
                        count -= 2;
                        advancePrefetchOnly(1, waitCnt);
                        nextSeq = true;
                        return 1;
                    }
                    if (count == 1) {
                        int req = prefetcherTargetCycles(waitCnt);
                        int rem = req - duty;
                        if (duty > 0)
                            lastFetchUsedDuty = true;
                        headAddr = tailAddr = wAddr + 4;
                        count = 0;
                        duty = 0;
                        nextSeq = true;
                        return (rem > 1) ? rem : 1;
                    }
                }
                int c0 = fetchHalfwordRom(wAddr, waitCnt);
                int c1 = fetchHalfwordRom(wAddr + 2u, waitCnt);
                return c0 + c1;
            }
            return fetchHalfwordRom(addr & ~1u, waitCnt);
        }
        // Non-ROM instruction fetch: GamePak bus is idle so prefetcher can advance if active!
        int cyc = 1;
        switch (region) {
            case 0x02: cyc = isArm ? 6 : 3; break;
            case 0x05:
            case 0x06: cyc = isArm ? 2 : 1; break;
            case 0x0E:
            case 0x0F: cyc = sramWait(waitCnt); active = false; count = 0; duty = 0; nextSeq = false; return cyc;
            default:   cyc = 1; break;
        }
        advancePrefetchOnly(cyc, waitCnt);
        nextSeq = true;
        return cyc;
    }

    inline int branchRefill(uint32_t targetPc, bool isArm, uint16_t waitCnt) {
        branched = true;
        active = false;
        count = 0;
        duty = 0;
        nextSeq = false;
        int c0 = fetchOpcode(targetPc, isArm, waitCnt);
        int c1 = fetchOpcode(targetPc + (isArm ? 4u : 2u), isArm, waitCnt);
        return c0 + c1;
    }

    inline int rawAccessCycles(uint32_t addr, uint32_t sizeBytes, bool isSeq, uint16_t waitCnt) const {
        uint8_t region = (addr >> 24) & 0xFFu;
        switch (region) {
            case 0x02:
                return (sizeBytes == 4) ? 6 : 3;
            case 0x05:
            case 0x06:
                return (sizeBytes == 4) ? 2 : 1;
            case 0x08: case 0x09:
            case 0x0A: case 0x0B:
            case 0x0C: case 0x0D: {
                bool seq0 = isSeq && ((addr & 0x1FFFEu) != 0);
                int first = seq0 ? romWaitS(addr, waitCnt) : romWaitN(addr, waitCnt);
                if (sizeBytes < 4)
                    return first;
                uint32_t addr2 = addr + 2u;
                int second = ((addr2 & 0x1FFFEu) == 0) ? romWaitN(addr2, waitCnt) : romWaitS(addr2, waitCnt);
                return first + second;
            }
            case 0x0E:
            case 0x0F:
                return sramWait(waitCnt);
            default:
                return 1;
        }
    }

    inline int dataAccess(uint32_t addr, uint32_t sizeBytes, bool isSeq, uint16_t waitCnt, bool fromDma) {
        uint8_t region = (addr >> 24) & 0xFFu;
        if (region >= 0x08u && region <= 0x0Fu) {
            int stall = 0;
            if (!fromDma && (waitCnt & (1u << 14)) && active && duty > 0) {
                int req = prefetcherTargetCycles(waitCnt);
                if (req - duty == 1)
                    stall = 1;
            }
            active = false;
            count = 0;
            duty = 0;
            nextSeq = false;
            return stall + rawAccessCycles(addr, sizeBytes, isSeq, waitCnt);
        }
        int cyc = rawAccessCycles(addr, sizeBytes, isSeq, waitCnt);
        advancePrefetchOnly(cyc, waitCnt);
        // Unmapped high open-bus (>= 0x10000000) never drives GamePak CS, preserving sequential burst counter!
        if (region < 0x10u)
            nextSeq = false;
        return cyc;
    }
};
