// SPDX-License-Identifier: GPL-3.0-or-later
// Real CartGame bus/save transaction acceptance; generated ROM/save bytes only.
#include "NDS.h"
#include "GBACart.h"
#include "Platform.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

using namespace melonDS;
using GBACart::CartGame;

// Reuse CartReplacement's one-shot, size-matched array-allocation failure.
// Arm only around SetSaveMemory; no allocation interposition in core sources.
static thread_local size_t FailArraySize = 0;
static thread_local unsigned ArrayFailures = 0;
void* operator new[](size_t size)
{
    if (size && size == FailArraySize)
    {
        FailArraySize = 0;
        ++ArrayFailures;
        throw std::bad_alloc();
    }
    if (void* data = std::malloc(size ? size : 1)) return data;
    throw std::bad_alloc();
}
void operator delete[](void* data) noexcept { std::free(data); }
void operator delete[](void* data, size_t) noexcept { std::free(data); }

static std::vector<u8> Snapshot(CartGame& cart);
struct SaveTrace
{
    const u8* Data = nullptr;
    u32 Capacity = 0, Offset = 0, Length = 0;
    unsigned Calls = 0;
    bool InRange = true;
    CartGame* Cart = nullptr;
    std::vector<u8> ObservedState;
};

namespace melonDS::Platform
{
void WriteGBASave(const u8* data, u32 capacity, u32 offset, u32 length, void* userdata)
{
    auto& trace = *static_cast<SaveTrace*>(userdata);
    trace.Data = data;
    trace.Capacity = capacity;
    trace.Offset = offset;
    trace.Length = length;
    ++trace.Calls;
    trace.InRange &= offset <= capacity && length <= capacity - offset;
    if (trace.Cart) trace.ObservedState = Snapshot(*trace.Cart);
}
void Log(LogLevel, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}
void Addon_RumbleStart(u32, void*) { std::abort(); }
void Addon_RumbleStop(void*) { std::abort(); }
bool Addon_KeyDown(KeyType, void*) { std::abort(); }
float Addon_MotionQuery(MotionQueryType, void*) { std::abort(); }
}

static unsigned Failures = 0;
static void Check(bool ok, const char* reason)
{
    if (!ok)
    {
        ++Failures;
        std::fprintf(stderr, "gba-eeprom-cart: %s\n", reason);
    }
}

struct Fixture
{
    SaveTrace Trace;
    std::unique_ptr<CartGame> Cart;
    explicit Fixture(u32 length, u8 value = 0x39, bool solar = false)
    {
        auto rom = std::make_unique<u8[]>(0x200);
        auto save = std::make_unique<u8[]>(length);
        std::fill_n(save.get(), length, value);
        if (solar)
            Cart = std::make_unique<GBACart::CartGameSolarSensor>(
                std::move(rom), 0x200, std::move(save), length, &Trace);
        else
            Cart = std::make_unique<CartGame>(
                std::move(rom), 0x200, std::move(save), length, &Trace);
    }
};

// Cart-relative ROM address, D0 protocol from GodMode9i ARM7 gba.c at
// 8a8f806054133978e186d3a55d4058c551dde497. These calls do not test DMA selection.
static constexpr u32 EEPROMAddress = 0x01000000;
static constexpr u64 Time = 1000;
static constexpr u64 ReadyTime = Time + (33513982ULL * 65 + 5000) / 10000;
static constexpr std::array<u8, 8> Payload {0x12, 0xE4, 0xA7, 0x10, 0x55, 0xAA, 0x06, 0xC3};

static std::vector<u16> Packet(u32 length, bool read, u16 block = 5)
{
    std::vector<u16> bits {1, u16(read)};
    for (int bit = (length == 512 ? 6 : 14) - 1; bit >= 0; --bit)
        bits.push_back((block >> bit) & 1);
    if (!read)
        for (u8 byte : Payload)
            for (int bit = 7; bit >= 0; --bit) bits.push_back((byte >> bit) & 1);
    bits.push_back(0);
    return bits;
}

static void Send(CartGame& cart, std::span<const u16> bits, u64 timestamp = Time, bool dma = true)
{
    for (u16 bit : bits) cart.ROMWriteBus(EEPROMAddress, bit, timestamp, dma);
}

static std::vector<u8> SaveBytes(CartGame& cart)
{
    return {cart.GetSaveMemory(), cart.GetSaveMemory() + cart.GetSaveMemoryLength()};
}

static std::vector<u8> Snapshot(CartGame& cart)
{
    Savestate state(0x2200);
    cart.DoSavestate(&state);
    state.Finish();
    if (state.Error)
    {
        std::fputs("gba-eeprom-cart: could not serialize test cart\n", stderr);
        std::abort();
    }
    const auto* bytes = static_cast<const u8*>(state.Buffer());
    return {bytes, bytes + state.Length()};
}

