// SPDX-License-Identifier: GPL-3.0-or-later
// Real Timer0 MMIO -> HALT wake -> ARM9 IRQ, including cached guest execution.
// ARM DDI 0100I A2.6.8: IRQ preserves F, saves CPSR and next-instruction+4.
// https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a
// DS timer, IF/IE/IME and ARM9 HALT contracts: original GBATEK research.
// https://problemkaputt.de/gbatek.htm#dstimers
// https://problemkaputt.de/gbatek.htm#dsinterrupts
#include "Args.h"
#include "ARM.h"
#include "DSi.h"
#include "NDS.h"
#include "teakra/src/btdmp.h"

#include <cstdio>
#include <memory>
#include <utility>

using namespace melonDS;

// Exercises the active sample clock, including DSi I2S -> DSP -> Teakra.
// A missing right word uses the upstream Teakra Tick fallback as an emulator
// safety policy. This fixture is not evidence of the DSi hardware underrun value.
int TestDSiBTDMP(NDSArgs&& args)
{
    unsigned failures = 0;
    auto require = [&](bool ok, const char* detail) {
        if (!ok)
        {
            ++failures;
            std::fprintf(stderr, "btdmp: FAIL %s\n", detail);
        }
    };
    Teakra::CoreTiming timing;
    Teakra::Btdmp port(timing, 0);
    unsigned irqs = 0;
    port.SetInterruptHandler([&] { ++irqs; });
    port.Reset();
    port.SetTransmitEnable(1);
    s16 output[2] = {-123, -456};

    // Minimal counterexample: one ordinary Send, then exactly one active clock.
    port.Send(0x1234);
    std::puts("btdmp: one word queued; calling SampleClock once");
    std::fflush(stdout);
    port.SampleClock(output, 0);
    std::printf("btdmp: single output=(%d,%d) empty=%u full=%u irqs=%u\n",
                output[0], output[1], port.GetTransmitEmpty(), port.GetTransmitFull(), irqs);
    require(output[0] == 0x1234 && output[1] == 0 && port.GetTransmitEmpty() &&
            !port.GetTransmitFull() && irqs == 1, "single-word consumption / fallback / empty IRQ");
    port.SampleClock(output, 0);
    require(output[0] == 0 && output[1] == 0 && irqs == 1, "empty clock repeated the TX IRQ");

    port.Reset();
    irqs = 0;
    port.Send(111);
    port.Send(static_cast<u16>(-222));
    port.SampleClock(output, 0);
    require(output[0] == 0 && output[1] == 0 && !port.GetTransmitEmpty() && irqs == 0,
            "disabled transmitter consumed queued words");
    port.SetTransmitEnable(1);
    port.Send(333);
    port.SampleClock(output, 0);
    require(output[0] == 111 && output[1] == -222 && !port.GetTransmitEmpty() && irqs == 0,
            "normal pair / odd tail retention");
    port.SampleClock(output, 0);
    require(output[0] == 333 && output[1] == 0 && port.GetTransmitEmpty() && irqs == 1,
            "odd tail consumption / empty IRQ");
    port.Send(static_cast<u16>(-444));
    port.Send(555);
    port.SampleClock(output, 0);
    require(output[0] == -444 && output[1] == 555 && port.GetTransmitEmpty() && irqs == 2,
            "append after underrun lost stereo order / empty IRQ");

    // The external I2S clock still owns consumption, regardless of DSP ticks.
    port.Send(777);
    port.Send(888);
    port.SetTransmitPeriod(1);
    port.SetTransmitClockConfig(0x1004);
    timing.Tick();
    timing.Skip(8192);
    require(!port.GetTransmitEmpty() && irqs == 2, "DSP ticks consumed I2S-clocked words");
    port.SampleClock(output, 0);
    require(output[0] == 777 && output[1] == 888 && irqs == 3,
            "clock configuration changed external sample consumption");

    port.Reset();
    irqs = 0;
    port.SetTransmitEnable(1);
    constexpr s16 words[] = {1, -2, 3, -4, 5, -6, 7, -8, 9, -10, 11, -12, 13, -14, 15, -16};
    for (s16 word : words) port.Send(static_cast<u16>(word));
    port.Send(999); // The seventeenth word must not displace an existing sample.
    require(port.GetTransmitFull() && !port.GetTransmitEmpty(), "16-word capacity");
    for (unsigned i = 0; i < 8; ++i)
    {
        port.SampleClock(output, 0);
        require(output[0] == words[2 * i] && output[1] == words[2 * i + 1] &&
                !port.GetTransmitFull() && irqs == (i == 7 ? 1u : 0u),
                "full FIFO order / full clear / final-pair IRQ");
    }
    require(port.GetTransmitEmpty(), "full FIFO did not drain");
    port.Send(123);
    port.SetTransmitFlush(1);
    port.SampleClock(output, 0);
    require(output[0] == 0 && output[1] == 0 && port.GetTransmitEmpty() &&
            !port.GetTransmitFull() && irqs == 1, "flush retained a tail or raised TX IRQ");

    // RX must still run on a TX underrun and retain its duplicated-mic contract.
    port.Reset();
    irqs = 0;
    port.SetTransmitEnable(1);
    port.SetReceiveEnable(1);
    port.Send(321);
    port.SampleClock(output, -37);
    require(output[0] == 321 && output[1] == 0 && irqs == 1 &&
            port.GetReceiveEmpty() && !port.GetReceiveFull(), "RX stopped during TX underrun");
    require(port.Receive() == static_cast<u16>(-37) && port.Receive() == static_cast<u16>(-37) &&
            !port.GetReceiveEmpty(), "duplicated mic sample / existing inverted empty bit");
    for (int i = 0; i < 8; ++i) port.SampleClock(output, 50);
    require(port.GetReceiveFull() && irqs == 2, "RX full IRQ with empty TX");
    require(port.Receive() == 50 && !port.GetReceiveFull(), "RX full clear");
    port.SampleClock(output, 75); // Fifteen words: room for just one copy.
    require(port.GetReceiveFull() && irqs == 3, "RX capacity after one receive");
    port.SampleClock(output, 99);
    require(port.GetReceiveFull() && irqs == 4, "existing RX-full level IRQ");
    port.SetReceiveEnable(0);
    for (int i = 0; i < 15; ++i) require(port.Receive() == 50, "RX FIFO order / overrun");
    require(port.Receive() == 75 && !port.GetReceiveEmpty(), "RX last available slot");
    port.SampleClock(output, 99);
    require(!port.GetReceiveEmpty() && irqs == 4, "disabled RX consumed input");
    port.SetReceiveEnable(1);
    port.SampleClock(output, 25);
    port.SetReceiveFlush(1);
    require(!port.GetReceiveEmpty() && !port.GetReceiveFull() && irqs == 4, "RX flush");
    std::printf("btdmp: FIFO/IRQ/clock/flush/receive controls: %s\n", failures ? "FAIL" : "PASS");

    DSiArgs dsiArgs;
    static_cast<NDSArgs&>(dsiArgs) = std::move(args);
    auto dsi = std::make_unique<DSi>(std::move(dsiArgs));
    dsi->Reset();
    dsi->SCFG_Clock9 |= 2;
    dsi->DSP.SetRstLine(true);
    auto writeMMIO = [&](u16 address, u16 value) {
        dsi->DSP.Write16(0x04004308, 0x1000); // MMIO, no address increment.
        dsi->DSP.Write16(0x04004304, address);
        dsi->DSP.Write16(0x04004300, value);
    };
    auto readMMIO = [&](u16 address) {
        dsi->DSP.Write16(0x04004304, address);
        dsi->DSP.Write16(0x04004308, 0x1010); // One-word PDATA read.
        const u16 value = dsi->DSP.Read16(0x04004300);
        dsi->DSP.Write16(0x04004308, 0x1000);
        return value;
    };
    for (u16 rate : {u16{0}, u16{0x2000}})
    {
        const unsigned before = failures;
        dsi->I2S.WriteSndExCnt(0, 0xFFFF);
        dsi->I2S.WriteSndExCnt(0x8000 | rate, 0xFFFF); // DSP-only mix, both I2S rates.
        writeMMIO(0x2CA, 1);
        writeMMIO(0x2BE, 1);
        writeMMIO(0x202, 0xFFFF);
        dsi->I2S.SampleClock(output);
        require(output[0] == 0 && output[1] == 0 && readMMIO(0x200) == 0,
                "I2S empty clock / IRQ");
        writeMMIO(0x2C6, 0x2345);
        require((readMMIO(0x2C2) & 0x18) == 0, "PDATA did not queue the single word");
        dsi->I2S.SampleClock(output);
        require(output[0] == 0x2345 && output[1] == 0 &&
                (readMMIO(0x2C2) & 0x18) == 0x10 && readMMIO(0x200) == 0x0800,
                "I2S -> DSP -> Teakra single word / TX empty / ICU request");
        writeMMIO(0x202, 0x0800);
        dsi->I2S.SampleClock(output);
        require(readMMIO(0x200) == 0, "I2S empty clock repeated acknowledged IRQ");
        writeMMIO(0x2C6, 0x8000);
        writeMMIO(0x2C6, 0x7FFF);
        dsi->I2S.WriteSndExCnt(0, 0xFFFF);
        dsi->I2S.SampleClock(output);
        require(output[0] == 0 && output[1] == 0 && !(readMMIO(0x2C2) & 0x10),
                "disabled I2S drained the pair");
        dsi->I2S.WriteSndExCnt(0x8000 | rate, 0xFFFF);
        dsi->I2S.SampleClock(output);
        require(output[0] == -32768 && output[1] == 32767 && readMMIO(0x200) == 0x0800,
                "I2S normal signed stereo pair / IRQ after underrun");
        require((dsi->I2S.ReadSndExCnt() & 0x2000) == rate, "I2S sample changed the rate");
        std::printf("btdmp: I2S rate-bit=%04x: %s\n", rate, failures == before ? "PASS" : "FAIL");
    }
    return failures ? 1 : 0;
}

