// SPDX-License-Identifier: GPL-3.0-or-later
// Differential oracle: the unchanged production SoftRenderer2D::ComposeScaledLine.
#include "NDS.h"
#include "GPU3D_Soft.h"
#include "Vulkan/ComputePipeline.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>
// Test-only state generation/failure injection, not a production setting/API.
#define private public
#include "GPU_Vulkan.h"
#include "GPU3D_Vulkan.h"
#undef private
#include "Vulkan/DisplayCompositor.h"
#include "Vulkan/EmbeddedShaders.h"
#include "GPU_ColorOp.h"
#include "PixelConvert.h"

using namespace melonDS;
using Line = SoftRenderer2D::ScaledLineContext;
namespace {
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
u32 Random(u32& state)
{
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
void Equal(std::span<const u32> actual, std::span<const u32> expected, const char* label)
{
    Require(actual.size() == expected.size(), "comparison size mismatch");
    for (size_t i = 0; i < actual.size(); ++i)
        if (actual[i] != expected[i])
        {
            std::fprintf(stderr, "%s pixel %zu: actual=%08x expected=%08x\n", label, i, actual[i], expected[i]);
            throw std::runtime_error(label);
        }
}
std::shared_ptr<Vulkan::Device::Image> Upload(const std::shared_ptr<Vulkan::Device>& device,
    std::span<const u32> pixels, u32 scale)
{
    auto image = device->CreateImage(256 * scale, 192 * scale, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    auto staging = device->CreateBuffer(pixels.size_bytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging->Data(), pixels.data(), pixels.size_bytes());
    const auto& f = device->Functions();
    const auto command = device->Begin();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = image->Handle();
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    f.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {256 * scale, 192 * scale, 1};
    f.vkCmdCopyBufferToImage(command, staging->Handle(), image->Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    f.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    device->SubmitAndWait();
    return image;
}

void Differential(const std::shared_ptr<Vulkan::Device>& device, int scale, int sourceScale, size_t& compared)
{
    NDSArgs args; args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    auto& parent = static_cast<SoftRenderer&>(nds->GetRenderer());
    SoftRenderer2D oracle(nds->GPU.GPU2D_A, parent);
    oracle.Reset(); oracle.Scaled3DActive = true;
    auto& regs = nds->GPU.GPU2D_A;
    u32 seed = 0x6E624EB7;
    std::vector<u32> rgba(size_t(256) * 192 * sourceScale * sourceScale), rgb6(rgba.size());
    for (size_t i = 0; i < rgba.size(); ++i)
    {
        rgba[i] = Random(seed);
        if (i % 5 == 0) rgba[i] &= 0x00FFFFFF;
        if (i % 7 == 0) rgba[i] |= 0xFF000000;
        rgb6[i] = ((rgba[i] >> 2) & 0x003F3F3F) | ((rgba[i] >> 3) & 0x1F000000);
    }
    auto input = Upload(device, rgba, sourceScale);
    Vulkan::DisplayCompositor compositor(device, Vulkan::EmbeddedDisplayCompose(), scale);
    std::vector<Line> lines(192);
    const int width = 256 * scale;
    std::vector<u32> expected(size_t(width) * 192 * scale, 0xA537C2E1), gpu(expected), cpu(expected);
    std::vector<u32> samples(width);
    constexpr u32 flags[] = {1, 2, 4, 8, 16, 32, 0x80, 0xCF, 0x40, 0xDF};
    constexpr u32 offsets[] = {0, 1, 127, 255, 256, 257, 384, 511};
    constexpr u32 coefficients[] = {0, 1, 8, 16, 31, 255};
    constexpr u32 factors[] = {0, 1, 8, 16, 31};
    for (u32 y = 0; y < 192; ++y)
    {
        regs.BlendCnt = ((y % 4) << 6) | ((y * 17) & 63) | (((y * 23) & 63) << 8);
        regs.EVA = coefficients[(y / 4) % 6]; regs.EVB = coefficients[(y / 7) % 6]; regs.EVY = y % 17;
        const u16 brightness = (((y / 4) % 4) << 14) | factors[(y / 16) % 5];
        for (u32 x = 0; x < 256; ++x)
        {
            oracle.BGOBJLine[x] = (Random(seed) & 0x003F3F3F) | (flags[x % 10] << 24);
            oracle.BGOBJLine[x + 256] = (Random(seed) & 0x003F3F3F) | (flags[(x / 3) % 10] << 24);
            oracle.Below3D[x] = (Random(seed) & 0x003F3F3F) | (flags[x % 6] << 24);
            oracle.Below3D[x + 256] = (Random(seed) & 0x003F3F3F) | (flags[(x + 2) % 6] << 24);
            oracle.WindowMask[x] = x % 64;
        }
        auto& line = lines[y];
        Require(oracle.ExportScaledContext(line, brightness), "ordinary 3D context export rejected");
        line.sourceLine = (y * 17) % 193; line.xpos = offsets[(y / 3) % 8]; line.abort = y % 17 == 0;
        if (y >= 176)
        {
            const Line::Mode modes[] = {Line::Flat, Line::ForcedBlank, Line::Off, Line::CaptureOverride, Line::Keep};
            line.mode = modes[(y - 176) % 5];
            for (u32 x = 0; x < 256; ++x)
                line.pixels[x].top = line.mode == Line::ForcedBlank ? 0xFFFFFFFF :
                    line.mode == Line::Off ? 0xFF000000 : Random(seed);
        }
        for (int sub = 0; sub < scale; ++sub)
        {
            u32* dst = expected.data() + (size_t(y) * scale + sub) * width;
            if (line.mode == Line::Keep || line.mode == Line::CaptureOverride) continue;
            if (line.mode != Line::Composite3D)
            {
                for (int x = 0; x < 256; ++x) std::fill_n(dst + x * scale, scale, line.pixels[x].top);
                continue;
            }
            for (u32 x = 0; x < u32(width); ++x)
            {
                const u32 sx = (x * sourceScale / scale + line.xpos * sourceScale) % (512 * sourceScale);
                const u32 sy = (line.sourceLine * scale + sub) * sourceScale / scale;
                samples[x] = !line.abort && line.sourceLine < 192 && sx < u32(256 * sourceScale) ?
                    rgb6[size_t(sy) * 256 * sourceScale + sx] : 0;
            }
            oracle.ComposeScaledLine(dst, samples.data(), scale, sub);
            const u32 factor = std::min<u32>(brightness & 31, 16);
            for (int x = 0; x < width; ++x)
            {
                if ((brightness >> 14) == 1) dst[x] = ColorBrightnessUp(dst[x], factor, 0);
                else if ((brightness >> 14) == 2) dst[x] = ColorBrightnessDown(dst[x], factor, 15);
            }
            PixelConvert::ExpandScalar(dst, width);
        }
    }
    compositor.Compose(0, lines, input, sourceScale, gpu);
    Equal(gpu, expected, "GPU != production ComposeScaledLine");
    Vulkan::ComposeDisplayCPU(lines, rgb6, sourceScale, scale, cpu);
    Equal(cpu, expected, "fallback replay != production ComposeScaledLine");
    // Screen image reuse, independent destination, and unchanged CPU override rows.
    std::fill(gpu.begin(), gpu.end(), 0xA537C2E1);
    compositor.Compose(1, lines, input, sourceScale, gpu);
    Equal(gpu, expected, "second screen composition");
    compared += expected.size() * 3;
    std::printf("Differential output=%dx source=%dx: %zu pixels x GPU/CPU/screen-1 PASS\n", scale, sourceScale, expected.size());
}

struct RendererAccess : Renderer
{
    static VulkanRenderer3D& Rasterizer(Renderer& renderer)
    {
        return static_cast<VulkanRenderer3D&>(*(renderer.*(&RendererAccess::Rend3D)));
    }
};
struct FailCacheAcquisition
{
    static inline FailCacheAcquisition* active = nullptr;
    Vulkan::Device& device;
    volk::VolkDeviceTable& f;
    PFN_vkCreatePipelineCache cache;
    PFN_vkCreateShaderModule create;
    PFN_vkDestroyShaderModule destroy;
    unsigned attempts = 0, created = 0, destroyed = 0;
    VkShaderModule live{};
    explicit FailCacheAcquisition(Vulkan::Device& device)
        : device(device), f(const_cast<volk::VolkDeviceTable&>(device.Functions())),
          cache(f.vkCreatePipelineCache), create(f.vkCreateShaderModule), destroy(f.vkDestroyShaderModule)
    {
        device.ClearPipelineCache();
        active = this;
        f.vkCreatePipelineCache = Fail;
        f.vkCreateShaderModule = Create;
        f.vkDestroyShaderModule = Destroy;
    }
    ~FailCacheAcquisition()
    {
        f.vkCreatePipelineCache = cache;
        f.vkCreateShaderModule = create;
        f.vkDestroyShaderModule = destroy;
        // Clean up even the intentionally failing RED run, without hiding its
        // observed production create/destroy imbalance from the assertion.
        if (live) destroy(device.Handle(), live, nullptr);
        active = nullptr;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Fail(VkDevice, const VkPipelineCacheCreateInfo*,
        const VkAllocationCallbacks*, VkPipelineCache*)
    {
        ++active->attempts;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice device, const VkShaderModuleCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkShaderModule* module)
    {
        const auto result = active->create(device, info, allocator, module);
        if (result == VK_SUCCESS) { ++active->created; active->live = *module; }
        return result;
    }
    static VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice device, VkShaderModule module, const VkAllocationCallbacks* allocator)
    {
        if (module == active->live) { ++active->destroyed; active->live = VK_NULL_HANDLE; }
        active->destroy(device, module, allocator);
    }
};
void CacheFailureLifetime(const std::shared_ptr<Vulkan::Device>& device)
{
    Vulkan::ComputePipeline pipeline(device, Vulkan::EmbeddedShaders());
    for (bool native : {false, true})
    {
        FailCacheAcquisition failure(*device);
        bool threw = false;
        try
        {
            if (native) pipeline.EnableNativeReadback(Vulkan::EmbeddedNativeReadback());
            else { Vulkan::DisplayCompositor display(device, Vulkan::EmbeddedDisplayCompose(), 2); }
        }
        catch (const std::runtime_error&) { threw = true; }
        std::printf("Cache failure native=%d: attempts=%u shader-created=%u shader-destroyed=%u\n",
            native, failure.attempts, failure.created, failure.destroyed);
        Require(threw && failure.attempts == 1 && failure.created == failure.destroyed,
            "cache acquisition failure leaked an optional shader module");
    }
    pipeline.EnableNativeReadback(Vulkan::EmbeddedNativeReadback());
    Vulkan::DisplayCompositor retry(device, Vulkan::EmbeddedDisplayCompose(), 2);
    std::puts("Display/native cache failure resource balance and healthy-device retry PASS");
}

// One recoverable command-start error, restored before CPU fallback needs a
// readback. This tests the actual catch/replay path without retiring the device.
struct FailOneBegin
{
    static inline volk::VolkDeviceTable* table = nullptr;
    static inline PFN_vkBeginCommandBuffer original = nullptr;
    static inline bool observed = false;
    explicit FailOneBegin(Vulkan::Device& device)
    {
        table = &const_cast<volk::VolkDeviceTable&>(device.Functions());
        original = table->vkBeginCommandBuffer;
        observed = false;
        table->vkBeginCommandBuffer = Fail;
    }
    ~FailOneBegin() { table->vkBeginCommandBuffer = original; }
    static VKAPI_ATTR VkResult VKAPI_CALL Fail(VkCommandBuffer, const VkCommandBufferBeginInfo*)
    {
        table->vkBeginCommandBuffer = original;
        observed = true;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
};

std::vector<u32> IntegrationFrame(NDS& nds, bool unavailable, bool deferred, bool failBegin = false)
{
    auto& renderer = static_cast<VulkanRenderer&>(nds.GetRenderer());
    auto& raster = RendererAccess::Rasterizer(renderer);
    Require(bool(raster.Compositor), "GPU compositor was not activated");
    if (unavailable && !deferred) raster.Compositor.reset();
    auto& g3 = nds.GPU.GPU3D;
    g3.RenderNumPolygons = 0; g3.RenderClearAttr1 = (31u << 16) | 0x15AD;
    g3.RenderClearAttr2 = 0x7FFF; g3.RenderDispCnt = 0;
    g3.RenderFrameIdentical = false;
    const u64 totalBefore = renderer.TotalSubmissionCount();
    const u64 rasterBefore = renderer.SubmissionCount();
    renderer.Start3DRendering();
    Require(!renderer.HasRenderFailure(), "3D failed before integration");
    if (raster.Compositor)
        Require(raster.ScaledColorBuffer.empty(), "ordinary GPU display performed a scaled 3D CPU conversion");
    nds.GPU.ScreensEnabled = true;
    for (u32 y = 0; y < 192; ++y)
    {
        nds.GPU.VCount = y;
        g3.RenderXPos = (y * 13) & 511;
        g3.AbortFrame = y % 19 == 0;
        nds.GPU.MasterBrightnessA = ((y % 3) << 14) | (y % 17);
        nds.GPU.GPU2D_A.UpdateWindows(y); nds.GPU.GPU2D_B.UpdateWindows(y);
        nds.GPU.GPU2D_A.UpdateRegistersPreDraw(y == 0); nds.GPU.GPU2D_B.UpdateRegistersPreDraw(y == 0);
        renderer.DrawSprites(y); renderer.DrawScanline(y);
        nds.GPU.GPU2D_A.UpdateRegistersPostDraw(y == 0); nds.GPU.GPU2D_B.UpdateRegistersPostDraw(y == 0);
    }
    if (unavailable && deferred) raster.Compositor.reset();
    std::optional<FailOneBegin> failure;
    if (failBegin) failure.emplace(*raster.Device);
    // A different next 3D image must not affect rows already latched.
    g3.AbortFrame = false; g3.RenderClearAttr1 = (31u << 16) | 0x7C00;
    renderer.Start3DRendering();
    renderer.SwapBuffers();
    const u64 total = renderer.TotalSubmissionCount() - totalBefore;
    const u64 rasterSubmits = renderer.SubmissionCount() - rasterBefore;
    Require(total >= rasterSubmits, "total submission diagnostic lost raster work");
    if (failBegin)
        Require(FailOneBegin::observed && !raster.Compositor && !renderer.HasRenderFailure(),
            "recoverable command failure did not select healthy CPU fallback");
    if (!unavailable && !failBegin)
    {
        Require(total > rasterSubmits, "total diagnostic hid GPU display submissions");
        Require(raster.ScaledColorBuffer.empty(), "GPU composition read the scaled 3D image back to CPU");
    }
    std::printf("Integration unavailable=%d deferred=%d command-failure=%d: 3D submits=%llu total submits=%llu\n",
        unavailable, deferred, failBegin, static_cast<unsigned long long>(rasterSubmits), static_cast<unsigned long long>(total));
    Renderer::DisplayFrame frame;
    Require(renderer.GetDisplayFrame(frame) && frame.kind == Renderer::DisplayFrame::Kind::CpuBGRA,
        "CpuBGRA contract lost");
    Require(frame.width == 768 && frame.height == 576, "integration extent changed");
    const auto* top = static_cast<const u32*>(frame.top);
    const auto* bottom = static_cast<const u32*>(frame.bottom);
    std::vector<u32> output(top, top + size_t(frame.width) * frame.height);
    output.insert(output.end(), bottom, bottom + size_t(frame.width) * frame.height);
    return output;
}
std::unique_ptr<NDS> Console()
{
    NDSArgs args; args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args)); nds->Reset();
    nds->SetRenderer(std::make_unique<VulkanRenderer>(*nds));
    Require(dynamic_cast<VulkanRenderer*>(&nds->GetRenderer()), "Vulkan initialization unavailable");
    RendererSettings settings{3, false, false, false};
    Require(nds->GetRenderer().SetRenderSettings(settings), "3x settings rejected");
    nds->ARM9Write16(0x04000304, 0x020F);
    nds->ARM9Write32(0x04000000, 0x00010108);
    return nds;
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::strcmp(argv[1], "fallback") == 0)
        {
            auto gpu = Console(); auto cpu = Console(); auto deferred = Console();
            const auto expected = IntegrationFrame(*gpu, false, false);
            Equal(IntegrationFrame(*cpu, true, false), expected, "unavailable compositor changed output");
            Equal(IntegrationFrame(*deferred, true, true), expected, "deferred compositor failure changed output");
            auto failure = Console();
            Equal(IntegrationFrame(*failure, false, false, true), expected, "recoverable command failure changed output");
            std::puts("GPU vs unavailable-before-frame vs unavailable-after-latching vs command-start failure: both screens byte-identical PASS");
            CacheFailureLifetime(RendererAccess::Rasterizer(gpu->GetRenderer()).Device);
            return 0;
        }
        std::string error;
        auto device = Vulkan::Device::Create(error);
        if (!device) throw std::runtime_error(error);
        size_t compared = 0;
        for (int scale : {1, 2, 3, 5, 16}) Differential(device, scale, scale, compared);
        Differential(device, 3, 1, compared);
        Differential(device, 3, 5, compared);
        std::printf("Differential total: %zu pixel comparisons (%zu bytes), zero mismatches PASS\n", compared, compared * 4);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Vulkan display composition FAIL: %s\n", error.what());
        return 1;
    }
}
