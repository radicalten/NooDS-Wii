/*
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <malloc.h>
#include <time.h>
#include <stdarg.h>
#include <new>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include <string>
#include <algorithm>

#include "wii_video.h"
#include "wii_audio.h"
#include "core.h"
#include "settings.h"
#include "console_ui.h"

// Browser build: ignore test-harness autoboot files and ROM fallbacks on SD.
// Regular regression builds leave this off so G2/G3 continue to auto-run.
#ifndef NOODS_FORCE_BROWSER
#define NOODS_FORCE_BROWSER 0
#endif
#ifndef NOODS_G2_SCRIPTED_INPUT
#define NOODS_G2_SCRIPTED_INPUT 0
#endif
#ifndef NOODS_FULL_RAM_DUMP
#define NOODS_FULL_RAM_DUMP 0
#endif
#ifndef NOODS_TEST_FAST
#define NOODS_TEST_FAST 0
#endif

extern "C" {
    #include <tuxedo/thread.h>
    #include <tuxedo/ppc/intrinsics.h>
    #include <tuxedo/ppc/clock.h>
    #include <tuxedo/ppc/exception.h>
}

#define EMULATION_STACK_SIZE (256 * 1024)
#define AUDIO_STACK_SIZE     (64  * 1024)

static u8* mem2_free_ptr  = nullptr;
static u8* mem2_max_limit = nullptr;

static KThread  emulatorThread;
static KThread  audioThread;
static u8*      emulatorThreadStack = nullptr;
static u8*      audioThreadStack    = nullptr;

static KThread* g_emulatorThreadHandle = nullptr;

void InitializeMem2Arena() {
    // libogc 3.x (tuxedo) unifies MEM1 + MEM2 inside malloc/memalign/free (_sbrk_r)
    // while leaving Arena2Hi available for WPAD_Init() Bluetooth HCI & USBStorage.
}

void* Noods_MEM2_Alloc(size_t size) {
    if (size == 0) return nullptr;
    return memalign(32, (size + 31u) & ~31u);
}

void Noods_MEM2_Free(void* ptr) {
    free(ptr);
}

static Core*     ndsCore   = nullptr;
static bool      romLoaded = false;

static void NoodsPanicHandler(unsigned exid, PPCContext* ctx) {
    uint32_t insn = (ctx && ctx->pc >= 0x80000000u) ? *(const uint32_t*)(uintptr_t)ctx->pc : 0u;
    SYS_Report("[PANIC] exid=%u pc=0x%08X insn=0x%08X lr=0x%08X ctr=0x%08X r1=0x%08X r3=0x%08X r29=0x%08X r30=0x%08X r31=0x%08X\n",
               exid, ctx ? ctx->pc : 0, insn, ctx ? ctx->lr : 0, ctx ? ctx->ctr : 0,
               ctx ? ctx->gpr[1] : 0, ctx ? ctx->gpr[3] : 0, ctx ? ctx->gpr[29] : 0, ctx ? ctx->gpr[30] : 0, ctx ? ctx->gpr[31] : 0);
    if (ndsCore) {
        SYS_Report("[PANIC_CPU] arm9_pc=0x%08X arm7_pc=0x%08X gcyc=%u\n",
                   ndsCore->interpreter[0].getPC(), ndsCore->interpreter[1].getPC(), ndsCore->globalCycles);
    }
    while (true) {}
}

static uint32_t* frontBuffer = nullptr;
static uint32_t* backBuffer  = nullptr;

static volatile bool newFrameReady     = false;
static volatile bool runEmulatorThread = true;
static volatile bool runAudioThread    = true;

static uint32_t* topScreenBuffer    = nullptr;
static uint32_t* bottomScreenBuffer = nullptr;

float g_cursorX    = 320.0f;
float g_cursorY    = 240.0f;
bool  g_cursorShow = false;
static bool g_stickCursorOn     = false;
static bool g_humanInputSeen    = false;
static bool g_cursorNeedsCenter = true;

struct BrowserItem {
    std::string name;
    bool isDirectory;
};

static std::string currentDir = NOODS_FORCE_BROWSER ? "sd:/" : "sd:/noods/";
static std::vector<BrowserItem> dirContents;
static int selectedItemIndex = 0;
static int displayOffset     = 0;
static bool showFileBrowser  = true;
static std::string romToLoadPath    = "";
static std::string g_lastLoadedName = "";
static std::string g_browserNotice  = "";
static volatile bool triggerRomLoad    = false;
static volatile bool triggerRomUnload  = false;
static volatile bool romLoading        = false;
static volatile bool g_needBrowserRefresh = false;
static volatile bool g_audioBusy       = false;

static int scrollHoldTimer = 0;
static const int SCROLL_DELAY_INITIAL  = 18;
static const int SCROLL_DELAY_REPEATED = 3;

static void UpdateFileBrowser(const std::string& path);

static void FocusBrowserOnLastGame() {
    if (g_lastLoadedName.empty() || dirContents.empty()) return;
    for (int i = 0; i < (int)dirContents.size(); i++) {
        if (!dirContents[i].isDirectory && dirContents[i].name == g_lastLoadedName) {
            selectedItemIndex = i;
            displayOffset = std::max(0, std::min((int)dirContents.size() - 5, i - 2));
            if (selectedItemIndex < displayOffset)
                displayOffset = selectedItemIndex;
            if (selectedItemIndex >= displayOffset + 5)
                displayOffset = selectedItemIndex - 4;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Settings menu state
// ---------------------------------------------------------------------------
static bool showSettingsMenu      = false;
static volatile bool frameskipUserSet = false;
static int  settingsMenuIndex     = 0;
static int  settingsDisplayOffset = 0;

static int s_quitGameSentinel = 0;
static int s_quitToHomeSentinel = 0;
static int s_hideTopMenu      = 0;

struct SettingsEntry {
    const char* label;
    int*        value;
    int         maxValue;
    const char* const* names;
    int         nameCount;
};

static const char* const s_toggleNames[]    = { "Off", "On" };
static const char* const s_frameskipNames[] = {
    "None", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "15", "20", "30"
};
static const char* const s_layoutNames[] = {
    "Vertical", "Side-Side", "HybridTop", "HybridBot", "Scr1 Only", "Scr2 Only"
};
static const char* const s_sizeNames[] = {
    "Auto", "0.50x", "0.75x", "1.00x", "1.25x", "1.50x", "1.75x", "2.00x", "2.50x", "Off"
};
static const char* const s_gapNames[] = {
    "0px", "2px", "8px", "16px", "32px"
};
static const char* const s_filterNames[] = {
    "Sharp", "Smooth"
};

static const SettingsEntry s_settingsTable[] = {
    { "Quit Game",       &s_quitGameSentinel,     1,  nullptr,       0 },
    { "Quit to Wii Menu",&s_quitToHomeSentinel,   1,  nullptr,       0 },
    { "Hide Top Menu",   &s_hideTopMenu,          2,  s_toggleNames, 2 },
    { "ARM JIT",         &Settings::armJit,       2,  s_toggleNames, 2 },
    { "Audio Emulation", &Settings::emulateAudio, 2,  s_toggleNames, 2 },
    { "Skip Frames",     &Settings::frameskip,    Settings::FRAMESKIP_COUNT, s_frameskipNames, Settings::FRAMESKIP_COUNT },
    { "Screen Layout",   &Settings::screenLayout, 6,  s_layoutNames, 6 },
    { "Screen 1 Size",   &Settings::screen1Size,  10, s_sizeNames,   10 },
    { "Screen 2 Size",   &Settings::screen2Size,  10, s_sizeNames,   10 },
    { "Swap Screens",    &Settings::screenSwap,   2,  s_toggleNames, 2 },
    { "Stylus Edge Clamp",&Settings::stylusEdgeClamp, 2, s_toggleNames, 2 },
    { "Screen Gap",      &Settings::wiiScreenGap, 5,  s_gapNames,    5 },
    { "Screen Filter",   &Settings::screenFilter, 2,  s_filterNames, 2 },
    { "Direct Boot",     &Settings::directBoot,   2,  s_toggleNames, 2 },
    { "FPS Limiter",     &Settings::fpsLimiter,   2,  s_toggleNames, 2 },
    { "16-bit Output",   &Settings::audio16Bit,   2,  s_toggleNames, 2 },
    { "Mono Audio",      &Settings::monoAudio,    2,  s_toggleNames, 2 },
};

static const int s_settingsCount =
    (int)(sizeof(s_settingsTable) / sizeof(s_settingsTable[0]));

static const int SETTINGS_VISIBLE = 6;

static std::string SettingsEntryValue(const SettingsEntry& e) {
    if (!e.value) return "";
    if (e.value == &s_quitGameSentinel)
        return romLoaded ? "[Browse]" : "[In Menu]";
    if (e.value == &s_quitToHomeSentinel)
        return romLoaded ? "[Exit Wii]" : "[Exit Wii]";
    if (e.value == &Settings::frameskip) {
        int idx = Settings::frameskipToIndex(*e.value);
        return s_frameskipNames[idx];
    }
    if (e.value == &Settings::screenFilter)
        return (*e.value != 0) ? "Smooth" : "Sharp";
    int v = *e.value;
    if (e.names && v >= 0 && v < e.nameCount)
        return e.names[v];
    return std::to_string(v);
}

static int NextSelectableSettings(int start, int dir) {
    int idx = start + dir;
    if (idx < 0)                idx = s_settingsCount - 1;
    if (idx >= s_settingsCount) idx = 0;
    return idx;
}

static void ClampSettingsScroll() {
    if (settingsMenuIndex < settingsDisplayOffset)
        settingsDisplayOffset = settingsMenuIndex;
    if (settingsMenuIndex >= settingsDisplayOffset + SETTINGS_VISIBLE)
        settingsDisplayOffset = settingsMenuIndex - SETTINGS_VISIBLE + 1;
}

static void OpenSettingsMenu() {
    s_hideTopMenu    = 0;
    Wii_DebugOverlaySetEnabled(true);
    showSettingsMenu = true;
    // Start on row 0 ("Quit Game -> [Browse]") when a game is running, or row 1 ("Hide Top Menu") from browser.
    if (settingsMenuIndex < 0 || settingsMenuIndex >= s_settingsCount)
        settingsMenuIndex = romLoaded ? 0 : 1;
    ClampSettingsScroll();
}

static void StepSettingsEntry(int idx, int dir) {
    const SettingsEntry& e = s_settingsTable[idx];
    if (!e.value || e.value == &s_quitGameSentinel || e.value == &s_quitToHomeSentinel) return;
    if (e.value == &s_hideTopMenu) {
        s_hideTopMenu = (s_hideTopMenu == 0) ? 1 : 0;
        if (s_hideTopMenu != 0) {
            Settings::save();
            showSettingsMenu = false;
            Wii_DebugOverlaySetEnabled(false);
        } else {
            Wii_DebugOverlaySetEnabled(true);
        }
        return;
    }
    if (e.value == &Settings::frameskip) {
        Settings::stepFrameskip(dir);
        frameskipUserSet = true;
        return;
    }
    if (e.value == &Settings::screenFilter) {
        *e.value = (*e.value != 0) ? 0 : 2;
        return;
    }
    int v = (*e.value + dir) % e.maxValue;
    if (v < 0) v += e.maxValue;
    *e.value = v;
    if (e.value == &Settings::emulateAudio && Settings::emulateAudio && ndsCore) {
        ndsCore->spu.wakeSpuSchedule();
    }
}

#define NDS_KEY_A      0
#define NDS_KEY_B      1
#define NDS_KEY_SELECT 2
#define NDS_KEY_START  3
#define NDS_KEY_RIGHT  4
#define NDS_KEY_LEFT   5
#define NDS_KEY_UP     6
#define NDS_KEY_DOWN   7
#define NDS_KEY_R      8
#define NDS_KEY_L      9
#define NDS_KEY_X      10
#define NDS_KEY_Y      11

static volatile uint16_t g_ndsButtons  = 0;
static volatile bool     g_ndsTouching = false;
static volatile int16_t  g_ndsTouchX   = 0;
static volatile int16_t  g_ndsTouchY   = 0;

struct PerformanceState {
    uint32_t renderFrameCount;
    float    fps;
    time_t   startTime;
};
static PerformanceState perf = {};

// Presentation-side timing for the test harness (not guest CPU emulation time).
static uint64_t videoRenderTicks = 0, videoFlushTicks = 0, videoVsyncTicks = 0;
static uint32_t videoLoopCount = 0;
static uint32_t framesProduced = 0, framesOverwritten = 0;

#define AUDIO_SPU_FRAMES  WIIAUD_MAX_SPU_FRAMES
#define AUDIO_DMA_RING    4   // [0]=streaming DMA, [1]=AI shadow reg, [2..3]=CPU write slots

// Staging slot [0] holds the previous slice's final stereo sample for C0
// boundary continuity; slots [1..n] hold the n freshly drained SPU samples.
alignas(32) static uint32_t s_spuStaging[AUDIO_SPU_FRAMES + 1];
alignas(32) static int16_t  s_audioBufs[AUDIO_DMA_RING][WIIAUD_FRAMES_PER_BUF * 2];
alignas(32) static int16_t  s_audioSilence[WIIAUD_FRAMES_PER_BUF * 2];

static volatile uint32_t s_dmaWr       = 0;
static volatile uint32_t s_dmaRd       = 0;
static uint32_t          s_prevSample  = 0;
static bool              s_audioPrimed = false;
static KThrQueue         s_audioQueue;

// Resamples `nIntervals` intervals across `pts[0..nIntervals]` into `nDst`
// big-endian 16-bit stereo frames in `dst`, guaranteeing exact sample
// continuity at both slice boundaries even when FPS fluctuates below 60 Hz.
static void ResampleContinuous(const uint32_t* pts, int nIntervals,
                               int16_t*        dst, int nDst,
                               bool            mono)
{
    const int32_t step = (int32_t)(((int64_t)nIntervals << 16) / nDst);
    int32_t       pos  = step; // start one step past pts[0] so dst[nDst-1] lands on pts[nIntervals]

    for (int i = 0; i < nDst; i++) {
        int idx0 = pos >> 16;
        if (idx0 >= nIntervals) idx0 = nIntervals - 1;
        const int idx1 = idx0 + 1;
        const int32_t frac = pos & 0xFFFF;

        const int16_t l0 = (int16_t)( pts[idx0]        & 0xFFFF);
        const int16_t r0 = (int16_t)((pts[idx0] >> 16) & 0xFFFF);
        const int16_t l1 = (int16_t)( pts[idx1]        & 0xFFFF);
        const int16_t r1 = (int16_t)((pts[idx1] >> 16) & 0xFFFF);

        const int16_t outL = (int16_t)(l0 + (((int32_t)(l1 - l0) * frac) >> 16));
        const int16_t outR = (int16_t)(r0 + (((int32_t)(r1 - r0) * frac) >> 16));

        if (mono) {
            const int16_t m = (int16_t)(((int32_t)outL + (int32_t)outR) >> 1);
            dst[i * 2 + 0] = m;
            dst[i * 2 + 1] = m;
        } else {
            // GameCube/Wii Flipper/Hollywood hardware AI DMA stereo order is [Right, Left]
            dst[i * 2 + 0] = outR;
            dst[i * 2 + 1] = outL;
        }
        pos += step;
    }
}

volatile bool g_wiiAudioHardwareActive = false;

static void AudioDmaCallback(void)
{
    g_wiiAudioHardwareActive = true;
    const int16_t* nextBuf = s_audioSilence;
    const uint32_t rd = s_dmaRd;
    if ((uint32_t)(s_dmaWr - rd) > 0) {
        nextBuf = s_audioBufs[rd & (AUDIO_DMA_RING - 1)];
        s_dmaRd = rd + 1;
    }
    AUDIO_InitDMA((u32)nextBuf, WIIAUD_BUF_BYTES_STEREO);
    KThrQueueUnblockAllByValue(&s_audioQueue, 1);
}

static sptr AudioThreadMain(void* /*arg*/)
{
    while (runAudioThread) {
        {
            PPCIrqState st = PPCIrqLockByMsr();
            while ((uint32_t)(s_dmaWr - s_dmaRd) >= 2 && runAudioThread)
                KThrQueueBlock(&s_audioQueue, 1);
            PPCIrqUnlockByMsr(st);
        }
        if (!runAudioThread) break;

        int16_t*   dst    = s_audioBufs[s_dmaWr & (AUDIO_DMA_RING - 1)];
        int        pulled = 0;
        const bool mono   = (Settings::monoAudio != 0);
        const bool activeGameAudio = Settings::emulateAudio && romLoaded &&
                                     !showSettingsMenu && !triggerRomLoad && !triggerRomUnload;

        if (activeGameAudio) {
            g_audioBusy = true;
            PPCCompilerBarrier();
            Core* c = ndsCore;
            if (romLoaded && c) {
                // Always wait until a full 699-sample 32,768 Hz grain is ready in the
                // 4096-sample ring buffer so every slice resamples at exact 1.000x native
                // pitch (32,768 Hz -> 48,000 Hz) even when frame rate drops below 60 FPS,
                // never pitch-shifting game audio downward into slow-motion.
                const int minReq = s_audioPrimed ? WIIAUD_NDS_FRAMES : (WIIAUD_NDS_FRAMES + 350);
                uint32_t rem = 0;
                pulled = c->spu.drainSamples(&s_spuStaging[1], WIIAUD_NDS_FRAMES,
                                             &rem, minReq);
                if (pulled > 0) {
                    s_audioPrimed = true;
                    if (rem > 1400 && pulled == WIIAUD_NDS_FRAMES) {
                        int extra = c->spu.drainSamples(&s_spuStaging[1 + pulled],
                                                        WIIAUD_MAX_SPU_FRAMES - WIIAUD_NDS_FRAMES,
                                                        nullptr, 1);
                        pulled += extra;
                    }
                }
            }
            PPCCompilerBarrier();
            g_audioBusy = false;
        } else {
            s_audioPrimed = false;
        }

        if (pulled > 0) {
            s_spuStaging[0] = s_prevSample;
            ResampleContinuous(s_spuStaging, pulled, dst, WIIAUD_FRAMES_PER_BUF, mono);
            s_prevSample = s_spuStaging[pulled];
        } else if (activeGameAudio) {
            // Sub-60 FPS hold: hold the last output sample flat (0 Hz DC, zero step click
            // entering AND exiting the hold slice) while the next 699-sample grain finishes.
            int16_t holdL = (int16_t)( s_prevSample        & 0xFFFF);
            int16_t holdR = (int16_t)((s_prevSample >> 16) & 0xFFFF);
            if (mono) {
                const int16_t m = (int16_t)(((int32_t)holdL + (int32_t)holdR) >> 1);
                holdL = holdR = m;
            }
            for (int i = 0; i < WIIAUD_FRAMES_PER_BUF; i++) {
                dst[i * 2 + 0] = holdR;
                dst[i * 2 + 1] = holdL;
            }
        } else if (s_prevSample != 0) {
            // Smoothly ramp residual DC level back to 0 across one slice when pausing/muting.
            int32_t curL = (int16_t)( s_prevSample        & 0xFFFF);
            int32_t curR = (int16_t)((s_prevSample >> 16) & 0xFFFF);
            if (mono) {
                curL = curR = (curL + curR) >> 1;
            }
            for (int i = 0; i < WIIAUD_FRAMES_PER_BUF; i++) {
                curL = (curL * 250) >> 8;
                curR = (curR * 250) >> 8;
                dst[i * 2 + 0] = (int16_t)curR;
                dst[i * 2 + 1] = (int16_t)curL;
            }
            s_prevSample = 0;
        } else {
            memset(dst, 0, WIIAUD_BUF_BYTES_STEREO);
        }

        DCFlushRange(dst, WIIAUD_BUF_BYTES_STEREO);
        PPCCompilerBarrier();
        s_dmaWr = s_dmaWr + 1;
    }

    KThreadExit(0);
    return 0;
}

