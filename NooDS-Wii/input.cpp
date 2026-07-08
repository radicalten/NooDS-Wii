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

#include "core.h"

void Input::pressKey(int key) {
    // Clear key bits to indicate presses
    if (key < 10) // A, B, select, start, right, left, up, down, R, L
        keyInput &= ~BIT(key);
    else if (key < 12) // X, Y
        extKeyIn &= ~BIT(key - 10);
}

void Input::releaseKey(int key) {
    // Set key bits to indicate releases
    if (key < 10) // A, B, select, start, right, left, up, down, R, L
        keyInput |= BIT(key);
    else if (key < 12) // X, Y
        extKeyIn |= BIT(key - 10);
}

void Input::pressScreen() {
    // Clear the pen down bit to indicate a touch press
    extKeyIn &= ~BIT(6);
}

void Input::releaseScreen() {
    // Set the pen down bit to indicate a touch release
    extKeyIn |= BIT(6);
}
