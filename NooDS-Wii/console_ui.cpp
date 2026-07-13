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
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <gccore.h>

extern "C" {
    #include <tuxedo/thread.h>
    #include <tuxedo/ppc/intrinsics.h>
}

#include "console_ui.h"
#include "nds_icon.h"
#include "settings.h"

#define SCALEH(x, h) (((x) * (h)) / 720)
#define SCALE(x) SCALEH(x, uiHeight)

extern uint8_t _binary_src_console_images_file_dark_bmp_start;
extern uint8_t _binary_src_console_images_file_light_bmp_start;
extern uint8_t _binary_src_console_images_folder_dark_bmp_start;
extern uint8_t _binary_src_console_images_folder_light_bmp_start;
extern uint8_t _binary_src_console_images_font_bmp_start;

void *ConsoleUI::fileTextures[2];
void *ConsoleUI::folderTextures[2];
void *ConsoleUI::fontTexture;

const uint32_t *ConsoleUI::palette;
uint32_t ConsoleUI::uiWidth, ConsoleUI::uiHeight;
uint32_t ConsoleUI::lineHeight;
bool ConsoleUI::touchMode;

Core *ConsoleUI::core;
volatile uint8_t ConsoleUI::running;
std::string ConsoleUI::ndsPath, ConsoleUI::gbaPath;
std::string ConsoleUI::basePath, ConsoleUI::curPath;

// Native RGB5A3 framebuffer — 256×192×8 uint16_t halfwords
uint16_t ConsoleUI::framebuffer[256 * 192 * 8];

ScreenLayout ConsoleUI::layout;
bool ConsoleUI::gbaMode;
bool ConsoleUI::changed;

KThread ConsoleUI::coreThreadObj;
KThread ConsoleUI::saveThreadObj;
alignas(32) static uint8_t coreThreadStack[16384];
alignas(32) static uint8_t saveThreadStack[16384];

KThrQueue ConsoleUI::saveCondQueue;
int ConsoleUI::fpsLimiterBackup = 0;

int ConsoleUI::showFpsCounter = 0;
int ConsoleUI::menuTheme = 0;
int ConsoleUI::keyBinds[] = {};

const uint32_t ConsoleUI::themeColors[] = {
    0xFF2D2D2D, 0xFFFFFFFF, 0xFF4B4B4B, 0xFF232323,
    0xFFE1B955, 0xFFC8FF00, 0xFFA2A2A2, // Dark
    0xFFEBEBEB, 0xFF2D2D2D, 0xFFCDCDCD, 0xFFFFFFFF,
    0xFFD2D732, 0xFFF05032, 0xFF727272  // Light
};

const uint8_t ConsoleUI::charWidths[] = {
    11, 9, 11, 20, 18, 28, 24, 7, 12, 12,
    14, 24, 9, 12, 9, 16, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21, 9, 9, 26, 24,
    26, 18, 28, 24, 21, 24, 26, 20, 20, 27,
    23, 9, 17, 21, 16, 31, 27, 29, 19, 29,
    20, 18, 21, 26, 24, 37, 21, 21, 24, 12,
    16, 12, 18, 16, 9, 20, 21, 18, 21, 20,
    10, 20, 20, 8, 12, 19, 9, 30, 20, 21,
    21, 21, 12, 16, 12, 20, 17, 29, 17, 17,
    16, 9, 8, 9, 12, 0, 40, 40, 40, 40
};

void ConsoleUI::drawRectangle(float x, float y, float w, float h,
    uint32_t color) {
    // Single white pixel as a 1×1 RGBA8 texture for UI rectangles
    static uint32_t data    = 0xFFFFFFFF;
    static void    *texture = createTextureRGBA8(&data, 1, 1);
    drawTexture(texture, 0, 0, 1, 1, x, y, w, h, false, 0, color);
}

void ConsoleUI::drawString(std::string string, float x, float y,
    float size, uint32_t color, bool alignRight) {
    float offset = alignRight ? -stringWidth(string) : 0;
    for (uint32_t i = 0; i < string.size(); i++) {
        float x1 = x + offset * size / 48;
        float tx  = 48.0f * (((uint8_t)string[i] - 32) % 10);
        float ty  = 48.0f * (((uint8_t)string[i] - 32) / 10);
        drawTexture(fontTexture, tx, ty, 47, 47,
            x1, y, size, size, true, 0, color);
        offset += charWidths[(uint8_t)string[i] - 32];
    }
}

uint32_t ConsoleUI::getInputPress() {
    static uint32_t buttons = 0;
    uint32_t held    = getInputHeld();
    uint32_t pressed = held & ~buttons;
    buttons = held;
    return pressed;
}

// bmpToTexture builds an RGBA8 uint32_t buffer from BMP data and uploads
// it as a UI texture (icons, font sheet, etc.) — NOT an emulator screen.
void *ConsoleUI::bmpToTexture(uint8_t *bmp) {
    int width  = U8TO32(bmp, 0x12);
    int height = U8TO32(bmp, 0x16);
    uint32_t *data = new uint32_t[width * height];
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t *color =
                &bmp[0x46 + (((height - y - 1) * width + x) << 2)];
            data[y * width + x] =
                ((uint32_t)color[3] << 24) |
                ((uint32_t)color[0] << 16) |
                ((uint32_t)color[1] <<  8) |
                 (uint32_t)color[2];
        }
    }
    void *texture = createTextureRGBA8(data, width, height);
    delete[] data;
    return texture;
}

int ConsoleUI::stringWidth(std::string &string) {
    int width = 0;
    for (uint32_t i = 0; i < string.size(); i++)
        width += charWidths[(uint8_t)string[i] - 32];
    return width;
}

