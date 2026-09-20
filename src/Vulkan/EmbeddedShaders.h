// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ComputePipeline.h"

namespace melonDS::Vulkan
{
const ComputePipeline::Shaders& EmbeddedShaders(int scale = 1);
std::span<const uint32_t> EmbeddedDisplayCompose();
std::span<const uint32_t> EmbeddedNativeReadback();
std::span<const uint32_t> EmbeddedCaptureBlend();
}
