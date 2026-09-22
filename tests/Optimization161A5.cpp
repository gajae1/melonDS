// SPDX-License-Identifier: GPL-3.0-or-later
// A5 differential fixture: the real LLE Teakra core instantiated through its
// public API, with the actual CoreTiming peripheral set registered (timer0,
// timer1, BTDMP0, BTDMP1), ICU -> DSP interrupt delivery into a real ISR/RETI,
// and the DSi I2S sample clock driving the BTDMP FIFOs. The previous
// interpreter-only probe used a synthetic tick recorder, so it never
// registered or exercised a peripheral and cannot speak about this change.
// Build and run commands live in the A5 evidence directory.
#include "teakra/teakra.h"
#include "Savestate.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s16 = std::int16_t;

void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
void Require(bool ok, const std::string& message)
{
    if (!ok) throw std::runtime_error(message);
}

// ---------------------------------------------------------------------------
// DSP opcode encodings, verified against src/teakra/src/decoder.h and
// src/teakra/src/operand.h in the frozen baseline.
//   nop   INST(nop, 0x0000)
//   reti  INST(reti, 0x45C0, At<Cond, 0>)
//   br    INST(br, 0x4180, At<Address18_16, 16>, At<Address18_2, 4>, At<Cond, 0>)
//         second word holds address bits 0..15, first word bits 16..17.
//   brr   INST(brr, 0x5000, At<RelAddr7, 4>, At<Cond, 0>); signed 7 bit offset
//         relative to the following instruction. -1 means "stay here" and puts
//         the interpreter into its idle/skip state.
//   mov   INST(mov, 0x5E00, At<Imm16, 16>, At<Register, 0>); 5 bit index into
//         the Register operand list, where 8 is st0 and 13 is sp.
//   mov   INST(mov, 0x5800, At<Register, 0>, At<Register, 5>)
// ---------------------------------------------------------------------------
constexpr u16 kNop = 0x0000;
constexpr u16 kReti = 0x45C0;
constexpr unsigned kRegisterSt0 = 8;
constexpr unsigned kRegisterSp = 13;
// st0 bit1 = ie (master enable), bit2 = im[0] (interrupt group 0 mask).
constexpr u16 kSt0EnableInterrupt0 = 0x0006;

constexpr u16 MovRegisterImmediate(unsigned index)
{
    return u16(0x5E00 | (index & 0x1F));
}
constexpr u16 MovRegisterRegister(unsigned destination, unsigned source)
{
    return u16(0x5800 | (source << 5) | destination);
}
constexpr u16 BranchAbsolute(u32 address)
{
    return u16(0x4180 | ((address >> 16) << 4));
}
constexpr u16 BranchRelative(int offset)
{
    return u16(0x5000 | ((u16(offset) & 0x7F) << 4));
}

constexpr u32 kMainEntry = 0x100;
constexpr u32 kVectorInterrupt0 = 0x0006;
constexpr u32 kVectorInterrupt1 = 0x000E;
constexpr u32 kStackTop = 0x7000;

// Steady state of the active program. The register moves are real single word
// instructions, so the measured stream is not a pure nop array.
constexpr u16 kActiveBody[] = {kNop, MovRegisterRegister(0, 1), kNop, MovRegisterRegister(1, 0),
                               kNop, kNop, MovRegisterRegister(2, 3), kNop};
constexpr u32 kActiveLoopWords = 48; // signed 7 bit branch offset, so <= 64
constexpr u32 kIdleRegionWords = 12;

// ---------------------------------------------------------------------------
// MMIO register file offsets, as used by Teakra::MMIOWrite/MMIORead (both mask
// with the MMIO window size). Mirrors src/teakra/src/mmio.cpp.
// ---------------------------------------------------------------------------
constexpr u16 kMmioIcuRequest = 0x200;
constexpr u16 kMmioIcuAcknowledge = 0x202;
constexpr u16 kMmioIcuEnable0 = 0x206;
constexpr u16 kMmioTimerCfg = 0x20;
constexpr u16 kMmioTimerEvent = 0x22;
constexpr u16 kMmioTimerStartLow = 0x24;
constexpr u16 kMmioTimerStartHigh = 0x26;
constexpr u16 kMmioTimerCounterLow = 0x28;
constexpr u16 kMmioTimerCounterHigh = 0x2A;
constexpr u16 kMmioBtdmpRxEnable = 0x29E;
constexpr u16 kMmioBtdmpRxStatus = 0x2C0;
constexpr u16 kMmioBtdmpRxReceive = 0x2C4;
constexpr u16 kMmioBtdmpRxFlush = 0x2C8;
constexpr u16 kMmioBtdmpTxEnable = 0x2BE;
constexpr u16 kMmioBtdmpTxStatus = 0x2C2;
constexpr u16 kMmioBtdmpTxSend = 0x2C6;
constexpr u16 kMmioBtdmpTxFlush = 0x2CA;

constexpr u16 kTimerAutoRestart = 1u << 2;
constexpr u16 kTimerEventCount = 3u << 2;
constexpr u16 kTimerPause = 1u << 8;
constexpr u16 kTimerUpdateMmio = 1u << 9;
constexpr u16 kTimerRestart = 1u << 10;

constexpr u16 kIrqTimer0 = 1u << 10; // icu.TriggerSingle(0xA)
constexpr u16 kIrqTimer1 = 1u << 9;  // icu.TriggerSingle(0x9)
constexpr u16 kIrqBtdmp0 = 1u << 11; // icu.TriggerSingle(0xB)

