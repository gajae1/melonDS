// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef AUDIODIAGNOSTICS_H
#define AUDIODIAGNOSTICS_H

#include <algorithm>
#include <SDL2/SDL.h>

// Written only by the device callback. Read/reset only while that device is
// paused, closed, or locked. No logging or allocation on the callback thread.
struct AudioDiagnostics
{
    bool Enabled = false;
    Uint64 Callbacks = 0, RequestedFrames = 0, SuppliedFrames = 0;
    Uint64 Underruns = 0, EmptyCallbacks = 0, MaxReadTicks = 0, MaxGapTicks = 0;
    Uint64 PreviousStart = 0, LastReportedCallbacks = 0;

    Uint64 Begin() const { return Enabled ? SDL_GetPerformanceCounter() : 0; }

    void Record(int requested, int supplied, Uint64 started)
    {
        if (!Enabled) return;
        MaxReadTicks = std::max(MaxReadTicks, SDL_GetPerformanceCounter() - started);
        if (PreviousStart) MaxGapTicks = std::max(MaxGapTicks, started - PreviousStart);
        PreviousStart = started;
        ++Callbacks;
        RequestedFrames += requested;
        SuppliedFrames += supplied;
        if (supplied < requested) ++Underruns;
        if (!supplied) ++EmptyCallbacks;
    }
};
#endif
