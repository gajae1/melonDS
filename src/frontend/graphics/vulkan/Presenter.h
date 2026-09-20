// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace Vulkan {
// Owns submission storage. Input pixels need only survive Present().
// First WSI implementation is Windows; unsupported hosts retain native output.
class Presenter {
public:
    enum class Result { Presented, Skipped, Failed };
    struct Diagnostics {
        bool Enabled = false;
        uint64_t Calls = 0, Submits = 0, Skips = 0, Failures = 0;
        uint64_t StagingBytes = 0, TransferBytes = 0;
        uint64_t UploadNs = 0, SubmitNs = 0, QueuePresentNs = 0;
    };
    // Host observations only; neither API return nor these counters proves
    // display completion. A skipped return may still follow a real submit.
    Diagnostics GetDiagnostics() const;
    static std::unique_ptr<Presenter> Create(void* nativeWindow, std::string& error, const std::string& preferredId = {});
    ~Presenter();
    Result Present(const void* bgra, uint32_t width, uint32_t height, uint32_t stride, std::string& error);
private:
    Presenter();
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
