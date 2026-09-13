// SPDX-License-Identifier: GPL-3.0-or-later
// Real CartRetail SPI and Savestate definitions, generated ROM/SRAM only.
// M95040-W DS1639 rev14, section 6.6: WRITE wraps within a 16-byte page.
// https://www.st.com/resource/en/datasheet/m95040-w.pdf#page=19
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <vector>
#include "NDSCart.h"
#include "NDSCart/CartRetail.h"

using namespace melonDS;
using namespace melonDS::NDSCart;
using Bytes = std::vector<u8>;

static int failures = 0;
static void Check(bool condition, const char* message)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "%s\n", message);
    }
}

struct SaveSink
{
    Bytes Persisted;
    unsigned Notices = 0;
};

// Explicit chip fixtures: ST M95640-W, M95512-W, M95M01-R. Legacy codes
// describe only the old capacity/protocol, not these chips' page geometry.
struct EEPROMProfile { u32 Type, Legacy, Capacity, Page, AddressBytes; };
static constexpr EEPROMProfile EEPROMProfiles[] = {
    {11, 2, 8192, 32, 2}, {12, 3, 65536, 128, 2}, {13, 4, 131072, 256, 3}
};

namespace melonDS::Platform
{
void Log(LogLevel, const char*, ...) {}
void WriteNDSSave(const u8* data, u32 total, u32 offset, u32 length, void* userdata)
{
    auto& sink = *static_cast<SaveSink*>(userdata);
    ++sink.Notices;
    if (total != sink.Persisted.size() || offset >= total || length > total)
    {
        Check(false, "Save callback has an invalid size or dirty range");
        return;
    }
    // Apply only the notified bytes. A range may wrap at the end of the whole
    // save, as supported by the host API; it cannot imply an EEPROM page wrap.
    for (u32 i = 0; i < length; ++i)
    {
        const u32 index = (offset + i) % total;
        sink.Persisted[index] = data[index];
    }
}
}

namespace melonDS::NDSCart
{
// ROM-bus encryption is outside this SPI fixture, as in IRCartState.
void NDSCartSlot::Key1_Decrypt(u32*) const noexcept { std::abort(); }
void NDSCartSlot::Key1_InitKeycode(bool, u32, u32, u32) noexcept { std::abort(); }
}

struct Fixture
{
    SaveSink Sink;
    std::unique_ptr<CartRetail> Cart;

    explicit Fixture(u32 saveType = 1, u32 padding = 0, u8 seed = 0x5A)
    {
        constexpr u32 sizes[] = {0, 512, 8192, 65536, 131072, 262144, 524288, 1048576};
        u32 physical;
        if (saveType <= 7) physical = sizes[saveType];
        else if (saveType >= 11 && saveType <= 13) physical = EEPROMProfiles[saveType - 11].Capacity;
        else if (saveType == 14) physical = 32768; // Infineon FM25W256
        else if (saveType >= 15 && saveType <= 17) physical = 262144u << (saveType - 15);
        else std::abort();
        const u32 size = physical + padding;
        Sink.Persisted.resize(size);
        for (u32 i = 0; i < size; ++i)
            Sink.Persisted[i] = u8((i * 37 + (i >> 8) * 19) ^ seed);
        auto sram = std::make_unique<u8[]>(size);
        std::copy(Sink.Persisted.begin(), Sink.Persisted.end(), sram.get());
        auto rom = std::make_unique<u8[]>(sizeof(NDSHeader));
        ROMListEntry params{0, sizeof(NDSHeader), saveType};
        Cart = std::make_unique<CartRetail>(std::move(rom), sizeof(NDSHeader), 0, false,
                                            params, std::move(sram), size, &Sink);
        Cart->Reset();
    }
};

static void CheckImage(const Fixture& f, const Bytes& expected)
{
    Check(f.Cart->GetSaveMemoryLength() == expected.size(), "SRAM backing length changed");
    if (f.Cart->GetSaveMemoryLength() != expected.size()) return;
    const u8* actual = f.Cart->GetSaveMemory();
    for (size_t i = 0; i < expected.size(); ++i)
    {
        if (actual[i] != expected[i])
        {
            ++failures;
            std::fprintf(stderr, "SRAM[%04zX]: got %02X, expected %02X\n",
                         i, actual[i], expected[i]);
            break;
        }
    }
    for (size_t i = 0; i < f.Sink.Persisted.size(); ++i)
    {
        if (f.Sink.Persisted[i] != actual[i])
        {
            ++failures;
            std::fprintf(stderr, "Callback replay[%04zX]: got %02X, SRAM contains %02X\n",
                         i, f.Sink.Persisted[i], actual[i]);
            break;
        }
    }
}

static void CheckMemory(const Fixture& f, const Bytes& expected, const char* reason)
{
    Check(f.Cart->GetSaveMemoryLength() == expected.size() &&
          std::equal(expected.begin(), expected.end(), f.Cart->GetSaveMemory()), reason);
}

static void Send(CartRetail& cart, std::initializer_list<u8> bytes)
{
    for (u8 byte : bytes) cart.SPITransmitReceive(byte);
}

static void CompleteChip(CartRetail& cart)
{
#if SAVESTATE_MAX_MINOR >= 10
    cart.CompleteSave();
#endif
}
static u32 ChipDelay(const CartRetail& cart)
{
#if SAVESTATE_MAX_MINOR >= 10
    return cart.GetSaveDelay();
#else
    return 0;
#endif
}
// Existing chip-only cases inspect the completed transaction. Actual deadline
// delivery belongs to the real scheduler tests in CartSPI.
static void ReleaseAndComplete(CartRetail& cart)
{
    cart.SPIRelease();
    CompleteChip(cart);
}

static void Command(CartRetail& cart, u8 command)
{
    cart.SPISelect();
    cart.SPITransmitReceive(command);
    ReleaseAndComplete(cart);
}

static u8 Status(CartRetail& cart)
{
    cart.SPISelect();
    cart.SPITransmitReceive(0x05);
    const u8 result = cart.SPITransmitReceive(0);
    ReleaseAndComplete(cart);
    return result;
}

static void StartTinyWrite(CartRetail& cart, u16 addr)
{
    cart.SPISelect();
    Send(cart, {u8(addr >= 0x100 ? 0x0A : 0x02), u8(addr)});
}

static void CheckRead(CartRetail& cart, u16 addr, std::initializer_list<u8> expected)
{
    cart.SPISelect();
    Send(cart, {u8(addr >= 0x100 ? 0x0B : 0x03), u8(addr)});
    for (u8 value : expected)
        Check(cart.SPITransmitReceive(0) == value, "Tiny EEPROM READ returned the wrong byte");
    ReleaseAndComplete(cart);
}

static void Control()
{
    Fixture f;
    auto& cart = *f.Cart;
    Bytes expected = f.Sink.Persisted;
    StartTinyWrite(cart, 0x32); // WREN is required.
    Send(cart, {0xA1, 0xB2});
    ReleaseAndComplete(cart);
    CheckImage(f, expected);
    Check(f.Sink.Notices == 0 && !(Status(cart) & 2), "Disabled WRITE changed persistence or WEL");

    Command(cart, 0x06);
    StartTinyWrite(cart, 0x32); // Address-only WRITE has no data to commit.
    ReleaseAndComplete(cart);
    Check(f.Sink.Notices == 0 && (Status(cart) & 2), "Empty WRITE consumed WEL or notified a save");
    StartTinyWrite(cart, 0x32);
    Send(cart, {0xE7, 0xD4, 0xC3});
    Check(f.Sink.Notices == 0, "WRITE notified persistence before CS release");
    ReleaseAndComplete(cart);
    expected[0x32] = 0xE7;
    expected[0x33] = 0xD4;
    expected[0x34] = 0xC3;
    CheckImage(f, expected);
    Check(f.Sink.Notices == 1 && !(Status(cart) & 2), "WRITE did not commit once and clear WEL");
    ReleaseAndComplete(cart);
    Check(f.Sink.Notices == 1, "Repeated CS release committed the same WRITE again");
    CheckRead(cart, 0x32, {0xE7, 0xD4, 0xC3});

    Command(cart, 0x06);
    Command(cart, 0x04); // WRDI must still protect later writes.
    StartTinyWrite(cart, 0x32);
    Send(cart, {0x11});
    ReleaseAndComplete(cart);
    CheckImage(f, expected);
    Check(f.Sink.Notices == 1 && !(Status(cart) & 2), "WRDI failed to disable WRITE");
    // READ advances through pages/banks and wraps across the entire 512 bytes.
    CheckRead(cart, 0xFF, {expected[0xFF], expected[0x100]});
    CheckRead(cart, 0x1FF, {expected[0x1FF], expected[0]});
}

static void PageEnds()
{
    struct Boundary { u16 Last, First, Second; };
    for (const auto& edge : {Boundary{0x00F, 0x000, 0x001},
                             Boundary{0x0FF, 0x0F0, 0x0F1},
                             Boundary{0x1FF, 0x1F0, 0x1F1}})
    {
        Fixture f;
        Bytes expected = f.Sink.Persisted;
        Command(*f.Cart, 0x06);
        StartTinyWrite(*f.Cart, edge.Last);
        Send(*f.Cart, {0xA1, 0xB2, 0xC3});
        Check(f.Sink.Notices == 0, "Page WRITE committed before CS release");
        ReleaseAndComplete(*f.Cart);
        expected[edge.Last] = 0xA1;
        expected[edge.First] = 0xB2;
        expected[edge.Second] = 0xC3;
        CheckImage(f, expected);
        CheckRead(*f.Cart, edge.First, {0xB2, 0xC3});
        Check(f.Sink.Notices == 1, "Page WRITE must issue one save notification");
    }
}

