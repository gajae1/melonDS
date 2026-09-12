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

static void Command(CartRetail& cart, u8 command)
{
    cart.SPISelect();
    cart.SPITransmitReceive(command);
    cart.SPIRelease();
}

static u8 Status(CartRetail& cart)
{
    cart.SPISelect();
    cart.SPITransmitReceive(0x05);
    const u8 result = cart.SPITransmitReceive(0);
    cart.SPIRelease();
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
    cart.SPIRelease();
}

static void Control()
{
    Fixture f;
    auto& cart = *f.Cart;
    Bytes expected = f.Sink.Persisted;
    StartTinyWrite(cart, 0x32); // WREN is required.
    Send(cart, {0xA1, 0xB2});
    cart.SPIRelease();
    CheckImage(f, expected);
    Check(f.Sink.Notices == 0 && !(Status(cart) & 2), "Disabled WRITE changed persistence or WEL");

    Command(cart, 0x06);
    StartTinyWrite(cart, 0x32); // Address-only WRITE has no data to commit.
    cart.SPIRelease();
    Check(f.Sink.Notices == 0 && (Status(cart) & 2), "Empty WRITE consumed WEL or notified a save");
    StartTinyWrite(cart, 0x32);
    Send(cart, {0xE7, 0xD4, 0xC3});
    Check(f.Sink.Notices == 0, "WRITE notified persistence before CS release");
    cart.SPIRelease();
    expected[0x32] = 0xE7;
    expected[0x33] = 0xD4;
    expected[0x34] = 0xC3;
    CheckImage(f, expected);
    Check(f.Sink.Notices == 1 && !(Status(cart) & 2), "WRITE did not commit once and clear WEL");
    cart.SPIRelease();
    Check(f.Sink.Notices == 1, "Repeated CS release committed the same WRITE again");
    CheckRead(cart, 0x32, {0xE7, 0xD4, 0xC3});

    Command(cart, 0x06);
    Command(cart, 0x04); // WRDI must still protect later writes.
    StartTinyWrite(cart, 0x32);
    Send(cart, {0x11});
    cart.SPIRelease();
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
        f.Cart->SPIRelease();
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
        f.Cart->SPIRelease();
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
        f.Cart->SPIRelease();
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
    // Fixed 14.1 CartCommon + 512B CartRetail record, including global/section
    // headers. No new serialized fields are needed to retain the page latch.
    Check(saved.Length() == 589, "512B CartRetail changed its existing 14.1 record length");
    saved.Section("TAIL");
    u32 marker = 0x1234ABCD;
    saved.Var32(&marker);
    saved.Finish();
    Check(!saved.Error, "Generated mid-WRITE state could not be saved");
    if (saved.Error) return;
    Bytes bytes(static_cast<const u8*>(saved.Buffer()), static_cast<const u8*>(saved.Buffer()) + saved.Length());

    Send(cart, {0xEE, 0xDD}); // Disturb both the live position and memory.
    cart.SPIRelease();
    Savestate loaded(bytes.data(), u32(bytes.size()), false);
    cart.DoSavestate(&loaded);
    Check(!loaded.Error && loaded.Length() == 589, "Mid-WRITE restore changed the record boundary");
    loaded.Section("TAIL");
    marker = 0;
    loaded.Var32(&marker);
    Check(!loaded.Error && marker == 0x1234ABCD, "Retail restore consumed the following section");
    expected[0x1FF] = 0xA1;
    CheckImage(f, expected); // Existing load callback must restore persisted bytes too.
    f.Sink.Notices = 0;
    Send(cart, {0xB2, 0xC3}); // Continue the same selected WRITE, without a new header.
    cart.SPIRelease();
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
    f.Cart->SPIRelease();
    // An 8KiB device may be FRAM: do not apply the tiny EEPROM's page mask.
    expected[0x0F] = 0xA1;
    expected[0x10] = 0xB2;
    CheckImage(f, expected);
    f.Cart->SPISelect();
    Send(*f.Cart, {0x03, 0, 0x0F});
    Check(f.Cart->SPITransmitReceive(0) == 0xA1 && f.Cart->SPITransmitReceive(0) == 0xB2,
          "Regular EEPROM/FRAM sequential read changed");
    f.Cart->SPIRelease();
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
            Send(cart, {0x3C, 0xFF}); cart.SPIRelease();
            CheckImage(f, expected);
            Check(f.Sink.Notices == 0, "PROGRAM without WREN changed the save");
            for (u8 byte : {0x3C, 0xF0, 0xFF})
            {
                Command(cart, 0x06);
                FlashStart(cart, 0x02, 0x127); Send(cart, {byte});
                CheckImage(f, expected); // The memory array changes at CS release.
                cart.SPIRelease(); expected[0x127] &= byte;
                CheckImage(f, expected);
                Check(!(Status(cart) & 2), "PROGRAM did not consume WEL");
            }
            Command(cart, 0x06);
            FlashStart(cart, 0x0A, 0x127); Send(cart, {0xFF}); cart.SPIRelease();
            expected[0x127] = 0xFF; CheckImage(f, expected);
            const auto notices = f.Sink.Notices; cart.SPIRelease();
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
                CheckImage(f, expected); cart.SPIRelease();
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
                CheckImage(f, expected); cart.SPIRelease();
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
                    cart.SPIRelease();
                }
            }
        }
        else if (test == "flash-erase")
        {
            for (u8 command : {0xDB, 0xD8})
            {
                const u32 start = command == 0xDB ? 0x12300 : 0x10000;
                const u32 count = command == 0xDB ? 256 : 65536;
                FlashStart(cart, command, 0x12345); cart.SPIRelease();
                CheckImage(f, expected);
                Command(cart, 0x06);
                cart.SPISelect(); Send(cart, {command, 1, 0x23}); cart.SPIRelease();
                CheckImage(f, expected);
                Check(Status(cart) & 2, "Truncated ERASE consumed WEL");
                FlashStart(cart, command, 0x12345); Send(cart, {0}); cart.SPIRelease();
                CheckImage(f, expected); // Extra data invalidates an address-only command.
                Check(Status(cart) & 2, "Overlong ERASE consumed WEL");
                FlashStart(cart, command, 0x12345);
                CheckImage(f, expected); cart.SPIRelease();
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
                restored.Cart->SPIRelease(); CheckImage(restored, expected);
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
    cart.SPIRelease();
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
        cart.SPIRelease();
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
            cart.SPIRelease();
            expected[last] = 0xA1;
            expected[last + 1 - profile.Page] = 0xB2;
            expected[last + 2 - profile.Page] = 0xC3;
            CheckImage(f, expected); // Includes the next page and opaque file padding.
            Check(f.Sink.Notices == notices + 1 && !(Status(cart) & 2), "EEPROM release did not commit once and clear WEL");
            cart.SPIRelease();
            Check(f.Sink.Notices == notices + 1, "EEPROM repeated CS release replayed a page");
        }

        Command(cart, 0x06);
        RegularStart(cart, profile.AddressBytes, 0x02, 0x800);
        for (u32 i = 0; i < profile.Page; ++i) cart.SPITransmitReceive(0);
        for (u32 i = 0; i < profile.Page; ++i) cart.SPITransmitReceive(0xA5);
        cart.SPITransmitReceive(0x3C);
        const auto notices = f.Sink.Notices;
        CheckImage(f, expected);
        cart.SPIRelease();
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
        legacy.Cart->SPIRelease();
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
    CheckImage(f, expected); cart.SPIRelease(); CheckImage(f, expected);
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
        cart.SPIRelease(); CheckImage(f, expected);
        Check(f.Sink.Notices == notices + 1 && !(Status(cart) & 2), "FRAM release did not publish its bytes and clear WEL");
        cart.SPIRelease();
        Check(f.Sink.Notices == notices + 1, "FRAM release duplicated persistence");
        RegularRead(cart, 2, last, {0xA1, 0xB2});
    }
    Command(cart, 0x06); Command(cart, 0x04);
    RegularStart(cart, 2, 0x02, 0x1F); Send(cart, {0}); cart.SPIRelease();
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
        receiver.Cart->SPIRelease();
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
            receiver.Cart->SPIRelease(); CheckImage(receiver, expected);
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
            receiver.Cart->SPIRelease(); CheckImage(receiver, expected);
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
        receiver.Cart->SPIRelease(); expected[0x5F] = 0xA6; expected[0x40] = 0xB2;
        CheckImage(receiver, expected);
        Check(receiver.Sink.Notices == notices + 1 && !(Status(*receiver.Cart) & 2),
              "Rejected profile state disturbed the receiver's pending page/protocol");
    }
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string_view test = argv[1];
    if (test == "control") Control();
    else if (test == "page-ends") PageEnds();
    else if (test == "page-overflow") PageOverflow();
    else if (test == "restore-write") RestoreWrite();
    else if (test == "regular-control") RegularControl();
    else if (test == "profile-page") ProfilePage();
    else if (test == "profile-fram") ProfileFRAM();
    else if (test == "profile-state") ProfileState();
    else if (test == "flash-program" || test == "flash-page" || test == "flash-erase" || test == "flash-state") Flash(test);
    else return 2;
    std::printf("%s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
