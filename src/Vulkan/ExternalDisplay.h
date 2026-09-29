// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Device.h"
#include <array>
#include <memory>

namespace melonDS::Vulkan {
// One Vulkan<->OpenGL handoff session for the two external display images of a
// Device::CreateExternalDisplayImage() pair. Win32 only; elsewhere the
// constructor throws std::runtime_error.
//
// Protocol per redraw (single thread; the caller serializes ALL host access to
// the device's compute queue, including the renderer's own submissions):
//   1. Caller has completed all renderer GPU work on both images (GENERAL layout).
//   2. Release(images): queue-ownership release to VK_QUEUE_FAMILY_EXTERNAL, signals
//      the Ready semaphore, then waits a finite 5 s on a private fence.
//   3. GL waits Ready(+both textures), samples, signals Returned and flushes.
//   4. Acquire(): waits Returned, acquires ownership back (GENERAL->GENERAL),
//      finite 5 s fence wait. Every redraw uses a fresh binary Ready/Returned pair.
// Release twice without Acquire, or Acquire without Release, is invalid (logic_error)
// and does not change state. This class never uses the Device's own command buffer,
// fence or Submit(); it only borrows the same queue.
//
// The caller must delete the GL semaphore/memory objects that imported
// ReadyHandle()/ReturnedHandle() BEFORE destroying the session: the NT handles are
// retained until the session is destroyed (observed driver compatibility condition).
//
// Failure/lifetime: if completion of submitted external work becomes unknown (fence
// timeout/error, submit failure, GL error after Release -> caller Abandon(), or
// destruction while a handoff is active) the session is quarantined: the device,
// images, pool, fence, semaphores and handles are deliberately never freed and no
// idle-wait is performed, and Device::RetireExternalWork() forbids further core
// submissions. Quarantine is a bounded leak taken instead of a use-after-free or a
// potentially unbounded wait on a driver/GL peer that may never signal.
class ExternalDisplay final {
public:
    // Throws if the device lacks ExternalImagesSupported() or setup fails; ordinary
    // constructor failures free everything.
    explicit ExternalDisplay(std::shared_ptr<Device> device);
    ~ExternalDisplay();
    ExternalDisplay(const ExternalDisplay&) = delete;
    ExternalDisplay& operator=(const ExternalDisplay&) = delete;
    // Win32 NT handles of the exported binary semaphores; owned by the session.
    void* ReadyHandle() const;     // Vulkan signals, GL waits
    void* ReturnedHandle() const;  // GL signals, Vulkan waits
    // Images must belong to the device, be distinct, non-empty external images in
    // GENERAL layout. Retained until Acquire() completes. Throws on failure; a throw
    // after submission has quarantined the session.
    void Release(const std::array<std::shared_ptr<Device::Image>,2>& images);
    void Acquire();
    bool Healthy() const;
    // Caller found the handoff in an unknown state. Never frees or waits. Before any
    // Release (or after a completed Acquire) it only makes the session unusable.
    void Abandon() noexcept;
private:
    struct State;
    std::shared_ptr<State> state;
};
}
