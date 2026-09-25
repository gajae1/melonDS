// SPDX-License-Identifier: GPL-3.0-or-later
// Renderer-side resident publication and on-demand CPU pixel materialization.
#include "GPU_Vulkan.h"
#include "NDS.h"
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
static VKAPI_ATTR void VKAPI_CALL Readback(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
    VkBuffer buffer, uint32_t count, const VkBufferImageCopy* regions) {
    ++downloads; originalReadback(cmd, image, layout, buffer, count, regions);
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
        renderer->SwapBuffers();
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
        table.vkCmdCopyImageToBuffer=originalReadback;
    } catch(const std::exception& e) { std::fprintf(stderr,"Direct display FAIL: %s\n",e.what()); result=1; }
    return result;
}

