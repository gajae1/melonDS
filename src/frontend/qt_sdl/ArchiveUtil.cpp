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

#include "ArchiveUtil.h"

#include <algorithm>
#include <new>
#include <utility>
#include <QByteArray>

using namespace melonDS;

namespace Archive
{

#ifdef __WIN32__
#define melon_archive_open(a, f, b) archive_read_open_filename_w(a, (const wchar_t*)f.utf16(), b)
#else
#define melon_archive_open(a, f, b) archive_read_open_filename(a, f.toUtf8().constData(), b)
#endif // __WIN32__

using ArchiveReader = std::unique_ptr<archive, decltype(&archive_read_free)>;

static ArchiveReader OpenArchive(const QString& path)
{
    ArchiveReader reader(archive_read_new(), archive_read_free);
    if (!reader || path.isEmpty() || path.contains(QChar(u'\0')) ||
        archive_read_support_filter_all(reader.get()) != ARCHIVE_OK ||
        archive_read_support_format_all(reader.get()) != ARCHIVE_OK ||
        melon_archive_open(reader.get(), path, 10240) != ARCHIVE_OK)
        return {nullptr, archive_read_free};
    return reader;
}

static bool ReadMemberName(archive_entry* entry, QString& name)
{
    const char* utf8 = archive_entry_pathname_utf8(entry);
    if (!utf8 || !*utf8) return false;
    name = QString::fromUtf8(utf8);
    // QString replaces invalid UTF-8. Do not silently select a different name.
    return name.toUtf8() == utf8;
}

QVector<QString> ListArchive(QString path)
{
    try
    {
        auto reader = OpenArchive(path);
        if (!reader) return {"Err"};

        QVector<QString> fileList;
        archive_entry* entry = nullptr;
        int status;
        while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK)
        {
            if (!entry) return {"Err"};
            if (archive_entry_filetype(entry) == AE_IFREG && !archive_entry_hardlink(entry))
            {
                QString name;
                if (!ReadMemberName(entry, name)) return {"Err"};
                fileList.push_back(std::move(name));
            }
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) return {"Err"};
        }

        // Warnings and failed headers must not publish a successful partial list.
        if (status != ARCHIVE_EOF) return {"Err"};
        if (archive_read_free(reader.release()) != ARCHIVE_OK) return {"Err"};
        std::stable_sort(fileList.begin(), fileList.end(), [](const QString& a, const QString& b) {
            return a.toLower() < b.toLower();
        });
        fileList.prepend("OK");
        return fileList;
    }
    catch (const std::bad_alloc&)
    {
        return {"Err"};
    }
}

s32 ExtractFileFromArchive(QString path, QString wantedFile, std::unique_ptr<u8[]>& filedata, u32* filesize)
{
    try
    {
        if (wantedFile.isEmpty() || wantedFile.contains(QChar(u'\0'))) return -1;
        auto reader = OpenArchive(path);
        if (!reader) return -1;

        archive_entry* entry = nullptr;
        for (;;)
        {
            const int status = archive_read_next_header(reader.get(), &entry);
            if (status == ARCHIVE_EOF)
                return archive_read_free(reader.release()) == ARCHIVE_OK ? -2 : -1;
            if (status != ARCHIVE_OK || !entry) return -1;

            QString name;
            if (!ReadMemberName(entry, name)) return -1;
            if (name == wantedFile) break;
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) return -1;
        }

        if (archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry) ||
            !archive_entry_size_is_set(entry))
            return -1;
        const la_int64_t size = archive_entry_size(entry);
        // Match loadROMData/decompressROM; the cart parsers own format limits.
        if (size <= 0 || size > 0x40000000) return -1;

        const size_t length = static_cast<size_t>(size);
        // Reject an unreadable body before allocating its entire advertised size.
        u8 first;
        if (archive_read_data(reader.get(), &first, 1) != 1) return -1;

        auto data = std::make_unique_for_overwrite<u8[]>(length);
        data[0] = first;
        size_t total = 1;
        while (total < length)
        {
            const la_ssize_t count = archive_read_data(reader.get(), data.get() + total, length - total);
            if (count <= 0 || static_cast<size_t>(count) > length - total) return -1;
            total += static_cast<size_t>(count);
        }

        // Consume the entry's end marker too: a checksum error may arrive only
        // after the last payload bytes. Extra bytes also contradict its size.
        u8 extra;
        if (archive_read_data(reader.get(), &extra, 1) != 0) return -1;
        // Free also closes the reader. Check that result before publishing data;
        // every earlier return/exception is covered by the same RAII deleter.
        if (archive_read_free(reader.release()) != ARCHIVE_OK) return -1;

        filedata = std::move(data);
        if (filesize) *filesize = static_cast<u32>(length);
        return static_cast<s32>(length);
    }
    catch (const std::bad_alloc&)
    {
        return -1;
    }
}

}
