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
    #include <tuxedo/thread.h>
    #include <tuxedo/ppc/intrinsics.h>
    #include <tuxedo/ppc/clock.h>
}

#include "core.h"

extern void* Noods_MEM2_Alloc(size_t size);
extern void  Noods_MEM2_Free(void* ptr);

// ---------------------------------------------------------------------------
// Custom allocators — route to MEM2
// ---------------------------------------------------------------------------
void* Core::operator new(size_t size) {
    void* p = Noods_MEM2_Alloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void Core::operator delete(void* p) noexcept { Noods_MEM2_Free(p); }

void* Core::operator new[](size_t size) {
    void* p = Noods_MEM2_Alloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void Core::operator delete[](void* p) noexcept { Noods_MEM2_Free(p); }

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
Core::Core(std::string ndsRom, std::string gbaRom, int id,
           int ndsRomFd,  int gbaRomFd,
           int ndsSaveFd, int gbaSaveFd,
           int ndsStateFd, int gbaStateFd, int ndsCheatFd)
    : id(id),
      actionReplay(this), cartridgeGba(this), cartridgeNds(this),
      cp15(this), divSqrt(this), dldi(this),
      dma { Dma(this, 0), Dma(this, 1) },
      gpu(this),
      gpu2D { Gpu2D(this, 0), Gpu2D(this, 1) },
      gpu3D(this), gpu3DRenderer(this),
      hleArm7(this),
      hleBios { HleBios(this, 0, HleBios::swiTable9),
                HleBios(this, 1, HleBios::swiTable7),
                HleBios(this, 1, HleBios::swiTableGba) },
      input(this),
      interpreter { Interpreter(this, 0), Interpreter(this, 1) },
      ipc(this), memory(this), rtc(this), saveStates(this),
      spi(this), spu(this),
      timers { Timers(this, 0), Timers(this, 1) },
      wifi(this),
      running(0),
      lastFpsTimeTicks(0),
      fpsCount(0)
{
    // ------------------------------------------------------------------
    // Bind task table.
    //
    // Layout: SchedCallback(trampoline, receiver_ptr, int_arg)
    // Tasks that take no integer argument pass 0; the compiler will
    // constant-fold the unused parameter in release builds.
    // ------------------------------------------------------------------

    // Core self-tasks
    tasks[UPDATE_RUN]       = SchedCallback(shim_updateRun,   this);
    tasks[RESET_CYCLES]     = SchedCallback(shim_resetCycles, this);

    // Cartridge — arg selects ARM9 (0) or ARM7 (1)
    tasks[CART9_WORD_READY] = SchedCallback(shim_cartWordReady, &cartridgeNds, 0);
    tasks[CART7_WORD_READY] = SchedCallback(shim_cartWordReady, &cartridgeNds, 1);

    // DMA — ARM9 channels 0-3
    tasks[DMA9_TRANSFER0]   = SchedCallback(shim_dmaTransfer, &dma[0], 0);
    tasks[DMA9_TRANSFER1]   = SchedCallback(shim_dmaTransfer, &dma[0], 1);
    tasks[DMA9_TRANSFER2]   = SchedCallback(shim_dmaTransfer, &dma[0], 2);
    tasks[DMA9_TRANSFER3]   = SchedCallback(shim_dmaTransfer, &dma[0], 3);

    // DMA — ARM7 channels 0-3
    tasks[DMA7_TRANSFER0]   = SchedCallback(shim_dmaTransfer, &dma[1], 0);
    tasks[DMA7_TRANSFER1]   = SchedCallback(shim_dmaTransfer, &dma[1], 1);
    tasks[DMA7_TRANSFER2]   = SchedCallback(shim_dmaTransfer, &dma[1], 2);
    tasks[DMA7_TRANSFER3]   = SchedCallback(shim_dmaTransfer, &dma[1], 3);

    // GPU scanlines
    tasks[NDS_SCANLINE256]  = SchedCallback(shim_gpuScanline256,   &gpu);
    tasks[NDS_SCANLINE355]  = SchedCallback(shim_gpuScanline355,   &gpu);
    tasks[GBA_SCANLINE240]  = SchedCallback(shim_gpuGbaScanline240, &gpu);
    tasks[GBA_SCANLINE308]  = SchedCallback(shim_gpuGbaScanline308, &gpu);

    // GPU 3D command FIFO
    tasks[GPU3D_COMMANDS]   = SchedCallback(shim_gpu3dCommands, &gpu3D);

    // CPU interrupts
    tasks[ARM9_INTERRUPT]   = SchedCallback(shim_interpreterInterrupt, &interpreter[0]);
    tasks[ARM7_INTERRUPT]   = SchedCallback(shim_interpreterInterrupt, &interpreter[1]);

    // SPU
    tasks[NDS_SPU_SAMPLE]   = SchedCallback(shim_spuSample,    &spu);
    tasks[GBA_SPU_SAMPLE]   = SchedCallback(shim_spuGbaSample, &spu);

    // Timers — ARM9 channels 0-3
    tasks[TIMER9_OVERFLOW0] = SchedCallback(shim_timersOverflow, &timers[0], 0);
    tasks[TIMER9_OVERFLOW1] = SchedCallback(shim_timersOverflow, &timers[0], 1);
    tasks[TIMER9_OVERFLOW2] = SchedCallback(shim_timersOverflow, &timers[0], 2);
    tasks[TIMER9_OVERFLOW3] = SchedCallback(shim_timersOverflow, &timers[0], 3);

    // Timers — ARM7 channels 0-3
    tasks[TIMER7_OVERFLOW0] = SchedCallback(shim_timersOverflow, &timers[1], 0);
    tasks[TIMER7_OVERFLOW1] = SchedCallback(shim_timersOverflow, &timers[1], 1);
    tasks[TIMER7_OVERFLOW2] = SchedCallback(shim_timersOverflow, &timers[1], 2);
    tasks[TIMER7_OVERFLOW3] = SchedCallback(shim_timersOverflow, &timers[1], 3);

    // WiFi — CMD_REPLY / CMD_ACK are integer constants from wifi.h
    tasks[WIFI_COUNT_MS]    = SchedCallback(shim_wifiCountMs, &wifi);
    tasks[WIFI_TRANS_REPLY] = SchedCallback(shim_wifiTransmit, &wifi, CMD_REPLY);
    tasks[WIFI_TRANS_ACK]   = SchedCallback(shim_wifiTransmit, &wifi, CMD_ACK);

    // ------------------------------------------------------------------
    // BIOS / firmware loading
    // ------------------------------------------------------------------
    bool required = !Settings::directBoot ||
                    (ndsRom == "" && gbaRom == "" &&
                     ndsRomFd == -1 && gbaRomFd == -1);

    if (!memory.loadBios9() && required) throw ERROR_BIOS;
    if (!memory.loadBios7() && required) throw ERROR_BIOS;
    if (!spi.loadFirmware()  && required) throw ERROR_FIRM;
    realGbaBios = memory.loadGbaBios();

    // ------------------------------------------------------------------
    // Initial scheduling
    // ------------------------------------------------------------------
    schedule(RESET_CYCLES,    0x7FFFFFFF);
    schedule(NDS_SCANLINE256, 256 * 6);
    schedule(NDS_SCANLINE355, 355 * 6);
    schedule(NDS_SPU_SAMPLE,  512 * 2);

    dsiMode = Settings::dsiMode;
    updateRun();

    memory.updateMap9(0x00000000, 0xFFFFFFFF);
    memory.updateMap7(0x00000000, 0xFFFFFFFF);
    interpreter[0].init();
    interpreter[1].init();

    // ------------------------------------------------------------------
    // GBA ROM
    // ------------------------------------------------------------------
    if (gbaRom != "" || gbaRomFd != -1) {
        if (!cartridgeGba.setRom(gbaRom, gbaRomFd, gbaSaveFd, gbaStateFd, -1))
            throw ERROR_ROM;

        if (Settings::directBoot && ndsRom == "" && ndsRomFd == -1) {
            memory.write<uint16_t>(0, 0x4000304, 0x8003);
            enterGbaMode();
        }
    }

    // ------------------------------------------------------------------
    // NDS ROM
    // ------------------------------------------------------------------
    if (ndsRom != "" || ndsRomFd != -1) {
        if (!cartridgeNds.setRom(ndsRom, ndsRomFd, ndsSaveFd, ndsStateFd, ndsCheatFd))
            throw ERROR_ROM;

        actionReplay.loadCheats();

        if (Settings::directBoot) {
            cp15.write(1, 0, 0, 0x0005707D);
            cp15.write(9, 1, 0, 0x0300000A);
            cp15.write(9, 1, 1, 0x00000020);
            memory.write<uint8_t> (0,  0x4000247, 0x03);
            memory.write<uint8_t> (0,  0x4000300, 0x01);
            memory.write<uint8_t> (1,  0x4000300, 0x01);
            memory.write<uint16_t>(0,  0x4000304, 0x0001);
            memory.write<uint16_t>(1,  0x4000504, 0x0200);
            memory.write<uint32_t>(0,  0x27FF800, 0x00001FC2);
            memory.write<uint32_t>(0,  0x27FF804, 0x00001FC2);
            memory.write<uint16_t>(0,  0x27FF850, 0x5835);
            memory.write<uint16_t>(0,  0x27FF880, 0x0007);
            memory.write<uint16_t>(0,  0x27FF884, 0x0006);
            memory.write<uint32_t>(0,  0x27FFC00, 0x00001FC2);
            memory.write<uint32_t>(0,  0x27FFC04, 0x00001FC2);
            memory.write<uint16_t>(0,  0x27FFC10, 0x5835);
            memory.write<uint16_t>(0,  0x27FFC40, 0x0001);
            cartridgeNds.directBoot();
            interpreter[0].directBoot();
            interpreter[1].directBoot();
            spi.directBoot();
        }
    }

    // ------------------------------------------------------------------
    // HLE ARM7
    // ------------------------------------------------------------------
    if (!gbaMode && Settings::arm7Hle) {
        arm7Hle = true;
        hleArm7.init();
    }

    lastFpsTimeTicks = PPCGetTickCount();

    PPCIrqState st = PPCIrqLockByMsr();
    running = 1;
    PPCIrqUnlockByMsr(st);
}

// ---------------------------------------------------------------------------
// State save / load
// ---------------------------------------------------------------------------
void Core::saveState(FILE* file) {
    fwrite(&arm7Hle,      sizeof(arm7Hle),      1, file);
    fwrite(&dsiMode,      sizeof(dsiMode),      1, file);
    fwrite(&gbaMode,      sizeof(gbaMode),      1, file);
    fwrite(&globalCycles, sizeof(globalCycles), 1, file);

    uint32_t count = static_cast<uint32_t>(events.size());
    fwrite(&count, sizeof(count), 1, file);
    for (uint32_t i = 0; i < count; i++)
        fwrite(&events[i], sizeof(events[i]), 1, file);
}

void Core::loadState(FILE* file) {
    fread(&arm7Hle,      sizeof(arm7Hle),      1, file);
    fread(&dsiMode,      sizeof(dsiMode),      1, file);
    fread(&gbaMode,      sizeof(gbaMode),      1, file);
    fread(&globalCycles, sizeof(globalCycles), 1, file);

    events.clear();
    uint32_t count = 0;
    fread(&count, sizeof(count), 1, file);
    SchedEvent event(MAX_TASKS, 0);
    for (uint32_t i = 0; i < count; i++) {
        fread(&event, sizeof(event), 1, file);
        events.push_back(event);
    }

    updateRun();
}

// ---------------------------------------------------------------------------
// updateRun — select the tightest interpreter loop for the current state
// ---------------------------------------------------------------------------
void Core::updateRun() {
    if (interpreter[0].halted && interpreter[1].halted)
        runFunc = &Interpreter::runCoreNone;
    else if (gbaMode)
        runFunc = &Interpreter::runCoreSingle<true, 0>;
    else if (dsiMode)
        runFunc = &Interpreter::runCoreDsi;
    else if (!interpreter[0].halted && !interpreter[1].halted)
        runFunc = &Interpreter::runCoreNds;
    else if (interpreter[0].halted)
        runFunc = &Interpreter::runCoreSingle<true, 1>;
    else
        runFunc = &Interpreter::runCoreSingle<false, 0>;

    PPCIrqState st = PPCIrqLockByMsr();
    running = 0;
    PPCIrqUnlockByMsr(st);
}

// ---------------------------------------------------------------------------
// resetCycles — prevent globalCycles overflow every ~2 billion ticks
// ---------------------------------------------------------------------------
void Core::resetCycles() {
    const size_t n = events.size();
    for (size_t i = 0; i < n; i++)
        events[i].cycles -= globalCycles;

    for (int i = 0; i < 2; i++) {
        interpreter[i].resetCycles();
        timers[i].resetCycles();
    }

    globalCycles = 0;
    schedule(RESET_CYCLES, 0x7FFFFFFF);
}

// ---------------------------------------------------------------------------
// schedule — insert a new event in sorted order (binary search insertion)
// ---------------------------------------------------------------------------
void Core::schedule(SchedTask task, uint32_t cycles) {
    SchedEvent event(task, globalCycles + cycles);
    auto it = std::upper_bound(events.cbegin(), events.cend(), event);
    events.insert(it, event);
}

// ---------------------------------------------------------------------------
// enterGbaMode
// ---------------------------------------------------------------------------
void Core::enterGbaMode() {
    gbaMode = true;

    interpreter[0].halt(2);
    interpreter[1].unhalt(2);
    updateRun();

    events.clear();
    schedule(RESET_CYCLES,    0x7FFFFFFF);
    schedule(GBA_SCANLINE240, 240 * 4);
    schedule(GBA_SCANLINE308, 308 * 4);
    schedule(GBA_SPU_SAMPLE,  512);

    memory.updateMap7(0x00000000, 0xFFFFFFFF);
    interpreter[1].init();
    rtc.reset();

    memory.write<uint8_t>(0, 0x4000240, 0x80);
    memory.write<uint8_t>(0, 0x4000241, 0x80);

    if (realGbaBios) {
        interpreter[1].bios = nullptr;
        return;
    }

    interpreter[1].bios = &hleBios[2];
    interpreter[1].directBoot();
    memory.write<uint16_t>(1, 0x4000088, 0x200);
}

// ---------------------------------------------------------------------------
// endFrame
// ---------------------------------------------------------------------------
void Core::endFrame() {
    PPCIrqState st = PPCIrqLockByMsr();
    running = 0;
    PPCIrqUnlockByMsr(st);

    fpsCount++;

    if (arm7Hle)
        hleArm7.runFrame();

    uint64_t now     = PPCGetTickCount();
    uint64_t elapsed = now - lastFpsTimeTicks;
    if (elapsed >= PPCMsToTicks(1000)) {
        fps              = fpsCount;
        fpsCount         = 0;
        lastFpsTimeTicks = now;
    }

    if (wifi.shouldSchedule())
        wifi.scheduleInit();
}