// GBATEK I2C ports/signals and devkitPro Calico's native MCU transactions:
// https://problemkaputt.de/gbatek-dsi-i2c-i-o-ports.htm
// https://problemkaputt.de/gbatek-dsi-i2c-signals.htm
// https://github.com/devkitPro/calico/blob/master/source/nds/arm7/i2c.twl.c
// SCFG bit meanings: https://github.com/devkitPro/calico/blob/master/include/calico/nds/scfg.h
// Generated memory only; these checks do not establish bus latency or boot compatibility.
int TestDSiResetI2C(NDSArgs&& args)
{
    DSiArgs dsiArgs;
    static_cast<NDSArgs&>(dsiArgs) = std::move(args);
    auto dsi = std::make_unique<DSi>(std::move(dsiArgs));
    dsi->CurCPU = 1;
    dsi->Reset();
    constexpr u32 Data = 0x04004500, Cnt = 0x04004501;
    constexpr u32 IE2 = 0x04000218, IF2 = 0x0400021C;
    constexpr u32 Done = 1u << 13, BptwlIRQ = 1u << 6;
    unsigned failures = 0;
    const char* name = "";
    auto require = [&](bool ok, const char* detail) {
        if (!ok)
        {
            ++failures;
            std::fprintf(stderr, "reset-i2c/%s: FAIL %s (CNT=%02x DATA=%02x IF2=%08x)\n",
                         name, detail, dsi->ARM7Read8(Cnt), dsi->ARM7Read8(Data),
                         dsi->ARM7Read32(IF2));
        }
    };
    auto send = [&](u8 data, u8 cnt) {
        dsi->ARM7Write8(Data, data);
        dsi->ARM7Write8(Cnt, cnt);
        return dsi->ARM7Read8(Cnt);
    };
    auto receive = [&](u8 cnt) {
        dsi->ARM7Write8(Cnt, cnt);
        return dsi->ARM7Read8(Data);
    };
    auto select = [&](u8 reg) {
        require(send(0x4A, 0xC2) & 0x10, "BPTWL address must ACK");
        require(send(reg, 0xC0) & 0x10, "BPTWL index must ACK");
    };
    auto writeRegister = [&](u8 reg, u8 data) {
        select(reg);
        require(send(data, 0xC0) & 0x10, "BPTWL data must ACK");
        dsi->ARM7Write8(Cnt, 0xC5); // Calico's delayed MCU STOP.
        require(dsi->ARM7Read8(Cnt) & 0x10, "STOP lost preceding write ACK");
    };
    auto readRegister = [&](u8 reg) {
        select(reg);
        require(send(0x4B, 0xC2) & 0x10, "repeated START read address must ACK");
        const u8 value = receive(0xE0); // Last byte: master NACK, separate STOP.
        dsi->ARM7Write8(Cnt, 0xC5);
        require(dsi->ARM7Read8(Data) == value, "STOP changed received data");
        return value;
    };
    auto report = [&](unsigned before) {
        std::printf("reset-i2c/%s: %s\n", name, failures == before ? "PASS" : "FAIL");
    };

    name = "native-transactions-and-stop";
    unsigned before = failures;
    require(readRegister(0) == 0x33, "MCU version read");
    writeRegister(0x40, 7);
    require(readRegister(0x40) == 7, "native volume write/read");
    require(!(send(0x40, 0x80) & 0x10), "write without START after STOP was ACKed");
    dsi->I2C.Reset();
    select(0x40);
    send(0x4B, 0xC2);
    require(receive(0xA1) == 0x1F, "combined last-read/STOP control");
    require(!(send(0x40, 0x80) & 0x10), "combined STOP retained a slave");
    dsi->I2C.Reset();
    require(!(send(0xA0, 0xC2) & 0x10), "absent slave address was ACKed");
    require(!(send(0, 0xC0) & 0x10), "absent slave data was ACKed");
    require(receive(0xA1) == 0xFF, "absent slave read must leave SDA high");
    report(before);

    name = "direction-and-repeated-start";
    before = failures;
    dsi->I2C.Reset();
    select(0x40);
    require(receive(0xB0) == 0xFF, "read in write-address phase consumed slave data");
    send(3, 0xC0);
    dsi->ARM7Write8(Cnt, 0xC5);
    require(readRegister(0x40) == 3, "wrong-direction read advanced write index");
    select(0x40);
    send(0x4B, 0xC2);
    require(!(send(2, 0xC0) & 0x10), "write in read-address phase was ACKed");
    require(receive(0xA1) == 3, "wrong-direction write altered slave/register position");
    select(0);
    send(0xA1, 0xC2); // Repeated START replaces even a previously valid slave.
    require(receive(0xA1) == 0xFF, "absent repeated START retained old slave");
    dsi->I2C.Reset();
    select(0);
    send(0x4B, 0xC2);
    require(receive(0xB0) == 0x33 && (dsi->ARM7Read8(Cnt) & 0x10),
            "read ACK must remain master-controlled");
    require(receive(0xA1) == 0 && !(dsi->ARM7Read8(Cnt) & 0x10),
            "sequential read / final master NACK");
    // Both existing camera slaves use 16-bit indices/data, unlike the MCU.
    // Repeated START must still invoke their byte-alignment Acquire hook.
    dsi->ARM9Write16(0x04004004, 0x0187); // Camera external clock enabled.
    dsi->ARM9Write16(0x04004200, 0x0020); // Release camera reset.
    for (u8 address : {0x78, 0x7A})
    {
        require(send(address, 0xC2) & 0x10, "camera write address ACK");
        send(0, 0xC0);
        send(0, 0xC0); // CHIP_VERSION at 0000h.
        require(send(address | 1, 0xC2) & 0x10, "camera repeated START/read ACK");
        require(receive(0xF0) == 0x22 && receive(0xE1) == 0x80,
                "normal camera 16-bit chip ID transaction");
    }
    report(before);

    name = "completion-irq-and-mask-controls";
    before = failures;
    dsi->I2C.Reset();
    dsi->ARM7Write32(IE2, Done);
    dsi->ARM7Write32(0x04000208, 1);
    dsi->ARM7Write32(IF2, 0xFFFFFFFF);
    send(0x4A, 0x82);
    require(!(dsi->ARM7Read32(IF2) & Done), "IRQ-disabled transfer raised completion");
    dsi->ARM7Write8(Cnt, 0x40);
    require(!(dsi->ARM7Read32(IF2) & Done), "non-start write raised completion");
    send(0xA0, 0xC2);
    require(!(dsi->ARM7Read8(Cnt) & 0x90) && (dsi->ARM7Read32(IF2) & Done) &&
            dsi->HaltInterrupted(1) && dsi->ARM7.IRQ,
            "NACK completion must clear busy and assert ARM7 IRQ2/IRQ");
    require(!(dsi->ARM7Read32(IF2) & BptwlIRQ), "host IRQ confused with MCU button IRQ");
    dsi->ARM7Write32(IF2, Done);
    require(!dsi->HaltInterrupted(1) && !dsi->ARM7.IRQ, "IF2 W1C did not clear host IRQ");
    dsi->ARM7Write32(IE2, 0);
    send(0x4A, 0xC2);
    require((dsi->ARM7Read32(IF2) & Done) && !dsi->HaltInterrupted(1) && !dsi->ARM7.IRQ,
            "IE2-masked transfer must latch IF2 without interrupting ARM7");
    dsi->ARM7Write32(IE2, Done);
    dsi->ARM7Write32(IF2, Done);
    send(0, 0xC0);
    send(0x4B, 0xC2);
    dsi->ARM7Write32(IF2, Done);
    require(receive(0xE1) == 0x33 && (dsi->ARM7Read32(IF2) & Done),
            "receive completion IRQ/data");
    report(before);

    name = "transaction-save-load-and-cold-reset";
    before = failures;
    // Save the real host and its slaves at an address/data boundary, then
    // continue twice via MMIO. No serializer offsets or private firmware.
    for (bool reading : {false, true})
    {
        dsi->I2C.Reset();
        select(0x40);
        if (reading) send(0x4B, 0xC2);
        Savestate saved;
        dsi->I2C.DoSavestate(&saved);
        saved.Finish();
        require(!saved.Error, "transaction save failed");
        for (bool replay : {false, true})
        {
            if (replay)
            {
                dsi->I2C.Reset();
                Savestate load(saved.Buffer(), saved.Length(), false);
                dsi->I2C.DoSavestate(&load);
                require(!load.Error, "transaction load failed");
            }
            if (reading)
            {
                require(!(send(6, 0xC0) & 0x10), "saved read direction lost");
                require(receive(0xA1) == 0x1F, "saved slave/index read continuation");
            }
            else
            {
                require(receive(0xB0) == 0xFF, "saved write direction lost");
                send(9, 0xC0);
                dsi->ARM7Write8(Cnt, 0xC5);
                require(readRegister(0x40) == 9, "saved write index continuation");
            }
        }
    }
    Savestate stopped;
    dsi->I2C.DoSavestate(&stopped);
    stopped.Finish();
    dsi->I2C.Reset();
    Savestate loadStopped(stopped.Buffer(), stopped.Length(), false);
    dsi->I2C.DoSavestate(&loadStopped);
    require(!loadStopped.Error && !(send(0x40, 0x80) & 0x10),
            "load revived a stopped slave");
    dsi->Reset();
    select(0x40);
    dsi->Reset();
    require(dsi->ARM7Read8(Cnt) == 0 && dsi->ARM7Read8(Data) == 0 &&
            !(dsi->ARM7Read32(IF2) & Done), "cold reset retained host registers/IRQ");
    require(!(send(7, 0x80) & 0x10), "cold reset retained transaction");
    require(readRegister(0x40) == 0x1F, "normal transaction after cold reset");
    report(before);

    name = "legacy-14.1-transaction-compatibility";
    before = failures;
    const struct {
        u8 Cnt, Data, Device;
        bool Stopped;
    } legacyRecords[] = {
        {0x52, 0x4B, 0x4A, false}, // Read address sent; old host discarded bit 0.
        {0x50, 0x40, 0x4A, false}, // Write index sent; old host still allowed reads.
        {0x55, 0x09, 0x4A, true},  // C5 STOP; old host retained the selected slave.
        {0x42, 0x4B, 0xA0, false}, // Absent address, then a DATA write without CNT.
    };
    auto restoreHost = [&](Savestate& saved) {
        dsi->I2C.Reset();
        Savestate load(saved.Buffer(), saved.Length(), false);
        dsi->I2C.DoSavestate(&load);
        require(!load.Error, "host state load failed");
    };
    auto checkSavedMode = [&](Savestate& saved, bool legacy) {
        // The 14.2 wire record appends a boolean after the three legacy bytes.
        // Check this compatibility contract without exposing private host state.
        require(saved.IsAtLeastVersion(14, 2), "parent must enable savestate 14.2");
        Savestate inspect(saved.Buffer(), saved.Length(), false);
        inspect.Section("I2Ci");
        u8 registers[3];
        bool permissive = false;
        inspect.VarArray(registers, sizeof(registers));
        inspect.VarBool(&permissive);
        require(!inspect.Error && permissive == legacy, "serialized transaction policy");
    };
    for (const auto& record : legacyRecords)
    {
        dsi->I2C.Reset();
        writeRegister(0x41, 2);
        writeRegister(0x40, 9);
        if (!record.Stopped) select(0x40);

        // Fixed 14.1 I2Ci payload, using the public serializer and the actual
        // slave serializers. Do not serialize a new host and relabel its version.
        Savestate legacy(0x20000);
        legacy.Section("I2Ci");
        u8 registers[] = {record.Cnt, record.Data, record.Device};
        legacy.VarArray(registers, sizeof(registers));
        dsi->I2C.GetBPTWL()->DoSavestate(&legacy);
        dsi->I2C.GetOuterCamera()->DoSavestate(&legacy);
        dsi->I2C.GetInnerCamera()->DoSavestate(&legacy);
        legacy.Finish();
        // Public global header: fixed major/minor 14.1, never current-minus-one.
        auto* header = static_cast<u8*>(legacy.Buffer());
        header[4] = 14; header[5] = 0;
        header[6] = 1;  header[7] = 0;
        require(!legacy.Error && legacy.MajorVersion() == 14 && legacy.MinorVersion() == 1,
                "fixed legacy fixture could not be saved");
        for (bool resaved : {false, true})
        {
            restoreHost(legacy);
            if (resaved)
            {
                Savestate migrated(0x20000);
                dsi->I2C.DoSavestate(&migrated);
                migrated.Finish();
                checkSavedMode(migrated, true);
                restoreHost(migrated);
            }
            require(dsi->ARM7Read8(Cnt) == record.Cnt && dsi->ARM7Read8(Data) == record.Data,
                    "legacy load changed visible registers");
            if (record.Device == 0xA0)
            {
                require(!(send(7, 0xC0) & 0x10) && receive(0xB0) == 0xFF,
                        "legacy load inferred a slave from DATA instead of saved ID");
                continue;
            }
            if (record.Stopped)
                require(send(0x40, 0xC0) & 0x10, "legacy STOP status lost old selection");
            require(receive(0xB0) == 9, "legacy read continuation lost register/index");
            // Switch direction without START, just as the old host permitted.
            require(send(3, 0xC0) & 0x10, "legacy write continuation was rejected");
            require(dsi->I2C.GetBPTWL()->GetBacklightLevel() == 3 && receive(0xB0) == 0x5A,
                    "legacy write/read continuation lost slave state");
        }
        if (record.Stopped || record.Device == 0xA0) continue;
        for (bool readStart : {false, true})
        {
            restoreHost(legacy);
            send(readStart ? 0x4B : 0x4A, 0xC2);
            Savestate strict(0x20000);
            dsi->I2C.DoSavestate(&strict);
            strict.Finish();
            checkSavedMode(strict, false);
            // A current state must replace a legacy policy even without reset.
            restoreHost(legacy);
            Savestate loadStrict(strict.Buffer(), strict.Length(), false);
            dsi->I2C.DoSavestate(&loadStrict);
            require(!loadStrict.Error, "strict state load over a legacy transaction");
            if (readStart)
                require(!(send(7, 0xC0) & 0x10) && receive(0xA1) == 9,
                        "fresh read START did not restore strict direction");
            else
            {
                require(receive(0xB0) == 0xFF, "fresh write START remained permissive");
                send(0x4B, 0xC2);
                require(receive(0xA1) == 9, "strict read rejection advanced slave index");
            }
        }
        for (bool reset : {false, true})
        {
            restoreHost(legacy);
            if (reset) dsi->I2C.Reset();
            else dsi->ARM7Write8(Cnt, 0xC5);
            Savestate strict(0x20000);
            dsi->I2C.DoSavestate(&strict);
            strict.Finish();
            checkSavedMode(strict, false);
            restoreHost(strict);
            require(!(send(0x40, 0xC0) & 0x10) && receive(0xB0) == 0xFF,
                    "reset/STOP restored a legacy-selected slave");
        }
    }
    report(before);

    name = "bptwl-soft-reset-scfg-consistency";
    before = failures;
    dsi->ARM7Write16(0x04004010, 0x8000);
    dsi->Reset();
    dsi->NDSCartSlot.WriteSPICnt(0, 0x4000, 0xFFFF);
    require(!(dsi->ARM7Read16(0x04004010) & 0x8000) &&
            dsi->ARM9Read16(0x040001A0) == 0x4000 &&
            dsi->ARM9Read16(0x040021A0) == 0, "cold reset retained swapped card routing");
    for (bool slowClock : {false, true})
    {
        dsi->Reset();
        // Establish normal routing independently of the cold-reset regression
        // above, so each warm-reset failure has its own valid setup.
        dsi->ARM7Write16(0x04004010, 0x8000);
        dsi->ARM7Write16(0x04004010, 0);
        dsi->ARM9Write16(0x04004004, slowClock ? 0x0186 : 0x0187);
        require(dsi->ARM9ClockShift == (slowClock ? 1 : 2), "clock MMIO setup");
        dsi->ARM9Timestamp = 1234u << dsi->ARM9ClockShift;
        dsi->ARM9Target = 1300u << dsi->ARM9ClockShift;
        dsi->ARM9Write32(0x02020000, 0xABCD1234);
        // Give the two actual card interfaces distinct MMIO readbacks.
        dsi->ARM9Write16(0x040001A0, 0x4000);
        dsi->ARM9Write16(0x040021A0, 0);
        dsi->ARM7Write16(0x04004010, 0x8000);
        require(dsi->ARM9Read16(0x040001A0) == 0 &&
                dsi->ARM9Read16(0x040021A0) == 0x4000, "SCFG card-swap setup");
        writeRegister(0x70, 1);
        // The button path calls SoftReset directly outside guest execution.
        dsi->I2C.GetBPTWL()->DoHardwareReset(true);
        require((dsi->ARM9Read16(0x04004004) & 1) && dsi->ARM9ClockShift == 2,
                "reset SCFG clock disagrees with effective ARM9 clock");
        require((dsi->ARM9Timestamp >> dsi->ARM9ClockShift) == 1234 &&
                (dsi->ARM9Target >> dsi->ARM9ClockShift) == 1300,
                "reset clock change moved scheduler time");
        require(!(dsi->ARM7Read16(0x04004010) & 0x8000) &&
                dsi->ARM9Read16(0x040001A0) == 0x4000 &&
                dsi->ARM9Read16(0x040021A0) == 0, "reset SCFG card routing disagrees with MMIO");
        require(dsi->ARM9Read32(0x02020000) == 0xABCD1234 && readRegister(0x70) == 1,
                "warm reset lost bootflag/main RAM");
    }
    dsi->Reset();
    writeRegister(0x70, 1);
    select(0x11);
    send(1, 0xC0);
    require(dsi->ARM7.Halted == 4 && dsi->I2C.GetBPTWL()->GetBootFlag() == 1,
            "native software reset request did not reach deferred CPU reset path");
    report(before);
    std::printf("dsi-reset-i2c: %u failures\n", failures);
    return failures ? 1 : 0;
}

