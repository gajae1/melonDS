// SPDX-License-Identifier: GPL-3.0-or-later
// Compile on Windows; include the selected baseline/candidate AudioOutput.cpp.
#include "SDLCompat.h"
#include "miniaudio/DeviceOnly.h"
#include "Platform.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace melonDS::Platform { void Log(LogLevel, const char*, ...) {} }
namespace fixture {
static std::atomic<int> failures{0};
static void Check(bool ok, const char* message)
{
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct Native {
    ma_device* device = nullptr;
    ma_context* context = nullptr;
    ma_device_data_proc data = nullptr;
    ma_device_notification_proc notify = nullptr;
    void* userdata = nullptr;
    std::thread::id owner;
    unsigned starts = 0, deviceCloses = 0, contextCloses = 0;
};
static Native records[4];
static unsigned count = 0;
static ma_uint32 endpointPeriod = 256;
static std::thread::id client;
static Native& Live(ma_device* device)
{
    for (unsigned i = 0; i < count; ++i)
        if (records[i].device == device && !records[i].deviceCloses) return records[i];
    std::fprintf(stderr, "Unexpected native device access\n");
    std::abort();
}
static void Event(Native& n, ma_device_notification_type type)
{
    ma_device_notification event{};
    event.pDevice = n.device;
    event.type = type;
    n.notify(&event);
}
static ma_result ContextInit(const ma_backend* backends, ma_uint32 backendCount,
                             const ma_context_config*, ma_context* context)
{
    Check(backendCount == 1 && backends && *backends == ma_backend_wasapi,
          "only the WASAPI context is requested");
    Check(std::this_thread::get_id() != client, "context initialized on owner thread");
    *context = {};
    return MA_SUCCESS;
}
static ma_result ContextUninit(ma_context* context)
{
    for (unsigned i = 0; i < count; ++i)
    {
        Native& n = records[i];
        if (n.context != context || n.contextCloses) continue;
        Check(std::this_thread::get_id() == n.owner, "context uninit on original owner");
        Check(n.deviceCloses == 1, "device released before context");
        ++n.contextCloses;
        return MA_SUCCESS;
    }
    Check(false, "unexpected or duplicate context uninit");
    return MA_SUCCESS;
}
static ma_result DeviceInit(ma_context* context, const ma_device_config* config, ma_device* device)
{
    if (count == 4) return MA_ERROR;
    Native& n = records[count++];
    n.device = device; n.context = context;
    n.data = config->dataCallback; n.notify = config->notificationCallback;
    n.userdata = config->pUserData; n.owner = std::this_thread::get_id();
    Check(n.owner != client, "device initialized on owner thread");
    Check(n.data && n.notify && n.userdata, "native callbacks and userdata captured");
    Check(!config->playback.pDeviceID && config->periodSizeInFrames == 256,
          "default endpoint with original 256-frame request");
    *device = {};
    device->pContext = context;
    device->pUserData = n.userdata;
    device->sampleRate = 48000;
    device->playback.internalPeriodSizeInFrames = endpointPeriod;
    device->wasapi.actualBufferSizeInFramesPlayback = endpointPeriod * 2;
    return MA_SUCCESS;
}
static ma_result DeviceStart(ma_device* device)
{
    Native& n = Live(device);
    ++n.starts;
    Event(n, ma_device_notification_type_started);
    return MA_SUCCESS;
}
static void DeviceUninit(ma_device* device)
{
    Native& n = Live(device);
    Check(std::this_thread::get_id() == n.owner, "device uninit on original owner");
    ++n.deviceCloses;
}
} // namespace fixture

#define ma_context_init fixture::ContextInit
#define ma_context_uninit fixture::ContextUninit
#define ma_device_init fixture::DeviceInit
#define ma_device_start fixture::DeviceStart
#define ma_device_uninit fixture::DeviceUninit
#include "AudioOutput.cpp"
#undef ma_device_uninit
#undef ma_device_start
#undef ma_device_init
#undef ma_context_uninit
#undef ma_context_init

static void Silence(void*, uint8_t* data, int bytes) { std::memset(data, 0, bytes); }
static bool SameSpec(const AudioOutput::Spec& a, const AudioOutput::Spec& b)
{
    return a.rate == b.rate && a.frames == b.frames &&
           a.bufferFrames == b.bufferFrames && a.backend == b.backend;
}
static void Scenario(ma_uint32 migratedPeriod)
{
    using namespace fixture;
    endpointPeriod = 256;
    const unsigned first = count;
    AudioOutput output;
    std::string error;
    const AudioOutput::Settings settings{AudioOutput::WASAPIShared, {}, 256};
    if (!output.Open(settings, Silence, nullptr, error))
    { Check(false, error.c_str()); return; }
    Check(output.Start(error), "initial client start");
    Check(output.IsRunning() && !output.NeedsRecovery(), "healthy initial output");
    const AudioOutput::Spec original = output.GetSpec();
    Check(original.frames == 256 && original.rate == 48000 && original.bufferFrames == 512,
          "initial negotiated spec");
    output.Stop();
    Check(!output.IsRunning() && !output.NeedsRecovery(), "ordinary client pause needs no recovery");
    Check(output.Start(error) && !output.NeedsRecovery(), "ordinary client resume needs no recovery");

    Native& old = records[first];
    Event(old, ma_device_notification_type_stopped);
    Check(output.NeedsRecovery(), "native stop requests recovery");
    endpointPeriod = migratedPeriod;
    old.device->playback.internalPeriodSizeInFrames = endpointPeriod;
    old.device->wasapi.actualBufferSizeInFramesPlayback = endpointPeriod * 2;
    Event(old, ma_device_notification_type_rerouted);
    Check(SameSpec(output.GetSpec(), original), "reroute notification does not mutate public spec");
    Event(old, ma_device_notification_type_started);
    Check(output.IsRunning(), "native restart restores running state");
    Check(output.NeedsRecovery(), "reroute latch survives native started (including same period)");
    Check(SameSpec(output.GetSpec(), original), "public spec remains old after native restart");
    output.Stop();
    Check(output.NeedsRecovery(), "reroute latch survives intentional client Stop");
    Check(output.Start(error), "client restart after reroute");
    Check(output.NeedsRecovery(), "reroute latch survives intentional client Start");
    Check(SameSpec(output.GetSpec(), original), "client restart cannot silently refresh spec");
    Check(old.deviceCloses == 0, "notification cannot destroy native device");

    if (!output.BeginReopen(settings, Silence, nullptr, error))
    { Check(false, error.c_str()); return; }
    Check(SameSpec(output.GetSpec(), original), "spec adoption waits for FinishReopen");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!output.IsOpenReady() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!output.IsOpenReady() || !output.FinishReopen(error))
    { Check(false, "asynchronous owner reopen completes"); return; }
    Check(count == first + 2, "one replacement device initialized");
    Check(old.deviceCloses == 1 && old.contextCloses == 1, "retired native resources released exactly once");
    Check(records[first + 1].owner == old.owner, "replacement uses existing owner thread");
    Check(output.GetSpec().frames == static_cast<int>(migratedPeriod) &&
          output.GetSpec().bufferFrames == static_cast<int>(migratedPeriod * 2) &&
          output.GetSpec().rate == 48000, "replacement adopts endpoint period/capacity");
    Check(!output.NeedsRecovery(), "new Impl clears sticky reroute latch");
    Check(output.Start(error) && !output.NeedsRecovery(), "replacement starts without recovery");
    Check(output.Close(), "final close completes");
    Check(output.Close(), "repeated close is harmless");
    for (unsigned i = first; i < count; ++i)
        Check(records[i].deviceCloses == 1 && records[i].contextCloses == 1,
              "each native generation uninitialized exactly once");
}
int main(int argc, char** argv)
{
    fixture::client = std::this_thread::get_id();
    Scenario(480);
    Scenario(256); // ANY reroute invalidates the snapshot, even unchanged periods.
    const int failures = fixture::failures.load();
    std::printf("WASAPI reroute sticky recovery: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
