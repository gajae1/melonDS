// SPDX-License-Identifier: GPL-3.0-or-later
// Generated callbacks exercise the actual NDS scheduler. No external ROM/BIOS,
// hardware event timing oracle, or copy of the dispatch implementation.
#include "Args.h"
#include "NDS.h"

#include <cstdio>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

using namespace melonDS;

namespace
{
struct Call
{
    char Source;
    u32 Param;
    bool operator==(const Call&) const = default;
};

struct SchedulerFixture final : NDS
{
    using NDS::NDS;

    // Unused DSi slots in this DS fixture avoid replacing component callbacks.
    static constexpr u32 Before = Event_DSi_SDMMCTransfer;
    static constexpr u32 Low = Event_DSi_CamIRQ;
    static constexpr u32 High = Event_DSi_DSPHLE;
    static constexpr u32 After = Event_DSi_Cart2Power;
    static_assert(Before < Low && Low < High && High < After);

    enum class Action { None, Cancel, Future, Replace, Publish };
    Action LowAction = Action::None;
    std::vector<Call> Calls;
    unsigned Failures = 0;
    unsigned FailedCases = 0;
    unsigned CaseStart = 0;
    const char* CaseName = "";

    void Begin(const char* name, Action action)
    {
        CurCPU = 1;
        Reset();
        for (u32 id = 0; id < Event_MAX; ++id)
            CancelEvent(id);
        Calls.clear();
        LowAction = action;
        CaseName = name;
        CaseStart = Failures;
        RegisterEventFuncs(Low, this, {MakeEventThunk(SchedulerFixture, OnLow)});
        RegisterHigh();
        RegisterEventFuncs(Before, this, {[](void* that, u32 param) {
            static_cast<SchedulerFixture*>(that)->Calls.push_back({'B', param});
        }});
        RegisterEventFuncs(After, this, {[](void* that, u32 param) {
            static_cast<SchedulerFixture*>(that)->Calls.push_back({'A', param});
        }});
    }

    void RegisterHigh()
    {
        RegisterEventFuncs(High, this, {
            [](void* that, u32 param) {
                static_cast<SchedulerFixture*>(that)->Calls.push_back({'H', param});
            },
            [](void* that, u32 param) {
                static_cast<SchedulerFixture*>(that)->Calls.push_back({'R', param});
            },
            MakeEventThunk(SchedulerFixture, OnPeriodic)
        });
    }

    void OnLow(u32 param)
    {
        Calls.push_back({'L', param});
        switch (LowAction)
        {
        case Action::None:
            break;
        case Action::Cancel:
            CancelEvent(High);
            break;
        case Action::Future:
            CancelEvent(High);
            ScheduleEvent(High, false, 20, 1, 30);
            break;
        case Action::Replace:
            CancelEvent(High);
            UnregisterEventFuncs(High);
            RegisterHigh();
            ScheduleEvent(High, false, 0, 1, 40);
            break;
        case Action::Publish:
            ScheduleEvent(Before, false, 0, 0, 50);
            ScheduleEvent(After, false, 0, 0, 60);
            break;
        }
    }

    void OnPeriodic(u32 param)
    {
        Calls.push_back({'P', param});
        ScheduleEvent(High, true, 4, 2, param + 1);
    }

    void SchedulePair()
    {
        // Reverse insertion order: dispatch order is the slot ID order.
        ScheduleEvent(High, false, 100, 0, 20);
        ScheduleEvent(Low, false, 100, 0, 10);
    }

    void Advance(u64 timestamp)
    {
        // Nonperiodic scheduling in callbacks uses the active CPU clock.
        ARM7Timestamp = timestamp;
        ARM9Timestamp = timestamp << ARM9ClockShift;
        RunSystem(timestamp);
    }

    bool Active(u32 id) const { return (SchedListMask & (1u << id)) != 0; }

    void Check(bool ok, const char* reason)
    {
        if (!ok)
        {
            ++Failures;
            std::fprintf(stderr, "scheduler/%s: %s\n", CaseName, reason);
        }
    }

    void Expect(std::initializer_list<Call> expected, const char* reason)
    {
        const bool ok = Calls == std::vector<Call>(expected);
        Check(ok, reason);
        if (!ok)
        {
            std::fprintf(stderr, "  actual callbacks:");
            for (const auto& call : Calls)
                std::fprintf(stderr, " %c(%u)", call.Source, call.Param);
            std::fprintf(stderr, "\n");
        }
    }

