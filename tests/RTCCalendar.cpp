// SPDX-License-Identifier: GPL-3.0-or-later
// The full production RTC and extracted current NDS scheduler definitions run
// here. The NDS shell supplies only clocks, an IRQ sink and host persistence;
// it does not execute CPUs or open firmware, rtc.bin, or any user data.
// Contracts: S-35190A datasheet pp. 14, 23, 26-27, 32 (S-35180 compatible).
// https://www.ablic.com/en/doc/datasheet/real_time_clock/S35190A_E.pdf
// https://problemkaputt.de/gbatek.htm#dsrealtimeclockrtc
// Normal serial transactions follow the waveform of libnds rtcTransaction:
// https://github.com/devkitPro/libnds/blob/v1.8.0/source/arm7/clock.c
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>
#include "Platform.h"

namespace melonDS
{
constexpr u32 Event_RTC = 3;
constexpr u32 Event_MAX = 4; // Only RTC is registered in this fixture.
constexpr u32 IRQ_RTC = 7;
constexpr u32 MaxEventFunctions = 3;
using EventFunc = void (*)(void*, u32);
struct SchedEvent
{
    std::array<EventFunc, MaxEventFunctions> Funcs;
    void* That;
    u64 Timestamp;
    u32 FuncID;
    u32 Param;
};

class NDS
{
public:
    SchedEvent SchedList[Event_MAX] {};
    u32 SchedListMask = 0;
    u64 SysTimestamp = 0, ARM9Timestamp = 0, ARM7Timestamp = 0;
    u64 ARM9Target = UINT64_MAX, ARM7Target = UINT64_MAX;
    u32 CurCPU = 1, ARM9ClockShift = 1;
    u32 ConsoleType = 0;
    u16 RCnt = 0x8100;
    void* UserData = nullptr;
    unsigned IRQCount = 0;

    void RegisterEventFuncs(u32 id, void* that, const std::initializer_list<EventFunc>& funcs);
    void UnregisterEventFuncs(u32 id);
    void ScheduleEvent(u32 id, bool periodic, s32 delay, u32 funcid, u32 param);
    void ScheduleEventAt(u32 id, u64 timestamp, u32 funcid, u32 param);
    void Reschedule(u64 target);
    void RunSystem(u64 timestamp);
    void CancelEvent(u32 id);
    void SetIRQ(u32 cpu, u32 irq)
    {
        if (cpu != 1 || irq != IRQ_RTC)
            throw std::runtime_error("Unexpected IRQ destination");
        ++IRQCount;
    }
};

using Platform::Log;
using Platform::LogLevel;
#include "RTCScheduleEvent.inc"
#include "RTCScheduleEventAt.inc"
#include "RTCRegisterEventFuncs.inc"
#include "RTCUnregisterEventFuncs.inc"
#include "RTCReschedule.inc"
#include "RTCRunSystem.inc"
#include "RTCCancelEvent.inc"
}

// Include the real RTC with the minimal NDS shell above, instead of pulling in
// unrelated CPU/GPU/cart implementations. No RTC method is copied or stubbed.
#define NDS_H
#define MakeEventThunk(type, func) [](void* that, melonDS::u32 param) { static_cast<type*>(that)->func(param); }
#include "../src/RTC.cpp"
#undef MakeEventThunk
#undef NDS_H

using namespace melonDS;
using DateTime = std::array<int, 6>;
static int Failures = 0;
static void Check(bool condition, const char* message)
{
    if (!condition) { ++Failures; std::fprintf(stderr, "%s\n", message); }
}

namespace melonDS::Platform
{
void Log(LogLevel, const char*, ...) {}
void WriteDateTime(int year, int month, int day, int hour, int minute, int second, void* userdata)
{
    *static_cast<DateTime*>(userdata) = {year, month, day, hour, minute, second};
}
}

struct Fixture
{
    NDS Host;
    RTC Clock {Host};
    DateTime Saved {};

    Fixture()
    {
        Host.UserData = &Saved;
        Clock.Reset();
        Clock.SetDateTime(2024, 2, 28, 0, 0, 0);
    }

