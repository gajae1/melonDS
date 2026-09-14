// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GPU_Vulkan.h"
#include "GPU3D_Vulkan.h"
#include "NDS.h"

namespace melonDS
{
VulkanRenderer::VulkanRenderer(NDS& nds)
    : SoftRenderer(nds, std::make_unique<VulkanRenderer3D>(nds.GPU.GPU3D))
{
}

bool VulkanRenderer::IsAvailable(std::string& error)
{
    return bool(Vulkan::Device::Create(error));
}

bool VulkanRenderer::HasRenderFailure() const
{
    return static_cast<const VulkanRenderer3D&>(*Rend3D).HasFailed();
}
}
