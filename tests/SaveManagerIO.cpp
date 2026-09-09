// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdarg>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSemaphore>
#include <QTemporaryDir>
#ifdef _WIN32
#include <windows.h>
#endif
#include "Platform.h"
#include "SaveManager.h"

using namespace melonDS;

struct FlushGate
{
    QSemaphore reached;
    QSemaphore resume;
    std::atomic_bool armed{true};
};

static std::atomic<FlushGate*> activeFlushGate{nullptr};

// Declare this after the manager and the gate before it: release a paused worker
// on every exit, then join the manager before destroying the gate's semaphores.
struct FlushGateScope
{
    FlushGate& gate;
    explicit FlushGateScope(FlushGate& value) : gate(value) { activeFlushGate = &gate; }
    ~FlushGateScope() { Release(); }
    void Release()
    {
        activeFlushGate = nullptr;
        gate.resume.release();
    }
};

// The production frontend's QFile I/O, limited to the modes SaveManager uses.
// Linking the full Platform.cpp would also require the emulator and SDL GUI.
namespace melonDS::Platform
{
FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    auto file = std::make_unique<QFile>(QString::fromStdString(path));
    if (!file->open(mode == FileMode::Read ? QIODevice::ReadOnly :
                       QIODevice::WriteOnly | QIODevice::Truncate))
        return nullptr;
    return reinterpret_cast<FileHandle*>(file.release());
}

bool CloseFile(FileHandle* file)
{
    auto qfile = reinterpret_cast<QFile*>(file);
    qfile->close();
    delete qfile;
    return true;
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    qint64 read = reinterpret_cast<QFile*>(file)->read(static_cast<char*>(data), size * count);
    return read > 0 ? read / size : read;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    qint64 written = reinterpret_cast<QFile*>(file)->write(static_cast<const char*>(data), size * count);
    return written > 0 ? written / size : written;
}

u64 FileLength(FileHandle* file)
{
    return reinterpret_cast<QFile*>(file)->size();
}

void Log(LogLevel, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);

    // The real SaveManager emits this after committing the file, while it still
    // holds its flush lock and before acknowledging the version. Pause only here.
    FlushGate* gate = activeFlushGate.load();
    if (gate && !std::strcmp(format, "SaveManager: Wrote %u bytes to %s\n") && gate->armed.exchange(false))
    {
        gate->reached.release();
        gate->resume.acquire();
    }
}
}

static QByteArray Read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static bool Write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