    void Ticks(unsigned count)
    {
        for (unsigned i = 0; i < count; ++i)
        {
            if (!(Host.SchedListMask & (1 << Event_RTC)))
                throw std::runtime_error("RTC event was not rearmed");
            const u64 next = Host.SchedList[Event_RTC].Timestamp;
            if (next <= Host.SysTimestamp)
                throw std::runtime_error("RTC event did not advance");
            Host.ARM7Timestamp = next;
            Host.RunSystem(next);
        }
    }

    RTC::StateData State() const
    {
        RTC::StateData state {};
        Clock.GetState(state);
        return state;
    }

    DateTime Time() const
    {
        DateTime time;
        Clock.GetDateTime(time[0], time[1], time[2], time[3], time[4], time[5]);
        return time;
    }

    void SetTime(const DateTime& time)
    {
        Clock.SetDateTime(time[0], time[1], time[2], time[3], time[4], time[5]);
    }

    void Begin(u8 command)
    {
        Clock.Write(0x72, true); // CS low, SCK high, write direction
        Clock.Write(0x76, true); // CS rising while SCK stays high
        Send(command);         // Forward (LSB first) command encoding
    }

    void Send(u8 value, bool repeatLow = false)
    {
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            const u16 low = 0x74 | ((value >> bit) & 1);
            Clock.Write(low, true);
            if (repeatLow) Clock.Write(low, true);
            Clock.Write(low | 2, true);
        }
    }

    u8 Receive(bool repeatLow = false)
    {
        u8 value = 0;
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            Clock.Write(0x64, true);
            if (repeatLow) Clock.Write(0x64, true);
            Clock.Write(0x66, true);
            value |= (Clock.Read() & 1) << bit;
        }
        return value;
    }

    void End() { Clock.Write(0x72, true); }

    void WriteRegister(u8 command, std::initializer_list<u8> values)
    {
        Begin(command);
        for (u8 value : values) Send(value);
        End();
    }

    u8 ReadRegister(u8 command)
    {
        Begin(command);
        u8 result = Receive();
        End();
        return result;
    }
};

static void Calendar()
{
    struct Boundary { DateTime Before, After; };
    const Boundary boundaries[] = {
        {{2023, 2, 28, 23, 59, 59}, {2023, 3, 1, 0, 0, 0}},
        {{2024, 2, 28, 23, 59, 59}, {2024, 2, 29, 0, 0, 0}},
        {{2000, 2, 29, 23, 59, 59}, {2000, 3, 1, 0, 0, 0}},
        {{2024, 4, 30, 23, 59, 59}, {2024, 5, 1, 0, 0, 0}},
        {{2099, 12, 31, 23, 59, 59}, {2000, 1, 1, 0, 0, 0}},
    };
    for (const auto& boundary : boundaries)
    {
        Fixture f;
        f.SetTime(boundary.Before);
        using namespace std::chrono;
        const auto& b = boundary.Before;
        const auto weekday = std::chrono::weekday(sys_days(year(b[0]) / b[1] / b[2])).c_encoding();
        Check(f.State().DateTime[3] == weekday, "Host date produced the wrong weekday");
        f.Ticks(32767);
        Check(f.Time() == boundary.Before, "Calendar advanced before the second boundary");
        f.Ticks(1);
        Check(f.Time() == boundary.After, "Calendar carry/leap/year-wrap failed");
        Check(f.State().DateTime[3] == (weekday + 1) % 7, "Weekday counter failed across midnight");
        Check(f.Host.SysTimestamp == 33513982, "RTC second did not preserve scheduler timing");
    }
}