constexpr u16 kTimer0Period = 997;
constexpr u16 kTimer1Period = 331;

// ---------------------------------------------------------------------------
// Program images. Both share the prologue, the interrupt vectors and the stack
// setup, so interrupt handling is identical; only the steady state differs
// (a real instruction loop versus the interpreter's idle self-branch).
// ---------------------------------------------------------------------------
struct ProgramImage
{
    std::vector<u16> Words;
};

ProgramImage MakeProgram(const u16* body, u32 bodyWords)
{
    ProgramImage image;
    image.Words.assign(0x40000, 0);
    image.Words[0x000] = BranchAbsolute(kMainEntry);
    image.Words[0x001] = u16(kMainEntry & 0xFFFF);
    image.Words[kVectorInterrupt0] = kReti;
    image.Words[kVectorInterrupt1] = kReti;
    image.Words[kMainEntry + 0] = MovRegisterImmediate(kRegisterSp);
    image.Words[kMainEntry + 1] = u16(kStackTop);
    image.Words[kMainEntry + 2] = MovRegisterImmediate(kRegisterSt0);
    image.Words[kMainEntry + 3] = kSt0EnableInterrupt0;
    for (u32 i = 0; i < bodyWords; ++i)
        image.Words[kMainEntry + 4 + i] = body[i];
    return image;
}

ProgramImage ActiveProgram()
{
    u16 body[kActiveLoopWords];
    const u32 bodyCount = sizeof(kActiveBody) / sizeof(kActiveBody[0]);
    for (u32 i = 0; i < kActiveLoopWords - 1; ++i)
        body[i] = kActiveBody[i % bodyCount];
    body[kActiveLoopWords - 1] = BranchRelative(-int(kActiveLoopWords));
    return MakeProgram(body, kActiveLoopWords);
}

ProgramImage IdleProgram()
{
    u16 body[kIdleRegionWords];
    for (u32 i = 0; i < kIdleRegionWords; ++i)
        body[i] = BranchRelative(-1);
    return MakeProgram(body, kIdleRegionWords);
}

// ---------------------------------------------------------------------------
// Machine: one real Teakra instance plus its external shared memory. Shared
// memory is addressed in bytes; word addresses below 0x20000 are program space,
// 0x20000.. is DSP data space (MemoryInterfaceUnit::DataMemoryOffset).
// ---------------------------------------------------------------------------
class Machine
{
public:
    static constexpr u32 kProgramWords = 0x40000;
    static constexpr u32 kDataOffset = 0x20000;
    static constexpr std::size_t kTraceLimit = 2048;

    Machine()
    {
        Words.assign(kProgramWords, 0);
        Dsp.SetSharedMemoryCallback({
            [this](std::uint32_t address) { return Read(address); },
            [this](std::uint32_t address, std::uint16_t value) { Write(address, value); }});
        Dsp.SetMicEnableCallback([this](bool enabled) {
            ++MicEnableEvents;
            MicEnabled = enabled;
        });
    }

    void LoadProgram(const ProgramImage& image)
    {
        Require(image.Words.size() == kProgramWords, "program image size");
        Words.assign(kProgramWords, 0);
        for (u32 i = 0; i < kProgramWords; ++i)
            if (image.Words[i]) Dsp.ProgramWrite(i, image.Words[i]);
        Dsp.Reset();
        ResetTrace();
    }

    void ResetTrace()
    {
        ProgramFetches = 0;
        ProgramWrites = 0;
        DataReads = 0;
        DataWrites = 0;
        HighWrites = 0;
        SampleClocks = 0;
        MicEnableEvents = 0;
        MicEnabled = false;
        FetchTrace.clear();
        WriteTrace.clear();
        Pcm.clear();
    }

    u16 Read(std::uint32_t byteAddress)
    {
        const u32 word = (byteAddress >> 1) & (kProgramWords - 1);
        if (word < kDataOffset)
        {
            ++ProgramFetches;
            if (FetchTrace.size() < kTraceLimit) FetchTrace.push_back(word);
            return Words[word];
        }
        ++DataReads;
        return Words[word];
    }

    void Write(std::uint32_t byteAddress, std::uint16_t value)
    {
        const u32 word = (byteAddress >> 1) & (kProgramWords - 1);
        if (word < kDataOffset) ++ProgramWrites;
        else if (word < kDataOffset * 2) ++DataWrites;
        else ++HighWrites;
        if (WriteTrace.size() < kTraceLimit) WriteTrace.emplace_back(word, value);
        Words[word] = value;
    }

    void Run(u32 cycles) { Dsp.Run(cycles); }

    void Sample(s16 microphone)
    {
        s16 output[2] = {0, 0};
        Dsp.SampleClock(output, microphone);
        Pcm.push_back(output[0]);
        Pcm.push_back(output[1]);
        ++SampleClocks;
    }

    u16 Mmio(u16 address) { return Dsp.MMIORead(address); }
    void MmioWrite(u16 address, u16 value) { Dsp.MMIOWrite(address, value); }
    u16 DataWord(u16 address) const { return Words[kDataOffset + address]; }

    std::vector<u8> SaveState()
    {
        melonDS::Savestate state;
        Dsp.DoSavestate(&state);
        state.Finish();
        Require(!state.Error, "savestate save failed");
        const u8* data = static_cast<const u8*>(state.Buffer());
        return std::vector<u8>(data, data + state.Length());
    }

