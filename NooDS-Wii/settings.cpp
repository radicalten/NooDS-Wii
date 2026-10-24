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

#include <sys/stat.h>
#include "core.h"

int Settings::directBoot  = 1;
int Settings::romInRam    = 0;
int Settings::fpsLimiter  = 0;
int Settings::frameskip   = 0;
int Settings::threaded2D  = 1;
int Settings::threaded3D  = 1;
int Settings::highRes3D   = 0;
int Settings::screenGhost = 0;
int Settings::emulateAudio = 1;
int Settings::audio16Bit  = 1;
int Settings::monoAudio   = 0;   // default: stereo
int Settings::savesFolder  = 0;
int Settings::statesFolder = 1;
int Settings::cheatsFolder = 1;
int Settings::screenFilter = 0;
int Settings::screenLayout = 0;
int Settings::screen1Size  = 0;
int Settings::screen2Size  = 0;
int Settings::screenSwap   = 0;
int Settings::wiiScreenGap = 1;
int Settings::armJit       = 1;
int Settings::arm7Hle      = 0;
int Settings::dsiMode      = 0;
int Settings::stylusEdgeClamp = 1; // default On — off-screen press snaps to nearest edge

const int Settings::FRAMESKIP_VALUES[Settings::FRAMESKIP_COUNT] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 30
};

int Settings::frameskipToIndex(int fs) {
    int bestIdx  = 0;
    int bestDiff = (fs < 0) ? -fs : fs;
    for (int i = 1; i < FRAMESKIP_COUNT; i++) {
        int d = fs - FRAMESKIP_VALUES[i];
        if (d < 0) d = -d;
        if (d < bestDiff) {
            bestDiff = d;
            bestIdx  = i;
        }
    }
    return bestIdx;
}

void Settings::stepFrameskip(int dir) {
    int idx = frameskipToIndex(frameskip);
    idx = (idx + dir) % FRAMESKIP_COUNT;
    if (idx < 0) idx += FRAMESKIP_COUNT;
    frameskip = FRAMESKIP_VALUES[idx];
}

std::string Settings::bios9Path    = "bios9.bin";
std::string Settings::bios7Path    = "bios7.bin";
std::string Settings::firmwarePath = "firmware.bin";
std::string Settings::gbaBiosPath  = "gba_bios.bin";
std::string Settings::sdImagePath  = "sd.img";
std::string Settings::basePath     = ".";

std::vector<Setting> Settings::settings = {
    Setting("directBoot",   &directBoot,   false),
    Setting("romInRam",     &romInRam,     false),
    Setting("fpsLimiter",   &fpsLimiter,   false),
    Setting("frameskip",    &frameskip,    false),
    Setting("threaded2D",   &threaded2D,   false),
    Setting("threaded3D",   &threaded3D,   false),
    Setting("highRes3D",    &highRes3D,    false),
    Setting("screenGhost",  &screenGhost,  false),
    Setting("emulateAudio", &emulateAudio, false),
    Setting("audio16Bit",   &audio16Bit,   false),
    Setting("monoAudio",    &monoAudio,    false),
    Setting("savesFolder",  &savesFolder,  false),
    Setting("statesFolder", &statesFolder, false),
    Setting("cheatsFolder", &cheatsFolder, false),
    Setting("screenFilter", &screenFilter, false),
    Setting("screenLayout", &screenLayout, false),
    Setting("screen1Size",  &screen1Size,  false),
    Setting("screen2Size",  &screen2Size,  false),
    Setting("screenSwap",   &screenSwap,   false),
    Setting("wiiScreenGap", &wiiScreenGap, false),
    Setting("armJit",       &armJit,       false),
    Setting("arm7Hle",      &arm7Hle,      false),
    Setting("dsiMode",      &dsiMode,      false),
    Setting("stylusEdgeClamp", &stylusEdgeClamp, false),
    Setting("bios9Path",    &bios9Path,    true),
    Setting("bios7Path",    &bios7Path,    true),
    Setting("firmwarePath", &firmwarePath, true),
    Setting("gbaBiosPath",  &gbaBiosPath,  true),
    Setting("sdImagePath",  &sdImagePath,  true),
};

void Settings::add(std::vector<Setting> &settings) {
    Settings::settings.insert(
        Settings::settings.end(), settings.begin(), settings.end());
}

bool Settings::load(std::string path) {
    basePath = path;
    while (basePath.size() > 1 && basePath.back() == '/' && basePath[basePath.size() - 2] != ':')
        basePath.pop_back();
    const std::string sep = (!basePath.empty() && basePath.back() == '/') ? "" : "/";

    // Ensure all folders exist.
    mkdir(basePath.c_str() MKDIR_ARGS);
    mkdir((basePath + sep + "saves").c_str()  MKDIR_ARGS);
    mkdir((basePath + sep + "states").c_str() MKDIR_ARGS);
    mkdir((basePath + sep + "cheats").c_str() MKDIR_ARGS);

    // Open the settings file; write defaults if it does not exist.
    FILE *file = fopen((basePath + sep + "noods.ini").c_str(), "r");
    if (!file) {
        Settings::bios9Path    = basePath + sep + "bios9.bin";
        Settings::bios7Path    = basePath + sep + "bios7.bin";
        Settings::firmwarePath = basePath + sep + "firmware.bin";
        Settings::gbaBiosPath  = basePath + sep + "gba_bios.bin";
        Settings::sdImagePath  = basePath + sep + "sd.img";
        Settings::save();
        return false;
    }

    // Read each key=value line.
    char data[512];
    while (fgets(data, 512, file) != nullptr) {
        std::string line  = data;
        size_t      split = line.find('=');
        if (split == std::string::npos) continue;

        std::string name = line.substr(0, split);
        for (size_t i = 0; i < settings.size(); i++) {
            if (name != settings[i].name) continue;
            // Strip any trailing CR/LF characters from the value.
            std::string value = line.substr(split + 1);
            while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
                value.pop_back();
            if (settings[i].isString)
                *(std::string*)settings[i].value = value;
            else if (!value.empty() &&
                     value[0] >= '0' && value[0] <= '9')
                *(int*)settings[i].value = stoi(value);
            break;
        }
    }

    fclose(file);
    frameskip = FRAMESKIP_VALUES[frameskipToIndex(frameskip)];
    if (screenLayout < 0 || screenLayout > 5) screenLayout = 0;
    if (screen1Size  < 0 || screen1Size  > 9) screen1Size  = 0;
    if (screen2Size  < 0 || screen2Size  > 9) screen2Size  = 0;
    if (screenSwap   < 0 || screenSwap   > 1) screenSwap   = 0;
    if (wiiScreenGap < 0 || wiiScreenGap > 4) wiiScreenGap = 1;
    if (screenFilter < 0 || screenFilter > 2) screenFilter = 0;
    armJit = (armJit != 0) ? 1 : 0;
    stylusEdgeClamp = (stylusEdgeClamp != 0) ? 1 : 0;
    return true;
}

bool Settings::save() {
    const std::string sep = (!basePath.empty() && basePath.back() == '/') ? "" : "/";
    FILE *file = fopen((basePath + sep + "noods.ini").c_str(), "w");
    if (!file) return false;

    for (size_t i = 0; i < settings.size(); i++) {
        std::string value = settings[i].isString
            ? *(std::string*)settings[i].value
            : std::to_string(*(int*)settings[i].value);
        fprintf(file, "%s=%s\n", settings[i].name.c_str(), value.c_str());
    }

    fclose(file);
    return true;
}
