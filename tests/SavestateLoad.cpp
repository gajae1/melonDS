// SPDX-License-Identifier: GPL-3.0-or-later
// Real core + current frontend load/undo functions; no user ROM/BIOS or physical devices.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <SDL2/SDL.h>
#include "ARM.h"
#include "NDS.h"
#include "NDSCart/CartRetailIR.h"
#include "Platform.h"
#include "StateLoadResult.h"
#include "AudioLowPass.h"
#include "AudioOutputRamp.h"
#include "AudioDiagnostics.h"
#include "AudioOutput.h"
using namespace melonDS;

enum class ReadFailure { None, Short, Error, Oversize };
static ReadFailure readFailure = ReadFailure::None;
static unsigned opens = 0, closes = 0, reads = 0;

// Redirect only the extracted frontend's file operations. The core keeps its
// headless host, so it cannot accidentally read firmware or persist guest saves.
namespace melonDS::Platform
{
FileHandle* OpenStateFile(const std::string& path, FileMode)
{
    auto file = std::make_unique<QFile>(QString::fromStdString(path));
    if (!file->open(QIODevice::ReadOnly)) return nullptr;
    ++opens;
    return reinterpret_cast<FileHandle*>(file.release());
}
bool CloseStateFile(FileHandle* handle)
{
    delete reinterpret_cast<QFile*>(handle);
    ++closes;
    return true;
}
u64 StateFileLength(FileHandle* handle)
{
    if (readFailure == ReadFailure::Oversize) return u64(UINT32_MAX) + 1;
    return reinterpret_cast<QFile*>(handle)->size();
}
u64 ReadStateFile(void* bytes, u64 size, u64 count, FileHandle* handle)
{
    ++reads;
    if (readFailure == ReadFailure::Error) return u64(-1);
    auto length = static_cast<qint64>(size * count);
    if (readFailure == ReadFailure::Short) --length;
    qint64 result = reinterpret_cast<QFile*>(handle)->read(static_cast<char*>(bytes), length);
    return result > 0 ? result / size : result;
}
}

struct FixtureConsole : NDS
{
    using NDS::NDS;
    unsigned loadCalls = 0;
    unsigned failLoads = 0;
    bool failSave = false;
    bool throwOnLoad = false;
    u32 extra = 0x12345678;
    void DoSavestateExtra(Savestate* state) override
    {
        // A final section models a device that fails after RAM/CPU/devices have
        // already been visited, without duplicating their serialized layouts.
        state->Section("TEST");
        state->Var32(&extra);
        if (state->Saving)
        {
            if (failSave) state->Error = true;
        }
        else if ((failLoads >> loadCalls++) & 1u)
        {
            if (throwOnLoad) throw std::bad_alloc();
            state->Error = true;
        }
    }
};