void ConsoleUI::initialize(int width, int height,
    std::string root, std::string prefix) {

    fileTextures[0]   =
        bmpToTexture(&_binary_src_console_images_file_dark_bmp_start);
    fileTextures[1]   =
        bmpToTexture(&_binary_src_console_images_file_light_bmp_start);
    folderTextures[0] =
        bmpToTexture(&_binary_src_console_images_folder_dark_bmp_start);
    folderTextures[1] =
        bmpToTexture(&_binary_src_console_images_folder_light_bmp_start);
    fontTexture       =
        bmpToTexture(&_binary_src_console_images_font_bmp_start);

    for (int i = 0; i < INPUT_MAX; i++)
        keyBinds[i] = defaultKeys[i];

    std::vector<Setting> platformSettings = {
        Setting("showFpsCounter",  &showFpsCounter,            false),
        Setting("menuTheme",       &menuTheme,                 false),
        Setting("keyA",            &keyBinds[INPUT_A],         false),
        Setting("keyB",            &keyBinds[INPUT_B],         false),
        Setting("keySelect",       &keyBinds[INPUT_SELECT],    false),
        Setting("keyStart",        &keyBinds[INPUT_START],     false),
        Setting("keyRight",        &keyBinds[INPUT_RIGHT],     false),
        Setting("keyLeft",         &keyBinds[INPUT_LEFT],      false),
        Setting("keyUp",           &keyBinds[INPUT_UP],        false),
        Setting("keyDown",         &keyBinds[INPUT_DOWN],      false),
        Setting("keyR",            &keyBinds[INPUT_R],         false),
        Setting("keyL",            &keyBinds[INPUT_L],         false),
        Setting("keyX",            &keyBinds[INPUT_X],         false),
        Setting("keyY",            &keyBinds[INPUT_Y],         false),
        Setting("keyMenu",         &keyBinds[INPUT_MENU],      false),
        Setting("keyFastHold",     &keyBinds[INPUT_FAST_HOLD], false),
        Setting("keyFastToggle",   &keyBinds[INPUT_FAST_TOGG], false),
        Setting("keyScreenSwap",   &keyBinds[INPUT_SCRN_SWAP], false)
    };

    ScreenLayout::addSettings();
    Settings::add(platformSettings);

    if (!Settings::load(prefix)) {
        ScreenLayout::screenArrangement = 2;
        Settings::save();
    }

    palette    = &themeColors[menuTheme * 7];
    uiWidth    = width;
    uiHeight   = height;
    lineHeight = height / 480;
    basePath   = curPath = root;
    changed    = true;

    memset(&saveCondQueue, 0, sizeof(saveCondQueue));
}

void ConsoleUI::mainLoop(MenuTouch (*specialTouch)(),
    ScreenLayout *touchLayout) {

    PPCIrqState st = PPCIrqLockByMsr();
    bool isRunning = running;
    PPCIrqUnlockByMsr(st);

    while (isRunning) {
        if (gbaMode != (core->gbaMode && ScreenLayout::gbaCrop)) {
            gbaMode = !gbaMode;
            changed = true;
        }

        if (changed) {
            layout.update(uiWidth, uiHeight, gbaMode);
            if (touchLayout)
                touchLayout->update(touchLayout->winWidth,
                    touchLayout->winHeight, gbaMode);
            changed = false;
        }

        // getFrame() now writes native RGB5A3 uint16_t pixels into
        // framebuffer[].  No hi-res ABGR8 path exists any more — the
        // 3D renderer already stores everything as RGB5A3.
        core->gpu.getFrame(framebuffer, gbaMode);
        startFrame(0);

        void *gbaTexture = nullptr;
        void *topTexture = nullptr;
        void *botTexture = nullptr;

        if (gbaMode) {
            // GBA: 240×160 RGB5A3 pixels at offset 0
            gbaTexture = createTexture(
                &framebuffer[0], 240, 160);
            drawTexture(gbaTexture, 0, 0, 240, 160,
                layout.topX, layout.topY,
                layout.topWidth, layout.topHeight,
                Settings::screenFilter,
                ScreenLayout::screenRotation);
        }
        else {
            // NDS: top screen starts at offset 0 (256×192 pixels)
            // Bottom screen follows immediately after
            // (256×192 uint16_t = 256*192 halfwords)
            const int screenPixels = 256 * 192;

            if (ScreenLayout::screenArrangement != 3 ||
                ScreenLayout::screenSizing < 2) {
                topTexture = createTexture(
                    &framebuffer[0], 256, 192);
                drawTexture(topTexture, 0, 0, 256, 192,
                    layout.topX, layout.topY,
                    layout.topWidth, layout.topHeight,
                    Settings::screenFilter,
                    ScreenLayout::screenRotation);
            }

            if (ScreenLayout::screenArrangement != 3 ||
                ScreenLayout::screenSizing == 2) {
                botTexture = createTexture(
                    &framebuffer[screenPixels], 256, 192);
                drawTexture(botTexture, 0, 0, 256, 192,
                    layout.botX, layout.botY,
                    layout.botWidth, layout.botHeight,
                    Settings::screenFilter,
                    ScreenLayout::screenRotation);
            }
        }

        if (showFpsCounter)
            drawString(std::to_string(core->fps) + " FPS",
                SCALE(5), 0, SCALE(48));

        uint32_t pressed = getInputPress();
        uint32_t held    = getInputHeld();

        for (int i = INPUT_A; i < INPUT_MENU; i++) {
            if (pressed & keyBinds[i])
                core->input.pressKey(i);
            else if (!(held & keyBinds[i]))
                core->input.releaseKey(i);
        }

        MenuTouch touch = getInputTouch();
        if (!touch.pressed && specialTouch)
            touch = (*specialTouch)();

        if (touch.pressed) {
            ScreenLayout *sl = touchLayout ? touchLayout : &layout;
            int touchX = sl->getTouchX(
                SCALEH(touch.x, sl->winHeight),
                SCALEH(touch.y, sl->winHeight));
            int touchY = sl->getTouchY(
                SCALEH(touch.x, sl->winHeight),
                SCALEH(touch.y, sl->winHeight));
            core->input.pressScreen();
            core->spi.setTouch(touchX, touchY);
        }
        else {
            core->input.releaseScreen();
            core->spi.clearTouch();
        }

        endFrame();
        if (gbaTexture) destroyTexture(gbaTexture);
        if (topTexture) destroyTexture(topTexture);
        if (botTexture) destroyTexture(botTexture);

        // Restore the FPS limiter when pausing or releasing fast-forward
        if ((fpsLimiterBackup &&
             (pressed & keyBinds[INPUT_MENU])) ||
            (uint32_t(fpsLimiterBackup - 1) < 0x100 &&
             !(held & keyBinds[INPUT_FAST_HOLD]))) {
            Settings::fpsLimiter = fpsLimiterBackup & 0xFF;
            fpsLimiterBackup     = 0;
        }

        if (pressed & keyBinds[INPUT_MENU]) {
            pauseMenu();
        }
        else if (pressed & keyBinds[INPUT_FAST_HOLD]) {
            if (Settings::fpsLimiter != 0) {
                fpsLimiterBackup   = Settings::fpsLimiter;
                Settings::fpsLimiter = 0;
            }
        }
        else if (pressed & keyBinds[INPUT_FAST_TOGG]) {
            if (Settings::fpsLimiter != 0) {
                fpsLimiterBackup   = Settings::fpsLimiter | 0x100;
                Settings::fpsLimiter = 0;
            }
            else if (fpsLimiterBackup != 0) {
                Settings::fpsLimiter = fpsLimiterBackup & 0xFF;
                fpsLimiterBackup     = 0;
            }
        }
        else if (pressed & keyBinds[INPUT_SCRN_SWAP]) {
            ScreenLayout::screenSizing =
                (ScreenLayout::screenSizing == 1) ? 2 : 1;
            changed = true;
        }

        st = PPCIrqLockByMsr();
        isRunning = running;
        PPCIrqUnlockByMsr(st);
    }
}