static u16 Minor(std::vector<u8>& image)
{
    Savestate state(image.data(), static_cast<u32>(image.size()), false);
    Check(!state.Error, "generated state header rejected");
    return state.MinorVersion();
}

static bool Load(CartGame& cart, std::vector<u8>& image)
{
    Savestate state(image.data(), static_cast<u32>(image.size()), false);
    // Corrupt fixtures repair outer framing so rejection reaches CartGame.
    Check(!state.Error, "fixture rejected before CartGame transaction");
    cart.DoSavestate(&state);
    return !state.Error;
}

static void Notification(Fixture& f, u32 offset, u32 count)
{
    Check(f.Trace.Calls == 1 && f.Trace.Data == f.Cart->GetSaveMemory() &&
          f.Trace.Capacity == f.Cart->GetSaveMemoryLength() &&
          f.Trace.Offset == offset && f.Trace.Length == count && f.Trace.InRange,
          "persistence count, owner, capacity or range incorrect");
}

static void CompletedBytes(Fixture& f, std::vector<u8> expected, u32 offset = 40)
{
    std::copy(Payload.begin(), Payload.end(), expected.begin() + offset);
    Check(SaveBytes(*f.Cart) == expected, "completed packet changed wrong bytes or neighbors");
    Notification(f, offset, 8);
}

static void ObserveLoad(Fixture& target, std::vector<u8>& image)
{
    target.Trace = {};
    target.Trace.Cart = target.Cart.get();
    Check(Load(*target.Cart, image), "valid cart state rejected");
    Notification(target, 0, target.Cart->GetSaveMemoryLength());
    Check(target.Trace.ObservedState == image && Snapshot(*target.Cart) == image,
          "callback observed an incomplete cart/chip transaction");
    target.Trace = {};
}

static void Bus(u32 length)
{
    Fixture f(length);
    const auto before = SaveBytes(*f.Cart);
    const auto packet = Packet(length, false);
    Send(*f.Cart, packet, Time, false);
    Check(SaveBytes(*f.Cart) == before && f.Trace.Calls == 0, "isolated CPU packet committed");
    auto idle = Snapshot(*f.Cart);
    Check(Minor(idle) == 2, "CPU packet left an active chip");

    Send(*f.Cart, std::span(packet).first(packet.size() - 1));
    Check(SaveBytes(*f.Cart) == before && f.Trace.Calls == 0, "partial write committed before dummy");
    f.Cart->ROMDeselect(false);
    Send(*f.Cart, std::span(packet).last(1));
    Check(SaveBytes(*f.Cart) == before && f.Trace.Calls == 0, "partial packet crossed deselection");
    Send(*f.Cart, packet);
    CompletedBytes(f, before);
    const auto programmed = SaveBytes(*f.Cart);
    f.Cart->ROMDeselect(true);
    Check(f.Cart->ROMReadBus(EEPROMAddress, ReadyTime - 1, false) == 0,
          "CPU poll or abort canceled committed busy");
    Send(*f.Cart, packet, ReadyTime - 1);
    Check(SaveBytes(*f.Cart) == programmed && f.Trace.Calls == 1, "busy accepted another packet");
    Check(f.Cart->ROMReadBus(EEPROMAddress, ReadyTime, false) == 1, "busy did not expire");
}

static void Import(u32 length)
{
    const auto packet = Packet(length, false);
    const size_t split = (length == 512 ? 6 : 14) + 2 + 16;
    for (bool fail : {false, true})
    {
        Fixture f(length);
        const auto original = SaveBytes(*f.Cart);
        const auto* owner = f.Cart->GetSaveMemory();
        Send(*f.Cart, std::span(packet).first(split));
        const auto pending = Snapshot(*f.Cart);
        std::vector<u8> imported(length, 0x96);
        const unsigned previousFailures = ArrayFailures;
        if (fail) FailArraySize = length;
        f.Cart->SetSaveMemory(imported.data(), length);
        const auto unconsumed = std::exchange(FailArraySize, size_t(0));
        if (fail)
        {
            Check(!unconsumed && ArrayFailures == previousFailures + 1, "import allocation hook was not reached");
            Check(f.Trace.Calls == 0 && f.Cart->GetSaveMemory() == owner &&
                  SaveBytes(*f.Cart) == original && Snapshot(*f.Cart) == pending,
                  "failed import changed owner, save or pending chip");
            Send(*f.Cart, std::span(packet).subspan(split));
            CompletedBytes(f, original);
        }
        else
        {
            Notification(f, 0, length);
            auto idle = Snapshot(*f.Cart);
            Check(Minor(idle) == 2 && SaveBytes(*f.Cart) == imported, "successful import retained pending state");
            f.Trace = {};
            Send(*f.Cart, std::span(packet).subspan(split));
            Check(f.Trace.Calls == 0 && SaveBytes(*f.Cart) == imported, "old packet wrote into imported save");
        }
    }
}