static void InitializeAudio()
{
    memset(s_audioBufs,    0, sizeof(s_audioBufs));
    memset(s_audioSilence, 0, sizeof(s_audioSilence));
    DCFlushRange(s_audioBufs,    sizeof(s_audioBufs));
    DCFlushRange(s_audioSilence, sizeof(s_audioSilence));

    s_dmaWr      = 0;
    s_dmaRd      = 0;
    s_prevSample = 0;

    // Reset DSP from any prior loader / Homebrew Channel microcode task, then
    // drive Wii Flipper/Hollywood hardware AI DMA directly at 48,000 Hz stereo
    // 16-bit big-endian (zero custom DSP microcode dependency or voice race).
    DSP_Init();
    AUDIO_Init(nullptr);
    AUDIO_StopDMA();
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    AUDIO_RegisterDMACallback(AudioDmaCallback);
    AUDIO_InitDMA((u32)s_audioSilence, WIIAUD_BUF_BYTES_STEREO);
    AUDIO_StartDMA();
}

static void ShutdownAudio()
{
    AUDIO_StopDMA();
    AUDIO_RegisterDMACallback(nullptr);
    PPCIrqState st = PPCIrqLockByMsr();
    runAudioThread = false;
    s_dmaWr      = 0;
    s_dmaRd      = 0;
    KThrQueueUnblockAllByValue(&s_audioQueue, 1);
    PPCIrqUnlockByMsr(st);
    KThreadJoin(&audioThread);
}

static void InitializeSettings() {
    Settings::directBoot   = 1;
    Settings::bios9Path    = "sd:/noods/bios/bios9.bin";
    Settings::bios7Path    = "sd:/noods/bios/bios7.bin";
    Settings::firmwarePath = "sd:/noods/bios/firmware.bin";
    Settings::gbaBiosPath  = "sd:/noods/bios/gba_bios.bin";
    Settings::sdImagePath  = "";
    Settings::basePath     = "sd:/";
    Settings::fpsLimiter   = 1;
    Settings::frameskip    = 0; // DS default; regression tests also pin this to zero.
#if NOODS_TEST_FAST
    Settings::fpsLimiter   = 0; // Uncap host presentation; emulated timing is unchanged.
    Settings::frameskip    = 0;
#endif
    Settings::threaded2D   = 0;
    Settings::threaded3D   = 0;
    Settings::highRes3D    = 0;
    Settings::screenGhost  = 0;
    Settings::emulateAudio = 1;
    Settings::audio16Bit   = 1;
    Settings::monoAudio    = 0;
    Settings::savesFolder  = 1;
    Settings::statesFolder = 1;
    Settings::cheatsFolder = 1;
    Settings::screenFilter = 0;
    Settings::screenLayout = 0;
    Settings::screen1Size  = 0;
    Settings::screen2Size  = 0;
    Settings::screenSwap   = 0;
    Settings::wiiScreenGap = 1;
    Settings::armJit       = 1;
    Settings::arm7Hle      = 0;
    Settings::dsiMode      = 0;
}

static bool CompareBrowserItems(const BrowserItem& a, const BrowserItem& b) {
    if (a.isDirectory && !b.isDirectory) return true;
    if (!a.isDirectory && b.isDirectory) return false;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
}

