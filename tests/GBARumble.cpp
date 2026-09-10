// SPDX-License-Identifier: GPL-3.0-or-later
#include "NDS.h"
#include "GBACart.h"
#include "Platform.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace melonDS;

struct RumbleEvent
{
    bool Start;
    u32 Duration;
};
using RumbleTrace = std::vector<RumbleEvent>;

namespace melonDS::Platform
{
void Addon_RumbleStart(u32 len, void* userdata)
{
    static_cast<RumbleTrace*>(userdata)->push_back({true, len});
}

void Addon_RumbleStop(void* userdata)
{
    static_cast<RumbleTrace*>(userdata)->push_back({false, 0});
}

void Log(LogLevel, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

// Link the real cartridge translation units without activating other devices
// or persisting anything. These boundaries are not used by a Rumble Pak.
void WriteGBASave(const u8*, u32, u32, u32, void*) { std::abort(); }
bool Addon_KeyDown(KeyType, void*) { std::abort(); }
float Addon_MotionQuery(MotionQueryType, void*) { std::abort(); }
}

struct WriteStep
{
    u16 Value;
    unsigned Pulses;
};

static bool CheckWrites(GBACart::CartCommon& cart, RumbleTrace& trace,
                        std::initializer_list<WriteStep> steps)
{
    bool passed = true;
    for (const auto& step : steps)
    {
        cart.ROMWrite(0x08001000, step.Value);
        if (trace.size() != step.Pulses * 2)
        {
            fprintf(stderr, "write %04X: expected %u Stop/Start pairs, got %zu callbacks\n",
                    unsigned(step.Value), step.Pulses, trace.size());
            passed = false;
        }
        // Preserve the existing host contract: Stop, then a 16 ms Start on
        // either actuator transition. This does not measure a physical effect.
        for (size_t i = 0; i + 1 < trace.size(); i += 2)
        {
            if (trace[i].Start || !trace[i + 1].Start || trace[i + 1].Duration != 16)
            {
                fputs("wrong Stop/Start order or duration\n", stderr);
                passed = false;
                break;
            }
        }
    }
    return passed;
}

static bool RunCase(const char* name)
{
    RumbleTrace trace;
    GBACart::CartRumblePak cart(&trace);
    cart.Reset();

    if (strcmp(name, "control") == 0)
    {
        // libnds rumbleSet writes 0/2; repeated positions must stay quiet.
        // https://github.com/devkitPro/libnds/blob/84e6082ce27c87ed218fb369a9944644aa2243a6/source/arm9/rumble.c#L56
        return CheckWrites(cart, trace, {{0, 0}, {2, 1}, {2, 1}, {0, 2}, {0, 2}, {2, 3}});
    }
    if (strcmp(name, "ad1-mask") == 0)
    {
        // Only AD1 is connected. Expected positions: low, high, high, low.
        // These literal expectations come from the wiring, not the emulator.
        // https://problemkaputt.de/gbatek.htm#dscartrumblepak
        return CheckWrites(cart, trace, {{0x0001, 0}, {0x0003, 1}, {0x0103, 1}, {0x0101, 2}});
    }
    if (strcmp(name, "state-load") == 0)
    {
        // Older states stored the entire bus value. AD1 is already high;
        // restoring unused bits must not turn the first same-position write
        // into an extra pulse. Generate a state in memory, with no user files.
        Savestate saved(64);
        saved.Section("GBCS");
        u16 legacyState = 0x0103;
        saved.Var16(&legacyState);
        saved.Finish();
        if (saved.Error) return false;
        Savestate loaded(saved.Buffer(), saved.Length(), false);
        cart.DoSavestate(&loaded);
        if (loaded.Error) return false;
        return CheckWrites(cart, trace, {{2, 0}, {0, 1}, {0, 1}, {2, 2}});
    }
    fprintf(stderr, "unknown case: %s\n", name);
    return false;
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fputs("usage: GBARumble control|ad1-mask|state-load\n", stderr);
        return 2;
    }
    const bool passed = RunCase(argv[1]);
    printf("%s: %s\n", argv[1], passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
