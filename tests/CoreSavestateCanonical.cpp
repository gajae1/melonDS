// SPDX-License-Identifier: GPL-3.0-or-later
// Dead command history must not change a state, but live queues and clocks must.
#include "Args.h"
#include "NDS.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace melonDS;

int TestCanonicalSavestate(NDSArgs&& args)
{
    struct Fixture : NDS { using NDS::NDS; using NDS::IPCFIFO9; };
    auto nds = std::make_unique<Fixture>(std::move(args));
    nds->Reset();
    auto& gpu = nds->GPU.GPU3D;
    unsigned failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("canonical-state %s: %s\n", name, ok ? "PASS" : "FAIL");
        failures += !ok;
    };
    const auto save = [&]() {
        Savestate state;
        if (!nds->DoSavestate(&state) || state.Error) return std::vector<u8>{};
        const auto* begin = static_cast<const u8*>(state.Buffer());
        return std::vector<u8>(begin, begin + state.Length());
    };
    const auto load = [&](std::vector<u8>& bytes) {
        Savestate state(bytes.data(), bytes.size(), false);
        return nds->DoSavestate(&state) && !state.Error;
    };
    const auto rawFIFO = [&]() {
        Savestate state;
        state.Section("FIFO");
        gpu.CmdFIFO.DoSavestate(&state);
        state.Finish();
        const auto* begin = static_cast<const u8*>(state.Buffer());
        return std::vector<u8>(begin, begin + state.Length());
    };
    const auto pristine = save();
    if (pristine.empty()) return 2;
    // More than one complete ring, leaving an empty queue with old commands.
    auto entry = gpu.CmdFIFO.Peek();
    for (unsigned i = 0; i < 273; ++i)
    {
        entry._contents = 0;
        entry.Command = 0x20;
        entry.Param = i;
        gpu.CmdFIFO.Write(entry);
        gpu.CmdFIFO.Read();
    }
    const auto history = rawFIFO();
    check(save() == pristine, "empty-command-history");
    check(rawFIFO() == history, "saving-does-not-mutate-live-fifo");
    gpu.CmdFIFO = {};
    for (auto id : {Event_CartROMTransfer9, Event_CartROMTransfer7,
                    Event_DSi_Cart2ROMTransfer9, Event_DSi_Cart2ROMTransfer7})
        nds->SchedList[id].Timestamp = 1234567;
    check(save() == pristine, "inactive-rom-transfer-history");
    check(nds->SchedList[Event_CartROMTransfer9].Timestamp == 1234567,
          "saving-does-not-mutate-live-event");

    // Build an old-format FIFO history using its real serializer, then insert
    // that payload into GP3D. No hard-coded NDSG/GP3D offsets or format changes.
    auto old = pristine;
    Savestate source(const_cast<u8*>(history.data()), history.size(), false);
    source.Section("FIFO");
    Savestate destination(old.data(), old.size(), false);
    destination.Section("GP3D");
    std::memcpy(old.data() + destination.Length(), history.data() + source.Length(),
                history.size() - source.Length());
    check(load(old) && rawFIFO() == history, "old-empty-fifo-load");
    check(save() == pristine, "old-history-resaves-canonically");

    // Keep a scheduled ROM deadline, and a dormant periodic clock's anchor.
    check(load(old), "restore-for-clocks");
    nds->ScheduleEventAt(Event_CartROMTransfer9, 987654, 0, 0);
    nds->CancelEvent(Event_RTC);
    nds->SchedList[Event_RTC].Timestamp = 777;
    nds->SchedList[Event_CartSPITransfer9].Timestamp = 456789;
    auto clocks = save();
    check(load(clocks) && nds->SchedList[Event_CartROMTransfer9].Timestamp == 987654 &&
          nds->EventScheduled(Event_CartROMTransfer9), "active-rom-deadline");
    check(nds->SchedList[Event_CartSPITransfer9].Timestamp == 456789,
          "spi-completion-time-preserved");
    nds->ScheduleEvent(Event_RTC, true, 23, 0, 0);
    check(nds->SchedList[Event_RTC].Timestamp == 800, "dormant-periodic-anchor");

    // Compare real GPU execution after loading either empty representation:
    // fill/wrap the FIFO, overflow into the stall queue, drain, and refill.
    // Include register readback, IRQ/CPU stall state and the complete state.
    const auto exercise = [&]() {
        nds->ARM9Write16(0x04000304, 0x000C);
        gpu.Write32(0x04000600, 2u << 30);
        for (unsigned i = 0; i < 300; ++i)
            gpu.Write32(0x04000480, i & 0x7FFF); // COLOR
        check(gpu.CmdFIFO.IsFull() && !gpu.CmdStallQueue.IsEmpty() &&
              (nds->CPUStop & CPUStop_GXStall), "fifo-overflow-stalls-cpu");
        auto pending = save();
        const auto occupied = rawFIFO();
        check(load(pending) && rawFIFO() == occupied, "occupied-fifo-roundtrip");
        nds->ARM9Timestamp += 20000;
        gpu.Run();
        check(gpu.CmdFIFO.IsEmpty() && gpu.CmdStallQueue.IsEmpty() && gpu.CmdPIPE.IsEmpty() &&
              !(nds->CPUStop & CPUStop_GXStall), "fifo-drain-unstalls-cpu");
        gpu.Write32(0x04000580, 0xBF7F0000); // VIEWPORT after the drain
        nds->ARM9Timestamp += 20000;
        gpu.Run();
        check((gpu.Read32(0x04000600) & (1u << 26)) &&
              (nds->IF[0] & (1u << IRQ_GXFIFO)), "empty-status-and-irq");
        return save();
    };
    auto canonical = pristine;
    check(load(old), "load-old-for-continuation");
    const auto oldContinuation = exercise();
    check(load(canonical), "load-canonical-for-continuation");
    check(exercise() == oldContinuation, "same-complete-continuation");

    // IPC intentionally exposes its last read value on underflow. It must
    // retain history even though the GPU command queue can discard it.
    check(load(canonical), "restore-for-ipc");
    nds->IPCFIFO9.Write(0x12345678);
    nds->IPCFIFO9.Read();
    auto ipc = save();
    check(load(ipc) && nds->IPCFIFO9.Read() == 0x12345678, "ipc-underflow-preserved");
    std::printf("canonical-state: %u failures\n", failures);
    return failures ? 1 : 0;
}
