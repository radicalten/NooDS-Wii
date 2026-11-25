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

#pragma once

#include <cstdint>

// Optional Wii-side renderer profiler. It is compiled out completely unless
// the Makefile is invoked with PROFILE=1; release builds contain no timing
// reads or global counters.
#ifdef NOODS_V180_PROFILE
#include <tuxedo/ppc/clock.h>

extern volatile uint64_t g_v180Profile[24];

struct V180ProfScopeConditional {
    int index; bool active; uint64_t start;
    V180ProfScopeConditional(int idx, bool enabled) : index(idx), active(enabled), start(enabled ? PPCGetTickCount() : 0) {}
    ~V180ProfScopeConditional() { if (active) g_v180Profile[index] += PPCGetTickCount() - start; }
};

struct V180ProfScope {
    int index;
    uint64_t start;
    explicit V180ProfScope(int idx) : index(idx), start(PPCGetTickCount()) {}
    ~V180ProfScope() { g_v180Profile[index] += PPCGetTickCount() - start; }
};

#define V180_PROFILE_SCOPE_IF(idx, enabled) V180ProfScopeConditional _v180ProfScopeConditional(idx, enabled)
#define V180_PROFILE_SCOPE(idx) V180ProfScope _v180ProfScope(idx)
#define V180_PROFILE_TICK() PPCGetTickCount()
#define V180_PROFILE_ADD(idx, value) (g_v180Profile[idx] += (value))
#define V180_PROFILE_INC(idx) (g_v180Profile[idx]++)
#else
#define V180_PROFILE_SCOPE_IF(idx, enabled) do {} while (0)
#define V180_PROFILE_SCOPE(idx) do {} while (0)
#define V180_PROFILE_TICK() UINT64_C(0)
#define V180_PROFILE_ADD(idx, value) do {} while (0)
#define V180_PROFILE_INC(idx) do {} while (0)
#endif