static void HourModes()
{
    for (int hour : {0, 11, 12, 19, 20, 23})
    {
        Fixture f;
        f.Clock.SetDateTime(2024, 2, 28, hour, 58, 59);
        const auto before = f.Time();
        f.WriteRegister(0x06, {0});
        Check(f.Time() == before, "24-to-12-hour conversion changed the time");
        const u8 encoded = f.State().DateTime[4];
        const int decoded = (encoded & 0xF) + 10 * ((encoded >> 4) & 3);
        Check(decoded == hour % 12 && bool(encoded & 0x40) == (hour >= 12),
              "12-hour BCD/PM encoding failed");
        f.SetTime(before);
        Check(f.Time() == before, "Host time setter failed in 12-hour mode");
        f.WriteRegister(0x06, {2});
        Check(f.Time() == before, "12-to-24-hour conversion changed the time");
    }
    for (int hour : {11, 23})
    {
        Fixture f;
        f.WriteRegister(0x06, {0});
        f.Clock.SetDateTime(2024, 2, 28, hour, 59, 59);
        f.Ticks(32768);
        const DateTime expected = hour == 11 ? DateTime{2024, 2, 28, 12, 0, 0}
                                             : DateTime{2024, 2, 29, 0, 0, 0};
        Check(f.Time() == expected, "12-hour noon/midnight carry failed");
    }
}

static void BCDRegisters()
{
    Fixture f;
    f.WriteRegister(0x26, {0x24, 0x02, 0x29, 0x04, 0x23, 0x59, 0x58});
    Check(f.Time() == DateTime{2024, 2, 29, 23, 59, 58}, "Normal BCD register write failed");
    Check(f.Saved == f.Time(), "Completed date write did not notify host with full date");
    f.Begin(0xA6);
    const std::array<u8, 7> expected {0x24, 0x02, 0x29, 0x04, 0x63, 0x59, 0x58};
    for (u8 byte : expected) Check(f.Receive() == byte, "Normal serial date read failed");
    f.End();
    f.WriteRegister(0x26, {0x24, 0x04, 0x31, 0x03, 0x12, 0x34, 0x56});
    Check(f.Time() == DateTime{2024, 5, 1, 12, 34, 56}, "Nonexistent month-end did not carry");
    // The datasheet explicitly specifies these nonexistent fields. Invalid
    // seconds have separate deferred-carry semantics, not asserted here.
    f.WriteRegister(0x26, {0xFA, 0x19, 0x39, 0x07, 0x2A, 0x6A, 0x00});
    Check(f.Time() == DateTime{2000, 1, 1, 0, 0, 0}, "Invalid BCD fields were not sanitized");
    Check(f.State().DateTime[3] == 0, "Invalid weekday was not sanitized");
}

static void SerialInput()
{
    Fixture f;
    f.WriteRegister(0x76, {0xA5});
    Check(f.ReadRegister(0xF6) == 0xA5, "Normal free-register control failed");
    f.Begin(0x76);
    f.Send(0x3C, true);
    f.End();
    Check(f.ReadRegister(0xF6) == 0x3C, "Repeated low-clock writes consumed input bits");

    f.WriteRegister(0x76, {0xA5});
    f.Begin(0x76);
    for (unsigned bit = 0; bit < 7; ++bit)
    {
        const u16 low = 0x74 | ((0x3C >> bit) & 1);
        f.Clock.Write(low, true);
        f.Clock.Write(low | 2, true);
    }
    f.Clock.Write(0x74, true); // Eighth bit set up; no sampling edge yet.
    Check(f.State().FreeReg == 0xA5, "Input byte committed before the eighth rising edge");
    f.Clock.Write(0x76, true);
    Check(f.State().FreeReg == 0x3C, "Input byte did not commit on the eighth rising edge");
    f.End();
}

static void SerialOutput()
{
    Fixture f;
    f.WriteRegister(0x76, {0xA5});
    Check(f.ReadRegister(0xF6) == 0xA5, "Normal output control failed");
    f.Begin(0xF6);
    Check(f.Receive(true) == 0xA5, "Repeated low-clock writes consumed output bits");
    f.End();
    Check(f.ReadRegister(0xF6) == 0xA5, "New CS transaction did not restore byte alignment");
}

