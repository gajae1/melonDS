// SPDX-License-Identifier: GPL-3.0-or-later
// Differential oracle: the unchanged production SoftRenderer2D::ComposeScaledLine.
#include "NDS.h"
#include "GPU3D_Soft.h"
#include "Vulkan/ComputeResources.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
// Test-only state generation/failure injection, not a production setting/API.
#define private public
#include "Vulkan/ComputePipeline.h"
#include "GPU_Vulkan.h"
#include "GPU3D_Vulkan.h"
#undef private
#include "Vulkan/DisplayCompositor.h"
#include "Vulkan/EmbeddedShaders.h"
#include "RenderCost.h"
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

void DirectDifferential(const std::shared_ptr<Vulkan::Device>& device, int scale, int sourceScale,
    const std::vector<Line>& original, const std::shared_ptr<Vulkan::Device::Image>& input,
    std::span<const u32> oracle, size_t& compared);

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
    DirectDifferential(device, scale, sourceScale, lines, input, expected, compared);
}

struct RendererAccess : Renderer
{
    static u32 Back(Renderer& renderer) { return renderer.*(&RendererAccess::BackBuffer); }
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
    static inline unsigned remaining = 0;
    explicit FailOneBegin(Vulkan::Device& device, unsigned skip = 0)
    {
        table = &const_cast<volk::VolkDeviceTable&>(device.Functions());
        original = table->vkBeginCommandBuffer;
        observed = false;
        remaining = skip;
        table->vkBeginCommandBuffer = Fail;
    }
    ~FailOneBegin() { table->vkBeginCommandBuffer = original; }
    static VKAPI_ATTR VkResult VKAPI_CALL Fail(VkCommandBuffer command, const VkCommandBufferBeginInfo* info)
    {
        if (remaining) { --remaining; return original(command, info); }
        table->vkBeginCommandBuffer = original;
        observed = true;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
};

std::vector<u32> IntegrationFrame(NDS& nds, bool unavailable, bool deferred, bool failBegin = false,
    bool switchScreens = false, unsigned failSkip = 0)
{
    auto& renderer = static_cast<VulkanRenderer&>(nds.GetRenderer());
    auto& raster = RendererAccess::Rasterizer(renderer);
    Require(bool(raster.Compositor), "GPU compositor was not activated");
    Renderer::DisplayFrame front;
    Require(renderer.GetDisplayFrame(front), "initial front unavailable");
    const size_t pixels = size_t(front.width) * front.height;
    const std::vector<u32> frontTop(static_cast<const u32*>(front.top), static_cast<const u32*>(front.top) + pixels);
    const std::vector<u32> frontBottom(static_cast<const u32*>(front.bottom), static_cast<const u32*>(front.bottom) + pixels);
    const u32 back = RendererAccess::Back(renderer);
    const u32* backTop = renderer.ScaledBuffers[back][0].data();
    const u32* backBottom = renderer.ScaledBuffers[back][1].data();
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
        if (switchScreens && y == 96) nds.GPU.ScreenSwap = !nds.GPU.ScreenSwap;
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
    if (failBegin) failure.emplace(*raster.Device, failSkip);
    // A different next 3D image must not affect rows already latched.
    g3.AbortFrame = false; g3.RenderClearAttr1 = (31u << 16) | 0x7C00;
    renderer.Start3DRendering();
    Renderer::DisplayFrame unchanged;
    Require(renderer.GetDisplayFrame(unchanged) && unchanged.top == front.top && unchanged.bottom == front.bottom &&
        unchanged.generation == front.generation, "back readback changed front publication");
    Equal({static_cast<const u32*>(unchanged.top), pixels}, frontTop, "back readback changed front top bytes");
    Equal({static_cast<const u32*>(unchanged.bottom), pixels}, frontBottom, "back readback changed front bottom bytes");
    Require(renderer.ScaledBuffers[back][0].data() == backTop && renderer.ScaledBuffers[back][1].data() == backBottom,
        "compositor failure changed renderer-owned backing");
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
    Require(frame.top == backTop && frame.bottom == backBottom && frame.generation > front.generation,
        "swap did not publish the completed back slot");
    Require(renderer.GetDisplayFrame(unchanged) && unchanged.top == frame.top && unchanged.bottom == frame.bottom &&
        unchanged.generation == frame.generation, "paused/repeated frame query changed publication");
    const auto* top = static_cast<const u32*>(frame.top);
    const auto* bottom = static_cast<const u32*>(frame.bottom);
    std::vector<u32> output(top, top + size_t(frame.width) * frame.height);
    output.insert(output.end(), bottom, bottom + size_t(frame.width) * frame.height);
    return output;
}
// Real Vulkan failures, confined to the first display-backing attempt(s).
// Record/free forwarding checks that partially allocated backing is released.
struct FailDisplayMemory
{
    enum Mode { Allocation, Mapping, MemoryType };
    static inline FailDisplayMemory* active = nullptr;
    volk::VolkDeviceTable& f;
    PFN_vkAllocateMemory allocate;
    PFN_vkFreeMemory free;
    PFN_vkMapMemory map;
    PFN_vkGetBufferMemoryRequirements requirements;
    Mode mode;
    unsigned nth, attempts = 0, freed = 0;
    bool injected = false;
    std::vector<VkDeviceMemory> allocated;
    FailDisplayMemory(Vulkan::Device& device, Mode mode, unsigned nth)
        : f(const_cast<volk::VolkDeviceTable&>(device.Functions())), allocate(f.vkAllocateMemory),
          free(f.vkFreeMemory), map(f.vkMapMemory), requirements(f.vkGetBufferMemoryRequirements), mode(mode), nth(nth)
    {
        allocated.reserve(4);
        active = this;
        f.vkAllocateMemory = Allocate; f.vkFreeMemory = Free;
        f.vkMapMemory = Map; f.vkGetBufferMemoryRequirements = Requirements;
    }
    ~FailDisplayMemory()
    {
        f.vkAllocateMemory = allocate; f.vkFreeMemory = free;
        f.vkMapMemory = map; f.vkGetBufferMemoryRequirements = requirements;
        active = nullptr;
    }
    static bool Fail(Mode mode)
    {
        if (!active->injected && active->mode == mode && ++active->attempts == active->nth)
            return active->injected = true;
        return false;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice device, const VkMemoryAllocateInfo* info,
        const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory)
    {
        if (Fail(Allocation)) return VK_ERROR_OUT_OF_HOST_MEMORY;
        const auto result = active->allocate(device, info, callbacks, memory);
        if (!active->injected && result == VK_SUCCESS) active->allocated.push_back(*memory);
        return result;
    }
    static VKAPI_ATTR void VKAPI_CALL Free(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* callbacks)
    {
        if (std::find(active->allocated.begin(), active->allocated.end(), memory) != active->allocated.end()) ++active->freed;
        active->free(device, memory, callbacks);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Map(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
        VkDeviceSize size, VkMemoryMapFlags flags, void** data)
    {
        if (Fail(Mapping)) return VK_ERROR_MEMORY_MAP_FAILED;
        return active->map(device, memory, offset, size, flags, data);
    }
    static VKAPI_ATTR void VKAPI_CALL Requirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements* requirements)
    {
        active->requirements(device, buffer, requirements);
        if (Fail(MemoryType)) requirements->memoryTypeBits = 0;
    }
};
std::unique_ptr<NDS> Console(int scale = 3, bool forceVector = false)
{
    NDSArgs args; args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args)); nds->Reset();
    nds->SetRenderer(std::make_unique<VulkanRenderer>(*nds));
    Require(dynamic_cast<VulkanRenderer*>(&nds->GetRenderer()), "Vulkan initialization unavailable");
    std::optional<FailDisplayMemory> failure;
    if (forceVector && scale > 1)
        failure.emplace(*RendererAccess::Rasterizer(nds->GetRenderer()).Device, FailDisplayMemory::Allocation, 1);
    RendererSettings settings{scale, false, false, false};
    Require(nds->GetRenderer().SetRenderSettings(settings), "integration settings rejected");
    if (failure) Require(failure->injected, "forced-vector benchmark did not exercise allocation fallback");
    nds->ARM9Write16(0x04000304, 0x020F);
    nds->ARM9Write32(0x04000000, 0x00010108);
    return nds;
}