    void LoadState(const std::vector<u8>& bytes)
    {
        melonDS::Savestate state(const_cast<u8*>(bytes.data()), u32(bytes.size()), false);
        Require(!state.Error, "savestate header rejected");
        Dsp.DoSavestate(&state);
        Require(!state.Error, "savestate load failed");
    }

    Teakra::Teakra Dsp;
    std::vector<u16> Words;

    u64 ProgramFetches = 0;
    u64 ProgramWrites = 0;
    u64 DataReads = 0;
    u64 DataWrites = 0;
    u64 HighWrites = 0;
    u64 SampleClocks = 0;
    u64 MicEnableEvents = 0;
    bool MicEnabled = false;
    std::vector<u32> FetchTrace;
    std::vector<std::pair<u32, u16>> WriteTrace;
    std::vector<s16> Pcm;
};

// ---------------------------------------------------------------------------
// Deterministic byte sink. Check mode writes one stream per build so the
// baseline/candidate comparison is a plain byte diff.
// ---------------------------------------------------------------------------
struct Bytes
{
    std::vector<u8> Data;

    void Half(u16 value)
    {
        Data.push_back(u8(value & 0xFF));
        Data.push_back(u8(value >> 8));
    }
    void Word(u32 value)
    {
        for (unsigned i = 0; i < 4; ++i) Data.push_back(u8(value >> (8 * i)));
    }
    void Tag(const char* text)
    {
        for (const char* p = text; *p; ++p) Data.push_back(u8(*p));
        Data.push_back(0);
    }
    void Halfs(const std::vector<u16>& values)
    {
        for (u16 value : values) Half(value);
    }
    void Samples(const std::vector<s16>& values)
    {
        for (s16 value : values) Half(u16(value));
    }
    void Raw(const std::vector<u8>& bytes)
    {
        Data.insert(Data.end(), bytes.begin(), bytes.end());
    }
};

const char* gOutputPath = nullptr;
const char* OutputPath() { return gOutputPath; }

void WriteFile(const char* path, const Bytes& bytes)
{
    std::FILE* file = std::fopen(path, "wb");
    Require(file != nullptr, std::string("cannot open output file: ") + path);
    const std::size_t written =
        bytes.Data.empty() ? 0 : std::fwrite(bytes.Data.data(), 1, bytes.Data.size(), file);
    std::fclose(file);
    Require(written == bytes.Data.size(), "short output write");
}

// ---------------------------------------------------------------------------
// Host side drive sequence. Steps are indexed so the second half can be replayed
// against a reloaded savestate.
// ---------------------------------------------------------------------------
constexpr unsigned kCheckpointStep = 10;
constexpr unsigned kStepCount = 19;

void ConfigureTimer(Machine& m, unsigned index, u32 start, u16 mode)
{
    const u16 base = u16(kMmioTimerCfg + index * 0x10);
    m.MmioWrite(u16(base + (kMmioTimerStartLow - kMmioTimerCfg)), u16(start & 0xFFFF));
    m.MmioWrite(u16(base + (kMmioTimerStartHigh - kMmioTimerCfg)), u16(start >> 16));
    m.MmioWrite(base, u16(kTimerUpdateMmio | kTimerRestart | mode));
}

void ConfigureInterruptAndTimers(Machine& m)
{
    m.MmioWrite(kMmioIcuEnable0, u16(kIrqTimer0 | kIrqTimer1 | kIrqBtdmp0));
    ConfigureTimer(m, 0, kTimer0Period, kTimerAutoRestart);
    ConfigureTimer(m, 1, kTimer1Period, kTimerAutoRestart);
}

u32 TimerCounter(Machine& m, unsigned index)
{
    const u16 base = u16(kMmioTimerCounterLow + index * 0x10);
    const u32 low = m.Mmio(base);
    const u32 high = m.Mmio(u16(base + (kMmioTimerCounterHigh - kMmioTimerCounterLow)));
    return (high << 16) | low;
}

unsigned StackPushes(const Machine& m)
{
    unsigned pushes = 0;
    // WriteTrace records shared memory word indices, so data addresses carry
    // the data memory offset.
    const u32 pushedWord = Machine::kDataOffset + kStackTop - 1;
    for (const auto& entry : m.WriteTrace)
        if (entry.first == pushedWord) ++pushes;
    return pushes;
}

// Every data memory write in this scenario comes from the interrupt stack.
void CheckDataWritesAreStackOnly(const Machine& m)
{
    std::string detail;
    for (const auto& entry : m.WriteTrace)
    {
        if (entry.first >= Machine::kDataOffset && entry.first < Machine::kDataOffset + kStackTop - 16)
        {
            detail += " [word=" + std::to_string(entry.first) + " data=" +
                      std::to_string(entry.first - Machine::kDataOffset) + " val=" +
                      std::to_string(entry.second) + "]";
        }
        if (detail.size() > 200) break;
    }
    Require(detail.empty(), "unexpected data memory writes:" + detail);
}

