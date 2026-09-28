// SPDX-License-Identifier: GPL-3.0-or-later
// Renderer-side resident publication and on-demand CPU pixel materialization.
#include "GPU_Vulkan.h"
#include "GPU_Soft.h"
#include "NDS.h"
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace melonDS;
static void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static PFN_vkCmdCopyImageToBuffer originalReadback;
static VKAPI_ATTR VkResult VKAPI_CALL DeviceLost(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) {
    return VK_ERROR_DEVICE_LOST;
}
static unsigned downloads;
static unsigned waits;
static PFN_vkWaitForFences originalWait;
static VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice device, uint32_t count,
    const VkFence* fences, VkBool32 all, uint64_t timeout) {
    ++waits;
    return originalWait(device, count, fences, all, timeout);
}
static VKAPI_ATTR void VKAPI_CALL Readback(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
    VkBuffer buffer, uint32_t count, const VkBufferImageCopy* regions) {
    ++downloads; originalReadback(cmd, image, layout, buffer, count, regions);
}
static PFN_vkAllocateMemory originalAllocate;
static unsigned deniedCombinedAllocations;
static unsigned readbackAllocations;
static VKAPI_ATTR VkResult VKAPI_CALL LimitReadbackAllocation(VkDevice device,
    const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
    ++readbackAllocations;
    // Two 3x screens plus native guest pixels. Single-screen storage still fits.
    constexpr VkDeviceSize combinedBytes = 2 * (256 * 192 * 3 * 3 * 4 + 256 * 192 * 4);
    if (info->allocationSize >= combinedBytes) {
        ++deniedCombinedAllocations;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return originalAllocate(device, info, callbacks, memory);
}
static void RunRenderer(const std::string& adapter) {
    NDSArgs args; args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    nds->SetRenderer(std::make_unique<VulkanRenderer>(*nds, adapter));
    auto* renderer = dynamic_cast<VulkanRenderer*>(&nds->GetRenderer());
    Require(renderer, "renderer initialization failed");
    RendererSettings settings{1, false, false, false};
    Require(renderer->SetRenderSettings(settings) && !renderer->EnableDirectDisplay(), "1x must retain RAM display");
    settings.ScaleFactor = 4;
    Require(renderer->SetRenderSettings(settings) && renderer->EnableDirectDisplay(), "resident renderer unavailable");
    nds->ARM9Write16(0x04000304, 0x020F);
    nds->ARM9Write32(0x04000000, 0x00010108);
    nds->GPU.ScreensEnabled = true;
    for (int frame = 0; frame < 4; ++frame) {
        auto& gpu = nds->GPU.GPU3D;
        gpu.RenderNumPolygons = 0;
        gpu.RenderClearAttr1 = (31u << 16) | (frame & 1 ? 0x3E0 : 0x1F);
        gpu.RenderClearAttr2 = 0x7FFF;
        gpu.RenderFrameIdentical = false;
        renderer->Start3DRendering();
        auto& table = const_cast<volk::VolkDeviceTable&>(renderer->DisplayDevice()->Functions());
        const auto saved = table.vkCmdCopyImageToBuffer;
        table.vkCmdCopyImageToBuffer = Readback;
        const auto before = downloads;
        for (u32 y = 0; y < 192; ++y) {
            nds->GPU.VCount = y;
            nds->GPU.GPU2D_A.UpdateWindows(y);
            nds->GPU.GPU2D_B.UpdateWindows(y);
            nds->GPU.GPU2D_A.UpdateRegistersPreDraw(y == 0);
            nds->GPU.GPU2D_B.UpdateRegistersPreDraw(y == 0);
            renderer->DrawSprites(y); renderer->DrawScanline(y);
            nds->GPU.GPU2D_A.UpdateRegistersPostDraw(y == 0);
            nds->GPU.GPU2D_B.UpdateRegistersPostDraw(y == 0);
        }
        originalWait = table.vkWaitForFences;
        table.vkWaitForFences = Wait;
        const auto waitsBefore = waits;
        const auto submitsBefore = renderer->TotalSubmissionCount();
        renderer->VBlank();
        const auto submitted = renderer->TotalSubmissionCount() - submitsBefore;
        Require(submitted > 0 && waits - waitsBefore < submitted,
            "VBlank did not leave display work overlapping emulation");
        if (frame & 1) {
            // [observed] A different device user must complete the display first.
            renderer->DisplayDevice()->Begin();
            renderer->DisplayDevice()->SubmitAndWait();
        }
        renderer->SwapBuffers();
        table.vkWaitForFences = originalWait;
        VulkanRenderer::ResidentFrame resident;
        Require(renderer->GetResidentFrame(resident), "renderer did not publish GPU frame");
        Require(downloads == before, "renderer publication downloaded pixels");
        Renderer::DisplayFrame cpu;
        Require(renderer->GetDisplayFrame(cpu) && cpu.width == 1024, "CPU demand failed");
        Require(downloads == before + 2, "CPU demand must download both screens exactly once");
        Require(renderer->GetDisplayFrame(cpu) && downloads == before + 2, "repeated CPU demand downloaded again");
        table.vkCmdCopyImageToBuffer = saved;
    }
    Renderer::DisplayFrame beforeResize, afterResize;
    Require(renderer->GetDisplayFrame(beforeResize), "paused frame unavailable");
    const u32 sample = static_cast<const u32*>(beforeResize.top)[0];
    settings.ScaleFactor = 2;
    Require(renderer->SetRenderSettings(settings), "paused resize failed");
    Require(renderer->GetDisplayFrame(afterResize) && afterResize.width == 512 &&
        static_cast<const u32*>(afterResize.top)[0] == sample, "paused resize lost the last frame");
    VulkanRenderer::ResidentFrame retired;
    Require(!renderer->GetResidentFrame(retired), "resize published a retired GPU frame");
    renderer->Reset();
    Require(renderer->GetDisplayFrame(afterResize) &&
        static_cast<const u32*>(afterResize.top)[0] == 0, "reset retained old pixels");
    renderer->DisableDirectDisplay("test missing extension");
    Require(!renderer->EnableDirectDisplay() && renderer->DirectDisplayStatus().find("test missing extension") != std::string::npos,
        "fallback did not retain RAM/status");
    auto& table = const_cast<volk::VolkDeviceTable&>(renderer->DisplayDevice()->Functions());
    const auto submit = table.vkQueueSubmit;
    table.vkQueueSubmit = DeviceLost;
    nds->GPU.GPU3D.RenderFrameIdentical = false;
    renderer->Start3DRendering();
    table.vkQueueSubmit = submit;
    Require(renderer->HasRenderFailure(), "device loss was not reported for renderer retirement");
    nds.reset();
    std::puts("RENDERER resident, RAM 1x, explicit readback, repeated demand, resize, reset, fallback, device loss, retirement PASS");
}

// GPU-composited 2D: resident publication stays device-side, the first CPU
// demand copies both screens in a single submission, and downloaded pixels
// match an independent software-rendered frame.
static void RunNative2D(const std::string& adapter, bool memoryPressure = false) {
    NDSArgs args; args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    nds->SetRenderer(std::make_unique<VulkanRenderer>(*nds, adapter));
    auto* renderer = dynamic_cast<VulkanRenderer*>(&nds->GetRenderer());
    Require(renderer, "native renderer initialization failed");
    auto oracleNds = std::make_unique<NDS>(NDSArgs{});
    oracleNds->Reset();
    oracleNds->SetRenderer(std::make_unique<SoftRenderer>(*oracleNds));
    auto* oracle = dynamic_cast<SoftRenderer*>(&oracleNds->GetRenderer());
    Require(oracle, "software oracle initialization failed");
    // Engine A displays the 3D clear color; engine B displays its backdrop.
    // Distinct colors across screens and frames catch stale or crossed images.
    static const u32 clearA[2] = {0x001F, 0x03E0};
    constexpr u16 backdropB = 0x7C00;
    for (const int scale : std::array<int, 2>{1, 3}) {
        if (memoryPressure && scale != 3) continue;
        RendererSettings settings{scale, false, false, false};
        settings.VulkanNative2D = true;
        Require(renderer->SetRenderSettings(settings) && renderer->Native2DActive() &&
            renderer->EnableDirectDisplay(), "native direct display unavailable");
        auto& table = const_cast<volk::VolkDeviceTable&>(renderer->DisplayDevice()->Functions());
        const auto saved = table.vkCmdCopyImageToBuffer;
        table.vkCmdCopyImageToBuffer = Readback;
        for (int frame = 0; frame < 2; ++frame) {
            for (auto* target : std::array<NDS*, 2>{nds.get(), oracleNds.get()}) {
                target->ARM9Write16(0x04000304, 0x020F);
                target->ARM9Write32(0x04000000, 0x00010108);
                target->ARM9Write32(0x04001000, 0x00010000);
                target->ARM9Write16(0x05000400, backdropB);
                target->GPU.ScreensEnabled = true;
                auto& gpu = target->GPU.GPU3D;
                gpu.RenderNumPolygons = 0;
                gpu.RenderClearAttr1 = (31u << 16) | clearA[frame];
                gpu.RenderClearAttr2 = 0x7FFF;
                gpu.RenderFrameIdentical = false;
            }
            renderer->Start3DRendering();
            oracle->Start3DRendering();
            const auto downloadsBefore = downloads;
            for (u32 y = 0; y < 192; ++y) {
                for (auto* target : std::array<NDS*, 2>{nds.get(), oracleNds.get()}) {
                    target->GPU.VCount = y;
                    target->GPU.GPU2D_A.UpdateWindows(y);
                    target->GPU.GPU2D_B.UpdateWindows(y);
                    target->GPU.GPU2D_A.UpdateRegistersPreDraw(y == 0);
                    target->GPU.GPU2D_B.UpdateRegistersPreDraw(y == 0);
                }
                renderer->DrawSprites(y); renderer->DrawScanline(y);
                oracle->DrawSprites(y); oracle->DrawScanline(y);
                for (auto* target : std::array<NDS*, 2>{nds.get(), oracleNds.get()}) {
                    target->GPU.GPU2D_A.UpdateRegistersPostDraw(y == 0);
                    target->GPU.GPU2D_B.UpdateRegistersPostDraw(y == 0);
                }
            }
            renderer->VBlank();
            oracle->VBlank();
            renderer->SwapBuffers();
            oracle->SwapBuffers();
            VulkanRenderer::ResidentFrame resident;
            Require(renderer->GetResidentFrame(resident) &&
                resident.width == 256u * scale && resident.height == 192u * scale &&
                resident.images[0] && resident.images[1],
                "native resident frame was not published");
            Require(downloads == downloadsBefore, "native resident publication downloaded pixels");
            // Resident handoff already drained the deferred batch; CPU demand
            // must add exactly one submission holding both screen copies.
            const auto submitsBefore = renderer->TotalSubmissionCount();
            Renderer::DisplayFrame cpu;
            const auto allocatedBefore = readbackAllocations;
            const auto deniedBefore = deniedCombinedAllocations;
            originalAllocate = table.vkAllocateMemory;
            if (memoryPressure) table.vkAllocateMemory = LimitReadbackAllocation;
            const bool materialized = renderer->GetDisplayFrame(cpu);
            table.vkAllocateMemory = originalAllocate;
            Require(materialized, "native CPU demand failed");
            const u64 expectedSubmissions = memoryPressure ? 2 : 1;
            if (memoryPressure) {
                if (frame == 0)
                    Require(deniedCombinedAllocations > deniedBefore,
                        "combined readback allocation failure was not injected");
                else
                    Require(readbackAllocations == allocatedBefore,
                        "fallback retried an allocation on the next CPU frame");
            }
            Require(renderer->TotalSubmissionCount() - submitsBefore == expectedSubmissions,
                "native CPU demand used an unexpected number of submissions");
            Require(downloads == downloadsBefore + 2,
                "native CPU demand must copy each screen image once");
            Require(renderer->GetDisplayFrame(cpu) &&
                renderer->TotalSubmissionCount() == submitsBefore + expectedSubmissions &&
                downloads == downloadsBefore + 2, "repeated native demand resubmitted");
            Require(cpu.kind == Renderer::DisplayFrame::Kind::CpuBGRA &&
                cpu.width == 256u * scale && cpu.height == 192u * scale &&
                cpu.top && cpu.bottom, "native CPU frame shape invalid");
            Renderer::DisplayFrame expected;
            Require(oracle->GetDisplayFrame(expected) && expected.top && expected.bottom,
                "oracle frame unavailable");
            const u32 width = 256u * scale;
            const size_t count = size_t(width) * 192u * scale;
            const auto* topPixels = static_cast<const u32*>(cpu.top);
            const auto* bottomPixels = static_cast<const u32*>(cpu.bottom);
            const auto* oracleTop = static_cast<const u32*>(expected.top);
            const auto* oracleBottom = static_cast<const u32*>(expected.bottom);
            bool match = true;
            for (size_t i = 0; i < count; ++i) {
                const size_t src = (i / width / scale) * 256 + (i % width) / scale;
                if (topPixels[i] != oracleTop[src] || bottomPixels[i] != oracleBottom[src]) {
                    match = false;
                    break;
                }
            }
            Require(match, "native CPU pixels differ from software-rendered frame");
            // DISPCNT layer enables take effect after the first scanlines.
            // Compare every pixel above; sample the settled 3D layer here.
            const size_t center = size_t(96 * scale) * width + 128 * scale;
            Require(topPixels[center] != bottomPixels[center],
                "distinct screen colors collapsed");
            // Saturated BGR15 channels map to 0x00/0xFF in CpuBGRA; engine A's
            // clear color is the exact expected 3D-screen pixel (bottom slot).
            const u32 expectA = clearA[frame] == 0x001F ? 0xFFFF0000u : 0xFF00FF00u;
            Require(expectA == bottomPixels[center], "3D screen pixel not the expected color");
            Require(expectA != topPixels[center], "3D screen color reached the 2D screen");
        }
        table.vkCmdCopyImageToBuffer = saved;
    }
    oracleNds.reset();
    nds.reset();
    std::puts(memoryPressure ?
        "NATIVE2D 3x combined allocation OOM: per-screen fallback, exact pixels, no allocation retry PASS" :
        "NATIVE2D scale1/3 resident, one-submission CPU readback, exact pixels, repeated demand PASS");
}

int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    int result=0;
    try {
        std::string reason;
        auto device=Vulkan::Device::Create(reason,{},true); Require(bool(device),reason.c_str());
        std::printf("Vulkan=%s\n",device->Properties().deviceName);
        // RunRenderer owns a device of its own. Patch this table first so the
        // counter's pristine entry point is valid for either adapter.
        auto& table=const_cast<volk::VolkDeviceTable&>(device->Functions());
        originalReadback=table.vkCmdCopyImageToBuffer; table.vkCmdCopyImageToBuffer=Readback;
        RunRenderer(device->Id());
        RunNative2D(device->Id());
        RunNative2D(device->Id(), true);
        table.vkCmdCopyImageToBuffer=originalReadback;
    } catch(const std::exception& e) { std::fprintf(stderr,"Direct display FAIL: %s\n",e.what()); result=1; }
    return result;
}

