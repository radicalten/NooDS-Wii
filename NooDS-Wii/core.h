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

// ---------------------------------------------------------------------------
// SchedCallback — a zero-overhead two-word closure.
//
//   fn  : plain function pointer, signature void(void*, int)
//   obj : opaque pointer forwarded as the first argument (the "this")
//   arg : small integer second argument (channel/index, 0 when unused)
//
// For tasks that carry no integer argument (arg == 0 always) the compiler
// will constant-fold the second parameter away after inlining the trampoline,
// leaving a single indirect branch — identical cost to a raw method pointer
// on PowerPC where member-function pointers are two words anyway.
// ---------------------------------------------------------------------------
struct SchedCallback {
    void (*fn)(void* obj, int arg);  // trampoline function
    void*  obj;                      // receiver object
    int    arg;                      // channel / index (0 if unused)

    // Default-construct to a safe no-op so the array is always callable.
    SchedCallback() : fn(nullptr), obj(nullptr), arg(0) {}

    SchedCallback(void (*f)(void*, int), void* o, int a = 0)
        : fn(f), obj(o), arg(a) {}

    // Invoke — inlined at every call site, single indirect branch on PPC.
    inline void operator()() const {
        fn(obj, arg);
    }
};

struct SchedEvent {
    SchedTask task;
    uint32_t  cycles;

    SchedEvent(SchedTask task, uint32_t cycles): task(task), cycles(cycles) {}
    bool operator<(const SchedEvent& e) const { return cycles < e.cycles; }
};

class Core {
public:
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

    volatile uint8_t running;

    std::vector<SchedEvent> events;

    // Flat array of two-word callbacks — fits in two cache lines (32 tasks).
    // Each entry is 12 bytes on 32-bit PPC: fn(4) + obj(4) + arg(4).
    SchedCallback tasks[MAX_TASKS];

    uint32_t globalCycles = 0;

    Core(std::string ndsRom = "", std::string gbaRom = "", int id = 0,
         int ndsRomFd  = -1, int gbaRomFd  = -1,
         int ndsSaveFd = -1, int gbaSaveFd = -1,
         int ndsStateFd = -1, int gbaStateFd = -1,
         int ndsCheatFd = -1);

    void saveState(FILE* file);
    void loadState(FILE* file);

    // Hot path: single load + indirect branch, no virtual dispatch.
    inline void runCore() { (*runFunc)(*this); }

    void schedule(SchedTask task, uint32_t cycles);
    void enterGbaMode();
    void endFrame();

private:
    bool realGbaBios;
    void (*runFunc)(Core&) = &Interpreter::runCoreNds;

    uint64_t lastFpsTimeTicks = 0;
    int      fpsCount         = 0;

    void updateRun();
    void resetCycles();

    // -----------------------------------------------------------------------
    // Static trampoline shims.
    //
    // Each shim is a plain function that recovers the typed pointer from the
    // opaque void* and calls the real member function.  Because they are
    // static and their bodies are trivial the compiler inlines them at the
    // SchedCallback::operator() call site, so the generated code is:
    //
    //   lwz   r3, obj(callback)      ; load receiver
    //   lwz   r4, arg(callback)      ; load channel (often optimised away)
    //   lwz   r12, fn(callback)      ; load trampoline address
    //   mtctr r12
    //   bctrl                        ; single indirect branch
    //
    // That is the minimum possible cost for a runtime-dispatched call on PPC.
    // -----------------------------------------------------------------------

    // Core self-tasks (arg ignored)
    static void shim_updateRun   (void* o, int) { static_cast<Core*>(o)->updateRun();    }
    static void shim_resetCycles (void* o, int) { static_cast<Core*>(o)->resetCycles();  }

    // CartridgeNds::wordReady(int cpu)  — arg = cpu index
    static void shim_cartWordReady(void* o, int a) {
        static_cast<CartridgeNds*>(o)->wordReady(a);
    }

    // Dma::transfer(int channel)  — arg = channel
    static void shim_dmaTransfer(void* o, int a) {
        static_cast<Dma*>(o)->transfer(a);
    }

    // Gpu scanlines (arg ignored)
    static void shim_gpuScanline256  (void* o, int) { static_cast<Gpu*>(o)->scanline256();     }
    static void shim_gpuScanline355  (void* o, int) { static_cast<Gpu*>(o)->scanline355();     }
    static void shim_gpuGbaScanline240(void* o, int) { static_cast<Gpu*>(o)->gbaScanline240(); }
    static void shim_gpuGbaScanline308(void* o, int) { static_cast<Gpu*>(o)->gbaScanline308(); }

    // Gpu3D (arg ignored)
    static void shim_gpu3dCommands(void* o, int) { static_cast<Gpu3D*>(o)->runCommands(); }

    // Interpreter::interrupt (arg ignored)
    static void shim_interpreterInterrupt(void* o, int) {
        static_cast<Interpreter*>(o)->interrupt();
    }

    // Spu (arg ignored)
    static void shim_spuSample   (void* o, int) { static_cast<Spu*>(o)->runSample();    }
    static void shim_spuGbaSample(void* o, int) { static_cast<Spu*>(o)->runGbaSample(); }

    // Timers::overflow(int timer)  — arg = timer index
    static void shim_timersOverflow(void* o, int a) {
        static_cast<Timers*>(o)->overflow(a);
    }

    // Wifi (arg ignored / arg = cmd)
    static void shim_wifiCountMs    (void* o, int)  { static_cast<Wifi*>(o)->countMs();          }
    static void shim_wifiTransmit(void* o, int a) {
    static_cast<Wifi*>(o)->transmitPacket(static_cast<PacketType>(a));
}
};