int ConsoleUI::setPath(std::string path) {
    if (path.find(".nds", path.length() - 4) != std::string::npos) {
        if (gbaPath != "") {
            if (!message("Loading NDS ROM",
                "Load the previous GBA ROM alongside this ROM?", 1))
                gbaPath = "";
        }
        ndsPath = path;
        if (createCore()) { startCore(); return 2; }
        ndsPath = "";
        return 1;
    }
    else if (path.find(".gba", path.length() - 4) != std::string::npos) {
        if (ndsPath != "") {
            if (!message("Loading GBA ROM",
                "Load the previous NDS ROM alongside this ROM?", 1))
                ndsPath = "";
        }
        gbaPath = path;
        if (createCore()) { startCore(); return 2; }
        gbaPath = "";
        return 1;
    }
    return 0;
}

uint32_t ConsoleUI::menu(std::string title, std::vector<MenuItem> &items,
    int &index, std::string actionX, std::string actionPlus) {

    if (actionPlus != "") actionPlus = "\x83 " + actionPlus + "     ";
    if (actionX    != "") actionX    = "\x82 " + actionX    + "     ";
    std::string actionB = "\x81 Back     ";
    std::string actionA = "\x80 OK";

    int boundsAB    = 1218 - (stringWidth(actionA) +
                       2.5f * charWidths[0]) * 34 / 48;
    int boundsBX    = boundsAB    - stringWidth(actionB)    * 34 / 48;
    int boundsXPlus = boundsBX    - stringWidth(actionX)    * 34 / 48;
    int boundsPlus  = boundsXPlus - stringWidth(actionPlus) * 34 / 48;

    bool upHeld = false, downHeld = false, scroll = false;
    std::chrono::steady_clock::time_point timeHeld;

    int  touchIndex   = 0;
    bool touchStarted = false, touchScroll = false;
    MenuTouch touchStart(false, 0, 0);

    index += items.empty() ? 0 : items[index].header;
    int min = items.empty() ? 0 : items[0].header;

    while (true) {
        startFrame(palette[0]);
        drawString(title, SCALE(72), SCALE(30), SCALE(42), palette[1]);
        drawRectangle(SCALE(30), SCALE(88),  SCALE(1220), lineHeight, palette[1]);
        drawRectangle(SCALE(30), SCALE(648), SCALE(1220), lineHeight, palette[1]);
        drawString(actionPlus + actionX + actionB + actionA,
            SCALE(1218), SCALE(667), SCALE(34), palette[1], true);

        uint32_t pressed = getInputPress();
        uint32_t held    = getInputHeld();

        if ((pressed & defaultKeys[INPUT_UP]) &&
            !(pressed & defaultKeys[INPUT_DOWN])) {
            if (touchMode) touchMode = false;
            else if (index > min) index--;
            index -= items.empty() ? 0 : items[index].header;
            upHeld   = true;
            timeHeld = std::chrono::steady_clock::now();
        }

        if ((pressed & defaultKeys[INPUT_DOWN]) &&
            !(pressed & defaultKeys[INPUT_UP])) {
            if (touchMode) touchMode = false;
            else if (index < (int)items.size() - 1) index++;
            index += items.empty() ? 0 : items[index].header;
            downHeld = true;
            timeHeld = std::chrono::steady_clock::now();
        }

        if (((pressed & defaultKeys[INPUT_A]) && !touchMode) ||
            (pressed & defaultKeys[INPUT_B]) ||
            (actionX    != "" && (pressed & defaultKeys[INPUT_X])) ||
            (actionPlus != "" && (pressed & defaultKeys[INPUT_START]))) {
            touchMode = false;
            return pressed;
        }

        if ((pressed & defaultKeys[INPUT_A]) && touchMode) {
            touchMode = false;
            index += items.empty() ? 0 : items[index].header;
        }

        if (upHeld   && !(held & defaultKeys[INPUT_UP]))
            { upHeld   = false; scroll = false; }
        if (downHeld && !(held & defaultKeys[INPUT_DOWN]))
            { downHeld = false; scroll = false; }

        if ((upHeld   && index > min) ||
            (downHeld && index < (int)items.size() - 1)) {
            std::chrono::duration<double> elapsed =
                std::chrono::steady_clock::now() - timeHeld;
            if (!scroll && elapsed.count() > 0.5f) scroll = true;
            if ( scroll && elapsed.count() > 0.1f) {
                index += (1 + (items.empty() ? 0 : items[index].header)) *
                         (upHeld ? -1 : 1);
                timeHeld = std::chrono::steady_clock::now();
            }
        }

        MenuTouch touch = getInputTouch();
        if (touch.pressed) {
            if (!touchStarted) {
                touchStart   = touch;
                touchStarted = true;
                touchScroll  = false;
                touchMode    = true;
            }
            if (touchScroll) {
                int newIndex = touchIndex +
                    (int)(touchStart.y - touch.y) / 70;
                if ((int)items.size() > 7 && newIndex != touchIndex)
                    index = std::max(3,
                        std::min<int>(items.size() - 4, newIndex));
            }
            else if (touch.x > touchStart.x + 25 ||
                     touch.x < touchStart.x - 25 ||
                     touch.y > touchStart.y + 25 ||
                     touch.y < touchStart.y - 25) {
                touchScroll = true;
                touchIndex  = std::max(3,
                    std::min<int>(items.size() - 4, index));
            }
        }
        else {
            if (!touchScroll && touchStart.y >= 650) {
                if (touchStart.x >= boundsBX &&
                    touchStart.x <  boundsAB)
                    return defaultKeys[INPUT_B];
                else if (touchStart.x >= boundsXPlus &&
                         touchStart.x <  boundsBX)
                    return defaultKeys[INPUT_X];
                else if (touchStart.x >= boundsPlus &&
                         touchStart.x <  boundsXPlus)
                    return defaultKeys[INPUT_START];
            }
            touchStarted = false;
        }

        if (!items.empty())
            drawRectangle(SCALE(90), SCALE(124),
                SCALE(1100), lineHeight, palette[2]);

        int size = std::min<int>(7, items.size());
        for (int i = 0, offset; i < size; i++) {
            if (index < 4 || (int)items.size() <= 7)
                offset = i;
            else if (index > (int)items.size() - 4)
                offset = (int)items.size() - 7 + i;
            else
                offset = i + index - 3;

            if (!items[offset].header &&
                !touchStarted && !touchScroll &&
                touchStart.x >= 90 && touchStart.x < 1190 &&
                touchStart.y >= 124 + i * 70 &&
                touchStart.y <  194 + i * 70) {
                index = offset;
                return defaultKeys[INPUT_A];
            }

            if (!touchMode && offset == index) {
                drawRectangle(SCALE(90),   SCALE(125+i*70),
                    SCALE(1100), SCALE(69),  palette[3]);
                drawRectangle(SCALE(89),   SCALE(121+i*70),
                    SCALE(1103), SCALE(5),   palette[4]);
                drawRectangle(SCALE(89),   SCALE(191+i*70),
                    SCALE(1103), SCALE(5),   palette[4]);
                drawRectangle(SCALE(88),   SCALE(122+i*70),
                    SCALE(5),    SCALE(73),  palette[4]);
                drawRectangle(SCALE(1188), SCALE(122+i*70),
                    SCALE(5),    SCALE(73),  palette[4]);
            }
            else {
                drawRectangle(SCALE(90), SCALE(194+i*70),
                    SCALE(1100), lineHeight, palette[2]);
            }

            if (items[offset].header) {
                drawString(items[offset].name,
                    SCALE(105), SCALE(160+i*70), SCALE(28), palette[6]);
                continue;
            }

            int x = (items[offset].iconSize > 0) ? 184 : 105;
            drawString(items[offset].name,
                SCALE(x), SCALE(140+i*70), SCALE(38), palette[1]);

            if (items[offset].iconSize > 0)
                drawTexture(items[offset].iconTex,
                    0, 0, items[offset].iconSize, items[offset].iconSize,
                    SCALE(105), SCALE(127+i*70), SCALE(64), SCALE(64));

            if (items[offset].setting != "")
                drawString(items[offset].setting,
                    SCALE(1175), SCALE(143+i*70),
                    SCALE(32), palette[5], true);
        }

        endFrame();
    }
}