static void PageOverflow()
{
    {
        Fixture f;
        Bytes expected = f.Sink.Persisted;
        Command(*f.Cart, 0x06);
        StartTinyWrite(*f.Cart, 0xF8);
        for (u8 byte = 0x80; byte < 0xA8; ++byte) f.Cart->SPITransmitReceive(byte);
        ReleaseAndComplete(*f.Cart);
        // Forty bytes from offset 8: the last full page is 98..9F,A0..A7.
        constexpr std::array<u8, 16> finalPage{0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
                                             0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7};
        std::copy(finalPage.begin(), finalPage.end(), expected.begin() + 0xF0);
        CheckImage(f, expected);
    }
    {
        Fixture f;
        Bytes expected = f.Sink.Persisted;
        Command(*f.Cart, 0x06);
        StartTinyWrite(*f.Cart, 0x1F0);
        // Thirty-two laps also expose an incorrectly masked dirty length of 0.
        for (unsigned i = 0; i < 512; ++i) f.Cart->SPITransmitReceive(u8(i));
        ReleaseAndComplete(*f.Cart);
        constexpr std::array<u8, 16> finalPage{0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
                                             0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};
        std::copy(finalPage.begin(), finalPage.end(), expected.begin() + 0x1F0);
        CheckImage(f, expected);
        Check(f.Sink.Notices == 1, "Repeated page wrap did not notify its final contents");
    }
}

static void RestoreWrite()
{
    Fixture f;
    auto& cart = *f.Cart;
    Bytes expected = f.Sink.Persisted;
    Command(cart, 0x06);
    StartTinyWrite(cart, 0x1FF);
    Send(cart, {0xA1});
    Savestate saved(1024);
    cart.DoSavestate(&saved);
    const auto recordEnd = saved.Length();
    Check(saved.MinorVersion() == 10, "Tiny uncommitted page must require14.10");
    saved.Section("TAIL");
    u32 marker = 0x1234ABCD;
    saved.Var32(&marker);
    saved.Finish();
    Check(!saved.Error, "Generated mid-WRITE state could not be saved");
    if (saved.Error) return;
    Bytes bytes(static_cast<const u8*>(saved.Buffer()), static_cast<const u8*>(saved.Buffer()) + saved.Length());

    Send(cart, {0xEE, 0xDD}); // Disturb both the live position and memory.
    ReleaseAndComplete(cart);
    Savestate loaded(bytes.data(), u32(bytes.size()), false);
    cart.DoSavestate(&loaded);
    Check(!loaded.Error && loaded.Length() == recordEnd, "Mid-WRITE restore changed the record boundary");
    loaded.Section("TAIL");
    marker = 0;
    loaded.Var32(&marker);
    Check(!loaded.Error && marker == 0x1234ABCD, "Retail restore consumed the following section");
    CheckImage(f, expected); // The old array persists while the page is uncommitted.
    f.Sink.Notices = 0;
    Send(cart, {0xB2, 0xC3}); // Continue the same selected WRITE, without a new header.
    ReleaseAndComplete(cart);
    expected[0x1FF] = 0xA1;
    expected[0x1F0] = 0xB2;
    expected[0x1F1] = 0xC3;
    CheckImage(f, expected);
    Check(f.Sink.Notices == 1 && !(Status(cart) & 2), "Resumed page WRITE lost pending data or WEL completion");
}

static void RegularControl()
{
    Fixture f(2);
    Bytes expected = f.Sink.Persisted;
    Command(*f.Cart, 0x06);
    f.Cart->SPISelect();
    Send(*f.Cart, {0x02, 0, 0x0F, 0xA1, 0xB2});
    ReleaseAndComplete(*f.Cart);
    // An 8KiB device may be FRAM: do not apply the tiny EEPROM's page mask.
    expected[0x0F] = 0xA1;
    expected[0x10] = 0xB2;
    CheckImage(f, expected);
    f.Cart->SPISelect();
    Send(*f.Cart, {0x03, 0, 0x0F});
    Check(f.Cart->SPITransmitReceive(0) == 0xA1 && f.Cart->SPITransmitReceive(0) == 0xB2,
          "Regular EEPROM/FRAM sequential read changed");
    ReleaseAndComplete(*f.Cart);
}

// Micron M25PE40 Rev B, pp29/31/34/36: 256-byte page latch, last data wins,
// PROGRAM clears bits only, WRITE preserves untouched bytes, ERASE produces FF.
// https://www.farnell.com/datasheets/2215260.pdf
static void FlashStart(CartRetail& cart, u8 command, u32 address)
{
    cart.SPISelect();
    Send(cart, {command, u8(address >> 16), u8(address >> 8), u8(address)});
}

static void Flash(const std::string_view test)
{
    for (u32 type : {5u, 6u, 7u})
    {
        Fixture f(type, 17);
        auto& cart = *f.Cart;
        Bytes expected = f.Sink.Persisted;
        const u32 physical = expected.size() - 17;
        if (test == "flash-program")
        {
            FlashStart(cart, 0x02, 0x127);
            Send(cart, {0x3C, 0xFF}); ReleaseAndComplete(cart);
            CheckImage(f, expected);
            Check(f.Sink.Notices == 0, "PROGRAM without WREN changed the save");
            for (u8 byte : {0x3C, 0xF0, 0xFF})
            {
                Command(cart, 0x06);
                FlashStart(cart, 0x02, 0x127); Send(cart, {byte});
                CheckImage(f, expected); // The memory array changes at CS release.
                ReleaseAndComplete(cart); expected[0x127] &= byte;
                CheckImage(f, expected);
                Check(!(Status(cart) & 2), "PROGRAM did not consume WEL");
            }
            Command(cart, 0x06);
            FlashStart(cart, 0x0A, 0x127); Send(cart, {0xFF}); ReleaseAndComplete(cart);
            expected[0x127] = 0xFF; CheckImage(f, expected);
            const auto notices = f.Sink.Notices; ReleaseAndComplete(cart);
            Check(f.Sink.Notices == notices, "Duplicate CS release repeated a Flash operation");
        }
        else if (test == "flash-page")
        {
            for (u8 command : {0x02, 0x0A})
            {
                const u32 page = physical - 256;
                const auto before = expected;
                Command(cart, 0x06);
                // High unused address bits alias the same chip; never file padding.
                FlashStart(cart, command, 0xF00000 | (physical - 1));
                Send(cart, {0x55, 0xA6, 0x19});
                CheckImage(f, expected); ReleaseAndComplete(cart);
                expected[physical - 1] = command == 2 ? before[physical - 1] & 0x55 : 0x55;
                expected[page] = command == 2 ? before[page] & 0xA6 : 0xA6;
                expected[page + 1] = command == 2 ? before[page + 1] & 0x19 : 0x19;
                CheckImage(f, expected);
                // The first zero-filled lap must be discarded, including for PP.
                const auto prior = expected;
                Command(cart, 0x06); FlashStart(cart, command, page + 255);
                for (unsigned i = 0; i < 256; ++i) cart.SPITransmitReceive(0);
                for (unsigned i = 0; i < 256; ++i) cart.SPITransmitReceive(0xA5);
                cart.SPITransmitReceive(0x3C);
                CheckImage(f, expected); ReleaseAndComplete(cart);
                for (u32 i = page; i < physical; ++i)
                {
                    const u8 last = i == physical - 1 ? 0x3C : 0xA5;
                    expected[i] = command == 2 ? prior[i] & last : last;
                }
                CheckImage(f, expected);
                // READ and FAST READ cross page boundaries instead of latching.
                for (u8 read : {0x03, 0x0B})
                {
                    FlashStart(cart, read, physical - 1);
                    if (read == 0x0B) cart.SPITransmitReceive(0);
                    Check(cart.SPITransmitReceive(0) == expected[physical - 1] &&
                          cart.SPITransmitReceive(0) == expected[0], "Flash READ did not wrap at physical capacity");
                    ReleaseAndComplete(cart);
                }
            }
        }
        else if (test == "flash-erase")
        {
            for (u8 command : {0xDB, 0xD8})
            {
                const u32 start = command == 0xDB ? 0x12300 : 0x10000;
                const u32 count = command == 0xDB ? 256 : 65536;
                FlashStart(cart, command, 0x12345); ReleaseAndComplete(cart);
                CheckImage(f, expected);
                Command(cart, 0x06);
                cart.SPISelect(); Send(cart, {command, 1, 0x23}); ReleaseAndComplete(cart);
                CheckImage(f, expected);
                Check(Status(cart) & 2, "Truncated ERASE consumed WEL");
                FlashStart(cart, command, 0x12345); Send(cart, {0}); ReleaseAndComplete(cart);
                CheckImage(f, expected); // Extra data invalidates an address-only command.
                Check(Status(cart) & 2, "Overlong ERASE consumed WEL");
                FlashStart(cart, command, 0x12345);
                CheckImage(f, expected); ReleaseAndComplete(cart);
                std::fill_n(expected.begin() + start, count, 0xFF);
                CheckImage(f, expected);
                Check(!(Status(cart) & 2), "ERASE did not consume WEL");
            }
        }
        else if (test == "flash-state")
        {
            for (u8 command : {0x02, 0x0A, 0xDB, 0xD8})
            {
                Command(cart, 0x06); FlashStart(cart, command, 0x123FE);
                if (command == 2 || command == 10) Send(cart, {0xA6, 0x19, 0xC3});
                Savestate saved(physical + 1024); cart.DoSavestate(&saved);
                saved.Section("TAIL"); u32 marker = 0x1234ABCD; saved.Var32(&marker); saved.Finish();
                Check(!saved.Error && saved.MinorVersion() == 6, "Pending Flash latch must require 14.6");
                if (command == 2)
                {
                    // Reject a truncated latch or an illegal pending length before
                    // replacing the receiver's live save bytes and active command.
                    Bytes broken(static_cast<const u8*>(saved.Buffer()),
                                 static_cast<const u8*>(saved.Buffer()) + saved.Length());
                    const u32 sectionBytes = physical + 61 + 255;
                    const u32 total = 16 + sectionBytes;
                    broken.resize(total);
                    std::memcpy(broken.data() + 8, &total, 4);
                    std::memcpy(broken.data() + 20, &sectionBytes, 4);
                    const auto notices = f.Sink.Notices;
                    auto* pointer = cart.GetSaveMemory();
                    Savestate truncated(broken.data(), broken.size(), false);
                    cart.DoSavestate(&truncated);
                    Check(truncated.Error && cart.GetSaveMemory() == pointer && f.Sink.Notices == notices,
                          "Truncated pending Flash replaced live save or notified persistence");
                    CheckImage(f, expected);
                    broken.assign(static_cast<const u8*>(saved.Buffer()),
                                  static_cast<const u8*>(saved.Buffer()) + saved.Length());
                    const u32 invalidLength = 0x80000101;
                    std::memcpy(broken.data() + physical + 73, &invalidLength, 4);
                    Savestate invalid(broken.data(), broken.size(), false);
                    cart.DoSavestate(&invalid);
                    Check(invalid.Error && cart.GetSaveMemory() == pointer && f.Sink.Notices == notices,
                          "Invalid pending Flash length replaced live save or notified persistence");
                    CheckImage(f, expected);
                }
                Fixture restored(type, 17);
                Savestate load(saved.Buffer(), saved.Length(), false);
                restored.Cart->DoSavestate(&load);
                load.Section("TAIL"); marker = 0; load.Var32(&marker);
                Check(!load.Error && marker == 0x1234ABCD, "Pending Flash state lost following section");
                CheckImage(restored, expected);
                if (command == 2 || command == 10)
                {
                    Send(*restored.Cart, {0x55});
                    const u32 positions[] = {0x123FE, 0x123FF, 0x12300, 0x12301};
                    const u8 values[] = {0xA6, 0x19, 0xC3, 0x55};
                    for (unsigned i = 0; i < 4; ++i)
                        expected[positions[i]] = command == 2 ? expected[positions[i]] & values[i] : values[i];
                }
                else std::fill_n(expected.begin() + (command == 0xDB ? 0x12300 : 0x10000),
                                 command == 0xDB ? 256 : 65536, 0xFF);
                ReleaseAndComplete(*restored.Cart); CheckImage(restored, expected);
                // Continue with the restored owner; original pending bytes are abandoned by Reset.
                cart.Reset(); cart.SetSaveMemory(expected.data(), expected.size());
                Savestate idle(physical + 1024); cart.DoSavestate(&idle); idle.Finish();
                Check(!idle.Error && idle.MinorVersion() == 2, "Idle Flash changed the normal 14.2 writer format");
            }
        }
    }
}

