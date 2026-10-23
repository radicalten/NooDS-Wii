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
#include <cstring>

Cp15::Cp15(Core *core): core(core) {
    pageGen = new uint16_t[0x100000]();
    pagePerm = new uint8_t[0x100000]();
}

Cp15::~Cp15() {
    delete[] pageGen;
    delete[] pagePerm;
}

void Cp15::saveState(FILE *file) {
    // Write state data to the file
    fwrite(&ctrlReg, sizeof(ctrlReg), 1, file);
    fwrite(&dtcmReg, sizeof(dtcmReg), 1, file);
    fwrite(&itcmReg, sizeof(itcmReg), 1, file);
    fwrite(&procId, sizeof(procId), 1, file);
    fwrite(&dataCacheConfig, sizeof(dataCacheConfig), 1, file);
    fwrite(&instructionCacheConfig, sizeof(instructionCacheConfig), 1, file);
    fwrite(&writeBufferControl, sizeof(writeBufferControl), 1, file);
    fwrite(regionPerms, sizeof(regionPerms), 1, file);
    fwrite(regionControl, sizeof(regionControl), 1, file);
}

void Cp15::loadState(FILE *file) {
    // Read state data from the file
    uint32_t ctrl, dtcm, itcm;
    fread(&ctrl, sizeof(ctrl), 1, file);
    fread(&dtcm, sizeof(dtcm), 1, file);
    fread(&itcm, sizeof(itcm), 1, file);
    fread(&procId, sizeof(procId), 1, file);
    fread(&dataCacheConfig, sizeof(dataCacheConfig), 1, file);
    fread(&instructionCacheConfig, sizeof(instructionCacheConfig), 1, file);
    fread(&writeBufferControl, sizeof(writeBufferControl), 1, file);
    fread(regionPerms, sizeof(regionPerms), 1, file);
    fread(regionControl, sizeof(regionControl), 1, file);

    // Set registers along with values based on them
    write(1, 0, 0, ctrl);
    write(9, 1, 0, dtcm);
    write(9, 1, 1, itcm);
}

uint32_t Cp15::read(uint8_t cn, uint8_t cm, uint8_t cp) {
    // Read a value from a CP15 register.
    if (cn == 5 && cm == 0 && cp <= 3) {
        uint32_t value = 0;
        const bool wide = cp >= 2;
        const unsigned fieldBits = wide ? 4 : 2;
        for (unsigned region = 0; region < 8; region++) {
            uint32_t field;
            if (cp == 0) field = (regionPerms[region] >> 4) & 0x03;
            else if (cp == 1) field = regionPerms[region] & 0x03;
            else if (cp == 2) field = (regionPerms[region] >> 4) & 0x0F;
            else field = regionPerms[region] & 0x0F;
            value |= field << (region * fieldBits);
        }
        return value;
    }
    if (cn == 6 && cm < 8 && cp <= 1)
        return regionControl[cm];

    switch ((cn << 16) | (cm << 8) | cp) {
        case 0x000000: case 0x000003: case 0x000004: case 0x000005:
        case 0x000006: case 0x000007: return 0x41059461; // Main ID (mirrored)
        case 0x000001: return 0x0F0D2112; // Cache type
        case 0x000002: return 0x00140180; // TCM size
        case 0x010000: return ctrlReg; // Control
        case 0x020000: return dataCacheConfig; // Data cache configuration
        case 0x020001: return instructionCacheConfig; // Instruction cache configuration
        case 0x030000: return writeBufferControl; // Write-buffer control
        case 0x090100: return dtcmReg; // Data TCM base/size
        case 0x090101: return itcmReg; // Instruction TCM size
        case 0x0D0001: case 0x0D0101: return procId; // Trace process ID

    default:
        LOG_WARN("Unknown CP15 register read: C%d,C%d,%d\n", cn, cm, cp);
        return 0;
    }
}