uint32_t ConsoleUI::message(std::string title, std::string text, int type) {
    std::string actionB = "\x81 Back     ";
    std::string actionA = "\x80 OK";

    int boundsA  = 1218 + (int)(2.5f * charWidths[0]) * 34 / 48;
    int boundsAB = 1218 - (stringWidth(actionA) +
                   2.5f * charWidths[0]) * 34 / 48;
    int boundsB  = boundsAB - stringWidth(actionB) * 34 / 48;

    bool touchStarted = false, touchScroll = false;
    MenuTouch touchStart(false, 0, 0);

    while (true) {
        startFrame(palette[0]);
        drawString(title, SCALE(72), SCALE(30), SCALE(42), palette[1]);
        drawRectangle(SCALE(30), SCALE(88),  SCALE(1220), lineHeight, palette[1]);
        drawRectangle(SCALE(30), SCALE(648), SCALE(1220), lineHeight, palette[1]);
        if (type < 2)
            drawString((type ? actionB : "") + actionA,
                SCALE(1218), SCALE(667), SCALE(34), palette[1], true);

        for (int i = 0, j = 0, y = 0; j != (int)std::string::npos; y += 38) {
            j = (int)text.find("\n", i);
            drawString(text.substr(i, j - i),
                SCALE(90), SCALE(124 + y), SCALE(38), palette[1]);
            i = j + 1;
        }

        uint32_t pressed = getInputPress();
        if (pressed && type == 2)                               return pressed;
        else if (pressed & defaultKeys[INPUT_A])                return 1;
        else if ((pressed & defaultKeys[INPUT_B]) && type == 1) return 0;

        MenuTouch touch = getInputTouch();
        if (touch.pressed) {
            if (!touchStarted) {
                touchStart   = touch;
                touchStarted = true;
                touchScroll  = false;
                touchMode    = true;
            }
            if (touch.x > touchStart.x + 25 ||
                touch.x < touchStart.x - 25 ||
                touch.y > touchStart.y + 25 ||
                touch.y < touchStart.y - 25)
                touchScroll = true;
        }
        else {
            if (!touchScroll && touchStart.y >= 650) {
                if (touchStart.x >= boundsAB &&
                    touchStart.x <  boundsA  && type < 2)  return 1;
                else if (touchStart.x >= boundsB &&
                         touchStart.x <  boundsAB &&
                         type == 1)                         return 0;
            }
            touchStarted = false;
        }

        endFrame();
    }
}