    void End()
    {
        const bool ok = Failures == CaseStart;
        if (!ok) ++FailedCases;
        std::printf("scheduler/%s: %s\n", CaseName, ok ? "PASS" : "FAIL");
    }
};
}

int TestSchedulerExecution(NDSArgs&& args)
{
    auto instance = std::make_unique<SchedulerFixture>(std::move(args));
    auto& nds = *instance;
    using Action = SchedulerFixture::Action;

    nds.Begin("no-cancel-control", Action::None);
    nds.SchedulePair();
    nds.Advance(99);
    nds.Expect({}, "an event ran before its deadline");
    nds.Advance(100);
    nds.Expect({{'L', 10}, {'H', 20}}, "same-time events did not run in ID order");
    nds.Advance(100);
    nds.Expect({{'L', 10}, {'H', 20}}, "a completed one-shot event ran again");
    nds.Check(!nds.Active(nds.Low) && !nds.Active(nds.High), "one-shot events remain active");
    nds.End();

    nds.Begin("cancel-due", Action::Cancel);
    nds.SchedulePair();
    nds.Advance(100);
    nds.Expect({{'L', 10}}, "the lower ID callback canceled the higher due ID, but it still ran");
    nds.Check(!nds.Active(nds.High), "canceled event remains active");
    nds.End();

    nds.Begin("cancel-reschedule-future", Action::Future);
    nds.SchedulePair();
    nds.Advance(100);
    nds.Expect({{'L', 10}}, "the replacement ran at the canceled deadline");
    nds.Check(nds.Active(nds.High) && nds.SchedList[nds.High].Timestamp == 120,
              "the future replacement was consumed or lost its deadline");
    nds.Advance(119);
    nds.Expect({{'L', 10}}, "the replacement ran before its new deadline");
    nds.Advance(120);
    nds.Expect({{'L', 10}, {'R', 30}}, "the future replacement callback/parameter was lost");
    nds.Check(!nds.Active(nds.High), "the future replacement was not consumed");
    nds.End();

    nds.Begin("replace-same-time", Action::Replace);
    nds.SchedulePair();
    nds.Advance(100);
    nds.Expect({{'L', 10}, {'R', 40}}, "an initially active slot must use its current due replacement");
    nds.Advance(100);
    nds.Expect({{'L', 10}, {'R', 40}}, "the same-time replacement ran twice");
    nds.Check(!nds.Active(nds.High), "the same-time replacement remains active");
    nds.End();

    nds.Begin("periodic-snapshot", Action::None);
    nds.ScheduleEvent(nds.High, false, 100, 2, 1);
    nds.Advance(109);
    nds.Expect({{'P', 1}}, "an overdue periodic event ran more than once in one pass");
    nds.Check(nds.Active(nds.High) && nds.SchedList[nds.High].Timestamp == 104,
              "periodic scheduling must use the previous deadline, not dispatch time");
    nds.Advance(109);
    nds.Expect({{'P', 1}, {'P', 2}}, "the pending periodic event was lost or drained in one pass");
    nds.Advance(109);
    nds.Expect({{'P', 1}, {'P', 2}, {'P', 3}}, "periodic catch-up changed the one-call-per-pass contract");
    nds.Check(nds.Active(nds.High) && nds.SchedList[nds.High].Timestamp == 112,
              "periodic catch-up did not retain its next deadline");
    nds.Advance(111);
    nds.Expect({{'P', 1}, {'P', 2}, {'P', 3}}, "periodic event ran before its next deadline");
    nds.End();

    nds.Begin("new-event-snapshot", Action::Publish);
    nds.SchedulePair();
    nds.Advance(100);
    nds.Expect({{'L', 10}, {'H', 20}}, "a newly active slot ran in the original snapshot");
    nds.Check(nds.Active(nds.Before) && nds.Active(nds.After), "new events were not left pending");
    nds.Advance(100);
    nds.Expect({{'L', 10}, {'H', 20}, {'B', 50}, {'A', 60}},
               "new events did not run in ID order on the next pass");
    nds.Check(!nds.Active(nds.Before) && !nds.Active(nds.After), "new one-shot events remain active");
    nds.End();

    std::printf("scheduler execution: %u/6 cases passed\n", 6 - nds.FailedCases);
    return nds.Failures ? 1 : 0;
}
