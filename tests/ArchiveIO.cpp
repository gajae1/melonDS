// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the current ArchiveUtil.cpp and real libarchive with generated files.
// Only documented reader/metadata results and allocation failure are injected;
// no archive parser or production extraction algorithm is copied into this test.
// Contracts: libarchive/libarchive (official repository), libarchive/
// archive_read_header.3, archive_read_data.3, archive_entry_stat.3,
// archive_read_free.3 and archive_entry.c (pathname_utf8 can return nullptr).
#include <QCoreApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include "ArchiveUtil.h"
#include "Platform.h"

using namespace melonDS;

static bool guardAllocation = false;
static bool failAllocation = false;
static int blockedAllocations = 0;

// Never allocate a claimed ROM-sized payload for an oversized-metadata test.
// The normal fixtures are much smaller than this test-only 1 MiB guard.
void* operator new[](std::size_t size)
{
    if (guardAllocation && (failAllocation || size > 1024 * 1024))
    {
        ++blockedAllocations;
        throw std::bad_alloc();
    }
    if (void* data = std::malloc(size ? size : 1)) return data;
    throw std::bad_alloc();
}
void operator delete[](void* data) noexcept { std::free(data); }
void operator delete[](void* data, std::size_t) noexcept { std::free(data); }

namespace melonDS::Platform
{
void Log(LogLevel, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}
}

enum class HeaderFault { None, Size, UnknownSize, InvalidName, NullName };
enum class DataFault { None, Chunked, EarlyEOF, Error };
static HeaderFault headerFault = HeaderFault::None;
static DataFault dataFault = DataFault::None;
static la_int64_t reportedSize = 0;
static int dataCalls = 0;
static std::vector<archive*> readers;

static archive* OpenReader()
{
    auto* reader = archive_read_new();
    if (reader) readers.push_back(reader);
    return reader;
}

static int FreeReader(archive* reader)
{
    readers.erase(std::remove(readers.begin(), readers.end(), reader), readers.end());
    return archive_read_free(reader);
}

static int NextHeader(archive* reader, archive_entry** entry)
{
    const int result = archive_read_next_header(reader, entry);
    if (result < ARCHIVE_OK)
    {
        const char* error = archive_error_string(reader);
        std::fprintf(stderr, "libarchive next_header returned %d: %s\n", result, error ? error : "(no detail)");
    }
    if (result != ARCHIVE_OK) return result;
    switch (headerFault)
    {
    case HeaderFault::Size: archive_entry_set_size(*entry, reportedSize); break;
    case HeaderFault::UnknownSize: archive_entry_unset_size(*entry); break;
    case HeaderFault::InvalidName: archive_entry_set_pathname_utf8(*entry, "\xFF.nds"); break;
    case HeaderFault::NullName: archive_entry_set_pathname_utf8(*entry, nullptr); break;
    default: break;
    }
    return result;
}

static la_ssize_t ReadData(archive* reader, void* buffer, size_t length)
{
    ++dataCalls;
    if (dataCalls > 1 && dataFault == DataFault::EarlyEOF) return 0;
    if (dataCalls > 1 && dataFault == DataFault::Error)
    {
        archive_set_error(reader, 0, "injected archive data read failure");
        return ARCHIVE_FATAL;
    }
    if (dataFault != DataFault::None) length = std::min<size_t>(length, 257);
    return archive_read_data(reader, buffer, length);
}

// Include the production translation unit, as CartReplacement already does for
// SaveManager.cpp. Do not also compile ArchiveUtil.cpp into the ArchiveIO target.
#define archive_read_new OpenReader
#define archive_read_free FreeReader
#define archive_read_next_header NextHeader
#define archive_read_data ReadData
#include "ArchiveUtil.cpp"
#undef archive_read_data
#undef archive_read_next_header
#undef archive_read_free
#undef archive_read_new

static int failures = 0;

