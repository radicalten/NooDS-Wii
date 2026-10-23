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

#include <string>
#include <vector>

struct Setting {
    std::string name;
    void *value;
    bool isString;

    Setting(std::string name, void *value, bool isString):
        name(name), value(value), isString(isString) {}
};

class Settings {
public:
    static int directBoot;
    static int romInRam;
    static int fpsLimiter;
    static int frameskip;
    static int threaded2D;
    static int threaded3D;
    static int highRes3D;
    static int screenGhost;
    static int emulateAudio;
    static int audio16Bit;
    static int monoAudio;       // 0 = stereo, 1 = mono
    static int savesFolder;
    static int statesFolder;
    static int cheatsFolder;
    static int screenFilter;
    static int screenLayout;    // 0=Vertical, 1=Side-Side, 2=HybridTop, 3=HybridBot, 4=Scr1Only, 5=Scr2Only
    static int screen1Size;     // 0=Auto, 1=0.50x..8=2.50x, 9=Off
    static int screen2Size;     // 0=Auto, 1=0.50x..8=2.50x, 9=Off
    static int screenSwap;      // 0=Off, 1=On
    static int wiiScreenGap;    // 0=0px, 1=2px, 2=8px, 3=16px, 4=32px
    static int armJit;          // 0=Off (Interpreter), 1=On (PPC JIT Recompiler)
    static int arm7Hle;
    static int dsiMode;

    static constexpr int FRAMESKIP_COUNT = 14;
    static const int     FRAMESKIP_VALUES[FRAMESKIP_COUNT];
    static int  frameskipToIndex(int fs);
    static void stepFrameskip(int dir);

    static std::string bios9Path;
    static std::string bios7Path;
    static std::string firmwarePath;
    static std::string gbaBiosPath;
    static std::string sdImagePath;
    static std::string basePath;

    static void add(std::vector<Setting> &settings);
    static bool load(std::string path = ".");
    static bool save();

private:
    static std::vector<Setting> settings;
    Settings() {}
};