namespace
{
constexpr u32 Code = 0x02010000;
constexpr u32 Idle = 0x02000200;
constexpr u32 Vectors = 0xFFFF0000;
constexpr u32 Timer0 = 0x04000100;
constexpr u32 IMEAddress = 0x04000208;
constexpr u32 IEAddress = 0x04000210;
constexpr u32 IFAddress = 0x04000214;
constexpr u32 TimerIRQ = 1u << 3;
constexpr u32 TimerConfig = 0x00C00000; // Reload=0, /1, IRQ enabled, start.
constexpr u64 FirstOverflow = 65536;   // 16-bit counter, not CPU instruction cycles.
constexpr u32 Sentinel = 0xDEADC0DE;
constexpr u32 LinkSentinel = 0x12345678;
constexpr u32 SPSRSentinel = 0x11223344;

enum class Outcome { IRQ, Held, Resumed };
struct DeviceCase
{
    const char* Name;
    u32 MasterEnable;
    u32 EnabledIRQs;
    u32 InitialCPSR;
    u32 FinalCPSR;
    Outcome Result;
};

constexpr DeviceCase Cases[] = {
    {"irq-f-set-control", 1, TimerIRQ, 0xA800005F, 0xA80000D2, Outcome::IRQ},
    {"irq-f-clear-warm",  1, TimerIRQ, 0xA800001F, 0xA8000092, Outcome::IRQ},
    {"halt-ime-masked",   0, TimerIRQ, 0xA800001F, 0xA800001F, Outcome::Held},
    {"halt-ie-masked",    1, 0,        0xA800001F, 0xA800001F, Outcome::Held},
    {"halt-cpsr-masked",  1, TimerIRQ, 0xA800009F, 0xA800009F, Outcome::Resumed},
};

// Observe real MMIO; delegate every operation to the production implementation.
struct ObservedNDS final : NDS
{
    using NDS::NDS;
    bool TimerStarted = false;
    u64 TimerStartedAt = 0;
    u64 HandlerReadAt = 0;
    unsigned HandlerReads = 0;

