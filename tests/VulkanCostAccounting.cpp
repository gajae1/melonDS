// SPDX-License-Identifier: GPL-3.0-or-later
// No core, OpenGL loader, Vulkan loader, Qt, or rendering device is linked.
#include "RenderCost.h"
#include "frontend/graphics/vulkan/Presenter.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <numeric>
#include <stdexcept>

int main()
{
#if defined(PLATFORMOGL_H) || defined(__glad_h_)
    std::fputs("CPU cost accounting unexpectedly imports OpenGL\n", stderr);
    return 1;
#else
    using Meter = melonDS::RenderCostVulkanMeter;
    const auto require = [](bool value, const char* text) {
        if (!value) throw std::runtime_error(text);
    };
    try
    {
        auto meter = std::make_unique<Meter>();
        meter->Begin(100);
        const auto outer = meter->Enter(Meter::Capture, 120);
        const auto nested = meter->Enter(Meter::FullCopy, 150);
        meter->Leave(nested, 210);
        const auto nativeStage = meter->Enter(Meter::NativeCapture, 220);
        meter->Leave(nativeStage, 240);
        meter->Leave(outer, 250);
        meter->Transfer(Meter::FullReadbackBytes, 50331648);
        meter->Gpu(Meter::Gpu3D, 9999);
        meter->End(300);
        const auto& frame = meter->Last();
        require(frame.Total == 200 && frame.HostNs[Meter::Capture] == 50 &&
            frame.HostNs[Meter::FullCopy] == 60 && frame.HostNs[Meter::NativeCapture] == 20 &&
            frame.HostNs[Meter::Residual] == 70, "nested capture/readback time was double counted");
        require(std::accumulate(frame.HostNs.begin(), frame.HostNs.end(), uint64_t{}) == frame.Total,
            "exclusive host stages plus residual do not equal the measured total");
        require(frame.GpuNs[Meter::Gpu3D] == 9999 && frame.Total == 200,
            "GPU observations contaminated the host total");
        require(frame.Bytes[Meter::FullReadbackBytes] == 50331648 &&
            frame.Events[Meter::FullReadbackBytes] == 1 && !frame.Events[Meter::OverrideUploadBytes],
            "traffic occurrence is missing or conflates full readback and overrides");
        meter->Begin(400); meter->End(500);
        require(meter->Count() == 2 && meter->Sum().Total == 300 &&
            meter->Sum().HostNs[Meter::FullCopy] == 60 && meter->Sum().HostCalls[Meter::FullCopy] == 1,
            "conditional work was treated as a different whole-frame cohort");
        char report[8192]{};
        meter->Report(report, sizeof(report), "accounting");
        require(std::strstr(report, "gpu_ms_per_frame") && std::strstr(report, "unmeasured") &&
            std::strstr(report, "host_mean_ms") && std::strstr(report, "residual"),
            "report omits axes, unmeasured GPU work, or residual");
        for (uint64_t i = 0; i < 300; ++i) { meter->Begin(1000 + i * 10); meter->End(1007 + i * 10); }
        require(meter->Count() == Meter::MaxFrames && meter->Sum().Total == Meter::MaxFrames * 7 &&
            meter->Sum().HostNs[Meter::FullCopy] == 0, "bounded frame cohort retained evicted samples");
        meter->Begin(5000); meter->Enter(Meter::Capture, 5001);
        meter->Begin(6000); meter->End(6007);
        require(meter->Abandoned == 1 && meter->Last().Total == 7 && !meter->Last().HostNs[Meter::Capture],
            "aborted frame leaked costs into a completed frame");
        meter->Reset();
        require(meter->Count() == 0 && meter->Sum().Total == 0 && !meter->Active(), "sample reset failed");
        melonDS::RenderCostVulkanScope disabled(nullptr, Meter::Capture);
        melonDS::RenderCostNativeMeter native;
        require(native.Start() == 0, "native diagnostics OFF reads a clock");
        native.RecordCopy(0, 123, 7); native.PaintEnd(0);
        require(!native.Frames && !native.Paints, "disabled native meter recorded work");
        native.Enabled = true;
        const auto paintStart = native.Start();
        native.RecordCopy(native.Start(), 2 * 256 * 192 * 4, 7);
        Vulkan::Presenter::Diagnostics before{}, after{};
        after.Enabled = true;
        after.Calls = after.Submits = after.Skips = 1;
        after.StagingBytes = after.TransferBytes = 320 * 240 * 4;
        after.UploadNs = 300; after.SubmitNs = 200; after.QueuePresentNs = 400;
        native.RecordPresent(native.Start(), 320, 240, before, after);
        native.PaintEnd(paintStart);
        require(native.Submits == 1 && native.Skips == 1 && native.LastGeneration == 7 &&
            native.TransferBytes == 320 * 240 * 4 && native.Copy.IntervalBytes == 2 * 256 * 192 * 4 &&
            native.PaintNs >= native.CopyNs + native.PainterNs + native.PresentNs,
            "post-submit skip, window/screen bytes, generation, or nested present accounting is wrong");
        char nativeReport[1536];
        native.Report(nativeReport, sizeof(nativeReport), 2);
        require(std::strstr(nativeReport, "actual_submits=1 skipped_returns=1") &&
            std::strstr(nativeReport, "gpu_completion=unmeasured"),
            "native report conflates skipped calls with unsubmitted work or display completion");
        std::puts("Vulkan cost accounting: GL-free, exclusive nesting/residual, separate GPU axis, traffic occurrence, aligned cohort, bounded eviction, abort/reset PASS");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Vulkan cost accounting FAIL: %s\n", error.what());
        return 1;
    }
#endif
}
