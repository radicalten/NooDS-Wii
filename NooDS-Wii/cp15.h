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

class Core;

class Cp15 {
public:
    uint32_t exceptionAddr = 0;
    bool dtcmCanRead = false, dtcmCanWrite = false;
    bool itcmCanRead = false, itcmCanWrite = false;
    bool mpuRestricted = false;
    bool mpuFastPriv = true;
    uint32_t dtcmAddr = 0, dtcmSize = 0;
    uint32_t itcmSize = 0;

    Cp15(Core *core);
    ~Cp15();
    void saveState(FILE *file);
    void loadState(FILE *file);

    uint32_t read(uint8_t cn, uint8_t cm, uint8_t cp);
    void write(uint8_t cn, uint8_t cm, uint8_t cp, uint32_t value);
    bool checkAccess(uint32_t addr, bool isWrite, bool isData, bool privileged) const;

    // O(1) generation-stamped 4 KB page permission lookup:
    // bit 0 = privDataRead, bit 1 = privDataWrite, bit 2 = privInstrExec,
    // bit 3 = userDataRead, bit 4 = userDataWrite, bit 5 = userInstrExec.
    inline uint8_t getPagePerm(uint32_t page) {
        if (__builtin_expect(pageGen && pageGen[page] == mpuGen, 1))
            return pagePerm[page];
        return fillPagePermSlow(page);
    }

private:
    void updateMpuState();
    uint8_t fillPagePermSlow(uint32_t page);
    Core *core;
    uint16_t *pageGen = nullptr;
    uint8_t *pagePerm = nullptr;
    uint16_t mpuGen = 1;
    uint32_t ctrlReg = 0x78;
    uint32_t dtcmReg = 0x00;
    uint32_t itcmReg = 0x00;
    uint32_t procId = 0x00;
    uint32_t dataCacheConfig = 0x00;
    uint32_t instructionCacheConfig = 0x00;
    uint32_t writeBufferControl = 0x00;
    uint8_t regionPerms[8] = {};
    uint32_t regionControl[8] = {};
};