uint8_t Cp15::fillPagePermSlow(uint32_t page) {
    const uint32_t addr = page << 12;
    uint8_t p = 0;
    if (!(ctrlReg & BIT(0))) {
        p = 0x3F;
    } else {
        if (checkAccess(addr, false, true,  true))  p |= 0x01;
        if (checkAccess(addr, true,  true,  true))  p |= 0x02;
        if (checkAccess(addr, false, false, true))  p |= 0x04;
        if (checkAccess(addr, false, true,  false)) p |= 0x08;
        if (checkAccess(addr, true,  true,  false)) p |= 0x10;
        if (checkAccess(addr, false, false, false)) p |= 0x20;
    }
    if (pageGen) {
        pageGen[page] = mpuGen;
        pagePerm[page] = p;
    }
    return p;
}

void Cp15::updateMpuState() {
    if (++mpuGen == 0) {
        if (pageGen) memset(pageGen, 0, sizeof(uint16_t) * 0x100000);
        mpuGen = 1;
    }
    if (!(ctrlReg & BIT(0))) {
        mpuRestricted = false;
        mpuFastPriv = true;
        return;
    }
    // Fast-path check: unrestricted only when region 0 covers full 4GB with AP=3/3
    // and no higher region is enabled with non-full permissions.
    bool fullBg = ((regionControl[0] & 0x3F) == 0x3F) && (regionPerms[0] == 0x33);
    bool higherRestricted = false;
    for (int r = 1; r < 8; r++) {
        if ((regionControl[r] & 1) && regionPerms[r] != 0x33) {
            higherRestricted = true;
            break;
        }
    }
    mpuRestricted = !fullBg || higherRestricted;

    // Standard libnds / BlocksDS / commercial privileged layout check:
    // Standard ARM9 data regions (DTCM stack, Main RAM, I/O..OAM/VRAM) grant privileged data R/W,
    // ITCM (at 0x01000000 or 0x01FF8000) grants privileged data read (BlocksDS maps .itcm read-only, AP=6),
    // and no region in [0x02000000..0x08000000) restricts privileged R/W (other than a 4 KB DTCM canary).
    bool itcmOk = checkAccess(0x01000000u, false, true, true) ||
                  checkAccess(0x01FF8000u, false, true, true);
    bool privOk =
        itcmOk &&
        checkAccess(dtcmAddr + 0x3FF0u, false, true, true) && checkAccess(dtcmAddr + 0x3FF0u, true, true, true) &&
        checkAccess(0x02000000u, false, true, true) && checkAccess(0x02000000u, true, true, true) &&
        checkAccess(0x04000000u, false, true, true) && checkAccess(0x04000000u, true, true, true) &&
        checkAccess(0x06000000u, false, true, true) && checkAccess(0x06000000u, true, true, true);
    if (privOk) {
        for (int r = 0; r < 8; r++) {
            uint32_t reg = regionControl[r];
            if (!(reg & 1)) continue;
            uint32_t sizeField = (reg >> 1) & 0x1F;
            if (sizeField < 11) continue;
            uint32_t base = (sizeField < 31) ? (reg & ~((2u << sizeField) - 1u)) : 0u;
            uint8_t dataAp = (regionPerms[r] >> 4) & 0x0F;
            const bool privRW = (dataAp == 1 || dataAp == 2 || dataAp == 3);
            if (!privRW && base >= 0x02000000u && base < 0x08000000u &&
                !(base == dtcmAddr && sizeField == 11) &&
                !checkAccess(base, true, true, true)) {
                privOk = false;
                break;
            }
        }
    }
    mpuFastPriv = privOk;
}

bool Cp15::checkAccess(uint32_t addr, bool isWrite, bool isData, bool privileged) const {
    if (!(ctrlReg & BIT(0)))
        return true;

    for (int r = 7; r >= 0; r--) {
        uint32_t reg = regionControl[r];
        if (!(reg & 1))
            continue;
        uint32_t sizeField = (reg >> 1) & 0x1F;
        if (sizeField < 11)
            continue;
        if (sizeField < 31) {
            uint32_t mask = ~((2u << sizeField) - 1u);
            if ((addr & mask) != (reg & mask))
                continue;
        }
        uint8_t ap = isData ? ((regionPerms[r] >> 4) & 0x0F) : (regionPerms[r] & 0x0F);
        switch (ap) {
            case 0: return false;
            case 1: return privileged;
            case 2: return privileged ? true : !isWrite;
            case 3: return true;
            case 5: return privileged ? !isWrite : false;
            case 6: return !isWrite;
            default: return false;
        }
    }
    return false;
}

