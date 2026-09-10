// SPDX-License-Identifier: GPL-3.0-or-later
// DSiTitle's generated 8 MiB NAND/footer/crypto fixture with real host export.
#include "DSi.h"
#include "DSi_AES.h"
#include "DSi_NAND.h"
#include "FATIO.h"
#include "StorageExportTest.h"
#include <array>
#include <cstring>
#include <vector>
#include <QCoreApplication>
#include <QTemporaryDir>

using namespace melonDS;
using namespace melonDS::DSi_NAND;
using ExportTest::Check;
namespace melonDS {
#include "ROL16.inc"
#include "DeriveNormalKey.inc"
}
namespace melonDS::Platform {
struct FileHandle
{
    std::vector<u8> bytes;
    u64 position = 0;
    QFile* host = nullptr;
};
std::string GetLocalFilePath(const std::string& path)
{
    const QString name = QString::fromStdString(path);
    return (QDir::isAbsolutePath(name) ? name : QDir(ExportTest::root).filePath(name)).toStdString();
}
FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    auto* host = new ExportTest::HostFile(QString::fromStdString(GetLocalFilePath(path)));
    if (!host->open((mode & Write ? QIODevice::WriteOnly : QIODevice::ReadOnly) | QIODevice::Unbuffered))
    { delete host; return nullptr; }
    return new FileHandle{{}, 0, host};
}
bool CloseFile(FileHandle* file) { delete file->host; delete file; return true; }
u64 FileLength(FileHandle* file) { return file->host ? file->host->size() : file->bytes.size(); }
bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    const s64 base = origin == FileSeekOrigin::Start ? 0 : static_cast<s64>(
        origin == FileSeekOrigin::End ? FileLength(file) : file->position);
    if (offset < -base || offset > static_cast<s64>(FileLength(file)) - base) return false;
    file->position = base + offset;
    return file->host ? file->host->seek(file->position) : true;
}
u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    if (!size) return 0;
    if (file->host) return file->host->read(static_cast<char*>(data), size * count) / size;
    if (ExportTest::backingRead)
    { ExportTest::backingRead = false; ++ExportTest::faults; return 0; }
    count = std::min(count, (file->bytes.size() - file->position) / size);
    std::memcpy(data, file->bytes.data() + file->position, size * count);
    file->position += size * count;
    return count;
}
u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    if (!size) return 0;
    if (file->host) return file->host->write(static_cast<const char*>(data), size * count) / size;
    count = std::min(count, (file->bytes.size() - file->position) / size);
    std::memcpy(file->bytes.data() + file->position, data, size * count);
    file->position += size * count;
    return count;
}
bool FileFlush(FileHandle* file) { return file->host ? file->host->flush() : true; }
void Log(LogLevel, const char*, ...) {}
#define QSaveFile ExportTest::SaveFile
#include "StorageExportAtomic.inc"
#undef QSaveFile
}
static void StorageDiskOpen(const ff_disk_read_cb& read, const ff_disk_write_cb& write, LBA_t count)
{
    // Same partition bound as DSiTitle.cpp, not a physical NAND dump.
    ff_disk_open(read, write, count - (0x10EE00 / 512) - 1);
}
#define ff_disk_open StorageDiskOpen
#define f_read ExportTest::Read
#define f_close ExportTest::Close
#include "../src/DSi_NAND.cpp"
#undef f_close
#undef f_read
#undef ff_disk_open

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Check(argc == 2, "one export case");
    const std::string mode = argv[1];
    QTemporaryDir temp;
    Check(temp.isValid(), "temporary export directory");
    ExportTest::root = temp.path();
    const QString output = temp.filePath("output.bin");
    const QByteArray old("prior host destination\0binary", 29);
    ExportTest::Put(output, old);
    const QStringList entries = ExportTest::Entries(temp.path());
    auto* backing = new Platform::FileHandle{std::vector<u8>(8 * 1024 * 1024)};
    std::memcpy(backing->bytes.data() + backing->bytes.size() - 0x40, "DSi eMMC CID/CPU", 16);
    NANDImage image(backing, DSiKey{});
    Check(bool(image), "synthetic footer and key inputs");
    {
        NANDMount mount(image);
        FF_MKFS_PARM options{}; options.fmt = FM_FAT | FM_SFD;
        std::array<u8, 4096> work{};
        Check(f_mkfs("0:", &options, work.data(), work.size()) == FR_OK, "generated partition format");
    }
    NANDMount mount(image);
    for (const char* path : {"0:/title", "0:/title/00030004", "0:/title/00030004/12345678",
        "0:/title/00030004/12345678/data"}) Check(f_mkdir(path) == FR_OK, "title directory");
    const char* path = "0:/title/00030004/12345678/data/public.sav";
    const QByteArray payload = ExportTest::Payload(mode == "empty" ? 0 : 12291);
    ExportTest::targetSize = payload.size();
    if (payload.isEmpty())
    {
        // ImportFile rejects zero-length input; an empty file is nevertheless a
        // legitimate export source. Create it using the real FatFs API.
        FF_FIL empty;
        Check(f_open(&empty, path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK && f_close(&empty) == FR_OK, "generated empty title file");
    }
    else Check(mount.ImportFile(path, reinterpret_cast<const u8*>(payload.constData()), payload.size()), "generated binary title save");
    const auto imageBefore = backing->bytes;
    ExportTest::ReplacementLock lock;
    if (mode == "replacement") lock.Lock(output);
    ExportTest::Reset(ExportTest::Parse(mode));
    bool result = mount.ExportTitleData(0x00030004, 0x12345678, TitleData_PublicSav, "output.bin");
    ExportTest::active = false;
    lock.Unlock();
    const bool failing = ExportTest::fault != ExportTest::Fault::None || mode == "replacement";
    std::printf("nand_export=%s result=%d preserved=%d reads=%u closes=%u faults=%u commits=%u\n",
        mode.c_str(), result, ExportTest::Bytes(output) == old, ExportTest::reads,
        ExportTest::closes, ExportTest::faults, ExportTest::commits);
    if (failing)
    {
        Check(!result, "public NAND export reports failure");
        Check(ExportTest::Bytes(output) == old, "NAND failure preserves prior host bytes");
        Check(mode == "replacement" || ExportTest::faults == 1, "requested NAND failure reached once");
        Check(ExportTest::Entries(temp.path()) == entries, "NAND failure cleans temp output");
        Check(ExportTest::closes == 1, "NAND source closed exactly once");
        ExportTest::Reset();
        Check(mount.ExportFile(path, output.toStdString().c_str()), "public NAND export retry");
        ExportTest::active = false;
    }
    else Check(result, "normal NAND title export");
    Check(ExportTest::Bytes(output) == payload, "exact empty/binary/multiblock NAND output");
    Check(backing->bytes == imageBefore, "export preserves generated NAND image");
    Check(ExportTest::Entries(temp.path()) == entries, "successful NAND commit leaves no temp output");
    std::printf("PASS: %s\n", mode.c_str());
}
