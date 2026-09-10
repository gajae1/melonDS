// SPDX-License-Identifier: GPL-3.0-or-later
// Real DSi core, SDIO CMD52, HTC/WMI mailbox and card/ARM7 IRQs. Generated
// packets only. No guest CPU, private images, network connections or files.
// Payload contracts: Linux v2.6.39 ath6kl/include/common/wmi.h and
// devkitPro/calico include/calico/dev/ar6k/wmi.h.
#include "Platform.h"
// Reuse the existing headless host, substituting only its two network sinks.
#define Net_SendPacket UnusedHeadlessNetSendPacket
#define Net_RecvPacket UnusedHeadlessNetRecvPacket
#include "PlatformHeadless.cpp"
#undef Net_SendPacket
#undef Net_RecvPacket
#include "Args.h"
#include "DSi.h"
#include "WifiAP.h"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace melonDS;
static std::vector<u8> SentPacket;
namespace melonDS::Platform
{
int Net_SendPacket(u8* data, int len, void*)
{
    SentPacket.assign(data, data + len);
    return len;
}
int Net_RecvPacket(u8*, void*) { return 0; }
}

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
static u16 LE16(const u8* bytes) { return bytes[0] | (bytes[1] << 8); }
static void Put16(std::vector<u8>& bytes, u16 value)
{
    bytes.push_back(value); bytes.push_back(value >> 8);
}

struct Device
{
    std::unique_ptr<DSi> Core = std::make_unique<DSi>(DSiArgs{});
    static constexpr u32 CardIRQs = (1u << IRQ2_DSi_SDIO) | (1u << IRQ2_DSi_SDIO_Data1);

    u8 IO(bool write, unsigned func, unsigned addr, u8 value = 0)
    {
        auto& host = Core->SDIO;
        const u32 param = (write ? 0x80000000u : 0) | (func << 28) | (addr << 9) | value;
        host.Write(0x004, param); host.Write(0x006, param >> 16);
        host.Write(0x000, 52);
        return host.Read(0x00C) & 0xFF;
    }
    void Packet(const std::vector<u8>& bytes)
    {
        Require(bytes.size() <= 256, "generated packet fits mailbox aperture");
        for (unsigned i = 0; i < bytes.size(); ++i)
            IO(true, 1, 0x100 - bytes.size() + i, bytes[i]);
    }
    void Command(u16 cmd, const std::vector<u8>& body = {}, bool credit = false)
    {
        std::vector<u8> bytes{1, u8(credit), u8(body.size()+2), 0, 0, 0};
        Put16(bytes, cmd);
        bytes.insert(bytes.end(), body.begin(), body.end());
        Packet(bytes);
    }
    bool Pending() { return IO(false, 1, 0x400) & 1; }
    std::vector<u8> Frame()
    {
        Require(Pending(), "expected guest-visible mailbox event is missing");
        Require((Core->SDIO.Read(0x036) & 1) != 0, "mailbox data must assert card IRQ");
        Require((Core->IF2 & CardIRQs) == CardIRQs, "mailbox must raise both ARM7 SDIO IRQs");
        std::vector<u8> frame(128);
        for (auto& byte : frame) byte = IO(false, 1, 0xFFF);
        Require(LE16(frame.data()+2)+6 <= frame.size(), "generated event fits one HTC block");
        frame.resize(LE16(frame.data()+2)+6);
        Require(bool(Core->SDIO.Read(0x036) & 1) == Pending(), "card IRQ follows remaining mailbox bytes");
        // IF2 is a latched guest interrupt; draining the mailbox clears the
        // device line but must not clear the ARM7 acknowledgement register.
        Require((Core->IF2 & CardIRQs) == CardIRQs, "IF2 stays latched until guest acknowledgement");
        if (!Pending())
        {
            Core->ARM7IOWrite32(0x0400021C, CardIRQs);
            Require((Core->IF2 & CardIRQs) == 0, "guest IF2 acknowledgement clears latched card IRQs");
        }
        return frame;
    }
    std::vector<u8> Event(u16 id)
    {
        auto frame = Frame();
        Require(frame[0] == 1 && LE16(frame.data()+6) == id, "unexpected WMI event/order");
        Require(frame[4] <= frame.size()-8, "trailer within event");
        std::printf("event=%04X payload=%u card=%u IF2=%04X\n", LE16(frame.data()+6),
            unsigned(frame.size()-8-frame[4]), Core->SDIO.Read(0x036)&1, Core->IF2 & CardIRQs);
        return {frame.begin()+8, frame.end()-frame[4]};
    }
    void Credit()
    {
        auto frame = Frame();
        Require(frame[0] == 0 && frame[4] == 12 && frame[6] == 1 &&
            frame[7] == 2 && frame[8] == 1 && frame[9] == 1, "event precedes correct endpoint credit report");
        Require(!Pending(), "no extra response after event and credit");
    }
    Device()
    {
        Core->Reset();
        IO(true, 0, 4, 3); // function interrupt + master enable
        IO(true, 1, 0x418, 1); // mailbox 0 interrupt
        Core->SDIO.Write(0x038, 0); // unmask SDIO card interrupt
        Core->SDIO.Write(0x034, 1);
        Packet({1, 0, 0, 0}); // BMI_DONE: emulator's public firmware-upload bypass
        Frame();
        Packet({0, 0, 2, 0, 0, 0, 4, 0}); // HTC_SETUP_COMPLETE
        Event(0x1001); Event(0x1006);
        Require(!Pending(), "initialization event queue drained");
    }
    void Tick(unsigned count)
    {
        // Dispatch only the registered hardware millisecond callback. Guest
        // execution/scheduler timing is outside this bounded device fixture.
        while (count--)
        {
            auto event = Core->SchedList[Event_DSi_NWifi];
            Core->CancelEvent(Event_DSi_NWifi);
            event.Funcs[event.FuncID](event.That, event.Param);
        }
    }
    void Data(bool connected)
    {
        // WMI 802.3 LLC/SNAP packet carrying a four-byte test payload.
        std::vector<u8> body{0, 0, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            2,0,0,0,0,1, 0,12, 0xAA,0xAA,3,0,0,0, 0x88,0xB5, 1,2,3,4};
        std::vector<u8> packet{2,0,u8(body.size()),0,0,0};
        packet.insert(packet.end(), body.begin(), body.end());
        SentPacket.clear(); Packet(packet);
        const std::vector<u8> expected{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            2,0,0,0,0,1, 0x88,0xB5, 1,2,3,4};
        Require(connected ? SentPacket == expected : SentPacket.empty(),
            "connection state must gate real WMI-to-Ethernet delivery");
    }
};

