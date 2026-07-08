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

#include <algorithm>
#include <cstring>

extern "C" {
    #include <tuxedo/ppc/intrinsics.h>
}

#include "core.h"

Language Spi::language = LG_ENGLISH;

Spi::~Spi() {
    if (firmware)  delete[] firmware;
    if (micBuffer) delete[] micBuffer;
}

void Spi::saveState(FILE *file) {
    fwrite(&writeCount, sizeof(writeCount), 1, file);
    fwrite(&address,    sizeof(address),    1, file);
    fwrite(&command,    sizeof(command),    1, file);
    fwrite(&spiCnt,     sizeof(spiCnt),     1, file);
    fwrite(&spiData,    sizeof(spiData),    1, file);
}

void Spi::loadState(FILE *file) {
    fread(&writeCount, sizeof(writeCount), 1, file);
    fread(&address,    sizeof(address),    1, file);
    fread(&command,    sizeof(command),    1, file);
    fread(&spiCnt,     sizeof(spiCnt),     1, file);
    fread(&spiData,    sizeof(spiData),    1, file);
}

uint16_t Spi::crc16(uint32_t value, uint8_t *data, size_t size) {
    static const uint16_t table[] = {
        0xC0C1, 0xC181, 0xC301, 0xC601,
        0xCC01, 0xD801, 0xF001, 0xA001
    };

    for (size_t i = 0; i < size; i++) {
        value ^= data[i];
        for (size_t j = 0; j < 8; j++)
            value = (value >> 1) ^ ((value & 1) ? (table[j] << (7 - j)) : 0);
    }

    return value;
}

bool Spi::loadFirmware() {
    if (firmware)
        delete[] firmware;

    if (FILE *file = fopen(Settings::firmwarePath.c_str(), "rb")) {
        fseek(file, 0, SEEK_END);
        firmSize = ftell(file);
        fseek(file, 0, SEEK_SET);
        firmware = new uint8_t[firmSize];
        fread(firmware, sizeof(uint8_t), firmSize, file);
        fclose(file);

        if (core->id > 0) {
            firmware[0x3B] += core->id;
            uint16_t crc = crc16(0, &firmware[0x2C], 0x138);
            firmware[0x2A] = crc >> 0;
            firmware[0x2B] = crc >> 8;
        }

        return (firmSize > 0x20000);
    }

    // Generate a minimal stub firmware when no file is present.
    firmSize = 0x20000;
    firmware = new uint8_t[firmSize];
    memset(firmware, 0, firmSize);

    firmware[0x20] = 0xC0;
    firmware[0x21] = 0x3F;

    firmware[0x2C] = 0x38;
    firmware[0x2D] = 0x01;
    firmware[0x36] = 0x00;
    firmware[0x37] = 0x09;
    firmware[0x38] = 0xBF;
    firmware[0x39] = 0x12;
    firmware[0x3A] = 0x34;
    firmware[0x3B] = core->id;
    firmware[0x3C] = 0xFE;
    firmware[0x3D] = 0x3F;

    uint16_t crc = crc16(0, &firmware[0x2C], 0x138);
    firmware[0x2A] = crc >> 0;
    firmware[0x2B] = crc >> 8;

    for (uint32_t addr = 0x1FA00; addr <= 0x1FC00; addr += 0x100) {
        firmware[addr + 0xE7] = 0xFF;
        firmware[addr + 0xF5] = 0x28;

        crc = crc16(0, &firmware[addr], 0xFE);
        firmware[addr + 0xFE] = crc >> 0;
        firmware[addr + 0xFF] = crc >> 8;
    }

    for (uint32_t addr = 0x1FE00; addr <= 0x1FF00; addr += 0x100) {
        firmware[addr + 0x00] = 5;
        firmware[addr + 0x02] = 2;
        firmware[addr + 0x03] = 5;
        firmware[addr + 0x04] = 25;
        firmware[addr + 0x06] = 'N';
        firmware[addr + 0x08] = 'o';
        firmware[addr + 0x0A] = 'o';
        firmware[addr + 0x0C] = 'D';
        firmware[addr + 0x0E] = 'S';
        firmware[addr + 0x1A] = 5;

        firmware[addr + 0x5E] = 0xF0;
        firmware[addr + 0x5F] = 0x0F;
        firmware[addr + 0x60] = 0xF0;
        firmware[addr + 0x61] = 0x0B;
        firmware[addr + 0x62] = 0xFF;
        firmware[addr + 0x63] = 0xBF;

        firmware[addr + 0x64] = language;

        crc = crc16(0xFFFF, &firmware[addr], 0x70);
        firmware[addr + 0x72] = crc >> 0;
        firmware[addr + 0x73] = crc >> 8;
    }

    return false;
}