// Each step performs a fixed amount of work and appends the observations a
// baseline/candidate diff must match byte for byte. It asserts only invariants
// that must hold for both builds.
void DriveStep(Machine& m, Bytes& out, unsigned step)
{
    const u16 base0 = kMmioTimerCfg;
    const u16 base1 = u16(kMmioTimerCfg + 0x10);
    switch (step)
    {
    case 0:
        m.Run(4000);
        out.Tag("run4000");
        out.Word(u32(m.ProgramFetches));
        out.Word(u32(StackPushes(m)));
        out.Half(m.Mmio(kMmioIcuRequest));
        out.Half(m.Mmio(kMmioIcuEnable0));
        out.Word(TimerCounter(m, 0));
        out.Word(TimerCounter(m, 1));
        break;
    case 1:
    {
        // One core tick per executed instruction: restarting the auto restart
        // counter and running fewer cycles than its period must move it by
        // exactly the requested cycle count.
        m.MmioWrite(base0, u16(kTimerUpdateMmio | kTimerRestart | kTimerAutoRestart));
        const u32 before = TimerCounter(m, 0);
        Require(before == kTimer0Period, "timer0 restart value");
        m.Run(500);
        const u32 after = TimerCounter(m, 0);
        Require(after == before - 500, "timer0 does not tick once per executed instruction");
        out.Tag("tickdelta");
        out.Word(before);
        out.Word(after);
        break;
    }
    case 2:
        m.MmioWrite(kMmioBtdmpTxEnable, 1);
        m.MmioWrite(kMmioBtdmpTxSend, 0x1111);
        m.MmioWrite(kMmioBtdmpTxSend, 0x2222);
        m.MmioWrite(kMmioBtdmpTxSend, 0x3333);
        Require(m.Mmio(kMmioBtdmpTxStatus) == 0, "tx status after three queued words");
        out.Tag("txqueue");
        out.Half(m.Mmio(kMmioBtdmpTxStatus));
        break;
    case 3:
        m.Sample(1000);
        Require(m.Pcm[m.Pcm.size() - 2] == 0x1111 && m.Pcm[m.Pcm.size() - 1] == 0x2222,
                "i2s consumption order");
        out.Tag("i2s1");
        out.Half(m.Mmio(kMmioBtdmpTxStatus));
        break;
    case 4:
        m.Sample(2000);
        Require(m.Pcm[m.Pcm.size() - 2] == 0x3333 && m.Pcm[m.Pcm.size() - 1] == 0,
                "i2s underrun fallback");
        Require((m.Mmio(kMmioBtdmpTxStatus) & 0x10) != 0, "tx not empty after drain");
        Require((m.Mmio(kMmioIcuRequest) & kIrqBtdmp0) != 0, "btdmp0 empty request missing");
        out.Tag("i2s2");
        out.Half(m.Mmio(kMmioBtdmpTxStatus));
        out.Half(m.Mmio(kMmioIcuRequest));
        break;
    case 5:
        m.MmioWrite(kMmioIcuAcknowledge, kIrqTimer0);
        Require((m.Mmio(kMmioIcuRequest) & kIrqTimer0) == 0, "icu acknowledge");
        out.Tag("icuack");
        out.Half(m.Mmio(kMmioIcuRequest));
        break;
    case 6:
        m.MmioWrite(kMmioBtdmpRxEnable, 1);
        // Only the transition is asserted: the event counter is meaningless on a
        // machine that resumed from a savestate taken mid scenario.
        Require(m.MicEnabled, "mic enable callback did not report enabled");
        out.Tag("micenable");
        out.Half(1);
        break;
    case 7:
    {
        m.Sample(-7);
        m.Sample(-7);
        const u16 rxStatus = m.Mmio(kMmioBtdmpRxStatus);
        // Receive status bit3 is "full" and bit4 is !empty, i.e. it reads as set
        // while the receive FIFO holds data (Btdmp::GetReceiveEmpty returns
        // !receive_empty, which the header documents as backwards).
        Require((rxStatus & 0x18) == 0x10,
                "rx should hold two duplicated samples and not be full, status=" +
                    std::to_string(rxStatus));
        out.Tag("rxfill");
        out.Half(rxStatus);
        break;
    }
    case 8:
    {
        const u16 first = m.Mmio(kMmioBtdmpRxReceive);
        const u16 second = m.Mmio(kMmioBtdmpRxReceive);
        Require(first == u16(-7) && second == u16(-7), "duplicated microphone sample");
        out.Tag("rxread");
        out.Half(first);
        out.Half(second);
        out.Half(m.Mmio(kMmioBtdmpRxStatus));
        break;
    }
    case 9:
        m.Run(3000);
        out.Tag("run3000");
        out.Word(u32(StackPushes(m)));
        out.Half(m.Mmio(kMmioIcuRequest));
        out.Word(TimerCounter(m, 0));
        break;
    case 10:
        m.Sample(0);
        out.Tag("sample0");
        out.Half(m.Mmio(kMmioBtdmpRxStatus));
        break;
    case 11:
    {
        unsigned calls = 0;
        while (calls < 8 && (m.Mmio(kMmioBtdmpRxStatus) & 0x08) == 0)
        {
            m.Sample(50);
            ++calls;
        }
        Require((m.Mmio(kMmioBtdmpRxStatus) & 0x08) != 0, "rx fifo never reported full");
        Require((m.Mmio(kMmioIcuRequest) & kIrqBtdmp0) != 0, "btdmp0 full request missing");
        out.Tag("rxfull");
        out.Word(calls);
        out.Half(m.Mmio(kMmioBtdmpRxStatus));
        break;
    }
    case 12:
        m.MmioWrite(kMmioBtdmpTxFlush, 1);
        m.MmioWrite(kMmioBtdmpRxFlush, 1);
        Require((m.Mmio(kMmioBtdmpTxStatus) & 0x10) != 0, "tx flush");
        Require((m.Mmio(kMmioBtdmpRxStatus) & 0x08) == 0, "rx flush");
        out.Tag("flush");
        out.Half(m.Mmio(kMmioBtdmpTxStatus));
        out.Half(m.Mmio(kMmioBtdmpRxStatus));
        break;
    case 13:
        m.MmioWrite(kMmioBtdmpRxEnable, 0);
        Require(!m.MicEnabled, "mic disable callback did not report disabled");
        out.Tag("micdisable");
        out.Half(0);
        break;
    case 14:
        // Pause timer1: one registered callback is now Infinity for core timing
        // skip purposes.
        m.MmioWrite(base1, u16(kTimerUpdateMmio | kTimerAutoRestart | kTimerPause));
        m.Run(1500);
        out.Tag("pause1");
        out.Half(m.Mmio(base1));
        out.Word(TimerCounter(m, 0));
        out.Word(TimerCounter(m, 1));
        break;
    case 15:
        // Event count mode ignores core ticks and only moves on the event pulse
        // register, so it is another Infinity entry for core timing.
        m.MmioWrite(u16(base1 + (kMmioTimerStartLow - kMmioTimerCfg)), 500);
        m.MmioWrite(u16(base1 + (kMmioTimerStartHigh - kMmioTimerCfg)), 0);
        m.MmioWrite(base1, u16(kTimerUpdateMmio | kTimerRestart | kTimerEventCount));
        Require(TimerCounter(m, 1) == 500, "event count restart value");
        m.Run(200);
        Require(TimerCounter(m, 1) == 500, "event count timer moved on core ticks");
        m.MmioWrite(u16(base1 + (kMmioTimerEvent - kMmioTimerCfg)), 1);
        Require(TimerCounter(m, 1) == 499, "event count pulse");
        out.Tag("eventcount");
        out.Word(TimerCounter(m, 1));
        break;
    case 16:
        ConfigureTimer(m, 0, kTimer0Period, kTimerAutoRestart);
        ConfigureTimer(m, 1, kTimer1Period, kTimerAutoRestart);
        out.Tag("restartboth");
        out.Word(TimerCounter(m, 0));
        out.Word(TimerCounter(m, 1));
        break;
    case 17:
        m.Run(2500);
        out.Tag("run2500");
        // Instantaneous state only: cumulative counters are dumped once, after
        // the whole run, so a savestate resume can be compared step by step.
        out.Word(u32(m.DataWord(u16(kStackTop - 2))) |
                 (u32(m.DataWord(u16(kStackTop - 1))) << 16));
        out.Half(m.Mmio(kMmioIcuRequest));
        out.Half(m.Mmio(kMmioIcuEnable0));
        break;
    case 18:
    {
        std::vector<u16> stack;
        for (u16 address = u16(kStackTop - 16); address <= kStackTop; ++address)
            stack.push_back(m.DataWord(address));
        out.Tag("stack");
        out.Halfs(stack);
        out.Tag("pcm");
        out.Samples(m.Pcm);
        out.Tag("state");
        out.Raw(m.SaveState());
        break;
    }
    default:
        throw std::runtime_error("unknown drive step");
    }
}