static void Check(bool condition, const char* message)
{
    if (!condition) { ++failures; std::fprintf(stderr, "%s\n", message); }
}

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void CheckReaders()
{
    Check(readers.empty(), "Archive reader resources were retained after returning to the caller");
    // Clean up baseline leaks after observing them, so one failure cannot exhaust
    // handles or make the temporary directory survive the test process.
    for (auto* reader : readers) archive_read_free(reader);
    readers.clear();
}

struct Member
{
    QString name;
    QByteArray data;
    unsigned int type = AE_IFREG;
    QString link;
};

static QByteArray MakeArchive(const std::string& format, const std::vector<Member>& members)
{
    QByteArray bytes(1024 * 1024, '\0');
    size_t used = 0;
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    Require(writer != nullptr, "Fixture writer allocation failed");
    int status = format == "zip" ? archive_write_set_format_zip(writer.get()) :
                 format == "7z" ? archive_write_set_format_7zip(writer.get()) :
                                  archive_write_set_format_pax_restricted(writer.get());
    Require(status == ARCHIVE_OK, "Fixture archive format is unavailable");
    if (format == "zip")
    {
        Require(archive_write_zip_set_compression_store(writer.get()) == ARCHIVE_OK, "Fixture ZIP store failed");
        // The Windows ZIP writer otherwise uses a legacy code page even in a
        // UTF-8 process locale. Declare UTF-8 so bit 11 identifies the encoding.
        Require(archive_write_set_format_option(writer.get(), "zip", "hdrcharset", "UTF-8") == ARCHIVE_OK,
                "Fixture ZIP UTF-8 header setup failed");
    }
    Require(archive_write_set_bytes_per_block(writer.get(), 0) == ARCHIVE_OK, "Fixture block setup failed");
    Require(archive_write_open_memory(writer.get(), bytes.data(), bytes.size(), &used) == ARCHIVE_OK,
            "Fixture memory writer could not open");
    for (const auto& member : members)
    {
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        Require(entry != nullptr, "Fixture entry allocation failed");
        archive_entry_set_pathname_utf8(entry.get(), member.name.toUtf8().constData());
        archive_entry_set_filetype(entry.get(), member.type);
        archive_entry_set_perm(entry.get(), 0644);
        archive_entry_set_size(entry.get(), member.type == AE_IFREG ? member.data.size() : 0);
        if (member.type == AE_IFLNK)
            archive_entry_set_symlink_utf8(entry.get(), member.link.toUtf8().constData());
        Require(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK, "Fixture header write failed");
        if (member.type == AE_IFREG && !member.data.isEmpty())
            Require(archive_write_data(writer.get(), member.data.constData(), member.data.size()) == member.data.size(),
                    "Fixture payload write failed");
        Require(archive_write_finish_entry(writer.get()) == ARCHIVE_OK, "Fixture entry finish failed");
    }
    Require(archive_write_close(writer.get()) == ARCHIVE_OK, "Fixture archive close failed");
    bytes.resize(static_cast<int>(used));
    return bytes;
}

static void Write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    Require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush(),
            "Fixture temporary file write failed");
}

static QVector<QString> List(const QString& path)
{
    QVector<QString> result;
    try { result = Archive::ListArchive(path); }
    catch (const std::exception& error)
    {
        Check(false, "Listing propagated an exception");
        std::fprintf(stderr, "%s\n", error.what());
    }
    CheckReaders();
    return result;
}