static void RegularStart(CartRetail& cart, u32 addressBytes, u8 command, u32 address)
{
    cart.SPISelect();
    cart.SPITransmitReceive(command);
    for (u32 i = addressBytes; i > 0; --i) cart.SPITransmitReceive(u8(address >> ((i - 1) * 8)));
}

static void RegularRead(CartRetail& cart, u32 addressBytes, u32 address,
                        std::initializer_list<u8> expected)
{
    RegularStart(cart, addressBytes, 0x03, address);
    for (u8 byte : expected)
        Check(cart.SPITransmitReceive(0) == byte, "Profile READ did not advance linearly at a page/chip boundary");
    ReleaseAndComplete(cart);
}

static void ProfilePage()
{
    for (const auto& profile : EEPROMProfiles)
    {
        std::printf("EEPROM profile %u: page=%u, capacity=%u\n", profile.Type, profile.Page, profile.Capacity);
        Fixture f(profile.Type, 17);
        auto& cart = *f.Cart;
        Bytes expected = f.Sink.Persisted;
        RegularStart(cart, profile.AddressBytes, 0x02, profile.Page - 1);
        Send(cart, {0xA1, 0xB2});
        CheckImage(f, expected);
        ReleaseAndComplete(cart);
        CheckImage(f, expected);
        Check(f.Sink.Notices == 0 && !(Status(cart) & 2), "Profile WRITE without WREN mutated memory/persistence");

        for (u32 last : {0x1000 + profile.Page - 1, profile.Capacity - 1})
        {
            const auto notices = f.Sink.Notices;
            Command(cart, 0x06);
            Check(Status(cart) & 2, "EEPROM WREN did not set WEL");
            RegularStart(cart, profile.AddressBytes, 0x02, last);
            Send(cart, {0xA1, 0xB2, 0xC3});
            CheckImage(f, expected); // Neither memory nor the save sink commits while CS is low.
            Check(f.Sink.Notices == notices, "EEPROM profile notified before CS release");
            ReleaseAndComplete(cart);
            expected[last] = 0xA1;
            expected[last + 1 - profile.Page] = 0xB2;
            expected[last + 2 - profile.Page] = 0xC3;
            CheckImage(f, expected); // Includes the next page and opaque file padding.
            Check(f.Sink.Notices == notices + 1 && !(Status(cart) & 2), "EEPROM release did not commit once and clear WEL");
            ReleaseAndComplete(cart);
            Check(f.Sink.Notices == notices + 1, "EEPROM repeated CS release replayed a page");
        }

        Command(cart, 0x06);
        RegularStart(cart, profile.AddressBytes, 0x02, 0x800);
        for (u32 i = 0; i < profile.Page; ++i) cart.SPITransmitReceive(0);
        for (u32 i = 0; i < profile.Page; ++i) cart.SPITransmitReceive(0xA5);
        cart.SPITransmitReceive(0x3C);
        const auto notices = f.Sink.Notices;
        CheckImage(f, expected);
        ReleaseAndComplete(cart);
        std::fill_n(expected.begin() + 0x800, profile.Page, 0xA5);
        expected[0x800] = 0x3C; // Last received byte wins over both earlier laps.
        CheckImage(f, expected);
        Check(f.Sink.Notices == notices + 1, "Overflowed EEPROM page lost its save notification");
        const u32 edge = 0x1000 + profile.Page - 1;
        RegularRead(cart, profile.AddressBytes, edge, {expected[edge], expected[edge + 1]});
        RegularRead(cart, profile.AddressBytes, profile.Capacity - 1,
                    {expected[profile.Capacity - 1], expected[0]});

        // Codes 2/3/4 retain their historical immediate, linear writes.
        Fixture legacy(profile.Legacy, 17);
        auto linear = legacy.Sink.Persisted;
        Command(*legacy.Cart, 0x06);
        RegularStart(*legacy.Cart, profile.AddressBytes, 0x02, profile.Page - 1);
        Send(*legacy.Cart, {0x19, 0xE7});
        linear[profile.Page - 1] = 0x19; linear[profile.Page] = 0xE7;
        CheckMemory(legacy, linear, "Exact EEPROM profile changed legacy 2/3/4 write semantics");
        ReleaseAndComplete(*legacy.Cart);
        CheckImage(legacy, linear);
    }
}

static void ProfileFRAM()
{
    // FM25W256 has no EEPROM page latch; writes cross 32-byte boundaries and
    // wrap at its 32KiB physical array, while file padding remains attached.
    Fixture f(14, 17);
    auto& cart = *f.Cart;
    Bytes expected = f.Sink.Persisted;
    RegularStart(cart, 2, 0x02, 0x1F); Send(cart, {0xA1, 0xB2});
    CheckImage(f, expected); ReleaseAndComplete(cart); CheckImage(f, expected);
    Check(f.Sink.Notices == 0 && !(Status(cart) & 2), "FRAM WRITE without WREN was accepted");
    for (u32 last : {0x1Fu, 0x7FFFu})
    {
        const auto before = expected;
        const auto notices = f.Sink.Notices;
        Command(cart, 0x06);
        Check(Status(cart) & 2, "FRAM WREN did not set WEL");
        RegularStart(cart, 2, 0x02, last);
        cart.SPITransmitReceive(0xA1); expected[last] = 0xA1;
        CheckMemory(f, expected, "FRAM first byte was not visible before CS release");
        cart.SPITransmitReceive(0xB2); expected[last == 0x7FFF ? 0 : 0x20] = 0xB2;
        CheckMemory(f, expected, "FRAM second byte wrapped at an EEPROM page or entered padding");
        Check(f.Sink.Notices == notices && f.Sink.Persisted == before, "FRAM host save committed before CS release");
        ReleaseAndComplete(cart); CheckImage(f, expected);
        Check(f.Sink.Notices == notices + 1 && !(Status(cart) & 2), "FRAM release did not publish its bytes and clear WEL");
        ReleaseAndComplete(cart);
        Check(f.Sink.Notices == notices + 1, "FRAM release duplicated persistence");
        RegularRead(cart, 2, last, {0xA1, 0xB2});
    }
    Command(cart, 0x06); Command(cart, 0x04);
    RegularStart(cart, 2, 0x02, 0x1F); Send(cart, {0}); ReleaseAndComplete(cart);
    CheckImage(f, expected);
    Check(!(Status(cart) & 2) && f.Sink.Notices == 2, "FRAM WRDI did not protect later writes");
}

struct RetailState
{
    Bytes Data;
    u32 LengthOffset, SaveLenOffset, ProfileOffset;
    bool Valid;
};