void Drive(Machine& m, Bytes& out, unsigned first, unsigned last)
{
    for (unsigned step = first; step < last; ++step) DriveStep(m, out, step);
}

// Per step byte ranges, so a divergence can be reported at the step that
// introduced it instead of as one opaque blob.
struct Segments
{
    std::vector<unsigned> Steps;
    std::vector<std::pair<std::size_t, std::size_t>> Ranges;
};

void DriveTracked(Machine& m, Bytes& out, unsigned first, unsigned last, Segments& segments)
{
    for (unsigned step = first; step < last; ++step)
    {
        const std::size_t begin = out.Data.size();
        DriveStep(m, out, step);
        segments.Steps.push_back(step);
        segments.Ranges.emplace_back(begin, out.Data.size());
    }
}

void RequireSameStream(const Bytes& left, const Segments& leftSegments, const Bytes& right,
                       const Segments& rightSegments, const char* what)
{
    Require(leftSegments.Steps == rightSegments.Steps, std::string(what) + ": step list");
    for (std::size_t index = 0; index < leftSegments.Ranges.size(); ++index)
    {
        const auto& a = leftSegments.Ranges[index];
        const auto& b = rightSegments.Ranges[index];
        if (a.second - a.first != b.second - b.first ||
            !std::equal(left.Data.begin() + a.first, left.Data.begin() + a.second,
                        right.Data.begin() + b.first))
        {
            std::string detail = std::string(what) + " diverged at step " +
                                 std::to_string(leftSegments.Steps[index]) + " (" +
                                 std::to_string(a.second - a.first) + " vs " +
                                 std::to_string(b.second - b.first) + " bytes)";
            Require(false, detail);
        }
    }
}

// The same observation set is taken after the uninterrupted run and after a
// savestate reload, so the two must be byte identical.
// Cumulative work counters and traces of one complete run. Dumped once, after
// the tracked step sequence, because they are not comparable across a savestate
// resume.
void DumpRunStatistics(const Machine& m, Bytes& out)
{
    out.Tag("counters");
    out.Word(u32(m.ProgramFetches));
    out.Word(u32(m.DataReads));
    out.Word(u32(m.DataWrites));
    out.Word(u32(m.SampleClocks));
    out.Word(u32(m.ProgramWrites));
    out.Word(u32(m.HighWrites));
    out.Word(u32(m.MicEnableEvents));
    out.Tag("fetchtrace");
    out.Word(u32(m.FetchTrace.size()));
    for (u32 word : m.FetchTrace) out.Word(word);
    out.Tag("writetrace");
    out.Word(u32(m.WriteTrace.size()));
    for (const auto& entry : m.WriteTrace)
    {
        out.Word(entry.first);
        out.Half(entry.second);
    }
}

