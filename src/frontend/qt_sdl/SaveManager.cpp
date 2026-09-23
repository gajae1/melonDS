/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <cstdlib>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSaveFile>
#include <QThread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "SaveManager.h"
#include "Platform.h"

using namespace melonDS;
using namespace melonDS::Platform;

namespace
{
constexpr int DefaultRetryAttempts = 5;
constexpr int DefaultRetryDelayMs = 40;

int ReadBoundedEnv(const char* name, int fallback, int min, int max)
{
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    long parsed = 0;
    for (; *value; ++value)
    {
        if (*value < '0' || *value > '9') return fallback;
        parsed = parsed * 10 + (*value - '0');
        if (parsed > max) return max;
    }
    return static_cast<int>(std::clamp(parsed, static_cast<long>(min), static_cast<long>(max)));
}

// A scanner or indexer holding a freshly written file without FILE_SHARE_DELETE
// makes the atomic replace fail with a sharing violation that clears on its own.
// Only those transient codes are retried; the wait is bounded and a permanent
// denial still ends as a reported failure with the original file untouched.
int RetryAttempts()
{
    static const int configured = ReadBoundedEnv("MELONDS_SAVE_RETRY_ATTEMPTS", DefaultRetryAttempts, 1, 20);
    return configured;
}

int RetryDelayMs()
{
    static const int configured = ReadBoundedEnv("MELONDS_SAVE_RETRY_DELAY_MS", DefaultRetryDelayMs, 10, 5000);
    return configured;
}

bool LastErrorIsTransient()
{
#ifdef _WIN32
    const DWORD error = GetLastError();
    return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION
        || error == ERROR_LOCK_VIOLATION;
#else
    return false;
#endif
}

enum class WriteStage
{
    Ok,
    Open,
    Write,
    Commit,
};

// One attempt owns a fresh QSaveFile: reopening the object after a denied commit
// commits an empty file, so a retry must start from a new staging file.
WriteStage TryWriteSaveFile(const QString& path, const u8* bytes, u32 length)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const auto stage = [&] {
        if (!file.open(QIODevice::WriteOnly)) return WriteStage::Open;
        if (file.write(reinterpret_cast<const char*>(bytes), length) != length) return WriteStage::Write;
        if (!file.commit()) return WriteStage::Commit;
        return WriteStage::Ok;
    };
    const WriteStage result = stage();
    if (result == WriteStage::Ok) return result;
    Log(LogLevel::Error, "SaveManager: Failed to %s save %s (Qt error %d): %s\n",
        result == WriteStage::Open ? "open" : result == WriteStage::Write ? "write" : "commit",
        path.toUtf8().constData(), int(file.error()), file.errorString().toUtf8().constData());
    return result;
}
}

static bool WriteSaveFile(const std::string& path, const u8* bytes, u32 length)
{
    const QString target = QString::fromStdString(path);
    const int budget = RetryAttempts();
    WriteStage stage = WriteStage::Ok;
    for (int attempt = 1; ; ++attempt)
    {
        stage = TryWriteSaveFile(target, bytes, length);
        if (stage == WriteStage::Ok) break;
        if (attempt >= budget || !LastErrorIsTransient()) break;
        QThread::msleep(static_cast<unsigned long>(RetryDelayMs()));
    }
    if (stage != WriteStage::Ok)
    {
        if (budget > 1)
            Log(LogLevel::Error,
                "SaveManager: Save %s was still denied after %d bounded replace attempts; keeping the pending save\n",
                path.c_str(), budget);
        return false;
    }
    Log(LogLevel::Info, "SaveManager: Wrote %u bytes to %s\n", length, path.c_str());
    return true;
}

static QString ResolvedSavePath(const std::string& path)
{
    const QFileInfo file(QString::fromStdString(path));
    const QString canonical = file.canonicalFilePath();
    if (!canonical.isEmpty()) return canonical;

    // A recovery file may not exist yet; resolve its existing parent instead.
    const QString parent = file.absoluteDir().canonicalPath();
    return QDir::cleanPath(parent.isEmpty() ? file.absoluteFilePath() : QDir(parent).filePath(file.fileName()));
}