static void UpdateFileBrowser(const std::string& path) {
    dirContents.clear();
    selectedItemIndex = 0;
    displayOffset     = 0;

    std::string activePath = path;
    if (!activePath.empty() && activePath.back() != '/')
        activePath += '/';

    DIR* dir = opendir(activePath.c_str());
    if (!dir && activePath != "sd:/") {
        currentDir = "sd:/";
        activePath = "sd:/";
        dir = opendir(activePath.c_str());
    }
    if (!dir) return;

    if (activePath != "sd:/" && activePath != "sd:") {
        BrowserItem parent;
        parent.name        = "..";
        parent.isDirectory = true;
        dirContents.push_back(parent);
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (strncmp(entry->d_name, "._", 2) == 0)
            continue;

        BrowserItem item;
        item.name = entry->d_name;

        std::string fullPath = activePath + item.name;
        struct stat st;
        bool isDir = false;
        if (stat(fullPath.c_str(), &st) == 0)
            isDir = S_ISDIR(st.st_mode);
        else
            isDir = (entry->d_type == DT_DIR);
        item.isDirectory = isDir;

        if (!item.isDirectory) {
            size_t extPos = item.name.find_last_of('.');
            if (extPos == std::string::npos) continue;
            std::string ext = item.name.substr(extPos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".nds" && ext != ".srl" && ext != ".gba" && ext != ".agb") continue;
        }

        dirContents.push_back(item);
    }
    closedir(dir);

    size_t sortStart = (activePath != "sd:/" && activePath != "sd:") ? 1 : 0;
    std::sort(dirContents.begin() + sortStart, dirContents.end(),
              CompareBrowserItems);

    selectedItemIndex = (dirContents.size() > 1 && dirContents[0].name == "..") ? 1 : 0;
    displayOffset     = 0;
}

alignas(32) static uint32_t tempBuffer[NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 8];

#if NOODS_JIT
#include "arm_jit.h"
#endif

struct NoodsTestHarness {
    static uint32_t maxFrames;
    static std::vector<uint32_t> checkpoints;
    static uint32_t poolLimitWords;
#if NOODS_G2_SCRIPTED_INPUT
    // Explicit test-build-only input windows; never compiled into the browser DOL.
    static uint32_t inputAFrom, inputATo, inputA2From, inputA2To;
    static uint32_t inputRightFrom, inputRightTo;
    static uint32_t inputRight2From, inputRight2To, inputRight3From, inputRight3To;
    static uint32_t vramDumpFrom, vramDumpTo;
#endif
    static uint32_t currentFrame;
    static uint64_t startTick;
    static uint64_t accumFrameUs;

    static uint64_t fnv1a64(const void* data, size_t len, uint64_t h = 14695981039346656037ULL) {
        const uint8_t* p = (const uint8_t*)data;
        for (size_t i = 0; i < len; i++) {
            h ^= (uint64_t)p[i];
            h *= 1099511628211ULL;
        }
        return h;
    }

    static std::string trimLine(const std::string& s) {
        size_t a = 0;
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
        size_t b = s.size();
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
        return s.substr(a, b - a);
    }

    static bool fileExists(const char* path) {
        if (FILE* f = fopen(path, "rb")) {
            fclose(f);
            return true;
        }
        return false;
    }

    static void tryAutoboot() {
        maxFrames = 0;
        checkpoints.clear();
        poolLimitWords = 0;
#if NOODS_G2_SCRIPTED_INPUT
        inputAFrom = inputATo = inputA2From = inputA2To = 0;
        inputRightFrom = inputRightTo = inputRight2From = inputRight2To = 0;
        inputRight3From = inputRight3To = 0;
        vramDumpFrom = vramDumpTo = 0;
#endif
        currentFrame = 0;
        accumFrameUs = 0;

        std::string chosen;
        const char* abFiles[] = { "sd:/autoboot.txt", "sd:/noods/autoboot.txt" };
        for (const char* ab : abFiles) {
            if (FILE* f = fopen(ab, "rb")) {
                char lineBuf[256];
                bool firstLine = true;
                while (fgets(lineBuf, sizeof(lineBuf), f)) {
                    std::string line = trimLine(lineBuf);
                    if (line.empty() || line[0] == '#') continue;
                    if (firstLine && line.find('=') == std::string::npos) {
                        chosen = line;
                        firstLine = false;
                    } else if (line.rfind("MAX_FRAMES=", 0) == 0) {
                        maxFrames = (uint32_t)strtoul(line.c_str() + 11, nullptr, 10);
                    } else if (line.rfind("CHECKPOINTS=", 0) == 0) {
                        const char* p = line.c_str() + 12;
                        while (*p) {
                            char* endp = nullptr;
                            uint32_t v = (uint32_t)strtoul(p, &endp, 10);
                            if (endp == p) break;
                            if (v > 0) checkpoints.push_back(v);
                            p = (*endp == ',') ? (endp + 1) : endp;
                        }
                    } else if (line.rfind("POOL_LIMIT_WORDS=", 0) == 0) {
                        poolLimitWords = (uint32_t)strtoul(line.c_str() + 17, nullptr, 10);
#if NOODS_G2_SCRIPTED_INPUT
                    } else if (line.rfind("INPUT_A_FROM=", 0) == 0) {
                        inputAFrom = (uint32_t)strtoul(line.c_str() + 13, nullptr, 10);
                    } else if (line.rfind("INPUT_A_TO=", 0) == 0) {
                        inputATo = (uint32_t)strtoul(line.c_str() + 11, nullptr, 10);
                    } else if (line.rfind("INPUT_A2_FROM=", 0) == 0) {
                        inputA2From = (uint32_t)strtoul(line.c_str() + 14, nullptr, 10);
                    } else if (line.rfind("INPUT_A2_TO=", 0) == 0) {
                        inputA2To = (uint32_t)strtoul(line.c_str() + 12, nullptr, 10);
                    } else if (line.rfind("INPUT_RIGHT_FROM=", 0) == 0) {
                        inputRightFrom = (uint32_t)strtoul(line.c_str() + 17, nullptr, 10);
                    } else if (line.rfind("INPUT_RIGHT_TO=", 0) == 0) {
                        inputRightTo = (uint32_t)strtoul(line.c_str() + 15, nullptr, 10);
                    } else if (line.rfind("INPUT_RIGHT2_FROM=", 0) == 0) {
                        inputRight2From = (uint32_t)strtoul(line.c_str() + 18, nullptr, 10);
                    } else if (line.rfind("INPUT_RIGHT2_TO=", 0) == 0) {
                        inputRight2To = (uint32_t)strtoul(line.c_str() + 16, nullptr, 10);
                    } else if (line.rfind("INPUT_RIGHT3_FROM=", 0) == 0) {
                        inputRight3From = (uint32_t)strtoul(line.c_str() + 18, nullptr, 10);
                    } else if (line.rfind("INPUT_RIGHT3_TO=", 0) == 0) {
                        inputRight3To = (uint32_t)strtoul(line.c_str() + 16, nullptr, 10);
                    } else if (line.rfind("DUMP_VRAM_FROM=", 0) == 0) {
                        vramDumpFrom = (uint32_t)strtoul(line.c_str() + 15, nullptr, 10);
                    } else if (line.rfind("DUMP_VRAM_TO=", 0) == 0) {
                        vramDumpTo = (uint32_t)strtoul(line.c_str() + 13, nullptr, 10);
#endif
                    }
                }
                fclose(f);
                break;
            }
        }

        if (!chosen.empty()) {
            if (!checkpoints.empty() || maxFrames > 0) {
                Settings::fpsLimiter = 0;
                Settings::frameskip  = 0;
                if (FILE* f = fopen("sd:/g2_state.log", "wb")) fclose(f);
            }
#if NOODS_JIT
            if (poolLimitWords > 0) {
                ArmPpcJit::setPoolLimitWords(poolLimitWords);
            }
#endif
            PPCIrqState st = PPCIrqLockByMsr();
            romToLoadPath   = chosen;
            romLoaded       = false;
            triggerRomLoad  = true;
            showFileBrowser = false;
            PPCIrqUnlockByMsr(st);
            SYS_Report("[NOODS] AUTOBOOT path=%s maxFrames=%u poolLimitWords=%u\n",
                       chosen.c_str(), maxFrames, poolLimitWords);
        }
    }

    static bool isCheckpoint(uint32_t frame) {
        if (maxFrames > 0 && frame == maxFrames) return true;
        for (uint32_t c : checkpoints) {
            if (c == frame) return true;
        }
        return false;
    }

    static void dumpCpuLine(FILE* f, const char* label, Interpreter& cpu) {
        uint32_t spsrVal = cpu.spsr ? *cpu.spsr : 0;
        fprintf(f, "%s cpsr=%08X spsr=%08X pc=%08X pipe0=%08X pipe1=%08X halted=%u ime=%u ie=%08X irf=%08X postFlg=%u cycles=%u",
                label, cpu.cpsr, spsrVal, *cpu.registers[15], cpu.pipeline[0], cpu.pipeline[1],
                (unsigned)cpu.halted, (unsigned)cpu.ime, cpu.ie, cpu.irf, (unsigned)cpu.postFlg, cpu.cycles);
        for (int r = 0; r < 16; r++) {
            fprintf(f, " r%d=%08X", r, cpu.registers[r] ? *cpu.registers[r] : cpu.registersUsr[r]);
        }
        for (int r = 0; r < 16; r++) {
            fprintf(f, " usr%d=%08X", r, cpu.registersUsr[r]);
        }
        fprintf(f, " svc13=%08X svc14=%08X irq13=%08X irq14=%08X fiq13=%08X fiq14=%08X\n",
                cpu.registersSvc[0], cpu.registersSvc[1],
                cpu.registersIrq[0], cpu.registersIrq[1],
                cpu.registersFiq[5], cpu.registersFiq[6]);
    }