static void Reset(u32 length)
{
    Fixture f(length);
    const auto original = SaveBytes(*f.Cart);
    const auto packet = Packet(length, false);
    const size_t split = packet.size() - 20;
    Send(*f.Cart, std::span(packet).first(split));
    f.Cart->Reset();
    auto idle = Snapshot(*f.Cart);
    Check(Minor(idle) == 2, "Reset retained active command");
    Send(*f.Cart, std::span(packet).subspan(split));
    Check(f.Trace.Calls == 0 && SaveBytes(*f.Cart) == original, "Reset allowed old partial write to finish");
}

static void State(u32 length)
{
    const auto packet = Packet(length, false);
    const size_t split = (length == 512 ? 6 : 14) + 2 + 23;
    Fixture source(length), target(length, 0x77);
    const auto original = SaveBytes(*source.Cart);
    Send(*source.Cart, std::span(packet).first(split));
    auto pending = Snapshot(*source.Cart);
    Check(Minor(pending) == 5, "mid-write state is not 14.5");
    ObserveLoad(target, pending);
    Check(SaveBytes(*target.Cart) == original, "mid-write load changed old save bytes");
    Send(*target.Cart, std::span(packet).subspan(split));
    CompletedBytes(target, original);

    auto busy = Snapshot(*target.Cart);
    Check(Minor(busy) == 5, "busy state is not 14.5");
    Fixture busyTarget(length, 0x88);
    ObserveLoad(busyTarget, busy);
    const auto programmed = SaveBytes(*target.Cart);
    busyTarget.Cart->ROMDeselect(true);
    Check(busyTarget.Cart->ROMReadBus(EEPROMAddress, ReadyTime - 1, false) == 0,
          "restored busy became ready early");
    Send(*busyTarget.Cart, packet, ReadyTime - 1);
    Check(busyTarget.Trace.Calls == 0 && SaveBytes(*busyTarget.Cart) == programmed,
          "restored busy accepted a write");
    Check(busyTarget.Cart->ROMReadBus(EEPROMAddress, ReadyTime, false) == 1,
          "restored busy deadline changed");

    Send(*busyTarget.Cart, Packet(length, true), ReadyTime);
    busyTarget.Cart->ROMDeselect(false);
    constexpr unsigned consumed = 17; // Across dummy nibble and first data byte.
    for (unsigned i = 0; i < consumed; ++i)
    {
        const u16 expected = i < 4 ? 0 : (Payload[(i - 4) / 8] >> (7 - (i - 4) % 8)) & 1;
        Check(busyTarget.Cart->ROMReadBus(EEPROMAddress, ReadyTime, true) == expected,
              "read command/response gap lost data");
    }
    auto reading = Snapshot(*busyTarget.Cart);
    Check(Minor(reading) == 5, "mid-read state is not 14.5");
    Fixture readTarget(length, 0x99);
    ObserveLoad(readTarget, reading);
    for (unsigned i = consumed; i < 68; ++i)
    {
        const u16 expected = (Payload[(i - 4) / 8] >> (7 - (i - 4) % 8)) & 1;
        Check(readTarget.Cart->ROMReadBus(EEPROMAddress, ReadyTime, true) == expected,
              "restored response resumed at wrong bit");
    }
    Check(readTarget.Trace.Calls == 0 && SaveBytes(*readTarget.Cart) == programmed,
          "read response changed save or notified persistence");
    auto idle = Snapshot(*readTarget.Cart);
    Check(Minor(idle) == 2, "completed read kept active version requirement");
}

// Only malformed/legacy fixtures inspect the serialized framing. GBCS starts
// at byte 16; its payload is GPIO(6), length(4), SRAM, Flash/type(6), optional
// solar tail(4), then EEPROM(21). Semantic tests use bus calls, not chip fields.
static void RepairLength(std::vector<u8>& image)
{
    u32 length = static_cast<u32>(image.size());
    std::memcpy(image.data() + 8, &length, 4);
    length -= 16;
    std::memcpy(image.data() + 20, &length, 4);
}

