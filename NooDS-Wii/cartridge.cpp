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
#include "core.h"

Cartridge::~Cartridge() {
    writeSave();
    if (romFile) fclose(romFile);
    if (rom) delete[] rom;
    if (save) delete[] save;
}

bool Cartridge::setRom(std::string romPath, int romFd, int saveFd, int stateFd, int cheatFd) {
    std::string basePath = romPath.substr(0, romPath.rfind('.'));
    std::string savePath = basePath + (core->id ? (".sv" + std::to_string(core->id + 1)) : ".sav");
    std::string statePath = basePath + (core->id ? (".no" + std::to_string(core->id + 1)) : ".noo");
    std::string cheatPath = basePath + ".cht";

    if (Settings::savesFolder)
        savePath = Settings::basePath + "/saves" + savePath.substr(savePath.find_last_of("/\\"));
    if (Settings::statesFolder)
        statePath = Settings::basePath + "/states" + statePath.substr(statePath.find_last_of("/\\"));
    if (Settings::cheatsFolder)
        cheatPath = Settings::basePath + "/cheats" + cheatPath.substr(cheatPath.find_last_of("/\\"));

    bool gba = (this == &core->cartridgeGba);
    if (romFd == -1) this->romPath = romPath; else this->romFd = romFd;
    if (saveFd == -1) this->savePath = savePath; else this->saveFd = saveFd;
    (stateFd == -1) ? core->saveStates.setPath(statePath, gba) : core->saveStates.setFd(stateFd, gba);
    if (!gba) (cheatFd == -1) ? core->actionReplay.setPath(cheatPath) : core->actionReplay.setFd(cheatFd);
    return loadRom();
}

bool Cartridge::loadRom() {
    romFile = (romFd == -1) ? fopen(romPath.c_str(), "rb") : fdopen(dup(romFd), "rb");
    if (!romFile) return false;
    fseek(romFile, 0, SEEK_END);
    romSize = ftell(romFile);
    fseek(romFile, 0, SEEK_SET);

    if (FILE *saveFile = (saveFd == -1) ? fopen(savePath.c_str(), "rb") : fdopen(dup(saveFd), "rb")) {
        fseek(saveFile, 0, SEEK_END);
        saveSize = ftell(saveFile);
        fseek(saveFile, 0, SEEK_SET);
        save = new uint8_t[saveSize];
        fread(save, sizeof(uint8_t), saveSize, saveFile);
        fclose(saveFile);
    }

    for (size_t i = 0; i < saveSizes.size(); i++)
        if (saveSize == saveSizes[i]) return true;
    saveSize = -1;
    return true;
}

void Cartridge::loadRomSection(size_t offset, size_t size) {
    if (rom) delete[] rom;
    rom = new uint8_t[size];
    fseek(romFile, offset, SEEK_SET);
    fread(rom, sizeof(uint8_t), size, romFile);
    core->dldi.patchRom(rom, offset, size);
}

void Cartridge::writeSave() {
    PPCIrqState st = PPCIrqLockByMsr();
    if (saveDirty) {
        if (FILE *saveFile = (saveFd == -1) ? fopen(savePath.c_str(), "wb") : fdopen(dup(saveFd), "wb")) {
            if (saveFd != -1) {
                fseek(saveFile, 0, SEEK_SET);
                ftruncate(saveFd, saveSize);
            }

            LOG_INFO("Writing save file to disk\n");
            fwrite(save, sizeof(uint8_t), saveSize, saveFile);
            fclose(saveFile);
            saveDirty = false;
        }
    }
    PPCIrqUnlockByMsr(st);
}

void Cartridge::trimRom() {
    int newSize;
    for (newSize = romSize & ~3; newSize > 0; newSize -= 4) {
        if (U8TO32(rom, newSize - 4) != 0xFFFFFFFF)
            break;
    }

    if (newSize < romSize) {
        romSize = newSize;
        uint8_t *newRom = new uint8_t[newSize];
        memcpy(newRom, rom, newSize * sizeof(uint8_t));
        delete[] rom;
        rom = newRom;

        FILE *romFile = (romFd == -1) ? fopen(romPath.c_str(), "wb") : fdopen(dup(romFd), "wb");
        if (romFile) {
            if (newSize > 0)
                fwrite(rom, sizeof(uint8_t), newSize, romFile);
            fclose(romFile);
        }
    }
}

