// SPDX-License-Identifier: GPL-3.0-or-later
// Real core + current frontend load/undo functions; no user ROM/BIOS or devices.
#include <algorithm>
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
#include "ARM.h"
#include "NDS.h"
#include "Platform.h"
#include "StateLoadResult.h"
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
    StateLoadResult applyState(Savestate& state, bool undo);
    StateLoadResult loadState(const std::string& filename);
    StateLoadResult undoStateLoad();
};
#define EmuInstance StateReader
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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 3) return 2;
    const std::string test = argv[1];
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
    Prepare(nds, 3);
    auto target = Snapshot(nds);
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
    if (test == "success" || test == "undo-error")
    {
        passed = accepted && Snapshot(nds) == target;
        if (test == "undo-error") nds.failLoads = 1u << nds.loadCalls;
        const auto undone = reader.undoStateLoad();
        passed &= undone == (test == "success" ? StateLoadResult::Success : StateLoadResult::Failed);
        passed &= Snapshot(nds) == (test == "success" ? previous : target);
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
