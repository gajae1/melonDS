// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Savestate.h"
#include "Platform.h"

using namespace melonDS;

// The section parser has no frontend dependency other than logging.
void melonDS::Platform::Log(LogLevel, const char*, ...) {}

static std::vector<u8> MakeState(u32 size)
{
    std::vector<u8> data(size, 0);
    memcpy(data.data(), "MELN", 4);
    u16 major = SAVESTATE_MAJOR;
    u16 minor = SAVESTATE_MINOR;
    memcpy(data.data() + 4, &major, sizeof(major));
    memcpy(data.data() + 6, &minor, sizeof(minor));
    memcpy(data.data() + 8, &size, sizeof(size));
    return data;
}

static bool Rejects(std::vector<u8>& data, const char* magic)
{
    Savestate state(data.data(), static_cast<u32>(data.size()), false);
    if (state.Error) return false; // The global header must be valid in these tests.
    state.Section(magic);
    return state.Error;
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "roundtrip"))
    {
        Savestate saved(128);
        u32 first = 0x12345678, second = 0xABCDEF01;
        saved.Section("ONE1");
        saved.Var32(&first);
        saved.Section("TWO2");
        saved.Var32(&second);
        saved.Section("EMPT");
        saved.Finish();
        if (saved.Error) return 1;
        Savestate loaded(saved.Buffer(), saved.Length(), false);
        u32 result = 0;
        loaded.Section("TWO2");
        loaded.Var32(&result);
        if (loaded.Error || result != second) return 1;
        loaded.Section("ONE1");
        loaded.Var32(&result);
        if (loaded.Error || result != first) return 1;
        loaded.Section("EMPT");
        if (loaded.Error) return 1; // A header-only (16-byte) section is valid.
        loaded.Section("MISS");
        return loaded.Error ? 0 : 1;
    }
    if (!strcmp(argv[1], "invalid-length"))
    {
        for (u32 size : {0u, 1u, 15u, 17u, 0xFFFFFFFFu})
        {
            auto data = MakeState(32);
            memcpy(data.data() + 16, "TEST", 4);
            memcpy(data.data() + 20, &size, sizeof(size));
            if (!Rejects(data, "TEST"))
            {
                fprintf(stderr, "Accepted matching section with invalid length %u\n", size);
                return 1;
            }
        }
        return 0;
    }
    if (!strcmp(argv[1], "truncated-header"))
    {
        for (u32 tail = 1; tail < 16; tail++)
        {
            auto data = MakeState(16 + tail);
            memcpy(data.data() + 16, "TEST", tail < 4 ? tail : 4);
            if (!Rejects(data, "TEST"))
            {
                fprintf(stderr, "Accepted truncated section header of %u bytes\n", tail);
                return 1;
            }
        }
        return 0;
    }
    if (!strcmp(argv[1], "zero-length-skip"))
    {
        auto data = MakeState(32);
        memcpy(data.data() + 16, "SKIP", 4);
        return Rejects(data, "TEST") ? 0 : 1;
    }
    if (!strcmp(argv[1], "oversized-skip"))
    {
        auto data = MakeState(32);
        memcpy(data.data() + 16, "SKIP", 4);
        u32 size = 0xFFFFFFFF;
        memcpy(data.data() + 20, &size, sizeof(size));
        return Rejects(data, "TEST") ? 0 : 1;
    }
    return 2;
}