SaveManager::SaveManager(const std::string& path) : QThread()
{
    SecondaryBuffer = nullptr;
    SecondaryBufferLength = 0;

    Running = false;

    Path = path;

    Buffer = nullptr;
    Length = 0;
    FlushRequested = false;

    FlushVersion = 0;
    PreviousFlushVersion = 0;
    TimeAtLastFlushRequest = 0;

    if (!path.empty())
    {
        Running = true;
        start();
    }
}

SaveManager::~SaveManager()
{
    if (Running)
    {
        Running = false;
        wait();
        Flush();
    }

    SecondaryBuffer = nullptr;

    Buffer = nullptr;
}

std::string SaveManager::GetPath()
{
    QMutexLocker lock(&StateLock);
    return Path;
}

void SaveManager::SetPath(const std::string& path)
{
    QMutexLocker lock(&StateLock);
    // Relocating only changes where the next write lands. Marking a flush here
    // would re-commit the previously finished bytes to the new path, which
    // overwrites the save the new game already owns there.
    Path = path;
}

void SaveManager::RequestFlush(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen)
{
    QMutexLocker lock(&StateLock);
    if (!savedata || !savelen) return;
    if (Length != savelen)
    {
        try
        {
            auto buffer = std::make_unique<u8[]>(savelen);
            memcpy(buffer.get(), savedata, savelen);
            Buffer = std::move(buffer);
            Length = savelen;
            DirtyStart = 0;
            DirtyEnd = savelen;
        }
        catch (const std::bad_alloc&)
        {
            if (!CaptureFailed)
                Log(LogLevel::Error, "SaveManager: Not enough memory to capture save; waiting for producer retry\n");
            CaptureFailed = true;
            return;
        }
    }
    else if (CaptureFailed)
    {
        // A later partial write cannot recover the bytes missed by a failed capture.
        memcpy(Buffer.get(), savedata, Length);
        DirtyStart = 0;
        DirtyEnd = Length;
    }
    else
    {
        if ((writeoffset+writelen) > savelen)
        {
            u32 len = savelen - writeoffset;
            memcpy(&Buffer[writeoffset], &savedata[writeoffset], len);
            len = writelen - len;
            if (len > savelen) len = savelen;
            memcpy(&Buffer[0], &savedata[0], len);
        }
        else
        {
            memcpy(&Buffer[writeoffset], &savedata[writeoffset], writelen);
        }

        // A wrapped or empty request conservatively republishes the whole save.
        if (!writelen || writeoffset >= savelen || writelen > savelen - writeoffset)
        {
            DirtyStart = 0;
            DirtyEnd = savelen;
        }
        else if (!DirtyEnd)
        {
            DirtyStart = writeoffset;
            DirtyEnd = writeoffset + writelen;
        }
        else
        {
            DirtyStart = std::min(DirtyStart, writeoffset);
            DirtyEnd = std::max(DirtyEnd, writeoffset + writelen);
        }
    }

    CaptureFailed = false;
    FlushRequested = true;
}

void SaveManager::CheckFlush()
{
    QMutexLocker lock(&StateLock);
    try
    {
        CheckFlushLocked();
    }
    catch (const std::bad_alloc&)
    {
        if (!PublicationFailed)
            Log(LogLevel::Error, "SaveManager: Not enough memory to publish save; keeping request pending\n");
        PublicationFailed = true;
    }
}

void SaveManager::CheckFlushLocked()
{
    if (CaptureFailed || !FlushRequested) return;
    if (!Buffer)
    {
        FlushRequested = false;
        return;
    }

    const bool newBuffer = SecondaryBufferLength != Length;
    if (newBuffer)
    {
        auto buffer = std::make_unique<u8[]>(Length);
        SecondaryBuffer = std::move(buffer);
        SecondaryBufferLength = Length;
    }

    // The secondary buffer is the last complete publication. Disk writers take
    // their own full snapshot, so updating this interval cannot mutate a write.
    if (Length)
    {
        if (newBuffer || !DirtyEnd)
            memcpy(SecondaryBuffer.get(), Buffer.get(), Length);
        else
            memcpy(SecondaryBuffer.get() + DirtyStart, Buffer.get() + DirtyStart,
                   DirtyEnd - DirtyStart);
    }

    DirtyStart = DirtyEnd = 0;

    Log(LogLevel::Info, "SaveManager: Flush requested\n");
    FlushRequested = false;
    PublicationFailed = false;
    FlushVersion++;
    TimeAtLastFlushRequest = time(nullptr);
}

