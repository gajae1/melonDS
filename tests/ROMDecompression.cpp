// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include <zstd.h>
#include "types.h"
#include "ROMPreparation.h"

using namespace melonDS;
using std::make_unique;
using std::unique_ptr;

enum class AllocationFailure { None, Stream, InitialBuffer, Growth, Result, Any };
static AllocationFailure allocationFailure = AllocationFailure::None;
static bool allocationFailureHit = false;

static bool FailAllocation(AllocationFailure point)
{
    if (allocationFailure != point && allocationFailure != AllocationFailure::Any) return false;
    allocationFailureHit = true;
    return true;
}

static ZSTD_DStream* CreateStream()
{
    return FailAllocation(AllocationFailure::Stream) ? nullptr : ZSTD_createDStream();
}

static void* AllocateBuffer(size_t size)
{
    return FailAllocation(AllocationFailure::InitialBuffer) ? nullptr : std::malloc(size);
}

static void* GrowBuffer(void* buffer, size_t size)
{
    return FailAllocation(AllocationFailure::Growth) ? nullptr : std::realloc(buffer, size);
}

template<typename T>
static unique_ptr<T> AllocateResult(size_t size)
{
    if (FailAllocation(AllocationFailure::Result)) throw std::bad_alloc();
    return make_unique<T>(size);
}

struct EmuInstance
{
    u32 decompressROM(const u8* inContent, const u32 inSize, unique_ptr<u8[]>& outContent)
    { return ROMPreparation::Decompress(inContent, inSize, outContent); }
};

// ExtractFunction.py generates this from the current EmuInstance.cpp definition.
// Only its owning type is supplied here; decoding uses the production body and zstd.
// Failure injection replaces allocation boundaries only, never the decoder or data.
#define ZSTD_createDStream CreateStream
#define malloc AllocateBuffer
#define realloc GrowBuffer
#define make_unique AllocateResult
#include "decompressROM.inc"
#undef make_unique
#undef realloc
#undef malloc
#undef ZSTD_createDStream

static int failures = 0;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void ZstdCheck(size_t result)
{
    if (ZSTD_isError(result)) throw std::runtime_error(ZSTD_getErrorName(result));
}

static std::vector<u8> Payload(size_t size)
{
    std::vector<u8> bytes(size);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<u8>((i * 37) ^ (i >> 8) ^ (i >> 15));
    return bytes;
}

static std::vector<u8> Frame(const std::vector<u8>& payload, bool knownSize)
{
    unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(), ZSTD_freeCCtx);
    Require(context != nullptr, "Could not create fixture compressor");
    ZstdCheck(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_contentSizeFlag, knownSize));
    ZstdCheck(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_checksumFlag, 1));
    std::vector<u8> frame(ZSTD_compressBound(payload.size()));
    const u8 empty = 0;
    const size_t size = ZSTD_compress2(context.get(), frame.data(), frame.size(),
                                     payload.empty() ? &empty : payload.data(), payload.size());
    ZstdCheck(size);
    frame.resize(size);
    Require(ZSTD_getFrameContentSize(frame.data(), frame.size()) ==
                (knownSize ? payload.size() : ZSTD_CONTENTSIZE_UNKNOWN),
            "Fixture does not have the requested content-size flag");
    return frame;
}