void Cp15::write(uint8_t cn, uint8_t cm, uint8_t cp, uint32_t value) {
    // Write a value to a CP15 register.
    uint32_t oldAddr, oldSize;
    if (cn == 5 && cm == 0 && cp <= 3) {
        const bool wide = cp >= 2;
        const unsigned fieldBits = wide ? 4 : 2;
        const uint32_t fieldMask = wide ? 0x0F : 0x03;
        for (unsigned region = 0; region < 8; region++) {
            const uint8_t field = (value >> (region * fieldBits)) & fieldMask;
            if (cp == 0 || cp == 2)
                regionPerms[region] = (regionPerms[region] & 0x0F) | (field << 4);
            else
                regionPerms[region] = (regionPerms[region] & 0xF0) | field;
        }
        updateMpuState();
        return;
    }
    if (cn == 6 && cm < 8 && cp <= 1) {
        regionControl[cm] = value & 0xFFFFF03F;
        updateMpuState();
        return;
    }

    switch ((cn << 16) | (cm << 8) | cp) {
    case 0x020000: // Data cache configuration
        dataCacheConfig = value & 0xFF;
        return;

    case 0x020001: // Instruction cache configuration
        instructionCacheConfig = value & 0xFF;
        return;

    case 0x030000: // Write-buffer control
        writeBufferControl = value & 0xFF;
        return;

    case 0x010000: // Control
        // Set writable control bits and update their state values
        ctrlReg = (ctrlReg & ~0xFF085) | (value & 0xFF085);
        exceptionAddr = (ctrlReg & BIT(13)) ? 0xFFFF0000 : 0x00000000;
        dtcmCanRead = (ctrlReg & BIT(16)) && !(ctrlReg & BIT(17));
        dtcmCanWrite = (ctrlReg & BIT(16));
        itcmCanRead = (ctrlReg & BIT(18)) && !(ctrlReg & BIT(19));
        itcmCanWrite = (ctrlReg & BIT(18));
        updateMpuState();

        // Update the memory map at the current TCM locations
        core->memory.updateMap9(dtcmAddr, dtcmAddr + dtcmSize, true);
        core->memory.updateMap9(0x00000000, itcmSize, true);
        return;

    case 0x090100: // Data TCM base/size
        // Set the DTCM register and update its address and size
        dtcmReg = value;
        oldAddr = dtcmAddr;
        oldSize = dtcmSize;
        dtcmAddr = dtcmReg & 0xFFFFF000;
        dtcmSize = std::max(0x1000, 0x200 << ((dtcmReg >> 1) & 0x1F)); // Min 4KB

        // Update the memory map at the old and new DTCM areas
        core->memory.updateMap9(oldAddr, oldAddr + oldSize, true);
        core->memory.updateMap9(dtcmAddr, dtcmAddr + dtcmSize, true);
        return;

    case 0x070004: case 0x070802: // Wait for interrupt
        // Halt the CPU
        core->interpreter[0].halt(0);
        return;

    case 0x090101: // Instruction TCM size
        // Set the ITCM register and update its size
        itcmReg = value;
        oldSize = itcmSize;
        itcmSize = std::max(0x1000, 0x200 << ((itcmReg >> 1) & 0x1F)); // Min 4KB

        // Update the memory map at the old and new ITCM areas
        core->memory.updateMap9(0x00000000, std::max(oldSize, itcmSize), true);
        return;

    case 0x0D0001: case 0x0D0101: // Trace process ID
        // Set the trace process ID register
        procId = value;
        return;

    default:
        LOG_WARN("Unknown CP15 register write: C%d,C%d,%d\n", cn, cm, cp);
        return;
    }
}