void Cartridge::resizeSave(int newSize, bool dirty) {
    PPCIrqState st = PPCIrqLockByMsr();
    uint8_t *newSave = new uint8_t[newSize];

    if (saveSize < newSize) {
        if (saveSize < 0) saveSize = 0;
        memcpy(newSave, save, saveSize * sizeof(uint8_t));
        memset(&newSave[saveSize], 0xFF, (newSize - saveSize) * sizeof(uint8_t));
    }
    else {
        memcpy(newSave, save, newSize * sizeof(uint8_t));
    }

    delete[] save;
    save = newSave;
    saveSize = newSize;
    if (dirty) saveDirty = true;
    PPCIrqUnlockByMsr(st);
}

void CartridgeNds::saveState(FILE *file) {
    fwrite(&saveSize, sizeof(saveSize), 1, file);
    if (saveSize > 0) fwrite(save, 1, saveSize, file);
    fwrite(&cmdMode, sizeof(cmdMode), 1, file);
    fwrite(encTable, 4, sizeof(encTable) / 4, file);
    fwrite(encCode, 4, sizeof(encCode) / 4, file);
    fwrite(romAddrReal, 4, sizeof(romAddrReal) / 4, file);
    fwrite(romAddrVirt, 4, sizeof(romAddrVirt) / 4, file);
    fwrite(blockSize, 2, sizeof(blockSize) / 2, file);
    fwrite(readCount, 2, sizeof(readCount) / 2, file);
    fwrite(wordCycles, 4, sizeof(wordCycles) / 4, file);
    fwrite(encrypted, sizeof(bool), sizeof(encrypted) / sizeof(bool), file);
    fwrite(auxCommand, 1, sizeof(auxCommand), file);
    fwrite(auxAddress, 4, sizeof(auxAddress) / 4, file);
    fwrite(auxWriteCount, 4, sizeof(auxWriteCount) / 4, file);
    fwrite(auxSpiCnt, 2, sizeof(auxSpiCnt) / 2, file);
    fwrite(auxSpiData, 1, sizeof(auxSpiData), file);
    fwrite(romCtrl, 4, sizeof(romCtrl) / 4, file);
    fwrite(romCmdOut, 8, sizeof(romCmdOut) / 8, file);
}

void CartridgeNds::loadState(FILE *file) {
    fread(&saveSize, sizeof(saveSize), 1, file);
    if (saveSize > 0) fread(save, 1, saveSize, file);
    fread(&cmdMode, sizeof(cmdMode), 1, file);
    fread(encTable, 4, sizeof(encTable) / 4, file);
    fread(encCode, 4, sizeof(encCode) / 4, file);
    fread(romAddrReal, 4, sizeof(romAddrReal) / 4, file);
    fread(romAddrVirt, 4, sizeof(romAddrVirt) / 4, file);
    fread(blockSize, 2, sizeof(blockSize) / 2, file);
    fread(readCount, 2, sizeof(readCount) / 2, file);
    fread(wordCycles, 4, sizeof(wordCycles) / 4, file);
    fread(encrypted, sizeof(bool), sizeof(encrypted) / sizeof(bool), file);
    fread(auxCommand, 1, sizeof(auxCommand), file);
    fread(auxAddress, 4, sizeof(auxAddress) / 4, file);
    fread(auxWriteCount, 4, sizeof(auxWriteCount) / 4, file);
    fread(auxSpiCnt, 2, sizeof(auxSpiCnt) / 2, file);
    fread(auxSpiData, 1, sizeof(auxSpiData), file);
    fread(romCtrl, 4, sizeof(romCtrl) / 4, file);
    fread(romCmdOut, 8, sizeof(romCmdOut) / 8, file);

    saveDirty = false;
}

bool CartridgeNds::loadRom() {
    if (saveSizes.empty()) {
        saveSizes.push_back(0x000000);
        saveSizes.push_back(0x000200);
        saveSizes.push_back(0x002000);
        saveSizes.push_back(0x008000);
        saveSizes.push_back(0x010000);
        saveSizes.push_back(0x020000);
        saveSizes.push_back(0x040000);
        saveSizes.push_back(0x080000);
        saveSizes.push_back(0x100000);
        saveSizes.push_back(0x800000);
    }

    if (!Cartridge::loadRom()) {
        return false;
    }
    else if (Settings::romInRam) {
        try {
            loadRomSection(0, romSize);
            fclose(romFile);
            romFile = nullptr;
        }
        catch (std::bad_alloc &ba) {
            loadRomSection(0, 0x5000);
        }
    }
    else {
        loadRomSection(0, 0x5000);
    }

    core->memory.copyBiosLogo(&rom[0xC0]);

    for (romMask = 1; romMask < romSize; romMask <<= 1);
    romMask -= 1;

    romCode = U8TO32(rom, 0x0C);

    if (romSize >= 0x8000) {
        uint64_t data = U8TO64(rom, 0x4000);
        initKeycode(2);
        data = decrypt64(data);
        initKeycode(3);
        data = decrypt64(data);

        if (data == 0x6A624F7972636E65) {
            LOG_INFO("Detected an encrypted ROM!\n");
            romEncrypted = true;
        }
    }
    return true;
}