struct StateReader
{
    FixtureConsole* nds;
    std::unique_ptr<Savestate> backupState;
    AudioOutput audioDevice;
    SDL_mutex* audioSyncLock = SDL_CreateMutex();
    SDL_cond* audioSyncCond = SDL_CreateCond();
    SDL_sem* captured = SDL_CreateSemaphore(0);
    int audioFreq = 48000, audioBufSize = 128;
    AudioLowPass audioLowPass;
    AudioOutputRamp audioOutputRamp;
    AudioDiagnostics audioDiagnostics;
    std::atomic<int> audioLowPassCutoff{1000}, audioVolume{256};
    bool audioMutedByWindowFocus = false, audioMutedToggle = false, audioMutedByFastForward = false;
    bool micStarted = false, received = false;
    std::array<s16, 256> pcm{};
    ~StateReader()
    {
        audioDevice.Close();
        SDL_DestroySemaphore(captured);
        SDL_DestroyCond(audioSyncCond);
        SDL_DestroyMutex(audioSyncLock);
    }
    void micOpen() { std::abort(); }
    void micClose() { std::abort(); }
    void audioEnable();
    void audioDisable();
    void audioResetOutput();
    void audioReportDiagnostics();
    static void audioCallback(void*, Uint8*, int);
    bool openAudio()
    {
        const auto callback = [](void* data, Uint8* stream, int len) {
            auto& self = *static_cast<StateReader*>(data);
            // Consume exactly one callback per resume; further dummy-device
            // requests stay silent until the main thread pauses it again.
            if (!self.received)
            {
                if (len != sizeof(self.pcm)) std::abort();
                audioCallback(data, stream, len);
                std::memcpy(self.pcm.data(), stream, len);
                self.received = true;
                SDL_SemPost(self.captured);
            }
            std::memset(stream, 0, len); // Never submit probe PCM to a real device.
        };
        std::string error;
        if (!audioDevice.Open({AudioOutput::SDL, {}, audioBufSize}, callback, this, error)) return false;
        audioLowPass.Init(audioFreq);
        audioLowPass.SetCutoffNow(audioLowPassCutoff);
        audioOutputRamp.Init(audioFreq);
        return static_cast<bool>(audioDevice);
    }
    std::array<s16, 256> nextCallback()
    {
        received = false;
        audioEnable();
        const int result = SDL_SemWaitTimeout(captured, 2000);
        audioDisable();
        if (result != 0) throw std::runtime_error("dummy audio callback timed out");
        return pcm;
    }
    StateLoadResult applyState(Savestate& state, bool undo);
    StateLoadResult loadState(const std::string& filename);
    StateLoadResult undoStateLoad();
};
#define EmuInstance StateReader
#include "audioCallback.inc"
#include "stateAudioEnable.inc"
#include "stateAudioDisable.inc"
#include "stateAudioReport.inc"
#include "stateAudioReset.inc"
#define OpenFile OpenStateFile
#define CloseFile CloseStateFile
#define FileLength StateFileLength
#define FileRead ReadStateFile
#include "loadState.inc"
#include "applyState.inc"
#include "undoStateLoad.inc"
#undef FileRead
#undef FileLength
#undef CloseFile
#undef OpenFile
#undef EmuInstance

static std::vector<u8> Snapshot(NDS& nds)
{
    Savestate state;
    if (!nds.DoSavestate(&state) || state.Error) throw std::runtime_error("fixture serialization failed");
    auto* bytes = static_cast<const u8*>(state.Buffer());
    return {bytes, bytes + state.Length()};
}

static void Prepare(NDS& nds, u32 increment)
{
    nds.Reset();
    // This loop changes RAM each frame, and changing its instruction also
    // exercises invalidation of previously compiled blocks on rollback.
    const u32 code[] = {0xE3A01402, 0xE3A00000, 0xE2800000 | increment,
                        0xE5810100, 0xEAFFFFFC};
    for (unsigned i = 0; i < std::size(code); ++i)
        nds.ARM9Write32(0x02000000 + 4 * i, code[i]);
    nds.ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds.ARM9.JumpTo(0x02000000);
    nds.ARM7.JumpTo(0x02000200);
    nds.Start();
    nds.RunFrame();
}

static std::unique_ptr<NDSCart::CartCommon> MakeCart(bool infrared, u8 identity)
{
    // The actual cartridge classes compute their identity from this generated
    // header. No serialized offsets or checksum implementation are duplicated.
    auto rom = std::make_unique<u8[]>(0x1000);
    rom[0] = identity;
    ROMListEntry params{0, 0x1000, 1};
    if (infrared)
        return std::make_unique<NDSCart::CartRetailIR>(std::move(rom), 0x1000,
            0, 1, false, params, nullptr, 0, nullptr);
    return std::make_unique<NDSCart::CartRetail>(std::move(rom), 0x1000,
        0, false, params, nullptr, 0, nullptr);
}

