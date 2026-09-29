// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Vulkan/Device.h"
#include <array>
#include <memory>

namespace GL {
class Context;

// OpenGL side of the Vulkan<->GL external display handoff (see
// Vulkan/ExternalDisplay.h for the Vulkan half). Imports the two R32UI screen
// images of a Device::CreateExternalDisplayImage() pair as GL_TEXTURE_2D objects
// (GL_EXT_memory_object[_win32], GL_EXT_semaphore[_win32]) and orders the queue
// ownership handoff around the caller's GL sampling. No shader, copy or readback.
//
// Threading/context: single thread; the GL context that was passed to the
// constructor must be current for the constructor, Begin, End and the normal
// destructor. The caller holds the renderLock for the whole Begin..End span,
// guarantees every producer (renderer) submission on both images has completed,
// and that both images are in VK_IMAGE_LAYOUT_GENERAL.
//
// Per redraw (every redraw, whether or not the pixels changed):
//   Begin(images)  import (cached) -> core Release -> GL wait Ready
//   ... caller samples Texture(0)/Texture(1) with texelFetch/NEAREST ...
//   End()          GL signal Returned -> core Acquire
//
// Failure model:
//  - Anything that throws before the core Release (capability, import, invalid
//    images) leaves nothing in flight: Healthy() turns false but the destructor
//    performs ordinary cleanup.
//  - Any GL error/exception after Release, or a throwing core Release/Acquire, is
//    an ambiguous handoff: the core session is Abandon()ed and this helper keeps
//    the session, cached image references and GL object names alive forever (a
//    bounded leak; NT handles owned by the core images stay open while GL may
//    still import them). No GL cleanup and no idle wait is done for that state.
//    The parent should latch the device as failed and not retry.
//  - Destroying with a handoff still active takes the same quarantine path.
// The cache holds at most two image pairs, keyed by shared_ptr identity. A pair
// is only replaced when no handoff is active, so the previous core Acquire fence
// has proven all earlier GL usage complete before its GL objects are deleted.
// The 4 GL extensions, matching device+driver UUIDs and R32UI OPTIMAL tiling are
// required; otherwise the constructor throws std::runtime_error (always the case
// off Win32, where the core session is unsupported). NT handles are never closed
// here.
class ExternalDisplay final {
public:
  ExternalDisplay(Context& context, std::shared_ptr<melonDS::Vulkan::Device> device);
  ~ExternalDisplay();
  ExternalDisplay(const ExternalDisplay&) = delete;
  ExternalDisplay& operator=(const ExternalDisplay&) = delete;

  bool UsesDevice(const std::shared_ptr<melonDS::Vulkan::Device>& device) const;
  // Throws std::runtime_error on failure; std::logic_error (state unchanged) if a
  // handoff is already active.
  void Begin(const std::array<std::shared_ptr<melonDS::Vulkan::Device::Image>, 2>& images);
  // Throws std::runtime_error on failure; std::logic_error if no handoff is active.
  void End();
  // GL texture of the given screen (0/1) for the active Begin..End span, else 0.
  unsigned int Texture(unsigned screen) const;
  bool Healthy() const;

private:
  struct State;
  std::shared_ptr<State> state;
};
} // namespace GL
