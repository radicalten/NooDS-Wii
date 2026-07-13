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
#include <asndlib.h>
#include <dirent.h>
#include <sys/types.h>
#include <vector>
#include <string>
#include <algorithm>

#include "wii_video.h"
#include "wii_audio.h"
#include "core.h"
#include "settings.h"
#include "console_ui.h"

extern "C" {
    #include <tuxedo/thread.h>
    #include <tuxedo/ppc/intrinsics.h>
    #include <tuxedo/ppc/clock.h>
}

void* ConsoleUI::createTexture(uint16_t* data, int width, int height) {
    return Wii_CreateTexture(data, width, height);
}

void* ConsoleUI::createTextureRGBA8(uint32_t* data, int width, int height) {
    return Wii_CreateTextureRGBA8(data, width, height);
}

void ConsoleUI::destroyTexture(void* texture) {
    Wii_DestroyTexture(texture);
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
    u8* lo = (u8*)SYS_GetArena2Lo();
    u8* hi = (u8*)SYS_GetArena2Hi();
    mem2_free_ptr  = (u8*)(((uptr)lo + 31) & ~31u);
    mem2_max_limit = (u8*)((uptr)hi  & ~31u);
}

void* Noods_MEM2_Alloc(size_t size) {
    if (size == 0) return nullptr;
    size = (size + 31) & ~31u;
    if (!mem2_free_ptr || (mem2_free_ptr + size > mem2_max_limit))
        return nullptr;
    void* p       = (void*)mem2_free_ptr;
    mem2_free_ptr += size;
    return p;
}

void Noods_MEM2_Free(void* /*ptr*/) {}

static inline bool IsMem2Ptr(void* p) {
    return (u8*)p >= (u8*)0x90000000 &&
           (u8*)p <  (u8*)0x93400000;
}

static void* GlobalAlloc(size_t size) {
    void* p = nullptr;
    if (g_emulatorThreadHandle &&
        KThreadGetSelf() == g_emulatorThreadHandle && size >= 4096) {
        PPCIrqState st = PPCIrqLockByMsr();
        p = Noods_MEM2_Alloc(size);
        PPCIrqUnlockByMsr(st);
    }
    if (!p) p = malloc(size);
    return p;
}

