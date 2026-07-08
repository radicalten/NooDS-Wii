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

void ActionReplay::setPath(std::string path) {
    this->path = path;
}

void ActionReplay::setFd(int fd) {
    this->fd = fd;
}

FILE *ActionReplay::openFile(const char *mode) {
    if (fd != -1)
        return fdopen(dup(fd), mode);
    else if (path != "")
        return fopen(path.c_str(), mode);
    return nullptr;
}

bool ActionReplay::loadCheats() {
    FILE *file = openFile("r");
    if (!file) return false;
    char data[512];

    PPCIrqState st = PPCIrqLockByMsr();

    cheats.clear();
    while (fgets(data, 512, file) != nullptr) {
        if (data[0] != '[') continue;
        cheats.push_back(ARCheat());
        ARCheat &cheat = cheats[cheats.size() - 1];

        cheat.name = &data[1];
        cheat.enabled = (cheat.name[cheat.name.size() - 2] == '+');
        cheat.name = cheat.name.substr(0, cheat.name.size() - 3);
        LOG_INFO("Loaded cheat: %s (%s)\n", cheat.name.c_str(), cheat.enabled ? "enabled" : "disabled");

        while (fgets(data, 512, file) != nullptr && data[0] != '\n') {
            cheat.code.push_back(strtoll(&data[0], nullptr, 16));
            cheat.code.push_back(strtoll(&data[8], nullptr, 16));
        }
    }

    PPCIrqUnlockByMsr(st);
    fclose(file);
    return true;
}

bool ActionReplay::saveCheats() {
    FILE *file = openFile("w");
    if (!file) return false;

    PPCIrqState st = PPCIrqLockByMsr();

    for (uint32_t i = 0; i < cheats.size(); i++) {
        fprintf(file, "[%s]%c\n", cheats[i].name.c_str(), cheats[i].enabled ? '+' : '-');
        for (uint32_t j = 0; j < cheats[i].code.size(); j += 2)
            fprintf(file, "%08X %08X\n", cheats[i].code[j], cheats[i].code[j + 1]);
        fprintf(file, "\n");
    }

    PPCIrqUnlockByMsr(st);
    fclose(file);
    return true;
}

