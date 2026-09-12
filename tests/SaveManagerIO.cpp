// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdarg>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSemaphore>
#include <QStringList>
#include <QTemporaryDir>
#ifdef _WIN32
#include <windows.h>
#endif
#include "Platform.h"
#include "SaveManager.h"

using namespace melonDS;

// Fail one production save-buffer allocation on this calling thread. Qt/file
// operations and worker allocations remain untouched; this is not real OOM.
static thread_local size_t failArraySize = 0;
static thread_local unsigned arrayFailures = 0;

void* operator new[](size_t size)
{
    if (size && size == failArraySize)
    {
        failArraySize = 0;
        ++arrayFailures;
        throw std::bad_alloc();
    }
    if (void* data = std::malloc(size ? size : 1)) return data;
    throw std::bad_alloc();
}
void operator delete[](void* data) noexcept { std::free(data); }
void operator delete[](void* data, size_t) noexcept { std::free(data); }

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

// File commits use production QSaveFile; only Platform logging needs a test hook.
namespace melonDS::Platform
{
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
        if (!worker) manager.SetPath(path.toStdString());
        Queue(manager, next);
        check(!manager.Flush(), "Failed open was reported as a successful flush");
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
            check(manager.Flush(), "Recovered path did not report a committed flush");
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
        manager.SetPath(path.toStdString());
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
            check(!manager.Flush(), "Failed replacement was reported as a successful flush");
            check(manager.NeedsFlush(), "Locked save discarded the pending write");
            check(Read(path) == previous, "Failed replacement damaged the previous save");
            CloseHandle(lock);
#else
            return 77;
#endif
        }
        check(manager.Flush(), "File replacement did not report a committed flush");
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
            manager.SetPath(newPath.toStdString());
            setterFinished.release();
        }));
        setter->start();
        const bool started = setterStarted.tryAcquire(1, 1000);
        const bool changedWhileFlushing = started && setterFinished.tryAcquire(1, 1000);
        pause.Release();
        setter->wait();
        if (!started) return 2;
        check(!changedWhileFlushing, "SetPath changed shared state while the worker held the flush lock");

        // SetPath relocates this same game's data. It does not load another
        // game's existing file, and the normal CheckFlush publication is retained.
        check(manager.GetPath() == newPath.toStdString(), "Save path did not change after the flush");
        manager.CheckFlush();
        manager.FlushSecondaryBuffer();
        check(Read(oldPath) == next, "In-flight flush lost the current game's original save");
        check(Read(newPath) == next, "Relocation did not write the current game's data");
        check(!manager.NeedsFlush(), "Completed relocation remained pending");
    }
    else if (!std::strcmp(argv[1], "relocation-pending"))
    {
        const QString oldPath = directory.filePath("pending-original.bin");
        const QString newPath = directory.filePath("relocated-save.bin");
        if (!Write(oldPath, previous) || !Write(newPath, next)) return 2;
        SaveManager manager("");
        manager.SetPath(oldPath.toStdString());
        QByteArray published = previous;
        published[5] = '\x3C';
        Queue(manager, published);
        QByteArray latest = published;
        latest[9] = '\x37';
        manager.RequestFlush(reinterpret_cast<const u8*>(latest.constData()),
                             static_cast<u32>(latest.size()), 9, 1);

        // The primary contains a newer partial write than the pending secondary.
        // Moving this game's path must retain both until the latest data commits.
        manager.SetPath(newPath.toStdString());
        check(manager.GetPath() == newPath.toStdString() && manager.NeedsFlush(),
              "Relocation discarded the unpublished write request");
        check(Read(oldPath) == previous && Read(newPath) == next,
              "Relocation changed a file before an explicit flush");
        check(manager.Flush() && Read(newPath) == latest,
              "Relocation lost the latest pending bytes or adopted destination data");
        check(Read(oldPath) == previous, "Relocated flush changed the previous file");
        check(!manager.NeedsFlush(), "Committed relocation remained pending");
    }
    else if (!std::strcmp(argv[1], "allocation-capture"))
    {
        const QByteArray grown(8193, '\x5C');
        const QByteArray restored(previous.size(), '\x3D');
        for (int scenario = 0; scenario < 3; ++scenario)
        {
            const bool initial = scenario == 0;
            const QByteArray& recovered = scenario == 2 ? restored : grown;
            const QString path = directory.filePath(QString("capture-%1.bin").arg(scenario));
            const QString copy = directory.filePath(QString("capture-copy-%1.bin").arg(scenario));
            SaveManager manager("");
            manager.SetPath(path.toStdString());
            if (!initial) Queue(manager, previous);
            const auto before = arrayFailures;
            failArraySize = grown.size();
            bool escaped = false;
            try
            {
                manager.RequestFlush(reinterpret_cast<const u8*>(grown.constData()), grown.size(), 0, grown.size());
            }
            catch (const std::bad_alloc&) { escaped = true; }
            check(arrayFailures == before + 1 && failArraySize == 0, "Capture allocation failure was not injected");
            check(!escaped, "Save capture allocation failure escaped into the producer");
            if (escaped) continue; // Old Length may exceed its allocation; never read that invalid buffer.
            check(manager.NeedsFlush(), "Failed capture was reported as clean");
            check(manager.NeedsCapture(), "Producer could not observe the missing capture");
            manager.CheckFlush();
            manager.FlushSecondaryBuffer(); // Exercise the same file path used by the worker.
            check(!manager.Flush() && !QFile::exists(path), "Failed capture committed stale or missing data");
            check(!manager.SaveCopy(copy.toStdString()) && !QFile::exists(copy),
                  "Failed capture offered stale data as the latest recovery copy");
            // The next callback may cover one byte only, but carries the full
            // source snapshot. Recovery must capture all previously missed bytes.
            manager.RequestFlush(reinterpret_cast<const u8*>(recovered.constData()), recovered.size(), 3, 1);
            check(!manager.NeedsCapture(), "Successful recapture remained unavailable");
            check(manager.Flush() && Read(path) == recovered && !manager.NeedsFlush(),
                  "Capture recovery lost the complete latest save");
        }
    }
    else if (!std::strcmp(argv[1], "allocation-publish"))
    {
        const QString path = directory.filePath("published-save.bin");
        const QString copy = directory.filePath("published-copy.bin");
        const QByteArray grown(8193, '\x5C');
        SaveManager manager("");
        manager.SetPath(path.toStdString());
        Queue(manager, previous);
        manager.RequestFlush(reinterpret_cast<const u8*>(grown.constData()), grown.size(), 0, grown.size());
        const auto before = arrayFailures;
        failArraySize = grown.size();
        bool escaped = false;
        try { manager.CheckFlush(); }
        catch (const std::bad_alloc&) { escaped = true; }
        check(!escaped, "Publication allocation failure escaped the frame boundary");
        check(arrayFailures == before + 1 && failArraySize == 0 && manager.NeedsFlush(),
              "Failed publication discarded the pending latest buffer");
        check(manager.SaveCopy(copy.toStdString()) && Read(copy) == grown && manager.NeedsFlush(),
              "Recovery copy did not retain the unpublished latest bytes");
        failArraySize = grown.size();
        check(!manager.Flush() && arrayFailures == before + 2 && !QFile::exists(path) && manager.NeedsFlush(),
              "Explicit flush acknowledged a failed publication");
        check(manager.Flush() && Read(path) == grown && !manager.NeedsFlush(),
              "Publication retry lost the latest data or remained pending");
    }
    else if (!std::strcmp(argv[1], "buffer-resize"))
    {
        const QString path = directory.filePath("resized-buffer.bin");
        SaveManager manager("");
        manager.SetPath(path.toStdString());
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
    else if (!std::strcmp(argv[1], "unpublished-pending"))
    {
        const QString path = directory.filePath("unpublished-save.bin");
        if (!Write(path, previous)) return 2;
        SaveManager manager(path.toStdString());
        // Closing can inspect pending state before the next frame's CheckFlush.
        manager.RequestFlush(reinterpret_cast<const u8*>(next.constData()),
                             static_cast<u32>(next.size()), 0, static_cast<u32>(next.size()));
        check(manager.NeedsFlush(), "Direct RequestFlush was reported as clean before publication");
        check(Read(path) == previous, "Unpublished bytes unexpectedly reached the save file");

        // Keep this regression on the original APIs and verify the pending bytes.
        manager.CheckFlush();
        manager.FlushSecondaryBuffer();
        check(Read(path) == next, "Publishing the direct request lost save bytes");
        check(!manager.NeedsFlush(), "Committed direct request remained pending");
    }
    else if (!std::strcmp(argv[1], "memory-copy-pending"))
    {
        const QString path = directory.filePath("unavailable/save.bin");
        SaveManager manager("");
        manager.SetPath(path.toStdString());
        Queue(manager, next);
        manager.FlushSecondaryBuffer(); // Keep the original storage unavailable.
        check(manager.NeedsFlush(), "Failed write discarded the pending file save");

        QByteArray snapshot(next.size(), '\x66');
        manager.FlushSecondaryBuffer(reinterpret_cast<u8*>(snapshot.data()),
                                     static_cast<u32>(snapshot.size()));
        check(snapshot == next, "Memory snapshot did not contain the pending save");
        check(manager.NeedsFlush(), "Memory copy acknowledged a file commit that never succeeded");
        manager.FlushSecondaryBuffer(); // Failure must remain observable until recovery.
        check(manager.NeedsFlush(), "Still-unavailable storage was reported as saved");
        check(!QFile::exists(path), "Unavailable storage unexpectedly contained a save");

        if (!QDir(directory.path()).mkdir("unavailable")) return 2;
        manager.FlushSecondaryBuffer(); // No new write or CheckFlush after the snapshot.
        check(Read(path) == next, "Memory copy prevented retrying the original file save");
        check(!manager.NeedsFlush(), "Recovered file commit remained pending");
    }
    else if (!std::strcmp(argv[1], "flush-latest"))
    {
        const QString path = directory.filePath("latest-save.bin");
        const QString copyPath = directory.filePath("empty-recovery.bin");
        if (!Write(path, previous)) return 2;
        SaveManager manager("");
        check(manager.Flush(), "Unused manager could not finish flushing");
        manager.SetPath(path.toStdString());
        check(manager.Flush(), "Manager without requested data could not finish flushing");
        check(Read(path) == previous, "Empty flush changed the existing file");
        check(!manager.NeedsFlush(), "Empty flush left a phantom pending request");
        check(!manager.SaveCopy(copyPath.toStdString()), "Missing data was reported as a saved recovery copy");
        check(!QFile::exists(copyPath), "Missing data created an empty recovery copy");

        manager.RequestFlush(reinterpret_cast<const u8*>(next.constData()),
                             static_cast<u32>(next.size()), 0, static_cast<u32>(next.size()));
        check(manager.Flush(), "Flush did not commit an unpublished request");
        check(Read(path) == next && !manager.NeedsFlush(), "Flush did not save and acknowledge the latest bytes");
        check(manager.Flush(), "Already committed data was reported as a failed flush");

        const QString closingPath = directory.filePath("direct-close.bin");
        if (!Write(closingPath, previous)) return 2;
        {
            SaveManager closing(closingPath.toStdString());
            closing.RequestFlush(reinterpret_cast<const u8*>(next.constData()),
                                 static_cast<u32>(next.size()), 0, static_cast<u32>(next.size()));
        }
        check(Read(closingPath) == next, "Final flush omitted an unpublished request");
    }
    else if (!std::strcmp(argv[1], "recovery-copy"))
    {
        const QString path = directory.filePath("offline/save.bin");
        const QString copyPath = directory.filePath(QStringLiteral("recovery-\uD55C\uAE00.bin"));
        const QString failedCopy = directory.filePath("also-offline/copy.bin");
        if (!Write(copyPath, previous)) return 2;
        SaveManager manager("");
        manager.SetPath(path.toStdString());
        Queue(manager, previous);
        check(!manager.Flush(), "Unavailable original storage was reported as committed");

        // A newer primary buffer must win over the older published secondary copy.
        manager.RequestFlush(reinterpret_cast<const u8*>(next.constData()),
                             static_cast<u32>(next.size()), 0, static_cast<u32>(next.size()));
        check(!manager.SaveCopy(failedCopy.toStdString()), "Missing recovery directory was reported as saved");
        check(!QFile::exists(failedCopy), "Failed recovery created a file");
        check(manager.GetPath() == path.toStdString() && manager.NeedsFlush(),
              "Failed recovery changed the original path or pending state");
        check(manager.SaveCopy(copyPath.toStdString()), "Recovery copy failed on writable storage");
        check(Read(copyPath) == next, "Recovery copy was missing, stale, or retained the previous tail");
        check(manager.GetPath() == path.toStdString() && manager.NeedsFlush(),
              "Recovery copy acknowledged or redirected the original save");
        check(!manager.Flush(), "Original failure disappeared after saving a recovery copy");
        check(!QFile::exists(path), "Recovery copy unexpectedly wrote the unavailable original");

        // Cancel/continue keeps the same manager; retry needs no new RequestFlush.
        if (!QDir(directory.path()).mkdir("offline")) return 2;
        check(manager.Flush(), "Original save could not retry after recovery and cancel");
        check(Read(path) == next && Read(copyPath) == next && !manager.NeedsFlush(),
              "Retry lost the latest original or recovery bytes");
    }
    else if (!std::strcmp(argv[1], "same-copy-path"))
    {
        const QString path = directory.filePath("original-save.bin");
        if (!Write(path, previous)) return 2;
        SaveManager manager("");
        manager.SetPath(path.toStdString());
        manager.RequestFlush(reinterpret_cast<const u8*>(next.constData()),
                             static_cast<u32>(next.size()), 0, static_cast<u32>(next.size()));
        QStringList aliases{path, QDir::current().relativeFilePath(path),
                            directory.filePath("./original-save.bin")};
#ifdef _WIN32
        aliases.append(path.toUpper());
#endif
        for (const QString& alias : aliases)
            check(!manager.SaveCopy(alias.toStdString()), "Recovery copy bypassed the original flush path");
        check(Read(path) == previous && manager.GetPath() == path.toStdString() && manager.NeedsFlush(),
              "Rejected recovery altered the original save or its pending state");

        const QString newPath = directory.filePath("not-created.bin");
        manager.SetPath(newPath.toStdString());
        check(!manager.SaveCopy(QDir::current().relativeFilePath(newPath).toStdString()),
              "Recovery copy accepted the original path before its first file existed");
        check(!QFile::exists(newPath) && manager.NeedsFlush(), "Rejected copy created or acknowledged the original");
        check(!manager.SaveCopy(""), "Empty recovery path was reported as saved");
    }
    else if (!std::strcmp(argv[1], "copy-commit-failure"))
    {
#ifdef _WIN32
        const QString path = directory.filePath("pending-original.bin");
        const QString copyPath = directory.filePath("locked-recovery.bin");
        if (!Write(path, previous) || !Write(copyPath, previous)) return 2;
        SaveManager manager("");
        manager.SetPath(path.toStdString());
        manager.RequestFlush(reinterpret_cast<const u8*>(next.constData()),
                             static_cast<u32>(next.size()), 0, static_cast<u32>(next.size()));
        HANDLE lock = CreateFileW(reinterpret_cast<LPCWSTR>(copyPath.utf16()), GENERIC_READ,
                                  FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock == INVALID_HANDLE_VALUE) return 2;
        const bool copied = manager.SaveCopy(copyPath.toStdString());
        CloseHandle(lock);
        check(!copied && Read(copyPath) == previous, "Failed recovery commit damaged or acknowledged its old file");
        check(manager.GetPath() == path.toStdString() && manager.NeedsFlush() && Read(path) == previous,
              "Failed recovery commit changed the pending original save");
        check(manager.SaveCopy(copyPath.toStdString()) && Read(copyPath) == next,
              "Unlocked recovery path could not retry the copy");
        check(manager.NeedsFlush(), "Successful recovery retry acknowledged the original file");
        check(manager.Flush() && Read(path) == next, "Original save could not commit after recovery retry");
#else
        return 77;
#endif
    }
    else
        return 2;

    std::printf("%s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
