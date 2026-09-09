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

#ifndef SAVEMANAGER_H
#define SAVEMANAGER_H

#include <string>
#ifndef _MSC_VER
#include <unistd.h>
#endif
#include <time.h>
#include <atomic>
#include <memory>
#include <QThread>
#include <QMutex>

#include "types.h"

class SaveManager : public QThread
{
    Q_OBJECT
    void run() override;

public:
    SaveManager(const std::string& path);
    ~SaveManager();

    std::string GetPath();
    // Relocate the current save and keep its latest bytes pending for the new path.
    void SetPath(const std::string& path);

    void RequestFlush(const melonDS::u8* savedata, melonDS::u32 savelen, melonDS::u32 writeoffset, melonDS::u32 writelen);
    void CheckFlush();

    // Finish original-file saves, including requests not yet published by CheckFlush.
    // No data or an already committed version succeeds without another file write.
    bool Flush();
    // Commit the latest data elsewhere, preserving the original path and pending state.
    bool SaveCopy(const std::string& path);

    bool NeedsFlush();
    void FlushSecondaryBuffer(melonDS::u8* dst = nullptr, melonDS::u32 dstLength = 0);

private:
    // Require StateLock; shared by the worker and explicit flushes.
    void CheckFlushLocked();
    bool FlushSecondaryBufferLocked(melonDS::u8* dst, melonDS::u32 dstLength);

    // Protects the path, both buffers, and flush/version/debounce state.
    QMutex StateLock;
    std::string Path;

    std::atomic_bool Running;

    std::unique_ptr<melonDS::u8[]> Buffer;
    melonDS::u32 Length;
    bool FlushRequested;

    std::unique_ptr<melonDS::u8[]> SecondaryBuffer;
    melonDS::u32 SecondaryBufferLength;

    time_t TimeAtLastFlushRequest;

    // We keep versions in case the user closes the application before
    // a flush cycle is finished.
    melonDS::u32 PreviousFlushVersion;
    melonDS::u32 FlushVersion;
};

#endif // SAVEMANAGER_H
