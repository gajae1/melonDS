// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded, payload-free checks of the production native display-frame lifetime.
#include "NDS.h"
#include "GPU_Soft.h"
#include <cstdio>
#include <stdexcept>

using namespace melonDS;
namespace
{
unsigned checks = 0;
void Require(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

template<class R> void Check(NDS& nds)
{
    // Keep the RED run executable against the old interface, not a build error.
    if constexpr (!requires { typename R::DisplayFrame; })
        Require(false, "typed display-frame contract is missing");
    else
    {
        using Frame = typename R::DisplayFrame;
        const auto query = [&] {
            Frame frame{};
            R& renderer = nds.GetRenderer();
            Require(renderer.GetDisplayFrame(frame), "native display frame unavailable");
            Require(frame.kind == Frame::Kind::CpuBGRA && frame.top && frame.bottom &&
                frame.width == 256 && frame.height == 192, "native display shape changed");
            return frame;
        };
        auto previous = query();
        Require(previous.generation == query().generation, "re-query changed a current generation");
        nds.NumFrames = 17;
        nds.GetRenderer().SwapBuffers();
        auto current = query();
        Require(current.generation > previous.generation && current.generation >= nds.NumFrames,
            "produced frame was not stamped from NumFrames");
        previous = current;
        // The same front buffer can reappear. Its address is not its identity.
        nds.GetRenderer().SwapBuffers();
        nds.GetRenderer().SwapBuffers();
        current = query();
        Require(current.generation > previous.generation, "buffer reuse was not detected");
        previous = current;
        RendererSettings settings{3, false, false, false};
        Require(nds.GetRenderer().SetRenderSettings(settings), "software settings rejected");
        current = query();
        Require(current.generation > previous.generation, "settings did not invalidate the old view");
        // Software stays native even when the requested display scale changes.
        previous = current;
        nds.SetRenderer(std::make_unique<SoftRenderer>(nds));
        current = query();
        Require(current.generation > previous.generation, "replacement reused the retired generation");
        previous = current;
        nds.NumFrames = 0; // A restored guest frame count must not revive an old view.
        nds.GetRenderer().SwapBuffers();
        current = query();
        Require(current.generation > previous.generation, "guest frame rewind revived a generation");
        Require(current.generation == query().generation, "paused re-query was not stable");
        // No payload from a view is ever dereferenced after an invalidating event.
    }
}
}

int main()
{
    try
    {
        NDSArgs args; args.JIT = std::nullopt;
        auto nds = std::make_unique<NDS>(std::move(args));
        nds->Reset();
        Check<Renderer>(*nds);
        std::printf("Native display generation: %u checks, swap/reuse/settings/replacement/rewind PASS\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Native display generation FAIL: %s\n", error.what());
        return 1;
    }
}