void CartridgeNds::directBoot() {
    if (romFile)
        loadRomSection(0, 0x170);

    uint32_t offset9 = U8TO32(rom, 0x20);
    core->interpreter[0].entryAddr = U8TO32(rom, 0x24);
    uint32_t ramAddr9 = U8TO32(rom, 0x28);
    uint32_t size9 = U8TO32(rom, 0x2C);
    LOG_INFO("ARM9 code ROM offset: 0x%X\n", offset9);
    LOG_INFO("ARM9 code entry address: 0x%X\n", core->interpreter[0].entryAddr);
    LOG_INFO("ARM9 RAM address: 0x%X\n", ramAddr9);
    LOG_INFO("ARM9 code size: 0x%X\n", size9);

    uint32_t offset7 = U8TO32(rom, 0x30);
    core->interpreter[1].entryAddr = U8TO32(rom, 0x34);
    uint32_t ramAddr7 = U8TO32(rom, 0x38);
    uint32_t size7 = U8TO32(rom, 0x3C);
    LOG_INFO("ARM7 code ROM offset: 0x%X\n", offset7);
    LOG_INFO("ARM7 code entry address: 0x%X\n", core->interpreter[1].entryAddr);
    LOG_INFO("ARM7 RAM address: 0x%X\n", ramAddr7);
    LOG_INFO("ARM7 code size: 0x%X\n", size7);

    for (uint32_t i = 0; i < 0x170; i += 4)
        core->memory.write<uint32_t>(0, 0x27FFE00 + i, U8TO32(rom, i));

    uint32_t offset;
    if (romFile) {
        loadRomSection(offset9, size9);
        offset = 0;
    }
    else {
        offset = offset9;
    }

    for (uint32_t i = 0; i < size9; i += 4) {
        if (romEncrypted && offset9 + i >= 0x4000 && offset9 + i < 0x4800) {
            if (offset9 + i < 0x4008) {
                core->memory.write<uint32_t>(0, ramAddr9 + i, 0xE7FFDEFF);
            }
            else {
                initKeycode(3);
                uint64_t data = decrypt64(U8TO64(rom, (offset + i) & ~7));
                core->memory.write<uint32_t>(0, ramAddr9 + i, data >> (((offset + i) & 4) ? 32 : 0));
            }
        }
        else {
            core->memory.write<uint32_t>(0, ramAddr9 + i, U8TO32(rom, offset + i));
        }
    }

    if (romFile) {
        loadRomSection(offset7, size7);
        offset = 0;
    }
    else {
        offset = offset7;
    }

    for (uint32_t i = 0; i < size7; i += 4) {
        if (romEncrypted && offset7 + i >= 0x4000 && offset7 + i < 0x4800) {
            if (offset7 + i < 0x4008) {
                core->memory.write<uint32_t>(1, ramAddr7 + i, 0xE7FFDEFF);
            }
            else {
                initKeycode(3);
                uint64_t data = decrypt64(U8TO64(rom, (offset + i) & ~7));
                core->memory.write<uint32_t>(1, ramAddr7 + i, data >> (((offset + i) & 4) ? 32 : 0));
            }
        }
        else {
            core->memory.write<uint32_t>(1, ramAddr7 + i, U8TO32(rom, offset + i));
        }
    }
}

uint64_t CartridgeNds::encrypt64(uint64_t value) {
    uint32_t y = value;
    uint32_t x = value >> 32;

    for (int i = 0x00; i <= 0x0F; i++) {
        uint32_t z = encTable[i] ^ x;
        x = encTable[0x012 + ((z >> 24) & 0xFF)];
        x = encTable[0x112 + ((z >> 16) & 0xFF)] + x;
        x = encTable[0x212 + ((z >> 8) & 0xFF)] ^ x;
        x = encTable[0x312 + ((z >> 0) & 0xFF)] + x;
        x ^= y;
        y = z;
    }

    return ((uint64_t)(y ^ encTable[0x11]) << 32) | (x ^ encTable[0x10]);
}