static RetailState SaveRetail(Fixture& f, u32 physical, u32 profile, bool pending)
{
    // Obtain the unchanged CartCommon boundary through its writer. Offsets
    // below describe the specified retail wire format, not live latch fields.
    Savestate common(128); f.Cart->CartCommon::DoSavestate(&common);
    const u32 lengthOffset = common.Length();
    const u32 saveLenOffset = lengthOffset + 4 + physical + 4 + 1 + 4 + 1 + 4;
    const u32 profileOffset = saveLenOffset + 4;
    const u32 expectedEnd = profileOffset + (profile ? 4 : 0) + (pending ? 256 : 0);
    Savestate saved(physical + 1024); f.Cart->DoSavestate(&saved);
    bool valid = !saved.Error && saved.Length() == expectedEnd;
    if (valid)
    {
        u32 capacity = 0, flags = 0, storedProfile = 0;
        const auto* bytes = static_cast<const u8*>(saved.Buffer());
        std::memcpy(&capacity, bytes + lengthOffset, 4);
        std::memcpy(&flags, bytes + saveLenOffset, 4);
        if (profile) std::memcpy(&storedProfile, bytes + profileOffset, 4);
        valid = capacity == physical && storedProfile == profile &&
                (flags & 0xC0000000u) == ((profile ? 0x40000000u : 0) | (pending ? 0x80000000u : 0));
    }
    Check(valid, "Retail state lost physical length, exact profile flag/code, or conditional page buffer");
    saved.Section("TAIL"); u32 marker = 0x1234ABCD; saved.Var32(&marker); saved.Finish();
    const u16 minor = profile ? 7 : pending ? 6 : 2;
    const bool versionOK = !saved.Error && saved.MajorVersion() == 14 && saved.MinorVersion() == minor;
    Check(versionOK, "Exact profiles require 14.7; legacy idle/Flash pending remain 14.2/14.6");
    return {Bytes(static_cast<const u8*>(saved.Buffer()), static_cast<const u8*>(saved.Buffer()) + saved.Length()),
            lengthOffset, saveLenOffset, profileOffset, valid && versionOK};
}

static bool LoadRetail(Fixture& f, RetailState& saved)
{
    Savestate load(saved.Data.data(), saved.Data.size(), false);
    f.Cart->DoSavestate(&load);
    if (load.Error) { Check(false, "Valid retail profile state was rejected"); return false; }
    load.Section("TAIL"); u32 marker = 0; load.Var32(&marker);
    Check(!load.Error && marker == 0x1234ABCD, "Retail profile consumed the following section");
    return !load.Error && marker == 0x1234ABCD;
}

static void ProfileState()
{
    for (const auto& profile : EEPROMProfiles)
    {
        Fixture source(profile.Type, 17);
        SaveRetail(source, profile.Capacity, profile.Type, false); // Exact even when idle.
        Command(*source.Cart, 0x06);
        RegularStart(*source.Cart, profile.AddressBytes, 0x02, profile.Capacity - 1);
        Send(*source.Cart, {0xA1});
        CheckImage(source, source.Sink.Persisted);
        auto saved = SaveRetail(source, profile.Capacity, profile.Type, true);
        // A failing baseline layout must not become an out-of-bounds test read.
        if (!saved.Valid) continue;
        Fixture receiver(profile.Legacy, 17, 0xC3);
        Bytes expected = receiver.Sink.Persisted;
        std::copy_n(source.Cart->GetSaveMemory(), profile.Capacity, expected.begin());
        if (!LoadRetail(receiver, saved)) continue;
        CheckImage(receiver, expected); // Receiver's opaque tail, source's physical array.
        receiver.Sink.Notices = 0;
        Send(*receiver.Cart, {0xB2, 0xC3});
        CheckImage(receiver, expected);
        ReleaseAndComplete(*receiver.Cart);
        expected[profile.Capacity - 1] = 0xA1;
        expected[profile.Capacity - profile.Page] = 0xB2;
        expected[profile.Capacity - profile.Page + 1] = 0xC3;
        CheckImage(receiver, expected);
        Check(receiver.Sink.Notices == 1 && !(Status(*receiver.Cart) & 2), "Restored EEPROM page lost pending data/WEL");
        SaveRetail(receiver, profile.Capacity, profile.Type, false);
    }
    {
        // Reverse direction at the same capacity: a current 14.2 legacy-layout
        // state retains already-written bytes and restores linear semantics.
        Fixture source(2, 17);
        Command(*source.Cart, 0x06); RegularStart(*source.Cart, 2, 0x02, 0x1F); Send(*source.Cart, {0xA1});
        auto saved = SaveRetail(source, 8192, 0, false);
        Fixture receiver(11, 17, 0xC3);
        Bytes expected = receiver.Sink.Persisted;
        std::copy_n(source.Cart->GetSaveMemory(), 8192, expected.begin());
        if (saved.Valid && LoadRetail(receiver, saved))
        {
            CheckImage(receiver, expected);
            Send(*receiver.Cart, {0xB2, 0xC3}); expected[0x20] = 0xB2; expected[0x21] = 0xC3;
            CheckMemory(receiver, expected, "Legacy 14.2 loaded into exact EEPROM retained page-latched semantics");
            ReleaseAndComplete(*receiver.Cart); CheckImage(receiver, expected);
            SaveRetail(receiver, 8192, 0, false);
        }
    }
    {
        Fixture source(14, 17);
        SaveRetail(source, 32768, 14, false);
        Command(*source.Cart, 0x06); RegularStart(*source.Cart, 2, 0x02, 0x7FFF); Send(*source.Cart, {0xA1});
        auto saved = SaveRetail(source, 32768, 14, false); // FRAM has no pending page buffer.
        Fixture receiver(14, 17, 0xC3);
        Bytes expected = receiver.Sink.Persisted;
        std::copy_n(source.Cart->GetSaveMemory(), 32768, expected.begin());
        if (saved.Valid && LoadRetail(receiver, saved))
        {
            CheckImage(receiver, expected);
            Send(*receiver.Cart, {0xB2}); expected[0] = 0xB2;
            CheckMemory(receiver, expected, "Restored FRAM lost immediate write or physical address wrap");
            ReleaseAndComplete(*receiver.Cart); CheckImage(receiver, expected);
            Check(!(Status(*receiver.Cart) & 2), "Restored FRAM failed to consume WEL");
        }
    }
    {
        Fixture flash(6, 17);
        SaveRetail(flash, 524288, 0, false);
        Command(*flash.Cart, 0x06); FlashStart(*flash.Cart, 0x0A, 0x1FF); Send(*flash.Cart, {0xA1});
        SaveRetail(flash, 524288, 0, true);
        CheckImage(flash, flash.Sink.Persisted);
    }

    Fixture incoming(11, 17, 0x3C);
    Command(*incoming.Cart, 0x06); RegularStart(*incoming.Cart, 2, 0x02, 0x40); Send(*incoming.Cart, {0xD4});
    auto valid = SaveRetail(incoming, 8192, 11, true);
    if (!valid.Valid) return;
    for (std::string_view defect : {"profile", "capacity-profile", "length", "pending-length", "profile-tail", "page-tail"})
    {
        std::printf("Reject profile state: %.*s\n", int(defect.size()), defect.data());
        Bytes broken = valid.Data;
        const auto put32 = [&](u32 offset, u32 value) { std::memcpy(broken.data() + offset, &value, 4); };
        if (defect == "profile") put32(valid.ProfileOffset, 10);
        else if (defect == "capacity-profile") put32(valid.ProfileOffset, 12); // 64KiB profile over an 8KiB record.
        else if (defect == "length") put32(valid.LengthOffset, 8193);
        else if (defect == "pending-length") put32(valid.SaveLenOffset, 0xC0000000u); // A flagged latch with no bytes.
        else
        {
            const u32 end = valid.ProfileOffset + (defect == "profile-tail" ? 3 : 4 + 255);
            broken.resize(end);
            put32(8, end); put32(20, end - 16); // Valid outer spans; truncated retail extension.
        }
        Fixture receiver(11, 17);
        Bytes expected = receiver.Sink.Persisted;
        Command(*receiver.Cart, 0x06); RegularStart(*receiver.Cart, 2, 0x02, 0x5F); Send(*receiver.Cart, {0xA6});
        auto* memory = receiver.Cart->GetSaveMemory();
        const auto notices = receiver.Sink.Notices;
        Savestate load(broken.data(), broken.size(), false); receiver.Cart->DoSavestate(&load);
        // This checks retail SRAM/protocol/latch staging, not whole-cart atomicity:
        // CartCommon's unrelated ROM-bus fields are outside this test's claim.
        Check(load.Error && receiver.Cart->GetSaveMemory() == memory && receiver.Sink.Notices == notices,
              "Rejected profile state replaced base SRAM or invoked a save callback");
        CheckImage(receiver, expected);
        Send(*receiver.Cart, {0xB2});
        CheckImage(receiver, expected);
        ReleaseAndComplete(*receiver.Cart); expected[0x5F] = 0xA6; expected[0x40] = 0xB2;
        CheckImage(receiver, expected);
        Check(receiver.Sink.Notices == notices + 1 && !(Status(*receiver.Cart) & 2),
              "Rejected profile state disturbed the receiver's pending page/protocol");
    }
}

// ST M95640 DS6633 sections 5.5/6.4 and FM25W256 Rev H pp7-8:
// WRSR needs WREN; BP protects the upper quarter/half/all of the array.
static void WriteStatus(CartRetail& cart, u8 value, bool enable = true)
{
    if (enable) Command(cart, 0x06);
    cart.SPISelect(); Send(cart, {0x01, value}); ReleaseAndComplete(cart);
}