void ConsoleUI::fileBrowser() {
    int index = 0;
    while (true) {
        std::vector<MenuItem> files;
        DIR    *dir = opendir(curPath.c_str());
        dirent *entry;

        while ((entry = readdir(dir))) {
            std::string name    = entry->d_name;
            std::string subpath = curPath + "/" + name;
            struct stat substat;
            stat(subpath.c_str(), &substat);

            if (S_ISDIR(substat.st_mode)) {
                files.push_back(MenuItem(name, "",
                    folderTextures[menuTheme], 64));
            }
            else if (name.find(".nds", name.length() - 4)
                     != std::string::npos) {
                // NDS icon: 32×32 RGBA8 pixels — use createTextureRGBA8
                void *texture = createTextureRGBA8(
                    NdsIcon(subpath).getIcon(), 32, 32);
                files.push_back(MenuItem(name, "", texture, 32));
            }
            else if (name.find(".gba", name.length() - 4)
                     != std::string::npos) {
                files.push_back(MenuItem(name, "",
                    fileTextures[menuTheme], 64));
            }
        }

        sort(files.begin(), files.end());
        closedir(dir);

        uint32_t pressed =
            menu("NooDS", files, index, "Settings", "Exit");

        if (pressed & defaultKeys[INPUT_A]) {
            if (files.empty()) continue;
            curPath += "/" + files[index].name;
            index = 0;
            switch (setPath(curPath)) {
            case 1:
                curPath = curPath.substr(0, curPath.rfind("/"));
                index   = 0;
                // fall through
            case 0:
                continue;
            case 2:
                curPath = curPath.substr(0, curPath.rfind("/"));
                return;
            }
        }
        else if (pressed & defaultKeys[INPUT_B]) {
            if (curPath != basePath) {
                curPath = curPath.substr(0, curPath.rfind("/"));
                index   = 0;
            }
        }
        else if (pressed & defaultKeys[INPUT_X]) {
            settingsMenu();
        }
        else if (pressed & defaultKeys[INPUT_START]) {
            return;
        }
    }
}