void AllocationFallback()
{
    auto reference = Console();
    const auto expected = IntegrationFrame(*reference, false, false);
    for (const auto [mode, nth] : {std::pair{FailDisplayMemory::Allocation, 1u},
        std::pair{FailDisplayMemory::Allocation, 3u}, std::pair{FailDisplayMemory::Mapping, 3u},
        std::pair{FailDisplayMemory::MemoryType, 1u}})
    {
        auto nds = Console(1);
        auto& renderer = static_cast<VulkanRenderer&>(nds->GetRenderer());
        auto& raster = RendererAccess::Rasterizer(renderer);
        {
            FailDisplayMemory failure(*raster.Device, mode, nth);
            RendererSettings settings{3, false, false, false};
            Require(renderer.SetRenderSettings(settings), "display allocation failure rejected the vector fallback");
            Require(failure.injected && failure.freed == failure.allocated.size(), "partial display backing leaked");
            for (unsigned slot = 0; slot < 2; ++slot)
            for (unsigned screen = 0; screen < 2; ++screen)
                Require(!renderer.ScaledMemory[slot][screen] &&
                    renderer.ScaledBuffers[slot][screen].data() == renderer.ScaledStorage[slot][screen].data() &&
                    renderer.ScaledStorage[slot][screen].size() == size_t(768) * 576,
                    "allocation failure did not preserve all-vector backing");
            std::printf("Backing failure mode=%u attempt=%u: prior_allocations=%zu freed=%u PASS\n",
                unsigned(mode), nth, failure.allocated.size(), failure.freed);
        }
        Equal(IntegrationFrame(*nds, false, false), expected, "vector/staging allocation fallback changed output");
        Require(bool(raster.Compositor->readback), "vector fallback did not use the legacy staging path");
    }
    auto mixed = Console(); auto late = Console();
    const auto mixedExpected = IntegrationFrame(*mixed, false, false, false, true);
    Equal(IntegrationFrame(*late, false, false, true, true, 1), mixedExpected,
        "failure after first screen transfer changed CPU replay/backing");
    std::puts("Allocation/map/type fallback and late second-screen failure: byte-identical PASS");
}