uint64_t CartridgeNds::decrypt64(uint64_t value) {
    uint32_t y = value;
    uint32_t x = value >> 32;

    for (int i = 0x11; i >= 0x02; i--) {
        uint32_t z = encTable[i] ^ x;
        x = encTable[0x012 + ((z >> 24) & 0xFF)];
        x = encTable[0x112 + ((z >> 16) & 0xFF)] + x;
        x = encTable[0x212 + ((z >> 8) & 0xFF)] ^ x;
        x = encTable[0x312 + ((z >> 0) & 0xFF)] + x;
        x ^= y;
        y = z;
    }

    return ((uint64_t)(y ^ encTable[0x00]) << 32) | (x ^ encTable[0x01]);
}

void CartridgeNds::initKeycode(int level) {
    for (int i = 0; i < 0x412; i++)
        encTable[i] = core->memory.read<uint32_t>(1, 0x30 + i * 4);

    encCode[0] = romCode;
    encCode[1] = romCode / 2;
    encCode[2] = romCode * 2;

    if (level >= 1) applyKeycode();
    if (level >= 2) applyKeycode();

    encCode[1] *= 2;
    encCode[2] /= 2;

    if (level >= 3) applyKeycode();
}

void CartridgeNds::applyKeycode() {
    uint64_t enc1 = encrypt64(((uint64_t)encCode[2] << 32) | encCode[1]);
    encCode[1] = enc1;
    encCode[2] = enc1 >> 32;

    uint64_t enc2 = encrypt64(((uint64_t)encCode[1] << 32) | encCode[0]);
    encCode[0] = enc2;
    encCode[1] = enc2 >> 32;

    for (int i = 0; i <= 0x11; i++) {
        uint32_t byteReverse = 0;
        for (int j = 0; j < 4; j++)
            byteReverse |= ((encCode[i % 2] >> (j * 8)) & 0xFF) << ((3 - j) * 8);

        encTable[i] ^= byteReverse;
    }

    uint64_t scratch = 0;

    for (int i = 0; i <= 0x410; i += 2) {
        scratch = encrypt64(scratch);
        encTable[i + 0] = scratch >> 32;
        encTable[i + 1] = scratch;
    }
}

void CartridgeNds::wordReady(bool cpu) {
    romCtrl[cpu] |= BIT(23);
    core->dma[cpu].trigger((cpu == 0) ? 5 : 2);
}

void CartridgeNds::writeAuxSpiCnt(bool cpu, uint16_t mask, uint16_t value) {
    mask &= 0xE043;
    auxSpiCnt[cpu] = (auxSpiCnt[cpu] & ~mask) | (value & mask);
}

void CartridgeNds::writeRomCmdOutL(bool cpu, uint32_t mask, uint32_t value) {
    romCmdOut[cpu] = (romCmdOut[cpu] & ~((uint64_t)mask)) | (value & mask);
}

void CartridgeNds::writeRomCmdOutH(bool cpu, uint32_t mask, uint32_t value) {
    romCmdOut[cpu] = (romCmdOut[cpu] & ~((uint64_t)mask << 32)) | ((uint64_t)(value & mask) << 32);
}

