// SPDX-License-Identifier: GPL-3.0-or-later
// Generated FAT images, full FATStorage/FatFs and current Qt I/O methods.
// Lifecycle faults use QFile boundaries; export faults also use FatFs I/O seams.
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include "FATStorage.h"
#include "StorageExportTest.h"

using namespace melonDS;
namespace {
enum class Failure { None, Read, Seek, LengthAndRead, Write };
Failure failure = Failure::None;
unsigned openImages = 0, imageWrites = 0, indexWrites = 0, injectedFailures = 0;
constexpr u64 Capacity = 8 * 1024 * 1024;

void Require(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

class ImageFile final : public QFile
{
public:
    using QFile::QFile;
    ~ImageFile() override { --openImages; }
    qint64 size() const override
    { return failure == Failure::LengthAndRead ? 0 : QFile::size(); }
    bool seek(qint64 offset) override
    {
        if (failure == Failure::Seek)
        { failure = Failure::None; ++injectedFailures; return false; }
        return QFile::seek(offset);
    }
protected:
    qint64 readData(char* data, qint64 size) override
    {
        if (ExportTest::backingRead)
        { ExportTest::backingRead = false; ++ExportTest::faults; return -1; }
        if (failure == Failure::Read || failure == Failure::LengthAndRead)
        { failure = Failure::None; ++injectedFailures; return -1; }
        return QFile::readData(data, size);
    }
    qint64 writeData(const char* data, qint64 size) override
    {
        ++imageWrites;
        if (failure == Failure::Write) { ++injectedFailures; return -1; }
        return QFile::writeData(data, size);
    }
};

QByteArray Bytes(const QString& path)
{
    QFile file(path);
    Require(file.open(QIODevice::ReadOnly), "readback open");
    return file.readAll();
}

void WriteBytes(const QString& path, const QByteArray& data)
{
    QFile file(path);
    Require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "fixture write");
}
}

namespace melonDS::Platform {
FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    const QString name = QString::fromStdString(path);
    const bool image = name.endsWith(".img");
    QFile* file = image ? static_cast<QFile*>(new ImageFile(name)) : new ExportTest::HostFile(name);
    if (image) ++openImages;
    QIODevice::OpenMode access = QIODevice::Unbuffered;
    if (mode & Read) access |= QIODevice::ReadOnly;
    if (mode & Write) access |= QIODevice::WriteOnly;
    if ((mode & Write) && !(mode & Preserve)) access |= QIODevice::Truncate;
    if (mode & Text) access |= QIODevice::Text;
    if (!file->open(access)) { delete file; return nullptr; }
    if (!image && (mode & Write) && name.endsWith(".idx")) ++indexWrites;
    return reinterpret_cast<FileHandle*>(file);
}
FileHandle* OpenLocalFile(const std::string& path, FileMode mode) { return OpenFile(path, mode); }
bool LocalFileExists(const std::string& path) { return QFile::exists(QString::fromStdString(path)); }
bool CloseFile(FileHandle* handle) { delete reinterpret_cast<QFile*>(handle); return true; }
void Log(LogLevel, const char* format, ...)
{
    if (ExportTest::active && std::string(format).find("sync") != std::string::npos) ++ExportTest::errors;
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args);
}
#include "DSiSDFATQtMethods.inc"
std::string GetLocalFilePath(const std::string& path) { return path; }
#define QSaveFile ExportTest::SaveFile
#include "StorageExportAtomic.inc"
#undef QSaveFile
}

// Compile the entire current implementation; only FatFs I/O boundaries change.
#define f_read ExportTest::Read
#define f_close ExportTest::Close
#include "../src/FATStorage.cpp"
#undef f_read
#undef f_close

