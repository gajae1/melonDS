// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#ifdef _WIN32
#include <windows.h>
#endif
#include "Platform.h"
#include "SaveManager.h"

using namespace melonDS;

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
}
}

static QByteArray Read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
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
    else
        return 2;

    std::printf("%s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