void ConsoleUI::settingsMenu() {
    const std::vector<std::string> toggle      = { "Off", "On" };
    const std::vector<std::string> theme       = { "Dark", "Light" };
    const std::vector<std::string> frames      = { "None", "1 Frame", "2 Frames",
                                                   "3 Frames", "4 Frames", "5 Frames" };
    const std::vector<std::string> threads     = { "Disabled", "1 Thread", "2 Threads" };
    const std::vector<std::string> position    = { "Center", "Top", "Bottom",
                                                   "Left", "Right" };
    const std::vector<std::string> rotation    = { "None", "Clockwise",
                                                   "Counter-Clockwise" };
    const std::vector<std::string> arrangement = { "Automatic", "Vertical",
                                                   "Horizontal", "Single Screen" };
    const std::vector<std::string> sizing      = { "Even", "Enlarge Top",
                                                   "Enlarge Bottom" };
    const std::vector<std::string> gap         = { "None", "Quarter", "Half", "Full" };
    const std::vector<std::string> filter      = { "Nearest", "Upscaled", "Linear" };
    const std::vector<std::string> aspect      = { "Default", "16:10",
                                                   "16:9", "18:9" };

    int index = 0;
    while (true) {
        std::vector<MenuItem> settings = {
            MenuItem("General Settings",       true),
            MenuItem("Direct Boot",            toggle[Settings::directBoot]),
            MenuItem("Keep ROM in RAM",        toggle[Settings::romInRam]),
            MenuItem("FPS Limiter",            toggle[Settings::fpsLimiter]),
            MenuItem("Show FPS Counter",       toggle[showFpsCounter]),
            MenuItem("Menu Theme",             theme[menuTheme]),
            MenuItem("Graphics Settings",      true),
            MenuItem("Skip Frames",            frames[Settings::frameskip]),
            MenuItem("Threaded 2D",            toggle[Settings::threaded2D]),
            MenuItem("Threaded 3D",            threads[Settings::threaded3D]),
            MenuItem("High-Resolution 3D",     toggle[Settings::highRes3D]),
            MenuItem("Simulate Ghosting",      toggle[Settings::screenGhost]),
            MenuItem("Audio Settings",         true),
            MenuItem("Audio Emulation",        toggle[Settings::emulateAudio]),
            MenuItem("16-bit Audio Output",    toggle[Settings::audio16Bit]),
            MenuItem("Experimental Settings",  true),
            MenuItem("High-Level ARM7",        toggle[Settings::arm7Hle]),
            MenuItem("DSi Homebrew Mode",      toggle[Settings::dsiMode]),
            MenuItem("Path Settings",          true),
            MenuItem("Separate Saves Folder",  toggle[Settings::savesFolder]),
            MenuItem("Separate States Folder", toggle[Settings::statesFolder]),
            MenuItem("Separate Cheats Folder", toggle[Settings::cheatsFolder]),
            MenuItem("Screen Layout",          true),
            MenuItem("Screen Position",        position[ScreenLayout::screenPosition]),
            MenuItem("Screen Rotation",        rotation[ScreenLayout::screenRotation]),
            MenuItem("Screen Arrangement",     arrangement[ScreenLayout::screenArrangement]),
            MenuItem("Screen Sizing",          sizing[ScreenLayout::screenSizing]),
            MenuItem("Screen Gap",             gap[ScreenLayout::screenGap]),
            MenuItem("Screen Filter",          filter[Settings::screenFilter]),
            MenuItem("Aspect Ratio",           aspect[ScreenLayout::aspectRatio]),
            MenuItem("Integer Scale",          toggle[ScreenLayout::integerScale]),
            MenuItem("GBA Crop",               toggle[ScreenLayout::gbaCrop])
        };

        uint32_t pressed = menu("Settings", settings, index, "Controls");

        if (pressed & defaultKeys[INPUT_A]) {
            switch (index) {
            case  1: Settings::directBoot            = (Settings::directBoot + 1) % 2;   break;
            case  2: Settings::romInRam              = (Settings::romInRam + 1) % 2;     break;
            case  3: Settings::fpsLimiter            = (Settings::fpsLimiter + 1) % 2;   break;
            case  4: showFpsCounter                  = (showFpsCounter + 1) % 2;         break;
            case  7: Settings::frameskip             = (Settings::frameskip + 1) % 6;    break;
            case  8: Settings::threaded2D            = (Settings::threaded2D + 1) % 2;   break;
            case  9: Settings::threaded3D            = (Settings::threaded3D + 1) % 3;   break;
            case 10: Settings::highRes3D             = (Settings::highRes3D + 1) % 2;    break;
            case 11: Settings::screenGhost           = (Settings::screenGhost + 1) % 2;  break;
            case 13: Settings::emulateAudio          = (Settings::emulateAudio + 1) % 2; break;
            case 14: Settings::audio16Bit            = (Settings::audio16Bit + 1) % 2;   break;
            case 16: Settings::arm7Hle               = (Settings::arm7Hle + 1) % 2;      break;
            case 17: Settings::dsiMode               = (Settings::dsiMode + 1) % 2;      break;
            case 19: Settings::savesFolder           = (Settings::savesFolder + 1) % 2;  break;
            case 20: Settings::statesFolder          = (Settings::statesFolder + 1) % 2; break;
            case 21: Settings::cheatsFolder          = (Settings::cheatsFolder + 1) % 2; break;
            case 23: ScreenLayout::screenPosition    = (ScreenLayout::screenPosition + 1) % 5;    break;
            case 24: ScreenLayout::screenRotation    = (ScreenLayout::screenRotation + 1) % 3;    break;
            case 25: ScreenLayout::screenArrangement = (ScreenLayout::screenArrangement + 1) % 4; break;
            case 26: ScreenLayout::screenSizing      = (ScreenLayout::screenSizing + 1) % 3;      break;
            case 27: ScreenLayout::screenGap         = (ScreenLayout::screenGap + 1) % 4;         break;
            case 28: Settings::screenFilter          = (Settings::screenFilter + 1) % 3;          break;
            case 29: ScreenLayout::aspectRatio       = (ScreenLayout::aspectRatio + 1) % 4;       break;
            case 30: ScreenLayout::integerScale      = (ScreenLayout::integerScale + 1) % 2;      break;
            case 31: ScreenLayout::gbaCrop           = (ScreenLayout::gbaCrop + 1) % 2;           break;
            case  5:
                menuTheme = (menuTheme + 1) % 2;
                palette   = &themeColors[menuTheme * 7];
                break;
            }
        }
        else if (pressed & defaultKeys[INPUT_B]) {
            changed = true;
            Settings::save();
            return;
        }
        else if (pressed & defaultKeys[INPUT_X]) {
            controlsMenu();
        }
    }
}

