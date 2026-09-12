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
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSaveFile>

#include "SaveManager.h"
#include "Platform.h"

using namespace melonDS;
using namespace melonDS::Platform;

static bool WriteSaveFile(const std::string& path, const u8* bytes, u32 length)
{
    QSaveFile file(QString::fromStdString(path));
    file.setDirectWriteFallback(false);
    const auto failed = [&](const char* phase) {
        Log(LogLevel::Error, "SaveManager: Failed to %s save %s (Qt error %d): %s\n",
            phase, path.c_str(), int(file.error()), file.errorString().toUtf8().constData());
        return false;
    };
    if (!file.open(QIODevice::WriteOnly)) return failed("open");
    if (file.write(reinterpret_cast<const char*>(bytes), length) != length) return failed("write");
    if (!file.commit()) return failed("commit");
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
    Path = path;
    FlushRequested = true;
}

void SaveManager::RequestFlush(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen)
{
    QMutexLocker lock(&StateLock);
    if (Length != savelen)
    {
        Length = savelen;
        Buffer = std::make_unique<u8[]>(Length);

        memcpy(Buffer.get(), savedata, Length);
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
    }

    FlushRequested = true;
}

void SaveManager::CheckFlush()
{
    QMutexLocker lock(&StateLock);
    CheckFlushLocked();
}

void SaveManager::CheckFlushLocked()
{
    if (!FlushRequested) return;
    if (!Buffer)
    {
        FlushRequested = false;
        return;
    }

    Log(LogLevel::Info, "SaveManager: Flush requested\n");

    if (SecondaryBufferLength != Length)
    {
        auto buffer = std::make_unique<u8[]>(Length);
        SecondaryBuffer = std::move(buffer);
        SecondaryBufferLength = Length;
    }

    if (Length) memcpy(SecondaryBuffer.get(), Buffer.get(), Length);

    FlushRequested = false;
    FlushVersion++;
    TimeAtLastFlushRequest = time(nullptr);
}

bool SaveManager::Flush()
{
    QMutexLocker lock(&StateLock);
    try
    {
        CheckFlushLocked();
        return FlushSecondaryBufferLocked(nullptr, 0);
    }
    catch (const std::bad_alloc&)
    {
        Log(LogLevel::Error, "SaveManager: Not enough memory to flush save\n");
        return false;
    }
}

bool SaveManager::SaveCopy(const std::string& path)
{
    QMutexLocker lock(&StateLock);
    if (path.empty() || !Buffer || Length == 0) return false;
    if (!Path.empty())
    {
#ifdef _WIN32
        constexpr auto caseSensitivity = Qt::CaseInsensitive;
#else
        constexpr auto caseSensitivity = Qt::CaseSensitive;
#endif
        if (ResolvedSavePath(path).compare(ResolvedSavePath(Path), caseSensitivity) == 0)
            return false;
    }

    // Buffer is newer than SecondaryBuffer when CheckFlush has not run yet.
    // A recovery copy must not publish or acknowledge the original-file request.
    return WriteSaveFile(path, Buffer.get(), Length);
}

void SaveManager::run()
{
    for (;;)
    {
        QThread::msleep(100);

        if (!Running) return;

        QMutexLocker lock(&StateLock);
        // We debounce for two seconds after last flush request to ensure that writing has finished.
        if (TimeAtLastFlushRequest == 0 || difftime(time(nullptr), TimeAtLastFlushRequest) < 2)
        {
            continue;
        }

        FlushSecondaryBufferLocked(nullptr, 0);
    }
}

void SaveManager::FlushSecondaryBuffer(u8* dst, u32 dstLength)
{
    QMutexLocker lock(&StateLock);
    FlushSecondaryBufferLocked(dst, dstLength);
}

bool SaveManager::FlushSecondaryBufferLocked(u8* dst, u32 dstLength)
{
    if (!SecondaryBuffer) return true;

    // When flushing to a file, there's no point in re-writing the exact same data.
    if (!dst && FlushVersion == PreviousFlushVersion) return true;
    // When flushing to memory, we don't know if dst already has any data so we only check that we CAN flush.
    if (dst && dstLength < SecondaryBufferLength) return false;

    if (dst)
    {
        memcpy(dst, SecondaryBuffer.get(), SecondaryBufferLength);
        return true; // Copying bytes to memory does not commit the original file.
    }
    if (!WriteSaveFile(Path, SecondaryBuffer.get(), SecondaryBufferLength))
    {
        // Keep this version pending and reuse the debounce interval before retrying.
        TimeAtLastFlushRequest = time(nullptr);
        return false;
    }
    PreviousFlushVersion = FlushVersion;
    TimeAtLastFlushRequest = 0;
    return true;
}

bool SaveManager::NeedsFlush()
{
    QMutexLocker lock(&StateLock);
    return FlushRequested || FlushVersion != PreviousFlushVersion;
}
