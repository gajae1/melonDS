// SPDX-License-Identifier: GPL-3.0-or-later
#include "DisplayCompositor.h"
#include "RenderCost.h"
#include "GPU_ColorOp.h"
#include "PixelConvert.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace melonDS::Vulkan {
namespace {
using Line = SoftRenderer2D::ScaledLineContext;
using Cost = RenderCostVulkanMeter;
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

DisplayCompositor::DisplayCompositor(std::shared_ptr<Device> device, std::span<const u32> shader, u32 scale)
    : owner(std::move(device)), f(owner->Functions()), device(owner->Handle()), scale(scale)
{
    if (!scale || scale > 16 || shader.empty()) throw std::invalid_argument("Invalid display compositor configuration");
    try { Init(shader); } catch (...) { Cleanup(); throw; }
}
DisplayCompositor::~DisplayCompositor() { Cleanup(); }
void DisplayCompositor::Cleanup()
{
    // Device::SubmitAndWait drains on failure before resources can be released.
    if (pipeline) f.vkDestroyPipeline(device, pipeline, nullptr);
    if (layout) f.vkDestroyPipelineLayout(device, layout, nullptr);
    if (pool) f.vkDestroyDescriptorPool(device, pool, nullptr);
    if (bindings) f.vkDestroyDescriptorSetLayout(device, bindings, nullptr);
}
void DisplayCompositor::Init(std::span<const u32> shader)
{
    contexts = owner->CreateBuffer(sizeof(Line) * 192, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    for (auto& image : outputs)
        image = owner->CreateImage(256 * scale, 192 * scale, 1, VK_FORMAT_R32_UINT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    blank3D = owner->CreateImage(256, 192, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const VkDescriptorSetLayoutBinding entries[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo bindingInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    bindingInfo.bindingCount = 3; bindingInfo.pBindings = entries;
    Device::Check(f.vkCreateDescriptorSetLayout(device, &bindingInfo, nullptr, &bindings), "Create display bindings");
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(u32)};
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
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1; poolInfo.poolSizeCount = 2; poolInfo.pPoolSizes = sizes;
    Device::Check(f.vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "Create display descriptor pool");
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = pool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &bindings;
    Device::Check(f.vkAllocateDescriptorSets(device, &allocation, &descriptors), "Allocate display descriptors");
    VkDescriptorBufferInfo bufferInfo{contexts->Handle(), 0, contexts->Size()};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptors; write.dstBinding = 0; write.descriptorCount = 1;
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
    const Device::Buffer* direct)
{
    RenderCostVulkanScope cost(owner->Costs(), Cost::RecordDisplay);
    CheckExtents(lines, sourceScale, scale, destination.size());
    constexpr VkMemoryPropertyFlags directProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    if (direct && (!direct->BelongsTo(*owner) || direct->Size() != destination.size_bytes() ||
        direct->Data() != destination.data() || !(direct->Usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        (direct->MemoryProperties() & directProperties) != directProperties))
        throw std::invalid_argument("Invalid direct display backing");
    // Direct callers never allocate the intermediate staging image-sized buffer.
    // Legacy callers keep the original full-image transfer and selective memcpy.
    if (!direct && !readback)
        readback = owner->CreateBuffer(destination.size_bytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            true, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (screen >= outputs.size() || (image3D && !image3D->BelongsTo(*owner)))
        throw std::invalid_argument("Invalid display composition image");
    const auto& input = image3D ? image3D : blank3D;
    if (!image3D) sourceScale = 1;
    {
        RenderCostVulkanScope copy(owner->Costs(), Cost::ContextCopy);
        std::memcpy(contexts->Data(), lines.data(), lines.size_bytes());
        if (owner->Costs()) owner->Costs()->Transfer(Cost::ContextCopyBytes, lines.size_bytes());
    }
    const VkDescriptorImageInfo images[] = {{VK_NULL_HANDLE, input->View(), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, outputs[screen]->View(), VK_IMAGE_LAYOUT_GENERAL}};
    for (u32 i = 0; i < 2; ++i)
    {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptors; write.dstBinding = i + 1; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; write.pImageInfo = &images[i];
        f.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    const auto command = owner->Begin(Device::SubmitKind::Display);
    VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    upload.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; upload.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    f.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &upload, 0, nullptr, 0, nullptr);
    ImageBarrier(f, command, input->Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    ImageBarrier(f, command, outputs[screen]->Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    f.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    f.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &descriptors, 0, nullptr);
    const u32 settings[] = {scale, sourceScale};
    f.vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), settings);
    f.vkCmdDispatch(command, 32 * scale, 24 * scale, 1);
    owner->Timestamp(Device::TimestampStage::DisplayCompose);
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
                direct->Handle(), count, copies.data());
    }
    else
    {
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {256 * scale, 192 * scale, 1};
        transferredRows = 192;
        f.vkCmdCopyImageToBuffer(command, outputs[screen]->Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->Handle(), 1, &copy);
    }
    ImageBarrier(f, command, outputs[screen]->Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    VkMemoryBarrier download{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    download.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    download.dstAccessMask = VK_ACCESS_HOST_READ_BIT | (direct ? VK_ACCESS_HOST_WRITE_BIT : 0);
    f.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &download, 0, nullptr, 0, nullptr);
    if (transferredRows)
    {
        owner->Timestamp(Device::TimestampStage::DisplayReadback);
        if (owner->Costs()) owner->Costs()->Transfer(Cost::DisplayReadbackBytes,
            uint64_t(transferredRows) * 256 * scale * scale * sizeof(u32), transferredRows);
    }
    if (owner->Costs()) owner->Costs()->Transfer(Cost::ComposedRows, 0,
        std::count_if(lines.begin(), lines.end(), [](const Line& line) { return !Preserved(line); }));
    owner->SubmitAndWait();
    // Cached coherent backing needs no invalidate; GPU completion and the host
    // visibility barrier still precede both reads and future CPU fills.
    if (direct) return;
    RenderCostVulkanScope copy(owner->Costs(), Cost::DisplayCopy);
    const size_t rowPixels = size_t(256) * scale * scale;
    for (u32 y = 0; y < 192; ++y)
        if (!Preserved(lines[y]))
        {
            std::memcpy(destination.data() + y * rowPixels,
                static_cast<const u32*>(readback->Data()) + y * rowPixels, rowPixels * sizeof(u32));
            if (owner->Costs()) owner->Costs()->Transfer(Cost::DisplayCopyBytes, rowPixels * sizeof(u32));
        }
}
}