void* operator new(size_t size) {
    void* p = GlobalAlloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t size) {
    void* p = GlobalAlloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept {
    if (!p) return;
    if (!IsMem2Ptr(p)) free(p);
}
void operator delete[](void* p) noexcept {
    if (!p) return;
    if (!IsMem2Ptr(p)) free(p);
}
void operator delete(void* p, size_t) noexcept {
    if (!p) return;
    if (!IsMem2Ptr(p)) free(p);
}
void operator delete[](void* p, size_t) noexcept {
    if (!p) return;
    if (!IsMem2Ptr(p)) free(p);
}

static Core*     ndsCore   = nullptr;
static bool      romLoaded = false;

// Direct 16-bit RGB5A3 buffers
static uint16_t* frontBuffer = nullptr;
static uint16_t* backBuffer  = nullptr;

static volatile bool newFrameReady     = false;
static volatile bool runEmulatorThread = true;
static volatile bool runAudioThread    = true;

static uint16_t* topScreenBuffer    = nullptr;
static uint16_t* bottomScreenBuffer = nullptr;

float g_cursorX    = 320.0f;
float g_cursorY    = 240.0f;
bool  g_cursorShow = false;

struct BrowserItem {
    std::string name;
    bool isDirectory;
};

static std::string currentDir = "sd:/noods/";
static std::vector<BrowserItem> dirContents;
static int selectedItemIndex = 0;
static int displayOffset     = 0;
static bool showFileBrowser  = true;
static std::string romToLoadPath = "";
static volatile bool triggerRomLoad = false;

static int scrollHoldTimer = 0;
static const int SCROLL_DELAY_INITIAL = 18; 
static const int SCROLL_DELAY_REPEATED = 3;  

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

alignas(32) static int16_t s_audioDbl[2][WIIAUD_FRAMES_PER_BUF * 2];
alignas(32) static int16_t s_audioSilence[WIIAUD_FRAMES_PER_BUF * 2];

static volatile int  s_writeBuf = 0;    
static volatile int  s_readBuf  = 1;    
static volatile bool s_bufReady = false;

static void ResampleLinear(const uint32_t* src, int nSrc,
                            int16_t*        dst, int nDst,
                            bool            mono)
{
    const int32_t step = (int32_t)(((int64_t)nSrc << 16) / nDst);
    int32_t       pos  = 0;

    for (int i = 0; i < nDst; i++) {
        const int idx0 = pos >> 16;
        const int idx1 = (idx0 + 1 < nSrc) ? idx0 + 1 : nSrc - 1;

        const int32_t frac = pos & 0xFFFF;

        const int16_t l0 = (int16_t)( src[idx0]        & 0xFFFF);
        const int16_t r0 = (int16_t)((src[idx0] >> 16) & 0xFFFF);
        const int16_t l1 = (int16_t)( src[idx1]        & 0xFFFF);
        const int16_t r1 = (int16_t)((src[idx1] >> 16) & 0xFFFF);

        const int16_t outL = (int16_t)(l0 + (int32_t)(l1 - l0) * frac / 65536);
        const int16_t outR = (int16_t)(r0 + (int32_t)(r1 - r0) * frac / 65536);

        if (mono) {
            const int16_t m = (int16_t)(((int32_t)outL + (int32_t)outR) >> 1);
            dst[i * 2 + 0] = m;
            dst[i * 2 + 1] = m;
        } else {
            dst[i * 2 + 0] = outL;
            dst[i * 2 + 1] = outR;
        }

        pos += step;
    }
}

static uint32_t* PullSpuSamples(int nFrames)
{
#if WIIAUD_GBA_FRAMES > WIIAUD_NDS_FRAMES
    static uint32_t staging[WIIAUD_GBA_FRAMES];
#else
    static uint32_t staging[WIIAUD_NDS_FRAMES];
#endif

    uint32_t* p = ndsCore->spu.getSamples(nFrames);
    if (!p) return nullptr;

    memcpy(staging, p, (size_t)nFrames * sizeof(uint32_t));
    delete[] p;        
    return staging;
}

static void AudioCallback(s32 voice)
{
    if (s_bufReady) {
        const int justFilled = s_writeBuf;
        s_readBuf            = justFilled;
        s_writeBuf           = justFilled ^ 1;
        s_bufReady           = false;

        ASND_AddVoice(voice,
                      s_audioDbl[justFilled],
                      WIIAUD_BUF_BYTES_STEREO);
    } else {
        ASND_AddVoice(voice, s_audioSilence, WIIAUD_BUF_BYTES_STEREO);
    }
}

static sptr AudioThreadMain(void* /*arg*/)
{
    while (runAudioThread) {
        while (s_bufReady && runAudioThread) {
            KThreadYield();
        }
        if (!runAudioThread) break;

        int16_t* dst  = s_audioDbl[s_writeBuf];
        bool     ok   = false;
        const bool mono = (Settings::monoAudio != 0);

        if (romLoaded && ndsCore) {
            if (ndsCore->gbaMode) {
                uint32_t* src = PullSpuSamples(WIIAUD_GBA_FRAMES);
                if (src) {
                    ResampleLinear(src, WIIAUD_GBA_FRAMES,
                                   dst, WIIAUD_FRAMES_PER_BUF, mono);
                    ok = true;
                }
            } else {
                uint32_t* src = PullSpuSamples(WIIAUD_NDS_FRAMES);
                if (src) {
                    ResampleLinear(src, WIIAUD_NDS_FRAMES,
                                   dst, WIIAUD_FRAMES_PER_BUF, mono);
                    ok = true;
                }
            }
        }
        if (!ok) {
            memset(dst, 0, WIIAUD_BUF_BYTES_STEREO);
        }
        DCFlushRange(dst, WIIAUD_BUF_BYTES_STEREO);
        s_bufReady = true;
    }
    KThreadExit(0);
    return 0;
}

static void InitializeAudio()
{
    memset(s_audioDbl,     0, sizeof(s_audioDbl));
    memset(s_audioSilence, 0, sizeof(s_audioSilence));
    DCFlushRange(s_audioDbl,     sizeof(s_audioDbl));
    DCFlushRange(s_audioSilence, sizeof(s_audioSilence));

    s_writeBuf = 0;
    s_readBuf  = 1;
    s_bufReady = false;

    ASND_Init();
    ASND_Pause(0);

    ASND_SetVoice(0,
                  VOICE_STEREO_16BIT_BE,
                  WIIAUD_OUT_RATE,
                  0,
                  s_audioSilence,
                  WIIAUD_BUF_BYTES_STEREO,
                  MAX_VOLUME,
                  MAX_VOLUME,
                  AudioCallback);
}

static void ShutdownAudio()
{
    runAudioThread = false;
    s_bufReady = false;       
    ASND_StopVoice(0);
    ASND_End();
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
    Settings::frameskip    = 2;
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

    DIR* dir = opendir(path.c_str());
    if (!dir) return;

    if (path != "sd:/" && path != "sd:") {
        BrowserItem parent;
        parent.name        = "..";
        parent.isDirectory = true;
        dirContents.push_back(parent);
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        BrowserItem item;
        item.name        = entry->d_name;
        item.isDirectory = (entry->d_type == DT_DIR);

        if (!item.isDirectory) {
            size_t extPos = item.name.find_last_of('.');
            if (extPos == std::string::npos) continue;
            std::string ext = item.name.substr(extPos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".nds" && ext != ".gba") continue;
        }

        dirContents.push_back(item);
    }
    closedir(dir);

    size_t sortStart = (path != "sd:/" && path != "sd:") ? 1 : 0;
    std::sort(dirContents.begin() + sortStart, dirContents.end(),
              CompareBrowserItems);
}

// 16-bit intermediate frame composite buffer
alignas(32) static uint16_t tempBuffer[NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 2];

static sptr EmulatorThreadMain(void* /*arg*/) {
    const size_t pixelCount = NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 2;

    while (runEmulatorThread) {
        if (triggerRomLoad) {
            std::string loadPath;
            {
                PPCIrqState st = PPCIrqLockByMsr();
                loadPath       = romToLoadPath;
                triggerRomLoad = false;
                romLoaded      = false;
                PPCIrqUnlockByMsr(st);
            }

            delete ndsCore;
            ndsCore = nullptr;

            std::string ndsRom, gbaRom;
            size_t extPos = loadPath.find_last_of('.');
            if (extPos != std::string::npos) {
                std::string ext = loadPath.substr(extPos);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".gba") gbaRom = loadPath;
                else               ndsRom = loadPath;
            }

            try {
                ndsCore   = new Core(ndsRom, gbaRom);
                romLoaded = true;
            }
            catch (...) {
                ndsCore = nullptr;
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

            ndsCore->runCore();

            // Get frame outputs directly in 16-bit RGB5A3
            if (ndsCore->gpu.getFrame(tempBuffer, ndsCore->gbaMode)) {
                PPCIrqState st = PPCIrqLockByMsr();
                // We copy 16-bit elements instead of 32-bit (Cutting copy bandwidth in half!)
                memcpy(backBuffer, tempBuffer, pixelCount * sizeof(uint16_t));
                newFrameReady = true;
                PPCIrqUnlockByMsr(st);
            }
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
        NDS_SCREEN_WIDTH * (NDS_SCREEN_HEIGHT * 2) * sizeof(uint16_t);

    frontBuffer = (uint16_t*)Noods_MEM2_Alloc(bufSize);
    if (!frontBuffer) frontBuffer = (uint16_t*)malloc(bufSize);

    backBuffer  = (uint16_t*)Noods_MEM2_Alloc(bufSize);
    if (!backBuffer) backBuffer = (uint16_t*)malloc(bufSize);

    if (!frontBuffer || !backBuffer) {
        while (true) KThreadSleepMs(16);
    }
    memset(frontBuffer, 0, bufSize);
    memset(backBuffer,  0, bufSize);

    InitializeSettings();

    if (fatInitDefault()) {
        UpdateFileBrowser(currentDir);
    }

    const size_t sz = NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * sizeof(uint16_t);
    topScreenBuffer    = (uint16_t*)Noods_MEM2_Alloc(sz);
    if (!topScreenBuffer) topScreenBuffer = (uint16_t*)malloc(sz);

    bottomScreenBuffer = (uint16_t*)Noods_MEM2_Alloc(sz);
    if (!bottomScreenBuffer) bottomScreenBuffer = (uint16_t*)malloc(sz);

    if (topScreenBuffer && bottomScreenBuffer) {
        // Direct RGB5A3 representation
        const uint16_t red    = 0xFC00u; // Opaque pure red
        const uint16_t yellow = 0xFFE0u; // Opaque pure yellow

        for (int i = 0; i < NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT; i++) {
            topScreenBuffer[i]    = red;
            bottomScreenBuffer[i] = yellow;
        }
    }
    
    emulatorThreadStack = (u8*)Noods_MEM2_Alloc(EMULATION_STACK_SIZE);
    if (!emulatorThreadStack) emulatorThreadStack = (u8*)malloc(EMULATION_STACK_SIZE);

    if (Settings::emulateAudio) {
        audioThreadStack = (u8*)Noods_MEM2_Alloc(AUDIO_STACK_SIZE);
        if (!audioThreadStack) audioThreadStack = (u8*)malloc(AUDIO_STACK_SIZE);
    }

    if (!emulatorThreadStack || (Settings::emulateAudio && !audioThreadStack)) {
        while (true) KThreadSleepMs(16);
    }

    KThreadPrepare(&emulatorThread, EmulatorThreadMain, nullptr,
                   emulatorThreadStack + EMULATION_STACK_SIZE, 0x50);
    g_emulatorThreadHandle = &emulatorThread;
    KThreadResume(&emulatorThread);

    if (Settings::emulateAudio) {
        KThreadPrepare(&audioThread, AudioThreadMain, nullptr,
                       audioThreadStack + AUDIO_STACK_SIZE, 0x51);
        KThreadResume(&audioThread);
        InitializeAudio();
    }

    perf.startTime = time(nullptr);
}

static uint16_t WiiButtonsToNDS(u32 held, u32 heldExt, bool hasNunchuk, bool hasClassic) {
    uint16_t nds = 0;

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

    if (hasNunchuk) {
        if (heldExt & WPAD_NUNCHUK_BUTTON_Z) nds |= (1 << NDS_KEY_L);
        if (heldExt & WPAD_NUNCHUK_BUTTON_C) nds |= (1 << NDS_KEY_R);
    }

    if (hasClassic) {
        if (held & WPAD_CLASSIC_BUTTON_RIGHT) nds |= (1 << NDS_KEY_RIGHT);
        if (held & WPAD_CLASSIC_BUTTON_LEFT)  nds |= (1 << NDS_KEY_LEFT);
        if (held & WPAD_CLASSIC_BUTTON_UP)    nds |= (1 << NDS_KEY_UP);
        if (held & WPAD_CLASSIC_BUTTON_DOWN)  nds |= (1 << NDS_KEY_DOWN);
        if (held & WPAD_CLASSIC_BUTTON_A)     nds |= (1 << NDS_KEY_A);
        if (held & WPAD_CLASSIC_BUTTON_B)     nds |= (1 << NDS_KEY_B);
        if (held & WPAD_CLASSIC_BUTTON_X)     nds |= (1 << NDS_KEY_X);
        if (held & WPAD_CLASSIC_BUTTON_Y)     nds |= (1 << NDS_KEY_Y);
        if (held & WPAD_CLASSIC_BUTTON_PLUS)  nds |= (1 << NDS_KEY_START);
        if (held & WPAD_CLASSIC_BUTTON_MINUS) nds |= (1 << NDS_KEY_SELECT);
        if (held & (WPAD_CLASSIC_BUTTON_FULL_L | WPAD_CLASSIC_BUTTON_ZL)) nds |= (1 << NDS_KEY_L);
        if (held & (WPAD_CLASSIC_BUTTON_FULL_R | WPAD_CLASSIC_BUTTON_ZR)) nds |= (1 << NDS_KEY_R);
    }

    return nds;
}

static void ScanWiiInputs() {
    WPAD_ScanPads();
    PAD_ScanPads();

    u32 held      = WPAD_ButtonsHeld(0);
    u32 pressed   = WPAD_ButtonsDown(0);
    u32 gcHeld    = PAD_ButtonsHeld(0);
    u32 gcPressed = PAD_ButtonsDown(0);

    WPADData* wdata      = WPAD_Data(0);
    bool      hasNunchuk = wdata && (wdata->exp.type == WPAD_EXP_NUNCHUK);
    bool      hasClassic = wdata && (wdata->exp.type == WPAD_EXP_CLASSIC);
    u32       heldExt    = hasNunchuk ? held : 0;

    if ((pressed & WPAD_BUTTON_HOME) || (pressed & WPAD_CLASSIC_BUTTON_HOME)) {
        runEmulatorThread = false;
        KThreadJoin(&emulatorThread);
        ShutdownAudio();
        delete ndsCore;
        ndsCore = nullptr;

        exit(0);
    }

    if (showFileBrowser) {
        if (!dirContents.empty()) {
            static bool classicLStickUpLast = false;
            static bool classicLStickDownLast = false;
            bool classicLStickUp = false;
            bool classicLStickDown = false;

            if (hasClassic && wdata) {
                float ly = wdata->exp.classic.ljs.mag * cosf(wdata->exp.classic.ljs.ang * 3.14159265f / 180.0f);
                if (wdata->exp.classic.ljs.mag > 0.5f) {
                    if (ly > 0.5f)  classicLStickUp = true;
                    if (ly < -0.5f) classicLStickDown = true;
                }
            }

            bool upHeld   = (held & WPAD_BUTTON_UP)     || (held & WPAD_CLASSIC_BUTTON_UP)     || (gcHeld & PAD_BUTTON_UP)   || classicLStickUp;
            bool downHeld = (held & WPAD_BUTTON_DOWN)   || (held & WPAD_CLASSIC_BUTTON_DOWN)   || (gcHeld & PAD_BUTTON_DOWN) || classicLStickDown;
            bool upDown   = (pressed & WPAD_BUTTON_UP)  || (pressed & WPAD_CLASSIC_BUTTON_UP)  || (gcPressed & PAD_BUTTON_UP)  || (classicLStickUp && !classicLStickUpLast);
            bool downDown = (pressed & WPAD_BUTTON_DOWN)|| (pressed & WPAD_CLASSIC_BUTTON_DOWN)|| (gcPressed & PAD_BUTTON_DOWN)|| (classicLStickDown && !classicLStickDownLast);

            classicLStickUpLast = classicLStickUp;
            classicLStickDownLast = classicLStickDown;

            bool performUp    = false;
            bool performDown  = false;

            if (upDown || downDown) {
                scrollHoldTimer = 0; 
                if (upDown)   performUp   = true;
                if (downDown) performDown = true;
            } else if (upHeld || downHeld) {
                scrollHoldTimer++;
                if (scrollHoldTimer >= SCROLL_DELAY_INITIAL) {
                    if ((scrollHoldTimer - SCROLL_DELAY_INITIAL) % SCROLL_DELAY_REPEATED == 0) {
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
                    selectedItemIndex = 0;
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

            if ((pressed & WPAD_BUTTON_LEFT) || (pressed & WPAD_CLASSIC_BUTTON_LEFT) || (gcPressed & PAD_BUTTON_LEFT)) {
                selectedItemIndex -= 5;
                if (selectedItemIndex < 0) {
                    selectedItemIndex = 0;
                }
                displayOffset = std::max(0, selectedItemIndex - 2);
            }
            if ((pressed & WPAD_BUTTON_RIGHT) || (pressed & WPAD_CLASSIC_BUTTON_RIGHT) || (gcPressed & PAD_BUTTON_RIGHT)) {
                selectedItemIndex += 5;
                if (selectedItemIndex >= (int)dirContents.size()) {
                    selectedItemIndex = (int)dirContents.size() - 1;
                }
                displayOffset = std::max(0, std::min((int)dirContents.size() - 5, selectedItemIndex - 2));
            }

            if ((pressed & WPAD_BUTTON_A) || (pressed & WPAD_CLASSIC_BUTTON_A) || (gcPressed & PAD_BUTTON_A)) {
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
                    PPCIrqState st = PPCIrqLockByMsr();
                    romToLoadPath  = romPath;
                    triggerRomLoad = true;
                    PPCIrqUnlockByMsr(st);

                    showFileBrowser = false;
                    
                    dirContents.clear();
                    dirContents.shrink_to_fit();
                }
            }
        }

        if ((pressed & WPAD_BUTTON_B) || (pressed & WPAD_CLASSIC_BUTTON_B) || (gcPressed & PAD_BUTTON_B)) {
            if (currentDir != "sd:/" && currentDir != "sd://" && currentDir != "sd:") {
                size_t slash = currentDir.find_last_of('/', currentDir.length() - 2);
                if (slash != std::string::npos) {
                    currentDir = currentDir.substr(0, slash + 1);
                    UpdateFileBrowser(currentDir);
                }
            } else if (romLoaded) {
                showFileBrowser = false;
            }
        }
    }
    else {
        uint16_t ndsButtons = WiiButtonsToNDS(held, heldExt, hasNunchuk, hasClassic);

        if (gcHeld & PAD_BUTTON_A)     ndsButtons |= (1 << NDS_KEY_A);
        if (gcHeld & PAD_BUTTON_B)     ndsButtons |= (1 << NDS_KEY_B);
        if (gcHeld & PAD_BUTTON_X)     ndsButtons |= (1 << NDS_KEY_X);
        if (gcHeld & PAD_BUTTON_Y)     ndsButtons |= (1 << NDS_KEY_Y);
        if (gcHeld & PAD_BUTTON_LEFT)  ndsButtons |= (1 << NDS_KEY_LEFT);
        if (gcHeld & PAD_BUTTON_RIGHT) ndsButtons |= (1 << NDS_KEY_RIGHT);
        if (gcHeld & PAD_BUTTON_UP)    ndsButtons |= (1 << NDS_KEY_UP);
        if (gcHeld & PAD_BUTTON_DOWN)  ndsButtons |= (1 << NDS_KEY_DOWN);
        if (gcHeld & PAD_TRIGGER_R)    ndsButtons |= (1 << NDS_KEY_R);
        if (gcHeld & PAD_TRIGGER_L)    ndsButtons |= (1 << NDS_KEY_L);
        if (gcHeld & PAD_BUTTON_START) ndsButtons |= (1 << NDS_KEY_START);

        if (hasClassic && wdata) {
            float lx = wdata->exp.classic.ljs.mag * sinf(wdata->exp.classic.ljs.ang * 3.14159265f / 180.0f);
            float ly = wdata->exp.classic.ljs.mag * cosf(wdata->exp.classic.ljs.ang * 3.14159265f / 180.0f);
            if (wdata->exp.classic.ljs.mag > 0.5f) {
                if (lx > 0.5f)  ndsButtons |= (1 << NDS_KEY_RIGHT);
                if (lx < -0.5f) ndsButtons |= (1 << NDS_KEY_LEFT);
                if (ly > 0.5f)  ndsButtons |= (1 << NDS_KEY_UP);
                if (ly < -0.5f) ndsButtons |= (1 << NDS_KEY_DOWN);
            }
        }

        bool isTouching = false;
        g_cursorShow = false;

        if (wdata && wdata->ir.valid) {
            g_cursorX    = wdata->ir.x;
            g_cursorY    = wdata->ir.y;
            g_cursorShow = true;
            if (held & WPAD_BUTTON_A)
                isTouching = true;
        }

        s8 cstickX = PAD_SubStickX(0);
        s8 cstickY = PAD_SubStickY(0);
        if (abs(cstickX) > 15 || abs(cstickY) > 15) {
            g_cursorX   += (cstickX / 15.0f) * 2.5f;
            g_cursorY   -= (cstickY / 15.0f) * 2.5f;
            g_cursorShow = true;

            if (g_cursorX < 0.0f)   g_cursorX = 0.0f;
            if (g_cursorX > 640.0f) g_cursorX = 640.0f;
            if (g_cursorY < 0.0f)   g_cursorY = 0.0f;
            if (g_cursorY > 480.0f) g_cursorY = 480.0f;
        }

        if (hasClassic && wdata) {
            float rx = wdata->exp.classic.rjs.mag * sinf(wdata->exp.classic.rjs.ang * 3.14159265f / 180.0f);
            float ry = wdata->exp.classic.rjs.mag * cosf(wdata->exp.classic.rjs.ang * 3.14159265f / 180.0f);
            if (wdata->exp.classic.rjs.mag > 0.15f) {
                g_cursorX   += rx * 4.0f;
                g_cursorY   -= ry * 4.0f;
                g_cursorShow = true;

                if (g_cursorX < 0.0f)   g_cursorX = 0.0f;
                if (g_cursorX > 640.0f) g_cursorX = 640.0f;
                if (g_cursorY < 0.0f)   g_cursorY = 0.0f;
                if (g_cursorY > 480.0f) g_cursorY = 480.0f;
            }
        }

        if (gcHeld & PAD_TRIGGER_Z) {
            isTouching   = true;
            g_cursorShow = true;
        }

        if (hasClassic && (held & (WPAD_CLASSIC_BUTTON_ZL | WPAD_CLASSIC_BUTTON_ZR))) {
            isTouching   = true;
            g_cursorShow = true;
        }

        int16_t touchX = 0;
        int16_t touchY = 0;

        if (g_cursorShow) {
            const int gap     = 2;
            const int scrW    = NDS_SCREEN_WIDTH;
            const int scrH    = NDS_SCREEN_HEIGHT;
            const int totalW  = scrW;
            const int totalH  = scrH * 2 + gap;

            const float fbWidth   = 640.0f;
            const float efbHeight = 480.0f;

            const float originX = (fbWidth  - totalW) / 2.0f;
            const float originY = (efbHeight - totalH) / 2.0f;

            const float dsLeft   = originX;
            const float dsRight  = originX + scrW;
            const float dsTop    = originY + scrH + gap;
            const float dsBottom = originY + scrH + gap + scrH;

            if (g_cursorX >= dsLeft  && g_cursorX <= dsRight &&
                g_cursorY >= dsTop   && g_cursorY <= dsBottom) {
                touchX = (int16_t)(((g_cursorX - dsLeft)  / (dsRight  - dsLeft))  * 256.0f);
                touchY = (int16_t)(((g_cursorY - dsTop)   / (dsBottom - dsTop))   * 192.0f);
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
    }
}

int main(int /*argc*/, char** /*argv*/) {
    Wii_VideoInit();
    InitializeMem2Arena();

    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_0, 640, 480);

    InitializeNDS();

    while (true) {
        ScanWiiInputs();

        // 16-bit rendering references
        const uint16_t* renderTop    = nullptr;
        const uint16_t* renderBottom = nullptr;

        if (romLoaded && !showFileBrowser) {
            PPCIrqState st  = PPCIrqLockByMsr();
            bool haveFrame  = newFrameReady;
            if (haveFrame) {
                uint16_t* tmp = frontBuffer;
                frontBuffer   = backBuffer;
                backBuffer    = tmp;
                newFrameReady = false;
                perf.renderFrameCount++;
            }
            PPCIrqUnlockByMsr(st);

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
        static time_t   lastSec = 0;
        time_t now = time(nullptr);
        if (now != lastSec) {
                perf.fps = (float)(perf.renderFrameCount - lastSecFrames);
                lastSecFrames = perf.renderFrameCount;
                lastSec = now;
        }

        if (showFileBrowser) {
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
            Wii_DebugOverlayPrint(6, "D-Pad=Select  L/R=Page Skip  A=Load  B=Back");
            Wii_DebugOverlayPrint(7, " ");
        }
        else {
            Wii_DebugOverlayPrint(0, " ");
            Wii_DebugOverlayPrint(1, "FPS: %5.1f", perf.fps);
            Wii_DebugOverlayPrint(2, "HOME=Quit");
            Wii_DebugOverlayPrint(3, " ");
            Wii_DebugOverlayPrint(4, " ");
            Wii_DebugOverlayPrint(5, " ");
            Wii_DebugOverlayPrint(6, " ");
            Wii_DebugOverlayPrint(7, " ");
        }

        bool isGba = (ndsCore && romLoaded && !showFileBrowser) ? ndsCore->gbaMode : false;
        Wii_VideoRender(renderTop, renderBottom, isGba);
        Wii_VideoFlushAsync();
        VIDEO_WaitVSync();
    }

    return 0;
}