    void ARM9Write32(u32 addr, u32 value) override
    {
        NDS::ARM9Write32(addr, value);
        if (addr == Timer0 && (value & 0x00800000))
        {
            TimerStarted = true;
            TimerStartedAt = ARM9Timestamp >> ARM9ClockShift;
        }
    }

    u32 ARM9Read32(u32 addr) override
    {
        const u32 value = NDS::ARM9Read32(addr);
        if (addr == IFAddress && (ARM9.CPSR & 0x1F) == 0x12)
        {
            ++HandlerReads;
            HandlerReadAt = ARM9Timestamp >> ARM9ClockShift;
        }
        return value;
    }
};
}

int TestDeviceExecution(NDSArgs&& args, bool jit)
{
    // All vectors are generated; no external BIOS or IRQ wrapper is involved.
    args.ARM9BIOS = std::make_unique<ARM9BIOSImage>();
    auto vectorWord = [&](u32 offset, u32 instruction) {
        for (unsigned byte = 0; byte < 4; ++byte)
            (*args.ARM9BIOS)[offset + byte] = instruction >> (byte * 8);
    };
    for (u32 offset = 0; offset < args.ARM9BIOS->size(); offset += 4)
        vectorWord(offset, 0xEAFFFFFE); // B .
    vectorWord(0x18, 0xE2899001); // ADD r9,r9,#1: count IRQ handler entries.
    vectorWord(0x1C, 0xE10FB000); // MRS r11,CPSR: observe F in the guest.
    vectorWord(0x20, 0xE598C000); // LDR r12,[r8]: observe the real pending IF.

    auto nds = std::make_unique<ObservedNDS>(std::move(args));
    nds->CurCPU = 0;
    nds->Reset();
    auto& cpu = nds->ARM9;
    constexpr u32 program[] = {
        0xE5823000, // STR r3,[r2]: IME
        0xE5845000, // STR r5,[r4]: IE
        0xE5887000, // STR r7,[r8]: acknowledge old Timer0 IF
        0xE5810000, // STR r0,[r1]: Timer0 reload/control in one MMIO write
        0xE598A000, // LDR r10,[r8]: IF must still be clear before HALT
        0xEE076F90, // MCR p15,0,r6,c7,c0,4: ARM9 HALT
        0xE3A06066, // MOV r6,#0x66: reached only by wake without IRQ
        0xEAFFFFFE, // B .
    };
    for (unsigned i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
        nds->ARM9Write32(Code + i * 4, program[i]);
    nds->ARM9Write32(Idle, 0xEAFFFFFE);
    nds->ARM7.JumpTo(Idle);
    nds->Start();

    unsigned failures = 0;
    unsigned caseIndex = 0;
    for (const auto& test : Cases)
    {
        // Neutralize the previous run through real MMIO. Do not reset the CPU,
        // scheduler, code bytes or JIT cache between the five guest entries.
        nds->CurCPU = 0;
        nds->ARM9Write16(Timer0 + 2, 0);
        nds->ARM9Write32(IMEAddress, 0);
        nds->ARM9Write32(IEAddress, 0);
        nds->ARM9Write32(IFAddress, TimerIRQ);
        nds->TimerStarted = false;
        nds->HandlerReads = 0;
        nds->HandlerReadAt = 0;

        const u32 oldCPSR = cpu.CPSR;
        cpu.CPSR = test.InitialCPSR;
        cpu.UpdateMode(oldCPSR, cpu.CPSR);
        cpu.StopExecution = 0;
        cpu.Cycles = 0;
        cpu.R[0] = TimerConfig;
        cpu.R[1] = Timer0;
        cpu.R[2] = IMEAddress;
        cpu.R[3] = test.MasterEnable;
        cpu.R[4] = IEAddress;
        cpu.R[5] = test.EnabledIRQs;
        cpu.R[6] = Sentinel;
        cpu.R[7] = TimerIRQ;
        cpu.R[8] = IFAddress;
        cpu.R[9] = 0;
        cpu.R[10] = cpu.R[11] = cpu.R[12] = Sentinel;
        cpu.R[14] = LinkSentinel;
        cpu.R_IRQ[2] = SPSRSentinel;
        cpu.JumpTo(Code);
        bool warm = true;
#ifdef JIT_ENABLED
        if (jit && caseIndex != 0)
            warm = nds->JIT.JitBlocks9.contains(Code);
#endif
        nds->RunFrame();

        const bool irq = test.Result == Outcome::IRQ;
        const bool held = test.Result == Outcome::Held;
        const bool resumed = test.Result == Outcome::Resumed;
        // Architectural IRQ entry is Vectors+0x18. The handler parks at +0x24;
        // between ARM instructions this core exposes the next address+4 in R15.
        const u32 expectedPC = irq ? Vectors + 0x28 : Code + (held ? 28 : 32);
        const u32 expectedLR = irq ? Code + 28 : LinkSentinel;
        const u32 expectedSPSR = irq ? test.InitialCPSR : SPSRSentinel;
        const u64 now = nds->ARM9Timestamp >> nds->ARM9ClockShift;
        const u64 elapsed = now - nds->TimerStartedAt;
        const bool timerOK = nds->TimerStarted && elapsed >= FirstOverflow &&
            (nds->IF[0] & TimerIRQ) && !(cpu.R[10] & TimerIRQ);
        const bool handlerOK = cpu.R[9] == (irq ? 1u : 0u) &&
            nds->HandlerReads == (irq ? 1u : 0u) &&
            cpu.R[11] == (irq ? test.FinalCPSR : Sentinel) &&
            (irq ? (cpu.R[12] & TimerIRQ) != 0 : cpu.R[12] == Sentinel) &&
            (!irq || nds->HandlerReadAt >= nds->TimerStartedAt + FirstOverflow);
        const bool ok = warm && nds->IsRunning() && timerOK && handlerOK &&
            nds->IME[0] == test.MasterEnable && nds->IE[0] == test.EnabledIRQs &&
            cpu.IRQ == (held ? 0u : 1u) && cpu.Halted == (held ? 1u : 0u) &&
            cpu.CPSR == test.FinalCPSR && cpu.R_IRQ[2] == expectedSPSR &&
            cpu.R[14] == expectedLR && cpu.R[15] == expectedPC &&
            cpu.R[6] == (resumed ? 0x66u : Sentinel);

        std::printf("device/%s/%s: %s\n", jit ? "jit" : "interpreter",
                    test.Name, ok ? "PASS" : "FAIL");
        if (!ok)
        {
            ++failures;
            std::fprintf(stderr,
                "warm=%d timer=%d handler=%d IF=%08x IE=%08x IME=%u IRQ=%u Halted=%u "
                "PC=%08x/%08x LR=%08x/%08x CPSR=%08x/%08x SPSR=%08x/%08x "
                "entries=%u guestCPSR=%08x resume=%08x elapsed=%llu handlerDelta=%llu\n",
                warm, timerOK, handlerOK, nds->IF[0], nds->IE[0], nds->IME[0],
                cpu.IRQ, cpu.Halted, cpu.R[15], expectedPC, cpu.R[14], expectedLR,
                cpu.CPSR, test.FinalCPSR, cpu.R_IRQ[2], expectedSPSR, cpu.R[9],
                cpu.R[11], cpu.R[6], static_cast<unsigned long long>(elapsed),
                static_cast<unsigned long long>(nds->HandlerReads ?
                    nds->HandlerReadAt - nds->TimerStartedAt : 0));
        }
        ++caseIndex;
    }

    std::printf("Device execution: %u/5 cases passed\n", 5 - failures);
    return failures ? 1 : 0;
}

// GBATEK: DSi NDMA fixed priority / logical and total lengths; GX FIFO packed
// command overkill must pause DMA until the geometry engine makes room.
// https://problemkaputt.de/gbatek.htm#dsinewdmandma
// https://problemkaputt.de/gbatek.htm#ds3dgeometrycommands
// Real MMIO, DMA, GPU and full savestates; no physical cycle-count oracle.
int TestDSiNDMAExecution(NDSArgs&& args)
{
    DSiArgs dsiArgs;
    static_cast<NDSArgs&>(dsiArgs) = std::move(args);
    auto dsi = std::make_unique<DSi>(std::move(dsiArgs));
    constexpr u32 Source = 0x02020000;
    constexpr u32 Dest = 0x02021000;
    constexpr u32 Enable = 1u << 31;
    constexpr u32 Done0 = 1u << 28;
    constexpr u32 Done1 = 1u << 29;
    unsigned failures = 0;
    const char* name = "";
    auto require = [&](bool ok, const char* detail) {
        if (!ok)
        {
            ++failures;
            std::fprintf(stderr, "ndma/%s: FAIL %s (CNT=%08x IF9=%08x stop=%08x)\n",
                         name, detail, dsi->NDMAs[0].Cnt, dsi->IF[0], dsi->CPUStop);
        }
    };
    auto reset = [&] {
        dsi->CurCPU = 0;
        dsi->Reset();
    };
    auto write = [&](unsigned cpu, u32 addr, u32 value) {
        if (cpu) dsi->ARM7Write32(addr, value);
        else     dsi->ARM9Write32(addr, value);
    };
    auto setup = [&](unsigned cpu, unsigned channel, u32 src, u32 dst,
                     u32 logical, u32 total, u32 control) {
        const u32 reg = 0x04004104 + channel * 0x1C;
        write(cpu, reg, src);
        write(cpu, reg + 4, dst);
        write(cpu, reg + 8, total);
        write(cpu, reg + 12, logical);
        write(cpu, reg + 16, 0); // No subblock interval / round-robin change.
        write(cpu, reg + 24, Enable | (1u << 30) | control);
    };
    auto step = [&](unsigned cpu) {
        dsi->CurCPU = cpu;
        // A scheduler slice smaller than one transfer: examine word boundaries
        // without asserting an undocumented DSi memory/bus duration.
        if (cpu) dsi->ARM7Target = dsi->ARM7Timestamp + 1;
        else     dsi->ARM9Target = dsi->ARM9Timestamp + 1;
        dsi->RunNDMAs(cpu);
    };

    for (unsigned cpu : {0u, 1u})
    {
        name = cpu ? "arm7-start-count-priority" : "arm9-start-count-priority";
        const unsigned before = failures;
        reset();
        for (unsigned i = 0; i < 8; ++i)
            write(cpu, Source + i * 4, 0xA1000000 + i);

        // Immediate transfer ignores TCNT. Lower priority channel is armed
        // first, but must not overwrite the destination before channel 0 ends.
        setup(cpu, 1, Source + 16, Dest, 1, 99, 0x10000000);
        setup(cpu, 0, Source, Dest, 4, 1, 0x10000000 | (2u << 16));
        for (unsigned i = 0; i < 4; ++i)
        {
            step(cpu);
            require(dsi->ARM9Read32(Dest + i * 4) == 0xA1000000 + i,
                    "higher priority word missing/overwritten");
            require((dsi->IF[cpu] & (Done0 | Done1)) == (i == 3 ? Done0 : 0),
                    "completion IRQ before final word / competing IRQ order");
        }
        require(!(dsi->NDMAs[cpu * 4].Cnt & Enable) &&
                (dsi->NDMAs[cpu * 4 + 1].Cnt & Enable), "channel enable at completion");
        step(cpu);
        require(dsi->ARM9Read32(Dest) == 0xA1000004 &&
                (dsi->IF[cpu] & (Done0 | Done1)) == (Done0 | Done1) &&
                !dsi->NDMAsRunning(cpu), "lower priority completion");

        // Existing VBlank startup with a final short logical block. The
        // physical block is four words; only the last three words remain.
        write(cpu, IFAddress, Done0 | Done1);
        setup(cpu, 0, Source, Dest + 32, 4, 7, 0x06000000 | (2u << 16));
        step(cpu);
        require(!dsi->NDMAsRunning(cpu) && dsi->ARM9Read32(Dest + 32) == 0,
                "timed channel started before request");
        for (unsigned block = 0; block < 2; ++block)
        {
            dsi->CheckDMAs(cpu, cpu ? 0x11 : 0x01); // Existing VBlank caller mapping.
            for (unsigned i = 0; i < (block ? 3u : 4u); ++i)
                step(cpu);
            require((dsi->IF[cpu] & Done0) == (block ? Done0 : 0),
                    "logical block IRQ did not respect total length");
            require(!dsi->NDMAsRunning(cpu), "logical block did not release CPU");
        }
        for (unsigned i = 0; i < 7; ++i)
            require(dsi->ARM9Read32(Dest + 32 + i * 4) == 0xA1000000 + i,
                    "timed transfer data");
        require(dsi->ARM9Read32(Dest + 60) == 0 && !(dsi->NDMAs[cpu * 4].Cnt & Enable),
                "final short block overran destination / remained enabled");
        std::printf("ndma/%s: %s\n", name, failures == before ? "PASS" : "FAIL");
    }

    for (bool fifoMode : {false, true})
    {
        name = fifoMode ? "gx-subdivision-stall-snapshot" : "immediate-gx-stall-snapshot";
        const unsigned before = failures;
        reset();
        dsi->ARM9Write16(0x04000304, 0x000F); // Geometry power, through real MMIO.
        const u32 words = fifoMode ? 224 : 112;
        for (u32 i = 0; i < words; ++i)
            dsi->ARM9Write32(Source + i * 4, i + 1 == words ? 0x11 : 0x00151515);
        // Three IDENTITY commands per word exceed the FIFO capacity. A final
        // PUSH makes a dropped tail visible in GXSTAT's projection stack bit.
        dsi->ARM9Write32(Source + 1024, 0xC001CAFE);
        setup(0, 0, Source, 0x04000400, words, words,
              (fifoMode ? 0x0A000000 : 0x10000000) | (2u << 10) | (4u << 16));
        setup(0, 1, Source + 1024, Dest, 1, 1, 0x10000000);
        dsi->ARM9Target = dsi->ARM9Timestamp + 1000000;
        dsi->RunNDMAs(0);
        require((dsi->CPUStop & CPUStop_GXStall) && dsi->NDMAs[0].IsRunning() &&
                (dsi->NDMAs[0].Cnt & Enable) && !(dsi->IF[0] & Done0),
                "FIFO full did not pause the active DMA before completion");
        require(dsi->ARM9Read32(Dest) == 0 && !(dsi->IF[0] & Done1),
                "competing ARM9 channel ran through GX stall");

        Savestate saved;
        if (!dsi->NDS::DoSavestate(&saved) || saved.Error)
        {
            require(false, "stalled full savestate failed");
            return 2;
        }
        saved.Finish();
        for (bool replay : {false, true})
        {
            if (replay)
            {
                reset();
                Savestate load(saved.Buffer(), saved.Length(), false);
                if (!dsi->NDS::DoSavestate(&load) || load.Error)
                {
                    require(false, "stalled full savestate load failed");
                    return 2;
                }
            }
            bool done = false;
            u32 gxstat = 0;
            for (unsigned slice = 0; slice < 4096 && !done; ++slice)
            {
                dsi->CurCPU = 0;
                dsi->ARM9Timestamp += 32u << dsi->ARM9ClockShift;
                dsi->GPU.GPU3D.Run();
                dsi->ARM9Target = dsi->ARM9Timestamp + (32u << dsi->ARM9ClockShift);
                dsi->RunNDMAs(0);
                gxstat = dsi->ARM9Read32(0x04000600);
                done = !(dsi->NDMAs[0].Cnt & Enable) && !dsi->NDMAsRunning(0) &&
                       !(gxstat & (1u << 27)) && !(dsi->CPUStop & CPUStop_GXStall);
            }
            require(done, "bounded DMA/GPU drain did not finish");
            require((gxstat & (1u << 13)) && !(gxstat & (1u << 15)),
                    "final PUSH was lost/duplicated after FIFO overflow");
            require((dsi->IF[0] & (Done0 | Done1)) == (Done0 | Done1) &&
                    dsi->ARM9Read32(Dest) == 0xC001CAFE &&
                    !(dsi->CPUStop & 0xF0), "completion IRQ / competing channel / CPU release");
            std::printf("ndma/%s/%s: drained=%d GXSTAT=%08x IF=%08x\n", name,
                        replay ? "replay" : "live", done, gxstat, dsi->IF[0]);
        }
        std::printf("ndma/%s: %s\n", name, failures == before ? "PASS" : "FAIL");
    }
    return failures ? 1 : 0;
}

// Arisotura's hardware observations, 2021-06-03: the last halfword of each
// 128 KiB GBA ROM block is nonsequential, including fixed/decrementing DMA.
// https://melonds.kuribo64.net/board/thread.php?pid=3805#3805
// EXMEMCNT first/second access durations are in 33 MHz bus cycles:
// https://problemkaputt.de/gbatek.htm#dsmemorycontrol
// Exercise the additive GBA slot <-> WRAM path. Main-RAM overlap and channel
// preemption need separate hardware measurements, not inferred cycle totals.
int TestDMASlotTiming(NDSArgs&& args)
{
    args.JIT = std::nullopt;
    struct DMAObservedNDS : NDS
    {
        using NDS::NDS;
        using NDS::DMAs;
    };
    auto nds = std::make_unique<DMAObservedNDS>(std::move(args));
    nds->SetGBACart(std::make_unique<GBACart::CartRAMExpansion>());
    nds->Reset();
    constexpr u32 SlotEnd = 0x09020000;
    constexpr u32 WRAM = 0x03001000;
    constexpr u32 DMA0 = 0x040000B0;
    constexpr u32 Enable = 1u << 31;
    constexpr u32 Done = 1u << IRQ_DMA0;
    constexpr u32 Guard = 0xCDEF;
    unsigned failures = 0;
    unsigned cases = 0;

    struct Transfer
    {
        const char* Name;
        unsigned Count;
        int Offset; // Units relative to the 128 KiB boundary.
        unsigned Mode;
        unsigned NonseqHalfwords;
    };
    // The counts below are from the bus rule, not DMA_Timings or the emulator's
    // memory tables: one initial N, plus every visit to the final halfword.
    // For a 16-bit initial access at that address, those are the same access.
    constexpr Transfer transfers[] = {
        {"one-before", 1, -2, 0, 1},
        {"one-terminal", 1, -1, 0, 2},
        {"one-after", 1, 0, 0, 1},
        {"two-through", 2, -2, 0, 2},
        {"many-through", 17, -9, 0, 2},
        {"decrement-through", 17, 7, 1, 2},
        {"fixed-terminal", 17, -1, 2, 18},
        {"fixed-interior", 17, -2, 2, 1},
        {"reload-through", 17, -9, 3, 2},
    };
    struct WaitSetting { u16 Control; unsigned Nonseq; unsigned Seq; };
    // Two documented EXMEMCNT settings, including both sequential speeds.
    constexpr WaitSetting waits[] = {{0x0000, 10, 6}, {0x001C, 18, 4}};

    for (unsigned cpu : {0u, 1u})
    {
        auto write16 = [&](u32 addr, u16 value) {
            if (cpu) nds->ARM7Write16(addr, value);
            else     nds->ARM9Write16(addr, value);
        };
        auto write32 = [&](u32 addr, u32 value) {
            if (cpu) nds->ARM7Write32(addr, value);
            else     nds->ARM9Write32(addr, value);
        };
        auto read16 = [&](u32 addr) {
            return cpu ? nds->ARM7Read16(addr) : nds->ARM9Read16(addr);
        };
        auto& timestamp = cpu ? nds->ARM7Timestamp : nds->ARM9Timestamp;
        auto& target = cpu ? nds->ARM7Target : nds->ARM9Target;
        auto& dma = nds->DMAs[cpu * 4];
        nds->CurCPU = cpu;
        nds->MapSharedWRAM(1);
        nds->MapSharedWRAM(cpu ? 3 : 0);
        write32(0x04000208, 1);
        write32(0x04000210, Done);

        for (const auto& wait : waits)
        for (unsigned width : {2u, 4u})
        for (bool toSlot : {false, true})
        for (const auto& test : transfers)
        {
            if (!toSlot && test.Mode == 3) continue; // Source mode 3 is reserved.
            nds->ARM9Write16(0x04000204, wait.Control | (cpu << 7));
            write16(0x04000204, wait.Control | (cpu << 7));
            write32(0x04000214, Done);
            timestamp = 0;
            target = 0;
            for (int i = -40; i < 40; ++i)
                write16(SlotEnd + i * 2, Guard);
            for (unsigned i = 0; i < 40; ++i)
                write16(WRAM + i * 2, Guard);

            const u32 slot = SlotEnd + test.Offset * static_cast<int>(width);
            const int stride = test.Mode == 1 ? -static_cast<int>(width) :
                               test.Mode == 2 ? 0 : static_cast<int>(width);
            const u32 source = toSlot ? WRAM : slot;
            const u32 dest = toSlot ? slot : WRAM;
            const int sourceStride = toSlot ? static_cast<int>(width) : stride;
            const int destStride = toSlot ? stride : static_cast<int>(width);
            auto pattern = [](u32 addr) -> u16 { return (addr >> 1) ^ 0x5A39; };
            for (unsigned i = 0; i < test.Count; ++i)
            for (unsigned half = 0; half < width; half += 2)
            {
                const u32 addr = source + i * sourceStride + half;
                write16(addr, pattern(addr));
            }

            write32(DMA0, source);
            write32(DMA0 + 4, dest);
            write32(DMA0 + 8, Enable | (1u << 30) | (width == 4 ? 1u << 26 : 0) |
                    (test.Mode << (toSlot ? 21 : 23)) | test.Count);
            bool ok = dma.IsRunning() && !(nds->IF[cpu] & Done);
            dma.Run(); // No budget: must not consume the initial burst access.
            ok &= timestamp == 0 && !(nds->IF[cpu] & Done);
            bool dataOK = true;
            bool irqOK = true;
            u64 firstAt = 0;
            for (unsigned i = 0; i < test.Count; ++i)
            {
                // One unit per scheduler slice; resuming is not a new burst.
                target = timestamp + 1;
                dma.Run();
                if (i == 0) firstAt = timestamp;
                for (unsigned half = 0; half < width; half += 2)
                    dataOK &= read16(dest + i * destStride + half) ==
                          pattern(source + i * sourceStride + half);
                irqOK &= bool(nds->IF[cpu] & Done) == (i + 1 == test.Count);
                ok &= dma.IsRunning() == (i + 1 < test.Count);
            }

            const unsigned visits = test.NonseqHalfwords -
                (width == 2 && test.Offset == -1 ? 1 : 0);
            const unsigned expected = test.Count * (width / 2 * wait.Seq + 1) +
                                      visits * (wait.Nonseq - wait.Seq);
            const unsigned firstExpected = wait.Nonseq + 1 + (width == 4 ?
                (test.Offset == -1 ? wait.Nonseq : wait.Seq) : 0);
            const unsigned shift = cpu ? 0 : nds->ARM9ClockShift;
            ok &= timestamp == (u64{expected} << shift);
            ok &= firstAt == (u64{firstExpected} << shift);
            ok &= !(dma.Cnt & Enable) && !(nds->CPUStop & (1u << (cpu * 16)));
            irqOK &= (cpu ? nds->ARM7.IRQ : nds->ARM9.IRQ) == 1;
            // Verify the entire destination plus guards, including the final
            // value of a fixed destination and both halves of a word transfer.
            const u32 checkStart = toSlot ? SlotEnd - 80 : WRAM;
            const unsigned checkHalves = toSlot ? 80 : 40;
            for (unsigned h = 0; h < checkHalves; ++h)
            {
                const u32 addr = checkStart + h * 2;
                u16 expectedData = Guard;
                for (unsigned i = 0; i < test.Count; ++i)
                for (unsigned half = 0; half < width; half += 2)
                    if (addr == dest + i * destStride + half)
                        expectedData = pattern(source + i * sourceStride + half);
                dataOK &= read16(addr) == expectedData;
            }
            write32(0x04000214, Done);
            const u64 completedAt = timestamp;
            target = timestamp + 100;
            dma.Run();
            ok &= timestamp == completedAt && !(nds->IF[cpu] & Done);
            ok &= dataOK && irqOK;
            ++cases;
            failures += !ok;
            std::printf("{\"case\":\"%s\",\"cpu\":%u,\"bits\":%u,\"to_slot\":%s,"
                        "\"units\":%u,\"exmem\":%u,\"cycles\":%llu,\"expected\":%u,"
                        "\"first\":%llu,\"first_expected\":%u,\"data_ok\":%s,\"irq_ok\":%s,\"ok\":%s}\n",
                        test.Name, cpu ? 7 : 9, width * 8, toSlot ? "true" : "false",
                        test.Count, wait.Control,
                        static_cast<unsigned long long>(completedAt >> shift), expected,
                        static_cast<unsigned long long>(firstAt >> shift), firstExpected,
                        dataOK ? "true" : "false", irqOK ? "true" : "false",
                        ok ? "true" : "false");
        }
    }
    std::printf("{\"test\":\"dma-slot-boundary\",\"cases\":%u,\"failed\":%u}\n", cases, failures);
    return failures ? 1 : 0;
}