static void StatusRegister()
{
    for (u32 type : {1u, 2u, 11u, 14u})
    {
        Fixture f(type, 17);
        auto& cart = *f.Cart;
        const auto before = f.Sink.Persisted;
        WriteStatus(cart, 0x0C, false);
        Check(!(Status(cart) & 0x0E), "WRSR without WREN changed protection or WEL");
        WriteStatus(cart, 0xFF);
        const u8 writable = type == 1 ? 0x0C : 0x8C;
        Check((Status(cart) & (type == 1 ? 0x0F : 0xFF)) == writable,
              "WRSR lost writable protection bits or wrote reserved/WEL/WIP bits");
        cart.Reset();
        Check((Status(cart) & writable) == writable && !(Status(cart) & 2),
              "Reset erased nonvolatile protection or retained volatile WEL");
        WriteStatus(cart, 0);
        Check(!(Status(cart) & 0x0E), "WREN did not permit protection to be cleared");
        CheckImage(f, before);
        Check(f.Sink.Notices == 0, "Status-only commands notified save-array changes");
    }
    // EEPROM status writes need exactly one data byte and a CS rising edge.
    Fixture f(11);
    Command(*f.Cart, 0x06);
    f.Cart->SPISelect(); Send(*f.Cart, {0x01, 0x0C});
    // Starting another selection abandons a held transaction without release.
    Check((Status(*f.Cart) & 0x0E) == 2, "WRSR committed before CS release");
    f.Cart->SPISelect(); Send(*f.Cart, {0x01, 0x0C, 0x00}); ReleaseAndComplete(*f.Cart);
    Check((Status(*f.Cart) & 0x0E) == 2, "Overlong EEPROM WRSR changed status or consumed WEL");
    Command(*f.Cart, 0x01);
    Check((Status(*f.Cart) & 0x0E) == 2, "Truncated WRSR consumed WEL");
}

static void WriteProtection()
{
    struct Medium { u32 Type, Capacity, AddressBytes; };
    for (const auto& medium : {Medium{1, 512, 1}, {2, 8192, 2}, {11, 8192, 2},
                               {12, 65536, 2}, {13, 131072, 3}, {14, 32768, 2}})
    for (const auto& protection : {std::pair<u8, u32>{0, medium.Capacity},
                                   {4, medium.Capacity * 3 / 4}, {8, medium.Capacity / 2}, {12, 0}})
    {
        Fixture f(medium.Type, 17);
        auto expected = f.Sink.Persisted;
        auto& cart = *f.Cart;
        WriteStatus(cart, protection.first);
        for (u32 address : {protection.second ? protection.second - 1 : 0,
                            protection.second < medium.Capacity ? protection.second : 0})
        {
            Command(cart, 0x06);
            if (medium.Type == 1) StartTinyWrite(cart, u16(address));
            else RegularStart(cart, medium.AddressBytes, 0x02, address);
            cart.SPITransmitReceive(0xA6); ReleaseAndComplete(cart);
            if (address < protection.second) expected[address] = 0xA6;
            CheckImage(f, expected);
        }
        // Read access remains available even when the whole array is protected.
        if (medium.Type == 1) CheckRead(cart, u16(medium.Capacity - 1), {expected[medium.Capacity - 1]});
        else RegularRead(cart, medium.AddressBytes, medium.Capacity - 1, {expected[medium.Capacity - 1]});
    }
    // FRAM stops its address counter at the protected boundary. Even a burst
    // long enough to wrap the chip must never resume writes at address zero.
    Fixture fram(14, 17);
    auto expected = fram.Sink.Persisted;
    WriteStatus(*fram.Cart, 4);
    Command(*fram.Cart, 0x06); RegularStart(*fram.Cart, 2, 0x02, 0x5FFF);
    Send(*fram.Cart, {0xA6}); expected[0x5FFF] = 0xA6;
    CheckMemory(fram, expected, "Unprotected FRAM byte was not written immediately");
    for (unsigned i = 0; i <= 32768; ++i) fram.Cart->SPITransmitReceive(0xC3);
    CheckMemory(fram, expected, "FRAM continued or wrapped a burst after entering protection");
    ReleaseAndComplete(*fram.Cart); CheckImage(fram, expected);
    Check(!(Status(*fram.Cart) & 2), "FRAM burst completion failed to clear WEL");
}

static void StatusState()
{
    for (u32 type : {1u, 11u, 14u, 6u})
    for (u8 command : {0x06, 0x04, 0x01})
    {
        if (type == 6 && command == 0x01) continue; // Not a generic EEPROM status register.
        Fixture source(type, 17), receiver(type, 17);
        auto& cart = *source.Cart;
        if (command != 0x06) Command(cart, 0x06);
        cart.SPISelect(); Send(cart, {command});
        if (command == 0x01) Send(cart, {0x0C});
        Savestate saved; cart.DoSavestate(&saved); saved.Finish();
        Check(!saved.Error && saved.MinorVersion() == 8, "Pending status control must require 14.8");
        Savestate load(saved.Buffer(), saved.Length(), false); receiver.Cart->DoSavestate(&load);
        Check(!load.Error, "Pending status command could not be restored");
        ReleaseAndComplete(*receiver.Cart);
        const u8 expected = command == 0x06 ? 2 : command == 0x01 ? 0x0C : 0;
        Check((Status(*receiver.Cart) & 0x0E) == expected, "Restored status control lost its CS completion");
        Savestate idle; receiver.Cart->DoSavestate(&idle); idle.Finish();
        Check(!idle.Error && idle.MinorVersion() == (type >= 11 ? 7 : 2),
              "Completed status command unnecessarily raised the idle state version");
    }
    Fixture source(11, 17);
    Command(*source.Cart, 0x06); source.Cart->SPISelect(); Send(*source.Cart, {0x01, 0x0C});
    Savestate common; source.Cart->CartCommon::DoSavestate(&common);
    const u32 addressOffset = common.Length() + 4 + 8192 + 4 + 1;
    const u32 saveLenOffset = addressOffset + 4 + 1 + 4;
    Savestate saved; source.Cart->DoSavestate(&saved); saved.Finish();
    for (auto defect : {std::pair<u32, u32>{addressOffset, 0x100},
                        {saveLenOffset, 0x40000001}, {saveLenOffset, 0xC0000000}})
    {
        Bytes broken(static_cast<const u8*>(saved.Buffer()), static_cast<const u8*>(saved.Buffer()) + saved.Length());
        std::memcpy(broken.data() + defect.first, &defect.second, 4);
        Fixture receiver(11, 17);
        Command(*receiver.Cart, 0x06); receiver.Cart->SPISelect(); Send(*receiver.Cart, {0x01, 4});
        auto* memory = receiver.Cart->GetSaveMemory();
        const auto before = receiver.Sink.Persisted;
        Savestate load(broken.data(), broken.size(), false); receiver.Cart->DoSavestate(&load);
        Check(load.Error && receiver.Cart->GetSaveMemory() == memory && receiver.Sink.Notices == 0,
              "Malformed status command replaced retail SRAM or notified persistence");
        ReleaseAndComplete(*receiver.Cart);
        Check((Status(*receiver.Cart) & 0x0E) == 4, "Malformed status state disturbed the live command");
        CheckImage(receiver, before);
    }
}

static u8 BusyStatus(CartRetail& cart)
{
    cart.SPISelect(); Send(cart, {0x05});
    const u8 status = cart.SPITransmitReceive(0);
    cart.SPIRelease();
    return status;
}