static void AlarmIRQ()
{
    Fixture f;
    f.Clock.SetDateTime(2024, 2, 29, 13, 4, 59);
    f.WriteRegister(0x46, {0x44}); // Both alarms, initially mismatching minute.
    f.WriteRegister(0x16, {0, 0xD3, 0x85});
    f.WriteRegister(0x56, {0, 0xD3, 0x85});
    Check(f.Host.IRQCount == 0, "Alarm fired before matching minute");
    f.Ticks(32768);
    Check(f.Host.IRQCount == 1 && (f.State().IRQFlag & 0x30) == 0x30,
          "Simultaneous alarms did not share one IRQ edge");
    Check((f.ReadRegister(0x86) & 0x30) == 0x30, "Alarm status flags were not readable");
    Check((f.ReadRegister(0x86) & 0x30) == 0, "Alarm status flags did not clear on read");
    f.WriteRegister(0x46, {0x40});
    Check((f.State().IRQFlag & 0x30) == 0x20 && f.Host.IRQCount == 1,
          "Clearing INT1 disturbed the asserted INT2 line");
    f.WriteRegister(0x46, {0});
    Check((f.State().IRQFlag & 0x30) == 0, "Disabling both alarms left the IRQ line asserted");
    f.Host.RCnt = 0x8000;
    f.WriteRegister(0x46, {0x40});
    f.Clock.SetDateTime(2024, 2, 29, 13, 4, 59);
    f.Ticks(32768);
    Check(f.Host.IRQCount == 1 && (f.State().IRQFlag & 0x20),
          "RCNT gating lost the alarm level or generated a disabled CPU IRQ");
}

static void PeriodicIRQ()
{
    Fixture f;
    f.Clock.SetDateTime(2024, 2, 28, 12, 34, 59);
    f.WriteRegister(0x46, {7});
    f.Ticks(32768);
    Check(f.Host.IRQCount == 1, "Short minute pulse did not start on carry");
    f.Ticks(255);
    Check((f.State().IRQFlag & 0x10) != 0, "Short minute pulse ended before 256 ticks");
    f.Ticks(1);
    Check((f.State().IRQFlag & 0x10) == 0, "Short minute pulse did not end at 256 ticks");

    Fixture steady;
    steady.Clock.SetDateTime(2024, 2, 28, 12, 34, 59);
    steady.WriteRegister(0x46, {3});
    steady.Ticks(32768 * 30);
    Check((steady.State().IRQFlag & 0x10) != 0, "30-second pulse ended early");
    steady.Ticks(32768);
    Check((steady.State().IRQFlag & 0x10) == 0, "30-second pulse did not end at second 30");
}

static void MinuteRearm()
{
    // Datasheet Figures 30-32: disabling and reenabling within 7.81 ms of the
    // minute carry asserts INT again; after that window it waits a minute.
    for (u8 mode : {2, 3, 7})
    {
        Fixture f;
        f.Clock.SetDateTime(2024, 2, 28, 12, 34, 59);
        f.WriteRegister(0x46, {mode});
        f.Ticks(32768);
        Check(f.Host.IRQCount == 1, "Minute rearm control did not get the initial edge");
        f.WriteRegister(0x46, {0});
        f.Ticks(128); // 3.90625 ms, well inside the documented carry window.
        f.WriteRegister(0x46, {mode});
        Check(f.Host.IRQCount == 2 && (f.State().IRQFlag & 0x10),
              "Minute IRQ did not reassert inside the 7.81 ms carry window");
        f.WriteRegister(0x46, {0});
        f.Ticks(128);
        f.WriteRegister(0x46, {mode});
        Check((f.State().IRQFlag & 0x10) == 0,
              "Minute IRQ reasserted after the carry window ended");
    }
}