void ObserveFinalState(Machine& m, Bytes& out, const std::vector<u8>& savestate)
{
    std::vector<u16> stack;
    for (u16 address = u16(kStackTop - 16); address <= kStackTop; ++address)
        stack.push_back(m.DataWord(address));
    out.Tag("final-stack");
    out.Halfs(stack);
    out.Tag("final-pcm");
    out.Samples(m.Pcm);
    out.Tag("final-mmio");
    out.Half(m.Mmio(kMmioIcuRequest));
    out.Half(m.Mmio(kMmioIcuEnable0));
    out.Word(TimerCounter(m, 0));
    out.Word(TimerCounter(m, 1));
    out.Half(m.Mmio(kMmioBtdmpTxStatus));
    out.Half(m.Mmio(kMmioBtdmpRxStatus));
    out.Tag("final-data");
    for (u32 i = Machine::kDataOffset; i < Machine::kProgramWords; ++i) out.Half(m.Words[i]);
    out.Tag("final-savestate");
    out.Raw(savestate);
}

void CheckInitialProgramDecode(const Machine& m)
{
    // Confirms the hand assembled image really decodes as intended: br is a two
    // word absolute branch to the loop entry, both prologue moves are two word
    // moves, and control reaches the first loop body word.
    const u32 expected[] = {0x000, 0x001, kMainEntry, kMainEntry + 1,
                            kMainEntry + 2, kMainEntry + 3, kMainEntry + 4};
    Require(m.FetchTrace.size() >= 7, "fetch trace too short");
    for (unsigned i = 0; i < 7; ++i)
        Require(m.FetchTrace[i] == expected[i], "unexpected program word fetch sequence");
}

void CheckInterruptDelivery(const Machine& m, u32 loopFirst, u32 loopLast)
{
    Require(StackPushes(m) >= 4, "no interrupt was delivered to the DSP core");
    const u32 pushed = u32(m.DataWord(u16(kStackTop - 2))) |
                       (u32(m.DataWord(u16(kStackTop - 1))) << 16);
    Require(pushed >= loopFirst && pushed <= loopLast,
            "interrupted program counter is outside the instruction loop");
}

