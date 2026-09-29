// SPDX-License-Identifier: GPL-3.0-or-later
#include "DisplayCompositor.h"
#include "RenderCost.h"
#include "GPU_ColorOp.h"
#include "PixelConvert.h"
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <stdexcept>

namespace melonDS::Vulkan {
namespace {
using Line = SoftRenderer2D::ScaledLineContext;
using Cost = RenderCostVulkanMeter;
constexpr u32 MaxGroupRows = 384;
static_assert(sizeof(Line::Pixel) == 5 * sizeof(u32));
static_assert(sizeof(Line) == (256 * 5 + 9) * sizeof(u32));
static_assert(offsetof(Line, blendCnt) == 256 * sizeof(Line::Pixel));
bool Preserved(const Line& line)
{
    return line.mode == Line::Keep || line.mode == Line::CaptureOverride;
}
void CheckExtents(std::span<const Line> lines, u32 sourceScale, u32 scale, size_t pixels)
{
    if (lines.size() != 192 || !scale || scale > 16 || !sourceScale || sourceScale > 16 ||
        pixels != size_t(256) * 192 * scale * scale)
        throw std::invalid_argument("Invalid display composition extent");
}
u32 Composite(u32 top, u32 second, const Line& line, u32 window)
{
    const u32 effect = (line.blendCnt >> 6) & 3;
    u32 flag = top >> 24;
    if (effect == 1 && !(flag & 0xC0) && (!(line.blendCnt & flag) || !(window & 0x20))) return top;
    if (effect == 1 || (flag & 0xC0))
    {
        const u32 flag2 = second >> 24;
        const u32 target2 = (flag2 & 0x80) ? 0x1000 : (flag2 & 0x40) ? 0x100 : flag2 << 8;
        if (line.blendCnt & target2)
        {
            if ((flag & 0xC0) == 0x40) return ColorBlend5(top, second);
            if ((flag & 0xC0) == 0xC0) return ColorBlend4(top, second, flag & 31, 16 - (flag & 31));
            return ColorBlend4(top, second, line.eva, line.evb);
        }
    }
    if (effect >= 2)
    {
        if (flag & 0x80) flag = 0x10;
        else if (flag & 0x40) flag = 1;
        if ((line.blendCnt & flag) && (window & 0x20))
            return effect == 2 ? ColorBrightnessUp(top, line.evy, 8) : ColorBrightnessDown(top, line.evy, 7);
    }
    return top;
}
void ImageBarrier(const volk::VolkDeviceTable& f, VkCommandBuffer command, VkImage image,
    VkImageLayout before, VkImageLayout after, VkPipelineStageFlags source, VkPipelineStageFlags dest,
    VkAccessFlags read, VkAccessFlags write)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = image; barrier.oldLayout = before; barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask = read; barrier.dstAccessMask = write;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdPipelineBarrier(command, source, dest, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
}

void ComposeDisplayCPU(std::span<const Line> lines, std::span<const u32> pixels3D,
    u32 sourceScale, u32 scale, std::span<u32> destination)
{
    CheckExtents(lines, sourceScale, scale, destination.size());
    if (pixels3D.size() != size_t(256) * 192 * sourceScale * sourceScale)
        throw std::invalid_argument("Invalid fallback 3D extent");
    const u32 width = 256 * scale;
    for (u32 y = 0; y < 192; ++y)
    {
        const auto& line = lines[y];
        if (Preserved(line)) continue;
        for (u32 sub = 0; sub < scale; ++sub)
        {
            auto* dst = destination.data() + (size_t(y) * scale + sub) * width;
            if (line.mode != Line::Composite3D)
            {
                for (u32 x = 0; x < 256; ++x) std::fill_n(dst + x * scale, scale, line.pixels[x].top);
                continue;
            }
            const u32 sy = (line.sourceLine * scale + sub) * sourceScale / scale;
            for (u32 x = 0; x < width; ++x)
            {
                const auto& pixel = line.pixels[x / scale];
                const u32 sx = (x * sourceScale / scale + (line.xpos & 511) * sourceScale) % (512 * sourceScale);
                const u32 color = !line.abort && line.sourceLine < 192 && sx < 256 * sourceScale ?
                    pixels3D[size_t(sy) * 256 * sourceScale + sx] : 0;
                u32 top = pixel.top, second = pixel.second;
                if (((top >> 24) & 0xC0) == 0x40)
                {
                    if (color >> 24) top = color | 0x40000000;
                    else { top = pixel.belowTop; second = pixel.belowSecond; }
                }
                else if (((second >> 24) & 0xC0) == 0x40)
                    second = color >> 24 ? color | 0x40000000 : pixel.belowTop;
                dst[x] = Composite(top, second, line, pixel.window);
                const u32 factor = std::min(line.masterBrightness & 31, 16u);
                if ((line.masterBrightness >> 14) == 1) dst[x] = ColorBrightnessUp(dst[x], factor, 0);
                else if ((line.masterBrightness >> 14) == 2) dst[x] = ColorBrightnessDown(dst[x], factor, 15);
            }
            PixelConvert::ExpandScalar(dst, width);
        }
    }
}

DisplayCompositor::DisplayCompositor(std::shared_ptr<Device> device, std::span<const u32> shader, u32 scale, u32 buffers)
    : owner(std::move(device)), f(owner->Functions()), device(owner->Handle()), scale(scale),
      outputs(2 * buffers), residentValid(2 * buffers)
{
    if (!scale || scale > 16 || !buffers || buffers > 2 || shader.empty()) throw std::invalid_argument("Invalid display compositor configuration");
    try { Init(shader); } catch (...) { Cleanup(); throw; }
}
DisplayCompositor::~DisplayCompositor() { Cleanup(); }
void DisplayCompositor::Cleanup()
{
    // [observed] WaitForSubmission drains on failure before resources are freed.
    try { Complete(); } catch (...) {}
    if (pipeline) f.vkDestroyPipeline(device, pipeline, nullptr);
    if (layout) f.vkDestroyPipelineLayout(device, layout, nullptr);
    if (pool) f.vkDestroyDescriptorPool(device, pool, nullptr);
    if (bindings) f.vkDestroyDescriptorSetLayout(device, bindings, nullptr);
}
void DisplayCompositor::Init(std::span<const u32> shader)
{
    scratch[0].contexts = owner->CreateBuffer(sizeof(Line) * 192 + MaxGroupRows * sizeof(u32),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    for (auto& image : outputs)
        image = owner->CreateImage(256 * scale, 192 * scale, 1, VK_FORMAT_R32_UINT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    blank3D = owner->CreateImage(256, 192, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const VkDescriptorSetLayoutBinding entries[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo bindingInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    bindingInfo.bindingCount = 3; bindingInfo.pBindings = entries;
    Device::Check(f.vkCreateDescriptorSetLayout(device, &bindingInfo, nullptr, &bindings), "Create display bindings");
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 3 * sizeof(u32)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1; layoutInfo.pSetLayouts = &bindings;
    layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &push;
    Device::Check(f.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "Create display layout");
    const auto cache = owner->GetPipelineCache();
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = shader.size_bytes(); moduleInfo.pCode = shader.data();
    VkShaderModule module{};
    Device::Check(f.vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "Create display shader");
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.layout = layout;
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
        VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    const auto result = f.vkCreateComputePipelines(device, cache, 1, &pipelineInfo, nullptr, &pipeline);
    f.vkDestroyShaderModule(device, module, nullptr);
    Device::Check(result, "Create display pipeline");
    owner->TrimPipelineCache();
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 2; poolInfo.poolSizeCount = 2; poolInfo.pPoolSizes = sizes;
    Device::Check(f.vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "Create display descriptor pool");
    const VkDescriptorSetLayout setLayouts[] = {bindings, bindings};
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = pool; allocation.descriptorSetCount = 2; allocation.pSetLayouts = setLayouts;
    VkDescriptorSet sets[2]{};
    Device::Check(f.vkAllocateDescriptorSets(device, &allocation, sets), "Allocate display descriptors");
    scratch[0].descriptors = sets[0]; scratch[1].descriptors = sets[1];
    VkDescriptorBufferInfo bufferInfo{scratch[0].contexts->Handle(), 0, scratch[0].contexts->Size()};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = scratch[0].descriptors; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &bufferInfo;
    f.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    const auto command = owner->Begin();
    for (const auto& image : outputs)
        ImageBarrier(f, command, image->Handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_ACCESS_SHADER_WRITE_BIT);
    ImageBarrier(f, command, blank3D->Handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue zero{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdClearColorImage(command, blank3D->Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    ImageBarrier(f, command, blank3D->Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    owner->SubmitAndWait();
}
void DisplayCompositor::Compose(u32 screen, std::span<const Line> lines,
    const std::shared_ptr<Device::Image>& image3D, u32 sourceScale, std::span<u32> destination,
    const Device::Buffer* direct, bool deferred)
{
    ComposeImpl(screen, lines, image3D, sourceScale, destination, direct, false, {}, deferred);
}

std::shared_ptr<Device::Image> DisplayCompositor::ComposeResident(u32 screen, std::span<const Line> lines,
    const std::shared_ptr<Device::Image>& image3D, u32 sourceScale, std::span<u32> cpuRows,
    std::span<const bool> changedRows, bool deferred)
{
    ComposeImpl(screen, lines, image3D, sourceScale, cpuRows, nullptr, true, changedRows, deferred);
    return outputs[screen];
}

void DisplayCompositor::ReadbackResident(u32 screen, std::span<u32> destination)
{
    Complete();
    if (screen >= outputs.size() || !residentValid[screen] ||
        destination.size() != size_t(256) * 192 * scale * scale)
        throw std::invalid_argument("No complete resident display at the requested extent");
    if (!readback) readback = owner->CreateBuffer(destination.size_bytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        true, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    const auto command = owner->Begin(Device::SubmitKind::Display);
    ImageBarrier(f, command, outputs[screen]->Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {256 * scale, 192 * scale, 1};
    f.vkCmdCopyImageToBuffer(command, outputs[screen]->Handle(), VK_IMAGE_LAYOUT_GENERAL, readback->Handle(), 1, &copy);
    VkMemoryBarrier download{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    download.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; download.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    f.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &download, 0, nullptr, 0, nullptr);
    owner->Timestamp(Device::TimestampStage::DisplayReadback);
    if (owner->Costs()) owner->Costs()->Transfer(Cost::DisplayReadbackBytes, destination.size_bytes(), 192);
    owner->SubmitAndWait();
    RenderCostVulkanScope cost(owner->Costs(), Cost::DisplayCopy);
    std::memcpy(destination.data(), readback->Data(), destination.size_bytes());
    if (owner->Costs()) owner->Costs()->Transfer(Cost::DisplayCopyBytes, destination.size_bytes());
}

struct DisplayCompositor::Work {
    u32 overrideCount = 0, activeGroups = 0, compactRows = 0, set = 0, screen = 0;
    std::array<VkBufferImageCopy, 192> overrideCopies{};
    std::array<u32, MaxGroupRows> groupRows{};
};

void DisplayCompositor::Validate(u32 screen, std::span<const Line> lines, u32 sourceScale,
    std::span<u32> destination, std::span<const bool> changedRows,
    const std::shared_ptr<Device::Image>& image3D, const Device::Buffer* direct) const
{
    CheckExtents(lines, sourceScale, scale, destination.size());
    constexpr VkMemoryPropertyFlags directProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    if (direct && (!direct->BelongsTo(*owner) || direct->Size() != destination.size_bytes() ||
        direct->Data() != destination.data() || !(direct->Usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        (direct->MemoryProperties() & directProperties) != directProperties))
        throw std::invalid_argument("Invalid direct display backing");
    if (screen >= outputs.size() || (image3D && !image3D->BelongsTo(*owner)))
        throw std::invalid_argument("Invalid display composition image");
    if (!changedRows.empty() && changedRows.size() != 192)
        throw std::invalid_argument("Invalid display row mask");
}

// Creates every lazily allocated resource a scratch set needs. Runs before any
// resident-valid changes or recording so allocation failures preserve prior images.
void DisplayCompositor::Reserve(u32 set, u32 screen, std::span<const Line> lines,
    std::span<const bool> changedRows, bool resident, size_t bytes)
{
    auto& s = scratch[set];
    if (!s.contexts)
    {
        auto buffer = owner->CreateBuffer(sizeof(Line) * 192 + MaxGroupRows * sizeof(u32),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        VkDescriptorBufferInfo bufferInfo{buffer->Handle(), 0, buffer->Size()};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = s.descriptors; write.dstBinding = 0; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &bufferInfo;
        f.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        s.contexts = std::move(buffer);
    }
    if (resident && !s.overrides)
    {
        const bool preserve = residentValid[screen] && !changedRows.empty();
        bool needed = false;
        for (u32 y = 0; y < 192 && !needed; ++y)
            needed = Preserved(lines[y]) && (!preserve || changedRows[y]);
        if (needed) s.overrides = owner->CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    }
}

// Host-side preparation only: sparse override upload, group compaction, context
// copy, descriptor update. Requires Reserve, and the previous submission complete.
void DisplayCompositor::Prepare(Work& work, u32 set, u32 screen, std::span<const Line> lines,
    std::span<u32> destination, std::span<const bool> changedRows, bool resident,
    const std::shared_ptr<Device::Image>& input)
{
    auto& s = scratch[set];
    work.set = set; work.screen = screen;
    const bool preserve = residentValid[screen] && !changedRows.empty();
    residentValid[screen] = false;
    if (resident)
    {
        for (u32 y = 0; y < 192;)
        {
            const auto upload = [&](u32 row) {
                return Preserved(lines[row]) && (!preserve || changedRows[row]);
            };
            if (!upload(y)) { ++y; continue; }
            const u32 first = y++;
            while (y < 192 && upload(y)) ++y;
            const size_t offset = size_t(first) * 256 * scale * scale * sizeof(u32);
            const size_t bytes = size_t(y - first) * 256 * scale * scale * sizeof(u32);
            std::memcpy(static_cast<char*>(s.overrides->Data()) + offset,
                reinterpret_cast<const char*>(destination.data()) + offset, bytes);
            auto& region = work.overrideCopies[work.overrideCount++];
            region.bufferOffset = offset; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset.y = first * scale; region.imageExtent = {256 * scale, (y - first) * scale, 1};
            if (owner->Costs()) owner->Costs()->Transfer(Cost::OverrideUploadBytes, bytes);
        }
    }
    // Compaction: dispatch one 8-row workgroup band only when some guest line
    // inside it is composed. A full map is skipped so the shader stays on the
    // untranslated GlobalInvocationID path.
    const u32 groupCount = 24 * scale;
    for (u32 g = 0; g < groupCount; ++g)
    {
        const u32 first = g * 8 / scale, last = ((g + 1) * 8 - 1) / scale;
        bool active = false;
        for (u32 y = first; y <= last; ++y) active |= !Preserved(lines[y]);
        if (active) work.groupRows[work.activeGroups++] = g;
    }
    work.compactRows = work.activeGroups != groupCount;
    if (!work.activeGroups) return;
    {
        RenderCostVulkanScope copy(owner->Costs(), Cost::ContextCopy);
        auto* data = static_cast<char*>(s.contexts->Data());
        size_t copied = 0;
        // Preserved lines can share a dispatched band with composed lines. Only
        // their mode is consumed before the shader returns; never copy pixels.
        // Coalesce all other contexts so a full frame remains one bulk copy.
        for (u32 y = 0; y < 192;)
        {
            if (Preserved(lines[y]))
            {
                std::memcpy(data + sizeof(Line) * y + offsetof(Line, mode),
                    &lines[y].mode, sizeof(lines[y].mode));
                copied += sizeof(lines[y].mode);
                ++y;
                continue;
            }
            const u32 first = y++;
            while (y < 192 && !Preserved(lines[y])) ++y;
            const size_t bytes = sizeof(Line) * (y - first);
            std::memcpy(data + sizeof(Line) * first, lines.data() + first, bytes);
            copied += bytes;
        }
        const u32 mapBytes = work.compactRows ? work.activeGroups * sizeof(u32) : 0;
        if (mapBytes)
            std::memcpy(data + sizeof(Line) * 192, work.groupRows.data(), mapBytes);
        if (owner->Costs()) owner->Costs()->Transfer(Cost::ContextCopyBytes, copied + mapBytes);
    }
    const VkDescriptorImageInfo images[] = {{VK_NULL_HANDLE, input->View(), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, outputs[screen]->View(), VK_IMAGE_LAYOUT_GENERAL}};
    for (u32 i = 0; i < 2; ++i)
    {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = s.descriptors; write.dstBinding = i + 1; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; write.pImageInfo = &images[i];
        f.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}

void DisplayCompositor::Record(VkCommandBuffer command, const Work& work,
    const std::shared_ptr<Device::Image>& input, u32 sourceScale)
{
    const u32 screen = work.screen;
    if (work.overrideCount)
    {
        ImageBarrier(f, command, outputs[screen]->Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        f.vkCmdCopyBufferToImage(command, scratch[work.set].overrides->Handle(), outputs[screen]->Handle(),
            VK_IMAGE_LAYOUT_GENERAL, work.overrideCount, work.overrideCopies.data());
    }
    if (!work.activeGroups) return;
    VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    upload.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; upload.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    f.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &upload, 0, nullptr, 0, nullptr);
    ImageBarrier(f, command, input->Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    ImageBarrier(f, command, outputs[screen]->Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    f.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    f.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
        &scratch[work.set].descriptors, 0, nullptr);
    const u32 settings[] = {scale, sourceScale, work.compactRows};
    f.vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), settings);
    f.vkCmdDispatch(command, 32 * scale, work.activeGroups, 1);
}

void DisplayCompositor::RecordResidentTail(VkCommandBuffer command, u32 screen, std::span<const Line> lines)
{
    ImageBarrier(f, command, outputs[screen]->Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    if (owner->Costs()) owner->Costs()->Transfer(Cost::ComposedRows, 0,
        std::count_if(lines.begin(), lines.end(), [](const Line& line) { return !Preserved(line); }));
}

std::array<std::shared_ptr<Device::Image>, 2> DisplayCompositor::ComposeResidentPair(
    const std::array<ResidentRequest, 2>& requests, const std::shared_ptr<Device::Image>& image3D,
    u32 sourceScale, bool deferred)
{
    if (requests[0].screen == requests[1].screen) throw std::invalid_argument("Duplicate display pair screen");
    for (const auto& request : requests)
        Validate(request.screen, request.lines, sourceScale, request.cpuRows, request.changedRows, image3D, nullptr);
    // Scratch sets and mapped uploads may still be in use by the previous submission.
    Complete();
    RenderCostVulkanScope cost(owner->Costs(), Cost::RecordDisplay);
    const auto& input = image3D ? image3D : blank3D;
    if (!image3D) sourceScale = 1;
    for (u32 i = 0; i < 2; ++i)
        Reserve(i, requests[i].screen, requests[i].lines, requests[i].changedRows, true, requests[i].cpuRows.size_bytes());
    std::array<Work, 2> works;
    for (u32 i = 0; i < 2; ++i)
        Prepare(works[i], i, requests[i].screen, requests[i].lines, requests[i].cpuRows,
            requests[i].changedRows, true, input);
    const auto command = owner->Begin(Device::SubmitKind::Display);
    for (const auto& work : works) Record(command, work, input, sourceScale);
    owner->Timestamp(Device::TimestampStage::DisplayCompose);
    for (u32 i = 0; i < 2; ++i) RecordResidentTail(command, requests[i].screen, requests[i].lines);
    const u32 screens[] = {requests[0].screen, requests[1].screen};
    Submit(screens, true, input, requests[0].lines, {}, nullptr, deferred);
    return {outputs[screens[0]], outputs[screens[1]]};
}

void DisplayCompositor::ComposeImpl(u32 screen, std::span<const Line> lines,
    const std::shared_ptr<Device::Image>& image3D, u32 sourceScale, std::span<u32> destination,
    const Device::Buffer* direct, bool resident, std::span<const bool> changedRows, bool deferred)
{
    // [observed] Screens share descriptors and mapped uploads. Finish before
    // changing either; Begin alone would complete too late for these host writes.
    Complete();
    RenderCostVulkanScope cost(owner->Costs(), Cost::RecordDisplay);
    Validate(screen, lines, sourceScale, destination, changedRows, image3D, direct);
    // Direct callers never allocate the intermediate staging image-sized buffer.
    // Legacy callers transfer the same composed ranges into staging, then copy them.
    if (!resident && !direct && !readback)
        readback = owner->CreateBuffer(destination.size_bytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            true, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    const auto& input = image3D ? image3D : blank3D;
    if (!image3D) sourceScale = 1;
    Reserve(0, screen, lines, changedRows, resident, destination.size_bytes());
    Work work;
    Prepare(work, 0, screen, lines, destination, changedRows, resident, input);
    const auto command = owner->Begin(Device::SubmitKind::Display);
    Record(command, work, input, sourceScale);
    owner->Timestamp(Device::TimestampStage::DisplayCompose);
    if (resident) RecordResidentTail(command, screen, lines);
    else
    {
        const u32 transferredRows = RecordReadback(command, screen, lines, direct);
        if (transferredRows)
        {
            owner->Timestamp(Device::TimestampStage::DisplayReadback);
            if (owner->Costs()) owner->Costs()->Transfer(Cost::DisplayReadbackBytes,
                uint64_t(transferredRows) * 256 * scale * scale * sizeof(u32), transferredRows);
        }
    }
    Submit(std::span<const u32>(&screen, 1), resident, input, lines, destination, resident ? nullptr : direct, deferred);
}

u32 DisplayCompositor::RecordReadback(VkCommandBuffer command, u32 screen, std::span<const Line> lines,
    const Device::Buffer* direct)
{
    u32 transferredRows = 0;
    ImageBarrier(f, command, outputs[screen]->Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    if (direct)
    {
        VkBufferMemoryBarrier target{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        target.srcAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
        target.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        target.srcQueueFamilyIndex = target.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        target.buffer = direct->Handle(); target.size = direct->Size();
        f.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 1, &target, 0, nullptr);
    }
    std::array<VkBufferImageCopy, 192> copies{};
    u32 count = 0;
    for (u32 y = 0; y < 192;)
    {
        if (Preserved(lines[y])) { ++y; continue; }
        const u32 first = y++;
        while (y < 192 && !Preserved(lines[y])) ++y;
        auto& copy = copies[count++];
        copy.bufferOffset = VkDeviceSize(first) * 256 * scale * scale * sizeof(u32);
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset.y = static_cast<int32_t>(first * scale);
        copy.imageExtent = {256 * scale, (y - first) * scale, 1};
        transferredRows += y - first;
    }
    if (count)
        f.vkCmdCopyImageToBuffer(command, outputs[screen]->Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            direct ? direct->Handle() : readback->Handle(), count, copies.data());
    ImageBarrier(f, command, outputs[screen]->Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    VkMemoryBarrier download{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    download.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    download.dstAccessMask = VK_ACCESS_HOST_READ_BIT | (direct ? VK_ACCESS_HOST_WRITE_BIT : 0);
    f.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &download, 0, nullptr, 0, nullptr);
    if (owner->Costs()) owner->Costs()->Transfer(Cost::ComposedRows, 0,
        std::count_if(lines.begin(), lines.end(), [](const Line& line) { return !Preserved(line); }));
    return transferredRows;
}

void DisplayCompositor::ComposeDirectPair(const std::array<DirectRequest, 2>& requests,
    const std::shared_ptr<Device::Image>& image3D, u32 sourceScale, bool deferred)
{
    if (!requests[0].direct || !requests[1].direct) throw std::invalid_argument("Display pair requires direct backings");
    if (requests[0].screen == requests[1].screen) throw std::invalid_argument("Duplicate display pair screen");
    for (const auto& request : requests)
        Validate(request.screen, request.lines, sourceScale, request.destination, {}, image3D, request.direct);
    // Validate proved each backing's data/size equals its destination; reject any overlap.
    const auto begin = [](const DirectRequest& r) { return reinterpret_cast<std::uintptr_t>(r.direct->Data()); };
    if (requests[0].direct->Handle() == requests[1].direct->Handle() ||
        (begin(requests[0]) < begin(requests[1]) + requests[1].direct->Size() &&
         begin(requests[1]) < begin(requests[0]) + requests[0].direct->Size()))
        throw std::invalid_argument("Aliasing display pair backings");
    // Scratch sets and mapped uploads may still be in use by the previous submission.
    Complete();
    RenderCostVulkanScope cost(owner->Costs(), Cost::RecordDisplay);
    const auto& input = image3D ? image3D : blank3D;
    if (!image3D) sourceScale = 1;
    for (u32 i = 0; i < 2; ++i)
        Reserve(i, requests[i].screen, requests[i].lines, {}, false, 0);
    std::array<Work, 2> works;
    for (u32 i = 0; i < 2; ++i)
        Prepare(works[i], i, requests[i].screen, requests[i].lines, requests[i].destination, {}, false, input);
    const auto command = owner->Begin(Device::SubmitKind::Display);
    for (const auto& work : works) Record(command, work, input, sourceScale);
    owner->Timestamp(Device::TimestampStage::DisplayCompose);
    u32 transferredRows = 0;
    for (const auto& request : requests)
        transferredRows += RecordReadback(command, request.screen, request.lines, request.direct);
    if (transferredRows)
    {
        owner->Timestamp(Device::TimestampStage::DisplayReadback);
        if (owner->Costs()) owner->Costs()->Transfer(Cost::DisplayReadbackBytes,
            uint64_t(transferredRows) * 256 * scale * scale * sizeof(u32), transferredRows);
    }
    // Direct backing means Submit retains no pendingCopy; residentValid stays false.
    const u32 screens[] = {requests[0].screen, requests[1].screen};
    Submit(screens, false, input, requests[0].lines, {}, requests[0].direct, deferred);
}

void DisplayCompositor::Submit(std::span<const u32> screens, bool resident, const std::shared_ptr<Device::Image>& input,
    std::span<const Line> lines, std::span<u32> destination, const Device::Buffer* direct, bool deferred)
{
    owner->Submit();
    // Everything the GPU may still touch is retained before anything can throw.
    pending = true;
    pendingScreenCount = static_cast<u32>(screens.size());
    std::copy(screens.begin(), screens.end(), pendingScreens.begin());
    pendingResident = resident;
    pendingInput = input;
    pendingCopy = !resident && !direct ? destination : std::span<u32>{};
    if (!pendingCopy.empty())
        for (u32 y = 0; y < 192; ++y) pendingRows[y] = !Preserved(lines[y]);
    try { owner->SetPendingCompletion([this] { Complete(); }); }
    catch (...) { Complete(); throw; }
    if (!deferred) Complete();
}

void DisplayCompositor::Complete()
{
    if (!pending) return;
    pending = false;
    owner->WaitForSubmission();
    pendingInput.reset();
    if (pendingResident)
        for (u32 i = 0; i < pendingScreenCount; ++i) residentValid[pendingScreens[i]] = true;
    // [observed] Cached coherent backing needs no invalidate. The recorded host
    // barrier and this fence precede CPU reads and reuse, including the copy below.
    if (pendingCopy.empty()) return;
    RenderCostVulkanScope copy(owner->Costs(), Cost::DisplayCopy);
    const size_t rowPixels = size_t(256) * scale * scale;
    for (u32 y = 0; y < 192; ++y)
        if (pendingRows[y])
        {
            std::memcpy(pendingCopy.data() + y * rowPixels,
                static_cast<const u32*>(readback->Data()) + y * rowPixels, rowPixels * sizeof(u32));
            if (owner->Costs()) owner->Costs()->Transfer(Cost::DisplayCopyBytes, rowPixels * sizeof(u32));
        }
    pendingCopy = {};
}

void DisplayCompositor::EnableExternalOutputs()
{
    // The previous submission may still be writing the current outputs.
    Complete();
    if (externalOutputs) return;
    const u32 width = 256 * scale, height = 192 * scale;
    // Allocate every replacement before recording any GPU work: an allocation
    // failure leaves the current outputs and the pipeline usable.
    std::vector<std::shared_ptr<Device::Image>> next(outputs.size());
    for (auto& image : next)
        image = owner->CreateExternalDisplayImage(width, height);
    const auto command = owner->Begin(Device::SubmitKind::Display);
    for (size_t i = 0; i < next.size(); ++i)
    {
        ImageBarrier(f, command, next[i]->Handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (!residentValid[i]) continue;
        // Init left every output in GENERAL, but contents are meaningful only
        // where residentValid is set. Carry those pixels into the replacement
        // so the next composition keeps its Keep/partial rows.
        ImageBarrier(f, command, outputs[i]->Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {width, height, 1};
        f.vkCmdCopyImage(command, outputs[i]->Handle(), VK_IMAGE_LAYOUT_GENERAL,
            next[i]->Handle(), VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        ImageBarrier(f, command, next[i]->Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    owner->SubmitAndWait();
    // The fence confirms the copies: only now may the retired objects drop.
    outputs.swap(next);
    externalOutputs = true;
}

const std::shared_ptr<Device::Image>& DisplayCompositor::Output(u32 screen) const
{
    if (screen >= outputs.size()) throw std::out_of_range("Display output index");
    return outputs[screen];
}
}