static void ResetBus()
{
    std::vector<u8> resetStates[2];
    for (unsigned sample = 0; sample < 2; ++sample)
    {
        Fixture f;
        f.Clock.SetDateTime(2024, 2, 29, 23, 59, 58);
        auto battery = f.State();
        battery.StatusReg1 = 0xB6;
        battery.StatusReg2 = 0x44;
        battery.Alarm1[0] = 0x84;
        battery.Alarm1[1] = 0xE3;
        battery.Alarm1[2] = 0xD9;
        battery.Alarm2[2] = 0x80;
        battery.FreeReg = 0xA5;
        battery.ClockAdjust = 0x13;
        battery.IRQFlag = 0x31;
        battery.MinuteCount = 0x123456;
        battery.FOUT1 = 0x80;
        battery.FOUT2 = 0x21;
        battery.AlarmDate1[0] = 0x24;
        battery.AlarmDate1[1] = 0xC2;
        battery.AlarmDate2[2] = 0x99;
        f.Clock.SetState(battery);

        // Poison only the serial state through defined public operations.
        // Raw placement-new over poisoned storage would leave indeterminate
        // members in the baseline, making its observed failures depend on UB.
        f.Begin(0xF6);
        for (unsigned bit = 0; bit < 1 + 4 * sample; ++bit)
        {
            f.Clock.Write(0x64, true);
            f.Clock.Write(0x66, true);
        }
        const u16 poisonIO = sample ? 0x5A74 : 0xA570;
        f.Clock.Write(poisonIO, false); // CS high/low, distinct high byte.

        RTC::StateData before {}, after {};
        f.Clock.GetState(before);
        const DateTime beforeTime = f.Time();
        const DateTime beforeSaved = f.Saved;
        // Match NDS reset: the scheduler is cleared before RTC is rearmed.
        f.Host.CancelEvent(Event_RTC);
        f.Clock.Reset();
        f.Clock.GetState(after);
        Check(std::memcmp(&before, &after, sizeof(before)) == 0,
              "Bus reset changed battery date/alarm/IRQ state");
        Check(f.Time() == beforeTime && f.Saved == beforeSaved && f.Host.IRQCount == 0,
              "Bus reset changed the saved date or generated a host/IRQ side effect");

        const u16 firstRead = f.Clock.Read();
        Check(firstRead == 0, "Reset retained poisoned IO instead of a deselected bus");
        Savestate snapshot(256);
        f.Clock.DoSavestate(&snapshot);
        snapshot.Finish();
        if (snapshot.Error) throw std::runtime_error("Reset snapshot could not be saved");
        const auto* bytes = static_cast<const u8*>(snapshot.Buffer());
        resetStates[sample].assign(bytes, bytes + snapshot.Length());

        // Begin from the reset bus without an extra CS-low write that could
        // hide a stale select bit. Byte access must not inherit poisoned bits.
        f.Clock.Write(0x76, true);
        const u16 firstByte = f.Clock.Read();
        Check(firstByte == 0x76, "First byte write inherited pre-reset upper IO bits");
        f.Send(0xF6);
        const u8 received = f.Receive();
        f.End();
        Check(received == 0xA5, "First CS assertion after Reset lost command alignment");
        std::printf("poison=%04X partial-read-bits=%u first-read=%04X first-byte=%04X data=%02X\n",
                    poisonIO, 1 + 4 * sample, firstRead, firstByte, received);
    }
    // The serialized bus includes the partial output-bit position. Comparing
    // both complete snapshots catches it even when a subsequent CS hides it.
    Check(resetStates[0] == resetStates[1], "Reset snapshots retained different serial positions");
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string name = argv[1];
    try
    {
        if (name == "calendar") Calendar();
        else if (name == "hour-modes") HourModes();
        else if (name == "bcd") BCDRegisters();
        else if (name == "serial-input") SerialInput();
        else if (name == "serial-output") SerialOutput();
        else if (name == "alarm-irq") AlarmIRQ();
        else if (name == "periodic-irq") PeriodicIRQ();
        else if (name == "minute-rearm") MinuteRearm();
        else if (name == "reset-bus") ResetBus();
        else return 2;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Fixture error: %s\n", error.what());
        return 2;
    }
    std::printf("%s: %s (%d failures)\n", name.c_str(), Failures ? "FAIL" : "PASS", Failures);
    return Failures ? 1 : 0;
}