void CheckPeripherals()
{
    // --- active instruction loop -------------------------------------------
    Machine active;
    active.LoadProgram(ActiveProgram());
    ConfigureInterruptAndTimers(active);
    Bytes activeHead;
    Drive(active, activeHead, 0, kCheckpointStep);
    Bytes activeTail;
    Segments activeTailSegments;
    DriveTracked(active, activeTail, kCheckpointStep, kStepCount, activeTailSegments);
    CheckInitialProgramDecode(active);
    CheckInterruptDelivery(active, kMainEntry + 4, kMainEntry + 4 + kActiveLoopWords - 1);
    CheckDataWritesAreStackOnly(active);
    Require(active.ProgramWrites == 0 && active.HighWrites == 0,
            "run time writes outside data memory, program=" +
                std::to_string(active.ProgramWrites) + " high=" +
                std::to_string(active.HighWrites));

    // Replay the second half against a savestate taken mid scenario: the
    // observations and the final machine state must be byte identical to the
    // uninterrupted run.
    Machine firstHalf;
    firstHalf.LoadProgram(ActiveProgram());
    ConfigureInterruptAndTimers(firstHalf);
    Bytes firstHalfHead;
    Drive(firstHalf, firstHalfHead, 0, kCheckpointStep);
    Require(firstHalfHead.Data == activeHead.Data, "pre-checkpoint drive is not deterministic");
    const std::vector<u8> checkpoint = firstHalf.SaveState();
    const std::vector<u16> checkpointMemory = firstHalf.Words;
    const std::vector<s16> checkpointPcm = firstHalf.Pcm;
    Bytes liveTail;
    Segments liveTailSegments;
    DriveTracked(firstHalf, liveTail, kCheckpointStep, kStepCount, liveTailSegments);
    RequireSameStream(activeTail, activeTailSegments, liveTail, liveTailSegments,
                      "live continuation");

    Machine reloaded;
    reloaded.LoadProgram(ActiveProgram());
    reloaded.Words = checkpointMemory;
    reloaded.Pcm = checkpointPcm;
    reloaded.LoadState(checkpoint);
    Bytes reloadTail;
    Segments reloadTailSegments;
    DriveTracked(reloaded, reloadTail, kCheckpointStep, kStepCount, reloadTailSegments);
    RequireSameStream(activeTail, activeTailSegments, reloadTail, reloadTailSegments,
                      "savestate continuation");

    const std::vector<u8> finalLive = firstHalf.SaveState();
    const std::vector<u8> finalReloaded = reloaded.SaveState();
    Require(finalLive == finalReloaded, "savestate continuation state mismatch");
    Require(firstHalf.Words == reloaded.Words, "savestate continuation memory mismatch");

    Bytes liveState;
    ObserveFinalState(firstHalf, liveState, finalLive);
    Bytes reloadState;
    ObserveFinalState(reloaded, reloadState, finalReloaded);
    Require(liveState.Data == reloadState.Data, "reloaded machine observation mismatch");

    Bytes out;
    out.Raw(activeHead.Data);
    out.Raw(activeTail.Data);
    out.Raw(liveState.Data);
    DumpRunStatistics(firstHalf, out);

    // --- interpreter idle loop --------------------------------------------
    u64 idleFetches = 0;
    Bytes idleOut;
    {
        Machine idle;
        idle.LoadProgram(IdleProgram());
        ConfigureInterruptAndTimers(idle);

        // Timer0 with a period longer than the window, timer1 paused: no timer
        // expires, so the skip path must account for every core cycle exactly.
        ConfigureTimer(idle, 0, 100000, kTimerAutoRestart);
        ConfigureTimer(idle, 1, kTimer1Period, kTimerAutoRestart);
        idle.MmioWrite(u16(kMmioTimerCfg + 0x10),
                       u16(kTimerUpdateMmio | kTimerAutoRestart | kTimerPause));
        const u32 before = TimerCounter(idle, 0);
        idle.Run(20000);
        const u32 after = TimerCounter(idle, 0);
        Require(before == 100000 && after == before - 20000,
                "idle skip accounting does not match the requested cycles");

        // Periodic timers plus delivered interrupts on the idle path.
        ConfigureInterruptAndTimers(idle);
        idle.Run(20000);
        CheckInterruptDelivery(idle, kMainEntry + 4, kMainEntry + 4 + kIdleRegionWords - 1);
        idleFetches = idle.ProgramFetches;

        idleOut.Tag("idle");
        idleOut.Word(u32(idle.ProgramFetches));
        idleOut.Word(u32(StackPushes(idle)));
        idleOut.Word(before);
        idleOut.Word(after);
        idleOut.Half(idle.Mmio(kMmioIcuRequest));
        idleOut.Word(TimerCounter(idle, 0));
        idleOut.Word(TimerCounter(idle, 1));
        idleOut.Raw(idle.SaveState());
    }

    // --- both timers paused: the skip budget must fall through unchanged -----
    u64 pausedFetches = 0;
    {
        Machine paused;
        paused.LoadProgram(IdleProgram());
        ConfigureInterruptAndTimers(paused);
        paused.MmioWrite(u16(kMmioTimerCfg + 0x00),
                         u16(kTimerUpdateMmio | kTimerAutoRestart | kTimerPause));
        paused.MmioWrite(u16(kMmioTimerCfg + 0x10),
                         u16(kTimerUpdateMmio | kTimerAutoRestart | kTimerPause));
        paused.Run(30000);
        Require((paused.Mmio(kMmioIcuRequest) & (kIrqTimer0 | kIrqTimer1)) == 0,
                "paused timers raised an interrupt");
        pausedFetches = paused.ProgramFetches;
        idleOut.Tag("idlepaused");
        idleOut.Word(u32(paused.ProgramFetches));
        idleOut.Word(u32(paused.DataReads));
        idleOut.Word(u32(paused.DataWrites));
        idleOut.Raw(paused.SaveState());
    }

    out.Raw(idleOut.Data);
    WriteFile(OutputPath(), out);
    std::printf("PASS a5 active_fetches=%llu idle_fetches=%llu idle_paused_fetches=%llu "
                "irq_pushes=%u pcm_words=%zu stream_bytes=%zu\n",
                (unsigned long long)active.ProgramFetches, (unsigned long long)idleFetches,
                (unsigned long long)pausedFetches, StackPushes(active), active.Pcm.size(),
                out.Data.size());
}

// ---------------------------------------------------------------------------
// Benchmark. The parent runs this serially, with no sibling build active. Each row
// builds its own machine, warms it up outside the measured region, and reports raw
// elapsed nanoseconds plus the work actually performed. Construction, program
// upload, host MMIO writes, reset and savestate serialization are never measured.
// ---------------------------------------------------------------------------
struct BenchCounts
{
    u32 ActiveCycles = 50000;
    u32 ActivePasses = 60;
    u32 IdleCycles = 5000000;
    u32 IdlePasses = 60;
    u32 SampleCalls = 50000;
    u32 SamplePasses = 60;
};

template <typename Body>
u64 TimeNanoseconds(Body body)
{
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto stop = std::chrono::steady_clock::now();
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
}

void ReportRow(const char* name, u64 nanoseconds, u64 cycles, u64 passes, Machine& m,
               u64 fetchesBefore, u64 pushesBefore, u64 clocksBefore, Bytes& out)
{
    const u64 fetches = m.ProgramFetches - fetchesBefore;
    const u64 pushes = StackPushes(m) - pushesBefore;
    const u64 clocks = m.SampleClocks - clocksBefore;
    std::printf("bench,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", name,
                (unsigned long long)cycles, (unsigned long long)passes,
                (unsigned long long)nanoseconds, (unsigned long long)fetches,
                (unsigned long long)clocks, (unsigned long long)pushes,
                (unsigned long long)(m.DataReads + m.DataWrites));
    out.Tag(name);
    out.Word(u32(fetches));
    out.Word(u32(clocks));
    out.Word(u32(pushes));
    out.Raw(m.SaveState());
}

void PauseTimers(Machine& m)
{
    for (unsigned index = 0; index < 2; ++index)
    {
        const u16 base = u16(kMmioTimerCfg + index * 0x10);
        m.MmioWrite(base, u16(kTimerUpdateMmio | kTimerAutoRestart | kTimerPause));
    }
}