static std::vector<u8> Concat(std::vector<u8> first, const std::vector<u8>& second)
{
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

static u32 Decode(const std::vector<u8>& frame, unique_ptr<u8[]>& bytes)
{
    bytes = make_unique<u8[]>(frame.size());
    if (!frame.empty()) std::memcpy(bytes.get(), frame.data(), frame.size());
    EmuInstance instance;
    // loadROMData also passes its compressed buffer as both input and output owner.
    return instance.decompressROM(bytes.get(), static_cast<u32>(frame.size()), bytes);
}

static void ExpectDecoded(const char* name, const std::vector<u8>& frame,
                          const std::vector<u8>& expected)
{
    unique_ptr<u8[]> bytes;
    const u32 size = Decode(frame, bytes);
    // Never trust a wrong returned length as a readable buffer size in a red run.
    if (size != expected.size() || !bytes ||
        std::memcmp(bytes.get(), expected.data(), expected.size()) != 0)
    {
        ++failures;
        std::fprintf(stderr, "%s: returned %u, expected %zu bytes with matching content\n",
                     name, size, expected.size());
    }
}

static void ExpectRejected(const char* name, const std::vector<u8>& frame)
{
    unique_ptr<u8[]> bytes;
    const u32 size = Decode(frame, bytes);
    if (size != 0)
    {
        ++failures;
        std::fprintf(stderr, "%s: accepted invalid/empty ROM input as %u bytes\n", name, size);
    }
}

static void Lengths()
{
    const auto small = Payload(4099);
    ExpectDecoded("known size", Frame(small, true), small);
    ExpectDecoded("unknown size", Frame(small, false), small);

    // Exercise a full output buffer and the growth/tail path without a huge fixture.
    for (const size_t size : {size_t(16 * 1024 * 1024), size_t(16 * 1024 * 1024 + 257)})
    {
        const auto payload = Payload(size);
        ExpectDecoded("stream output boundary", Frame(payload, false), payload);
    }
}

static void Completion()
{
    const auto payload = Payload(4099);
    for (const bool knownSize : {true, false})
    {
        const auto frame = Frame(payload, knownSize);
        ExpectDecoded("complete checksummed frame", frame, payload);
        // Removing only the checksum tail leaves decoded bytes but no complete frame.
        auto truncated = frame;
        truncated.pop_back();
        ExpectRejected(knownSize ? "known checksum truncated" : "unknown checksum truncated", truncated);
        truncated.resize(frame.size() / 2);
        ExpectRejected(knownSize ? "known body truncated" : "unknown body truncated", truncated);
        auto corrupt = frame;
        corrupt.back() ^= 0x80;
        ExpectRejected("checksum mismatch", corrupt);
    }
}

static void Framing()
{
    const auto first = Payload(257);
    const auto second = Payload(515);
    const auto expected = Concat(first, second);
    // Zstd defines concatenated complete frames as concatenated decompressed bytes.
    ExpectDecoded("known first frame", Concat(Frame(first, true), Frame(second, false)), expected);
    ExpectDecoded("unknown first frame", Concat(Frame(first, false), Frame(second, true)), expected);
    ExpectDecoded("leading empty frame", Concat(Frame({}, true), Frame(second, true)), second);
    const std::vector<u8> skippable = {0x50, 0x2A, 0x4D, 0x18, 4, 0, 0, 0, 1, 2, 3, 4};
    ExpectDecoded("leading skippable frame", Concat(skippable, Frame(second, false)), second);
}

static void Trailing()
{
    const auto payload = Payload(4099);
    const auto next = Frame(Payload(31), false);
    for (const bool knownSize : {true, false})
    {
        const auto frame = Frame(payload, knownSize);
        ExpectRejected("trailing garbage", Concat(frame, {0xA5, 0x5A}));
        ExpectRejected("trailing partial frame header", Concat(frame, {next[0], next[1], next[2]}));
    }
}

static void Empty()
{
    ExpectRejected("empty input", {});
    ExpectRejected("empty known-size frame", Frame({}, true));
    ExpectRejected("empty unknown-size frame", Frame({}, false));
}

static void SizeLimit()
{
    // A single-segment frame header declaring 1 GiB + 1. No large payload is allocated.
    // Refuse any allocation as well, so rejecting this incomplete frame later cannot
    // conceal removal of the early size guard or allocate its declared output.
    const std::vector<u8> oversized = {0x28, 0xB5, 0x2F, 0xFD, 0xA0, 1, 0, 0, 0x40};
    Require(ZSTD_getFrameContentSize(oversized.data(), oversized.size()) == 0x40000001ULL,
            "Oversize fixture header was not recognized");
    allocationFailure = AllocationFailure::Any;
    allocationFailureHit = false;
    ExpectRejected("declared output over 1 GiB", oversized);
    Require(!allocationFailureHit, "Oversized output was not rejected before allocation");
    allocationFailure = AllocationFailure::None;
}

static void ExpectAllocationFailure(const char* name, const std::vector<u8>& frame,
                                    AllocationFailure point)
{
    auto bytes = make_unique<u8[]>(frame.size());
    std::memcpy(bytes.get(), frame.data(), frame.size());
    const u8* originalOwner = bytes.get();
    allocationFailure = point;
    allocationFailureHit = false;
    EmuInstance instance;
    const u32 size = instance.decompressROM(bytes.get(), static_cast<u32>(frame.size()), bytes);
    allocationFailure = AllocationFailure::None;
    Require(allocationFailureHit, "Allocation failure injection was not reached");
    if (size != 0 || bytes.get() != originalOwner)
    {
        ++failures;
        std::fprintf(stderr, "%s: allocation failure did not preserve the input owner and return zero\n", name);
    }
}

static void Allocation()
{
    const auto small = Payload(4099);
    const auto unknown = Frame(small, false);
    ExpectAllocationFailure("stream context", unknown, AllocationFailure::Stream);
    ExpectAllocationFailure("initial buffer", unknown, AllocationFailure::InitialBuffer);
    ExpectAllocationFailure("stream result", unknown, AllocationFailure::Result);
    ExpectAllocationFailure("known-size result", Frame(small, true), AllocationFailure::Result);
    ExpectAllocationFailure("buffer growth", Frame(Payload(16 * 1024 * 1024 + 257), false),
                            AllocationFailure::Growth);
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    try
    {
        if (!std::strcmp(argv[1], "lengths")) Lengths();
        else if (!std::strcmp(argv[1], "completion")) Completion();
        else if (!std::strcmp(argv[1], "framing")) Framing();
        else if (!std::strcmp(argv[1], "trailing")) Trailing();
        else if (!std::strcmp(argv[1], "empty")) Empty();
        else if (!std::strcmp(argv[1], "size-limit")) SizeLimit();
        else if (!std::strcmp(argv[1], "allocation")) Allocation();
        else return 2;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: exception: %s\n", argv[1], error.what());
        return 1;
    }
    std::printf("rom-decompression-%s: %s (%d failures)\n", argv[1],
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