void BackingLifecycle()
{
    auto nds = Console();
    auto& renderer = static_cast<VulkanRenderer&>(nds->GetRenderer());
    IntegrationFrame(*nds, false, false);
    using Snapshots = std::array<std::array<std::vector<u32>, 2>, 2>;
    using WeakBacking = std::array<std::array<std::weak_ptr<Vulkan::Device::Buffer>, 2>, 2>;
    Snapshots native;
    // Distinguish both native slots and all subpixels; resize must retain the
    // appropriate old slot, not copy the front screen into all four buffers.
    for (unsigned pass = 0; pass < 2; ++pass)
    {
        void* top = nullptr; void* bottom = nullptr;
        Require(renderer.GetFramebuffers(&top, &bottom), "native lifecycle buffers missing");
        const u32 slot = RendererAccess::Back(renderer) ^ 1;
        u32* screens[] = {static_cast<u32*>(top), static_cast<u32*>(bottom)};
        for (unsigned screen = 0; screen < 2; ++screen)
        {
            for (size_t i = 0; i < 256 * 192; ++i)
                screens[screen][i] = 0xFF005500 ^ u32(i * 19 + slot * 127 + screen * 31);
            native[slot][screen].assign(screens[screen], screens[screen] + 256 * 192);
        }
        renderer.SwapBuffers();
    }
    for (unsigned slot = 0; slot < 2; ++slot)
    for (unsigned screen = 0; screen < 2; ++screen)
        for (size_t i = 0; i < renderer.ScaledBuffers[slot][screen].size(); ++i)
            renderer.ScaledBuffers[slot][screen][i] = 0xFF123400 ^ u32(i * 23 + slot * 113 + screen * 29);
    int oldScale = 3;
    size_t compared = 0;
    for (int scale : {16, 1, 16, 3})
    {
        Renderer::DisplayFrame before, after;
        Require(renderer.GetDisplayFrame(before), "pre-resize display missing");
        Snapshots old = native;
        WeakBacking retired;
        for (unsigned slot = 0; slot < 2; ++slot)
        for (unsigned screen = 0; screen < 2; ++screen)
        {
            retired[slot][screen] = renderer.ScaledMemory[slot][screen];
            if (oldScale > 1)
                old[slot][screen].assign(renderer.ScaledBuffers[slot][screen].begin(), renderer.ScaledBuffers[slot][screen].end());
        }
        RendererSettings settings{scale, false, false, false};
        Require(renderer.SetRenderSettings(settings) && renderer.GetDisplayFrame(after), "paused resize failed");
        Require(after.width == u32(256 * scale) && after.height == u32(192 * scale) && after.generation > before.generation,
            "paused resize did not invalidate extent/generation");
        for (unsigned slot = 0; slot < 2; ++slot)
        for (unsigned screen = 0; screen < 2; ++screen)
        {
            Require(retired[slot][screen].expired(), "resize retained retired mapped backing");
            if (scale == 1)
            {
                Require(!renderer.ScaledMemory[slot][screen] && renderer.ScaledBuffers[slot][screen].empty(),
                    "1x retained scaled storage");
                continue;
            }
            const auto& memory = renderer.ScaledMemory[slot][screen];
            constexpr auto required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            Require(memory && (memory->MemoryProperties() & required) == required && renderer.ScaledStorage[slot][screen].empty(),
                "direct backing selected unsuitable CPU memory or kept redundant vectors");
            const auto output = renderer.ScaledBuffers[slot][screen];
            for (int y = 0; y < 192 * scale; ++y)
            for (int x = 0; x < 256 * scale; ++x)
                Require(output[size_t(y) * 256 * scale + x] ==
                    old[slot][screen][size_t(y * oldScale / scale) * 256 * oldScale + x * oldScale / scale],
                    "paused resize changed slot-specific subpixels");
            compared += output.size();
        }
        if (scale == 1)
        {
            const u32 front = RendererAccess::Back(renderer) ^ 1;
            Equal({static_cast<const u32*>(after.top), 256 * 192}, native[front][0], "1x changed native top");
            Equal({static_cast<const u32*>(after.bottom), 256 * 192}, native[front][1], "1x changed native bottom");
        }
        const auto pointers = renderer.ScaledBuffers;
        Require(renderer.SetRenderSettings(settings), "same-scale settings rejected");
        for (unsigned slot = 0; slot < 2; ++slot)
        for (unsigned screen = 0; screen < 2; ++screen)
            Require(renderer.ScaledBuffers[slot][screen].data() == pointers[slot][screen].data(),
                "same-scale settings replaced backing");
        oldScale = scale;
    }
    std::printf("Backing paused resize 3->16->1->16->3: %zu resampled pixels; memory_flags=0x%x PASS\n",
        compared, renderer.ScaledMemory[0][0]->MemoryProperties());
    for (bool reset : {false, true})
    {
        for (auto& slot : renderer.ScaledBuffers)
            for (auto screen : slot) std::fill(screen.begin(), screen.end(), 0xFFA55331);
        if (reset) renderer.Reset(); else renderer.Stop();
        for (auto& slot : renderer.ScaledBuffers)
            for (auto screen : slot)
                Require(std::all_of(screen.begin(), screen.end(), [](u32 pixel) { return pixel == 0; }),
                    "reset/stop left mapped pixels stale");
    }
    WeakBacking retired;
    for (unsigned slot = 0; slot < 2; ++slot)
        for (unsigned screen = 0; screen < 2; ++screen) retired[slot][screen] = renderer.ScaledMemory[slot][screen];
    std::weak_ptr<Vulkan::Device> device = RendererAccess::Rasterizer(renderer).Device;
    Renderer::DisplayFrame before, after;
    Require(renderer.GetDisplayFrame(before), "pre-replacement frame missing");
    nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
    Require(nds->GetRenderer().GetDisplayFrame(after) && after.kind == Renderer::DisplayFrame::Kind::CpuBGRA &&
        after.width == 256 && after.height == 192 && after.generation > before.generation,
        "renderer replacement broke CPU publication");
    for (const auto& slot : retired)
        for (const auto& memory : slot) Require(memory.expired(), "renderer replacement leaked mapped backing");
    Require(device.expired(), "renderer replacement left direct-backing device alive");
    std::puts("Mapped backing reset/stop/replacement ownership and immediate CPU publication PASS");
}

