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
#include <QFile>
#include <array>
#include <cerrno>
#include <limits>

using namespace melonDS;

namespace Archive
{

// libarchive's filename reader can perform many reads inside a single header
// or skip operation. Supplying callbacks makes those reads cooperative too.
constexpr size_t ReadChunk = 64 * 1024;
struct ArchiveInput
{
    QFile file;
    std::stop_token stop;
    std::array<char, ReadChunk> buffer;
    ArchiveInput(const QString& path, std::stop_token token) : file(path), stop(token) {}
};
struct ArchiveReader
{
    std::unique_ptr<ArchiveInput> input;
    // Free the archive before its callback context and QFile.
    std::unique_ptr<archive, decltype(&archive_read_free)> handle{nullptr, archive_read_free};
    archive* get() const { return handle.get(); }
    archive* release() { return handle.release(); }
    explicit operator bool() const { return bool(handle); }
};
static bool Cancelled(archive* reader, ArchiveInput* input)
{
    if (!input->stop.stop_requested()) return false;
    archive_set_error(reader, ECANCELED, "ROM preparation cancelled");
    return true;
}
static int OpenInput(archive* reader, void* context)
{
    auto* input = static_cast<ArchiveInput*>(context);
    if (Cancelled(reader, input)) return ARCHIVE_FATAL;
    return input->file.open(QIODevice::ReadOnly) ? ARCHIVE_OK : ARCHIVE_FATAL;
}
static la_ssize_t ReadInput(archive* reader, void* context, const void** buffer)
{
    auto* input = static_cast<ArchiveInput*>(context);
    if (Cancelled(reader, input)) return -1;
    const auto count = input->file.read(input->buffer.data(), input->buffer.size());
    if (Cancelled(reader, input)) return -1;
    *buffer = input->buffer.data();
    return count;
}
static la_int64_t SeekInput(archive* reader, void* context, la_int64_t offset, int whence)
{
    auto* input = static_cast<ArchiveInput*>(context);
    if (Cancelled(reader, input)) return ARCHIVE_FATAL;
    qint64 base = whence == SEEK_CUR ? input->file.pos() : whence == SEEK_END ? input->file.size() : 0;
    if (base < 0 || (offset > 0 && base > std::numeric_limits<qint64>::max() - offset) ||
        (offset < 0 && offset < -base) || !input->file.seek(base + offset)) return ARCHIVE_FATAL;
    if (Cancelled(reader, input)) return ARCHIVE_FATAL;
    return input->file.pos();
}
static la_int64_t SkipInput(archive* reader, void* context, la_int64_t count)
{
    auto* input = static_cast<ArchiveInput*>(context);
    const auto before = input->file.pos();
    const auto after = SeekInput(reader, context, count, SEEK_CUR);
    return after < 0 ? -1 : after - before;
}
static int CloseInput(archive*, void* context)
{
    static_cast<ArchiveInput*>(context)->file.close();
    return ARCHIVE_OK;
}
static ArchiveReader OpenArchive(const QString& path, std::stop_token stop)
{
    ArchiveReader reader;
    if (stop.stop_requested() || path.isEmpty() || path.contains(QChar(u'\0'))) return reader;
    reader.input = std::make_unique<ArchiveInput>(path, stop);
    reader.handle.reset(archive_read_new());
    if (!reader || archive_read_support_filter_all(reader.get()) != ARCHIVE_OK ||
        archive_read_support_format_all(reader.get()) != ARCHIVE_OK ||
        archive_read_set_seek_callback(reader.get(), SeekInput) != ARCHIVE_OK ||
        archive_read_open2(reader.get(), reader.input.get(), OpenInput, ReadInput, SkipInput, CloseInput) != ARCHIVE_OK)
        reader.handle.reset();
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

QVector<QString> ListArchive(QString path, std::stop_token stop)
{
    try
    {
        auto reader = OpenArchive(path, stop);
        if (!reader) return {stop.stop_requested() ? "Cancelled" : "Err"};

        QVector<QString> fileList;
        archive_entry* entry = nullptr;
        int status;
        while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK)
        {
            if (stop.stop_requested()) return {"Cancelled"};
            if (!entry) return {stop.stop_requested() ? "Cancelled" : "Err"};
            if (archive_entry_filetype(entry) == AE_IFREG && !archive_entry_hardlink(entry))
            {
                QString name;
                if (!ReadMemberName(entry, name)) return {stop.stop_requested() ? "Cancelled" : "Err"};
                fileList.push_back(std::move(name));
            }
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) return {stop.stop_requested() ? "Cancelled" : "Err"};
        }

        if (stop.stop_requested()) return {"Cancelled"};
        // Warnings and failed headers must not publish a successful partial list.
        if (status != ARCHIVE_EOF) return {stop.stop_requested() ? "Cancelled" : "Err"};
        if (archive_read_free(reader.release()) != ARCHIVE_OK) return {stop.stop_requested() ? "Cancelled" : "Err"};
        std::stable_sort(fileList.begin(), fileList.end(), [](const QString& a, const QString& b) {
            return a.toLower() < b.toLower();
        });
        if (stop.stop_requested()) return {"Cancelled"};
        fileList.prepend("OK");
        return fileList;
    }
    catch (const std::bad_alloc&)
    {
        return {stop.stop_requested() ? "Cancelled" : "Err"};
    }
}