void ConsoleUI::controlsMenu() {
    int index = 0;
    while (true) {
        const char *names[] = {
            "A Button", "B Button", "Select Button", "Start Button",
            "Right Button", "Left Button", "Up Button", "Down Button",
            "R Button", "L Button", "X Button", "Y Button",
            "Menu Button", "Fast Forward Hold",
            "Fast Forward Toggle", "Screen Swap Toggle"
        };

        std::string bindings[INPUT_MAX];
        for (int i = 0; i < INPUT_MAX; i++) {
            for (int j = 0, k = -1; j < 32 && k < 8; j++) {
                if (!(keyBinds[i] & (1 << j))) continue;
                if (bindings[i] != "") bindings[i] += ", ";
                bindings[i] += (++k < 8) ? keyNames[j] : "...";
            }
            if (bindings[i] == "") bindings[i] = "None";
        }

        std::vector<MenuItem> controls = {
            MenuItem("Buttons",                          true),
            MenuItem(names[INPUT_A],        bindings[INPUT_A]),
            MenuItem(names[INPUT_B],        bindings[INPUT_B]),
            MenuItem(names[INPUT_SELECT],   bindings[INPUT_SELECT]),
            MenuItem(names[INPUT_START],    bindings[INPUT_START]),
            MenuItem(names[INPUT_RIGHT],    bindings[INPUT_RIGHT]),
            MenuItem(names[INPUT_LEFT],     bindings[INPUT_LEFT]),
            MenuItem(names[INPUT_UP],       bindings[INPUT_UP]),
            MenuItem(names[INPUT_DOWN],     bindings[INPUT_DOWN]),
            MenuItem(names[INPUT_R],        bindings[INPUT_R]),
            MenuItem(names[INPUT_L],        bindings[INPUT_L]),
            MenuItem(names[INPUT_X],        bindings[INPUT_X]),
            MenuItem(names[INPUT_Y],        bindings[INPUT_Y]),
            MenuItem("Hotkeys",                          true),
            MenuItem(names[INPUT_MENU],      bindings[INPUT_MENU]),
            MenuItem(names[INPUT_FAST_HOLD], bindings[INPUT_FAST_HOLD]),
            MenuItem(names[INPUT_FAST_TOGG], bindings[INPUT_FAST_TOGG]),
            MenuItem(names[INPUT_SCRN_SWAP], bindings[INPUT_SCRN_SWAP])
        };

        uint32_t pressed = menu("Controls", controls, index, "Clear");
        int i = index - ((index > 13) ? 2 : 1);

        if (pressed & defaultKeys[INPUT_A]) {
            keyBinds[i] |= message(
                std::string("Remap ") + names[i],
                "Press an input to add it as a binding.", 2);
        }
        else if (pressed & defaultKeys[INPUT_B]) {
            return;
        }
        else if (pressed & defaultKeys[INPUT_X]) {
            keyBinds[i] = 0;
        }
    }
}

void ConsoleUI::pauseMenu() {
    stopCore();

    std::vector<MenuItem> items = {
        MenuItem("Resume"),
        MenuItem("Restart"),
        MenuItem("Save State"),
        MenuItem("Load State"),
        MenuItem("Change Save Type"),
        MenuItem("Settings"),
        MenuItem("File Browser")
    };

    int index = 0;
    while (true) {
        uint32_t pressed = menu("NooDS", items, index);

        if (pressed & defaultKeys[INPUT_A]) {
            switch (index) {
            case 0:
                startCore();
                return;

            case 1:
                createCore() ? startCore() : fileBrowser();
                return;

            case 2:
                if (!message("Save State",
                    (core->saveStates.checkState() == STATE_FILE_FAIL) ?
                    "Saving and loading states is dangerous and can lead to data loss.\n"
                    "States are also not guaranteed to be compatible across emulator versions.\n"
                    "Please rely on in-game saving to keep your progress, and back up .sav files\n"
                    "before using this feature. Do you want to save the current state?" :
                    "Do you want to overwrite the saved state with the current state?"
                    " This can't be undone!", 1))
                    break;
                core->saveStates.saveState();
                startCore();
                return;

            case 3: {
                bool error = true;
                std::string ltitle, ltext;
                switch (core->saveStates.checkState()) {
                case STATE_SUCCESS:
                    error  = false;
                    ltitle = "Load State";
                    ltext  = "Do you want to load the saved state and lose the"
                             " current state? This can't be undone!";
                    break;
                case STATE_FILE_FAIL:
                    ltitle = "Error";
                    ltext  = "The state file doesn't exist or couldn't be opened.";
                    break;
                case STATE_FORMAT_FAIL:
                    ltitle = "Error";
                    ltext  = "The state file doesn't have a valid format.";
                    break;
                case STATE_VERSION_FAIL:
                    ltitle = "Error";
                    ltext  = "The state file isn't compatible with"
                             " this version of NooDS.";
                    break;
                }
                if (!message(ltitle, ltext, !error) || error) break;
                core->saveStates.loadState();
                startCore();
                return;
            }

            case 4:
                if (saveTypeMenu())
                    return createCore() ? startCore() : fileBrowser();
                break;

            case 5:
                settingsMenu();
                break;

            case 6:
                fileBrowser();
                return;
            }
        }
        else if (pressed & defaultKeys[INPUT_B]) {
            startCore();
            return;
        }
    }
}

