// SPDX-License-Identifier: GPL-3.0-or-later
// Actual FAT sector and Qt Platform I/O methods, extracted from current source.
// Only FAT construction skips formatting, and QFile::seek has a failure switch.
// QFile/QIODevice errors return -1: https://doc.qt.io/qt-6/qiodevice.html#read
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <QFile>
#include <QTemporaryFile>
#include "FATStorage.h"

using namespace melonDS;
static QFile* Scratch;
namespace melonDS::Platform
{
#include "DSiSDFATQtMethods.inc"
}
namespace melonDS
{
using namespace Platform;
FATStorage::FATStorage(const std::string&, u64 size, bool readonly, const std::optional<std::string>&)
    : ReadOnly(readonly), File(reinterpret_cast<FileHandle*>(Scratch)), FileSize(size) {}
FATStorage::~FATStorage() = default;
#include "DSiSDFATMethods.inc"
}
static void Require(bool ok, const char* message)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
class ObservedFile final : public QFile
{
public:
    using QFile::QFile;
    bool FailSeek = false;
    unsigned Reads = 0, Writes = 0;
    bool seek(qint64 offset) override { return !FailSeek && QFile::seek(offset); }
protected:
    qint64 readData(char* data, qint64 len) override
    { ++Reads; return QFile::readData(data, len); }
    qint64 writeData(const char* data, qint64 len) override
    { ++Writes; return QFile::writeData(data, len); }
};

int main(int argc, char** argv)
{
    Require(argc == 2, "one case required");
    std::string name = argv[1];
    // QTemporaryFile creates/removes only this test's unique scratch in the build.
    QTemporaryFile temp("DSiSDFATBacking-XXXXXX");
    Require(temp.open(), "scratch file");
    QByteArray original(1536, '\0');
    for (qsizetype i = 0; i < original.size(); ++i) original[i] = char((i * 13 + i / 512 + 7) & 255);
    Require(temp.write(original) == original.size() && temp.flush(), "scratch seed");
    temp.close();
    ObservedFile file(temp.fileName());
    auto mode = name == "read-error" ? QIODevice::WriteOnly : name == "write-error" ? QIODevice::ReadOnly : QIODevice::ReadWrite;
    Require(file.open(mode | QIODevice::Unbuffered), "scratch reopen");
    Scratch = &file;
    FATStorage storage("synthetic", 1536, false);
    std::array<u8, 1024> data;
    data.fill(0xA6);

    if (name == "seek-read" || name == "seek-write")
    {
        Require(file.seek(0), "initial position"); file.FailSeek = true;
        u32 result = name == "seek-read" ? storage.ReadSectors(1, 1, data.data()) : storage.WriteSectors(1, 1, data.data());
        file.flush();
        QFile readback(temp.fileName()); Require(readback.open(QIODevice::ReadOnly), "readback open");
        bool unchanged = readback.readAll() == original;
        std::printf("result=%u reads=%u writes=%u backing_unchanged=%d\n", result, file.Reads, file.Writes, unchanged);
        Require(result == 0, "failed seek did not return zero");
        Require(file.Reads == 0 && file.Writes == 0 && file.pos() == 0, "I/O continued after failed seek");
        Require(unchanged, "failed seek changed backing bytes");
        Require(std::all_of(data.begin(), data.end(), [](u8 b) { return b == 0xA6; }), "failed seek changed caller buffer");
        file.FailSeek = false;
        Require(storage.ReadSectors(1, 1, data.data()) == 1, "retry after seek recovery");
        Require(std::memcmp(data.data(), original.data()+512, 512) == 0, "retry addressed wrong sector");
    }
    else if (name == "normal")
    {
        Require(storage.ReadSectors(1, 1, data.data()) == 1, "sector read count");
        Require(std::memcmp(data.data(), original.data()+512, 512) == 0, "sector read offset/data");
        data.fill(0xA6);
        Require(storage.WriteSectors(1, 1, data.data()) == 1 && file.flush(), "sector write count");
        original.replace(512, 512, QByteArray(512, char(0xA6)));
        Require(file.seek(0) && file.readAll() == original, "sector write changed neighboring bytes");
        Require(storage.ReadSectors(3, 1, data.data()) == 0 && storage.WriteSectors(3, 1, data.data()) == 0,
                "virtual capacity boundary");
    }
    else if (name == "sparse")
    {
        // Virtual capacity remains 1536 bytes; only a complete first sector is
        // materialized. This is intentionally supported by FATStorage::Load.
        Require(file.resize(512), "create unmaterialized tail");
        Require(storage.GetSectorCount() == 3, "virtual size retained");
        Require(storage.ReadSectors(0, 2, data.data()) == 2, "mixed data/EOF count");
        Require(std::memcmp(data.data(), original.data(), 512) == 0, "materialized sector preserved");
        Require(std::all_of(data.begin()+512, data.end(), [](u8 b) { return b == 0; }), "EOF sector not zero-filled");
        data.fill(0xA6);
        Require(storage.ReadSectors(2, 1, data.data()) == 1, "beyond physical EOF within virtual capacity");
        Require(std::all_of(data.begin(), data.begin()+512, [](u8 b) { return b == 0; }), "sparse EOF bytes");
        Require(data[512] == 0xA6 && file.size() == 512, "sparse read touched adjacent buffer/file");
        data.fill(0xA6);
        Require(storage.WriteSectors(2, 1, data.data()) == 1 && file.flush(), "materialize sparse sector");
        Require(file.size() == 1536, "sparse write size");
        QByteArray expected = original.left(512) + QByteArray(512, '\0') + QByteArray(512, char(0xA6));
        Require(file.seek(0) && file.readAll() == expected, "sparse hole and neighboring bytes");
    }
    else
    {
        Require(name == "read-error" || name == "write-error", "unknown case");
        // Real QIODevice access-mode errors, through unchanged Platform methods.
        auto* handle = reinterpret_cast<Platform::FileHandle*>(&file);
        u64 raw = name == "read-error" ? Platform::FileRead(data.data(), 512, 1, handle) : Platform::FileWrite(data.data(), 512, 1, handle);
        Require(raw == UINT64_MAX, "Qt error was not propagated as unsigned -1");
        u32 result = name == "read-error" ? storage.ReadSectors(0, 1, data.data()) : storage.WriteSectors(0, 1, data.data());
        std::printf("platform_result=%llu sector_result=%u atEnd=%d\n", static_cast<unsigned long long>(raw), result, Platform::IsEndOfFile(handle));
        Require(result == 0, "hard I/O error reported as a sector count");
        Require(std::all_of(data.begin(), data.end(), [](u8 b) { return b == 0xA6; }), "hard read error zero-filled as sparse success");
    }
    std::printf("PASS: %s\n", name.c_str());
}
