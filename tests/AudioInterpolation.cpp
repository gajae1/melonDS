// SPDX-License-Identifier: GPL-3.0-or-later
#include "NDS.h"
#include "DSi.h"
#include "AudioInterpolationRenderer.h"
#include <utility>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>
using namespace melonDS;

static bool failNextAllocation = false;
static size_t failAllocationSize = 0;
void* operator new(size_t size)
{
    if (failNextAllocation && (!failAllocationSize || size == failAllocationSize))
    {
        failNextAllocation = false;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static std::unique_ptr<NDS> Make(bool dsi, AudioInterpolation interpolation)
{
    NDSArgs args;
    args.JIT.reset();
    args.BitDepth = AudioBitDepth::_16Bit;
    args.Interpolation = interpolation;
    std::unique_ptr<NDS> nds;
    if (dsi)
    {
        DSiArgs d;
        static_cast<NDSArgs&>(d) = std::move(args);
        nds = std::make_unique<DSi>(std::move(d));
    }
    else nds = std::make_unique<NDS>(std::move(args));
    Require(nds->SPU.GetInterpolation() == interpolation, "Constructor ignored requested interpolation");
    nds->Reset();
    nds->ARM9.Halt(1);
    nds->ARM7.Halt(1);
    nds->ARM7Write16(0x04000304, 1);
    nds->ARM7Write16(0x04000504, 0x200);
    nds->ARM7Write16(0x04000500, 0x807F);
    if (dsi) static_cast<DSi&>(*nds).I2S.WriteSndExCnt(0x8008, 0xFFFF);
    for (unsigned i = 0; i < 32; ++i)
        nds->ARM7Write16(0x02004000 + 2*i, s16(i*1901 - 28000));
    nds->ARM7Write32(0x04000404, 0x02004000);
    nds->ARM7Write32(0x04000408, 65536 - 4096);
    nds->ARM7Write32(0x0400040C, 16);
    nds->ARM7Write32(0x04000400, 0xA840007F);
    nds->ARM7Write32(0x04000510, 0x02008000);
    nds->ARM7Write16(0x04000514, 16);
    nds->ARM7Write8(0x04000508, 0x80);
    nds->Start();
    return nds;
}

static std::vector<s16> Frame(NDS& nds)
{
    nds.RunFrame();
    std::array<s16, 4096> pcm;
    int count = nds.SPU.ReadOutput(pcm.data(), pcm.size()/2);
    Require(count > 0 && !nds.SPU.GetOutputDroppedFrames(), "No output or discarded PCM");
    return {pcm.begin(), pcm.begin() + 2*count};
}

static std::vector<u8> State(NDS& nds, bool full)
{
    Savestate state;
    if (full) Require(nds.DoSavestate(&state), "Full state save failed");
    else
    {
        nds.SPU.PrepareSavestate(&state);
        nds.SPU.DoSavestate(&state);
    }
    state.Finish();
    Require(!state.Error, "State serialization failed");
    const auto* data = static_cast<const u8*>(state.Buffer());
    return {data, data + state.Length()};
}

static bool Load(NDS& nds, std::vector<u8>& data)
{
    Savestate state(data.data(), data.size(), false);
    return nds.DoSavestate(&state) && !state.Error;
}

static void PairedFrames(NDS& a, NDS& b, unsigned frames)
{
    for (unsigned i = 0; i < frames; ++i)
    {
        const auto left = Frame(a), right = Frame(b);
        Require(left == right && State(a, false) == State(b, false), "Paired output/history changed");
    }
}

int main() try
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (bool dsi : {false, true})
    {
        auto plain = Make(dsi, AudioInterpolation::None);
        auto quality = Make(dsi, AudioInterpolation::MinimumPhase);
        bool differentPCM = false;
        for (unsigned frame = 0; frame < 12; ++frame)
        {
            if (dsi && frame && frame%3 == 0)
            {
                for (auto* n : {plain.get(), quality.get()})
                {
                    auto& i2s = static_cast<DSi&>(*n).I2S;
                    i2s.WriteSndExCnt(0, 0x8000);
                    i2s.WriteSndExCnt(0x8008 | ((frame/3)&1 ? 0x2000 : 0), 0xFFFF);
                }
            }
            auto a = Frame(*plain), b = Frame(*quality);
            Require(a.size() == b.size(), "Reconstruction changed output frame count");
            differentPCM |= a != b;
            Require(State(*plain, false) == State(*quality, false), "Host enhancement changed guest SPU state");
            for (unsigned off = 0; off < 64; off += 4)
                Require(plain->ARM7Read32(0x02008000 + off) == quality->ARM7Read32(0x02008000 + off),
                        "Host enhancement changed captured guest audio");
        }
        Require(differentPCM, "High-quality option never reached PCM output");
        auto control = Make(dsi, AudioInterpolation::MinimumPhase);
        auto saved = State(*quality, true);
        Require(Load(*control, saved), "Clone restore failed");
        quality->SPU.ResetOutputHistory();
        control->SPU.ResetOutputHistory();
        PairedFrames(*quality, *control, 3);
        auto older = State(*quality, true);
        PairedFrames(*quality, *control, 4);
        auto backup = State(*quality, true), before = State(*quality, false);
        auto bad = older;
        bool renamed = false;
        for (size_t off = 16; off + 16 <= bad.size();)
        {
            u32 length;
            std::memcpy(&length, bad.data() + off + 4, 4);
            Require(length >= 16 && length <= bad.size() - off, "Invalid generated section length");
            if (!std::memcmp(bad.data() + off, "MIC.", 4))
            {
                std::memcpy(bad.data() + off, "BAD!", 4);
                renamed = true;
                break;
            }
            off += length;
        }
        Require(renamed && !Load(*quality, bad), "Broken state was accepted");
        Require(State(*quality, false) != before, "Failure did not exercise partial SPU mutation");
        Require(Load(*quality, backup), "Backup restore failed");
        PairedFrames(*quality, *control, 3); // Failed transaction does not reset host history.

        failNextAllocation = true;
        quality->SPU.SetInterpolation(AudioInterpolation::MinimumPhase);
        Require(std::exchange(failNextAllocation, false), "Unchanged mode allocated a new renderer");
        const auto preserved = State(*plain, false);
        failNextAllocation = true;
        bool rejected = false;
        try { plain->SPU.SetInterpolation(AudioInterpolation::MinimumPhase); }
        catch (const std::bad_alloc&) { rejected = true; }
        Require(rejected && plain->SPU.GetInterpolation() == AudioInterpolation::None &&
                State(*plain, false) == preserved, "Failed preparation changed the active mode/state");
        std::printf("dsi=%u embedded banks, scheduled rate change, capture, rollback, idempotence and preparation failure PASS\n", dsi);
    }
    NDSArgs args;
    args.JIT.reset();
    args.Interpolation = AudioInterpolation::MinimumPhase;
    void* storage = std::malloc(sizeof(NDS));
    Require(storage != nullptr, "Could not allocate constructor fixture storage");
    bool rejected = false;
    // Target the new renderer, not earlier legacy JIT allocations in the
    // constructor chain, which have a separate noexcept recovery limitation.
    failAllocationSize = sizeof(AudioInterpolationRenderer);
    failNextAllocation = true;
    try { auto* nds = new (storage) NDS(std::move(args)); nds->~NDS(); }
    catch (const std::bad_alloc&) { rejected = true; }
    std::free(storage);
    Require(rejected && !failNextAllocation, "Constructor did not propagate preparation allocation failure");
    std::puts("constructor allocation failure PASS");
    return 0;
}
catch (const std::exception& error)
{
    failNextAllocation = false;
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