    static void onFrameEnd(Core* core, uint64_t frameTicks) {
        currentFrame++;
        if (currentFrame == 1) startTick = PPCGetTickCount();
        uint64_t frameUs = PPCTicksToUs(frameTicks);
        accumFrameUs += frameUs;

        Memory& mem = core->memory;
#if NOODS_G2_SCRIPTED_INPUT
        if (vramDumpFrom > 0 && currentFrame >= vramDumpFrom && currentFrame <= vramDumpTo) {
            if (FILE* vf = fopen("sd:/vramA_frames.bin", "ab")) {
                uint8_t frameBytes[4] = {
                    (uint8_t)(currentFrame >> 0), (uint8_t)(currentFrame >> 8),
                    (uint8_t)(currentFrame >> 16), (uint8_t)(currentFrame >> 24)
                };
                fwrite(frameBytes, 1, sizeof(frameBytes), vf);
                fwrite(mem.ram + 0x3F0400, 1, 20 * sizeof(uint32_t), vf);
                fwrite(mem.vramA, 1, sizeof(mem.vramA), vf);
                fflush(vf);
                fclose(vf);
            }
        }
#endif
        if (!isCheckpoint(currentFrame))
            return;

        size_t ramLen = core->gbaMode ? 0x40000 : 0x400000;
        uint64_t hRam   = fnv1a64(mem.ram, ramLen);
        uint64_t hWram  = fnv1a64(mem.wram, sizeof(mem.wram));
        uint64_t hItcm  = fnv1a64(mem.instrTcm, sizeof(mem.instrTcm));
        uint64_t hDtcm  = fnv1a64(mem.dataTcm, sizeof(mem.dataTcm));
        uint64_t hWram7 = fnv1a64(mem.wram7, sizeof(mem.wram7));
        uint64_t hPal   = fnv1a64(mem.palette, sizeof(mem.palette));
        uint64_t hOam   = fnv1a64(mem.oam, sizeof(mem.oam));
        uint64_t hVram  = fnv1a64(mem.vramA, sizeof(mem.vramA));
        hVram = fnv1a64(mem.vramB, sizeof(mem.vramB), hVram);
        hVram = fnv1a64(mem.vramC, sizeof(mem.vramC), hVram);
        hVram = fnv1a64(mem.vramD, sizeof(mem.vramD), hVram);
        hVram = fnv1a64(mem.vramE, sizeof(mem.vramE), hVram);
        hVram = fnv1a64(mem.vramF, sizeof(mem.vramF), hVram);
        hVram = fnv1a64(mem.vramG, sizeof(mem.vramG), hVram);
        hVram = fnv1a64(mem.vramH, sizeof(mem.vramH), hVram);
        hVram = fnv1a64(mem.vramI, sizeof(mem.vramI), hVram);

        uint64_t hTotalMem = hRam ^ (hWram << 1) ^ (hItcm << 2) ^ (hDtcm << 3) ^
                             (hWram7 << 4) ^ (hPal << 5) ^ (hOam << 6) ^ (hVram << 7);

        size_t scrPixels = NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT;
        uint64_t hFbTop  = fnv1a64(tempBuffer, scrPixels * sizeof(uint32_t));
        uint64_t hFbBot  = fnv1a64(tempBuffer + scrPixels, scrPixels * sizeof(uint32_t));
        uint64_t hFbAll  = fnv1a64(tempBuffer, scrPixels * 2 * sizeof(uint32_t));

        if (FILE* f = fopen("sd:/g2_state.log", "ab")) {
            fprintf(f, "=== CHECKPOINT FRAME %u ===\n", currentFrame);
            fprintf(f, "CYCLES global=%u arm9=%u arm7=%u gbaMode=%d\n",
                    core->globalCycles, core->interpreter[0].cycles, core->interpreter[1].cycles, (int)core->gbaMode);
            dumpCpuLine(f, "ARM9", core->interpreter[0]);
            dumpCpuLine(f, "ARM7", core->interpreter[1]);
#if NOODS_G2_SCRIPTED_INPUT
            fprintf(f, "SCRIPT_INPUT frame=%u key=%04X ext=%04X right=%u:%u,%u:%u,%u:%u\n",
                    currentFrame, core->input.readKeyInput(), core->input.readExtKeyIn(),
                    inputRightFrom, inputRightTo, inputRight2From, inputRight2To,
                    inputRight3From, inputRight3To);
#endif
            fprintf(f, "MEM_HASH ram=%016llX wram=%016llX itcm=%016llX dtcm=%016llX wram7=%016llX pal=%016llX vram=%016llX oam=%016llX total=%016llX\n",
                    (unsigned long long)hRam, (unsigned long long)hWram,
                    (unsigned long long)hItcm, (unsigned long long)hDtcm,
                    (unsigned long long)hWram7, (unsigned long long)hPal,
                    (unsigned long long)hVram, (unsigned long long)hOam,
                    (unsigned long long)hTotalMem);
            fprintf(f, "FB_HASH top=%016llX bottom=%016llX combined=%016llX\n",
                    (unsigned long long)hFbTop, (unsigned long long)hFbBot, (unsigned long long)hFbAll);
            fprintf(f, "PERF frame=%u accum_us=%llu avg_frame_us=%llu\n",
                    currentFrame, (unsigned long long)accumFrameUs,
                    (unsigned long long)(accumFrameUs / currentFrame));
            // Rendered FPS and emulated FPS are different when frameskip is nonzero.
            // Sample the UI's rendered-frame counter separately from Core::fps.
            PPCIrqState frameLock = PPCIrqLockByMsr();
            uint32_t rendered = perf.renderFrameCount;
            PPCIrqUnlockByMsr(frameLock);
            fprintf(f, "DISPLAY frames=%u rendered=%u produced=%u overwritten=%u core_fps=%d skip=%d elapsed_us=%llu loops=%u render_us=%llu flush_us=%llu vsync_us=%llu\n",
                    currentFrame, rendered, framesProduced, framesOverwritten,
                    core->fps, Settings::frameskip,
                    (unsigned long long)PPCTicksToUs(PPCGetTickCount() - startTick),
                    videoLoopCount,
                    (unsigned long long)PPCTicksToUs(videoRenderTicks),
                    (unsigned long long)PPCTicksToUs(videoFlushTicks),
                    (unsigned long long)PPCTicksToUs(videoVsyncTicks));
#if NOODS_JIT
            ArmPpcJit::dumpStats(f);
#else
            fprintf(f, "JIT_STATS enabled=0\n");
#endif
#if NOODS_NDS_TASK_PROFILE
            if (!core->gbaMode) Interpreter::dumpNdsTaskProfile(f);
#endif
            fflush(f);
            fclose(f);
        }

        if (maxFrames > 0 && currentFrame == maxFrames) {
            mem.syncMgbaLogToSd(false);
            if (FILE* mf = fopen("sd:/mem_dump.bin", "wb")) {
                fwrite(mem.wram, 1, sizeof(mem.wram), mf);
                fwrite(mem.instrTcm, 1, sizeof(mem.instrTcm), mf);
                fwrite(mem.dataTcm, 1, sizeof(mem.dataTcm), mf);
                fwrite(mem.wram7, 1, sizeof(mem.wram7), mf);
                fwrite(mem.palette, 1, sizeof(mem.palette), mf);
                fwrite(mem.oam, 1, sizeof(mem.oam), mf);
                fwrite(mem.ram, 1, (NOODS_FULL_RAM_DUMP || !core->gbaMode) ? 0x400000 : 0x40000, mf);
                fflush(mf);
                fclose(mf);
            }
            if (FILE* ff = fopen("sd:/fb_dump.bin", "wb")) {
                fwrite(tempBuffer, sizeof(uint32_t), scrPixels * 2, ff);
                fflush(ff);
                fclose(ff);
            }
            fatUnmount("sd");
            fatInitDefault();
        }

        SYS_Report("[NOODS] CHECKPOINT frame=%u cyc=%u mem=%016llX fb=%016llX us=%llu\n",
                   currentFrame, core->globalCycles,
                   (unsigned long long)hTotalMem, (unsigned long long)hFbAll,
                   (unsigned long long)accumFrameUs);

        if (maxFrames > 0 && currentFrame >= maxFrames) {
            SYS_Report("[NOODS] MAX_FRAMES_DONE frame=%u\n", currentFrame);
            runEmulatorThread = false;
        }
    }
};

uint32_t NoodsTestHarness::maxFrames = 0;
std::vector<uint32_t> NoodsTestHarness::checkpoints;
uint32_t NoodsTestHarness::poolLimitWords = 0;
#if NOODS_G2_SCRIPTED_INPUT
uint32_t NoodsTestHarness::inputAFrom = 0, NoodsTestHarness::inputATo = 0;
uint32_t NoodsTestHarness::inputA2From = 0, NoodsTestHarness::inputA2To = 0;
uint32_t NoodsTestHarness::inputRightFrom = 0, NoodsTestHarness::inputRightTo = 0;
uint32_t NoodsTestHarness::inputRight2From = 0, NoodsTestHarness::inputRight2To = 0;
uint32_t NoodsTestHarness::inputRight3From = 0, NoodsTestHarness::inputRight3To = 0;
uint32_t NoodsTestHarness::vramDumpFrom = 0, NoodsTestHarness::vramDumpTo = 0;
#endif
uint32_t NoodsTestHarness::currentFrame = 0;
uint64_t NoodsTestHarness::startTick = 0;
uint64_t NoodsTestHarness::accumFrameUs = 0;

// Extern declaration kept in sync with wii_video.cpp cursor globals.
float g_wiiCursorMirrorUnused = 0.0f;

static void StopAndDeleteCore() {
    {
        PPCIrqState st = PPCIrqLockByMsr();
        romLoaded     = false;
        newFrameReady = false;
        PPCIrqUnlockByMsr(st);
    }
    PPCCompilerBarrier();
    while (g_audioBusy)
        KThreadSleepMs(1);
    memset(s_spuStaging, 0, sizeof(s_spuStaging));
    if (ndsCore) {
        ndsCore->cartridgeNds.writeSave();
        ndsCore->cartridgeGba.writeSave();
        delete ndsCore;
        ndsCore = nullptr;
    }
}

static sptr EmulatorThreadMain(void* /*arg*/) {
    const size_t pixelCount = NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 2;

    while (runEmulatorThread) {
        if (triggerRomUnload) {
            StopAndDeleteCore();
            PPCIrqState st = PPCIrqLockByMsr();
            triggerRomUnload = false;
            romLoading       = false;
            PPCIrqUnlockByMsr(st);
        }

        if (triggerRomLoad) {
            std::string loadPath;
            {
                PPCIrqState st = PPCIrqLockByMsr();
                loadPath         = romToLoadPath;
                triggerRomLoad   = false;
                triggerRomUnload = false;
                romLoading       = true;
                PPCIrqUnlockByMsr(st);
            }

            StopAndDeleteCore();

            std::string ndsRom, gbaRom;
            size_t extPos = loadPath.find_last_of('.');
            if (extPos != std::string::npos) {
                std::string ext = loadPath.substr(extPos);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".gba" || ext == ".agb") gbaRom = loadPath;
                else                                ndsRom = loadPath;
            }

            Settings::frameskip =
                Settings::FRAMESKIP_VALUES[Settings::frameskipToIndex(Settings::frameskip)];

            const char* errMsg = nullptr;
            try {
                ndsCore = new Core(ndsRom, gbaRom);
                PPCIrqState st = PPCIrqLockByMsr();
                romLoaded  = true;
                romLoading = false;
                PPCIrqUnlockByMsr(st);
            }
            catch (CoreError ce) {
                ndsCore = nullptr;
                errMsg = (ce == ERROR_BIOS) ? "Missing BIOS file"
                       : (ce == ERROR_FIRM) ? "Missing firmware"
                                            : "Failed reading ROM";
            }
            catch (std::bad_alloc&) {
                ndsCore = nullptr;
                errMsg = "Out of memory loading ROM";
            }
            catch (...) {
                ndsCore = nullptr;
                errMsg = "Failed launching ROM";
            }

            if (errMsg) {
                PPCIrqState st = PPCIrqLockByMsr();
                romLoaded            = false;
                romLoading           = false;
                s_hideTopMenu        = 0;
                showFileBrowser      = true;
                g_needBrowserRefresh = true;
                g_browserNotice      = errMsg;
                PPCIrqUnlockByMsr(st);
            }
        }

        if (romLoaded && ndsCore) {
            {
                PPCIrqState st  = PPCIrqLockByMsr();
                uint16_t buttons  = g_ndsButtons;
                bool     touching = g_ndsTouching;
                int16_t  touchX   = g_ndsTouchX;
                int16_t  touchY   = g_ndsTouchY;
                PPCIrqUnlockByMsr(st);

#if NOODS_G2_SCRIPTED_INPUT
                uint32_t f = NoodsTestHarness::currentFrame;
                if ((NoodsTestHarness::inputAFrom && f >= NoodsTestHarness::inputAFrom && f < NoodsTestHarness::inputATo) ||
                    (NoodsTestHarness::inputA2From && f >= NoodsTestHarness::inputA2From && f < NoodsTestHarness::inputA2To))
                    buttons |= (1u << NDS_KEY_A);
                if ((NoodsTestHarness::inputRightFrom && f >= NoodsTestHarness::inputRightFrom && f < NoodsTestHarness::inputRightTo) ||
                    (NoodsTestHarness::inputRight2From && f >= NoodsTestHarness::inputRight2From && f < NoodsTestHarness::inputRight2To) ||
                    (NoodsTestHarness::inputRight3From && f >= NoodsTestHarness::inputRight3From && f < NoodsTestHarness::inputRight3To))
                    buttons |= (1u << NDS_KEY_RIGHT);
#endif

                for (int bit = 0; bit < 12; bit++) {
                    if (buttons & (1 << bit)) ndsCore->input.pressKey(bit);
                    else                      ndsCore->input.releaseKey(bit);
                }
                if (touching) {
                    ndsCore->spi.setTouch((int)touchX, (int)touchY);
                    ndsCore->input.pressScreen();
                } else {
                    ndsCore->spi.clearTouch();
                    ndsCore->input.releaseScreen();
                }
            }

#if NOODS_TEST_FAST
            // Keep the raster regression deterministic even if a test UI changes settings.
            Settings::frameskip = 0;
#endif
            uint64_t t0 = PPCGetTickCount();
            ndsCore->runCore();
            uint64_t t1 = PPCGetTickCount();

            if (ndsCore->gpu.getFrame(tempBuffer, ndsCore->gbaMode)) {
                PPCIrqState st = PPCIrqLockByMsr();
                framesProduced++;
                if (newFrameReady) framesOverwritten++;
                // GBA output is 240x160, not two 256x192 DS screens. Keep the
                // interrupt-disabled copy limited to the pixels actually displayed.
                size_t copyPixels = ndsCore->gbaMode ? (size_t)GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT : pixelCount;
                memcpy(backBuffer, tempBuffer, copyPixels * sizeof(uint32_t));
                newFrameReady = true;
                PPCIrqUnlockByMsr(st);
            }

            NoodsTestHarness::onFrameEnd(ndsCore, t1 - t0);
        }
        else {
            KThreadSleepMs(16);
        }
    }

    KThreadExit(0);
    return 0;
}