void CartridgeNds::writeAuxSpiData(bool cpu, uint8_t value) {
    if (saveSize == 0) return;

    if (auxWriteCount[cpu] == 0) {
        if (value == 0) return;
        auxCommand[cpu] = value;
        auxAddress[cpu] = 0;
        auxSpiData[cpu] = 0;
    }
    else {
        if (saveSize == -1) {
            switch (auxCommand[cpu]) {
            case 0x0B:
                LOG_INFO("Detected EEPROM 0.5KB save type\n");
                resizeSave(0x200, false);
                break;

            case 0x02:
                LOG_INFO("Detected EEPROM 64KB save type\n");
                resizeSave(0x10000, false);
                break;

            case 0x0A:
                LOG_INFO("Detected FLASH 512KB save type\n");
                resizeSave(0x80000, false);
                break;

            default:
                if (!(auxSpiCnt[cpu] & BIT(6)))
                    auxWriteCount[cpu] = 0;
                return;
            }
        }

        switch (saveSize) {
        case 0x200:
            switch (auxCommand[cpu]) {
            case 0x03:
                if (auxWriteCount[cpu] < 2) {
                    auxAddress[cpu] = value;
                    auxSpiData[cpu] = 0;
                }
                else {
                    auxSpiData[cpu] = (auxAddress[cpu] < 0x200) ? save[auxAddress[cpu]] : 0;
                    auxAddress[cpu]++;
                }
                break;

            case 0x0B:
                if (auxWriteCount[cpu] < 2) {
                    auxAddress[cpu] = 0x100 + value;
                    auxSpiData[cpu] = 0;
                }
                else {
                    auxSpiData[cpu] = (auxAddress[cpu] < 0x200) ? save[auxAddress[cpu]] : 0;
                    auxAddress[cpu]++;
                }
                break;

            case 0x02:
                if (auxWriteCount[cpu] < 2) {
                    auxAddress[cpu] = value;
                    auxSpiData[cpu] = 0;
                }
                else {
                    if (auxAddress[cpu] < 0x200) {
                        PPCIrqState st = PPCIrqLockByMsr();
                        save[auxAddress[cpu]] = value;
                        saveDirty = true;
                        PPCIrqUnlockByMsr(st);
                    }

                    auxAddress[cpu]++;
                    auxSpiData[cpu] = 0;
                }
                break;

            case 0x0A:
                if (auxWriteCount[cpu] < 2) {
                    auxAddress[cpu] = 0x100 + value;
                    auxSpiData[cpu] = 0;
                }
                else {
                    if (auxAddress[cpu] < 0x200) {
                        PPCIrqState st = PPCIrqLockByMsr();
                        save[auxAddress[cpu]] = value;
                        saveDirty = true;
                        PPCIrqUnlockByMsr(st);
                    }

                    auxAddress[cpu]++;
                    auxSpiData[cpu] = 0;
                }
                break;

            default:
                LOG_CRIT("Write to AUX SPI with unknown EEPROM 0.5KB command: 0x%X\n", auxCommand[cpu]);
                auxSpiData[cpu] = 0;
                break;
            }
            break;

        case 0x2000: case 0x8000: case 0x10000: case 0x20000:
            switch (auxCommand[cpu]) {
            case 0x03:
                if (auxWriteCount[cpu] < ((saveSize == 0x20000) ? 4 : 3)) {
                    auxAddress[cpu] |= value << ((((saveSize == 0x20000) ? 3 : 2) - auxWriteCount[cpu]) * 8);
                    auxSpiData[cpu] = 0;
                }
                else {
                    auxSpiData[cpu] = (auxAddress[cpu] < saveSize) ? save[auxAddress[cpu]] : 0;
                    auxAddress[cpu]++;
                }
                break;

            case 0x02:
                if (auxWriteCount[cpu] < ((saveSize == 0x20000) ? 4 : 3)) {
                    auxAddress[cpu] |= value << ((((saveSize == 0x20000) ? 3 : 2) - auxWriteCount[cpu]) * 8);
                    auxSpiData[cpu] = 0;
                }
                else {
                    if (auxAddress[cpu] < saveSize) {
                        PPCIrqState st = PPCIrqLockByMsr();
                        save[auxAddress[cpu]] = value;
                        saveDirty = true;
                        PPCIrqUnlockByMsr(st);
                    }

                    auxAddress[cpu]++;
                    auxSpiData[cpu] = 0;
                }
                break;

            default:
                LOG_CRIT("Write to AUX SPI with unknown EEPROM/FRAM command: 0x%X\n", auxCommand[cpu]);
                auxSpiData[cpu] = 0;
                break;
            }
            break;

        case 0x40000: case 0x80000: case 0x100000: case 0x800000:
            switch (auxCommand[cpu]) {
            case 0x03:
                if (auxWriteCount[cpu] < 4) {
                    auxAddress[cpu] |= value << ((3 - auxWriteCount[cpu]) * 8);
                    auxSpiData[cpu] = 0;
                }
                else {
                    auxSpiData[cpu] = (auxAddress[cpu] < saveSize) ? save[auxAddress[cpu]] : 0;
                    auxAddress[cpu]++;
                }
                break;

            case 0x0A:
                if (auxWriteCount[cpu] < 4) {
                    auxAddress[cpu] |= value << ((3 - auxWriteCount[cpu]) * 8);
                    auxSpiData[cpu] = 0;
                }
                else {
                    if (auxAddress[cpu] < saveSize) {
                        PPCIrqState st = PPCIrqLockByMsr();
                        save[auxAddress[cpu]] = value;
                        saveDirty = true;
                        PPCIrqUnlockByMsr(st);
                    }

                    auxAddress[cpu]++;
                    auxSpiData[cpu] = 0;
                }
                break;

            case 0x08:
                auxSpiData[cpu] = ((romCode & 0xFF) == 'I') ? 0xAA : 0;
                break;

            default:
                LOG_CRIT("Write to AUX SPI with unknown FLASH command: 0x%X\n", auxCommand[cpu]);
                auxSpiData[cpu] = 0;
                break;
            }
            break;

        default:
            LOG_CRIT("Write to AUX SPI with unknown save size: 0x%X\n", saveSize);
            break;
        }
    }

    if (auxSpiCnt[cpu] & BIT(6))
        auxWriteCount[cpu]++;
    else
        auxWriteCount[cpu] = 0;
}

