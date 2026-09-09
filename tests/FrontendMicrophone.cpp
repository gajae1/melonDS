// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "Platform.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace melonDS;
using namespace std::chrono_literals;

enum { micInputType_Silence, micInputType_External, micInputType_Noise, micInputType_Wav };
constexpr int HK_Mic = 1;

struct CallbackGate
{
    std::binary_semaphore locked{0};
    std::binary_semaphore resume{0};
};
static thread_local CallbackGate* callbackGate = nullptr;

static int LockMicMutex(SDL_mutex* mutex)
{
    const int result = SDL_LockMutex(mutex);
    if (result == 0 && callbackGate)
    {
        CallbackGate* gate = std::exchange(callbackGate, nullptr);
        gate->locked.release();
        gate->resume.acquire();
    }
    return result;
}

// Even accidental fixture calls cannot enumerate or activate a real microphone.
static int CaptureDeviceCount(int) { return 0; }
static SDL_AudioDeviceID OpenCaptureDevice(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int) { return 0; }
static void PauseCaptureDevice(SDL_AudioDeviceID, int) {}
namespace melonDS::Platform { void Log(LogLevel, const char*, ...) {} }

struct MicrophoneState
{
    std::atomic<double> curFPS{60};
    SDL_AudioDeviceID micDevice = 0;
    int micFreq = 48000, micBufSize = 1024;
    float micSampleFrac = 0;
    s16 micExtBuffer[4096] = {};
    u32 micExtBufferWritePos = 0, micExtBufferCount = 0;
    s16* micBuffer = micExtBuffer;
    u32 micBufferLength = 4096, micBufferReadPos = 0;
    SDL_mutex* micLock = SDL_CreateMutex();
    int micInputType = micInputType_External;
    std::string micDeviceName;
    bool micHotkey = false;

    ~MicrophoneState() { SDL_DestroyMutex(micLock); }
    bool hotkeyDown(int) { return micHotkey; }
    void micOpen();
    int micGetNumSamplesIn(int inlen);
    void micResample(s16* inbuf, int inlen);
    int micReadInput(s16* data, int maxlength);
    static void micCallback(void* data, Uint8* stream, int len);
};

// Extract the current production definitions using tests/ExtractFunction.py.
#define EmuInstance MicrophoneState
#define SDL_LockMutex LockMicMutex
#define SDL_GetNumAudioDevices CaptureDeviceCount
#define SDL_OpenAudioDevice OpenCaptureDevice
#define SDL_PauseAudioDevice PauseCaptureDevice
#include "micOpen.inc"
#undef SDL_GetNumAudioDevices
#undef SDL_OpenAudioDevice
#undef SDL_PauseAudioDevice
#include "micGetNumSamplesIn.inc"
#include "micResample.inc"
#include "micReadInput.inc"
#include "micCallback.inc"
#undef SDL_LockMutex
#undef EmuInstance

// The logical input ends immediately before an inaccessible page. No extra
// sample is allocated for the interpolator to borrow from the next callback.
struct GuardedInput
{
    void* allocation = nullptr;
    size_t pageSize = 0;
    s16* samples = nullptr;
    explicit GuardedInput(size_t count)
    {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        pageSize = info.dwPageSize;
        if (count * sizeof(s16) > pageSize) return;
        allocation = VirtualAlloc(nullptr, pageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!allocation) return;
        DWORD oldProtect;
        if (!VirtualProtect(static_cast<char*>(allocation) + pageSize, pageSize, PAGE_NOACCESS, &oldProtect))
            return;
#else
        pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        if (count * sizeof(s16) > pageSize) return;
        allocation = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (allocation == MAP_FAILED) { allocation = nullptr; return; }
        if (mprotect(static_cast<char*>(allocation) + pageSize, pageSize, PROT_NONE)) return;
#endif
        samples = reinterpret_cast<s16*>(static_cast<char*>(allocation) + pageSize) - count;
    }
    ~GuardedInput()
    {
        if (!allocation) return;
#ifdef _WIN32
        VirtualFree(allocation, 0, MEM_RELEASE);
#else
        munmap(allocation, pageSize * 2);
#endif
    }
};

