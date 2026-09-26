// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual scheduler's deadline selection; no copied scheduler loop.
#include "Args.h"
#include "NDS.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace melonDS;

struct SchedulerProbe final : NDS
{
    using NDS::NDS;
    using NDS::NextTarget;
    using NDS::NextTargetSleep;
    using NDS::RunSystem;
    using NDS::SchedListMask;
    using NDS::RefreshEarliestEvent;
    using NDS::SysTimestamp;
    u64 Calls = 0;
    u64 Trace = 0;

    void Prepare(u32 mask, u64 deadline)
    {
        SchedListMask = mask;
        SysTimestamp = 1000;
        Calls = Trace = 0;
        for (u32 i = 0; i < Event_MAX; ++i)
        {
            RegisterEventFuncs(i, this, {[](void* that, u32 id) {
                auto& self = *static_cast<SchedulerProbe*>(that);
                ++self.Calls;
                self.Trace = self.Trace * 131 + id + 1;
            }});
            SchedList[i].Timestamp = deadline;
            SchedList[i].FuncID = 0;
            SchedList[i].Param = i;
        }
        // The fixture writes the raw mask and timestamps, so the derived earliest
        // deadline has to be recomputed from them, like the savestate load does.
        RefreshEarliestEvent();
    }

    void SetDeadline(u32 id, u64 deadline)
    {
        SchedList[id].Timestamp = deadline;
        RefreshEarliestEvent();
    }
};

int main(int argc, char** argv)
{
    NDSArgs args;
    args.JIT = std::nullopt;
    auto nds = std::make_unique<SchedulerProbe>(std::move(args));
    nds->Reset();
    if (argc == 1)
    {
        unsigned failures = 0;
        auto check = [&](bool value, const char* message) {
            if (!value) { ++failures; std::fprintf(stderr, "%s\n", message); }
        };
        nds->Prepare(0, 1000);
        check(nds->NextTarget() == 1064, "empty scheduler horizon");
        check(nds->NextTargetSleep() == UINT64_MAX, "empty sleeping scheduler");
        for (u32 id = 0; id < Event_MAX; ++id)
        {
            nds->Prepare(1u << id, 1041);
            check(nds->NextTarget() == 1041, "active deadline not selected");
            const bool sleep = id == Event_SPU || id == Event_RTC ||
                               id == Event_CartSave || id == Event_DSi_Cart2Save;
            check(nds->NextTargetSleep() == (sleep ? 1041u : UINT64_MAX),
                  "sleep event visibility");
        }
        nds->Prepare((1u << Event_DSi_Cart2Save) | 1u, 1071);
        check(nds->NextTarget() == 1071, "horizon margin includes near deadline");
        nds->SetDeadline(0, 1072);
        nds->SetDeadline(Event_DSi_Cart2Save, 1072);
        check(nds->NextTarget() == 1064, "horizon margin boundary");
        nds->SetDeadline(Event_DSi_Cart2Save, 999);
        check(nds->NextTarget() == 999, "overdue high ID deadline");
        nds->Prepare(0xF8000000u, 1000);
        check(nds->NextTarget() == 1064, "out-of-range mask bits ignored");
        nds->RunSystem(1000);
        check(nds->Calls == 0, "out-of-range mask bits dispatched");
        std::printf("scheduler deadline checks: %s\n", failures ? "FAIL" : "PASS");
        return failures ? 1 : 0;
    }
    const u32 iterations = std::strtoul(argv[1], nullptr, 10);
    if (iterations == 0) return 2;
    const std::array<u32, 6> masks = {
        1u, 1u << Event_DSi_Cart2Save, (1u << Event_DSi_Cart2Save) | 1u,
        0xFu, 0xFu | (1u << Event_DSi_SDMMCTransfer) | (1u << Event_DSi_DSP),
        (1u << Event_MAX) - 1
    };
    for (unsigned cell = 0; cell < masks.size(); ++cell)
    for (unsigned mode = 0; mode < 3; ++mode)
    {
        const u32 mask = masks[cell];
        nds->Prepare(mask, mode == 1 ? 1000 : 1000000000);
        u64 sum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < iterations; ++i)
        {
            if (mode == 0)
            {
                sum += nds->NextTarget();
                nds->RunSystem(1000);
            }
            else if (mode == 1)
            {
                nds->SchedListMask = mask;
                nds->RefreshEarliestEvent();
                nds->RunSystem(1000);
            }
            else sum += nds->NextTargetSleep();
        }
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::printf("BENCH,%u,%u,%u,%lld,%llu,%llu,%llu,%u\n", cell, mode,
            iterations, static_cast<long long>(nanos), static_cast<unsigned long long>(sum),
            static_cast<unsigned long long>(nds->Calls),
            static_cast<unsigned long long>(nds->Trace), nds->SchedListMask);
    }
}