static void InitializeNDS() {
    const size_t bufSize =
        NDS_SCREEN_WIDTH * (NDS_SCREEN_HEIGHT * 2) * sizeof(uint32_t);

    frontBuffer = (uint32_t*)Noods_MEM2_Alloc(bufSize);
    if (!frontBuffer) frontBuffer = (uint32_t*)malloc(bufSize);

    backBuffer  = (uint32_t*)Noods_MEM2_Alloc(bufSize);
    if (!backBuffer) backBuffer = (uint32_t*)malloc(bufSize);

    if (!frontBuffer || !backBuffer) {
        while (true) KThreadSleepMs(16);
    }
    memset(frontBuffer, 0, bufSize);
    memset(backBuffer,  0, bufSize);

    InitializeSettings();

    if (fatInitDefault()) {
        FILE* iniProbe = fopen("sd:/noods.ini", "r");
        if (iniProbe) {
            fclose(iniProbe);
            Settings::load("sd:/");
        }
        UpdateFileBrowser(currentDir);
#if NOODS_FORCE_BROWSER
        SYS_Report("[NOODS] FILE_BROWSER active dir=%s\n", currentDir.c_str());
#else
        NoodsTestHarness::tryAutoboot();
#endif
    }

    const size_t sz = NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * sizeof(uint32_t);
    topScreenBuffer    = (uint32_t*)Noods_MEM2_Alloc(sz);
    if (!topScreenBuffer) topScreenBuffer = (uint32_t*)malloc(sz);

    bottomScreenBuffer = (uint32_t*)Noods_MEM2_Alloc(sz);
    if (!bottomScreenBuffer) bottomScreenBuffer = (uint32_t*)malloc(sz);

    if (topScreenBuffer && bottomScreenBuffer) {
        for (int i = 0; i < NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT; i++) {
            topScreenBuffer[i]    = 0xFF0000FF;
            bottomScreenBuffer[i] = 0x0000FFFF;
        }
    }

    emulatorThreadStack = (u8*)Noods_MEM2_Alloc(EMULATION_STACK_SIZE);
    if (!emulatorThreadStack) emulatorThreadStack = (u8*)malloc(EMULATION_STACK_SIZE);

    audioThreadStack = (u8*)Noods_MEM2_Alloc(AUDIO_STACK_SIZE);
    if (!audioThreadStack) audioThreadStack = (u8*)malloc(AUDIO_STACK_SIZE);

    if (!emulatorThreadStack || !audioThreadStack) {
        while (true) KThreadSleepMs(16);
    }

    KThreadPrepare(&emulatorThread, EmulatorThreadMain, nullptr,
                   emulatorThreadStack + EMULATION_STACK_SIZE, 0x50);
    g_emulatorThreadHandle = &emulatorThread;
    KThreadResume(&emulatorThread);

    InitializeAudio();
    KThreadPrepare(&audioThread, AudioThreadMain, nullptr,
                   audioThreadStack + AUDIO_STACK_SIZE, 0x48);
    KThreadResume(&audioThread);

    perf.startTime = time(nullptr);
}

// ---------------------------------------------------------------------------
// Unified multi-port controller state (Wiimote / Nunchuk / Wii Classic &
// Classic Pro / GameCube controllers across all channels 0..3).
// ---------------------------------------------------------------------------
static void ReadJoystickAxes(const joystick_t& js, int defaultMax,
                             float* outX, float* outY)
{
    float x = 0.0f, y = 0.0f;
    bool saneCal = (js.max.x > js.center.x) && (js.center.x > js.min.x) &&
                   (js.max.y > js.center.y) && (js.center.y > js.min.y) &&
                   isfinite(js.mag) && isfinite(js.ang);
    if (saneCal && js.mag >= 0.0f && js.mag <= 1.5f) {
        float rad = js.ang * 3.14159265f / 180.0f;
        x = js.mag * sinf(rad);
        y = js.mag * cosf(rad);
    } else if (defaultMax > 2) {
        float half = 0.5f * (float)defaultMax;
        x = ((float)js.pos.x - half) / half;
        y = ((float)js.pos.y - half) / half;
    }
    if (!isfinite(x)) x = 0.0f;
    if (!isfinite(y)) y = 0.0f;
    if (x < -1.0f) x = -1.0f;
    if (x >  1.0f) x =  1.0f;
    if (y < -1.0f) y = -1.0f;
    if (y >  1.0f) y =  1.0f;
    *outX = x;
    *outY = y;
}

struct ControllerState {
    u32   wpadHeld      = 0;
    u32   wpadPressed   = 0;
    u32   gcHeld        = 0;
    u32   gcPressed     = 0;
    bool  hasNunchuk    = false;
    bool  hasClassic    = false;
    bool  classicLHeld  = false;
    bool  classicRHeld  = false;
    bool  classicLDown  = false;
    bool  classicRDown  = false;
    bool  gcAnalogLHeld = false;
    bool  gcAnalogRHeld = false;
    bool  gcAnalogLDown = false;
    bool  gcAnalogRDown = false;
    float leftX         = 0.0f;
    float leftY         = 0.0f;
    float rightX        = 0.0f;
    float rightY        = 0.0f;
    bool  irValid       = false;
    float irX           = 320.0f;
    float irY           = 240.0f;
};

static void PollControllers(ControllerState& cs) {
    static u32  s_prevMergedHeld   = 0;
    static bool s_prevClassicL     = false;
    static bool s_prevClassicR     = false;
    static bool s_prevGcL          = false;
    static bool s_prevGcR          = false;
    static s32  s_chanFmt[4]       = { -1, -1, -1, -1 };
    static int  s_classicWarmup[4] = { 0, 0, 0, 0 };

    WPAD_ScanPads();
    PAD_ScanPads();

    for (int ch = 0; ch < 4; ch++) {
        WPADData* wdata = WPAD_Data(ch);

        // CRITICAL HARDWARE FIX (Wii Bluetooth + libogc 3.x):
        // Never call WPAD_SetDataFormat / WPAD_SetVRes while wiiuse_handshake is
        // still in flight (WPAD_Probe != WPAD_ERR_NONE) — doing so aborts the
        // initial status/extension handshake at hs=1 and leaves Nunchuk & Classic
        // Controller permanently dead (exp.type = 0)!
        // Once WPAD_Probe succeeds:
        //  - Wiimote, Nunchuk, Classic Controller, and Classic Controller Pro all
        //    use 6-byte Extension Data Format 1 inside WPAD_FMT_BTNS_ACC_IR (report
        //    0x37: 2B buttons + 3B accel + 10B IR + 6B extension), preserving full
        //    640x480 Wiimote IR stylus tracking simultaneously with all 16 Classic
        //    buttons & both analog sticks!
        //  - Only Wii U Pro Controller (classic.type == 2, 11-byte report, no IR
        //    camera) uses WPAD_FMT_BTNS_ACC (report 0x35).
        u32 probedExt = WPAD_EXP_NONE;
        s32 probeRes  = WPAD_Probe(ch, &probedExt);
        if (probeRes == WPAD_ERR_NONE) {
            bool isWiiUPro = (probedExt == WPAD_EXP_CLASSIC && wdata &&
                              wdata->exp.classic.type == 2);
            s32 wantFmt = isWiiUPro ? WPAD_FMT_BTNS_ACC : WPAD_FMT_BTNS_ACC_IR;
            if (s_chanFmt[ch] != wantFmt) {
                if (wantFmt == WPAD_FMT_BTNS_ACC_IR)
                    WPAD_SetVRes(ch, 640, 480);
                WPAD_SetDataFormat(ch, wantFmt);
                if (wantFmt == WPAD_FMT_BTNS_ACC_IR)
                    WPAD_SetVRes(ch, 640, 480);
                s_chanFmt[ch] = wantFmt;
            }
        } else if (probeRes == WPAD_ERR_NO_CONTROLLER) {
            s_chanFmt[ch] = -1;
        }

        wdata = WPAD_Data(ch);
        u32 wh = WPAD_ButtonsHeld(ch);
        u32 wd = WPAD_ButtonsDown(ch);

        if (wdata) {
            if (wdata->exp.type == WPAD_EXP_CLASSIC) {
                cs.hasClassic = true;
                // Note: in libogc 3.x wiiuse, classic.btns holds ALL currently held
                // buttons (with masks 0x0001..0x8000 matching WPAD_CLASSIC_BUTTON_* >> 16).
                // Flush initial MotionPlus->Classic handover FIFO packets and ignore any
                // corrupt packet claiming opposite D-Pad directions simultaneously held.
                u32 cbtns = (u32)wdata->exp.classic.btns & 0xFEFFu;
                const u32 udMask = (WPAD_CLASSIC_BUTTON_UP   | WPAD_CLASSIC_BUTTON_DOWN)  >> 16;
                const u32 lrMask = (WPAD_CLASSIC_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_RIGHT) >> 16;
                bool bogusPacket = ((cbtns & udMask) == udMask) || ((cbtns & lrMask) == lrMask);
                if (s_classicWarmup[ch] < 8) {
                    s_classicWarmup[ch]++;
                } else if (!bogusPacket) {
                    wh |= cbtns << 16;
                    if (wdata->exp.classic.l_shoulder > 0.35f) cs.classicLHeld = true;
                    if (wdata->exp.classic.r_shoulder > 0.35f) cs.classicRHeld = true;

                    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
                    ReadJoystickAxes(wdata->exp.classic.ljs, 63, &lx, &ly);
                    ReadJoystickAxes(wdata->exp.classic.rjs, 31, &rx, &ry);
                    if (fabsf(lx) > fabsf(cs.leftX))  cs.leftX  = lx;
                    if (fabsf(ly) > fabsf(cs.leftY))  cs.leftY  = ly;
                    if (fabsf(rx) > fabsf(cs.rightX)) cs.rightX = rx;
                    if (fabsf(ry) > fabsf(cs.rightY)) cs.rightY = ry;
                }
            }
            else {
                s_classicWarmup[ch] = 0;
                if (wdata->exp.type == WPAD_EXP_NUNCHUK) {
                    cs.hasNunchuk = true;
                    wh |= ((u32)wdata->exp.nunchuk.btns & 0x03u) << 16;
                    float nx = 0.0f, ny = 0.0f;
                    ReadJoystickAxes(wdata->exp.nunchuk.js, 255, &nx, &ny);
                    if (fabsf(nx) > fabsf(cs.leftX)) cs.leftX = nx;
                    if (fabsf(ny) > fabsf(cs.leftY)) cs.leftY = ny;
                }
            }

            if (!cs.irValid && wdata->ir.valid) {
                cs.irValid = true;
                cs.irX     = wdata->ir.x;
                cs.irY     = wdata->ir.y;
            }
        }

        cs.wpadHeld    |= wh;
        cs.wpadPressed |= wd;

        cs.gcHeld    |= PAD_ButtonsHeld(ch);
        cs.gcPressed |= PAD_ButtonsDown(ch);

        if (PAD_TriggerL(ch) > 80) cs.gcAnalogLHeld = true;
        if (PAD_TriggerR(ch) > 80) cs.gcAnalogRHeld = true;

        float gx = (float)PAD_StickX(ch) / 100.0f;
        float gy = (float)PAD_StickY(ch) / 100.0f;
        if (fabsf(gx) > fabsf(cs.leftX)) cs.leftX = gx;
        if (fabsf(gy) > fabsf(cs.leftY)) cs.leftY = gy;

        float cx = (float)PAD_SubStickX(ch) / 90.0f;
        float cy = (float)PAD_SubStickY(ch) / 90.0f;
        if (fabsf(cx) > fabsf(cs.rightX)) cs.rightX = cx;
        if (fabsf(cy) > fabsf(cs.rightY)) cs.rightY = cy;
    }

    cs.wpadPressed |= (cs.wpadHeld & ~s_prevMergedHeld);
    s_prevMergedHeld = cs.wpadHeld;

    if (cs.wpadHeld & WPAD_CLASSIC_BUTTON_FULL_L) cs.classicLHeld = true;
    if (cs.wpadHeld & WPAD_CLASSIC_BUTTON_FULL_R) cs.classicRHeld = true;
    if (cs.gcHeld & PAD_TRIGGER_L) cs.gcAnalogLHeld = true;
    if (cs.gcHeld & PAD_TRIGGER_R) cs.gcAnalogRHeld = true;

    cs.classicLDown = (cs.wpadPressed & WPAD_CLASSIC_BUTTON_FULL_L) || (cs.classicLHeld && !s_prevClassicL);
    cs.classicRDown = (cs.wpadPressed & WPAD_CLASSIC_BUTTON_FULL_R) || (cs.classicRHeld && !s_prevClassicR);
    cs.gcAnalogLDown = (cs.gcPressed & PAD_TRIGGER_L) || (cs.gcAnalogLHeld && !s_prevGcL);
    cs.gcAnalogRDown = (cs.gcPressed & PAD_TRIGGER_R) || (cs.gcAnalogRHeld && !s_prevGcR);

    s_prevClassicL = cs.classicLHeld;
    s_prevClassicR = cs.classicRHeld;
    s_prevGcL      = cs.gcAnalogLHeld;
    s_prevGcR      = cs.gcAnalogRHeld;
}

