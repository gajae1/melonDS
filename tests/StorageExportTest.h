// SPDX-License-Identifier: GPL-3.0-or-later
// Shared disposable host/FatFs fault boundaries for storage export regressions.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include "fatfs/ff.h"
#ifdef _WIN32
#include "frontend/graphics/windows_headers.h"
#endif

namespace ExportTest {
enum class Fault { None, Read, ShortRead, BackingRead, Write, Close, Commit, IndexWrite, IndexCommit };
inline Fault fault = Fault::None;
inline bool active = false, backingRead = false;
inline unsigned reads = 0, writes = 0, faults = 0, closes = 0, commits = 0, errors = 0;
inline UINT targetSize = 12291;
inline QString root;
inline void Check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
inline void Reset(Fault next = Fault::None)
{
    fault = next; reads = writes = faults = closes = commits = errors = 0;
    backingRead = false; active = true;
}
inline QByteArray Bytes(const QString& path)
{
    QFile file(path);
    Check(file.open(QIODevice::ReadOnly), "host readback open");
    return file.readAll();
}
inline void Put(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    Check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush(), "host fixture write");
}
inline QByteArray Payload(int size)
{
    QByteArray result(size, '\0');
    for (int i = 0; i < size; ++i) result[i] = char((i * 29 + i / 512 + 11) & 255);
    return result;
}
inline QStringList Entries(const QString& path)
{
    return QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
}
inline FRESULT Read(FF_FIL* file, void* data, UINT length, UINT* got)
{
    // Keep unchanged control files on the real path while targeting the
    // generated export payload (including its second 4096-byte block).
    if (active && f_size(file) == targetSize && ++reads == 2)
    {
        if (fault == Fault::Read) { ++faults; *got = 0; return FR_DISK_ERR; }
        if (fault == Fault::ShortRead) { ++faults; return f_read(file, data, length / 2, got); }
        if (fault == Fault::BackingRead) backingRead = true;
    }
    return f_read(file, data, length, got);
}
inline FRESULT Close(FF_FIL* file)
{
    const bool target = active && f_size(file) == targetSize;
    const FRESULT result = f_close(file);
    if (target) ++closes;
    if (target && fault == Fault::Close) { ++faults; return FR_DISK_ERR; }
    return result;
}
inline qint64 WriteLength(const QString& name, qint64 size)
{
    if (!active) return size;
    const bool index = name.endsWith(".idx");
    if ((!index && fault == Fault::Write && ++writes == 2) ||
        (index && fault == Fault::IndexWrite && faults == 0))
    { ++faults; return size / 2; }
    return size;
}
class HostFile : public QFile
{
public:
    using QFile::QFile;
protected:
    qint64 writeData(const char* data, qint64 size) override
    { return QFile::writeData(data, WriteLength(fileName(), size)); }
};
class SaveFile : public QSaveFile
{
public:
    using QSaveFile::QSaveFile;
    qint64 write(const char* data, qint64 size)
    { return QSaveFile::write(data, WriteLength(fileName(), size)); }
    bool commit()
    {
        if (active) ++commits;
        if (active && ((fault == Fault::Commit && !fileName().endsWith(".idx")) ||
                       (fault == Fault::IndexCommit && fileName().endsWith(".idx"))))
        { ++faults; return false; }
        return QSaveFile::commit();
    }
};
// Windows denies replacement while permitting ordinary reads/writes. This
// reaches QSaveFile's real final rename, and does not prevent its temp writes.
class ReplacementLock
{
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#endif
public:
    void Lock(const QString& path)
    {
#ifdef _WIN32
        handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        Check(handle != INVALID_HANDLE_VALUE, "hold destination without delete sharing");
#else
        Check(false, "native replacement lock requires Windows");
#endif
    }
    void Unlock()
    {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); handle = INVALID_HANDLE_VALUE; }
#endif
    }
    ~ReplacementLock() { Unlock(); }
};
inline Fault Parse(const std::string& name)
{
    if (name == "read") return Fault::Read;
    if (name == "short-read") return Fault::ShortRead;
    if (name == "backing-read") return Fault::BackingRead;
    if (name == "short-write") return Fault::Write;
    if (name == "close") return Fault::Close;
    if (name == "commit") return Fault::Commit;
    if (name == "index-write") return Fault::IndexWrite;
    if (name == "index-commit") return Fault::IndexCommit;
    return Fault::None;
}
}