void CartridgeNds::writeRomCtrl(bool cpu, uint32_t mask, uint32_t value) {
    bool transfer = false;

    romCtrl[cpu] |= (value & BIT(29));

    if (!(romCtrl[cpu] & BIT(31)) && (value & BIT(31)))
        transfer = true;

    mask &= 0xDF7F7FFF;
    romCtrl[cpu] = (romCtrl[cpu] & ~mask) | (value & mask);

    wordCycles[cpu] = (romCtrl[cpu] & BIT(27)) ? (4 * 8) : (4 * 5);

    if (!transfer) return;

    uint8_t size = (romCtrl[cpu] & 0x07000000) >> 24;
    switch (size) {
        case 0: blockSize[cpu] = 0; break;
        case 7: blockSize[cpu] = 4; break;
        default: blockSize[cpu] = 0x100 << size; break;
    }

    uint64_t command = 0;
    for (int i = 0; i < 8; i++)
        command |= ((romCmdOut[cpu] >> (i * 8)) & 0xFF) << ((7 - i) * 8);

    if (encrypted[cpu]) {
        initKeycode(2);
        command = decrypt64(command);
    }

    cmdMode = CMD_NONE;

    if (rom) {
        if (command == 0x0000000000000000) {
            cmdMode = CMD_HEADER;
            if (romFile)
                loadRomSection(0, blockSize[cpu]);
        }
        else if (command == 0x9000000000000000 || (command >> 60) == 0x1 || command == 0xB800000000000000) {
            cmdMode = CMD_CHIP;
        }
        else if ((command >> 56) == 0x3C) {
            encrypted[cpu] = true;
        }
        else if ((command >> 60) == 0x2) {
            cmdMode = CMD_SECURE;
            romAddrReal[cpu] = ((command & 0x0FFFF00000000000) >> 44) * 0x1000;

            if (romFile) {
                loadRomSection(romAddrReal[cpu], blockSize[cpu]);
                romAddrVirt[cpu] = 0;
            }
            else {
                romAddrVirt[cpu] = romAddrReal[cpu];
            }
        }
        else if ((command >> 60) == 0xA) {
            encrypted[cpu] = false;
        }
        else if ((command >> 56) == 0xB7) {
            cmdMode = CMD_DATA;
            romAddrReal[cpu] = (command >> 24) & romMask;

            if (romFile) {
                if (romAddrReal[cpu] < 0x8000) {
                    loadRomSection(0, 0x8200 + blockSize[cpu]);
                    romAddrVirt[cpu] = romAddrReal[cpu];
                }
                else {
                    loadRomSection(romAddrReal[cpu], blockSize[cpu]);
                    romAddrVirt[cpu] = 0;
                }
            }
            else {
                romAddrVirt[cpu] = romAddrReal[cpu];
            }
        }
        else if (command != 0x9F00000000000000) {
            LOG_CRIT("ROM transfer with unknown command: 0x%llX\n", command);
        }
    }

    if (blockSize[cpu] == 0) {
        romCtrl[cpu] &= ~BIT(23);
        romCtrl[cpu] &= ~BIT(31);

        if (auxSpiCnt[cpu] & BIT(14))
            core->interpreter[cpu].sendInterrupt(19);
    }
    else {
        core->schedule(SchedTask(CART9_WORD_READY + cpu), wordCycles[cpu]);
        readCount[cpu] = 0;
    }
}

uint32_t CartridgeNds::readRomDataIn(bool cpu) {
    if (!(romCtrl[cpu] & BIT(23)))
        return 0;

    romCtrl[cpu] &= ~BIT(23);

    if ((readCount[cpu] += 4) == blockSize[cpu]) {
        romCtrl[cpu] &= ~BIT(31);

        if (auxSpiCnt[cpu] & BIT(14))
            core->interpreter[cpu].sendInterrupt(19);
    }
    else {
        core->schedule(SchedTask(CART9_WORD_READY + cpu), wordCycles[cpu]);
    }

    switch (cmdMode) {
    case CMD_HEADER:
        return U8TO32(rom, (readCount[cpu] - 4) & 0xFFF);

    case CMD_CHIP:
        return 0x00001FC2;

    case CMD_SECURE:
        if (!romEncrypted && romAddrReal[cpu] == 0x4000 && readCount[cpu] <= 0x800) {
            uint64_t data = (readCount[cpu] <= 8) ? 0x6A624F7972636E65 :
                U8TO64(rom, (romAddrVirt[cpu] + readCount[cpu] - 4) & ~7);

            initKeycode(3);
            data = encrypt64(data);

            if (readCount[cpu] <= 8) {
                initKeycode(2);
                data = encrypt64(data);
            }

            return data >> (((romAddrReal[cpu] + readCount[cpu]) & 4) ? 0 : 32);
        }

        return U8TO32(rom, romAddrVirt[cpu] + readCount[cpu] - 4);

    case CMD_DATA:
        uint32_t address = romAddrVirt[cpu] + readCount[cpu] - 4;
        if (romAddrReal[cpu] + readCount[cpu] <= 0x8000) address = 0x8000 + (address & 0x1FF);
        if (address < romSize) return U8TO32(rom, address);
    }

    return 0xFFFFFFFF;
}