static void ReturnToFileBrowserFromGame() {
    Settings::save();

    if (romLoaded || romLoading || triggerRomLoad) {
        {
            PPCIrqState st = PPCIrqLockByMsr();
            triggerRomLoad   = false;
            triggerRomUnload = true;
            g_ndsButtons     = 0;
            g_ndsTouching    = false;
            PPCIrqUnlockByMsr(st);
        }

        // Wait cleanly for EmulatorThreadMain to flush save data & free Core.
        while (triggerRomUnload && runEmulatorThread)
            KThreadSleepMs(2);
    }

    const size_t bufBytes = NDS_SCREEN_WIDTH * (NDS_SCREEN_HEIGHT * 2) * sizeof(uint32_t);
    if (frontBuffer) memset(frontBuffer, 0, bufBytes);
    if (backBuffer)  memset(backBuffer,  0, bufBytes);

    newFrameReady       = false;
    g_stickCursorOn     = false;
    g_cursorNeedsCenter = true;
    g_cursorShow        = false;
    perf.fps            = 0.0f;
    g_browserNotice     = "";
    s_hideTopMenu       = 0;
    Wii_DebugOverlaySetEnabled(true);
    showSettingsMenu    = false;
    showFileBrowser     = true;

    UpdateFileBrowser(currentDir);
    FocusBrowserOnLastGame();
}

static void QuitToWiiMenuSafely() {
    // Safe quit to Wii Menu / Homebrew Channel — mirrors HOME button path
    Settings::save();
    runEmulatorThread = false;
    PPCCompilerBarrier();
    KThreadJoin(&emulatorThread);
    ShutdownAudio();
    StopAndDeleteCore();
    exit(0);
}

static uint16_t MapInputsToNDS(const ControllerState& cs, bool gbaMode) {
    const u32 held = cs.wpadHeld;
    const u32 gcHeld = cs.gcHeld;
    uint16_t nds = 0;

    // Wii Remote buttons
    if (held & WPAD_BUTTON_RIGHT)  nds |= (1 << NDS_KEY_RIGHT);
    if (held & WPAD_BUTTON_LEFT)   nds |= (1 << NDS_KEY_LEFT);
    if (held & WPAD_BUTTON_UP)     nds |= (1 << NDS_KEY_UP);
    if (held & WPAD_BUTTON_DOWN)   nds |= (1 << NDS_KEY_DOWN);
    if (held & WPAD_BUTTON_2)      nds |= (1 << NDS_KEY_A);
    if (held & WPAD_BUTTON_1)      nds |= (1 << NDS_KEY_B);
    if (held & WPAD_BUTTON_A)      nds |= (1 << NDS_KEY_X);
    if (held & WPAD_BUTTON_B)      nds |= (1 << NDS_KEY_Y);
    if (held & WPAD_BUTTON_PLUS)   nds |= (1 << NDS_KEY_START);
    if (held & WPAD_BUTTON_MINUS)  nds |= (1 << NDS_KEY_SELECT);

    // Nunchuk buttons
    if (cs.hasNunchuk) {
        if (held & WPAD_NUNCHUK_BUTTON_Z) nds |= (1 << NDS_KEY_L);
        if (held & WPAD_NUNCHUK_BUTTON_C) nds |= (1 << NDS_KEY_R);
    }

    // Wii Classic Controller / Classic Controller Pro buttons
    if (cs.hasClassic || (held & 0xFFFF0000u)) {
        if (held & WPAD_CLASSIC_BUTTON_RIGHT) nds |= (1 << NDS_KEY_RIGHT);
        if (held & WPAD_CLASSIC_BUTTON_LEFT)  nds |= (1 << NDS_KEY_LEFT);
        if (held & WPAD_CLASSIC_BUTTON_UP)    nds |= (1 << NDS_KEY_UP);
        if (held & WPAD_CLASSIC_BUTTON_DOWN)  nds |= (1 << NDS_KEY_DOWN);
        if (held & WPAD_CLASSIC_BUTTON_A)     nds |= (1 << NDS_KEY_A);
        if (held & WPAD_CLASSIC_BUTTON_B)     nds |= (1 << NDS_KEY_B);
        if (held & WPAD_CLASSIC_BUTTON_X)     nds |= (gbaMode ? (1 << NDS_KEY_A) : (1 << NDS_KEY_X));
        if (held & WPAD_CLASSIC_BUTTON_Y)     nds |= (gbaMode ? (1 << NDS_KEY_B) : (1 << NDS_KEY_Y));
        if (held & WPAD_CLASSIC_BUTTON_PLUS)  nds |= (1 << NDS_KEY_START);
        if (held & WPAD_CLASSIC_BUTTON_MINUS) nds |= (1 << NDS_KEY_SELECT);
        if (cs.classicLHeld)                  nds |= (1 << NDS_KEY_L);
        if (cs.classicRHeld)                  nds |= (1 << NDS_KEY_R);
        if (gbaMode) {
            if (held & WPAD_CLASSIC_BUTTON_ZL) nds |= (1 << NDS_KEY_L);
            if (held & WPAD_CLASSIC_BUTTON_ZR) nds |= (1 << NDS_KEY_R);
        }
    }

    // GameCube Controller buttons + analog triggers
    if (gcHeld & PAD_BUTTON_A)     nds |= (1 << NDS_KEY_A);
    if (gcHeld & PAD_BUTTON_B)     nds |= (1 << NDS_KEY_B);
    if (gcHeld & PAD_BUTTON_X)     nds |= (gbaMode ? (1 << NDS_KEY_A)      : (1 << NDS_KEY_X));
    if (gcHeld & PAD_BUTTON_Y)     nds |= (gbaMode ? (1 << NDS_KEY_SELECT) : (1 << NDS_KEY_Y));
    if (gcHeld & PAD_BUTTON_LEFT)  nds |= (1 << NDS_KEY_LEFT);
    if (gcHeld & PAD_BUTTON_RIGHT) nds |= (1 << NDS_KEY_RIGHT);
    if (gcHeld & PAD_BUTTON_UP)    nds |= (1 << NDS_KEY_UP);
    if (gcHeld & PAD_BUTTON_DOWN)  nds |= (1 << NDS_KEY_DOWN);
    if (cs.gcAnalogRHeld)          nds |= (1 << NDS_KEY_R);
    if (cs.gcAnalogLHeld)          nds |= (1 << NDS_KEY_L);
    if (gcHeld & PAD_BUTTON_START) nds |= (1 << NDS_KEY_START);
    if (gbaMode && (gcHeld & PAD_TRIGGER_Z))
        nds |= (1 << NDS_KEY_SELECT);

    // Unified Left Stick (Classic Controller Left Stick, Nunchuk Stick, or GameCube Main Stick)
    if (cs.leftX >  0.38f) nds |= (1 << NDS_KEY_RIGHT);
    if (cs.leftX < -0.38f) nds |= (1 << NDS_KEY_LEFT);
    if (cs.leftY >  0.38f) nds |= (1 << NDS_KEY_UP);
    if (cs.leftY < -0.38f) nds |= (1 << NDS_KEY_DOWN);

    return nds;
}

// ---------------------------------------------------------------------------
// Settings combo helper — shared between browser and in-game contexts.
// Wii Remote / Classic Controller: B + PLUS  (or MINUS + PLUS)
// GameCube Controller:             B + START
// ---------------------------------------------------------------------------
static bool CheckSettingsCombo(const ControllerState& cs)
{
    const u32 held      = cs.wpadHeld;
    const u32 pressed   = cs.wpadPressed;
    const u32 gcHeld    = cs.gcHeld;
    const u32 gcPressed = cs.gcPressed;

    bool bHeld     = (held    & (WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B)) != 0;
    bool bDown     = (pressed & (WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B)) != 0;
    bool minusHeld = (held    & (WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS)) != 0;
    bool minusDown = (pressed & (WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS)) != 0;
    bool plusHeld  = (held    & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) != 0;
    bool plusDown  = (pressed & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) != 0;

    bool gcBHeld     = (gcHeld    & PAD_BUTTON_B) != 0;
    bool gcBDown     = (gcPressed & PAD_BUTTON_B) != 0;
    bool gcStartHeld = (gcHeld    & PAD_BUTTON_START) != 0;
    bool gcStartDown = (gcPressed & PAD_BUTTON_START) != 0;

    return (bDown && plusHeld) || (plusDown && bHeld) ||
           (minusDown && plusHeld) || (plusDown && minusHeld) ||
           (gcBDown && gcStartHeld) || (gcStartDown && gcBHeld);
}

// ---------------------------------------------------------------------------
// HandleSettingsInput — drives the settings menu each frame it is open.
// ---------------------------------------------------------------------------
static int settingsScrollHoldTimer = 0;