static std::vector<u8> Profile()
{
    std::vector<u8> profile(52);
    profile[0] = profile[1] = profile[2] = profile[3] = profile[5] = 1;
    profile[7] = 7;
    std::memcpy(profile.data()+8, "melonAP", 7);
    profile[40] = 0x85; profile[41] = 9; // 2437 MHz
    std::memcpy(profile.data()+42, WifiAP::APMac, 6);
    return profile;
}
static void Connected(Device& device, bool payloadCheck = true)
{
    auto payload = device.Event(0x1002);
    Require(payload.size() >= 19 && LE16(payload.data()) == 2437 &&
        std::memcmp(payload.data()+2, WifiAP::APMac, 6) == 0, "connected AP identity");
    if (payloadCheck)
    {
        std::printf("connect IE bytes declared=%u available=%u\n",
            payload[16]+payload[17]+payload[18], unsigned(payload.size()-19));
        Require(unsigned(payload[16]+payload[17]+payload[18]) <= payload.size()-19,
            "connect event advertises association bytes that were never sent");
        Require(payload[17] >= 4 && payload[18] >= 6,
            "association request/response retain mandatory fixed headers");
        const unsigned response = 19 + payload[16] + payload[17];
        Require(LE16(payload.data()+response+2) == 0 &&
            (LE16(payload.data()+response+4) & 0x3FFF) == 1,
            "successful association response carries status and AID");
        unsigned start = 19;
        for (unsigned block = 0; block < 3; ++block)
        {
            const unsigned end = start + payload[16+block];
            unsigned cursor = start + (block == 0 ? 0 : block == 1 ? 4 : 6);
            while (cursor < end)
            {
                Require(end-cursor >= 2 && payload[cursor+1] <= end-cursor-2,
                    "association information elements remain inside their declared block");
                cursor += 2 + payload[cursor+1];
            }
            start = end;
        }
    }
    device.Data(true);
}
static void Disconnect(Device& device)
{
    device.Command(3, {}, true);
    auto payload = device.Event(0x1003);
    Require(payload.size() >= 10 && LE16(payload.data()) == 3 && payload[8] == 3 &&
        payload[9] <= payload.size()-10, "explicit disconnect reason and response span");
    device.Credit(); device.Data(false);
}
static void Refusal(bool initiallyConnected)
{
    Device device;
    if (initiallyConnected) { device.Command(1, Profile()); Connected(device, false); }
    auto refused = Profile(); refused[1] = 2; // Shared-key auth: already refused by melonAP.
    device.Command(1, refused, true);
    auto payload = device.Event(0x1005);
    Require(payload == std::vector<u8>({1,0,1}), "CONNECT INVALID_PARAM payload");
    device.Credit(); device.Data(initiallyConnected);
    Disconnect(device);
    device.Command(1, Profile()); Connected(device);
    Disconnect(device);
    device.Command(1, Profile()); Connected(device);
}
static void ScanRefusal()
{
    Device device;
    std::vector<u8> scan(18); scan[8] = 1; // existing bounded 8 ms scan
    device.Command(7, scan); device.Tick(3);
    auto refused = scan; refused[8] = 100; refused[16] = 0xFF;
    device.Command(7, refused, true);
    auto payload = device.Event(0x1005);
    Require(payload == std::vector<u8>({7,0,1}), "START_SCAN INVALID_PARAM payload");
    device.Credit(); device.Tick(4);
    Require(!device.Pending(), "rejected scan must preserve original completion time");
    device.Tick(1);
    Require(device.Event(0x100A) == std::vector<u8>(4), "original scan completes successfully");
    scan[16] = 1; device.Command(7, scan); device.Tick(8);
    Require(device.Event(0x100A) == std::vector<u8>(4), "valid short scan recovers");
    device.Command(1, Profile()); Connected(device); Disconnect(device);
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    try
    {
        const std::string mode = argv[1];
        if (mode == "connect-refusal") Refusal(false);
        else if (mode == "connected-refusal") Refusal(true);
        else if (mode == "scan-refusal") ScanRefusal();
        else if (mode == "connect-payload")
        {
            Device device; device.Command(1, Profile()); Connected(device); Disconnect(device);
        }
        else return 2;
        std::printf("PASS %s\n", argv[1]);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL %s: %s\n", argv[1], error.what()); return 1;
    }
}