static int AudioHistory(const std::string& test)
{
    // Reuse the actual core/load/late-section fixture above and the actual SDL
    // callback below. Only the backend is dummy; no sound device or mic is used.
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return 2;
    struct Observation
    {
        std::array<s16, 256> first{}, next{};
        int queuedBefore = 0, queuedAfter = 0;
        bool corePreserved = false;
        StateLoadResult result = StateLoadResult::Failed;
    };
    const bool success = test == "audio-success" || test == "audio-rebase";
    const auto observe = [&](bool load)
    {
        NDSArgs args; args.JIT.reset();
        auto console = std::make_unique<FixtureConsole>(std::move(args));
        auto& nds = *console;
        StateReader reader{&nds};
        if (!reader.openAudio()) throw std::runtime_error(SDL_GetError());
        Prepare(nds, 3);
        if (test == "audio-rebase")
        {
            nds.ARM7Write16(0x04000304, 1);
            nds.ARM7Write16(0x04000504, 0x240);
            nds.RunFrame();
        }
        auto target = Snapshot(nds);
        Prepare(nds, 1);
        nds.SPU.Stop();
        nds.ARM7Write16(0x04000304, 1);
        nds.ARM7Write16(0x04000504, 0x280);
        nds.RunFrame();
        reader.nextCallback(); // Prime real frontend filter history too.
        const auto previous = Snapshot(nds);
        nds.loadCalls = 0;
        Observation observed;
        observed.queuedBefore = nds.SPU.GetOutputSize();
        if (load)
        {
            if (test == "audio-rollback") target[target.size() - 20] = 'X';
            if (test == "audio-preflight") target[0] = 'X';
            QTemporaryDir directory;
            if (!directory.isValid()) throw std::runtime_error("temporary directory");
            const QString name = directory.filePath("generated-audio.mln");
            QFile file(name);
            if (!file.open(QIODevice::WriteOnly) || file.write(reinterpret_cast<const char*>(target.data()), target.size()) != qint64(target.size()))
                throw std::runtime_error("temporary state write");
            file.close();
            observed.result = reader.loadState(name.toStdString());
        }
        observed.corePreserved = Snapshot(nds) == (load && success ? target : previous);
        observed.queuedAfter = nds.SPU.GetOutputSize();
        observed.first = reader.nextCallback();
        // Pass the old queue through real callbacks before checking newly
        // produced audio, so a rollback also proves the pending blip history.
        while (nds.SPU.GetOutputSize()) reader.nextCallback();
        nds.RunFrame();
        observed.next = reader.nextCallback(); // Includes the pending blip tail.
        return observed;
    };
    const auto control = observe(false);
    const auto loaded = observe(true);
    const auto peak = [](const auto& pcm) {
        int value = 0;
        for (s16 sample : pcm) value = std::max(value, std::abs(int(sample)));
        return value;
    };
    bool passed = loaded.corePreserved && loaded.queuedBefore > 128 && peak(control.first) > 100;
    if (success)
    {
        passed &= loaded.result == StateLoadResult::Success && peak(loaded.first) == 0;
        if (test == "audio-rebase")
            passed &= *std::max_element(loaded.next.begin(), loaded.next.end()) > 500;
        else passed &= peak(loaded.next) == 0;
    }
    else
        passed &= loaded.result == StateLoadResult::Failed && loaded.queuedAfter == loaded.queuedBefore &&
            loaded.first == control.first && loaded.next == control.next;
    std::printf("%s: %s result=%d queue=%d->%d first_peak=%d next_peak=%d control_peak=%d "
                "resume_equal=%d core_snapshot_equal=%d backend=dummy\n", test.c_str(), passed ? "PASS" : "FAIL",
        int(loaded.result), loaded.queuedBefore, loaded.queuedAfter, peak(loaded.first), peak(loaded.next),
        peak(control.first), loaded.first == control.first && loaded.next == control.next, loaded.corePreserved);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return passed ? 0 : 1;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 3) return 2;
    const std::string test = argv[1];
    if (test.starts_with("audio-")) return AudioHistory(test);
    const bool jit = std::strcmp(argv[2], "interpreter") != 0;
#ifndef JIT_ENABLED
    if (jit) return 77;