static void InternalWrite(bool state)
{
    for (u32 type : {1u, 11u, 12u, 13u, 6u, 14u})
    {
        std::printf("Internal write type=%u state=%d\n", type, state);
        Fixture f(type, 17);
        auto& cart = *f.Cart;
        Bytes expected = f.Sink.Persisted;
        const u32 addressBytes = type == 1 ? 1 : type == 13 || type == 6 ? 3 : 2;
        const u32 page = type == 1 ? 16 : type == 11 ? 32 : type == 12 ? 128 : 256;
        const u32 first = type == 14 ? 31 : page - 1;
        Command(cart, 0x06);
        if (type == 1) StartTinyWrite(cart, first);
        else RegularStart(cart, addressBytes, type == 6 ? 0x0A : 0x02, first);
        Send(cart, {0xA1, 0xB2});
        if (type != 14) CheckImage(f, expected);
        cart.SPIRelease();
        if (type == 14)
        {
            expected[first] = 0xA1; expected[first + 1] = 0xB2;
            Check(ChipDelay(cart) == 0 && !(BusyStatus(cart) & 1), "FRAM acquired EEPROM write latency");
            CheckImage(f, expected);
            continue;
        }
        Check(ChipDelay(cart) > 0 && (BusyStatus(cart) & 3) == 3, "CS failed to begin WIP with WEL held");
        CheckImage(f, expected);
        Check(f.Sink.Notices == 0, "Internal write notified persistence before completion");
        for (u8 command : {u8(0x04), u8(0x06)})
        {
            cart.SPISelect(); Send(cart, {command}); cart.SPIRelease();
        }
        Check((BusyStatus(cart) & 3) == 3, "Busy WRDI/WREN disturbed the pending operation");
        cart.SPISelect(); Send(cart, {0x03, 0, 0, 0});
        Check(cart.SPITransmitReceive(0) == 0xFF, "Busy READ was accepted");
        cart.SPIRelease();
        if (state)
        {
            Savestate saved; cart.DoSavestate(&saved); saved.Finish();
            Check(!saved.Error && saved.MinorVersion() == 10, "Internal write state did not require14.10");
            Fixture receiver(type, 17, 0xC3);
            Bytes restoredExpected = receiver.Sink.Persisted;
            const u32 capacity = cart.GetSaveMemoryLength() - 17;
            std::copy_n(expected.begin(), capacity, restoredExpected.begin());
            Savestate load(saved.Buffer(), saved.Length(), false); receiver.Cart->DoSavestate(&load);
            Check(!load.Error && ChipDelay(*receiver.Cart) == ChipDelay(cart), "Cold chip lost internal operation");
            CheckImage(receiver, restoredExpected);
            const auto notices = receiver.Sink.Notices;
            CompleteChip(*receiver.Cart);
            restoredExpected[first] = 0xA1; restoredExpected[0] = 0xB2;
            CheckImage(receiver, restoredExpected);
            Check(receiver.Sink.Notices == notices + 1, "Restored operation failed to notify once at completion");
            CompleteChip(*receiver.Cart);
            Check(receiver.Sink.Notices == notices + 1, "Restored operation was completed twice");
        }
        // A READ rejected at its opcode must remain rejected when WIP clears
        // during this same CS; the next command can observe the new array.
        cart.SPISelect(); Send(cart, {0x03});
        CompleteChip(cart);
        Check(cart.SPITransmitReceive(0) == 0xFF, "Completion revived a command rejected while busy");
        cart.SPIRelease();
        expected[first] = 0xA1; expected[0] = 0xB2;
        CheckImage(f, expected);
        Check(!ChipDelay(cart) && !(BusyStatus(cart) & 3) && f.Sink.Notices == 1,
              "Completion failed to clear WIP/WEL and publish exactly once");
        CompleteChip(cart);
        Check(f.Sink.Notices == 1, "Repeated completion replayed the save notification");
    }
    if (state)
    {
        for (u8 command : {u8(0xDB), u8(0xD8)})
        {
            Fixture source(6, 17); Command(*source.Cart, 0x06);
            FlashStart(*source.Cart, command, 0x12345); source.Cart->SPIRelease();
            Savestate saved; source.Cart->DoSavestate(&saved); saved.Finish();
            Check(!saved.Error && saved.MinorVersion() == 10, "Unaligned-address erase could not save its internal latch");
            Fixture receiver(6, 17, 0xC3);
            Bytes expected = receiver.Sink.Persisted;
            std::copy_n(source.Cart->GetSaveMemory(), 524288, expected.begin());
            Savestate load(saved.Buffer(), saved.Length(), false); receiver.Cart->DoSavestate(&load);
            Check(!load.Error && ChipDelay(*receiver.Cart), "Erase internal latch failed cold load");
            CheckImage(receiver, expected);
            CompleteChip(*receiver.Cart);
            const u32 length = command == 0xDB ? 256 : 65536;
            const u32 base = 0x12345 & ~(length - 1);
            std::fill_n(expected.begin() + base, length, 0xFF);
            CheckImage(receiver, expected);
        }
    }
    for (u32 type : {1u, 11u, 12u, 13u})
    {
        Fixture f(type); auto& cart = *f.Cart;
        Command(cart, 0x06); cart.SPISelect(); Send(cart, {0x01, 0x0C}); cart.SPIRelease();
        Check((BusyStatus(cart) & 0x0F) == 3, "WRSR changed protection bits before its write cycle ended");
        CompleteChip(cart);
        Check((BusyStatus(cart) & 0x0F) == 0x0C && f.Sink.Notices == 0,
              "WRSR completion lost protection or notified array persistence");
    }
}

// Explicit M25PE T9HX profiles. BP protects whole upper64KiB sectors;
// sector locks are a separate volatile mechanism, never EEPROM BP geometry.
static u8 FlashLock(CartRetail& cart, u32 address)
{
    FlashStart(cart, 0xE8, address);
    const u8 value = cart.SPITransmitReceive(0);
    Check(cart.SPITransmitReceive(0) == value, "RDLR advanced to a different sector during one CS");
    cart.SPIRelease();
    return value;
}

static void SetFlashLock(CartRetail& cart, u32 address, u8 value, bool enable = true)
{
    if (enable) Command(cart, 0x06);
    FlashStart(cart, 0xE5, address); Send(cart, {value}); cart.SPIRelease();
}

static bool BeginFlashProfile(Fixture& f)
{
    Command(*f.Cart, 0x06);
    const bool ready = (BusyStatus(*f.Cart) & 3) == 2;
    Check(ready, "Explicit Flash profile did not decode its WREN/RDSR protocol");
    return ready;
}

static void FlashProtection(const std::string_view mode)
{
    for (u32 type : {15u, 16u, 17u})
    {
        Fixture f(type, 17);
        if (!BeginFlashProfile(f)) continue;
        auto& cart = *f.Cart;
        const u32 capacity = 262144u << (type - 15);
        cart.SPISelect(); Send(cart, {0x9F});
        Check(cart.SPITransmitReceive(0) == 0x20 && cart.SPITransmitReceive(0) == 0x80 &&
              cart.SPITransmitReceive(0) == u8(0x12 + type - 15), "Explicit Flash profile returned the wrong JEDEC ID");
        cart.SPIRelease();
        Bytes expected = f.Sink.Persisted;
        const u8 mask = type == 15 ? 0x8C : 0x9C;
        if (mode == "flash-protection")
        {
            Command(cart, 0x04);
            WriteStatus(cart, 0xFF, false);
            Check((BusyStatus(cart) & mask) == 0 && !ChipDelay(cart), "Flash WRSR bypassed WREN");
            Command(cart, 0x06); cart.SPISelect(); Send(cart, {0x01, 0xFF});
            Check(!ChipDelay(cart), "Flash WRSR started before CS");
            cart.SPIRelease();
            Check(ChipDelay(cart) == 100542 && (BusyStatus(cart) & (mask | 3)) == 3,
                  "Flash WRSR did not retain old status for its3ms cycle");
            CompleteChip(cart);
            Check((BusyStatus(cart) & (mask | 3)) == mask && f.Sink.Notices == 0,
                  "Flash WRSR mask/completion changed array persistence");
            cart.Reset();
            Check((BusyStatus(cart) & mask) == mask, "Chip reset lost nonvolatile Flash BP/SRWD");
            const unsigned bpCount = type == 15 ? 4 : 8;
            for (unsigned bp = 0; bp < bpCount; ++bp)
            {
                WriteStatus(cart, u8(bp << 2));
                const u32 protectedBytes = bp ? std::min(capacity, 65536u << (bp - 1)) : 0;
                const u32 boundary = capacity - protectedBytes;
                for (u32 address : {boundary ? boundary - 1 : 0u, boundary < capacity ? boundary : capacity - 1})
                for (u8 command : {u8(0x02), u8(0x0A), u8(0xDB), u8(0xD8)})
                {
                    Command(cart, 0x06); FlashStart(cart, command, address | 0x800000u);
                    if (command == 0x02 || command == 0x0A) Send(cart, {0x36});
                    const auto notices = f.Sink.Notices;
                    cart.SPIRelease();
                    const bool protectedAddress = address >= boundary;
                    Check(bool(ChipDelay(cart)) == !protectedAddress, "Flash BP boundary/alias accepted or rejected the wrong write");
                    CheckImage(f, expected);
                    CompleteChip(cart);
                    if (!protectedAddress)
                    {
                        if (command == 0x02) expected[address] &= 0x36;
                        else if (command == 0x0A) expected[address] = 0x36;
                        else
                        {
                            const u32 length = command == 0xDB ? 256 : 65536;
                            std::fill_n(expected.begin() + (address & ~(length - 1)), length, 0xFF);
                        }
                    }
                    CheckImage(f, expected);
                    Check(f.Sink.Notices == notices + !protectedAddress, "Protected Flash operation notified persistence");
                }
            }
            WriteStatus(cart, 0);
            Check((BusyStatus(cart) & mask) == 0, "SRWD alone prevented software unprotect without a low W# pin");
        }
        else if (mode == "flash-lock")
        {
            const u32 target = capacity - 65536;
            Command(cart, 0x04); SetFlashLock(cart, target, 1, false);
            Check(FlashLock(cart, target) == 0, "WRLR bypassed WREN");
            Command(cart, 0x06); FlashStart(cart, 0xE5, target); cart.SPIRelease();
            Check(FlashLock(cart, target) == 0, "Truncated WRLR changed sector lock");
            Command(cart, 0x06); FlashStart(cart, 0xE5, target); Send(cart, {1, 0}); cart.SPIRelease();
            Check(FlashLock(cart, target) == 0, "Overlong WRLR changed sector lock");
            SetFlashLock(cart, target + 123, 1);
            Check(!ChipDelay(cart) && !(BusyStatus(cart) & 3) && FlashLock(cart, target | 0x800000u) == 1 &&
                  FlashLock(cart, target - 1) == 0, "WRLR timing/WEL/sector alias or neighboring lock is wrong");
            for (u8 command : {u8(0x02), u8(0x0A), u8(0xDB), u8(0xD8)})
            {
                Command(cart, 0x06); FlashStart(cart, command, target);
                if (command == 0x02 || command == 0x0A) Send(cart, {0});
                cart.SPIRelease();
                Check(!ChipDelay(cart), "Locked sector began a write cycle");
                CheckImage(f, expected);
            }
            SetFlashLock(cart, target, 3); SetFlashLock(cart, target, 0);
            Check(FlashLock(cart, target) == 3, "WRLR bypassed lock-down");
            cart.SetSaveMemory(expected.data(), expected.size());
            Check(FlashLock(cart, target) == 3, "Save import cleared live sector lock-down");
            cart.Reset();
            Check(FlashLock(cart, target) == 0, "Chip reset did not clear volatile sector locks");
            SetFlashLock(cart, target, 2); SetFlashLock(cart, target, 1);
            Check(FlashLock(cart, target) == 2, "Lock-down with an unlocked sector was reversible");
            Command(cart, 0x06); FlashStart(cart, 0x0A, target); Send(cart, {0xA6}); cart.SPIRelease();
            Check(ChipDelay(cart) && FlashLock(cart, target) == 0xFF, "RDLR was accepted during internal write");
            CompleteChip(cart); expected[target] = 0xA6; CheckImage(f, expected);
            Check(FlashLock(cart, target) == 2, "Busy RDLR disturbed the lock register");
        }
        else
        {
            SetFlashLock(cart, 65536, 3);
            Command(cart, 0x06); cart.SPISelect(); Send(cart, {0x01, 0x04}); cart.SPIRelease();
            Savestate saved; cart.DoSavestate(&saved); saved.Finish();
            Check(!saved.Error && saved.MinorVersion() == 11, "Explicit Flash state did not require14.11");
            Fixture receiver(type, 17, 0xC3);
            Bytes received = receiver.Sink.Persisted;
            std::copy_n(expected.begin(), capacity, received.begin());
            Savestate load(saved.Buffer(), saved.Length(), false); receiver.Cart->DoSavestate(&load);
            Check(!load.Error && ChipDelay(*receiver.Cart) == 100542, "Cold restore lost pending Flash status cycle");
            CompleteChip(*receiver.Cart);
            Check((BusyStatus(*receiver.Cart) & mask) == 4 && FlashLock(*receiver.Cart, 65536) == 3,
                  "Cold restore lost Flash BP/lock state");
            CheckImage(receiver, received);
            WriteStatus(*receiver.Cart, 0);
            Command(*receiver.Cart, 0x06); FlashStart(*receiver.Cart, 0xE5, 0); Send(*receiver.Cart, {1});
            Savestate held; receiver.Cart->DoSavestate(&held); held.Finish();
            Fixture next(type, 17, 0xC3);
            Savestate resume(held.Buffer(), held.Length(), false); next.Cart->DoSavestate(&resume);
            Check(!resume.Error, "Held WRLR failed cold restore");
            next.Cart->SPIRelease();
            Check(FlashLock(*next.Cart, 0) == 1, "Restored WRLR lost its CS completion");
            const auto* bytes = static_cast<const u8*>(held.Buffer());
            Bytes truncated(bytes, bytes + held.Length() - 1);
            auto* memory = next.Cart->GetSaveMemory();
            Savestate broken(truncated.data(), truncated.size(), false); next.Cart->DoSavestate(&broken);
            Check(broken.Error && next.Cart->GetSaveMemory() == memory && FlashLock(*next.Cart, 0) == 1,
                  "Truncated Flash lock state replaced live array/locks");
            Fixture legacy(type - 10, 17);
            Savestate old; legacy.Cart->DoSavestate(&old); old.Finish();
            Savestate oldLoad(old.Buffer(), old.Length(), false); next.Cart->DoSavestate(&oldLoad);
            Check(!oldLoad.Error && FlashLock(*next.Cart, 0) == 0xFF,
                  "Loading generic Flash state retained the explicit profile's locks");
        }
    }
}

