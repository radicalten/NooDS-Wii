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

void Timers::saveState(FILE *file) {
    // Write state data to the file
    fwrite(timers, 2, sizeof(timers) / 2, file);
    fwrite(shifts, 1, sizeof(shifts), file);
    fwrite(endCycles, 4, sizeof(endCycles) / 4, file);
    fwrite(enableCycles, 4, sizeof(enableCycles) / 4, file);
    fwrite(reloadWriteCycle, 4, sizeof(reloadWriteCycle) / 4, file);
    fwrite(prevTmCntL, 2, sizeof(prevTmCntL) / 2, file);
    fwrite(tmCntL, 2, sizeof(tmCntL) / 2, file);
    fwrite(tmCntH, 2, sizeof(tmCntH) / 2, file);
}

void Timers::loadState(FILE *file) {
    // Read state data from the file
    fread(timers, 2, sizeof(timers) / 2, file);
    fread(shifts, 1, sizeof(shifts), file);
    fread(endCycles, 4, sizeof(endCycles) / 4, file);
    fread(enableCycles, 4, sizeof(enableCycles) / 4, file);
    fread(reloadWriteCycle, 4, sizeof(reloadWriteCycle) / 4, file);
    fread(prevTmCntL, 2, sizeof(prevTmCntL) / 2, file);
    fread(tmCntL, 2, sizeof(tmCntL) / 2, file);
    fread(tmCntH, 2, sizeof(tmCntH) / 2, file);
}

void Timers::resetCycles() {
    // Adjust timer end cycles for a global cycle reset
    for (int i = 0; i < 4; i++) {
        endCycles[i] -= core->globalCycles;
        enableCycles[i] = (enableCycles[i] > core->globalCycles) ? (enableCycles[i] - core->globalCycles) : 0;
        reloadWriteCycle[i] = 0;
    }
}

void Timers::overflow(int timer) {
    // Ensure the timer is enabled and the end cycle is correct if not in count-up mode
    // The end cycle check ensures that if a timer was changed while running, outdated events are ignored
    if (!(tmCntH[timer] & BIT(7)) || ((timer == 0 || !(tmCntH[timer] & BIT(2)))
        && endCycles[timer] != core->globalCycles)) return;

    // If TMxCNT_L was written on or 1 cycle before the overflow tick, hardware still reloads the previous value once.
    uint16_t reloadVal = (core->gbaMode && reloadWriteCycle[timer] != 0 &&
                          core->globalCycles <= reloadWriteCycle[timer] + 1u)
                         ? prevTmCntL[timer] : tmCntL[timer];
    timers[timer] = reloadVal;
    if (timer == 0 || !(tmCntH[timer] & BIT(2))) {
        uint32_t nextSpan = (0x10000u - timers[timer]) << shifts[timer];
        core->schedule(SchedTask(TIMER9_OVERFLOW0 + (arm7 << 2) + timer), nextSpan);
        endCycles[timer] = core->globalCycles + nextSpan;
    }

    // Trigger a timer overflow IRQ if enabled
    if (tmCntH[timer] & BIT(6))
        core->interpreter[arm7].sendInterrupt(3 + timer);

    // Trigger a GBA sound FIFO event
    if (core->gbaMode && timer < 2)
        core->spu.gbaFifoTimer(timer);

    // If the next timer has count-up timing enabled, tick it now
    // In count-up timing mode, the timer only ticks when the previous timer overflows
    if (timer < 3 && (tmCntH[timer + 1] & BIT(7)) && (tmCntH[timer + 1] & BIT(2)) && ++timers[timer + 1] == 0)
        overflow(timer + 1);
}

void Timers::writeTmCntL(int timer, uint16_t mask, uint16_t value) {
    // Write to one of the TMCNT_L registers
    // Save 1-cycle shadow in case an overflow fires on this exact cycle
    prevTmCntL[timer] = tmCntL[timer];
    reloadWriteCycle[timer] = core->globalCycles;
    tmCntL[timer] = (tmCntL[timer] & ~mask) | (value & mask);
}