void ActionReplay::applyCheats() {
    PPCIrqState st = PPCIrqLockByMsr();
    for (uint32_t i = 0; i < cheats.size(); i++) {
        if (!cheats[i].enabled) continue;
        uint32_t offset = 0;
        uint32_t dataReg = 0;
        uint32_t counter = 0;
        uint32_t loopCount = 0;
        uint32_t loopAddress = 0;
        bool condFlag = false;

        for (uint32_t address = 0; address < cheats[i].code.size(); address += 2) {
            uint32_t *line = &cheats[i].code[address];
            if (condFlag) {
                uint8_t op = (line[0] >> 24);
                if ((op >> 4) == 0xE)
                    address += ((line[1] + 0x7) & ~0x7) >> 2;
                else if (op == 0xC5)
                    counter++;

                if (op != 0xD0 && op != 0xD1 && op != 0xD2)
                    continue;
            }

            switch (line[0] >> 28) {
            case 0x0:
                core->memory.write<uint32_t>(1, (line[0] & 0xFFFFFFF) + offset, line[1]);
                continue;

            case 0x1:
                core->memory.write<uint16_t>(1, (line[0] & 0xFFFFFFF) + offset, line[1]);
                continue;

            case 0x2:
                core->memory.write<uint8_t>(1, (line[0] & 0xFFFFFFF) + offset, line[1]);
                continue;

            case 0x3: {
                uint32_t addr = (line[0] & 0xFFFFFFF) ? (line[0] & 0xFFFFFFF) : offset;
                condFlag = (line[1] <= core->memory.read<uint32_t>(1, addr));
                continue;
            }

            case 0x4: {
                uint32_t addr = (line[0] & 0xFFFFFFF) ? (line[0] & 0xFFFFFFF) : offset;
                condFlag = (line[1] >= core->memory.read<uint32_t>(1, addr));
                continue;
            }

            case 0x5: {
                uint32_t addr = (line[0] & 0xFFFFFFF) ? (line[0] & 0xFFFFFFF) : offset;
                condFlag = (line[1] != core->memory.read<uint32_t>(1, addr));
                continue;
            }

            case 0x6: {
                uint32_t addr = (line[0] & 0xFFFFFFF) ? (line[0] & 0xFFFFFFF) : offset;
                condFlag = (line[1] == core->memory.read<uint32_t>(1, addr));
                continue;
            }

            case 0x7: {
                uint32_t addr = (line[0] & 0xFFFFFFF) ? (line[0] & 0xFFFFFFF) : offset;
                condFlag = (line[1] & 0xFFFF) <= (core->memory.read<uint16_t>(1, addr) & ~(line[1] >> 16));
                continue;
            }

            case 0x8: {
                uint32_t addr = (line[0] & 0xFFFFFFF) ? (line[0] & 0xFFFFFFF) : offset;
                condFlag = (line[1] & 0xFFFF) >= (core->memory.read<uint16_t>(1, addr) & ~(line[1] >> 16));
                continue;
            }

            case 0x9: {
                uint32_t addr = (line[0] & 0xFFFFFFF) ? (line[0] & 0xFFFFFFF) : offset;
                condFlag = (line[1] & 0xFFFF) != (core->memory.read<uint16_t>(1, addr) & ~(line[1] >> 16));
                continue;
            }

            case 0xA: {
                uint32_t addr = (line[0] & 0xFFFFFFF) ? (line[0] & 0xFFFFFFF) : offset;
                condFlag = (line[1] & 0xFFFF) == (core->memory.read<uint16_t>(1, addr) & ~(line[1] >> 16));
                continue;
            }

            case 0xB:
                offset = core->memory.read<uint32_t>(1, (line[0] & 0xFFFFFFF) + offset);
                continue;

            case 0xC:
                switch (line[0] >> 24) {
                case 0xC0:
                    loopCount = line[1];
                    loopAddress = address;
                    continue;

                case 0xC5:
                    condFlag = (++counter & line[1] & 0xFFFF) != (line[1] >> 16);
                    continue;

                case 0xC6:
                    core->memory.write<uint32_t>(1, line[1], offset);
                    continue;
                }
                goto invalid;

            case 0xD:
                switch (line[0] >> 24) {
                case 0xD0:
                    condFlag = false;
                    continue;

                case 0xD1:
                    if (loopCount) {
                        loopCount--;
                        address = loopAddress;
                        continue;
                    }
                    condFlag = false;
                    continue;

                case 0xD2:
                    if (loopCount) {
                        loopCount--;
                        address = loopAddress;
                        continue;
                    }
                    offset = 0;
                    dataReg = 0;
                    condFlag = false;
                    continue;

                case 0xD3:
                    offset = line[1];
                    continue;

                case 0xD4:
                    dataReg += line[1];
                    continue;

                case 0xD5:
                    dataReg = line[1];
                    continue;

                case 0xD6:
                    core->memory.write<uint32_t>(1, line[1] + offset, dataReg);
                    offset += 4;
                    continue;

                case 0xD7:
                    core->memory.write<uint16_t>(1, line[1] + offset, dataReg);
                    offset += 2;
                    continue;

                case 0xD8:
                    core->memory.write<uint8_t>(1, line[1] + offset, dataReg);
                    offset += 1;
                    continue;

                case 0xD9:
                    dataReg = core->memory.read<uint32_t>(1, line[1] + offset);
                    continue;

                case 0xDA:
                    dataReg = core->memory.read<uint16_t>(1, line[1] + offset);
                    continue;

                case 0xDB:
                    dataReg = core->memory.read<uint8_t>(1, line[1] + offset);
                    continue;

                case 0xDC:
                    offset += line[1];
                    continue;
                }
                goto invalid;

            case 0xE:
                for (uint32_t j = 0; j < line[1]; j++) {
                    uint8_t value = line[(j >> 2) + 2] >> ((j & 0x3) * 8);
                    core->memory.write<uint8_t>(1, (line[0] & 0xFFFFFFF) + offset + j, value);
                }
                address += ((line[1] + 0x7) & ~0x7) >> 2;
                continue;

            case 0xF:
                for (uint32_t j = 0; j < line[1]; j++) {
                    uint8_t value = core->memory.read<uint8_t>(1, offset + j);
                    core->memory.write<uint8_t>(1, (line[0] & 0xFFFFFFF) + j, value);
                }
                continue;

            default:
            invalid:
                LOG_CRIT("Invalid AR code: %08X %08X\n", line[0], line[1]);
                continue;
            }
        }
    }
    PPCIrqUnlockByMsr(st);
}
