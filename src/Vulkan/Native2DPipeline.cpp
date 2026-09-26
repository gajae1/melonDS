// SPDX-License-Identifier: GPL-3.0-or-later
#include "Native2DPipeline.h"
#include "Native2DMemory.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace melonDS::Vulkan::Native2D {
namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
constexpr size_t HistoryBytes = size_t(Pipeline::MaxRecords + 2) * 512 * sizeof(uint32_t);
constexpr uint32_t NativeFrameWords = 256 * 192;
// Buffers 0-3 memory/records/raw/history; images 4 guest 3D, 5/6 top/bottom,
// 7 display 3D; buffers 8 native frames, 9 scaled capture A, 10 hires capture.
constexpr uint32_t BindingCount = 11;
constexpr bool ImageBinding(uint32_t binding) { return binding >= 4 && binding < 8; }
void MemoryBarrier(Device& device, VkCommandBuffer command, VkPipelineStageFlags src,
    VkPipelineStageFlags dst, VkAccessFlags read, VkAccessFlags write)
{
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = read; barrier.dstAccessMask = write;
    device.Functions().vkCmdPipelineBarrier(command, src, dst, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
}

Pipeline::Pipeline(std::shared_ptr<Device> device, std::span<const uint32_t> shader,
    std::span<const uint32_t> mergeShader, uint32_t scale)
    : owner(std::move(device)), displayScale(scale)
{
    Require(owner && !shader.empty(), "Invalid native 2D pipeline input");
    const auto& limits = owner->Properties().limits;
    Require(scale && scale <= limits.maxImageDimension2D / 256, "Invalid native 2D display scale");
    Require(limits.maxPerStageDescriptorStorageBuffers >= 7 && limits.maxPerStageDescriptorStorageImages >= 4 &&
        limits.maxComputeWorkGroupInvocations >= 64 && limits.maxComputeWorkGroupSize[0] >= 64 &&
        limits.maxComputeSharedMemorySize >= 3088 && limits.maxComputeWorkGroupCount[1] >= MaxRecords,
        "Native 2D compute limits unavailable");
    const auto& f = owner->Functions();
    const auto d = owner->Handle();
    VkShaderModule module{}, mergeModule{};
    try
    {
        std::array<VkDescriptorSetLayoutBinding, BindingCount> entries{};
        for (uint32_t i = 0; i < entries.size(); ++i)
            entries[i] = {i, ImageBinding(i) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo bi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        bi.bindingCount = uint32_t(entries.size()); bi.pBindings = entries.data();
        Device::Check(f.vkCreateDescriptorSetLayout(d, &bi, nullptr, &bindings), "Native 2D bindings");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &bindings;
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &push;
        Device::Check(f.vkCreatePipelineLayout(d, &li, nullptr, &layout), "Native 2D layout");
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = shader.size_bytes(); si.pCode = shader.data();
        Device::Check(f.vkCreateShaderModule(d, &si, nullptr, &module), "Native 2D shader");
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.layout = layout;
        pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        Device::Check(f.vkCreateComputePipelines(d, owner->GetPipelineCache(), 1, &pi, nullptr, &pipeline), "Native 2D pipeline");
        f.vkDestroyShaderModule(d, module, nullptr); module = VK_NULL_HANDLE;
        if (!mergeShader.empty())
        {
            // Optional merge pass reuses this descriptor layout/pipeline layout;
            // it declares only binding 0 as a read/write storage buffer and
            // reads the first 8 push-constant bytes.
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = mergeShader.size_bytes(); mi.pCode = mergeShader.data();
            Device::Check(f.vkCreateShaderModule(d, &mi, nullptr, &mergeModule), "Native 2D merge shader");
            pi.stage.module = mergeModule;
            Device::Check(f.vkCreateComputePipelines(d, owner->GetPipelineCache(), 1, &pi, nullptr, &mergePipeline), "Native 2D merge pipeline");
            f.vkDestroyShaderModule(d, mergeModule, nullptr); mergeModule = VK_NULL_HANDLE;
        }
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1; poolInfo.poolSizeCount = 2; poolInfo.pPoolSizes = sizes;
        Device::Check(f.vkCreateDescriptorPool(d, &poolInfo, nullptr, &pool), "Native 2D pool");
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &bindings;
        Device::Check(f.vkAllocateDescriptorSets(d, &allocation, &set), "Native 2D descriptors");
        history = owner->CreateBuffer(HistoryBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        nativeFrames = owner->CreateBuffer(VkDeviceSize(outputs.size()) * NativeFrameWords * sizeof(uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        for (auto& image : outputs)
            image = owner->CreateImage(256 * scale, 192 * scale, 1, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        blank3D = owner->CreateImage(256, 192, 1, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    catch (...)
    {
        if (module) f.vkDestroyShaderModule(d, module, nullptr);
        if (mergeModule) f.vkDestroyShaderModule(d, mergeModule, nullptr);
        Cleanup(); throw;
    }
}

Pipeline::~Pipeline() { Cleanup(); }
void Pipeline::Cleanup()
{
    const auto& f = owner->Functions(); const auto d = owner->Handle();
    if (pool) f.vkDestroyDescriptorPool(d, pool, nullptr);
    if (mergePipeline) f.vkDestroyPipeline(d, mergePipeline, nullptr);
    if (pipeline) f.vkDestroyPipeline(d, pipeline, nullptr);
    if (layout) f.vkDestroyPipelineLayout(d, layout, nullptr);
    if (bindings) f.vkDestroyDescriptorSetLayout(d, bindings, nullptr);
    pool = VK_NULL_HANDLE; mergePipeline = VK_NULL_HANDLE; pipeline = VK_NULL_HANDLE;
    layout = VK_NULL_HANDLE; bindings = VK_NULL_HANDLE;
}

void Pipeline::EnsureBuffer(std::shared_ptr<Device::Buffer>& buffer, size_t bytes, VkBufferUsageFlags usage, bool host)
{
    Require(bytes && bytes <= owner->Properties().limits.maxStorageBufferRange, "Native 2D buffer range exceeded");
    if (!buffer || buffer->Size() < bytes)
        buffer = owner->CreateBuffer(bytes, usage, host, host ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
}

void Pipeline::ImageBarrier(VkCommandBuffer command, const Device::Image& image,
    VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkAccessFlags read, VkAccessFlags write)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = image.Handle(); barrier.oldLayout = from; barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask = read; barrier.dstAccessMask = write;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    owner->Functions().vkCmdPipelineBarrier(command, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void Pipeline::Render(std::span<const uint32_t> bytes, std::span<const Record> lines,
    const std::shared_ptr<Device::Image>& native3D, uint32_t buffer,
    CapturePipeline* capture, std::span<const CaptureCommand> captures,
    std::vector<MemoryCopy> copies, std::span<const MemoryMerge> merges,
    const std::shared_ptr<Device::Image>& display3D)
{
    Require(buffer < 2 && !bytes.empty() && !lines.empty() && lines.size() <= MaxRecords,
        "Invalid native 2D batch");
    Require(captures.empty() || capture, "Native capture batch has no pipeline");
    for (const auto& copy : copies)
        Require(copy.source && copy.source->BelongsTo(*owner) &&
            (copy.source->Usage() & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) && copy.words &&
            uint64_t(copy.sourceWord) + copy.words <= copy.source->Size() / 4 &&
            uint64_t(copy.destinationWord) + copy.words <= bytes.size(),
            "Invalid native 2D GPU memory copy");
    std::vector<VkBuffer> copySources;
    copySources.reserve(copies.size());
    for (auto& copy : copies)
    {
        copySources.push_back(copy.source->Handle());
        // These input leases are consumed by this call, before capture writes.
        // CapturePipeline itself retains this source as banks or previousBanks
        // through the fence. Drop only our lease so ordinary LCDC feedback can
        // reuse the bank allocation without a needless full-bank copy. Any
        // genuinely future consumer still holds its own reference and forces COW.
        if (!captures.empty() && copy.source == capture->banks) copy.source.reset();
    }
    Require(merges.empty() || mergePipeline, "Native 2D merge pipeline unavailable");
    // Record words must stay within uint32 push-constant addressing; the buffer
    // limit itself is enforced by EnsureBuffer before Begin below.
    Require(merges.size() <= (uint64_t(std::numeric_limits<uint32_t>::max()) - bytes.size()) / 3,
        "Native 2D merge records exceed addressable memory");
    for (const auto& merge : merges)
        // Merge sources stay outside the destination pages by caller contract;
        // bounds are still checked against the caller bytes, not the tail.
        Require(merge.words >= 1 && merge.words <= 128 &&
            uint64_t(merge.sourceWord) + merge.words <= bytes.size() &&
            uint64_t(merge.destinationWord) + merge.words <= bytes.size(),
            "Invalid native 2D memory merge");
    const auto image = native3D ? native3D : blank3D;
    const auto display = display3D ? display3D : image;
    Require(image->BelongsTo(*owner) && image->Format() == VK_FORMAT_R8G8B8A8_UNORM &&
        (image->Usage() & VK_IMAGE_USAGE_STORAGE_BIT), "Invalid native 3D image");
    Require(display->BelongsTo(*owner) && display->Format() == VK_FORMAT_R8G8B8A8_UNORM &&
        (display->Usage() & VK_IMAGE_USAGE_STORAGE_BIT), "Invalid native display 3D image");
    // Enhanced BG/LCDC reads and scaled capture A writes share this storage.
    const std::shared_ptr<Device::Buffer> hires = capture ? capture->Hires() : std::shared_ptr<Device::Buffer>{};
    const bool hiresReady = hires && capture->HiresScale() == displayScale;
    bool scaledCaptures = false;
    std::vector<uint32_t> starts{0};
    std::array<bool, 384> destinations{};
    std::array<uint32_t, 2> last{0, 512};
    for (uint32_t i = 0; i < lines.size(); ++i)
    {
        const auto& line = lines[i];
        Require(line.layers.engine < 2 && line.screen < 2 && line.physicalLine < 192 &&
            line.rawOutput == i * 256 && line.objectWrite == (i + 2) * 512 &&
            line.hiresLCDC <= 15 && (!line.hiresLCDC || hiresReady) &&
            line.scaledCapture <= captures.size() &&
            (!line.scaledCapture || (hiresReady && line.layers.engine == 0)), "Invalid native 2D record");
        if (line.layers.reserved0)
            Require(hiresReady && line.layers.reserved0 < bytes.size() &&
                uint64_t(bytes[line.layers.reserved0]) + Memory::PageWords <= bytes.size(),
                "Invalid enhanced BG provenance page");
        scaledCaptures |= line.scaledCapture != 0;
        const uint32_t source = line.object.historyRead;
        Require(source == NoHistory || (source % 512 == 0 && source < (i + 2) * 512),
            "Native OBJ history must precede its consumer");
        if (!line.source3DAbort && line.source3DLine < 192)
            Require(line.source3DScale && image->Width() == line.source3DScale * 256 &&
                image->Height() == line.source3DScale * 192 && display->Width() == image->Width() &&
                display->Height() == image->Height(), "Invalid native 3D sample scale");
        const uint32_t destination = line.screen * 192 + line.physicalLine;
        const bool dependency = source != NoHistory && source >= (starts.back() + 2) * 512;
        if (i && (dependency || destinations[destination]))
        {
            starts.push_back(i); destinations.fill(false);
        }
        destinations[destination] = true;
        last[line.layers.engine] = line.objectWrite;
    }
    starts.push_back(uint32_t(lines.size()));
    // All allocation/validation precedes Begin: a host-side exception never
    // leaves the device's shared command buffer partially recording.
    EnsureBuffer(memory, bytes.size_bytes() + merges.size_bytes(),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    EnsureBuffer(records, lines.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    EnsureBuffer(raw, lines.size() * 256 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    if (scaledCaptures)
    {
        // Bound only for this batch; otherwise raw is a never-accessed dummy.
        Require(captures.size() <= 256, "Native scaled capture batch too large");
        EnsureBuffer(scaledRaw, captures.size() * 256 * displayScale * displayScale * sizeof(uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    }
    const auto& scaled = scaledCaptures ? scaledRaw : raw;
    std::memcpy(memory->Data(), bytes.data(), bytes.size_bytes());
    if (!merges.empty())
        std::memcpy(static_cast<uint8_t*>(memory->Data()) + bytes.size_bytes(),
            merges.data(), merges.size_bytes());
    std::memcpy(records->Data(), lines.data(), lines.size_bytes());
    const auto& f = owner->Functions(); const auto d = owner->Handle();
    const std::array<std::shared_ptr<Device::Buffer>, 7> buffers{memory, records, raw, history,
        nativeFrames, scaled, hires ? hires : raw};
    const std::array<std::shared_ptr<Device::Image>, 4> images{image,
        outputs[buffer * 2], outputs[buffer * 2 + 1], display};
    std::array<VkDescriptorBufferInfo, 7> bufferInfo{};
    std::array<VkDescriptorImageInfo, 4> imageInfo{};
    std::array<VkWriteDescriptorSet, BindingCount> writes{};
    for (uint32_t i = 0, b = 0, m = 0; i < BindingCount; ++i)
    {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
        if (ImageBinding(i))
        {
            imageInfo[m] = {VK_NULL_HANDLE, images[m]->View(), VK_IMAGE_LAYOUT_GENERAL};
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[i].pImageInfo = &imageInfo[m++];
        }
        else
        {
            bufferInfo[b] = {buffers[b]->Handle(), 0, buffers[b]->Size()};
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &bufferInfo[b++];
        }
    }
    f.vkUpdateDescriptorSets(d, uint32_t(writes.size()), writes.data(), 0, nullptr);
    if (!captures.empty()) capture->Prepare(captures, raw, uint32_t(lines.size()) * 256, native3D,
        scaledCaptures ? scaledRaw : std::shared_ptr<Device::Buffer>{}, display3D);
    const auto command = owner->Begin(Device::SubmitKind::Display);
    VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!historyInitialized) f.vkCmdFillBuffer(command, history->Handle(), 0, history->Size(), 0);
    if (!blankInitialized)
    {
        ImageBarrier(command, *blank3D, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        f.vkCmdClearColorImage(command, blank3D->Handle(), VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    for (uint32_t screen = 0; screen < 2; ++screen)
    {
        const uint32_t slot = buffer * 2 + screen;
        if (!initialized[slot])
        {
            ImageBarrier(command, *outputs[slot], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            f.vkCmdClearColorImage(command, outputs[slot]->Handle(), VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            f.vkCmdFillBuffer(command, nativeFrames->Handle(), VkDeviceSize(slot) * NativeFrameWords * sizeof(uint32_t),
                VkDeviceSize(NativeFrameWords) * sizeof(uint32_t), 0);
        }
    }
    if (!copies.empty())
    {
        MemoryBarrier(*owner, command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        for (size_t i = 0; i < copies.size(); ++i)
        {
            const auto& copy = copies[i];
            const VkBufferCopy region{VkDeviceSize(copy.sourceWord) * 4,
                VkDeviceSize(copy.destinationWord) * 4, VkDeviceSize(copy.words) * 4};
            f.vkCmdCopyBuffer(command, copySources[i], memory->Handle(), 1, &region);
        }
    }
    if (!merges.empty())
    {
        // OR merge sources into disjoint destination pages after every GPU copy
        // input landed; merge records live past the caller bytes in memory.
        // Sources never overlap destinations, so oversized batches dispatch in
        // groupY-limited chunks with no inter-chunk barrier.
        MemoryBarrier(*owner, command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        f.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, mergePipeline);
        f.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        const size_t groupLimit = owner->Properties().limits.maxComputeWorkGroupCount[1];
        for (size_t first = 0; first < merges.size(); first += groupLimit)
        {
            const uint32_t count = uint32_t(std::min(groupLimit, merges.size() - first));
            const uint32_t push[] = {uint32_t(bytes.size() + first * 3), count};
            f.vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
            f.vkCmdDispatch(command, 2, count, 1);
        }
        MemoryBarrier(*owner, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    MemoryBarrier(*owner, command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    f.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    f.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    for (size_t i = 1; i < starts.size(); ++i)
    {
        if (i > 1) MemoryBarrier(*owner, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        const uint32_t push[] = {starts[i - 1], starts[i] - starts[i - 1], displayScale, buffer};
        f.vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        f.vkCmdDispatch(command, 1, push[1], 1);
    }
    MemoryBarrier(*owner, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    for (uint32_t engine = 0; engine < 2; ++engine)
        if (last[engine] >= 1024)
        {
            const VkBufferCopy copy{VkDeviceSize(last[engine]) * 4, VkDeviceSize(engine) * 512 * 4, 512 * 4};
            f.vkCmdCopyBuffer(command, history->Handle(), history->Handle(), 1, &copy);
        }
    if (!captures.empty()) capture->Record(command);
    owner->SubmitAndWait();
    if (!captures.empty()) capture->Complete();
    historyInitialized = blankInitialized = true;
    initialized[buffer * 2] = initialized[buffer * 2 + 1] = true;
    rawWords = uint32_t(lines.size()) * 256;
}

const std::shared_ptr<Device::Image>& Pipeline::Output(uint32_t buffer, uint32_t screen) const
{
    Require(buffer < 2 && screen < 2 && initialized[buffer * 2 + screen], "Native 2D output is not ready");
    return outputs[buffer * 2 + screen];
}

void Pipeline::ReadRaw(uint32_t first, std::span<uint32_t> destination)
{
    Require(!destination.empty() && first <= rawWords && destination.size() <= rawWords - first,
        "Invalid native 2D raw readback");
    EnsureBuffer(landing, destination.size_bytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    const auto command = owner->Begin(Device::SubmitKind::FullReadback);
    MemoryBarrier(*owner, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy copy{VkDeviceSize(first) * 4, 0, destination.size_bytes()};
    owner->Functions().vkCmdCopyBuffer(command, raw->Handle(), landing->Handle(), 1, &copy);
    MemoryBarrier(*owner, command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    owner->SubmitAndWait();
    std::memcpy(destination.data(), landing->Data(), destination.size_bytes());
}

void Pipeline::ReadFrame(uint32_t buffer, uint32_t screen, std::span<uint32_t> destination,
    std::span<uint32_t> nativeDestination)
{
    Require(destination.size() == size_t(NativeFrameWords) * displayScale * displayScale &&
        (nativeDestination.empty() || nativeDestination.size() == NativeFrameWords), "Invalid native 2D frame readback");
    const auto& image = Output(buffer, screen);
    EnsureBuffer(landing, destination.size_bytes() + nativeDestination.size_bytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    const auto command = owner->Begin(Device::SubmitKind::FullReadback);
    ImageBarrier(command, *image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {256 * displayScale, 192 * displayScale, 1};
    owner->Functions().vkCmdCopyImageToBuffer(command, image->Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        landing->Handle(), 1, &copy);
    if (!nativeDestination.empty())
    {
        // The guest frame lands after the complete display-scale image bytes.
        MemoryBarrier(*owner, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy native{VkDeviceSize(buffer * 2 + screen) * NativeFrameWords * sizeof(uint32_t),
            destination.size_bytes(), nativeDestination.size_bytes()};
        owner->Functions().vkCmdCopyBuffer(command, nativeFrames->Handle(), landing->Handle(), 1, &native);
    }
    ImageBarrier(command, *image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    MemoryBarrier(*owner, command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    owner->SubmitAndWait();
    std::memcpy(destination.data(), landing->Data(), destination.size_bytes());
    if (!nativeDestination.empty())
        std::memcpy(nativeDestination.data(), static_cast<const uint8_t*>(landing->Data()) + destination.size_bytes(),
            nativeDestination.size_bytes());
}
}