static void TestExport(const std::string& scenario, const QString& path, const QString& source)
{
    using namespace ExportTest;
    const std::string mode = scenario.substr(7);
    const bool newFile = mode == "new-read" || mode == "new-conflict";
    const bool mixed = mode == "mixed";
    const bool conflict = mode == "host-conflict" || mode == "new-conflict" || mode == "host-collision";
    const QString output = source + (newFile ? "/NEW.BIN" : "/KEEP.BIN");
    const QString index = path + ".idx";
    QByteArray before, indexBefore;
    std::filesystem::file_time_type hostTime;
    const QByteArray next = Payload(mode == "empty" ? 0 : mode == "guest-collision" ? 8193 : 12291);
    targetSize = next.size();
    ReplacementLock lock;
    QStringList entries;
    QStringList rootEntries;
    {
        FATStorage storage(path.toStdString(), Capacity, false, source.toStdString());
        Check(storage.IsValid(), "export seed valid");
        before = newFile ? QByteArray{} : ExportTest::Bytes(output);
        if (!newFile) hostTime = std::filesystem::last_write_time(PathFromUTF8(output.toStdString()));
        indexBefore = ExportTest::Bytes(index);
        entries = Entries(source);
        rootEntries = Entries(QFileInfo(path).absolutePath());
        FF_FILINFO timestamp{};
        const auto guestTimestamp = [&](bool restore)
        {
            ff_disk_open([&](BYTE* data, LBA_t start, UINT count) { return storage.ReadSectors(start, count, data); },
                [&](const BYTE* data, LBA_t start, UINT count) { return storage.WriteSectors(start, count, data); }, storage.GetSectorCount());
            FATFS volume;
            Check(f_mount(&volume, "0:", 1) == FR_OK, "guest timestamp fixture mount");
            Check((restore ? f_utime("0:/KEEP.BIN", &timestamp) : f_stat("0:/KEEP.BIN", &timestamp)) == FR_OK,
                "preserve exact guest FAT timestamp");
            f_unmount("0:"); ff_disk_close();
        };
        if (mode == "guest-collision") guestTimestamp(false);
        Check(storage.InjectFile(newFile ? "NEW.BIN" : "KEEP.BIN",
            reinterpret_cast<u8*>(const_cast<char*>(next.constData())), next.size()), "guest update");
        if (mode == "guest-collision") guestTimestamp(true);
        if (mixed)
        {
            auto other = Payload(513);
            Check(storage.InjectFile("ZGOOD.BIN", reinterpret_cast<u8*>(other.data()), other.size()), "second guest file");
            entries.append("ZGOOD.BIN"); entries.sort();
        }
        if (mode == "replacement") lock.Lock(output);
        Reset((newFile || conflict || mixed) ? Fault::Read : Parse(mode));
    } // Actual production destructor -> Save -> ExportChanges -> ExportFile.
    active = false;
    lock.Unlock();
    const bool failing = fault != Fault::None || mode == "replacement";
    const bool indexFailure = fault == Fault::IndexWrite || fault == Fault::IndexCommit;
    const bool preserved = newFile ? !QFile::exists(output) : ExportTest::Bytes(output) == before;
    std::printf("export=%s preserved=%d index_unchanged=%d reads=%u closes=%u faults=%u commits=%u owner_errors=%u\n",
        mode.c_str(), preserved, ExportTest::Bytes(index) == indexBefore, reads, closes, faults, commits, errors);
    if (failing)
    {
        Check(indexFailure || preserved, "failed export preserves prior host destination");
        if (mixed)
        {
            const auto record = [](const QByteArray& text)
            {
                QByteArray result;
                for (const auto& line : text.split('\n')) if (line.endsWith(" KEEP.BIN")) result += line + '\n';
                return result;
            };
            Check(record(ExportTest::Bytes(index)) == record(indexBefore), "failed entry unchanged while another file succeeds");
            Check(ExportTest::Bytes(source + "/ZGOOD.BIN") == Payload(513) && ExportTest::Bytes(index).contains(" ZGOOD.BIN\n"),
                "other file and its index commit independently");
        }
        else Check(ExportTest::Bytes(index) == indexBefore, "failed export/index commit preserves exact prior index");
        Check(mode == "replacement" || faults == 1, "requested failure reached once");
        Check(errors > 0, "destructor reports sync failure");
        Check(Entries(source) == entries, "failed export leaves no temporary file");
        Check(Entries(QFileInfo(path).absolutePath()) == rootEntries, "failed index write leaves no temporary file");
        if (conflict)
        {
            QByteArray hostEdit("independent host edit after failed export\0binary", 47);
            if (mode == "host-collision")
            {
                hostEdit = before;
                hostEdit[hostEdit.size() / 2] ^= 0x5A;
            }
            Put(output, hostEdit);
            if (mode == "host-collision")
                std::filesystem::last_write_time(PathFromUTF8(output.toStdString()), hostTime);
            const QByteArray imageBefore = ExportTest::Bytes(path);
            {
                FATStorage retry(path.toStdString(), Capacity, false, source.toStdString());
                Check(!retry.IsValid(), "conflicting retry refuses safely");
            }
            Check(ExportTest::Bytes(path) == imageBefore && ExportTest::Bytes(output) == hostEdit &&
                ExportTest::Bytes(index) == indexBefore, "conflict preserves guest image host edit and index");
            // Resolve only this generated conflict by restoring the prior host side.
            if (newFile) Check(QFile::remove(output), "remove generated conflicting host file");
            else
            {
                Put(output, before);
                std::filesystem::last_write_time(PathFromUTF8(output.toStdString()), hostTime);
            }
        }
        Reset();
        {
            FATStorage retry(path.toStdString(), Capacity, false, source.toStdString());
            Check(retry.IsValid(), "unchanged-host retry opens");
        }
        active = false;
    }
    Check(ExportTest::Bytes(output) == next, "normal/retry exact empty binary multiblock export");
    Check(ExportTest::Bytes(index).contains(QByteArray::number(next.size()) + " "), "successful export indexed");
    Check(openImages == 0, "export lifecycle releases backing image");
    std::printf("PASS: %s\n", scenario.c_str());
}