static void Produce(MicrophoneState& state, s16* input, int count)
{
    MicrophoneState::micCallback(&state, reinterpret_cast<Uint8*>(input), count * sizeof(s16));
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };

    if (!std::strcmp(argv[1], "resample-control"))
    {
        // Hand-calculated ramp values preserve the existing -0.5 sample phase.
        std::array<s16, 4> ramp{0, 600, 1200, 1800};
        for (int frequency : {47743, 95486})
        {
            MicrophoneState state;
            if (!state.micLock) return 2;
            state.micFreq = frequency;
            state.micExtBufferCount = 1024; // Neither +6 nor -6 correction.
            state.micExtBufferWritePos = 1024;
            Produce(state, ramp.data(), ramp.size());
            const std::array<s16, 4> expected{ -300, frequency == 47743 ? s16{300} : s16{900}, 900, 1500 };
            const int count = frequency == 47743 ? 4 : 2;
            check(state.micExtBufferCount == 1024 + count, "Nominal/downsample output length changed");
            check(std::equal(expected.begin(), expected.begin() + count, state.micExtBuffer + 1024),
                  "Normal interpolation or initial sample phase changed");
        }

        // 32 samples at 48 kHz produce 31 nominal guest samples. Keep the
        // existing ring-fill correction and capacity behavior, including wrap.
        std::array<s16, 33> constant;
        constant.fill(700);
        for (const auto [queued, produced] : {std::pair{0, 37}, {1024, 31}, {3073, 25}, {4096, 0}})
        {
            MicrophoneState state;
            state.micExtBufferCount = queued;
            state.micExtBufferWritePos = 4090;
            state.micBufferReadPos = (4090 + 4096 - queued) % 4096;
            std::fill(std::begin(state.micExtBuffer), std::end(state.micExtBuffer), -1234);
            Produce(state, constant.data(), 32);
            check(state.micExtBufferCount == queued + produced, "Ring-fill output correction changed");
            check(state.micExtBufferWritePos == (4090 + produced) % 4096, "Mic producer wrap changed");
            for (int i = 0; i < 4096; ++i)
                check(state.micExtBuffer[(4090 + i) % 4096] == (i < produced ? 700 : -1234),
                      "Producer changed an unwritten sample or damaged the constant waveform");
        }

        MicrophoneState state;
        std::array<s16, 8> output;
        output.fill(1234);
        check(state.micReadInput(output.data(), output.size()) == 0 && output.front() == 1234,
              "An empty external queue changed the guest input contract");
        state.micInputType = micInputType_Silence;
        check(state.micReadInput(output.data(), output.size()) == output.size() &&
              std::all_of(output.begin(), output.end(), [](s16 value) { return value == 0; }),
              "Silent microphone input changed");
    }
    else if (!std::strcmp(argv[1], "resample-window"))
    {
        // At 3x, the final sample position falls after the last input sample.
        // A physically readable next element still belongs outside this block.
        constexpr std::array<s16, 12> expected{-300, -100, 100, 300, 500, 700, 900, 1100, 1300, 1500, 1700, 1800};
        for (s16 outside : {s16{30000}, s16{-30000}})
        {
            std::array<s16, 5> input{0, 600, 1200, 1800, outside};
            MicrophoneState state;
            state.micFreq = 15914;
            state.micExtBufferCount = 1024;
            state.micExtBufferWritePos = 1024;
            Produce(state, input.data(), 4);
            check(state.micExtBufferCount == 1036, "Upsampling output length changed");
            check(std::equal(expected.begin(), expected.end(), state.micExtBuffer + 1024),
                  "Interpolation borrowed a sample outside the logical callback window");
        }
    }
    else if (!std::strcmp(argv[1], "resample-guard"))
    {
        GuardedInput input(1024);
        if (!input.samples) return 2;
        std::fill_n(input.samples, 1024, s16{700});
        for (u32 queued : {0u, 1024u})
        {
            MicrophoneState state;
            state.micFreq = 22050;
            state.micExtBufferCount = queued;
            state.micExtBufferWritePos = queued;
            std::fprintf(stderr, "22,050 Hz, 1024 samples ending at guard page; queued=%u\n", queued);
            Produce(state, input.samples, 1024);
            const int count = queued == 0 ? 2223 : 2217;
            check(state.micExtBufferCount == queued + count, "Guarded callback output length changed");
            check(std::all_of(state.micExtBuffer + queued, state.micExtBuffer + queued + count,
                              [](s16 value) { return value == 700; }), "Guarded constant waveform changed");
        }
    }
    else if (!std::strcmp(argv[1], "resample-tiny"))
    {
        GuardedInput input(1);
        if (!input.samples) return 2;
        input.samples[0] = 700;
        MicrophoneState state;
        std::fprintf(stderr, "One sample ending at guard page\n");
        Produce(state, input.samples, 1);
        check(state.micExtBufferCount == 6 && state.micExtBuffer[5] == 700,
              "One-sample input did not hold its endpoint");
        const u32 before = state.micExtBufferCount;
        std::fprintf(stderr, "Zero-length input points at inaccessible page\n");
        Produce(state, input.samples + 1, 0);
        check(state.micExtBufferCount == before, "Empty callback generated samples");
        state.micExtBufferCount = 3073;
        state.micBufferReadPos = (state.micExtBufferWritePos + 4096 - 3073) % 4096;
        Produce(state, input.samples, 1);
        check(state.micExtBufferCount == 3073, "Nonpositive corrected output length wrote samples");
    }
    else if (!std::strcmp(argv[1], "producer-consumer"))
    {
        MicrophoneState state;
        if (!state.micLock) return 2;
        std::array<s16, 33> input;
        input.fill(700);
        std::array<s16, 64> output;
        output.fill(-1234);
        CallbackGate gate;
        std::binary_semaphore started{0}, finished{0};
        int consumed = -1;
        std::thread producer([&] { callbackGate = &gate; Produce(state, input.data(), 32); });
        if (!gate.locked.try_acquire_for(2s))
        {
            gate.resume.release();
            producer.join();
            return 2;
        }
        std::thread consumer([&] {
            started.release();
            consumed = state.micReadInput(output.data(), output.size());
            finished.release();
        });
        const bool consumerStarted = started.try_acquire_for(2s);
        const bool returnedWhileLocked = consumerStarted && finished.try_acquire_for(150ms);
        gate.resume.release();
        producer.join();
        consumer.join();
        if (!consumerStarted) return 2;
        check(!returnedWhileLocked, "Empty-count read returned while the producer held micLock");
        check(consumed == 37 && state.micExtBufferCount == 0,
              "Consumer missed the block published by the producer it should have waited for");
        check(std::all_of(output.begin(), output.begin() + 37, [](s16 value) { return value == 700; }) &&
              std::all_of(output.begin() + 37, output.end(), [](s16 value) { return value == -1234; }),
              "Producer/consumer handoff lost samples or exceeded the returned length");
    }
    else if (!std::strcmp(argv[1], "reopen-preserve"))
    {
        MicrophoneState state;
        state.micDevice = 1; // An already open fixture device; never passed to SDL.
        state.micExtBufferCount = 8;
        state.micExtBufferWritePos = 11;
        state.micBufferReadPos = 3;
        state.micSampleFrac = 0.25f;
        std::fill(std::begin(state.micExtBuffer), std::end(state.micExtBuffer), 700);
        state.micOpen();
        check(state.micExtBufferCount == 8 && state.micExtBufferWritePos == 11 &&
              state.micBufferReadPos == 3 && state.micSampleFrac == 0.25f,
              "Reopening an active microphone reset its shared queue state");
        check(std::all_of(std::begin(state.micExtBuffer), std::end(state.micExtBuffer),
                          [](s16 value) { return value == 700; }), "Reopening erased queued microphone samples");
    }
    else return 2;

    std::printf("%s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