bool ConsoleUI::saveTypeMenu() {
    std::vector<MenuItem> items;
    if (core->gbaMode) {
        items.push_back(MenuItem("None"));
        items.push_back(MenuItem("EEPROM 0.5KB"));
        items.push_back(MenuItem("EEPROM 8KB"));
        items.push_back(MenuItem("SRAM 32KB"));
        items.push_back(MenuItem("FLASH 64KB"));
        items.push_back(MenuItem("FLASH 128KB"));
    }
    else {
        items.push_back(MenuItem("None"));
        items.push_back(MenuItem("EEPROM 0.5KB"));
        items.push_back(MenuItem("EEPROM 8KB"));
        items.push_back(MenuItem("EEPROM 64KB"));
        items.push_back(MenuItem("EEPROM 128KB"));
        items.push_back(MenuItem("FRAM 32KB"));
        items.push_back(MenuItem("FLASH 256KB"));
        items.push_back(MenuItem("FLASH 512KB"));
        items.push_back(MenuItem("FLASH 1024KB"));
        items.push_back(MenuItem("FLASH 8192KB"));
    }

    int index = 0;
    while (true) {
        uint32_t pressed = menu("Change Save Type", items, index);

        if (pressed & defaultKeys[INPUT_A]) {
            if (!message("Changing Save Type",
                "Are you sure? This may result in data loss!", 1))
                continue;

            if (core->gbaMode) {
                switch (index) {
                case 0: core->cartridgeGba.resizeSave(0x00000); break;
                case 1: core->cartridgeGba.resizeSave(0x00200); break;
                case 2: core->cartridgeGba.resizeSave(0x02000); break;
                case 3: core->cartridgeGba.resizeSave(0x08000); break;
                case 4: core->cartridgeGba.resizeSave(0x10000); break;
                case 5: core->cartridgeGba.resizeSave(0x20000); break;
                }
            }
            else {
                switch (index) {
                case 0: core->cartridgeNds.resizeSave(0x000000); break;
                case 1: core->cartridgeNds.resizeSave(0x000200); break;
                case 2: core->cartridgeNds.resizeSave(0x002000); break;
                case 3: core->cartridgeNds.resizeSave(0x010000); break;
                case 4: core->cartridgeNds.resizeSave(0x020000); break;
                case 5: core->cartridgeNds.resizeSave(0x008000); break;
                case 6: core->cartridgeNds.resizeSave(0x040000); break;
                case 7: core->cartridgeNds.resizeSave(0x080000); break;
                case 8: core->cartridgeNds.resizeSave(0x100000); break;
                case 9: core->cartridgeNds.resizeSave(0x800000); break;
                }
            }
            return true;
        }
        else if (pressed & defaultKeys[INPUT_B]) {
            return false;
        }
    }
}

bool ConsoleUI::createCore() {
    try {
        if (core) delete core;
        core = new Core(ndsPath, gbaPath);
        return true;
    }
    catch (CoreError e) {
        std::string text;
        switch (e) {
        case ERROR_BIOS:
            text = "Make sure the path settings point to valid BIOS files"
                   " and try again.\n"
                   "You can modify the path settings in the noods.ini file.";
            message("Error Loading BIOS", text);
            break;
        case ERROR_FIRM:
            text = "Make sure the path settings point to a bootable firmware"
                   " file or try another boot method.\n"
                   "You can modify the path settings in the noods.ini file.";
            message("Error Loading Firmware", text);
            break;
        case ERROR_ROM:
            text = "Make sure the ROM file is accessible and try again.";
            message("Error Loading ROM", text);
            break;
        }
        core = nullptr;
        return false;
    }
}

void ConsoleUI::startCore() {
    PPCIrqState st = PPCIrqLockByMsr();
    bool isRunning = running;
    PPCIrqUnlockByMsr(st);
    if (isRunning) return;

    st = PPCIrqLockByMsr();
    running = true;
    PPCIrqUnlockByMsr(st);

    KThreadPrepare(&coreThreadObj, runCore, nullptr,
        coreThreadStack + sizeof(coreThreadStack), KTHR_MAIN_PRIO);
    KThreadPrepare(&saveThreadObj, checkSave, nullptr,
        saveThreadStack + sizeof(saveThreadStack), KTHR_MAIN_PRIO + 1);

    KThreadResume(&coreThreadObj);
    KThreadResume(&saveThreadObj);
}

void ConsoleUI::stopCore() {
    PPCIrqState st = PPCIrqLockByMsr();
    bool isRunning = running;
    PPCIrqUnlockByMsr(st);
    if (!isRunning) return;

    st = PPCIrqLockByMsr();
    running = false;
    PPCIrqUnlockByMsr(st);

    KThrQueueUnblockAllByValue(&saveCondQueue, 0);

    KThreadJoin(&coreThreadObj);
    KThreadJoin(&saveThreadObj);
}

sptr ConsoleUI::runCore(void *arg) {
    (void)arg;
    PPCIrqState st = PPCIrqLockByMsr();
    bool isRunning = running;
    PPCIrqUnlockByMsr(st);

    while (isRunning) {
        {
            PPCIrqState st2 = PPCIrqLockByMsr();
            core->running = 1;
            PPCIrqUnlockByMsr(st2);
        }

        core->runCore();

        st = PPCIrqLockByMsr();
        isRunning = running;
        PPCIrqUnlockByMsr(st);
    }
    return 0;
}

sptr ConsoleUI::checkSave(void *arg) {
    (void)arg;
    PPCIrqState st = PPCIrqLockByMsr();
    bool isRunning = running;
    PPCIrqUnlockByMsr(st);

    while (isRunning) {
        KThreadSleepMs(3000);

        st = PPCIrqLockByMsr();
        isRunning = running;
        PPCIrqUnlockByMsr(st);

        if (isRunning) {
            core->cartridgeNds.writeSave();
            core->cartridgeGba.writeSave();
        }
    }
    return 0;
}