static void HandleSettingsInput(const ControllerState& cs)
{
    const u32 pressed   = cs.wpadPressed;
    const u32 held      = cs.wpadHeld;
    const u32 gcPressed = cs.gcPressed;
    const u32 gcHeld    = cs.gcHeld;

    static bool settingsStickUpLast   = false;
    static bool settingsStickDownLast = false;
    bool analogUp   = (cs.leftY >  0.42f);
    bool analogDown = (cs.leftY < -0.42f);

    bool upPressed   = (pressed & (WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP))
                     || (gcPressed & PAD_BUTTON_UP)
                     || (analogUp && !settingsStickUpLast);
    bool downPressed = (pressed & (WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN))
                     || (gcPressed & PAD_BUTTON_DOWN)
                     || (analogDown && !settingsStickDownLast);
    bool upHeld      = (held & (WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP))
                     || (gcHeld & PAD_BUTTON_UP)
                     || analogUp;
    bool downHeld    = (held & (WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN))
                     || (gcHeld & PAD_BUTTON_DOWN)
                     || analogDown;
    settingsStickUpLast   = analogUp;
    settingsStickDownLast = analogDown;

    bool doUp   = false;
    bool doDown = false;

    if (upPressed || downPressed) {
        settingsScrollHoldTimer = 0;
        if (upPressed)   doUp   = true;
        if (downPressed) doDown = true;
    } else if (upHeld || downHeld) {
        settingsScrollHoldTimer++;
        int elapsed = settingsScrollHoldTimer - SCROLL_DELAY_INITIAL;
        if (elapsed >= 0 && elapsed % SCROLL_DELAY_REPEATED == 0) {
            if (upHeld)   doUp   = true;
            if (downHeld) doDown = true;
        }
    } else {
        settingsScrollHoldTimer = 0;
    }

    if (doUp)
        settingsMenuIndex = NextSelectableSettings(settingsMenuIndex, -1);
    if (doDown)
        settingsMenuIndex = NextSelectableSettings(settingsMenuIndex, +1);

    ClampSettingsScroll();

    // Page up / down with shoulder triggers (L / R / ZL / ZR).
    bool pageUp   = cs.classicLDown || cs.gcAnalogLDown || (pressed & WPAD_CLASSIC_BUTTON_ZL);
    bool pageDown = cs.classicRDown || cs.gcAnalogRDown || (pressed & WPAD_CLASSIC_BUTTON_ZR);

    if (pageUp) {
        for (int i = 0; i < SETTINGS_VISIBLE; i++)
            settingsMenuIndex = NextSelectableSettings(settingsMenuIndex, -1);
        ClampSettingsScroll();
    }
    if (pageDown) {
        for (int i = 0; i < SETTINGS_VISIBLE; i++)
            settingsMenuIndex = NextSelectableSettings(settingsMenuIndex, +1);
        ClampSettingsScroll();
    }

    bool confirmAction = (pressed & (WPAD_BUTTON_A | WPAD_BUTTON_2 | WPAD_CLASSIC_BUTTON_A))
                       || (gcPressed & PAD_BUTTON_A);

    // Special action rows: Quit Game -> file browser, Quit to Wii Menu -> HOME exit
    if (s_settingsTable[settingsMenuIndex].value == &s_quitGameSentinel ||
        s_settingsTable[settingsMenuIndex].value == &s_quitToHomeSentinel) {
        if (confirmAction) {
            if (s_settingsTable[settingsMenuIndex].value == &s_quitGameSentinel)
                ReturnToFileBrowserFromGame();
            else
                QuitToWiiMenuSafely();
            return;
        }
    } else {
        // Left steps backward (-1); A / 2 / Right steps forward (+1).
        bool stepBack = (pressed & (WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT))
                      || (gcPressed & PAD_BUTTON_LEFT);
        bool stepFwd  = confirmAction
                      || (pressed & (WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT))
                      || (gcPressed & PAD_BUTTON_RIGHT);
        if (stepBack)
            StepSettingsEntry(settingsMenuIndex, -1);
        else if (stepFwd)
            StepSettingsEntry(settingsMenuIndex, +1);
    }

    // B / 1 / MINUS (when PLUS is not held) to save and close.
    bool plusHeldNow = (held & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) != 0;
    bool close = (pressed & (WPAD_BUTTON_B | WPAD_BUTTON_1 | WPAD_CLASSIC_BUTTON_B))
               || (gcPressed & PAD_BUTTON_B)
               || (!plusHeldNow && (pressed & (WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS)));
    if (close) {
        Settings::save();
        showSettingsMenu = false;
    }
}

// ---------------------------------------------------------------------------
// DrawSettingsOverlay
// Line 0   : title + hint
// Lines 1-6: up to SETTINGS_VISIBLE entries
// Line 7   : scroll indicator + live help
// ---------------------------------------------------------------------------
static void DrawSettingsOverlay() {
    Wii_DebugOverlayPrint(0, "Settings [</> A=Set] [B=Save]");

    int lineIdx = 1;
    for (int row = settingsDisplayOffset;
         row < s_settingsCount && lineIdx <= SETTINGS_VISIBLE;
         row++, lineIdx++)
    {
        const SettingsEntry& e = s_settingsTable[row];
        std::string val  = SettingsEntryValue(e);
        bool selected    = (row == settingsMenuIndex);
        Wii_DebugOverlayPrint(lineIdx, "%s%-15s %-9s",
            selected ? "->" : "  ",
            e.label,
            val.c_str());
    }

    while (lineIdx <= SETTINGS_VISIBLE)
        Wii_DebugOverlayPrint(lineIdx++, " ");

    Wii_DebugOverlayPrint(7,
        "%s %2d/%2d %s  %s",
        (settingsDisplayOffset > 0) ? "^^" : "  ",
        settingsMenuIndex + 1,
        s_settingsCount,
        (settingsDisplayOffset + SETTINGS_VISIBLE < s_settingsCount) ? "vv" : "  ",
        romLoaded ? "Live Preview Active" : "ROM Browser Ready");
}

static void ScanWiiInputs() {
    ControllerState cs;
    PollControllers(cs);

    if (g_needBrowserRefresh) {
        g_needBrowserRefresh = false;
        UpdateFileBrowser(currentDir);
        FocusBrowserOnLastGame();
    }

    const u32 held      = cs.wpadHeld;
    const u32 pressed   = cs.wpadPressed;
    const u32 gcHeld    = cs.gcHeld;
    const u32 gcPressed = cs.gcPressed;

    if (held || gcHeld || cs.irValid ||
        fabsf(cs.leftX) > 0.25f || fabsf(cs.leftY) > 0.25f ||
        fabsf(cs.rightX) > 0.16f || fabsf(cs.rightY) > 0.16f) {
        g_humanInputSeen = true;
    }

    // HOME always quits cleanly.
    if ((pressed & WPAD_BUTTON_HOME) || (pressed & WPAD_CLASSIC_BUTTON_HOME)) {
        runEmulatorThread = false;
        PPCCompilerBarrier();
        KThreadJoin(&emulatorThread);
        ShutdownAudio();
        StopAndDeleteCore();
        exit(0);
    }

    // -----------------------------------------------------------------------
    // Settings menu — highest priority, blocks everything else.
    // -----------------------------------------------------------------------
    if (showSettingsMenu) {
        HandleSettingsInput(cs);
        return;
    }

    // -----------------------------------------------------------------------
    // File browser
    // -----------------------------------------------------------------------
    if (showFileBrowser) {
        if (!dirContents.empty()) {
            static bool stickUpLast    = false;
            static bool stickDownLast  = false;
            static bool stickLeftLast  = false;
            static bool stickRightLast = false;

            bool analogUp    = (cs.leftY >  0.42f);
            bool analogDown  = (cs.leftY < -0.42f);
            bool analogLeft  = (cs.leftX < -0.42f);
            bool analogRight = (cs.leftX >  0.42f);

            bool upHeld   = (held & (WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP))
                          || (gcHeld & PAD_BUTTON_UP)
                          || analogUp;
            bool downHeld = (held & (WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN))
                          || (gcHeld & PAD_BUTTON_DOWN)
                          || analogDown;
            bool upDown   = (pressed & (WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP))
                          || (gcPressed & PAD_BUTTON_UP)
                          || (analogUp && !stickUpLast);
            bool downDown = (pressed & (WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN))
                          || (gcPressed & PAD_BUTTON_DOWN)
                          || (analogDown && !stickDownLast);
            bool leftDown  = (pressed & (WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT))
                           || (gcPressed & PAD_BUTTON_LEFT)
                           || cs.classicLDown || cs.gcAnalogLDown
                           || (pressed & WPAD_CLASSIC_BUTTON_ZL)
                           || (analogLeft && !stickLeftLast);
            bool rightDown = (pressed & (WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT))
                           || (gcPressed & PAD_BUTTON_RIGHT)
                           || cs.classicRDown || cs.gcAnalogRDown
                           || (pressed & WPAD_CLASSIC_BUTTON_ZR)
                           || (analogRight && !stickRightLast);

            stickUpLast    = analogUp;
            stickDownLast  = analogDown;
            stickLeftLast  = analogLeft;
            stickRightLast = analogRight;

            bool performUp   = false;
            bool performDown = false;

            if (upDown || downDown) {
                scrollHoldTimer = 0;
                if (upDown)   performUp   = true;
                if (downDown) performDown = true;
            } else if (upHeld || downHeld) {
                scrollHoldTimer++;
                if (scrollHoldTimer >= SCROLL_DELAY_INITIAL) {
                    if ((scrollHoldTimer - SCROLL_DELAY_INITIAL) %
                        SCROLL_DELAY_REPEATED == 0) {
                        if (upHeld)   performUp   = true;
                        if (downHeld) performDown = true;
                    }
                }
            } else {
                scrollHoldTimer = 0;
            }

            if (performDown) {
                selectedItemIndex++;
                if (selectedItemIndex >= (int)dirContents.size()) {
    selectedItemIndex = (dirContents.size() > 1 && dirContents[0].name == "..") ? 1 : 0;
    displayOffset     = 0;
}
                if (selectedItemIndex >= displayOffset + 5)
                    displayOffset = selectedItemIndex - 4;
            }
            if (performUp) {
                selectedItemIndex--;
                if (selectedItemIndex < 0) {
                    selectedItemIndex = (int)dirContents.size() - 1;
                    displayOffset     = std::max(0, selectedItemIndex - 4);
                }
                if (selectedItemIndex < displayOffset)
                    displayOffset = selectedItemIndex;
            }

            if (leftDown) {
                selectedItemIndex -= 5;
                if (selectedItemIndex < 0) selectedItemIndex = 0;
                displayOffset = std::max(0, selectedItemIndex - 2);
            }
            if (rightDown) {
                selectedItemIndex += 5;
                if (selectedItemIndex >= (int)dirContents.size())
                    selectedItemIndex = (int)dirContents.size() - 1;
                displayOffset = std::max(0,
                    std::min((int)dirContents.size() - 5,
                             selectedItemIndex - 2));
            }

            if ((pressed & (WPAD_BUTTON_A | WPAD_BUTTON_2 | WPAD_CLASSIC_BUTTON_A))
             || (gcPressed & PAD_BUTTON_A)) {
                g_browserNotice = "";
                BrowserItem selected = dirContents[selectedItemIndex];
                if (selected.isDirectory) {
                    if (selected.name == "..") {
                        size_t slash = currentDir.find_last_of('/',
                            currentDir.length() - 2);
                        if (slash != std::string::npos)
                            currentDir = currentDir.substr(0, slash + 1);
                    } else {
                        currentDir += selected.name + "/";
                    }
                    UpdateFileBrowser(currentDir);
                } else {
                    std::string romPath = currentDir + selected.name;
                    g_lastLoadedName = selected.name;

                    PPCIrqState st = PPCIrqLockByMsr();
                    romToLoadPath    = romPath;
                    romLoaded        = false;
                    romLoading       = true;
                    triggerRomUnload = false;
                    triggerRomLoad   = true;
                    PPCIrqUnlockByMsr(st);

                    g_humanInputSeen    = true;
                    g_cursorNeedsCenter = true;
                    showFileBrowser     = false;

                    dirContents.clear();
                    dirContents.shrink_to_fit();
                }
            }
        }

        // B (or 1 on Wiimote): go up a directory, or close browser if a ROM is already loaded.
        if ((pressed & (WPAD_BUTTON_B | WPAD_BUTTON_1 | WPAD_CLASSIC_BUTTON_B))
         || (gcPressed & PAD_BUTTON_B)) {
            if (currentDir != "sd:/" && currentDir != "sd://"
             && currentDir != "sd:") {
                size_t slash = currentDir.find_last_of('/',
                    currentDir.length() - 2);
                if (slash != std::string::npos) {
                    currentDir = currentDir.substr(0, slash + 1);
                    UpdateFileBrowser(currentDir);
                }
            } else if (romLoaded) {
                showFileBrowser = false;
            }
        }

        // Open Settings from browser via X, MINUS, or B+PLUS / B+START combo.
        if (CheckSettingsCombo(cs)
         || (pressed & (WPAD_CLASSIC_BUTTON_X | WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS))
         || (gcPressed & PAD_BUTTON_X)) {
            settingsMenuIndex = 1; // Default to Skip Frames when opened in browser
            OpenSettingsMenu();
        }
    }
    // -----------------------------------------------------------------------
    // In-game (emulation running or loading, browser closed)
    // -----------------------------------------------------------------------
    else {
        static bool  s_stickOverridesIr = false;
        static bool  s_prevIrValid      = false;
        static float s_prevIrX          = -1000.0f;
        static float s_prevIrY          = -1000.0f;

        const bool isGbaGame = (romLoaded && ndsCore) ? ndsCore->gbaMode : false;
        uint16_t ndsButtons  = MapInputsToNDS(cs, isGbaGame);

        float dsLeft = 0.0f, dsTop = 0.0f, dsRight = 0.0f, dsBottom = 0.0f;
        bool touchVis = !isGbaGame &&
                        Wii_GetTouchScreenRect(&dsLeft, &dsTop, &dsRight, &dsBottom) &&
                        dsRight > dsLeft && dsBottom > dsTop;

        if (!isGbaGame && g_humanInputSeen && g_cursorNeedsCenter && touchVis) {
            g_cursorX           = 0.5f * (dsLeft + dsRight);
            g_cursorY           = 0.5f * (dsTop + dsBottom);
            g_stickCursorOn     = true;
            g_cursorNeedsCenter = false;
        }

        bool isTouching = false;
        g_cursorShow = g_stickCursorOn && !isGbaGame;

        if (!isGbaGame && cs.irValid) {
            float dx = cs.irX - s_prevIrX;
            float dy = cs.irY - s_prevIrY;
            bool irMoved = !s_prevIrValid || ((dx * dx + dy * dy) > 25.0f);
            if (!s_stickOverridesIr || irMoved || (held & WPAD_BUTTON_A)) {
                s_stickOverridesIr = false;
                g_cursorX          = cs.irX;
                g_cursorY          = cs.irY;
                g_stickCursorOn    = true;
                g_cursorShow       = true;
            }
            if (held & WPAD_BUTTON_A) {
                isTouching   = true;
                g_cursorShow = true;
            }
            s_prevIrValid = true;
            s_prevIrX     = cs.irX;
            s_prevIrY     = cs.irY;
        } else {
            s_prevIrValid = false;
        }

        if (!isGbaGame && (fabsf(cs.rightX) > 0.16f || fabsf(cs.rightY) > 0.16f)) {
            s_stickOverridesIr = true;
            g_cursorX         += cs.rightX * 4.5f;
            g_cursorY         -= cs.rightY * 4.5f;
            g_stickCursorOn    = true;
            g_cursorShow       = true;

            if (g_cursorX < 0.0f)   g_cursorX = 0.0f;
            if (g_cursorX > 640.0f) g_cursorX = 640.0f;
            if (g_cursorY < 0.0f)   g_cursorY = 0.0f;
            if (g_cursorY > 480.0f) g_cursorY = 480.0f;
        }

        if (!isGbaGame && (gcHeld & PAD_TRIGGER_Z)) {
            s_stickOverridesIr = true;
            isTouching         = true;
            g_stickCursorOn    = true;
            g_cursorShow       = true;
        }

        if (!isGbaGame && (held & (WPAD_CLASSIC_BUTTON_ZL | WPAD_CLASSIC_BUTTON_ZR))) {
            s_stickOverridesIr = true;
            isTouching         = true;
            g_stickCursorOn    = true;
            g_cursorShow       = true;
        }

        int16_t touchX = 0;
        int16_t touchY = 0;

        if (g_cursorShow) {
            if (touchVis) {
                bool inside = (g_cursorX >= dsLeft  && g_cursorX <= dsRight &&
                               g_cursorY >= dsTop   && g_cursorY <= dsBottom);
                if (inside) {
                    touchX = (int16_t)(((g_cursorX - dsLeft) /
                                        (dsRight  - dsLeft)) * 256.0f);
                    touchY = (int16_t)(((g_cursorY - dsTop)  /
                                        (dsBottom - dsTop))  * 192.0f);
                } else if (isTouching && Settings::stylusEdgeClamp) {
                    // Clamp off-screen press to nearest edge (cyan square outside → touch at boundary)
                    float cx = g_cursorX;
                    if (cx < dsLeft)  cx = dsLeft;
                    else if (cx > dsRight) cx = dsRight;
                    float cy = g_cursorY;
                    if (cy < dsTop)    cy = dsTop;
                    else if (cy > dsBottom) cy = dsBottom;
                    float fx = (cx - dsLeft) / (dsRight  - dsLeft) * 256.0f;
                    float fy = (cy - dsTop)  / (dsBottom - dsTop)  * 192.0f;
                    if (fx < 0.0f) fx = 0.0f; else if (fx > 255.0f) fx = 255.0f;
                    if (fy < 0.0f) fy = 0.0f; else if (fy > 191.0f) fy = 191.0f;
                    touchX = (int16_t)fx;
                    touchY = (int16_t)fy;
                    // keep isTouching = true (clamped touch)
                } else {
                    isTouching = false;
                }
            } else {
                isTouching = false;
            }
        }

        PPCIrqState st = PPCIrqLockByMsr();
        g_ndsButtons  = ndsButtons;
        g_ndsTouching = isTouching;
        g_ndsTouchX   = touchX;
        g_ndsTouchY   = touchY;
        PPCIrqUnlockByMsr(st);

        // Open Settings while in-game via B+PLUS / MINUS+PLUS / B+START
        // (even when the top-left menu is hidden — unhides & reopens it immediately!).
        if (CheckSettingsCombo(cs)) {
            settingsMenuIndex     = 0; // Start right on "Quit Game -> [Browse]" for instant game switching!
            settingsDisplayOffset = 0;
            OpenSettingsMenu();
        }
    }
}

