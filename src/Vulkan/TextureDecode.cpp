// SPDX-License-Identifier: GPL-3.0-or-later
#include "TextureDecode.h"
#include <limits>
#include <stdexcept>

namespace melonDS::Vulkan {
namespace {
void Require(bool value, const char* reason) { if (!value) throw std::invalid_argument(reason); }
// Both clear kinds decode one 256x256 plane regardless of texParam.
constexpr uint32_t ClearSize = 256;
void Barrier(Device& owner, VkCommandBuffer cmd, VkPipelineStageFlags from, VkPipelineStageFlags to,
    VkAccessFlags src, VkAccessFlags dst) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask = src; b.dstAccessMask = dst;
    owner.Functions().vkCmdPipelineBarrier(cmd, from, to, 0, 1, &b, 0, nullptr, 0, nullptr);
}
}
TextureDecode::TextureDecode(std::shared_ptr<Device> device, std::span<const uint32_t> shader)
    : owner(std::move(device))
{
    Require(owner && !shader.empty(), "Invalid texture decoder input");
    const auto& f = owner->Functions(); const auto d = owner->Handle();
    VkShaderModule module{};
    try {
        std::array<VkDescriptorSetLayoutBinding, 4> entries{};
        for (uint32_t i = 0; i < entries.size(); ++i)
            entries[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo bi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        bi.bindingCount = uint32_t(entries.size()); bi.pBindings = entries.data();
        Device::Check(f.vkCreateDescriptorSetLayout(d, &bi, nullptr, &bindings), "Texture decoder bindings");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &bindings; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &push;
        Device::Check(f.vkCreatePipelineLayout(d, &li, nullptr, &layout), "Texture decoder layout");
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = shader.size_bytes(); si.pCode = shader.data();
        Device::Check(f.vkCreateShaderModule(d, &si, nullptr, &module), "Texture decoder shader");
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pi.layout = layout;
        pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        Device::Check(f.vkCreateComputePipelines(d, owner->GetPipelineCache(), 1, &pi, nullptr, &pipeline), "Texture decoder pipeline");
        f.vkDestroyShaderModule(d, module, nullptr); module = VK_NULL_HANDLE;
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &size;
        Device::Check(f.vkCreateDescriptorPool(d, &pci, nullptr, &pool), "Texture decoder pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &bindings;
        Device::Check(f.vkAllocateDescriptorSets(d, &ai, &set), "Texture decoder descriptors");
    } catch (...) {
        if (module) f.vkDestroyShaderModule(d, module, nullptr);
        Cleanup(); throw;
    }
}
TextureDecode::~TextureDecode() { Cleanup(); }
void TextureDecode::Cleanup()
{
    const auto& f = owner->Functions(); const auto d = owner->Handle();
    if (pool) f.vkDestroyDescriptorPool(d, pool, nullptr);
    if (pipeline) f.vkDestroyPipeline(d, pipeline, nullptr);
    if (layout) f.vkDestroyPipelineLayout(d, layout, nullptr);
    if (bindings) f.vkDestroyDescriptorSetLayout(d, bindings, nullptr);
    pool = VK_NULL_HANDLE; pipeline = VK_NULL_HANDLE; layout = VK_NULL_HANDLE; bindings = VK_NULL_HANDLE;
}
std::span<const TextureDecode::Slice> TextureDecode::Prepare(std::span<const Job> requests,
    const std::shared_ptr<Device::Buffer>& textures, const std::shared_ptr<Device::Buffer>& palettes,
    const std::shared_ptr<Device::Buffer>& captured)
{
    if (prepared) throw std::logic_error("Texture decode batch has not completed");
    Require(!requests.empty(), "Empty texture decode batch");
    const auto valid = [&](const std::shared_ptr<Device::Buffer>& buffer, uint64_t bytes) {
        return buffer && buffer->BelongsTo(*owner) && buffer->Size() >= bytes &&
            (buffer->Usage() & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    };
    const bool banks = captured != nullptr;
    const bool hires = requests.front().kind == Kind::HiresCapture;
    Require(!banks || valid(captured, 524288), "Invalid captured bank buffer");
    Require(valid(textures, hires ? 4u : banks ? 532480u : 524288u) && valid(palettes, hires ? 4u : 131072u),
        "Invalid flat texture/palette buffers");
    std::vector<Slice> next;
    next.reserve(requests.size());
    uint64_t words = 0;
    for (const auto& job : requests) {
        Require(uint32_t(job.kind) <= uint32_t(Kind::HiresCapture) &&
            (job.kind == Kind::HiresCapture) == hires, "Invalid texture decode job kind");
        const bool clear = job.kind == Kind::ClearColor || job.kind == Kind::ClearDepth;
        if (!clear) Require((job.texParam >> 26) & 7, "Invalid texture format");
        const uint32_t scale = hires ? job.palBase >> 2 : 1;
        if (hires) Require(scale >= 2 && scale <= 11 && ((job.texParam >> 26) & 7) == 7 &&
            valid(captured, uint64_t(524288) * ((scale * scale + 1u) & ~1u)),
            "Invalid enhanced capture texture source");
        const uint32_t width = clear ? ClearSize : (8u << ((job.texParam >> 20) & 7)) * scale;
        const uint32_t height = clear ? ClearSize : (8u << ((job.texParam >> 23) & 7)) * scale;
        const uint32_t pixels = width * height;
        Require((pixels + 63) / 64 <= owner->Properties().limits.maxComputeWorkGroupCount[0],
            "Texture decode exceeds dispatch limit");
        next.push_back({uint32_t(words), width, height}); words += pixels;
        Require(words <= std::numeric_limits<uint32_t>::max() && words * 4 <= owner->Properties().limits.maxStorageBufferRange,
            "Texture decode exceeds storage limit");
    }
    auto nextJobs = std::vector<Job>(requests.begin(), requests.end());
    if (!output || output->Size() < words * 4 || output.use_count() > 1)
        output = owner->CreateBuffer(words * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    Require(textures != output && palettes != output && (!banks || captured != output),
        "Texture decode input aliases output");
    const VkDeviceSize textureBytes = hires ? 4 : banks ? VkDeviceSize(532480) : VkDeviceSize(524288);
    // Binding 3 must stay a valid descriptor without bank data; the shader only
    // reads it while control bit 0 is set, so the baseline start stands in for
    // the physical banks and is never read.
    const VkDescriptorBufferInfo capturedInfo{banks ? captured->Handle() : textures->Handle(), 0,
        hires ? captured->Size() : banks ? VkDeviceSize(524288) : VkDeviceSize(4)};
    const std::array<VkDescriptorBufferInfo, 4> infos{{{textures->Handle(), 0, textureBytes},
        {palettes->Handle(), 0, hires ? VkDeviceSize(4) : VkDeviceSize(131072)}, {output->Handle(), 0, words * 4}, capturedInfo}};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet = set;
        writes[i].dstBinding = i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &infos[i];
    }
    owner->Functions().vkUpdateDescriptorSets(owner->Handle(), uint32_t(writes.size()), writes.data(), 0, nullptr);
    inputs = {textures, palettes, captured}; jobs = std::move(nextJobs); slices = std::move(next);
    hasCaptured = banks;
    prepared = true; recorded = false;
    return slices;
}
void TextureDecode::Record(VkCommandBuffer command)
{
    if (!prepared || recorded || !command) throw std::logic_error("Invalid texture decode recording state");
    const auto& f = owner->Functions();
    Barrier(*owner, command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    f.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    f.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    for (size_t i = 0; i < jobs.size(); ++i) {
        const uint32_t control = uint32_t(jobs[i].kind) * 2u + (hasCaptured ? 1u : 0u);
        const uint32_t push[] = {jobs[i].texParam, jobs[i].palBase, slices[i].word, control};
        f.vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        f.vkCmdDispatch(command, (slices[i].width * slices[i].height + 63) / 64, 1, 1);
    }
    // Independent slices need no inter-dispatch barriers. Make every decoded
    // word available to the caller's buffer-to-cache-image transfers.
    Barrier(*owner, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    recorded = true;
}
void TextureDecode::Complete()
{
    if (!prepared || !recorded) throw std::logic_error("Texture decode batch was not recorded");
    jobs.clear(); inputs = {}; hasCaptured = false; prepared = false; recorded = false;
}
}