bool SaveManager::Flush()
{
    return FlushFile(true, false);
}

bool SaveManager::SaveCopy(const std::string& path)
{
    QMutexLocker writer(&FileLock);
    try
    {
        std::unique_ptr<u8[]> bytes;
        u32 length;
        std::string originalPath;
        {
            QMutexLocker lock(&StateLock);
            if (CaptureFailed || path.empty() || !Buffer || Length == 0) return false;
            originalPath = Path;
            length = Length;
            bytes = std::make_unique<u8[]>(length);
            memcpy(bytes.get(), Buffer.get(), length);
        }
        if (!originalPath.empty())
        {
#ifdef _WIN32
            constexpr auto caseSensitivity = Qt::CaseInsensitive;
#else
            constexpr auto caseSensitivity = Qt::CaseSensitive;
#endif
            if (ResolvedSavePath(path).compare(ResolvedSavePath(originalPath), caseSensitivity) == 0)
                return false;
        }

        // Buffer is newer than SecondaryBuffer when CheckFlush has not run yet.
        // A recovery copy must not publish or acknowledge the original-file request.
        return WriteSaveFile(path, bytes.get(), length);
    }
    catch (const std::bad_alloc&)
    {
        Log(LogLevel::Error, "SaveManager: Not enough memory to copy save\n");
        return false;
    }
}

void SaveManager::run()
{
    for (;;)
    {
        QThread::msleep(100);

        if (!Running) return;

        FlushFile(false, true);
    }
}

void SaveManager::FlushSecondaryBuffer(u8* dst, u32 dstLength)
{
    if (!dst)
    {
        FlushFile(false, false);
        return;
    }
    QMutexLocker lock(&StateLock);
    if (SecondaryBuffer && dstLength >= SecondaryBufferLength)
        memcpy(dst, SecondaryBuffer.get(), SecondaryBufferLength);
}

bool SaveManager::FlushFile(bool publish, bool debounce)
{
    QMutexLocker writer(&FileLock);
    try
    {
        std::unique_ptr<u8[]> bytes;
        std::string path;
        u32 length, version;
        {
            QMutexLocker lock(&StateLock);
            if (CaptureFailed) return false;
            if (publish) CheckFlushLocked();
            // Recheck after acquiring the writer lock: a queued writer may have
            // committed, or the producer may have published a newer save.
            if (debounce && (TimeAtLastFlushRequest == 0 ||
                difftime(time(nullptr), TimeAtLastFlushRequest) < 2)) return true;
            if (!SecondaryBuffer || FlushVersion == PreviousFlushVersion) return true;
            path = Path;
            version = FlushVersion;
            length = SecondaryBufferLength;
            bytes = std::make_unique<u8[]>(length);
            memcpy(bytes.get(), SecondaryBuffer.get(), length);
        }

        const bool committed = WriteSaveFile(path, bytes.get(), length);
        QMutexLocker lock(&StateLock);
        // This write owns only its snapshot. A relocation or newer publication
        // during disk I/O must retain its pending version and debounce deadline.
        if (Path == path && FlushVersion == version)
        {
            if (committed)
            {
                PreviousFlushVersion = version;
                if (!FlushRequested && !CaptureFailed) TimeAtLastFlushRequest = 0;
            }
            else TimeAtLastFlushRequest = time(nullptr);
        }
        return committed && !CaptureFailed && !FlushRequested && FlushVersion == PreviousFlushVersion;
    }
    catch (const std::bad_alloc&)
    {
        Log(LogLevel::Error, "SaveManager: Not enough memory to flush save\n");
        QMutexLocker lock(&StateLock);
        TimeAtLastFlushRequest = time(nullptr);
        return false;
    }
}

bool SaveManager::NeedsFlush()
{
    QMutexLocker lock(&StateLock);
    return CaptureFailed || FlushRequested || FlushVersion != PreviousFlushVersion;
}

bool SaveManager::NeedsCapture()
{
    QMutexLocker lock(&StateLock);
    return CaptureFailed;
}