static void FlashExtendedErase(bool state)
{
    for (u32 type : {15u, 16u, 17u})
    for (u8 command : {0x20, 0xC7})
    {
        Fixture f(type, 17);
        auto& cart = *f.Cart;
        Bytes expected = f.Sink.Persisted;
        const u32 capacity = expected.size() - 17;
        const u32 start = command == 0x20 ? capacity - 4096 : 0;
        const u32 count = command == 0x20 ? 4096 : capacity;
        const auto begin = [&] {
            if (command == 0x20) FlashStart(cart, command, (capacity - 1) | 0x800000);
            else { cart.SPISelect(); Send(cart, {command}); }
        };
        if (!state)
        {
            begin(); cart.SPIRelease();
            Check(!ChipDelay(cart), "Extended erase bypassed WREN");
            CheckImage(f, expected);
            Command(cart, 0x06);
            begin(); Send(cart, {0}); cart.SPIRelease();
            Check(!ChipDelay(cart) && (BusyStatus(cart) & 2), "Overlong extended erase started or consumed WEL");
            CheckImage(f, expected);
            if (command == 0x20)
            {
                cart.SPISelect(); Send(cart, {0x20, 1, 2}); cart.SPIRelease();
                Check(!ChipDelay(cart) && (BusyStatus(cart) & 2), "Truncated SSE changed WEL or started erase");
            }
            WriteStatus(cart, 4); // BP protects the highest64KiB sector.
            Command(cart, 0x06); begin(); cart.SPIRelease();
            Check(!ChipDelay(cart) && (BusyStatus(cart) & 2), "BP failed to reject the whole extended erase");
            CheckImage(f, expected);
            WriteStatus(cart, 0);
            for (u32 sector : {0u, capacity / 2, capacity - 65536})
            {
                SetFlashLock(cart, sector, 1);
                Command(cart, 0x06);
                if (command == 0x20) FlashStart(cart, command, sector + 0x123);
                else begin();
                cart.SPIRelease();
                Check(!ChipDelay(cart) && (BusyStatus(cart) & 2), "Sector WL failed to reject extended erase");
                CheckImage(f, expected);
                SetFlashLock(cart, sector, 0);
            }
            SetFlashLock(cart, start, 2); // LD without WL must allow erase.
        }
        Command(cart, 0x06); begin();
        Check(!ChipDelay(cart), "Extended erase started before CS");
        CheckImage(f, expected);
        if (state)
        {
            Savestate held(capacity + 1024); cart.DoSavestate(&held); held.Finish();
            Check(!held.Error && held.MinorVersion() == 12, "Held extended erase must require14.12");
            Fixture restored(type, 17);
            Savestate load(held.Buffer(), held.Length(), false); restored.Cart->DoSavestate(&load);
            Check(!load.Error, "Held extended erase failed cold restore");
            restored.Cart->SPIRelease();
            Check(ChipDelay(*restored.Cart) && (BusyStatus(*restored.Cart) & 3) == 1,
                  "Cold restored extended erase did not start its internal operation");
            CheckImage(restored, expected);
            Savestate pending(capacity + 1024); restored.Cart->DoSavestate(&pending); pending.Finish();
            Check(!pending.Error && pending.MinorVersion() == 12, "Internal extended erase must require14.12");
            Bytes truncated(static_cast<const u8*>(pending.Buffer()),
                            static_cast<const u8*>(pending.Buffer()) + pending.Length() - 1);
            const u32 total = truncated.size(), section = total - 16;
            std::memcpy(truncated.data() + 8, &total, 4);
            std::memcpy(truncated.data() + 20, &section, 4);
            const auto* live = cart.GetSaveMemory(); const auto notices = f.Sink.Notices;
            Savestate broken(truncated.data(), truncated.size(), false); cart.DoSavestate(&broken);
            Check(broken.Error && cart.GetSaveMemory() == live && f.Sink.Notices == notices,
                  "Truncated internal erase changed live save or emitted persistence");
            Savestate resume(pending.Buffer(), pending.Length(), false); cart.DoSavestate(&resume);
            Check(!resume.Error, "Pending extended erase failed cold restore");
        }
        else cart.SPIRelease();
        const u32 microseconds = command == 0x20 ? 40000 : type == 15 ? 4500000 : type == 16 ? 5000000 : 10000000;
        Check(ChipDelay(cart) == (u64(microseconds) * 33513982 + 999999) / 1000000 &&
              (BusyStatus(cart) & 3) == 1, "Extended erase did not start its specified typical delay or consume WEL");
        CheckImage(f, expected);
        const auto notices = f.Sink.Notices;
        const auto delay = ChipDelay(cart);
        cart.SPISelect(); Send(cart, {0xC7}); cart.SPIRelease();
        Check(ChipDelay(cart) == delay && f.Sink.Notices == notices,
              "Busy erase replaced or completed the running operation");
        CompleteChip(cart);
        std::fill_n(expected.begin() + start, count, 0xFF);
        CheckImage(f, expected);
        Check(!(BusyStatus(cart) & 3) && f.Sink.Notices == notices + 1,
              "Extended erase completion did not consume WEL or notify once");
        CompleteChip(cart);
        Check(f.Sink.Notices == notices + 1, "Duplicate extended erase completion notified again");
    }
    if (!state)
    for (u32 type : {5u, 6u, 7u})
    for (u8 command : {0x20, 0xC7})
    {
        Fixture f(type, 17); const auto expected = f.Sink.Persisted;
        Command(*f.Cart, 0x06);
        if (command == 0x20) FlashStart(*f.Cart, command, 0x12345);
        else { f.Cart->SPISelect(); Send(*f.Cart, {command}); }
        f.Cart->SPIRelease();
        Check(!ChipDelay(*f.Cart) && f.Sink.Notices == 0, "Generic Flash guessed T9HX extended erase");
        CheckImage(f, expected);
    }
}

// Unlike Command(), leave the internal power transition to its completion event.
static void PowerCommand(CartRetail& cart, u8 command)
{
    cart.SPISelect(); Send(cart, {command}); cart.SPIRelease();
}

