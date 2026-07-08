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
#include <functional>
#include <string>
#include <vector>

extern "C" {
    #include <tuxedo/thread.h>
    #include <tuxedo/tick.h>
    #include <tuxedo/ppc/intrinsics.h>
    #include <tuxedo/ppc/clock.h>
}

#include "action_replay.h"
#include "cartridge.h"
#include "cp15.h"
#include "defines.h"
#include "div_sqrt.h"
#include "dldi.h"
#include "dma.h"
#include "gpu.h"
#include "gpu_2d.h"
#include "gpu_3d.h"
#include "gpu_3d_renderer.h"
#include "hle_arm7.h"
#include "hle_bios.h"
#include "input.h"
#include "interpreter.h"
#include "ipc.h"
#include "memory.h"
#include "rtc.h"
#include "save_states.h"
#include "settings.h"
#include "spi.h"
#include "spu.h"
#include "timers.h"
#include "wifi.h"

enum CoreError {
    ERROR_BIOS,
    ERROR_FIRM,
    ERROR_ROM
};

enum SchedTask {
    UPDATE_RUN,
    RESET_CYCLES,
    CART9_WORD_READY,
    CART7_WORD_READY,
    DMA9_TRANSFER0,
    DMA9_TRANSFER1,
    DMA9_TRANSFER2,
    DMA9_TRANSFER3,
    DMA7_TRANSFER0,
    DMA7_TRANSFER1,
    DMA7_TRANSFER2,
    DMA7_TRANSFER3,
    NDS_SCANLINE256,
    NDS_SCANLINE355,
    GBA_SCANLINE240,
    GBA_SCANLINE308,
    GPU3D_COMMANDS,
    ARM9_INTERRUPT,
    ARM7_INTERRUPT,
    NDS_SPU_SAMPLE,
    GBA_SPU_SAMPLE,
    TIMER9_OVERFLOW0,
    TIMER9_OVERFLOW1,
    TIMER9_OVERFLOW2,
    TIMER9_OVERFLOW3,
    TIMER7_OVERFLOW0,
    TIMER7_OVERFLOW1,
    TIMER7_OVERFLOW2,
    TIMER7_OVERFLOW3,
    WIFI_COUNT_MS,
    WIFI_TRANS_REPLY,
    WIFI_TRANS_ACK,
    MAX_TASKS
};

struct SchedEvent {
    SchedTask task;
    uint32_t  cycles;

    SchedEvent(SchedTask task, uint32_t cycles): task(task), cycles(cycles) {}
    bool operator<(const SchedEvent& e) const { return cycles < e.cycles; }
};

class Core {
public:
    // Route Core and all sub-component allocations to MEM2.
    void* operator new  (size_t size);
    void  operator delete  (void* p) noexcept;
    void* operator new[](size_t size);
    void  operator delete[](void* p) noexcept;

    int  id      = 0;
    int  fps     = 0;
    bool arm7Hle = false;
    bool dsiMode = false;
    bool gbaMode = false;

    ActionReplay    actionReplay;
    CartridgeGba    cartridgeGba;
    CartridgeNds    cartridgeNds;
    Cp15            cp15;
    DivSqrt         divSqrt;
    Dldi            dldi;
    Dma             dma[2];
    Gpu             gpu;
    Gpu2D           gpu2D[2];
    Gpu3D           gpu3D;
    Gpu3DRenderer   gpu3DRenderer;
    HleArm7         hleArm7;
    HleBios         hleBios[3];
    Input           input;
    Interpreter     interpreter[2];
    Ipc             ipc;
    Memory          memory;
    Rtc             rtc;
    SaveStates      saveStates;
    Spi             spi;
    Spu             spu;
    Timers          timers[2];
    Wifi            wifi;

    // Replaces std::atomic<bool> running.
    // Writes are guarded by PPCIrqLockByMsr; reads on the emulator thread
    // side use PPCCompilerBarrier() to prevent the compiler from caching.
    volatile uint8_t running;

    std::vector<SchedEvent>   events;
    std::function<void()>     tasks[MAX_TASKS];
    uint32_t                  globalCycles = 0;

    Core(std::string ndsRom = "", std::string gbaRom = "", int id = 0,
         int ndsRomFd  = -1, int gbaRomFd  = -1,
         int ndsSaveFd = -1, int gbaSaveFd = -1,
         int ndsStateFd = -1, int gbaStateFd = -1,
         int ndsCheatFd = -1);

    void saveState(FILE* file);
    void loadState(FILE* file);

    void runCore() { (*runFunc)(*this); }
    void schedule(SchedTask task, uint32_t cycles);
    void enterGbaMode();
    void endFrame();

private:
    bool realGbaBios;
    void (*runFunc)(Core&) = &Interpreter::runCoreNds;

    // FPS timing via PPC timebase — replaces std::chrono.
    uint64_t lastFpsTimeTicks = 0;
    int      fpsCount         = 0;

    void updateRun();
    void resetCycles();
};