s32 ExtractFileFromArchive(QString path, QString wantedFile, std::unique_ptr<u8[]>& filedata, u32* filesize, std::stop_token stop)
{
    try
    {
        if (wantedFile.isEmpty() || wantedFile.contains(QChar(u'\0'))) return stop.stop_requested() ? -3 : -1;
        auto reader = OpenArchive(path, stop);
        if (!reader) return stop.stop_requested() ? -3 : -1;

        archive_entry* entry = nullptr;
        for (;;)
        {
            if (stop.stop_requested()) return -3;
            const int status = archive_read_next_header(reader.get(), &entry);
            if (stop.stop_requested()) return -3;
            if (status == ARCHIVE_EOF)
                return archive_read_free(reader.release()) == ARCHIVE_OK ? -2 : -1;
            if (status != ARCHIVE_OK || !entry) return stop.stop_requested() ? -3 : -1;

            QString name;
            if (!ReadMemberName(entry, name)) return stop.stop_requested() ? -3 : -1;
            if (name == wantedFile) break;
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) return stop.stop_requested() ? -3 : -1;
        }

        if (archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry) ||
            !archive_entry_size_is_set(entry))
            return stop.stop_requested() ? -3 : -1;
        const la_int64_t size = archive_entry_size(entry);
        // Match loadROMData/decompressROM; the cart parsers own format limits.
        if (size <= 0 || size > 0x40000000) return stop.stop_requested() ? -3 : -1;

        if (stop.stop_requested()) return -3;
        const size_t length = static_cast<size_t>(size);
        // Reject an unreadable body before allocating its entire advertised size.
        u8 first;
        if (archive_read_data(reader.get(), &first, 1) != 1) return stop.stop_requested() ? -3 : -1;
        if (stop.stop_requested()) return -3;

        auto data = std::make_unique_for_overwrite<u8[]>(length);
        data[0] = first;
        size_t total = 1;
        while (total < length)
        {
            if (stop.stop_requested()) return -3;
            const la_ssize_t count = archive_read_data(reader.get(), data.get() + total, std::min(ReadChunk, length - total));
            if (stop.stop_requested()) return -3;
            if (count <= 0 || static_cast<size_t>(count) > length - total) return stop.stop_requested() ? -3 : -1;
            total += static_cast<size_t>(count);
        }

        // Consume the entry's end marker too: a checksum error may arrive only
        // after the last payload bytes. Extra bytes also contradict its size.
        u8 extra;
        if (archive_read_data(reader.get(), &extra, 1) != 0) return stop.stop_requested() ? -3 : -1;
        // Free also closes the reader. Check that result before publishing data;
        // every earlier return/exception is covered by the same RAII deleter.
        if (archive_read_free(reader.release()) != ARCHIVE_OK) return stop.stop_requested() ? -3 : -1;

        if (stop.stop_requested()) return -3;
        filedata = std::move(data);
        if (filesize) *filesize = static_cast<u32>(length);
        return static_cast<s32>(length);
    }
    catch (const std::bad_alloc&)
    {
        return stop.stop_requested() ? -3 : -1;
    }
}

}