static void FlashPower()
{
    for (u32 type : {15u, 16u, 17u})
    {
        Fixture f(type, 17); auto& cart = *f.Cart;
        auto expected = f.Sink.Persisted;
        PowerCommand(cart, 0xAB);
        Check(!ChipDelay(cart) && !BusyStatus(cart), "Awake AB started a power transition");
        cart.SPISelect(); Send(cart, {0xB9, 0}); cart.SPIRelease();
        Check(!ChipDelay(cart), "Overlong B9 entered deep power-down");
        // Neither power instruction requires WREN.
        cart.SPISelect(); Send(cart, {0xB9});
        Check(!ChipDelay(cart), "Deep power-down started before CS rose");
        cart.SPIRelease();
        Check(ChipDelay(cart) == 101, "B9 did not start the specified 3us entry bound");
        if (ChipDelay(cart) != 101) continue; // Useful negative baseline, no cascade.
        PowerCommand(cart, 0xAB);
        Check(ChipDelay(cart) == 101 && BusyStatus(cart) == 0xFF,
              "Early AB/status changed entry timing or drove the serial output");
        CompleteChip(cart);
        Check(!ChipDelay(cart) && BusyStatus(cart) == 0xFF, "Entered chip still answered RDSR");
        for (u8 command : {0x03, 0x05, 0x06, 0x01, 0x9F, 0xB9})
        {
            cart.SPISelect();
            for (u8 byte : {command, u8(0xAB), u8(0), u8(0)})
                Check(cart.SPITransmitReceive(byte) == 0xFF, "Sleeping chip drove data or reinterpreted a rejected command");
            cart.SPIRelease();
            Check(!ChipDelay(cart), "Ignored sleeping command started an operation");
        }
        cart.SPISelect(); Send(cart, {0xAB, 0}); cart.SPIRelease();
        Check(!ChipDelay(cart) && BusyStatus(cart) == 0xFF, "Overlong AB woke the chip");
        PowerCommand(cart, 0xAB);
        Check(ChipDelay(cart) == 1006, "AB did not start the specified 30us release bound");
        PowerCommand(cart, 0xB9);
        Check(ChipDelay(cart) == 1006, "Early B9 restarted or replaced the release deadline");
        // CS fell during tRDP; even the first byte arriving after completion
        // belongs to the rejected transaction. Only a new CS may issue RDSR.
        cart.SPISelect(); CompleteChip(cart);
        Check(cart.SPITransmitReceive(0x05) == 0xFF && cart.SPITransmitReceive(0) == 0xFF,
              "A transaction selected during wake became valid at completion");
        cart.SPIRelease();
        Check(!BusyStatus(cart) && !ChipDelay(cart), "Awake chip retained WIP or failed to answer");
        CheckImage(f, expected);
        Check(f.Sink.Notices == 0, "Power commands published array writes");

        WriteStatus(cart, 0x84); SetFlashLock(cart, 0, 2); SetFlashLock(cart, 65536, 1);
        Command(cart, 0x06);
        PowerCommand(cart, 0xB9); CompleteChip(cart);
        PowerCommand(cart, 0xAB); CompleteChip(cart);
        Check(BusyStatus(cart) == 0x86 && FlashLock(cart, 0) == 2 && FlashLock(cart, 65536) == 1,
              "Power-down lost WEL, protection or volatile sector locks");
        PowerCommand(cart, 0xB9); CompleteChip(cart);
        expected[0x27] = 0x96;
        cart.SetSaveMemory(expected.data(), 0x28);
        Check(BusyStatus(cart) == 0xFF && !ChipDelay(cart), "Raw save import woke a sleeping chip");
        PowerCommand(cart, 0xAB); CompleteChip(cart);
        Check(BusyStatus(cart) == 0x84 && FlashLock(cart, 65536) == 1,
              "Import changed protection/locks or power completion restored stale WEL");
        CheckImage(f, expected);
        Check(f.Sink.Notices == 1, "Import/power cycle notified more than the imported range");

        WriteStatus(cart, 0);
        // Sector zero has LD only, so this program must start normally.
        Command(cart, 0x06); FlashStart(cart, 0x0A, 0x27); Send(cart, {0x5B}); cart.SPIRelease();
        const auto delay = ChipDelay(cart);
        PowerCommand(cart, 0xB9); PowerCommand(cart, 0xAB);
        Check(delay && ChipDelay(cart) == delay && (BusyStatus(cart) & 3) == 3,
              "Power command interrupted an internal write");
        CompleteChip(cart); expected[0x27] = 0x5B;
        CheckImage(f, expected);
        for (unsigned phase = 0; phase < 3; ++phase)
        {
            PowerCommand(cart, 0xB9);
            if (phase) CompleteChip(cart);
            if (phase == 2) PowerCommand(cart, 0xAB);
            cart.CancelSave(true);
            Check(!ChipDelay(cart) && !BusyStatus(cart) && !FlashLock(cart, 0),
                  "Physical power-off did not cancel entry/sleep/release and clear volatile locks");
        }
        CheckImage(f, expected);
        Check(f.Sink.Notices == 2, "Power cancellation notified persistence");
    }
    for (u32 type : {5u, 6u, 7u})
    {
        Fixture f(type, 17); const auto expected = f.Sink.Persisted;
        Command(*f.Cart, 0x06); PowerCommand(*f.Cart, 0xB9); PowerCommand(*f.Cart, 0xAB);
        Check(!ChipDelay(*f.Cart) && BusyStatus(*f.Cart) == 2 && f.Sink.Notices == 0,
              "Capacity-only Flash guessed the T9HX power protocol");
        CheckImage(f, expected);
    }
}

static void FlashPowerState()
{
    for (u32 type : {15u, 16u, 17u})
    for (unsigned phase = 0; phase < 6; ++phase)
    {
        // Held B9, entering, asleep, held AB, waking, awake but blocked CS.
        Fixture source(type, 17); auto& cart = *source.Cart;
        WriteStatus(cart, 0x84); Command(cart, 0x06);
        cart.SPISelect(); Send(cart, {0xB9});
        if (phase) cart.SPIRelease();
        if (phase >= 2) CompleteChip(cart);
        if (phase >= 3) { cart.SPISelect(); Send(cart, {0xAB}); }
        if (phase >= 4) cart.SPIRelease();
        if (phase == 5) { cart.SPISelect(); CompleteChip(cart); }
        Savestate saved; cart.DoSavestate(&saved); saved.Finish();
        Check(!saved.Error && saved.MinorVersion() == 13, "Active power state must require14.13");
        if (saved.Error || saved.MinorVersion() != 13) continue;
        if (type == 16 && phase == 4)
        for (unsigned damage = 0; damage < 3; ++damage)
        {
            Bytes corrupt(static_cast<const u8*>(saved.Buffer()),
                          static_cast<const u8*>(saved.Buffer()) + saved.Length());
            if (damage == 0) corrupt.back() = 2; // Invalid blocked flag.
            else if (damage == 1) corrupt[corrupt.size() - 6] = 4; // Unknown power phase.
            else
            {
                corrupt.pop_back();
                const u32 length = corrupt.size(), section = length - 16;
                std::memcpy(corrupt.data() + 8, &length, 4);
                std::memcpy(corrupt.data() + 20, &section, 4);
            }
            const auto* live = cart.GetSaveMemory(); const auto notices = source.Sink.Notices;
            const auto delay = ChipDelay(cart);
            Savestate bad(corrupt.data(), corrupt.size(), false); cart.DoSavestate(&bad);
            Check(bad.Error && cart.GetSaveMemory() == live && source.Sink.Notices == notices && ChipDelay(cart) == delay,
                  "Malformed power payload replaced live memory, deadline or persistence");
        }
        Fixture receiver(type - 10, 17, 0xC3);
        auto expected = receiver.Sink.Persisted;
        std::copy_n(cart.GetSaveMemory(), expected.size() - 17, expected.begin());
        Savestate load(saved.Buffer(), saved.Length(), false); receiver.Cart->DoSavestate(&load);
        Check(!load.Error, "Cold power-state load failed");
        auto& restored = *receiver.Cart;
        CheckImage(receiver, expected);
        const auto notices = receiver.Sink.Notices;
        if (phase == 0 || phase == 3) restored.SPIRelease();
        if (phase < 2) { Check(ChipDelay(restored) == 101, "Cold entry delay differs"); CompleteChip(restored); }
        if (phase <= 2)
        {
            Check(BusyStatus(restored) == 0xFF, "Restored sleeping chip answers status");
            PowerCommand(restored, 0xAB);
        }
        if (phase < 5) { Check(ChipDelay(restored) == 1006, "Cold wake delay differs"); CompleteChip(restored); }
        else
        {
            Check(restored.SPITransmitReceive(0x05) == 0xFF && restored.SPITransmitReceive(0) == 0xFF,
                  "Cold restore lost a transaction blocked before wake completion");
            restored.SPIRelease();
        }
        Check(BusyStatus(restored) == 0x86 && !ChipDelay(restored), "Cold power completion lost WEL/protection");
        Check(receiver.Sink.Notices == notices, "Cold power completion published a save");
        CheckImage(receiver, expected);
        Savestate idle; restored.DoSavestate(&idle); idle.Finish();
        Check(!idle.Error && idle.MinorVersion() == 11, "Completed power state did not return to14.11");
    }
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string_view test = argv[1];
    if (test == "flash-power") FlashPower();
    else if (test == "flash-power-state") FlashPowerState();
    else if (test == "flash-extended-erase") FlashExtendedErase(false);
    else if (test == "flash-extended-state") FlashExtendedErase(true);
    else if (test == "flash-protection" || test == "flash-lock" || test == "flash-protection-state") FlashProtection(test);
    else if (test == "internal-write") InternalWrite(false);
    else if (test == "internal-state") InternalWrite(true);
    else if (test == "control") Control();
    else if (test == "page-ends") PageEnds();
    else if (test == "page-overflow") PageOverflow();
    else if (test == "restore-write") RestoreWrite();
    else if (test == "regular-control") RegularControl();
    else if (test == "profile-page") ProfilePage();
    else if (test == "profile-fram") ProfileFRAM();
    else if (test == "profile-state") ProfileState();
    else if (test == "status-register") StatusRegister();
    else if (test == "write-protection") WriteProtection();
    else if (test == "status-state") StatusState();
    else if (test == "flash-program" || test == "flash-page" || test == "flash-erase" || test == "flash-state") Flash(test);
    else return 2;
    std::printf("%s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