static void Queue(SaveManager& manager, const QByteArray& bytes)
{
    manager.RequestFlush(reinterpret_cast<const u8*>(bytes.constData()),
                         static_cast<u32>(bytes.size()), 0, static_cast<u32>(bytes.size()));
    manager.CheckFlush();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;

    // All fixtures are generated under the test's working directory.
    QTemporaryDir directory(QDir::current().filePath("save-manager-io-XXXXXX"));
    if (!directory.isValid()) return 2;
    const QByteArray previous(4096, '\xA5');
    const QByteArray next = QByteArray::fromHex("007F80FF102030405060708090A0B0C0D0E0F000");
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };

    if (!std::strcmp(argv[1], "retry-open") || !std::strcmp(argv[1], "retry-worker"))
    {
        const QString path = directory.filePath("missing/save.bin");
        const bool worker = !std::strcmp(argv[1], "retry-worker");
        SaveManager manager(worker ? path.toStdString() : std::string());
        if (!worker) manager.SetPath(path.toStdString(), false);
        Queue(manager, next);
        manager.FlushSecondaryBuffer(); // The parent directory does not exist.
        check(manager.NeedsFlush(), "Failed open discarded the pending save");
        check(!QFile::exists(path), "Failed open unexpectedly created the save");
        if (!QDir(directory.path()).mkdir("missing")) return 2;

        // No new RequestFlush: the failed write itself must remain retryable.
        if (worker)
        {
            QDeadlineTimer deadline(4500);
            while (!deadline.hasExpired() && Read(path) != next)
                QThread::msleep(25);
        }
        else
        {
            manager.FlushSecondaryBuffer();
            check(!manager.NeedsFlush(), "Successful retry remained pending");
        }
        check(Read(path) == next, "Recovered path never received the pending save");
    }
    else if (!std::strcmp(argv[1], "replace") || !std::strcmp(argv[1], "retry-rename"))
    {
        const QString path = directory.filePath(QStringLiteral("save-\uD55C\uAE00.bin"));
        {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(previous) != previous.size() || !file.flush())
                return 2;
        }
        SaveManager manager(""); // Drive flushes synchronously without a worker race.
        manager.SetPath(path.toStdString(), false);
        Queue(manager, next);
        if (!std::strcmp(argv[1], "retry-rename"))
        {
#ifdef _WIN32
            // Deny writing and replacement of our fixture, without changing ACLs.
            HANDLE lock = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (lock == INVALID_HANDLE_VALUE) return 2;
            {
                // Confirm this OS lock permits staging but rejects the final commit.
                QSaveFile probe(path);
                check(probe.open(QIODevice::WriteOnly), "Rename fixture cannot stage a temporary file");
                check(probe.write(next) == next.size(), "Rename fixture failed before commit");
                check(!probe.commit(), "Rename fixture unexpectedly allowed replacement");
            }
            manager.FlushSecondaryBuffer();
            check(manager.NeedsFlush(), "Locked save discarded the pending write");
            check(Read(path) == previous, "Failed replacement damaged the previous save");
            CloseHandle(lock);
#else
            return 77;
#endif
        }
        manager.FlushSecondaryBuffer();
        check(!manager.NeedsFlush(), "Successful file replacement remained pending");
        check(Read(path) == next, "File replacement lost bytes or retained the previous tail");
    }
    else if (!std::strcmp(argv[1], "path-during-flush"))
    {
        const QString oldPath = directory.filePath("current-location.bin");
        const QString newPath = directory.filePath("moved-location.bin");
        if (!Write(newPath, previous)) return 2;
        FlushGate gate;
        SaveManager manager(oldPath.toStdString()); // Exercise the actual worker.
        FlushGateScope pause(gate);
        Queue(manager, next);
        if (!gate.reached.tryAcquire(1, 5000))
        {
            std::fprintf(stderr, "Worker did not reach the committed-file flush gate\n");
            return 2;
        }

        QSemaphore setterStarted;
        QSemaphore setterFinished;
        auto setter = std::unique_ptr<QThread>(QThread::create([&] {
            setterStarted.release();
            manager.SetPath(newPath.toStdString(), false);
            setterFinished.release();
        }));
        setter->start();
        const bool started = setterStarted.tryAcquire(1, 1000);
        const bool changedWhileFlushing = started && setterFinished.tryAcquire(1, 1000);
        pause.Release();
        setter->wait();
        if (!started) return 2;
        check(!changedWhileFlushing, "SetPath changed shared state while the worker held the flush lock");

        // reload=false relocates this same game's data. It does not load another
        // game's existing file, and the normal CheckFlush publication is retained.
        check(manager.GetPath() == newPath.toStdString(), "Save path did not change after the flush");
        manager.CheckFlush();
        manager.FlushSecondaryBuffer();
        check(Read(oldPath) == next, "In-flight flush lost the current game's original save");
        check(Read(newPath) == next, "Relocation did not write the current game's data");
        check(!manager.NeedsFlush(), "Completed relocation remained pending");
    }
    else if (!std::strcmp(argv[1], "reload-partial"))
    {
        const QString path = directory.filePath("generated-reload.bin");
        if (!Write(path, previous)) return 2;
        // reset currently calls reload=true on a newly created firmware manager.
        SaveManager manager("");
        manager.SetPath(path.toStdString(), true);
        QByteArray patched = previous;
        patched[5] = '\x3C';
        manager.RequestFlush(reinterpret_cast<const u8*>(patched.constData()),
                             static_cast<u32>(patched.size()), 5, 1);
        manager.CheckFlush();
        manager.FlushSecondaryBuffer();
        check(Read(path) == patched, "Partial update did not preserve the reloaded bytes");
        check(!manager.NeedsFlush(), "Completed reloaded save remained pending");
    }
    else if (!std::strcmp(argv[1], "buffer-resize"))
    {
        const QString path = directory.filePath("resized-buffer.bin");
        SaveManager manager("");
        manager.SetPath(path.toStdString(), false);
        Queue(manager, previous);
        manager.FlushSecondaryBuffer();

        const QByteArray grown(8193, '\x5C');
        Queue(manager, grown);
        QByteArray tooSmall(64, '\x33');
        manager.FlushSecondaryBuffer(reinterpret_cast<u8*>(tooSmall.data()), 20);
        check(tooSmall == QByteArray(64, '\x33'), "Rejected memory copy changed its destination");
        check(manager.NeedsFlush(), "Rejected memory copy discarded the pending file write");
        manager.FlushSecondaryBuffer();
        check(Read(path) == grown, "Growing the buffer lost save bytes");

        Queue(manager, next);
        manager.FlushSecondaryBuffer();
        check(Read(path) == next, "Shrinking the buffer retained stale bytes");
        QByteArray copied(next.size() + 8, '\x66');
        manager.FlushSecondaryBuffer(reinterpret_cast<u8*>(copied.data()), static_cast<u32>(next.size()));
        check(copied.left(next.size()) == next && copied.right(8) == QByteArray(8, '\x66'),
              "Memory snapshot copied the wrong bytes or exceeded its declared length");
        check(!manager.NeedsFlush(), "Completed resized save remained pending");
    }
    else
        return 2;

    std::printf("%s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
