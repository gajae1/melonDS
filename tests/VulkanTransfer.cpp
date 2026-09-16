// SPDX-License-Identifier: GPL-3.0-or-later
// Real Vulkan allocation/lifetime checks. No ROM, BIOS or presentation window.
#include "Vulkan/ComputePipeline.h"
#include "Vulkan/EmbeddedShaders.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace melonDS::Vulkan;
namespace {
PFN_vkCreateBuffer DriverCreate;
unsigned Allocations = 0;
bool FailNext = false;
VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice device, const VkBufferCreateInfo* info,
    const VkAllocationCallbacks* callbacks, VkBuffer* buffer)
{
    ++Allocations;
    if (FailNext) { FailNext = false; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    return DriverCreate(device, info, callbacks, buffer);
}
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Observe {
    volk::VolkDeviceTable& table;
    explicit Observe(Device& d) : table(const_cast<volk::VolkDeviceTable&>(d.Functions()))
    { DriverCreate = table.vkCreateBuffer; table.vkCreateBuffer = Create; }
    ~Observe() { table.vkCreateBuffer = DriverCreate; FailNext = false; }
};
}
int main()
{
    try {
        std::string error;
        auto device = Device::Create(error);
        if (!device) { std::fprintf(stderr, "%s\n", error.c_str()); return 77; }
        ComputePipeline pipeline(device, EmbeddedShaders());
        auto small = pipeline.CreateTexture(8, 8, 2);
        auto large = pipeline.CreateTexture(32, 16, 1);
        Observe observe(*device);
        std::vector<uint32_t> pixels(64, 0x1F123456);
        pipeline.UploadTextureLayer(*small, 0, pixels);
        const auto warm = Allocations;
        for (unsigned i = 0; i < 12; ++i) {
            std::fill(pixels.begin(), pixels.end(), 0x1F000000 | i);
            pipeline.UploadTextureLayer(*small, i % 2, pixels);
        }
        const auto repeated = Allocations - warm;
        FailNext = true;
        bool failed = false;
        std::vector<uint32_t> bigger(512, 0x1F332211);
        try { pipeline.UploadTextureLayer(*large, 0, bigger); }
        catch (const std::runtime_error&) { failed = true; }
        Require(failed && !FailNext, "staging growth failure was not reported");
        const auto beforeRetry = Allocations;
        pipeline.UploadTextureLayer(*small, 1, pixels);
        const auto retry = Allocations - beforeRetry;
        pipeline.UploadTextureLayer(*large, 0, bigger);
        const auto grown = Allocations;
        for (unsigned i = 0; i < 4; ++i) {
            pipeline.UploadTextureLayer(*large, 0, bigger);
            pipeline.UploadTextureLayer(*small, 0, pixels);
        }
        const auto steady = Allocations - grown;
        bool invalid = false;
        try { pipeline.UploadTextureLayer(*small, 2, pixels); }
        catch (const std::invalid_argument&) { invalid = true; }
        Require(invalid, "invalid texture layer accepted");
        std::printf("staging allocations: repeated=%u after-failed-growth=%u grown-steady=%u\n",
            repeated, retry, steady);
        Require(repeated == 0 && retry == 0 && steady == 0,
            "synchronous texture uploads unnecessarily reallocate staging");
        ComputePipeline::Batch batch{};
        batch.meta.ClearColor = 0x1F123456; batch.meta.ClearDepth = 0xFFFFFF;
        const auto owned = pipeline.Render(batch);
        const auto view = pipeline.RenderView(std::span<const ComputePipeline::Batch>(&batch, 1));
        Require(std::equal(view.begin(), view.end(), owned.begin(), owned.end()), "borrowed readback differs from owned result");
        const auto* address = view.data();
        batch.meta.ClearColor = 0x1F010203;
        const auto changed = pipeline.RenderView(std::span<const ComputePipeline::Batch>(&batch, 1));
        Require(changed.data() == address && changed.front() != owned.front(), "readback storage is not reused or frame stayed stale");
        const auto retained = changed.front();
        bool rejected = false;
        try { (void)pipeline.RenderView({}); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected && changed.front() == retained, "rejected frame damaged retained readback");
        std::puts("Vulkan staging reuse, growth failure/retry and readback lifetime PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what()); return 1;
    }
}