void CartridgeGba::saveState(FILE *file) {
    fwrite(&saveSize, sizeof(saveSize), 1, file);
    if (saveSize > 0) fwrite(save, 1, saveSize, file);
    fwrite(&eepromCount, sizeof(eepromCount), 1, file);
    fwrite(&eepromCmd, sizeof(eepromCmd), 1, file);
    fwrite(&eepromData, sizeof(eepromData), 1, file);
    fwrite(&eepromDone, sizeof(eepromDone), 1, file);
    fwrite(&flashCmd, sizeof(flashCmd), 1, file);
    fwrite(&bankSwap, sizeof(bankSwap), 1, file);
    fwrite(&flashErase, sizeof(flashErase), 1, file);
}

void CartridgeGba::loadState(FILE *file) {
    fread(&saveSize, sizeof(saveSize), 1, file);
    if (saveSize > 0) fread(save, 1, saveSize, file);
    fread(&eepromCount, sizeof(eepromCount), 1, file);
    fread(&eepromCmd, sizeof(eepromCmd), 1, file);
    fread(&eepromData, sizeof(eepromData), 1, file);
    fread(&eepromDone, sizeof(eepromDone), 1, file);
    fread(&flashCmd, sizeof(flashCmd), 1, file);
    fread(&bankSwap, sizeof(bankSwap), 1, file);
    fread(&flashErase, sizeof(flashErase), 1, file);

    saveDirty = false;
}

bool CartridgeGba::findString(std::string string) {
    for (int i = 0; i < romSize; i += 4) {
        bool found = true;

        for (size_t j = 0; j < string.length(); j++) {
            if (i + j >= romSize || rom[i + j] != string[j]) {
                found = false;
                break;
            }
        }

        if (found)
            return true;
    }

    return false;
}

bool CartridgeGba::loadRom() {
    if (saveSizes.empty()) {
        saveSizes.push_back(0x00000);
        saveSizes.push_back(0x00200);
        saveSizes.push_back(0x02000);
        saveSizes.push_back(0x08000);
        saveSizes.push_back(0x10000);
        saveSizes.push_back(0x20000);
    }

    if (!Cartridge::loadRom()) return false;
    loadRomSection(0, romSize);
    fclose(romFile);
    romFile = nullptr;

    if (romSize > 0xAC && rom[0xAC] == 'F') {
        for (romMask = 1; romMask < romSize; romMask <<= 1);
        romMask--;
    }
    else {
        romMask = 0x1FFFFFF;
    }

    if (findString("SIIRTC_V"))
        core->rtc.enableGpRtc();

    core->memory.updateMap9(0x08000000, 0x0A000000);
    core->memory.updateMap7(0x08000000, 0x0D000000);

    if (saveSize == -1) {
        const std::string saveStrs[] = { "EEPROM_V", "SRAM_V", "FLASH_V", "FLASH512_V", "FLASH1M_V" };

        for (int i = 0; i < 5; i++) {
            if (!findString(saveStrs[i]))
                continue;

            switch (i) {
            case 0:
                return true;

            case 1:
                LOG_INFO("Detected SRAM 32KB save type\n");
                resizeSave(0x8000, false);
                return true;

            case 2: case 3:
                LOG_INFO("Detected FLASH 64KB save type\n");
                resizeSave(0x10000, false);
                return true;

            case 4:
                LOG_INFO("Detected FLASH 128KB save type\n");
                resizeSave(0x20000, false);
                return true;
            }
        }
    }
    return true;
}