void RunBenchmark(const BenchCounts& counts, Bytes& out)
{
    std::printf("bench,row,cycles,passes,elapsed_ns,program_fetches,sample_clocks,irq_pushes,"
                "data_accesses\n");

    // 1: active instruction loop with both timer callbacks registered and ticking.
    {
        Machine m;
        m.LoadProgram(ActiveProgram());
        ConfigureInterruptAndTimers(m);
        m.Run(2000); // untimed warmup
        const u64 fetches = m.ProgramFetches;
        const u64 pushes = StackPushes(m);
        const u64 clocks = m.SampleClocks;
        const u64 nanoseconds = TimeNanoseconds([&] {
            for (u32 pass = 0; pass < counts.ActivePasses; ++pass) m.Run(counts.ActiveCycles);
        });
        Require(StackPushes(m) > pushes, "active benchmark delivered no timer interrupt");
        ReportRow("active-ticks", nanoseconds, u64(counts.ActiveCycles) * counts.ActivePasses,
                  counts.ActivePasses, m, fetches, pushes, clocks, out);
    }

    // 2: the same loop with both timer callbacks inert.
    {
        Machine m;
        m.LoadProgram(ActiveProgram());
        ConfigureInterruptAndTimers(m);
        PauseTimers(m);
        m.Run(2000);
        const u64 fetches = m.ProgramFetches;
        const u64 pushes = StackPushes(m);
        const u64 clocks = m.SampleClocks;
        const u64 nanoseconds = TimeNanoseconds([&] {
            for (u32 pass = 0; pass < counts.ActivePasses; ++pass) m.Run(counts.ActiveCycles);
        });
        ReportRow("active-timers-paused", nanoseconds,
                  u64(counts.ActiveCycles) * counts.ActivePasses, counts.ActivePasses, m, fetches,
                  pushes, clocks, out);
    }

    // 3: interpreter idle self-branch, so the core timing skip path dominates.
    {
        Machine m;
        m.LoadProgram(IdleProgram());
        ConfigureInterruptAndTimers(m);
        m.Run(10000);
        const u64 fetches = m.ProgramFetches;
        const u64 pushes = StackPushes(m);
        const u64 clocks = m.SampleClocks;
        const u64 nanoseconds = TimeNanoseconds([&] {
            for (u32 pass = 0; pass < counts.IdlePasses; ++pass) m.Run(counts.IdleCycles);
        });
        Require(StackPushes(m) > pushes, "idle benchmark delivered no timer interrupt");
        ReportRow("idle-skip", nanoseconds, u64(counts.IdleCycles) * counts.IdlePasses,
                  counts.IdlePasses, m, fetches, pushes, clocks, out);
    }

    // 4: the I2S sample clock alone. It never touches CoreTiming, so it is the
    // negative control: a difference here would mean the harness, not the change.
    {
        Machine m;
        m.LoadProgram(ActiveProgram());
        m.MmioWrite(kMmioBtdmpTxEnable, 1);
        m.MmioWrite(kMmioBtdmpRxEnable, 1);
        for (u16 word = 0; word < 16; ++word) m.MmioWrite(kMmioBtdmpTxSend, u16(0x100 + word));
        m.Sample(0);
        const u64 fetches = m.ProgramFetches;
        const u64 pushes = StackPushes(m);
        const u64 clocks = m.SampleClocks;
        const u64 nanoseconds = TimeNanoseconds([&] {
            for (u32 pass = 0; pass < counts.SamplePasses; ++pass)
                for (u32 call = 0; call < counts.SampleCalls; ++call)
                {
                    if (call % 8 == 0)
                    {
                        m.MmioWrite(kMmioBtdmpTxFlush, 1);
                        for (u16 word = 0; word < 16; ++word)
                            m.MmioWrite(kMmioBtdmpTxSend, u16(0x200 + word));
                        m.MmioWrite(kMmioBtdmpRxFlush, 1);
                    }
                    m.Sample(s16(call & 0x7FFF));
                }
        });
        Require(m.SampleClocks - clocks == u64(counts.SampleCalls) * counts.SamplePasses,
                "sample clock count mismatch");
        ReportRow("sampleclock-only", nanoseconds, 0,
                  u64(counts.SampleCalls) * counts.SamplePasses, m, fetches, pushes, clocks, out);
    }
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const std::string mode = argc > 1 ? argv[1] : "check";
        if (argc > 2) gOutputPath = argv[2];
        Require(gOutputPath != nullptr,
                "usage: Optimization161A5 <check|bench|bench-smoke> <output.bin>");
        if (mode == "check")
        {
            CheckPeripherals();
            return 0;
        }
        if (mode == "bench" || mode == "bench-smoke")
        {
            Bytes out;
            BenchCounts counts;
            if (mode == "bench-smoke")
            {
                counts.ActiveCycles = 2000;
                counts.ActivePasses = 2;
                counts.IdleCycles = 20000;
                counts.IdlePasses = 2;
                counts.SampleCalls = 2000;
                counts.SamplePasses = 2;
            }
            RunBenchmark(counts, out);
            WriteFile(gOutputPath, out);
            std::printf("PASS a5 %s stream_bytes=%zu\n", mode.c_str(), out.Data.size());
            return 0;
        }
        std::fprintf(stderr, "usage: Optimization161A5 <check|bench|bench-smoke> <output.bin>\n");
        return 2;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