static void TestReconciliation(const std::string& mode, const QString& path, const QString& source)
{
    using namespace ExportTest;
    const QString host = source + "/KEEP.BIN", index = path + ".idx";
    QByteArray expected = ExportTest::Bytes(host);
    if (mode == "no-source-index-error")
    {
        Reset(Fault::IndexCommit);
        FATStorage storage(path.toStdString(), Capacity, false);
        active = false;
        Check(storage.IsValid() && faults == 1, "image-only storage does not require host-sync index support");
        Check(storage.InjectFile("KEEP.BIN", reinterpret_cast<u8*>(expected.data()), expected.size()), "image-only guest I/O after sidecar failure");
        Check(!QFile::exists(index), "failed optional sidecar remains absent");
        std::printf("PASS: sync-%s\n", mode.c_str());
        return;
    }
    {
        FATStorage seed(path.toStdString(), Capacity, false,
            mode == "no-index-adoption" ? std::nullopt : std::optional(source.toStdString()));
        Check(seed.IsValid(), "reconciliation seed");
        if (mode == "no-index-adoption")
            Check(seed.InjectFile("OLDONLY.BIN", reinterpret_cast<u8*>(expected.data()), expected.size()), "unindexed adoption fixture");
    }
    if (mode.starts_with("legacy-"))
    {
        QByteArray legacy;
        for (const auto& line : ExportTest::Bytes(index).split('\n'))
            if (!line.startsWith("HASH ") && !line.isEmpty()) legacy += line + '\n';
        Put(index, legacy);
    }
    if (mode == "legacy-pending")
    {
        {
            FATStorage guest(path.toStdString(), Capacity, false);
            auto data = Payload(12291);
            Check(guest.InjectFile("KEEP.BIN", reinterpret_cast<u8*>(data.data()), data.size()), "legacy pending guest edit");
        }
        const auto imageBefore = ExportTest::Bytes(path), indexBefore = ExportTest::Bytes(index);
        {
            FATStorage retry(path.toStdString(), Capacity, false, source.toStdString());
            Check(!retry.IsValid(), "ambiguous legacy pending data refuses");
        }
        Check(ExportTest::Bytes(path) == imageBefore && ExportTest::Bytes(index) == indexBefore && ExportTest::Bytes(host) == expected,
            "legacy refusal preserves exact image host and index");
        FATStorage recovery(path.toStdString(), Capacity, true);
        QByteArray actual(12291, '\0');
        Check(recovery.ReadFile("KEEP.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size() && actual == Payload(12291),
            "refused guest data remains accessible without folder sync");
    }
    else
    {
        if (mode == "no-index-adoption") Check(QFile::remove(index), "remove generated sidecar for explicit no-index adoption");
        if (mode == "host-only")
        {
            const auto time = std::filesystem::last_write_time(PathFromUTF8(host.toStdString()));
            expected[23] ^= 0x6A;
            Put(host, expected);
            std::filesystem::last_write_time(PathFromUTF8(host.toStdString()), time);
        }
        {
            FATStorage storage(path.toStdString(), Capacity, false, source.toStdString());
            Check(storage.IsValid(), "ordinary legacy/no-index/host-only adoption");
            QByteArray actual(expected.size(), '\0');
            Check(storage.ReadFile("KEEP.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size() && actual == expected,
                "ordinary host import stays exact");
            if (mode == "no-index-adoption")
                Check(storage.ReadFile("OLDONLY.BIN", 0, 1, reinterpret_cast<u8*>(actual.data())) == 0, "no-index source adoption retains existing semantics");
        }
        Check(ExportTest::Bytes(host) == expected && ExportTest::Bytes(index).contains("HASH "), "ordinary adoption retains host bytes and upgrades index");
    }
    std::printf("PASS: sync-%s\n", mode.c_str());
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Require(argc == 2, "one scenario required");
    const std::string scenario = argv[1];
    QTemporaryDir temp;
    Require(temp.isValid(), "temporary directory");
    const QString path = temp.filePath("card.img");
    const QString index = path + ".idx";
    const QString source = temp.filePath("source");
    Require(QDir().mkdir(source), "source directory");
    const QString hostFile = source + "/KEEP.BIN";
    QByteArray payload(8193, '\0');
    for (qsizetype i = 0; i < payload.size(); ++i) payload[i] = char((i * 17 + i / 512 + 9) & 255);
    WriteBytes(hostFile, payload);

    if (scenario.starts_with("sync-"))
    {
        TestReconciliation(scenario.substr(5), path, source);
        return 0;
    }

    if (scenario.starts_with("export-"))
    {
        TestExport(scenario, path, source);
        return 0;
    }

    if (scenario == "empty" || scenario == "format-write-error")
    {
        if (scenario == "empty") WriteBytes(path, {}); // Existing empty selection remains supported.
        else
        {
            WriteBytes(index, "existing sidecar must survive a failed format\n");
            failure = Failure::Write;
        }
        {
            FATStorage storage(path.toStdString(), Capacity, false, source.toStdString());
            if (scenario == "empty")
            {
                QByteArray actual(payload.size(), '\0');
                Require(storage.ReadFile("KEEP.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size()
                    && actual == payload, "empty image format and import");
            }
            else
            {
                std::printf("capacity_after_failed_format=%llu index_writes=%u\n",
                    static_cast<unsigned long long>(storage.GetSectorCount()), indexWrites);
                Require(!storage.IsValid() && storage.GetSectorCount() == 0 && openImages == 0, "failed format left an active image");
                Require(indexWrites == 0 && Bytes(index) == "existing sidecar must survive a failed format\n", "failed format replaced index");
            }
        }
        failure = Failure::None;
    }
    else
    {
        // Production constructor formats and imports a multi-cluster file.
        {
            FATStorage storage(path.toStdString(), Capacity, false, source.toStdString());
            Require(storage.GetSectorCount() == Capacity / 512, "new image capacity");
            QByteArray actual(payload.size(), '\0');
            Require(storage.ReadFile("KEEP.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size()
                && actual == payload, "new image format and import");
        }
        Require(openImages == 0, "seed handle closed");
        if (scenario == "normal")
        {
            FATStorage original(path.toStdString(), 0, false, source.toStdString());
            FATStorage moved(std::move(original));
            Require(moved.IsValid() && !original.IsValid() && original.GetSectorCount() == 0
                && moved.GetSectorCount() == Capacity / 512, "sparse indexed image capacity after move");
            QByteArray actual(payload.size(), '\0');
            Require(moved.ReadFile("KEEP.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size()
                && actual == payload, "existing image payload after move");
        }
        else
        {
            if (scenario == "mount-read-error") failure = Failure::Read;
            else if (scenario == "mount-seek-error") failure = Failure::Seek;
            else if (scenario == "length-error")
            {
                // No index capacity fallback: a host size error must not make
                // an existing nonempty image eligible for formatting.
                WriteBytes(index, "unrecognized index, retain verbatim\n");
                failure = Failure::LengthAndRead;
            }
            else if (scenario == "malformed") WriteBytes(path, QByteArray(4096, char(0xA5)));
            else Require(false, "unknown scenario");

            const QByteArray before = Bytes(path), indexBefore = Bytes(index);
            imageWrites = indexWrites = 0;
            {
                FATStorage storage(path.toStdString(), 0, false, source.toStdString());
                std::printf("capacity_after_failed_mount=%llu image_writes=%u index_writes=%u injected=%u\n",
                    static_cast<unsigned long long>(storage.GetSectorCount()), imageWrites, indexWrites, injectedFailures);
                Require(imageWrites == 0 && indexWrites == 0, "mount failure triggered formatting or index replacement");
                Require(!storage.IsValid() && storage.GetSectorCount() == 0 && openImages == 0, "failed mount left an active image");
                std::array<u8, 512> sector{};
                Require(storage.ReadSectors(0, 1, sector.data()) == 0 && storage.WriteSectors(0, 1, sector.data()) == 0,
                    "failed image accepted guest I/O");
                if (scenario != "malformed") Require(injectedFailures == 1, "host failure was not exercised");
            }
            Require(imageWrites == 0 && indexWrites == 0 && Bytes(path) == before && Bytes(index) == indexBefore
                && Bytes(hostFile) == payload, "failed mount/destructor changed persistent data");
            failure = Failure::None;
            if (scenario != "malformed")
            {
                // Retry the same preserved image after the one-shot error.
                FATStorage retry(path.toStdString(), Capacity, true);
                QByteArray actual(payload.size(), '\0');
                Require(retry.ReadFile("KEEP.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size()
                    && actual == payload, "same-image retry lost guest data");
            }
        }
    }
    Require(openImages == 0 && Bytes(hostFile) == payload, "lifetime or host file preservation");
    std::printf("PASS: %s\n", scenario.c_str());
}