int main(int /*argc*/, char** /*argv*/) {
    PPCExcptCurPanicFn = NoodsPanicHandler;
    Wii_VideoInit();
    InitializeMem2Arena();

    PAD_Init();
    WPAD_Init();

    InitializeNDS();

    while (true) {
        ScanWiiInputs();

        const uint32_t* renderTop    = nullptr;
        const uint32_t* renderBottom = nullptr;
        bool haveFrame = false;
        const bool inGame = romLoaded && !showFileBrowser && !showSettingsMenu;

        if (romLoaded && !showFileBrowser) {
            if (inGame) {
                PPCIrqState st  = PPCIrqLockByMsr();
                haveFrame       = newFrameReady;
                if (haveFrame) {
                    uint32_t* tmp = frontBuffer;
                    frontBuffer   = backBuffer;
                    backBuffer    = tmp;
                    newFrameReady = false;
                    perf.renderFrameCount++;
                }
                PPCIrqUnlockByMsr(st);
            }

            renderTop = frontBuffer;
            if (ndsCore && ndsCore->gbaMode)
                renderBottom = bottomScreenBuffer;
            else
                renderBottom = frontBuffer + (NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT);
        }
        else {
            renderTop    = topScreenBuffer;
            renderBottom = bottomScreenBuffer;
        }

        static uint32_t lastSecFrames = 0;
        static u64      lastFpsTick   = 0;
        u64 nowFpsTick = PPCGetTickCount();
        if (!inGame || lastFpsTick == 0) {
            lastFpsTick   = nowFpsTick;
            lastSecFrames = perf.renderFrameCount;
        } else {
            u64 elapsedUs = PPCTicksToUs(nowFpsTick - lastFpsTick);
            if (elapsedUs >= 1000000ULL) {
                perf.fps      = (float)(perf.renderFrameCount - lastSecFrames) * 1000000.0f / (float)elapsedUs;
                lastSecFrames = perf.renderFrameCount;
                lastFpsTick   = nowFpsTick;
            }
        }

        // -------------------------------------------------------------------
        // Overlay — settings menu takes all 8 lines when open; Hide Top Menu
        // suppresses the entire top-left box until re-opened via hotkey.
        // -------------------------------------------------------------------
        const bool shouldShowOverlay = showSettingsMenu || (s_hideTopMenu == 0);
        Wii_DebugOverlaySetEnabled(shouldShowOverlay);
        if (shouldShowOverlay) {
            if (showSettingsMenu) {
                DrawSettingsOverlay();
            }
            else if (showFileBrowser) {
                Wii_DebugOverlayPrint(0, "Dir: %s", currentDir.c_str());

                int lineIndex = 1;
                for (int i = displayOffset;
                     i < std::min((int)dirContents.size(), displayOffset + 5); i++) {
                    const char* prefix = (i == selectedItemIndex) ? "-> " : "   ";
                    const char* suffix = dirContents[i].isDirectory ? "/" : "";
                    Wii_DebugOverlayPrint(lineIndex++, "%s%s%s",
                        prefix, dirContents[i].name.c_str(), suffix);
                }
                while (lineIndex < 6)
                    Wii_DebugOverlayPrint(lineIndex++, " ");
                Wii_DebugOverlayPrint(6, "A=Load  B=Up  X/-=Settings");
                Wii_DebugOverlayPrint(7, "%s", g_browserNotice.empty() ? " " : g_browserNotice.c_str());
            }
            else if (romLoading) {
                Wii_DebugOverlayPrint(0, "Loading ROM: %s", g_lastLoadedName.c_str());
                Wii_DebugOverlayPrint(1, "Please wait...");
                Wii_DebugOverlayPrint(2, " ");
                Wii_DebugOverlayPrint(3, " ");
                Wii_DebugOverlayPrint(4, " ");
                Wii_DebugOverlayPrint(5, " ");
                Wii_DebugOverlayPrint(6, " ");
                Wii_DebugOverlayPrint(7, " ");
            }
            else {
                Wii_DebugOverlayPrint(0, " ");
                // Present FPS excludes frames skipped by the GPU; Core::fps counts
                // every emulated video frame. Show both to avoid conflating them.
                Wii_DebugOverlayPrint(1, "Display: %5.1f  Emu: %d FPS", perf.fps,
                                      ndsCore ? ndsCore->fps : 0);
                Wii_DebugOverlayPrint(2, "HOME=Exit  B+/B+Start=Menu");
                Wii_DebugOverlayPrint(3, " ");
                Wii_DebugOverlayPrint(4, " ");
                Wii_DebugOverlayPrint(5, " ");
                Wii_DebugOverlayPrint(6, " ");
                Wii_DebugOverlayPrint(7, " ");
            }
        }

        static bool prevActiveRom = false;
        static float prevCursorX = -1.0f, prevCursorY = -1.0f;
        static bool prevCursorShow = false;
        static bool firstFrame = true;

        const bool activeRom = romLoaded && !showFileBrowser;
        bool isGba = (activeRom && ndsCore) ? ndsCore->gbaMode : false;
        bool modeChanged = (activeRom != prevActiveRom) || firstFrame;

        prevActiveRom = activeRom;
        firstFrame = false;

        uint64_t videoT0 = PPCGetTickCount();
        Wii_VideoRender(renderTop, renderBottom, isGba, haveFrame || modeChanged || !activeRom);
        uint64_t videoT1 = PPCGetTickCount();
        Wii_VideoFlushAsync();
        uint64_t videoT2 = PPCGetTickCount();
        VIDEO_WaitVSync();
        uint64_t videoT3 = PPCGetTickCount();
        videoRenderTicks += videoT1 - videoT0;
        videoFlushTicks += videoT2 - videoT1;
        videoVsyncTicks += videoT3 - videoT2;
        videoLoopCount++;
    }

    return 0;
}