// Observe real transfers without replacing their work or changing diagnostics.
struct CopyProbe
{
    static inline CopyProbe* active = nullptr;
    volk::VolkDeviceTable& f;
    PFN_vkCmdCopyImageToBuffer original;
    u64 bytes = 0;
    VkBuffer target{};
    std::vector<VkBufferImageCopy> regions;
    explicit CopyProbe(Vulkan::Device& device)
        : f(const_cast<volk::VolkDeviceTable&>(device.Functions())), original(f.vkCmdCopyImageToBuffer)
    {
        regions.reserve(192);
        active = this;
        f.vkCmdCopyImageToBuffer = Copy;
    }
    ~CopyProbe() { f.vkCmdCopyImageToBuffer = original; active = nullptr; }
    static VKAPI_ATTR void VKAPI_CALL Copy(VkCommandBuffer command, VkImage image, VkImageLayout layout,
        VkBuffer buffer, uint32_t count, const VkBufferImageCopy* copies)
    {
        active->target = buffer;
        active->regions.assign(copies, copies + count);
        for (u32 i = 0; i < count; ++i)
            active->bytes += u64(copies[i].imageExtent.width) * copies[i].imageExtent.height * copies[i].imageExtent.depth * 4;
        active->original(command, image, layout, buffer, count, copies);
    }
};
void DirectDifferential(const std::shared_ptr<Vulkan::Device>& device, int scale, int sourceScale,
    const std::vector<Line>& original, const std::shared_ptr<Vulkan::Device::Image>& input,
    std::span<const u32> oracle, size_t& compared)
{
    Vulkan::DisplayCompositor compositor(device, Vulkan::EmbeddedDisplayCompose(), scale);
    std::array<std::array<std::shared_ptr<Vulkan::Device::Buffer>, 2>, 2> backing;
    const size_t row = size_t(256) * scale * scale;
    for (auto& slot : backing)
        for (auto& screen : slot)
            screen = device->CreateBuffer(oracle.size_bytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                true, 0, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    for (unsigned slot = 0; slot < 2; ++slot)
    for (unsigned screen = 0; screen < 2; ++screen)
    {
        const auto& memory = backing[slot][screen];
        std::span<u32> output{static_cast<u32*>(memory->Data()), oracle.size()};
        std::vector<u32> expected(oracle.begin(), oracle.end());
        auto lines = original;
        const u32 sentinel = 0xA537C2E1 ^ (slot << 20) ^ (screen << 24);
        std::fill(output.begin(), output.end(), sentinel);
        for (u32 y = 0; y < 192; ++y)
        {
            const bool cpuFill = slot == 0 && (y % 8 == 2 || y % 8 == 3);
            if (cpuFill || (slot == 0 && y % 8 == 7) || (slot == 1 && y % 2 && y != 191))
                lines[y].mode = Line::Keep;
            else if (slot == 0 && y % 8 == 5) lines[y].mode = Line::CaptureOverride;
            if (cpuFill || lines[y].mode == Line::CaptureOverride)
                for (size_t x = 0; x < row; ++x)
                    output[y * row + x] = (cpuFill ? 0xFF125600 : 0xFF983400) ^ u32(x + y * 131 + screen);
            if (lines[y].mode == Line::Keep || lines[y].mode == Line::CaptureOverride)
                std::copy_n(output.data() + y * row, row, expected.data() + y * row);
        }
        CopyProbe probe(*device);
        compositor.Compose(screen, lines, input, sourceScale, output, memory.get());
        Equal(output, expected, "direct backing != production oracle/preserved CPU rows");
        Require(probe.target == memory->Handle() && !compositor.readback,
            "direct copy did not target renderer-compatible backing without staging");
        std::array<unsigned, 192> visits{};
        for (const auto& copy : probe.regions)
        {
            Require(copy.imageOffset.x == 0 && copy.imageOffset.z == 0 && copy.imageOffset.y >= 0 &&
                copy.imageOffset.y % scale == 0 && copy.imageExtent.width == u32(256 * scale) &&
                copy.imageExtent.height % scale == 0 && copy.imageExtent.depth == 1 &&
                copy.imageSubresource.layerCount == 1 && copy.bufferRowLength == 0 && copy.bufferImageHeight == 0,
                "direct transfer region has wrong addressing");
            const u32 first = copy.imageOffset.y / scale, end = first + copy.imageExtent.height / scale;
            Require(first < end && end <= 192 && copy.bufferOffset == first * row * sizeof(u32),
                "direct transfer range exceeds backing or has wrong offset");
            for (u32 y = first; y < end; ++y) ++visits[y];
            Require(first == 0 || lines[first - 1].mode == Line::Keep || lines[first - 1].mode == Line::CaptureOverride,
                "contiguous composed rows were not coalesced");
        }
        unsigned composed = 0;
        for (u32 y = 0; y < 192; ++y)
        {
            const bool writes = lines[y].mode != Line::Keep && lines[y].mode != Line::CaptureOverride;
            Require(visits[y] == unsigned(writes), "direct transfer overwrote or missed a row");
            composed += writes;
        }
        Require(probe.bytes == composed * row * sizeof(u32), "direct transfer byte accounting mismatch");
        compared += oracle.size();
        std::printf("Direct output=%dx source=%dx slot=%u screen=%u: ranges=%zu copied_bytes=%llu preserved_bytes=%llu PASS\n",
            scale, sourceScale, slot, screen, probe.regions.size(), static_cast<unsigned long long>(probe.bytes),
            static_cast<unsigned long long>(oracle.size_bytes() - probe.bytes));
    }
    auto memory = backing[0][0];
    std::span<u32> output{static_cast<u32*>(memory->Data()), oracle.size()};
    std::vector<u32> before(output.begin(), output.end());
    std::vector<Line> keep(192);
    for (auto& line : keep) line.mode = Line::Keep;
    CopyProbe probe(*device);
    compositor.Compose(0, keep, input, sourceScale, output, memory.get());
    Equal(output, before, "all-Keep changed direct backing");
    Require(probe.bytes == 0, "all-Keep issued a transfer");
    bool rejected = false;
    try { compositor.Compose(0, keep, input, sourceScale, before, memory.get()); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "mismatched mapped destination accepted");
    compared += oracle.size();
}
// Diagnostics must observe, not alter, real rendering or synchronization.
struct DiagnosticEnvironment
{
    std::optional<std::string> previous;
    explicit DiagnosticEnvironment(bool enabled)
    {
        if (const char* value = std::getenv("MELONDS_RENDER_DIAGNOSTICS")) previous = value;
        Set(enabled ? "1" : "0");
    }
    static void Set(const char* value)
    {
#ifdef _WIN32
        _putenv_s("MELONDS_RENDER_DIAGNOSTICS", value ? value : "");
#else
        if (value) setenv("MELONDS_RENDER_DIAGNOSTICS", value, 1);
        else unsetenv("MELONDS_RENDER_DIAGNOSTICS");
#endif
    }
    ~DiagnosticEnvironment() { Set(previous ? previous->c_str() : nullptr); }
};
struct DiagnosticProbe
{
    enum Failure { None, PoolFailure, ResultFailure, MissingTimestamp, UnreadyResults };
    static inline DiagnosticProbe* active = nullptr;
    volk::VolkDeviceTable& f;
    volk::VolkDeviceTable original;
    Failure failure;
    u64 uploads = 0, downloads = 0, submits = 0, waits = 0, idles = 0;
    u64 pools = 0, timestamps = 0, results = 0;
    bool completed = false, invalid = false;
    DiagnosticProbe(Vulkan::Device& device, Failure failure)
        : f(const_cast<volk::VolkDeviceTable&>(device.Functions())), original(f), failure(failure)
    {
        active = this;
        f.vkCmdCopyBufferToImage = Upload;
        f.vkCmdCopyImageToBuffer = Download;
        f.vkQueueSubmit = Submit;
        f.vkWaitForFences = Wait;
        f.vkDeviceWaitIdle = Idle;
        f.vkCreateQueryPool = Pool;
        f.vkCmdWriteTimestamp = failure == MissingTimestamp ? nullptr : Timestamp;
        f.vkGetQueryPoolResults = Results;
    }
    ~DiagnosticProbe() { f = original; active = nullptr; }
    static u64 Bytes(u32 count, const VkBufferImageCopy* regions)
    {
        u64 bytes = 0;
        for (u32 i = 0; i < count; ++i)
            bytes += u64(regions[i].imageExtent.width) * regions[i].imageExtent.height *
                regions[i].imageExtent.depth * regions[i].imageSubresource.layerCount * 4;
        return bytes;
    }
    static VKAPI_ATTR void VKAPI_CALL Upload(VkCommandBuffer command, VkBuffer buffer, VkImage image,
        VkImageLayout layout, u32 count, const VkBufferImageCopy* regions)
    {
        active->uploads += Bytes(count, regions);
        active->original.vkCmdCopyBufferToImage(command, buffer, image, layout, count, regions);
    }
    static VKAPI_ATTR void VKAPI_CALL Download(VkCommandBuffer command, VkImage image, VkImageLayout layout,
        VkBuffer buffer, u32 count, const VkBufferImageCopy* regions)
    {
        active->downloads += Bytes(count, regions);
        active->original.vkCmdCopyImageToBuffer(command, image, layout, buffer, count, regions);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue, u32 count, const VkSubmitInfo* info, VkFence fence)
    {
        active->completed = false;
        const auto result = active->original.vkQueueSubmit(queue, count, info, fence);
        if (result == VK_SUCCESS) active->submits += count;
        return result;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice device, u32 count, const VkFence* fences,
        VkBool32 all, u64 timeout)
    {
        ++active->waits;
        const auto result = active->original.vkWaitForFences(device, count, fences, all, timeout);
        active->completed = result == VK_SUCCESS;
        return result;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Idle(VkDevice device)
    {
        ++active->idles;
        return active->original.vkDeviceWaitIdle(device);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Pool(VkDevice device, const VkQueryPoolCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkQueryPool* pool)
    {
        ++active->pools;
        if (active->failure == PoolFailure) return VK_ERROR_OUT_OF_HOST_MEMORY;
        return active->original.vkCreateQueryPool(device, info, allocator, pool);
    }
    static VKAPI_ATTR void VKAPI_CALL Timestamp(VkCommandBuffer command, VkPipelineStageFlagBits stage,
        VkQueryPool pool, u32 query)
    {
        ++active->timestamps;
        active->original.vkCmdWriteTimestamp(command, stage, pool, query);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Results(VkDevice device, VkQueryPool pool, u32 first, u32 count,
        size_t bytes, void* data, VkDeviceSize stride, VkQueryResultFlags flags)
    {
        ++active->results;
        active->invalid |= !active->completed || (flags & VK_QUERY_RESULT_WAIT_BIT);
        if (active->failure == ResultFailure) return VK_ERROR_OUT_OF_HOST_MEMORY;
        const auto result = active->original.vkGetQueryPoolResults(device, pool, first, count, bytes, data, stride, flags);
        if (active->failure == UnreadyResults)
        {
            std::memset(data, 0, bytes);
            return VK_NOT_READY;
        }
        return result;
    }
};
struct DiagnosticOutput
{
    std::vector<u32> pixels;
    u64 uploads, downloads, submits, waits, idles, pools, timestamps, results;
    bool timestampSupport;
};
DiagnosticOutput DiagnosticExercise(bool enabled, DiagnosticProbe::Failure failure)
{
    DiagnosticEnvironment environment(enabled);
    std::string error;
    auto device = Vulkan::Device::Create(error);
    Require(bool(device), error.c_str());
    DiagnosticProbe probe(*device, failure);
    if (device->Costs()) device->Costs()->Begin(RenderCostNowNs());
    constexpr u32 scale = 3;
    const size_t count = size_t(256) * 192 * scale * scale;
    std::vector<u32> rgba(count, 0xFF6C98DC), rgb6(count, 0x1F1B2637);
    auto input = Upload(device, rgba, scale);
    Vulkan::DisplayCompositor compositor(device, Vulkan::EmbeddedDisplayCompose(), scale);
    auto memory = device->CreateBuffer(count * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        true, 0, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    std::span<u32> output{static_cast<u32*>(memory->Data()), count};
    std::vector<Line> lines(192);
    for (u32 y = 0; y < 192; ++y)
    {
        lines[y].mode = y % 4 == 0 ? Line::Composite3D : y % 2 ? Line::Keep : Line::CaptureOverride;
        lines[y].sourceLine = y;
        for (auto& pixel : lines[y].pixels) { pixel.top = 0x40000000; pixel.second = 0x013F0000; pixel.window = 0x3F; }
    }
    DiagnosticOutput result{};
    for (u32 screen = 0; screen < 2; ++screen)
    {
        std::fill(output.begin(), output.end(), 0xFF125600 ^ (screen << 20));
        std::vector<u32> expected(output.begin(), output.end());
        Vulkan::ComposeDisplayCPU(lines, rgb6, scale, scale, expected);
        compositor.Compose(screen, lines, input, scale, output, memory.get());
        Equal(output, expected, "diagnostic path changed immediate CPU-readable screen");
        result.pixels.insert(result.pixels.end(), output.begin(), output.end());
    }
    Require(!probe.invalid, "diagnostics waited for queries or read them before the existing fence completed");
    if (device->Costs())
    {
        auto& cost = *device->Costs();
        cost.End(RenderCostNowNs());
        if (failure == DiagnosticProbe::UnreadyResults)
        {
            u64 observed = 0, dropped = 0;
            for (u64 count : cost.Sum().GpuCalls) observed += count;
            for (u64 count : cost.Sum().GpuDropped) dropped += count;
            Require(!observed && dropped && std::strcmp(cost.GpuState, "on"),
                "unavailable GPU samples were reported as zero-time observations");
        }
    }
    result.uploads = probe.uploads; result.downloads = probe.downloads;
    result.submits = probe.submits; result.waits = probe.waits; result.idles = probe.idles;
    result.pools = probe.pools; result.timestamps = probe.timestamps; result.results = probe.results;
    result.timestampSupport = device->Properties().limits.timestampComputeAndGraphics &&
        device->Properties().limits.timestampPeriod > 0;
    std::printf("DIAG enabled=%d failure=%d submits=%llu waits=%llu idle=%llu upload_bytes=%llu download_bytes=%llu pools=%llu timestamps=%llu results=%llu\n",
        enabled, int(failure), result.submits, result.waits, result.idles, result.uploads, result.downloads,
        result.pools, result.timestamps, result.results);
    return result;
}
void DiagnosticsContract()
{
    const auto off = DiagnosticExercise(false, DiagnosticProbe::None);
    Require(!off.pools && !off.timestamps && !off.results, "diagnostics OFF issued GPU queries");
    for (auto failure : {DiagnosticProbe::None, DiagnosticProbe::PoolFailure,
        DiagnosticProbe::ResultFailure, DiagnosticProbe::MissingTimestamp, DiagnosticProbe::UnreadyResults})
    {
        const auto on = DiagnosticExercise(true, failure);
        Equal(on.pixels, off.pixels, "diagnostics OFF/ON two-screen identity");
        Require(on.submits == off.submits && on.waits == off.waits && on.idles == off.idles &&
            on.uploads == off.uploads && on.downloads == off.downloads,
            "diagnostics changed real submissions, waits or image-transfer bytes");
        if (failure == DiagnosticProbe::None && on.timestampSupport)
            Require(on.pools && on.timestamps && on.results, "supported diagnostics ON has no GPU observations");
        if (failure == DiagnosticProbe::MissingTimestamp)
            Require(!on.timestamps, "missing timestamp support was not tolerated");
    }
    std::printf("Diagnostics OFF/ON/failure: %zu two-screen pixels per comparison, identical; no extra submissions/transfers/waits PASS\n", off.pixels.size());
}
using Clock = std::chrono::steady_clock;
double Milliseconds(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
double Quantile(std::vector<double> samples, double fraction)
{
    std::sort(samples.begin(), samples.end());
    // Match RenderCost's existing empirical ranks, including even-sized cohorts.
    return samples[std::min(samples.size() - 1, size_t(samples.size() * fraction))];
}
u64 Digest(std::span<const u32> pixels)
{
    u64 result = 1469598103934665603ull;
    for (u32 pixel : pixels) { result ^= pixel; result *= 1099511628211ull; }
    return result;
}
void BenchmarkCompose(const std::shared_ptr<Vulkan::Device>& device, int scale, int stride, bool directPath)
{
    const size_t row = size_t(256) * scale * scale, count = row * 192;
    std::vector<u32> rgba(count, 0xFF6C98DC), rgb6(count, 0x1F1B2637);
    auto input = Upload(device, rgba, scale);
    Vulkan::DisplayCompositor compositor(device, Vulkan::EmbeddedDisplayCompose(), scale);
    std::vector<Line> lines(192);
    std::vector<u32> storage(count, 0xA537C2E1), expected(storage);
    auto memory = directPath ? device->CreateBuffer(count * sizeof(u32), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        true, 0, VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : nullptr;
    std::span<u32> output = memory ? std::span<u32>{static_cast<u32*>(memory->Data()), count} : std::span<u32>(storage);
    std::copy(expected.begin(), expected.end(), output.begin());
    unsigned composed = 0;
    for (u32 y = 0; y < 192; ++y)
    {
        auto& line = lines[y];
        line.mode = y % stride == 0 ? Line::Composite3D : (y % 2 ? Line::CaptureOverride : Line::Keep);
        line.sourceLine = y;
        for (auto& pixel : line.pixels) { pixel.top = 0x40000000; pixel.second = 0x013F0000; pixel.window = 0x3F; }
        composed += line.mode == Line::Composite3D;
    }
    Vulkan::ComposeDisplayCPU(lines, rgb6, scale, scale, expected);
    CopyProbe probe(*device);
    std::vector<double> samples;
    for (int i = -5; i < 31; ++i)
    {
        if (i == 0 && device->Costs()) device->Costs()->Reset();
        const auto start = Clock::now();
        {
            RenderCostVulkanFrame interval(device->Costs());
            compositor.Compose(0, lines, input, scale, output, memory.get());
        }
        const double elapsed = Milliseconds(start);
        if (i >= 0) samples.push_back(elapsed);
    }
    Equal(output, expected, "benchmark composition output");
    Require(!directPath || !compositor.readback, "direct benchmark allocated staging");
    std::printf("BENCH compose path=%s scale=%d stride=%d samples=%zu median_ms=%.6f p95_ms=%.6f host_copy_bytes=%llu transfer_bytes=%llu digest=%016llx\n",
        directPath ? "direct" : "staging", scale, stride, samples.size(), Quantile(samples, .5), Quantile(samples, .95),
        static_cast<unsigned long long>(directPath ? 0 : composed * row * 4), static_cast<unsigned long long>(probe.bytes / 36),
        static_cast<unsigned long long>(Digest(output)));
    if (auto* cost = device->Costs())
    {
        char report[8192], label[96];
        std::snprintf(label, sizeof(label), "compose-%s-s%d-stride%d", directPath ? "direct" : "staging", scale, stride);
        cost->Report(report, sizeof(report), label);
        std::puts(report);
        cost->Reset();
    }
}
void PrepareRunFrame(NDS* nds, int workload)
{
    nds->ARM9Write32(0x02000000, 0xEAFFFFFE);
    nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds->ARM9.JumpTo(0x02000000); nds->ARM7.JumpTo(0x02000200);
    nds->ARM9Write32(0x04000350, (31u << 16) | 0x3E0);
    nds->ARM9Write32(0x04000354, 0x7FFF);
    if (workload == 1) nds->ARM9Write32(0x04000000, 0x00010000);
    if (workload >= 2)
    {
        nds->ARM9Write8(0x04000241, 0x80);
        nds->ARM9Write32(0x04000000, 0x00060000);
        // Additional source-B cases use generated guest data, never a ROM/save.
        if (workload >= 4)
            for (u32 word = 0; word < 65536; ++word)
                nds->ARM9Write16(0x06820000 + word * 2, u16(word * 73 + 0x801F));
    }
    nds->Start();
}
DiagnosticOutput DiagnosticRunFrame(bool enabled, int scale, int workload)
{
    DiagnosticEnvironment environment(enabled);
    auto nds = Console(scale);
    auto& renderer = static_cast<VulkanRenderer&>(nds->GetRenderer());
    auto& raster = RendererAccess::Rasterizer(renderer);
    DiagnosticProbe probe(*raster.Device, DiagnosticProbe::None);
    PrepareRunFrame(nds.get(), workload);
    u64 generation = 0;
    Renderer::DisplayFrame frame;
    for (int i = -10; i < 4; ++i)
    {
        nds->GPU.GPU3D.RenderFrameIdentical = false;
        raster.FrameDirty = true;
        if (workload == 2) nds->ARM9Write32(0x04000064, 0x81310000);
        if (i == 0 && renderer.Costs()) renderer.Costs()->Reset();
        u32 lines;
        {
            RenderCostVulkanFrame interval(renderer.Costs());
            lines = nds->RunFrame();
        }
        Require(lines > 0 && !renderer.HasRenderFailure(), "diagnostic RunFrame failed");
        Require(renderer.GetDisplayFrame(frame) && frame.kind == Renderer::DisplayFrame::Kind::CpuBGRA &&
            frame.top && frame.bottom && frame.generation > generation &&
            frame.width == u32(256 * scale) && frame.height == u32(192 * scale),
            "diagnostics changed completed CPU publication or generation");
        generation = frame.generation;
        Renderer::DisplayFrame repeated;
        Require(renderer.GetDisplayFrame(repeated) && repeated.generation == frame.generation &&
            repeated.top == frame.top && repeated.bottom == frame.bottom,
            "diagnostic paused/repeated query changed publication");
        if (i >= 0 && renderer.Costs())
        {
            using Cost = RenderCostVulkanMeter;
            const auto& sample = renderer.Costs()->Last();
            u64 accounted = 0;
            for (u64 ns : sample.HostNs) accounted += ns;
            Require(accounted == sample.Total, "real RunFrame host accounting overlaps or loses its residual");
            const u64 expectedFull = (scale == 1 || workload == 2) ? u64(256) * 192 * scale * scale * 4 : 0;
            const auto readbackProperties = raster.Pipeline->readback->MemoryProperties();
            const u64 expectedFullCopy = (readbackProperties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 0 : expectedFull;
            if (i == 0)
                std::printf("DIAG FullCopy scale=%d workload=%d properties=0x%08x expected_bytes=%llu actual_bytes=%llu\n",
                    scale, workload, unsigned(readbackProperties),
                    static_cast<unsigned long long>(expectedFullCopy),
                    static_cast<unsigned long long>(sample.Bytes[Cost::FullCopyBytes]));
            Require(sample.Bytes[Cost::FullCopyBytes] == expectedFullCopy &&
                sample.Events[Cost::FullCopyBytes] == unsigned(expectedFullCopy != 0) &&
                sample.HostCalls[Cost::FullCopy] == unsigned(expectedFullCopy != 0) &&
                (expectedFullCopy != 0 || sample.HostNs[Cost::FullCopy] == 0),
                "full-copy accounting does not match actual landing-buffer memory properties");
            const u64 expectedNative = scale == 1 ? 0 : 256 * 192 * 4;
            const u64 expectedDisplay = scale > 1 && workload == 0 ? u64(256) * 192 * scale * scale * 4 : 0;
            Require(sample.Bytes[Cost::FullReadbackBytes] == expectedFull &&
                sample.Events[Cost::FullReadbackBytes] == unsigned(expectedFull != 0) &&
                sample.Bytes[Cost::NativeReadbackBytes] == expectedNative &&
                sample.Bytes[Cost::NativeCopyBytes] == expectedNative &&
                sample.Bytes[Cost::DisplayReadbackBytes] == expectedDisplay &&
                !sample.Bytes[Cost::DisplayCopyBytes] && !sample.Events[Cost::OverrideUploadBytes],
                "RunFrame traffic accounting conflates native/full/display/override paths");
            if (workload == 2)
                Require(sample.HostCalls[Cost::Capture] == 192 && sample.HostCalls[Cost::LCDC] == 192 &&
                    sample.HostCalls[Cost::ScaledConvert] == 1 &&
                    sample.Bytes[Cost::CaptureCopyBytes] == u64(256) * 192 * scale * scale * 2,
                    "source-A 3D capture/LCDC accounting lost lazy conversion or scanlines");
        }
    }
    DiagnosticOutput result{};
    const size_t pixels = size_t(frame.width) * frame.height;
    const auto* top = static_cast<const u32*>(frame.top);
    const auto* bottom = static_cast<const u32*>(frame.bottom);
    result.pixels.assign(top, top + pixels);
    result.pixels.insert(result.pixels.end(), bottom, bottom + pixels);
    void* nativeTop = nullptr; void* nativeBottom = nullptr;
    Require(renderer.GetFramebuffers(&nativeTop, &nativeBottom), "diagnostics lost guest-native framebuffers");
    result.pixels.insert(result.pixels.end(), static_cast<const u32*>(nativeTop), static_cast<const u32*>(nativeTop) + 256 * 192);
    result.pixels.insert(result.pixels.end(), static_cast<const u32*>(nativeBottom), static_cast<const u32*>(nativeBottom) + 256 * 192);
    result.uploads = probe.uploads; result.downloads = probe.downloads;
    result.submits = probe.submits; result.waits = probe.waits; result.idles = probe.idles;
    result.pools = probe.pools; result.timestamps = probe.timestamps; result.results = probe.results;
    Require(!probe.invalid, "RunFrame query retrieval added a wait or preceded fence completion");
    if (!enabled) Require(!result.pools && !result.timestamps && !result.results && !renderer.Costs(),
        "diagnostics OFF allocated a meter or issued queries");
    std::printf("DIAG RunFrame enabled=%d scale=%d workload=%d top=%016llx bottom=%016llx submits=%llu waits=%llu upload_bytes=%llu download_bytes=%llu\n",
        enabled, scale, workload, static_cast<unsigned long long>(Digest({top, pixels})),
        static_cast<unsigned long long>(Digest({bottom, pixels})), result.submits, result.waits, result.uploads, result.downloads);
    if (renderer.Costs()) renderer.Costs()->Reset();
    return result;
}
void DiagnosticsRunFrames()
{
    size_t compared = 0;
    for (const auto [scale, workload] : {std::pair{1, 0}, {3, 0}, {5, 0}, {8, 0}, {16, 0}, {16, 1}, {16, 2}})
    {
        const auto off = DiagnosticRunFrame(false, scale, workload);
        const auto on = DiagnosticRunFrame(true, scale, workload);
        Equal(on.pixels, off.pixels, "RunFrame diagnostics OFF/ON two-screen/native byte identity");
        Require(on.submits == off.submits && on.waits == off.waits && on.idles == off.idles &&
            on.uploads == off.uploads && on.downloads == off.downloads,
            "RunFrame diagnostics changed physical submissions/transfers/waits");
        compared += off.pixels.size();
    }
    std::printf("Diagnostics RunFrame: %zu display/native pixel comparisons, unchanged physical submissions/transfers/waits; aligned host and traffic accounting PASS\n", compared);
}
void BenchmarkRunFrame(int scale, int workload, bool vectorPath)
{
    auto nds = Console(scale, vectorPath);
    auto& renderer = static_cast<VulkanRenderer&>(nds->GetRenderer());
    auto& raster = RendererAccess::Rasterizer(renderer);
    DiagnosticProbe probe(*raster.Device, DiagnosticProbe::None);
    PrepareRunFrame(nds.get(), workload);
    // Preserve workload 2 exactly; other controls exercise the unchanged
    // composed-A, same-bank B, blended, and FIFO capture paths.
    constexpr u32 controls[] = {0, 0, 0x81310000, 0x80310000,
        0xA0310000, 0xC1310808, 0xA2310000, 0xC3310808};
    constexpr const char* names[] = {"3D", "2D-only", "capture-heavy",
        "capture-composed-A", "capture-B-VRAM", "capture-blend-VRAM",
        "capture-B-FIFO", "capture-blend-FIFO"};
    std::vector<double> samples;
    u64 totalBefore = 0, rasterBefore = 0;
    Renderer::DisplayFrame frame;
    for (int i = -10; i < 60; ++i)
    {
        nds->GPU.GPU3D.RenderFrameIdentical = false;
        raster.FrameDirty = true;
        if (workload >= 2) nds->ARM9Write32(0x04000064, controls[workload]);
        if (i == 0)
        {
            totalBefore = renderer.TotalSubmissionCount(); rasterBefore = renderer.SubmissionCount();
            probe.uploads = probe.downloads = probe.submits = probe.waits = probe.idles = 0;
            if (renderer.Costs()) renderer.Costs()->Reset();
        }
        const auto start = Clock::now();
        u32 drawn;
        {
            RenderCostVulkanFrame interval(renderer.Costs());
            drawn = nds->RunFrame();
        }
        Require(drawn > 0, "generated RunFrame did not run");
        const double elapsed = Milliseconds(start);
        Require(renderer.GetDisplayFrame(frame) && frame.kind == Renderer::DisplayFrame::Kind::CpuBGRA &&
            !renderer.HasRenderFailure(), "generated RunFrame publication failed");
        // Immediate CPU access is outside timing, never a deferred completion call.
        Require(static_cast<const u32*>(frame.top) != nullptr && static_cast<const u32*>(frame.bottom) != nullptr,
            "generated frame is not immediately CPU-readable");
        if (i >= 0)
        {
            samples.push_back(elapsed);
            if (renderer.Costs())
            {
                const auto& sample = renderer.Costs()->Last();
                u64 accounted = 0;
                for (u64 ns : sample.HostNs) accounted += ns;
                Require(accounted == sample.Total, "capture benchmark host stages lost exact reconciliation");
            }
        }
    }
    Require(!probe.invalid, "capture benchmark queries added a wait or preceded fence completion");
    const size_t pixels = size_t(frame.width) * frame.height;
    const u64 digest = Digest({static_cast<const u32*>(frame.top), pixels}) ^ Digest({static_cast<const u32*>(frame.bottom), pixels});
    std::printf("BENCH RunFrame path=%s scale=%d workload=%s samples=%zu median_ms=%.6f p95_ms=%.6f raster_submits=%llu total_submits=%llu digest=%016llx\n",
        vectorPath ? "vector" : "direct", scale, names[workload], samples.size(),
        Quantile(samples, .5), Quantile(samples, .95),
        static_cast<unsigned long long>(renderer.SubmissionCount() - rasterBefore),
        static_cast<unsigned long long>(renderer.TotalSubmissionCount() - totalBefore), static_cast<unsigned long long>(digest));
    std::printf("BENCH physical submits=%llu waits=%llu idles=%llu upload_bytes=%llu download_bytes=%llu\n",
        probe.submits, probe.waits, probe.idles, probe.uploads, probe.downloads);
    if (workload >= 2)
    {
        std::printf("BENCH capture control=%08x top=%016llx bottom=%016llx native_bank1=%016llx native_bytes=131072\n",
            controls[workload], static_cast<unsigned long long>(Digest({static_cast<const u32*>(frame.top), pixels})),
            static_cast<unsigned long long>(Digest({static_cast<const u32*>(frame.bottom), pixels})),
            static_cast<unsigned long long>(Digest({reinterpret_cast<const u32*>(nds->GPU.VRAM[1]), 131072 / 4})));
        const auto& capture = renderer.DisplayCaptures[4]; // All benchmark controls target bank B, start 0.
        u64 sidecar = 1469598103934665603ull;
        for (u16 pixel : capture.pixels) { sidecar ^= pixel; sidecar *= 1099511628211ull; }
        std::printf("BENCH sidecar words=%zu digest=%016llx valid_rows=%zu\n", capture.pixels.size(),
            static_cast<unsigned long long>(sidecar), size_t(std::count(capture.valid.begin(), capture.valid.end(), true)));
    }
    if (auto* cost = renderer.Costs())
    {
        char report[8192], label[96];
        std::snprintf(label, sizeof(label), "RunFrame-%s-s%d-w%d", vectorPath ? "vector" : "direct", scale, workload);
        cost->Report(report, sizeof(report), label);
        std::puts(report);
        cost->Reset();
    }
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 4 && std::strcmp(argv[1], "benchmark-capture") == 0)
        {
            const int scale = std::atoi(argv[2]), workload = std::atoi(argv[3]);
            Require(scale == 1 || scale == 3 || scale == 5 || scale == 8 || scale == 16,
                "capture benchmark scale must be 1/3/5/8/16");
            Require(workload >= 2 && workload <= 7, "capture benchmark workload must be 2..7");
            BenchmarkRunFrame(scale, workload, false);
            return 0;
        }
        if (argc > 1 && std::strcmp(argv[1], "diagnostics") == 0)
        {
            DiagnosticsContract();
            return 0;
        }
        if (argc > 1 && std::strcmp(argv[1], "diagnostics-frames") == 0)
        {
            DiagnosticsRunFrames();
            return 0;
        }
        if (argc > 1 && std::strcmp(argv[1], "direct") == 0)
        {
            auto nds = Console();
            IntegrationFrame(*nds, false, false);
            Require(!RendererAccess::Rasterizer(nds->GetRenderer()).Compositor->readback,
                "renderer composition still allocates intermediate display staging");
            AllocationFallback();
            BackingLifecycle();
            return 0;
        }
        if (argc > 1 && (std::strcmp(argv[1], "benchmark") == 0 || std::strcmp(argv[1], "benchmark-staging") == 0))
        {
            const bool directPath = std::strcmp(argv[1], "benchmark") == 0;
            std::string error;
            auto device = Vulkan::Device::Create(error);
            if (!device) throw std::runtime_error(error);
            std::printf("BENCH device=%s warmup_compose=5 warmup_RunFrame=10\n", device->Properties().deviceName);
            BenchmarkCompose(device, 3, 1, directPath);
            for (int stride : {1, 4, 16}) BenchmarkCompose(device, 16, stride, directPath);
            for (int scale : {1, 3, 5, 8, 16}) BenchmarkRunFrame(scale, 0, !directPath);
            BenchmarkRunFrame(16, 1, !directPath); BenchmarkRunFrame(16, 2, !directPath);
            return 0;
        }
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
