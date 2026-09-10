// SPDX-License-Identifier: GPL-3.0-or-later
// Real software rasterizer and core state with deterministic host scheduling.
#include "Args.h"
#include "NDS.h"
#include "Savestate.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <string>

// Observe the existing semaphore boundaries, without adding product test hooks.
#define private public
#include "GPU3D_Soft.h"
#undef private
#include "GPU_Soft.h"
#define Semaphore_Wait RealSemaphoreWait
#define Semaphore_Post RealSemaphorePost
#include "PlatformSync.cpp"
#undef Semaphore_Post
#undef Semaphore_Wait

using namespace melonDS;
using namespace std::chrono_literals;

static void Require(bool ok, const char* message)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::fflush(nullptr);
        // A failing liveness case may intentionally have an owner blocked.
        std::_Exit(1);
    }
}

struct BoundaryGate
{
    std::mutex mutex;
    std::condition_variable changed;
    Platform::Semaphore* target = nullptr;
    bool afterWait = false, entered = false, released = false;

    void Arm(Platform::Semaphore* semaphore, bool wait)
    {
        std::lock_guard lock(mutex);
        target = semaphore; afterWait = wait; entered = released = false;
    }
    void Observe(Platform::Semaphore* semaphore, bool wait)
    {
        std::unique_lock lock(mutex);
        if (target != semaphore || afterWait != wait) return;
        target = nullptr;
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
    void Wait()
    {
        std::unique_lock lock(mutex);
        Require(changed.wait_for(lock, 5s, [&] { return entered; }), "worker did not reach boundary");
    }
    void Release()
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
} Gate;

namespace melonDS::Platform
{
void Semaphore_Wait(Semaphore* semaphore)
{
    RealSemaphoreWait(semaphore);
    Gate.Observe(semaphore, true);
}
void Semaphore_Post(Semaphore* semaphore, int count)
{
    RealSemaphorePost(semaphore, count);
    Gate.Observe(semaphore, false);
}
}

class ObservedRenderer : public SoftRenderer
{
public:
    using SoftRenderer::SoftRenderer;
    SoftRenderer3D& Three() { return *static_cast<SoftRenderer3D*>(Rend3D.get()); }
};

static void ReadFrame(SoftRenderer3D& renderer, u32 color)
{
    for (int y = 0; y < 192; ++y)
    {
        const auto* line = renderer.GetLine(y);
        for (int x = 0; x < 256; ++x)
            Require(line[x] == color, "threaded/scalar scanline pixels differ");
    }
    renderer.FinishRendering();
}

int main(int argc, char** argv)
{
    Require(argc == 2, "one scenario required");
    const std::string mode = argv[1];
    auto nds = std::make_unique<NDS>(NDSArgs{});
    nds->Reset();
    auto renderer = std::make_unique<ObservedRenderer>(*nds);
    auto* software = renderer.get();
    nds->SetRenderer(std::move(renderer));
    auto& three = software->Three();
    auto& gpu = nds->GPU.GPU3D;
    gpu.RenderClearAttr1 = 0x001F001F; // Opaque red, expected RGB6/alpha5 output.
    gpu.RenderNumPolygons = 0;
    gpu.RenderFrameIdentical = false;
    three.RenderFrame(); // Establish a real, unthreaded reference frame.
    ReadFrame(three, 0x1F00003F);

    if (mode == "normal")
    {
        for (bool threaded : {true, false, true})
        {
            three.SetThreaded(threaded);
            if (threaded) ReadFrame(three, 0x1F00003F);
            gpu.RenderClearAttr1 = 0x001F03E0;
            gpu.RenderFrameIdentical = false;
            three.RenderFrame();
            ReadFrame(three, 0x1F003F00);
            gpu.RenderFrameIdentical = true;
            three.RenderFrame();
            ReadFrame(three, 0x1F003F00);

            Savestate saved(0x100000);
            Require(nds->DoSavestate(&saved) && !saved.Error, "real core state save");
            saved.Finish();
            if (threaded) ReadFrame(three, 0x1F003F00);
            Savestate loaded(saved.Buffer(), saved.Length(), false);
            Require(nds->DoSavestate(&loaded) && !loaded.Error, "real core state load");
            if (threaded) ReadFrame(three, 0x1F003F00);
            gpu.RenderClearAttr1 = 0x001F001F;
            gpu.RenderFrameIdentical = false;
            three.RenderFrame();
            ReadFrame(three, 0x1F00003F);
        }
    }
    else
    {
        const bool afterDone = mode == "after-done";
        const bool aborted = mode == "aborted";
        const bool reset = mode == "reset";
        const bool stop = mode == "stop";
        Require(afterDone || aborted || reset || stop || mode == "before-start", "unknown scenario");
        Gate.Arm(afterDone ? three.Sema_RenderDone : aborted || stop ? three.Sema_ScanlineCount : three.Sema_RenderStart,
                 !afterDone && !aborted && !stop);
        three.SetThreaded(true);
        Gate.Wait();
        if (afterDone) three.FinishRendering();
        if (aborted) gpu.AbortFrame = true;

        std::promise<void> entered;
        auto starting = entered.get_future();
        auto pause = std::async(std::launch::async, [&] {
            entered.set_value();
            if (reset) nds->GPU.Reset();
            else if (stop) three.SetThreaded(false);
            else if (aborted) three.FinishRendering();
            else software->PreSavestate();
        });
        starting.wait();
        const bool completed = pause.wait_for(250ms) == std::future_status::ready;
        Require(completed == afterDone, afterDone
            ? "save waited for a completion already consumed by FinishRendering"
            : "state mutation/finish escaped while a render job was still in flight");
        Gate.Release();
        Require(pause.wait_for(5s) == std::future_status::ready, "owner did not resume after worker release");
        pause.get();
        if (reset) ReadFrame(three, 0);
        else if (stop)
        {
            ReadFrame(three, 0x1F00003F);
            three.SetThreaded(true);
            ReadFrame(three, 0x1F00003F);
        }
        else
        {
            gpu.AbortFrame = false;
            if (aborted) three.RestartFrame();
            else software->PostSavestate();
            ReadFrame(three, 0x1F00003F);
        }
        three.SetThreaded(false);
    }
    std::printf("software thread %s: PASS\n", mode.c_str());
}
