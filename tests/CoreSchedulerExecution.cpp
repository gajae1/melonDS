// SPDX-License-Identifier: GPL-3.0-or-later
// Generated callbacks exercise the actual NDS scheduler. No external ROM/BIOS,
// hardware event timing oracle, or copy of the dispatch implementation.
#include "Args.h"
#include "DSi.h"
#include "DSP_HLE/G711Ucode.h"
#include "NDS.h"

#include <array>
#include <cstdio>
#include <cstring>
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

// Generated DSP state enters the production savestate factory, then guest MMIO
// starts a real G711 command. This covers HLE recreation/dispatch, not SDK
// program CRC recognition, BIOS boot, codec accuracy, or frontend undo.
int TestDSiHLESavestate(NDSArgs&& args)
{
    DSiArgs dsiArgs;
    static_cast<NDSArgs&>(dsiArgs) = std::move(args);
    dsiArgs.DSPHLE = true;
    auto instance = std::make_unique<DSi>(std::move(dsiArgs));
    auto& dsi = *instance;
    dsi.Reset();
    dsi.CurCPU = 1;
    dsi.MapNWRAM_C(0, 0x82); // DSP data page for the production pipe setup.
    dsi.DSP.SetRstLine(true);
    for (u32 id = 0; id < Event_MAX; ++id) dsi.CancelEvent(id);
    // Park both actual CPUs in generated ARM loops while RunFrame drives the
    // production scheduler. DSi is final; no private/protected pointer tricks.
    dsi.ARM9Write32(0x02000000, 0xEAFFFFFE);
    dsi.ARM9Write32(0x02000004, 0xEAFFFFFE);
    dsi.ARM9.JumpTo(0x02000000);
    dsi.ARM7.JumpTo(0x02000004);
    dsi.Start();

    unsigned failures = 0;
    const auto check = [&](bool ok, const char* reason) {
        if (!ok)
        {
            ++failures;
            std::fprintf(stderr, "dsi-hle-savestate: %s\n", reason);
        }
        return ok;
    };

    // No program blob, forged CRC, private DSPCore access, callback injection,
    // or copied HLE state layout. Serialize the real reset/start implementation.
    // DSPi ends with the core ID; replace its no-core marker with GetID(), then
    // append DSPH through the production serializer before loading this seed.
    Savestate seed(1024);
    dsi.DSP.DoSavestate(&seed);
    if (!check(!seed.Error && seed.Length() >= sizeof(u32), "DSP seed save")) return 1;
    const u32 idOffset = seed.Length() - sizeof(u32);
    u32 emptyID = 0;
    std::memcpy(&emptyID, static_cast<u8*>(seed.Buffer()) + idOffset, sizeof(emptyID));
    if (!check(emptyID == 0xFFFFFFFF, "DSPi seed must end with the no-core marker")) return 1;
    {
        DSP_HLE::G711Ucode ucode(dsi, 0x10);
        ucode.Reset();
        ucode.Start();
        check(ucode.RecvData(0) == 1 && ucode.RecvData(1) == 1 &&
              ucode.RecvData(2) == 1 && ucode.RecvData(2) == 0x0800, "G711 start handshake");
        const u32 id = ucode.GetID();
        std::memcpy(static_cast<u8*>(seed.Buffer()) + idOffset, &id, sizeof(id));
        ucode.DoSavestate(&seed);
    } // Real destructor unregisters; only the actual loader creates the owner.
    seed.Finish();
    Savestate seedLoad(seed.Buffer(), seed.Length(), false);
    dsi.DSP.DoSavestate(&seedLoad);
    if (!check(!seed.Error && !seedLoad.Error && dsi.SchedList[Event_DSi_DSPHLE].Funcs[0],
               "production DSP loader did not create G711")) return 1;

    constexpr u32 Source = 0x02001000, Dest = 0x02002000;
    constexpr u32 PipeMonitor = 0x0800;
    constexpr u16 Sentinel = 0x5A5A;
    const auto readData = [&](u32 word) { return dsi.DSP.DSPRead16(0x40000 + word * 2); };
    const auto writeData = [&](u32 word, u16 value) { dsi.DSP.DSPWrite16(0x40000 + word * 2, value); };
    const std::array<u8, 4> input{0xFE, 0x7E, 0x80, 0x00};
    for (u32 i = 0; i < input.size(); ++i)
    {
        dsi.ARM9Write8(Source + i, input[i]);
        dsi.ARM9Write16(Dest + i * 2, Sentinel);
    }
    const u16 command[] = {0, 2, Source >> 16, Source & 0xFFFF,
                           Dest >> 16, Dest & 0xFFFF, 0, u16(input.size())};
    const u32 commandPipe = PipeMonitor + 7 * 5;
    const u32 commandBuffer = readData(commandPipe);
    for (u32 i = 0; i < std::size(command); ++i) writeData(commandBuffer + i, command[i]);
    writeData(commandPipe + 3, sizeof(command));
    dsi.ARM9Write16(0x04004330, 7); // CMD2 -> G711::TryStartCmd -> ScheduleEvent.
    auto& event = dsi.SchedList[Event_DSi_DSPHLE];
    if (!check(event.Timestamp > 0 && event.FuncID == 0 && event.Param == 0,
               "real G711 command did not schedule FinishCmd")) return 1;
    const u64 deadline = event.Timestamp;
    check(dsi.ARM9Read16(0x04004334) == 7, "command pipe acknowledgement");
    dsi.ARM9Write16(0x04004318, 0xFFFF); // Clear its semaphore/IRQ before completion.
    dsi.IF[0] = 0;

    Savestate saved, invalidID, unregistered;
    if (!check(dsi.NDS::DoSavestate(&saved) && !saved.Error, "normal full-state save")) return 1;
    // Corrupt only the serialized function ID of an event created by production.
    // Never dispatch while the live metadata is temporarily invalid.
    event.FuncID = MaxEventFunctions;
    const bool savedInvalid = dsi.NDS::DoSavestate(&invalidID) && !invalidID.Error;
    event.FuncID = 1; // In bounds, but G711 registers only function zero.
    const bool savedUnregistered = dsi.NDS::DoSavestate(&unregistered) && !unregistered.Error;
    event.FuncID = 0;
    if (!check(savedInvalid && savedUnregistered, "malformed full-state generation")) return 1;

    const auto pcm = [&] {
        std::array<u16, 4> output{};
        for (u32 i = 0; i < output.size(); ++i) output[i] = dsi.ARM9Read16(Dest + i * 2);
        return output;
    };
    const auto complete = [&] {
        const u32 responsePipe = PipeMonitor + 6 * 5;
        const u32 responseBuffer = readData(responsePipe);
        check(readData(responsePipe + 3) == 4 && readData(responseBuffer) == 0 &&
              readData(responseBuffer + 1) == input.size(), "completion pipe length/response");
        check((dsi.IF[0] & (1u << IRQ_DSi_DSP)) && dsi.ARM9Read16(0x04004334) == 6,
              "completion DSP IRQ/reply");
        const auto output = pcm();
        dsi.RunFrame();
        check(pcm() == output && readData(responsePipe + 3) == 4,
              "completion wrote output/reply twice");
        return output;
    };
    const unsigned controlStart = failures;
    check(pcm() == std::array<u16, 4>{Sentinel, Sentinel, Sentinel, Sentinel}, "early control dispatch");
    dsi.RunFrame();
    const auto control = complete();
    check(control != std::array<u16, 4>{Sentinel, Sentinel, Sentinel, Sentinel}, "control wrote no PCM");
    std::printf("dsi-hle/control: %s deadline=%llu PCM=%04X,%04X,%04X,%04X state_bytes=%u\n",
        failures == controlStart ? "PASS" : "FAIL", (unsigned long long)deadline,
        control[0], control[1], control[2], control[3], saved.Length());

    struct Case { const char* Name; Savestate* State; bool Accepted; bool ExistingCore; };
    const Case cases[] = {
        {"restore-stopped", &saved, true, false},
        {"replace-pending-core", &saved, true, true},
        {"invalid-funcid", &invalidID, false, false},
        {"unregistered-funcid", &unregistered, false, false},
    };
    for (const auto& test : cases)
    {
        const unsigned before = failures;
        dsi.DSP.StopDSP();
        for (u32 id = 0; id < Event_MAX; ++id) dsi.CancelEvent(id);
        check(!event.That && !event.Funcs[0], "StopDSP left a callback registered");
        if (test.ExistingCore)
        {
            Savestate pending(saved.Buffer(), saved.Length(), false);
            if (!check(dsi.NDS::DoSavestate(&pending) && !pending.Error,
                       "prepare existing pending HLE owner")) return 1;
        }
        const auto previous = event;
        Savestate load(test.State->Buffer(), test.State->Length(), false);
        const bool accepted = dsi.NDS::DoSavestate(&load);
        check(accepted == test.Accepted && !load.Error, "full-state acceptance mismatch");
        if (!test.Accepted)
        {
            check(event.Timestamp == previous.Timestamp &&
                  event.FuncID == previous.FuncID && event.Param == previous.Param,
                  "rejected scheduler metadata became live");
            const bool recreated = test.State == &unregistered;
            check(bool(event.Funcs[0]) == recreated && !event.Funcs[1],
                  "validation ran at the wrong side of DSP recreation");
            Savestate retry(saved.Buffer(), saved.Length(), false);
            if (!check(dsi.NDS::DoSavestate(&retry) && !retry.Error, "valid retry after rejection")) return 1;
        }
        check(event.That && event.Funcs[0] && event.FuncID == 0 &&
              event.Timestamp == deadline && event.Param == 0, "restored scheduler/owner metadata");
        check(pcm() == std::array<u16, 4>{Sentinel, Sentinel, Sentinel, Sentinel}, "early restored dispatch");
        dsi.RunFrame();
        check(complete() == control, "recreated callback differs from uninterrupted G711 command");
        std::printf("dsi-hle/%s: %s accepted=%d expected=%d dispatch_equal=%d\n",
            test.Name, failures == before ? "PASS" : "FAIL", accepted, test.Accepted, pcm() == control);
    }
    std::printf("dsi-hle-savestate: %u failures (generated DSP-state seed; no program CRC/boot acceptance)\n", failures);
    return failures ? 1 : 0;
}