#endif
    NDSArgs args;
    if (!jit) args.JIT = std::nullopt;
    else args.JIT->FastMemory = std::strcmp(argv[2], "fastmem") == 0;
    auto console = std::make_unique<FixtureConsole>(std::move(args));
    auto& nds = *console;
    StateReader reader{&nds};
    const bool cartTest = test.starts_with("cart-");
    if (cartTest && test != "cart-unexpected") nds.SetNDSCart(MakeCart(false, 1));
    Prepare(nds, 3);
    auto target = Snapshot(nds);
    if (cartTest)
    {
        if (test == "cart-missing") nds.SetNDSCart(nullptr);
        else nds.SetNDSCart(MakeCart(test == "cart-type", test == "cart-checksum" ? 2 : 1));
    }
    Prepare(nds, 1);
    const auto previous = Snapshot(nds);
    nds.RunFrame();
    const auto nextFrame = Snapshot(nds);
    Savestate restore(const_cast<u8*>(previous.data()), previous.size(), false);
    if (!nds.DoSavestate(&restore) || restore.Error) return 2;
    nds.loadCalls = 0;

    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    const auto path = directory.filePath(QStringLiteral("state-\uD55C\uAE00.mln")).toStdString();
    if (test == "late-section") target[target.size() - 20] = 'X';
    if (test == "missing-global") target[16] = 'X';
    if (test == "empty-global")
    {
        u32 sectionLength = 0;
        std::memcpy(&sectionLength, target.data() + 20, sizeof(sectionLength));
        target.erase(target.begin() + 32, target.begin() + 16 + sectionLength);
        sectionLength = 16;
        std::memcpy(target.data() + 20, &sectionLength, sizeof(sectionLength));
    }
    if (test == "short-section")
    {
        // The final TEST device lacks its word. A valid unknown section follows,
        // so a file-wide bounds check alone consumes its magic as device data.
        target.resize(target.size() - 4);
        u32 sectionLength = 16;
        std::memcpy(target.data() + target.size() - 12, &sectionLength, sizeof(sectionLength));
        const u8 tail[] = {'T', 'A', 'I', 'L', 20, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, 0, 0, 0x12, 0x34, 0x56, 0x78};
        target.insert(target.end(), std::begin(tail), std::end(tail));
    }
    if (test == "empty-global" || test == "short-section")
    {
        const u32 size = static_cast<u32>(target.size());
        std::memcpy(target.data() + 8, &size, sizeof(size));
    }
    if (test == "missing-global" || test == "empty-global")
    {
        Savestate early(target.data(), static_cast<u32>(target.size()), false);
        const bool rejected = !nds.DoSavestate(&early);
        if (!rejected || !early.Error || Snapshot(nds) != previous || nds.loadCalls)
        {
            std::fprintf(stderr, "%s: invalid NDSG did not reject before applying core state\n", test.c_str());
            return 1;
        }
    }
    if (test == "header") target[0] = 'X';
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char*>(target.data()), target.size()) != qint64(target.size())) return 2;
    file.close();
    if (test == "short-read") readFailure = ReadFailure::Short;
    if (test == "read-error") readFailure = ReadFailure::Error;
    if (test == "oversize") readFailure = ReadFailure::Oversize;
    if (test == "backup-error") nds.failSave = true;
    if (test == "load-error" || test == "load-oom") nds.failLoads = 1;
    if (test == "load-oom") nds.throwOnLoad = true;
    if (test == "rollback-error") nds.failLoads = 3;

    StateLoadResult result = StateLoadResult::Failed;
    try { result = reader.loadState(path); }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s: escaped exception: %s\n", test.c_str(), e.what());
        return 1;
    }
    nds.failSave = false;
    const bool accepted = result == StateLoadResult::Success;
    bool passed;
    if (test == "success" || test == "cart-match" || test == "undo-error")
    {
        passed = accepted && Snapshot(nds) == target;
        if (test == "undo-error") nds.failLoads = 1u << nds.loadCalls;
        const auto undone = reader.undoStateLoad();
        passed &= undone == (test != "undo-error" ? StateLoadResult::Success : StateLoadResult::Failed);
        passed &= Snapshot(nds) == (test != "undo-error" ? previous : target);
        if (test == "undo-error")
        {
            passed &= reader.undoStateLoad() == StateLoadResult::Success;
            passed &= Snapshot(nds) == previous;
        }
        passed &= reader.undoStateLoad() == StateLoadResult::Failed && !reader.backupState;
    }
    else if (test == "rollback-error")
    {
        passed = result == StateLoadResult::RecoveryFailed && !nds.IsRunning() && !reader.backupState;
    }
    else
    {
        passed = result == StateLoadResult::Failed && Snapshot(nds) == previous;
        if (passed)
        {
            nds.RunFrame();
            passed = nds.IsRunning() && Snapshot(nds) == nextFrame;
        }
    }
    passed &= opens == closes;
    if (test == "oversize") passed &= reads == 0;
    if (test == "header" || test == "short-read" || test == "read-error" ||
        test == "oversize" || test == "backup-error") passed &= nds.loadCalls == 0;
    std::printf("state-load %s/%s: %s (accepted=%d loads=%u files=%u/%u reads=%u)\n",
                test.c_str(), argv[2], passed ? "PASS" : "FAIL", accepted,
                nds.loadCalls, opens, closes, reads);
    return passed ? 0 : 1;
}