uint8_t CartridgeGba::eepromRead() {
    if (saveSize == -1) {
        if (eepromCount == 9) {
            LOG_INFO("Detected EEPROM 0.5KB save type\n");
            resizeSave(0x200, false);
        }
        else {
            LOG_INFO("Detected EEPROM 8KB save type\n");
            resizeSave(0x2000, false);
        }
    }

    uint8_t length = (saveSize == 0x200) ? 8 : 16;

    if (((eepromCmd & 0xC000) >> 14) == 0x3 && eepromCount >= length + 1) {
        if (++eepromCount >= length + 6) {
            int bit = 63 - (eepromCount - (length + 6));
            uint16_t addr = (saveSize == 0x200) ? ((eepromCmd & 0x3F00) >> 8) : (eepromCmd & 0x03FF);
            uint8_t value = (save[addr * 8 + bit / 8] & BIT(bit % 8)) >> (bit % 8);

            if (eepromCount >= length + 69) {
                eepromCount = 0;
                eepromCmd = 0;
                eepromData = 0;
            }

            return value;
        }
    }
    else if (eepromDone) {
        return 1;
    }

    return 0;
}

void CartridgeGba::eepromWrite(uint8_t value) {
    eepromDone = false;

    uint8_t length = (saveSize == 0x200) ? 8 : 16;

    if (eepromCount < length) {
        eepromCmd |= (value & BIT(0)) << (16 - ++eepromCount);
    }
    else if (((eepromCmd & 0xC000) >> 14) == 0x3) {
        if (eepromCount < length + 1)
            eepromCount++;
    }
    else if (((eepromCmd & 0xC000) >> 14) == 0x2) {
        if (++eepromCount <= length + 64)
            eepromData |= (uint64_t)(value & BIT(0)) << (length + 64 - eepromCount);

        if (eepromCount >= length + 65) {
            if (saveSize == -1) {
                LOG_INFO("Detected EEPROM 8KB save type\n");
                resizeSave(0x2000, false);
            }

            PPCIrqState st = PPCIrqLockByMsr();
            uint16_t addr = (saveSize == 0x200) ? ((eepromCmd & 0x3F00) >> 8) : (eepromCmd & 0x03FF);
            for (unsigned int i = 0; i < 8; i++)
                save[addr * 8 + i] = eepromData >> (i * 8);
            saveDirty = true;
            PPCIrqUnlockByMsr(st);

            eepromCount = 0;
            eepromCmd = 0;
            eepromData = 0;
            eepromDone = true;
        }
    }
}

uint8_t CartridgeGba::sramRead(uint32_t address) {
    if (saveSize == 0x8000 && address < 0xE008000) {
        return save[address - 0xE000000];
    }
    else if ((saveSize == 0x10000 || saveSize == 0x20000) && address < 0xE010000) {
        if (flashCmd == 0x90 && address == 0xE000000) {
            return 0xC2;
        }
        else if (flashCmd == 0x90 && address == 0xE000001) {
            return (saveSize == 0x10000) ? 0x1C : 0x09;
        }
        else {
            if (bankSwap) address += 0x10000;
            return save[address - 0xE000000];
        }
    }

    return 0xFF;
}

void CartridgeGba::sramWrite(uint32_t address, uint8_t value) {
    if (saveSize == 0x8000 && address < 0xE008000) {
        PPCIrqState st = PPCIrqLockByMsr();
        save[address - 0xE000000] = value;
        saveDirty = true;
        PPCIrqUnlockByMsr(st);
    }
    else if ((saveSize == 0x10000 || saveSize == 0x20000) && address < 0xE010000) {
        if (flashCmd == 0xA0) {
            if (bankSwap) address += 0x10000;
            PPCIrqState st = PPCIrqLockByMsr();
            save[address - 0xE000000] = value;
            saveDirty = true;
            PPCIrqUnlockByMsr(st);
            flashCmd = 0xF0;
        }
        else if (flashErase && (address & ~0x000F000) == 0xE000000 && (value & 0xFF) == 0x30) {
            if (bankSwap) address += 0x10000;
            PPCIrqState st = PPCIrqLockByMsr();
            memset(&save[address - 0xE000000], 0xFF, 0x1000 * sizeof(uint8_t));
            saveDirty = true;
            PPCIrqUnlockByMsr(st);
            flashErase = false;
        }
        else if (saveSize == 0x20000 && flashCmd == 0xB0 && address == 0xE000000) {
            bankSwap = value;
            flashCmd = 0xF0;
        }
        else if (address == 0xE005555) {
            flashCmd = value;

            if (flashCmd == 0x80) {
                flashErase = true;
            }
            else if (flashCmd != 0xAA) {
                flashErase = false;
            }
            else if (flashErase && flashCmd == 0x10) {
                PPCIrqState st = PPCIrqLockByMsr();
                memset(save, 0xFF, saveSize * sizeof(uint8_t));
                saveDirty = true;
                PPCIrqUnlockByMsr(st);
            }
        }
    }
}
