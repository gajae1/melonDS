// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ComputePipeline.h"

namespace melonDS::Vulkan
{
const ComputePipeline::Shaders& EmbeddedShaders(int scale = 1);
std::span<const uint32_t> EmbeddedDisplayCompose();
std::span<const uint32_t> EmbeddedNativeReadback();
std::span<const uint32_t> EmbeddedCaptureBlend();
std::span<const uint32_t> EmbeddedNative2D();
std::span<const uint32_t> EmbeddedNative2DMerge();
std::span<const uint32_t> EmbeddedNative2DCapture();
std::span<const uint32_t> EmbeddedNative2DCaptureHires();
std::span<const uint32_t> EmbeddedTextureDecode();
std::span<const uint32_t> EmbeddedPresent_vert();
std::span<const uint32_t> EmbeddedPresent_frag();
}
