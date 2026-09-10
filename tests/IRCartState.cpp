// SPDX-License-Identifier: GPL-3.0-or-later
// Real cartridge classes and Savestate.cpp, with generated ROM/SRAM only.
// Save notifications are recorded; ROM-bus crypto is outside this SPI test.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>
#include "NDSCart.h"
#include "NDSCart/CartRetailIR.h"

using namespace melonDS;
using namespace melonDS::NDSCart;
using Bytes = std::vector<u8>;
struct SaveNotice { u32 total, offset, length; };
static std::vector<SaveNotice> notices;
namespace melonDS::Platform
{
void Log(LogLevel, const char*, ...) {}
void WriteNDSSave(const u8*, u32 total, u32 offset, u32 length, void*)
{
    notices.push_back({total, offset, length});
}
}
namespace melonDS::NDSCart
{
void NDSCartSlot::Key1_Decrypt(u32*) const noexcept { std::abort(); }
void NDSCartSlot::Key1_InitKeycode(bool, u32, u32, u32) noexcept { std::abort(); }
}

static int failures = 0;
static void Check(bool condition, const char* message)
{
    if (!condition) { ++failures; std::fprintf(stderr, "%s\n", message); }
}

static std::unique_ptr<CartRetailIR> MakeCart(u32 version = 1, u32 saveType = 5)
{
    auto rom = std::make_unique<u8[]>(sizeof(NDSHeader));
    ROMListEntry params{0, sizeof(NDSHeader), saveType};
    auto cart = std::make_unique<CartRetailIR>(std::move(rom), sizeof(NDSHeader), 0, version,
                                             false, params, nullptr, 0, nullptr);
    cart->Reset();
    cart->GetSaveMemory()[0x1234] = 0x4A;
    cart->GetSaveMemory()[0x1235] = 0x9C;
    notices.clear();
    return cart;
}

static void Send(CartRetailIR& cart, std::initializer_list<u8> bytes,
                 std::initializer_list<u8> expected)
{
    Check(bytes.size() == expected.size(), "Invalid SPI trace fixture");
    auto answer = expected.begin();
    for (u8 byte : bytes)
    {
        const u8 received = cart.SPITransmitReceive(byte);
        if (received != *answer)
        {
            ++failures;
            std::fprintf(stderr, "SPI byte %02X: got %02X, expected %02X\n", byte, received, *answer);
        }
        ++answer;
    }
}

static void ID(CartRetailIR& cart)
{
    cart.SPISelect();
    Send(cart, {0x08, 0, 0xFF, 0x08}, {0, 0xAA, 0xAA, 0xAA});
    cart.SPIRelease();
}

static Bytes Save(CartRetailIR& cart, bool legacy = false, u8 legacyCommand = 0)
{
    Savestate state(1024);
    if (legacy)
    {
        // Exact 14.0 layout: the real base state followed by IRCmd only.
        cart.CartRetail::DoSavestate(&state);
        state.Var8(&legacyCommand);
    }
    else cart.DoSavestate(&state);
    state.Section("TAIL");
    u32 marker = 0x1234ABCD;
    state.Var32(&marker);
    state.Finish();
    Check(!state.Error, "Generated cartridge savestate could not be saved");
    const auto* data = static_cast<const u8*>(state.Buffer());
    Bytes result(data, data + state.Length());
    if (legacy) { result[6] = 0; result[7] = 0; }
    return result;
}

static void Restore(CartRetailIR& cart, Bytes& bytes)
{
    Savestate state(bytes.data(), static_cast<u32>(bytes.size()), false);
    cart.DoSavestate(&state);
    Check(!state.Error, "Cartridge savestate could not be restored");
    // The cartridge must stop at its own record, before the following section.
    Check(state.Length() + 20 == bytes.size(), "IR state consumed the wrong record length");
    state.Section("TAIL");
    u32 marker = 0;
    state.Var32(&marker);
    Check(!state.Error && marker == 0x1234ABCD, "IR restore damaged the following state section");
}

static void StartRead(CartRetailIR& cart, bool flash = true)
{
    cart.SPISelect();
    Send(cart, {0x00, 0x03}, {0, 0xFF});
    if (flash) Send(cart, {0}, {0});
    Send(cart, {0x12, 0x34}, {0, 0});
}