void Spi::directBoot() {
    uint32_t address = 0x27FFC80 + (Settings::dsiMode << 23);
    for (uint32_t i = 0; i < 0x70; i++)
        core->memory.write<uint8_t>(0, address + i, firmware[firmSize - 0x100 + i]);
}

void Spi::setTouch(int x, int y) {
    if (!firmware)
        return;

    uint16_t adcX1 = U8TO16(firmware, firmSize - 0xA8);
    uint16_t adcY1 = U8TO16(firmware, firmSize - 0xA6);
    uint8_t  scrX1 = firmware[firmSize - 0xA4];
    uint8_t  scrY1 = firmware[firmSize - 0xA3];
    uint16_t adcX2 = U8TO16(firmware, firmSize - 0xA2);
    uint16_t adcY2 = U8TO16(firmware, firmSize - 0xA0);
    uint8_t  scrX2 = firmware[firmSize - 0x9E];
    uint8_t  scrY2 = firmware[firmSize - 0x9D];

    if (x < 1)   x = 1;   else if (x > 254) x = 254;
    if (y < 1)   y = 1;   else if (y > 190) y = 190;

    if (scrX2 - scrX1 != 0)
        touchX = (x - (scrX1 - 1)) * (adcX2 - adcX1) / (scrX2 - scrX1) + adcX1;
    if (scrY2 - scrY1 != 0)
        touchY = (y - (scrY1 - 1)) * (adcY2 - adcY1) / (scrY2 - scrY1) + adcY1;
}

void Spi::clearTouch() {
    touchX = 0x000;
    touchY = 0xFFF;
}

void Spi::sendMicData(const int16_t *samples, size_t count, size_t rate) {
    // IRQ lock protects micBuffer, micBufSize, micCycles, micStep
    // against concurrent reads in writeSpiData (mic channel).
    PPCIrqState st = PPCIrqLockByMsr();

    if (micBuffer) delete[] micBuffer;
    micBuffer  = new int16_t[count];
    memcpy(micBuffer, samples, count * sizeof(int16_t));
    micBufSize = count;
    micCycles  = core->globalCycles;
    micStep    = (60 * 263 * 355 * 6) / rate;

    PPCIrqUnlockByMsr(st);
}

void Spi::writeSpiCnt(uint16_t mask, uint16_t value) {
    mask  &= 0xCF03;
    spiCnt = (spiCnt & ~mask) | (value & mask);
}

void Spi::writeSpiData(uint8_t value) {
    if (!(spiCnt & BIT(15))) {
        spiData = 0;
        return;
    }

    if (writeCount == 0) {
        command = value;
        address = 0;
        spiData = 0;
    } else {
        switch ((spiCnt & 0x0300) >> 8) {
        case 1: // Firmware
            switch (command) {
            case 0x03: // Read
                if (writeCount < 4) {
                    address |= value << ((3 - writeCount) * 8);
                } else {
                    spiData  = (address < firmSize) ? firmware[address] : 0;
                    address += (spiCnt & BIT(10)) ? 2 : 1;
                }
                break;

            default:
                LOG_CRIT("Write to SPI with unknown firmware command: 0x%X\n", command);
                spiData = 0;
                break;
            }
            break;

        case 2: // Touchscreen / mic (TSC2046 / ADS7846 compatible)
            switch ((command & 0x70) >> 4) {
            case 1: // Y position
                spiData = (writeCount & 1) ? (touchY >> 5) : (touchY << 3);
                break;

            case 5: // X position
                spiData = (writeCount & 1) ? (touchX >> 5) : (touchX << 3);
                break;

            case 6: // Microphone
                if (writeCount & 1) {
                    // IRQ lock: read micBuffer/micBufSize/micCycles/micStep
                    // atomically with sendMicData writes.
                    PPCIrqState st = PPCIrqLockByMsr();
                    size_t index = std::min<size_t>(
                        (core->globalCycles - micCycles) / micStep,
                        micBufSize);
                    micSample = (micBufSize > 0)
                        ? ((micBuffer[index] >> 4) + 0x800)
                        : 0;
                    PPCIrqUnlockByMsr(st);

                    spiData = micSample >> 5;
                    break;
                }
                spiData = micSample << 3;
                break;

            default:
                LOG_WARN("Write to SPI with unknown touchscreen channel: %d\n",
                         (command & 0x70) >> 4);
                spiData = 0;
                break;
            }
            break;

        default:
            LOG_CRIT("Write to SPI with unknown device: %d\n",
                     (spiCnt & 0x0300) >> 8);
            spiData = 0;
            break;
        }
    }

    if (spiCnt & BIT(11))
        writeCount++;
    else
        writeCount = 0;

    if (spiCnt & BIT(14))
        core->interpreter[1].sendInterrupt(23);
}