void Timers::writeTmCntH(int timer, uint16_t mask, uint16_t value) {
    // Update the current timer value if it's running on the scheduler
    bool dirty = false;
    if ((tmCntH[timer] & BIT(7)) && (timer == 0 || !(tmCntH[timer] & BIT(2))))
        timers[timer] = readTmCntL(timer);

    // Update the timer shift if the prescaler setting was changed
    // The prescaler allows timers to tick at frequencies of f/1, f/64, f/256, or f/1024 (when not in count-up mode)
    if (mask & 0xFF) {
        uint8_t shift = (((value & 0x3) && (timer == 0 || !(value & BIT(2)))) ? (4 + (value & 0x3) * 2) : 0);
        if (shifts[timer] != shift) {
            shifts[timer] = shift;
            dirty = true;
        }
    }

    bool newlyEnabled = !(tmCntH[timer] & BIT(7)) && (value & BIT(7));
    // Reload the counter if the enable bit changes from 0 to 1
    if (newlyEnabled) {
        timers[timer] = tmCntL[timer];
        dirty = true;
    }

    // Write to one of the TMCNT_H registers
    mask &= 0x00C7;
    tmCntH[timer] = (tmCntH[timer] & ~mask) | (value & mask);

    // Schedule a timer overflow if the timer changed and isn't in count-up mode
    if (dirty && (tmCntH[timer] & BIT(7)) && (timer == 0 || !(tmCntH[timer] & BIT(2)))) {
        if (core->gbaMode) {
            uint32_t enCyc = newlyEnabled ? (core->globalCycles + 2u) : core->globalCycles;
            enableCycles[timer] = enCyc;
            uint32_t pMask = (1u << shifts[timer]) - 1u;
            uint32_t firstAligned = (enCyc & ~pMask) + ((0x10000u - timers[timer]) << shifts[timer]);
            endCycles[timer] = firstAligned;
            core->schedule(SchedTask(TIMER9_OVERFLOW0 + (arm7 << 2) + timer), firstAligned - core->globalCycles);
        }
        else {
            core->schedule(SchedTask(TIMER9_OVERFLOW0 + (arm7 << 2) + timer), (0x10000 - timers[timer]) << shifts[timer]);
            endCycles[timer] = core->globalCycles + ((0x10000 - timers[timer]) << shifts[timer]);
        }
    }
}

uint16_t Timers::readTmCntL(int timer) {
    // Read the current timer value, updating it if it's running on the scheduler
    if ((tmCntH[timer] & BIT(7)) && (timer == 0 || !(tmCntH[timer] & BIT(2)))) {
        if (core->gbaMode) {
            if ((int32_t)(core->globalCycles - enableCycles[timer]) <= 0)
                return timers[timer];
            int32_t diff = (int32_t)(endCycles[timer] - core->globalCycles);
            if (diff > 0) {
                uint32_t pMask = (1u << shifts[timer]) - 1u;
                timers[timer] = uint16_t(0x10000u - ((uint32_t(diff) + pMask) >> shifts[timer]));
            }
            else {
                // Counter has just crossed endCycles mid-instruction before scheduled overflow ran!
                uint32_t elapsed = uint32_t(-diff);
                uint16_t firstReload = (reloadWriteCycle[timer] != 0 &&
                                        endCycles[timer] <= reloadWriteCycle[timer] + 1u)
                                       ? prevTmCntL[timer] : tmCntL[timer];
                uint32_t span1 = (0x10000u - firstReload) << shifts[timer];
                if (elapsed < span1) {
                    timers[timer] = uint16_t(firstReload + (elapsed >> shifts[timer]));
                }
                else {
                    elapsed -= span1;
                    uint32_t spanN = (0x10000u - tmCntL[timer]) << shifts[timer];
                    elapsed %= spanN;
                    timers[timer] = uint16_t(tmCntL[timer] + (elapsed >> shifts[timer]));
                }
            }
        }
        else {
            timers[timer] = 0x10000 - ((endCycles[timer] - core->globalCycles) >> shifts[timer]);
        }
    }
    return timers[timer];
}
