// SPDX-License-Identifier: GPL-3.0-or-later
// Generated FAT images, full FATStorage/FatFs and current Qt I/O methods.
// QFile failures are injected only at the host file boundary, never in FatFs.
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
    QFile* file = image ? new ImageFile(name) : new QFile(name);
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
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args);
}
#include "DSiSDFATQtMethods.inc"
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