static s32 Extract(const QString& path, const QString& name, const QByteArray* expected = nullptr,
                   bool withSize = true)
{
    auto output = std::make_unique<u8[]>(16);
    std::memset(output.get(), 0xA5, 16);
    auto* previous = output.get();
    u32 size = 16;
    s32 result = -999;
    dataCalls = 0;
    guardAllocation = true;
    try { result = Archive::ExtractFileFromArchive(path, name, output, withSize ? &size : nullptr); }
    catch (const std::exception& error)
    {
        Check(false, "Extraction propagated an exception to the noexcept ROM loader");
        std::fprintf(stderr, "%s\n", error.what());
    }
    guardAllocation = false;
    if (expected)
    {
        Check(result == expected->size() && (!withSize || size == static_cast<u32>(expected->size())),
              "Successful extraction did not report the full member size");
        Check(result == expected->size() && output &&
              std::memcmp(output.get(), expected->constData(), expected->size()) == 0,
              "Successful extraction did not return the exact member bytes");
    }
    else
    {
        Check(result < 0, "Missing, invalid or unreadable member was reported as successfully extracted");
        Check(output.get() == previous && size == 16 && output[0] == 0xA5 && output[15] == 0xA5,
              "Failed extraction changed the caller's previous output buffer or size");
    }
    CheckReaders();
    return result;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
#ifdef _WIN32
    std::setlocale(LC_ALL, ".UTF-8");
#else
    std::setlocale(LC_ALL, "");
#endif
    if (argc != 2) return 2;
    const std::string scenario = argv[1];
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    const QString path = directory.filePath(QStringLiteral("archive-\uD55C\uAE00.bin"));
    const QString unicode = QStringLiteral("\uD55C\uAE00/\uAC8C\uC784.nds");
    QByteArray payload(16384, '\0');
    for (int i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>((i * 37 + i / 11) & 255);
    try
    {
        if (scenario == "formats" || scenario == "locale")
        {
            const char* activeLocale = std::setlocale(LC_ALL, nullptr);
            Require(activeLocale != nullptr, "Fixture writer locale query failed");
            const std::string writerLocale(activeLocale);
            for (const auto& format : {"zip", "7z", "tar"})
            {
                Write(path, MakeArchive(format, {{"z.nds", payload}, {"A.gba", QByteArray(4096, '\x73')},
                                                 {unicode, payload}}));
                // Generate valid Unicode headers in the writer's UTF-8 locale;
                // only production List/Extract are exercised in the C locale.
                if (scenario == "locale")
                    Require(std::setlocale(LC_ALL, "C") != nullptr, "Fixture reader C locale setup failed");
                std::printf("Reading generated %s with %s locale\n", format,
                            scenario == "locale" ? "C" : "writer");
                std::fflush(stdout);
                Check(List(path) == QVector<QString>({"OK", "A.gba", "z.nds", unicode}),
                      "Regular Unicode members were lost or the existing case-insensitive list order changed");
                Extract(path, "z.nds", &payload);
                const QByteArray gba(4096, '\x73');
                Extract(path, "A.gba", &gba, false);
                Extract(path, unicode, &payload);
                if (scenario == "locale")
                    Require(std::setlocale(LC_ALL, writerLocale.c_str()) != nullptr, "Fixture writer locale restore failed");
                std::printf("Generated %s list/extract checked\n", format);
            }
        }
        else if (scenario == "missing")
        {
            Write(path, MakeArchive("zip", {{"present.nds", payload}}));
            // Existing callers accept any negative failure; clean EOF now also
            // distinguishes a missing selection from an open/header/data error.
            Check(Extract(path, "absent.nds") == -2, "Clean EOF did not identify a missing member");
            const QString absent = directory.filePath("missing.zip");
            Check(List(absent) == QVector<QString>({"Err"}), "Missing archive was listed as readable");
            Check(Extract(absent, "present.nds") == -1, "Open failure was confused with a missing member");
        }
        else if (scenario == "empty")
        {
            Write(path, MakeArchive("tar", {}));
            Check(List(path) == QVector<QString>({"OK"}), "Clean empty archive EOF was treated as a read error");
            Extract(path, "absent.nds");
            Write(path, MakeArchive("zip", {{"empty.nds", {}}}));
            Extract(path, "empty.nds");
            Write(path, {});
            // libarchive's documented empty format identifies this as clean EOF.
            Check(List(path) == QVector<QString>({"OK"}), "The libarchive empty format was treated as a read error");
            Extract(path, "absent.nds");
            Write(path, "This is not an archive");
            Check(List(path) == QVector<QString>({"Err"}), "Unrecognized input was listed as a readable archive");
            Extract(path, "absent.nds");
        }
        else if (scenario == "corrupt-header")
        {
            auto bytes = MakeArchive("tar", {{"first.nds", payload}, {"second.nds", payload}});
            const int secondHeader = 512 + ((payload.size() + 511) / 512) * 512;
            Require(bytes.mid(secondHeader, 10) == "second.nds", "Fixture second tar header was not found");
            bytes[secondHeader + 148] = 'X'; // Invalid checksum field, after one readable member.
            Write(path, bytes);
            Check(List(path) == QVector<QString>({"Err"}), "Header error returned a successful partial member list");
            Extract(path, "second.nds");
        }
        else if (scenario == "truncated-data")
        {
            auto bytes = MakeArchive("tar", {{"payload.nds", payload}});
            Require(bytes.startsWith("payload.nds"), "Fixture first tar header was not found");
            bytes.truncate(512 + payload.size() / 2);
            Write(path, bytes);
            Extract(path, "payload.nds");
        }
        else if (scenario == "crc")
        {
            auto bytes = MakeArchive("zip", {{"payload.nds", payload}});
            Require(bytes.startsWith(QByteArray("PK\x03\x04", 4)), "Fixture ZIP local header was not found");
            const auto le16 = [&](int offset) {
                return static_cast<unsigned char>(bytes[offset]) |
                       (static_cast<unsigned char>(bytes[offset + 1]) << 8);
            };
            const int start = 30 + le16(26) + le16(28);
            Require(bytes.mid(start, payload.size()) == payload, "Fixture stored ZIP payload was not found");
            bytes[start] = static_cast<char>(bytes.at(start) ^ 0x40);
            Write(path, bytes);
            Extract(path, "payload.nds");
        }
        else if (scenario == "member-type")
        {
            Write(path, MakeArchive("tar", {{"payload.nds", payload}, {"folder/", {}, AE_IFDIR},
                                            {"alias.nds", {}, AE_IFLNK, "payload.nds"}}));
            Check(List(path) == QVector<QString>({"OK", "payload.nds"}), "Non-regular members appeared in the file list");
            Extract(path, "folder/");
            Extract(path, "alias.nds");
        }
        else if (scenario == "chunked-read" || scenario == "early-eof" || scenario == "read-error")
        {
            Write(path, MakeArchive("7z", {{"payload.nds", payload}}));
            dataFault = scenario == "chunked-read" ? DataFault::Chunked :
                        scenario == "early-eof" ? DataFault::EarlyEOF : DataFault::Error;
            Extract(path, "payload.nds", dataFault == DataFault::Chunked ? &payload : nullptr);
        }
        else if (scenario == "limits")
        {
            // Match loadROMData/decompressROM's shared 1 GiB input bound.
            // Cart parsers own format support; filenames do not change this cap.
            // The allocation guard avoids constructing a real oversized payload.
            Write(path, MakeArchive("zip", {{"payload.nds", payload}}));
            headerFault = HeaderFault::Size;
            for (const la_int64_t size : {la_int64_t{-1}, la_int64_t{0x40000000} + 1})
            {
                reportedSize = size;
                Extract(path, "payload.nds");
            }
            headerFault = HeaderFault::UnknownSize;
            Extract(path, "payload.nds");
            Check(blockedAllocations == 0, "Invalid metadata reached a payload allocation before being rejected");
        }
        else if (scenario == "invalid-name" || scenario == "null-name")
        {
            Write(path, MakeArchive("zip", {{"payload.nds", payload}}));
            headerFault = scenario == "invalid-name" ? HeaderFault::InvalidName : HeaderFault::NullName;
            Check(List(path) == QVector<QString>({"Err"}), "Invalid UTF-8 or missing pathname became a successful member list");
            Extract(path, QString::fromUtf8("\xFF.nds"));
        }
        else if (scenario == "allocation")
        {
            Write(path, MakeArchive("zip", {{"payload.nds", payload}}));
            failAllocation = true;
            Extract(path, "payload.nds");
            Check(blockedAllocations > 0, "Allocation failure fixture was not exercised");
        }
        else return 2;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Fixture error (not a valid red result): %s\n", error.what());
        return 2;
    }
    std::printf("Archive %s: %d failures\n", scenario.c_str(), failures);
    return failures ? 1 : 0;
}
