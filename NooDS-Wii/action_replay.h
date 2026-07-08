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
}

class Core;

struct ARCheat {
    std::string name;
    std::vector<uint32_t> code;
    bool enabled;
};

class ActionReplay {
public:
    std::vector<ARCheat> cheats;

    ActionReplay(Core *core): core(core) {}
    void setPath(std::string path);
    void setFd(int fd);

    bool loadCheats();
    bool saveCheats();
    void applyCheats();

private:
    Core *core;
    std::string path;
    int fd = -1;

    FILE *openFile(const char *mode);
};