static void Rejected(u32 length)
{
    Fixture source(length, 0x88);
    const auto packet = Packet(length, false);
    const size_t split = (length == 512 ? 6 : 14) + 2 + 23;
    Send(*source.Cart, std::span(packet).first(split));
    const auto saved = Snapshot(*source.Cart);
    for (unsigned corruption : {0u, 1u, 2u})
    {
        Fixture target(length);
        const auto original = SaveBytes(*target.Cart);
        const auto* owner = target.Cart->GetSaveMemory();
        Send(*target.Cart, std::span(packet).first(split));
        const auto before = Snapshot(*target.Cart);
        auto invalid = saved;
        if (corruption == 0) invalid[invalid.size() - 21] = 0xFF; // Invalid chip phase.
        else if (corruption == 1) invalid.resize(invalid.size() - 1); // Busy field truncated.
        else invalid.resize(42 + length / 2); // SRAM truncated.
        RepairLength(invalid);
        Check(!Load(*target.Cart, invalid), "invalid/truncated active state accepted");
        Check(target.Trace.Calls == 0 && target.Cart->GetSaveMemory() == owner &&
              SaveBytes(*target.Cart) == original && Snapshot(*target.Cart) == before,
              "failed state changed SRAM owner, bytes, chip or persistence");
        Send(*target.Cart, std::span(packet).subspan(split));
        CompletedBytes(target, original); // Rejected load must retain executable pending command.
    }
}

static void Legacy(u32 length)
{
    Fixture source(length, 0x62), target(length);
    auto idle = Snapshot(*source.Cart);
    Check(Minor(idle) == 2, "idle cart state version is not 14.2");
    auto legacy = idle;
    legacy.resize(legacy.size() - 21); // Generated old GBCS without the new chip tail.
    RepairLength(legacy);
    const auto packet = Packet(length, false);
    const size_t split = packet.size() - 20;
    Send(*target.Cart, std::span(packet).first(split));
    target.Trace.Cart = target.Cart.get();
    Check(Load(*target.Cart, legacy), "old 14.2 GBCS failed to load");
    Notification(target, 0, length);
    Check(target.Trace.ObservedState == idle && Snapshot(*target.Cart) == idle,
          "legacy callback observed stale serial state");
    target.Trace = {};
    Send(*target.Cart, std::span(packet).subspan(split));
    Check(target.Trace.Calls == 0 && SaveBytes(*target.Cart) == SaveBytes(*source.Cart),
          "legacy load retained executable pending command");
}

static void Solar(u32 length)
{
    Fixture source(length, 0x39, true), target(length, 0x77, true);
    auto& sensor = static_cast<GBACart::CartGameSolarSensor&>(*source.Cart);
    sensor.SetLightLevel(6);
    sensor.ROMWrite(0xC8, 1);
    sensor.ROMWrite(0xC6, 7);
    for (u16 value : {u16(2), u16(0), u16(1), u16(0)}) sensor.ROMWrite(0xC4, value);
    const auto packet = Packet(length, false);
    const size_t split = (length == 512 ? 6 : 14) + 2 + 23;
    Send(sensor, std::span(packet).first(split));
    auto saved = Snapshot(sensor);
    Check(Minor(saved) == 5, "solar pending chip version is not 14.5");
    // The four-byte derived tail must be complete before publishing anything.
    Send(*target.Cart, std::span(packet).first(split));
    const auto before = Snapshot(*target.Cart);
    const auto* owner = target.Cart->GetSaveMemory();
    auto truncated = saved;
    truncated.resize(42 + length + 6 + 3);
    RepairLength(truncated);
    Check(!Load(*target.Cart, truncated) && target.Trace.Calls == 0 &&
          target.Cart->GetSaveMemory() == owner && Snapshot(*target.Cart) == before,
          "truncated solar tail published partial cart/chip state");
    ObserveLoad(target, saved);
    Check(static_cast<GBACart::CartGameSolarSensor&>(*target.Cart).GetLightLevel() == 6 &&
          target.Cart->ROMRead(0xC6) == 7 && target.Cart->ROMRead(0xC8) == 1,
          "solar/GPIO fields were not restored");
    Send(*target.Cart, std::span(packet).subspan(split));
    CompletedBytes(target, SaveBytes(sensor));
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    void (*run)(u32) = nullptr;
    if (!std::strcmp(argv[1], "bus")) run = Bus;
    else if (!std::strcmp(argv[1], "import")) run = Import;
    else if (!std::strcmp(argv[1], "reset")) run = Reset;
    else if (!std::strcmp(argv[1], "state")) run = State;
    else if (!std::strcmp(argv[1], "rejected")) run = Rejected;
    else if (!std::strcmp(argv[1], "legacy")) run = Legacy;
    else if (!std::strcmp(argv[1], "solar")) run = Solar;
    else return 2;
    for (u32 length : {512u, 8192u})
    {
        const unsigned before = Failures;
        run(length);
        std::printf("gba-eeprom-cart/%s/%u: %s\n", argv[1], length, before == Failures ? "PASS" : "FAIL");
    }
    return Failures ? 1 : 0;
}