static void Put32(Bytes& bytes, size_t offset, u32 value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string_view test = argv[1];
    if (test == "id-control")
    {
        for (u32 version : {1U, 2U})
        {
            auto cart = MakeCart(version);
            ID(*cart);
            ID(*cart); // Reselect starts a new IR command.
            Check(notices.empty(), "IR ID unexpectedly wrote SRAM");
        }
    }
    else if (test == "save-control")
    {
        for (u32 type : {2U, 5U})
        {
            auto cart = MakeCart(1, type);
            const bool flash = type == 5;
            StartRead(*cart, flash);
            Send(*cart, {0, 0}, {0x4A, 0x9C});
            cart->SPIRelease();
            cart->SPISelect();
            Send(*cart, {0, 0x06}, {0, 0}); // Save write enable through IR prefix.
            cart->SPIRelease();
            cart->SPISelect();
            Send(*cart, {0, u8(flash ? 0x0A : 0x02)}, {0, 0xFF});
            if (flash) Send(*cart, {0}, {0});
            Send(*cart, {0x12, 0x34, 0x77, 0x66}, {0, 0, 0, 0});
            cart->SPIRelease();
            Check(cart->GetSaveMemory()[0x1234] == 0x77 && cart->GetSaveMemory()[0x1235] == 0x66,
                  "IR passthrough changed save write behavior");
            Check(notices.size() == 1 && notices[0].total == cart->GetSaveMemoryLength() &&
                  notices[0].offset == 0x1234 && notices[0].length == 2,
                  "Save SPI did not notify the exact changed range once");
        }
    }
    else if (test == "unknown-command")
    {
        auto cart = MakeCart();
        const Bytes before(cart->GetSaveMemory(), cart->GetSaveMemory() + cart->GetSaveMemoryLength());
        for (u8 command : {u8(0x01), u8(0x09), u8(0xFF)})
        {
            ID(*cart);
            cart->SPISelect();
            Send(*cart, {command}, {0});
            // 0xFF is the emulator's unsupported-response policy, following
            // CartRetail SPI defaults. This is not a captured hardware trace.
            Send(*cart, {0, 0x06, 0x08, 0xFF}, {0xFF, 0xFF, 0xFF, 0xFF});
            cart->SPIRelease();
        }
        cart->SPISelect();
        Send(*cart, {0, 0x05, 0}, {0, 0xFF, 0});
        Check(notices.empty() && std::equal(before.begin(), before.end(), cart->GetSaveMemory()),
              "Unsupported IR command changed save state or reached a save callback");
    }
    else if (test == "reset-command")
    {
        auto cart = MakeCart();
        ID(*cart);
        cart->Reset();
        Send(*cart, {0x08, 0}, {0, 0xAA});
    }
    else if (test == "restore-id")
    {
        auto cart = MakeCart();
        cart->SPISelect();
        Send(*cart, {0x08}, {0});
        Bytes state = Save(*cart);
        cart->SPISelect(); // Dirty the live position to the opposite phase.
        Restore(*cart, state);
        Send(*cart, {0, 0xFF}, {0xAA, 0xAA});
    }
    else if (test == "restore-command-start")
    {
        auto cart = MakeCart();
        ID(*cart);
        cart->SPISelect();
        Bytes state = Save(*cart); // Next byte has to be a command, not ID data.
        Send(*cart, {0x08, 0}, {0, 0xAA});
        Restore(*cart, state);
        Send(*cart, {0x08, 0}, {0, 0xAA});
    }
    else if (test == "restore-save")
    {
        auto cart = MakeCart();
        StartRead(*cart);
        Bytes state = Save(*cart);
        Send(*cart, {0}, {0x4A});
        cart->GetSaveMemory()[0x1234] = 0xEE;
        cart->SPISelect();
        Restore(*cart, state);
        Send(*cart, {0, 0}, {0x4A, 0x9C});
    }
    else if (test == "restore-write")
    {
        auto cart = MakeCart();
        cart->SPISelect();
        Send(*cart, {0, 0x06}, {0, 0});
        cart->SPIRelease();
        cart->SPISelect();
        Send(*cart, {0, 0x0A, 0, 0x12, 0x34, 0x77}, {0, 0xFF, 0, 0, 0, 0});
        Bytes state = Save(*cart);
        Send(*cart, {0xEE}, {0});
        cart->SPIRelease();
        ID(*cart);
        Restore(*cart, state);
        Check(cart->GetSaveMemory()[0x1234] == 0x77 && cart->GetSaveMemory()[0x1235] == 0x9C,
              "Mid-write restore did not restore the saved SRAM snapshot");
        notices.clear(); // The base loader already notified the restored whole save.
        Send(*cart, {0x66}, {0});
        cart->SPIRelease();
        Check(cart->GetSaveMemory()[0x1234] == 0x77 && cart->GetSaveMemory()[0x1235] == 0x66,
              "Mid-write restore did not resume at the next SRAM address");
        Check(notices.size() == 1 && notices[0].offset == 0x1234 && notices[0].length == 2,
              "Resumed SPI write lost its original pending save range");
    }
    else if (test == "truncated-section")
    {
        for (u32 retained : {0U, 1U, 4U})
        {
            auto cart = MakeCart();
            cart->SPISelect();
            Send(*cart, {0x01}, {0});
            Bytes state = Save(*cart);
            const size_t end = state.size() - 20; // Following TAIL section stays intact.
            const u32 removed = 5 - retained;
            state.erase(state.begin() + end - removed, state.begin() + end);
            Put32(state, 8, static_cast<u32>(state.size()));
            Put32(state, 20, static_cast<u32>(end - removed - 16));
            ID(*cart);
            Savestate loaded(state.data(), static_cast<u32>(state.size()), false);
            Check(!loaded.Error, "Truncation fixture has an invalid global header");
            cart->DoSavestate(&loaded);
            Check(loaded.Error, "Truncated 14.1 IR record consumed bytes from the next section");
            Check(loaded.Length() <= end - removed, "Truncated IR read crossed the NDCS boundary");
            Send(*cart, {0}, {0xAA}); // No partial IRCmd/IRPos replacement on rejection.
        }
    }
    else if (test == "position-wrap")
    {
        auto cart = MakeCart();
        ID(*cart);
        Bytes state = Save(*cart);
        Put32(state, state.size() - 24, 0xFFFFFFFF); // Final IRPos, before the TAIL section.
        Restore(*cart, state);
        Send(*cart, {0, 0x08}, {0xAA, 0xAA});
    }
    else if (test == "legacy-boundary")
    {
        auto cart = MakeCart();
        StartRead(*cart);
        Bytes state = Save(*cart, true, 0);
        ID(*cart);
        Restore(*cart, state);
        // 14.0 has no IR position: resume at a deterministic new command
        // boundary, retaining SRAM contents rather than the live IR phase.
        Send(*cart, {0, 0x06}, {0, 0});
        cart->SPIRelease();
        cart->SPISelect();
        Send(*cart, {0, 0x05, 0}, {0, 0xFF, 2});
        Check(cart->GetSaveMemory()[0x1234] == 0x4A, "Legacy restore changed saved SRAM bytes");
    }
    else return 2;
    std::printf("%s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
