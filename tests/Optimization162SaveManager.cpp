// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdarg>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include "Platform.h"
#include "SaveManager.h"

using namespace melonDS;

namespace
{
QSemaphore* reached = nullptr;
QSemaphore* resume = nullptr;
std::atomic_bool gateArmed{false};

QByteArray Read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool Write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

void Request(SaveManager& manager, const QByteArray& source, u32 offset, u32 length)
{
    manager.RequestFlush(reinterpret_cast<const u8*>(source.constData()), source.size(), offset, length);
}
}

namespace melonDS::Platform
{
void Log(LogLevel, const char* format, ...)
{
    if (!std::strcmp(format, "SaveManager: Wrote %u bytes to %s\n") &&
        gateArmed.exchange(false) && reached && resume)
    {
        reached->release();
        resume->acquire();
    }
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 3) return 2;
    const QString mode = QString::fromLocal8Bit(argv[1]);
    const QString base = QString::fromLocal8Bit(argv[2]);
    if (!QDir().mkpath(base)) return 2;
    QTemporaryDir runDirectory(QDir(base).filePath("run-XXXXXX"));
    if (!runDirectory.isValid()) return 2;
    const QString root = runDirectory.path();
    const QString path = QDir(root).filePath(mode + ".sav");
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };

    if (mode == "verify")
    {
        QByteArray source(4096, '\x5A');
        const QByteArray previous(4096, '\xA5');
        check(Write(path, previous), "fixture could not create previous save");
        SaveManager manager("");
        manager.SetPath(path.toStdString());
        Request(manager, source, 0, source.size());
        manager.CheckFlush();
        check(manager.Flush() && Read(path) == source, "initial save mismatch");

        source.replace(100, 64, QByteArray(64, '\x11'));
        Request(manager, source, 100, 64);
        source.replace(140, 80, QByteArray(80, '\x22'));
        Request(manager, source, 140, 80);
        check(Read(path) != source && manager.NeedsFlush(), "unpublished overlap reached disk");
        manager.CheckFlush();
        QByteArray snapshot(source.size(), '\0');
        manager.FlushSecondaryBuffer(reinterpret_cast<u8*>(snapshot.data()), snapshot.size());
        check(snapshot == source, "overlapping publication lost or changed bytes");
        check(manager.Flush() && Read(path) == source && !manager.NeedsFlush(),
              "overlapping partial writes did not commit exactly");

        source.replace(4080, 16, QByteArray(16, '\x33'));
        source.replace(0, 24, QByteArray(24, '\x44'));
        Request(manager, source, 4080, 40);
        manager.CheckFlush();
        check(manager.Flush() && Read(path) == source, "wrapped partial write mismatch");

        const QString missing = QDir(root).filePath("missing/retry.sav");
        manager.SetPath(missing.toStdString());
        source.replace(1000, 32, QByteArray(32, '\x77'));
        Request(manager, source, 1000, 32);
        manager.CheckFlush();
        check(!manager.Flush() && manager.NeedsFlush() && !QFile::exists(missing),
              "failed open acknowledged or created a save");
        check(Read(path) != source, "failed save changed old file");
        check(QDir().mkpath(QFileInfo(missing).absolutePath()), "retry directory creation failed");
        check(manager.Flush() && Read(missing) == source && !manager.NeedsFlush(),
              "retry did not commit pending bytes");

        source.replace(2000, 16, QByteArray(16, '\x66'));
        Request(manager, source, 2000, 16);
        const QString copy = QDir(root).filePath("recovery.sav");
        check(manager.SaveCopy(copy.toStdString()) && Read(copy) == source && manager.NeedsFlush(),
              "recovery copy lost unpublished partial data");
        check(Read(missing) != source, "recovery copy changed original");
        check(manager.Flush() && Read(missing) == source && !manager.NeedsFlush(),
              "original save did not finish after recovery copy");
    }
    else if (mode == "concurrent")
    {
        QByteArray source(4096, '\x5A');
        QSemaphore arrived, continueWriter;
        SaveManager manager(path.toStdString());
        reached = &arrived;
        resume = &continueWriter;
        gateArmed = true;
        Request(manager, source, 0, source.size());
        manager.CheckFlush();
        const bool paused = arrived.tryAcquire(1, 5000);
        if (paused)
        {
            source.replace(128, 48, QByteArray(48, '\x73'));
            Request(manager, source, 128, 48);
            manager.CheckFlush();
            QByteArray snapshot(source.size(), '\0');
            manager.FlushSecondaryBuffer(reinterpret_cast<u8*>(snapshot.data()), snapshot.size());
            check(snapshot == source && manager.NeedsFlush(),
                  "partial update during old commit lost its memory snapshot");
        }
        else check(false, "worker did not reach commit gate");
        continueWriter.release();
        check(manager.Flush() && Read(path) == source && !manager.NeedsFlush(),
              "old completion acknowledged or overwrote newer partial publication");
        gateArmed = false;
        reached = nullptr;
        resume = nullptr;
    }
    else if (mode == "partial" || mode == "full")
    {
        constexpr int iterations = 2048;
        constexpr int saveSize = 1024 * 1024;
        constexpr int partialSize = 64;
        QByteArray source(saveSize, '\x5A');
        SaveManager manager("");
        manager.SetPath(path.toStdString());
        Request(manager, source, 0, source.size());
        manager.CheckFlush();
        if (!manager.Flush()) return 2;
        QElapsedTimer clock;
        clock.start();
        for (int i = 0; i < iterations; ++i)
        {
            const int offset = mode == "full" ? 0 : 16384 + ((i % 16) * 32);
            const int length = mode == "full" ? saveSize : partialSize;
            std::memset(source.data() + offset, i & 0xFF, length);
            Request(manager, source, offset, length);
            manager.CheckFlush();
        }
        const qint64 publishNs = clock.nsecsElapsed();
        check(manager.Flush() && Read(path) == source && !manager.NeedsFlush(),
              "benchmark final file differs from source");
        std::printf("mode=%s iterations=%d save_bytes=%d requested_bytes=%lld publications=%d "
                    "full_copy_control_bytes=%lld publish_ns=%lld file_bytes=%lld result=%s\n",
                    argv[1], iterations, saveSize,
                    static_cast<long long>(iterations) * (mode == "full" ? saveSize : partialSize),
                    iterations, static_cast<long long>(iterations) * saveSize,
                    static_cast<long long>(publishNs), static_cast<long long>(QFileInfo(path).size()),
                    failures ? "FAIL" : "PASS");
    }
    else return 2;
    if (mode == "verify" || mode == "concurrent")
        std::printf("%s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
