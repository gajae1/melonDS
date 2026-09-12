// SPDX-License-Identifier: GPL-3.0-or-later
#include "ROMPreparation.h"
#include "ROMFileTypes.h"
#include "ArchiveUtil.h"
#include "Platform.h"
#include <QFileInfo>
#include <QDir>
#include <QPointer>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <new>
#include <utility>
#include <zstd.h>
using namespace melonDS;
using namespace melonDS::Platform;
using std::string;
using std::unique_ptr;
using std::make_unique;

ROMPreparation::Result ROMPreparation::Prepare(const Request& request, std::stop_token stop)
{
    Result result;
    result.Source = request.Source;
    result.Stop = stop;
    try
    {
        const auto& files = request.Source;
        if (stop.stop_requested()) return result;
        if (files.isEmpty() || files.size() > 2) { result.ReadFailed = true; return result; }
        const QString path = files.first();
        if (!QFileInfo::exists(path))
        {
            result.Error = files.size() == 2 ? "This archive does not exist." : "This ROM file does not exist.";
            result.Warning = true;
            return result;
        }
#ifdef ARCHIVE_SUPPORT_ENABLED
        if (request.Inspect && (files.size() == 2 || SupportedArchiveByExtension(path) ||
            SupportedArchiveByMimetype(QMimeDatabase().mimeTypeForFile(path))))
        {
            auto members = Archive::ListArchive(path, stop);
            if (stop.stop_requested()) return result;
            if (files.size() == 2)
            {
                if (members.size() < 2 || members.first() != "OK" || !members.mid(1).contains(files.last()))
                {
                    result.Error = "This archive does not contain the desired file.";
                    result.Warning = true;
                    return result;
                }
            }
            else
            {
                if (members.isEmpty() || members.first() != "OK")
                    result.Error = "This archive could not be read. It may be corrupt or you don't have the permissions.";
                else if (members.size() == 1)
                {
                    result.Error = "This archive is empty.";
                    result.Warning = true;
                }
                else
                {
                    members.removeFirst();
                    for (const auto& name : members)
                    {
                        if (stop.stop_requested()) return result;
                        const auto mime = QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension);
                        if (NdsRomByExtension(name) || GbaRomByExtension(name) ||
                            NdsRomByMimetype(mime) || GbaRomByMimetype(mime))
                            result.Members.append(name);
                    }
                    if (result.Members.isEmpty())
                    {
                        result.Error = "This archive does not contain any supported ROMs.";
                        result.Warning = true;
                    }
                }
                return result;
            }
        }
#endif
        auto data = std::make_shared<Data>();
        data->Source = files;
        data->Stop = stop;
        const auto& name = files.last();
        const auto mime = QMimeDatabase().mimeTypeForFile(name,
            files.size() == 2 ? QMimeDatabase::MatchExtension : QMimeDatabase::MatchDefault);
        if (NdsRomByExtension(name) || ZstdNdsRomByExtension(name) || NdsRomByMimetype(mime)) data->Type = Kind::DS;
        else if (GbaRomByExtension(name) || ZstdGbaRomByExtension(name) || GbaRomByMimetype(mime)) data->Type = Kind::GBA;
        result.Type = data->Type;
        if (!Read(files, data->Bytes, data->Length, data->BasePath, data->Name, stop))
            result.ReadFailed = !stop.stop_requested();
        else if (!stop.stop_requested()) result.ROM = std::move(data);
    }
    catch (const std::bad_alloc&)
    {
        result.ROM.reset();
        result.Members.clear();
        result.ReadFailed = !stop.stop_requested();
    }
    return result;
}

ROMPreparation::Controller::Controller(QObject* parent) : QObject(parent) {}

ROMPreparation::Controller::~Controller()
{
    cancel();
    if (worker)
    {
        disconnect(worker.get(), nullptr, this, nullptr);
        // No forced termination or detached work. Normal window close defers
        // destruction until idle; direct owner destruction still joins safely.
        worker->wait();
    }
}

void ROMPreparation::Controller::cancel()
{
    stopSource.request_stop();
    ++generation;
    pending.reset();
}

void ROMPreparation::Controller::start(Request request)
{
    cancel();
    stopSource = std::stop_source{};
    pending = std::move(request);
    if (!worker) launch();
}

void ROMPreparation::Controller::launch()
{
    const auto request = std::move(*pending);
    pending.reset();
    const auto version = generation;
    const auto stop = stopSource.get_token();
    auto result = std::make_shared<Result>();
    worker.reset(QThread::create([request, stop, result] { *result = Prepare(request, stop); }));
    worker->setParent(this);
    connect(worker.get(), &QThread::finished, this, [this, version, stop, result] {
        worker->wait();
        worker.reset();
        const QPointer<Controller> alive(this);
        if (version == generation && !stop.stop_requested()) emit ready(*result);
        if (!alive) return;
        // A ready handler may itself start the selected archive member.
        if (!worker && pending) launch();
        if (!worker) emit idle();
    });
    worker->start();
}

u32 ROMPreparation::Decompress(const u8* inContent, const u32 inSize, unique_ptr<u8[]>& outContent, std::stop_token stop)
{
    if (stop.stop_requested()) return 0;
    const u64 realSize = ZSTD_getFrameContentSize(inContent, inSize);
    const u32 maxSize = 0x40000000;

    if (realSize == ZSTD_CONTENTSIZE_ERROR || (realSize > maxSize && realSize != ZSTD_CONTENTSIZE_UNKNOWN))
    {
        return 0;
    }

    // A frame's declared size does not include any following frames.
    if (!stop.stop_possible() && realSize != ZSTD_CONTENTSIZE_UNKNOWN &&
        ZSTD_findFrameCompressedSize(inContent, inSize) == inSize)
    {
        if (realSize == 0) return 0;

        try
        {
            auto newOutContent = make_unique<u8[]>(realSize);
            size_t decompressed = ZSTD_decompress(newOutContent.get(), realSize, inContent, inSize);

            if (ZSTD_isError(decompressed) || decompressed != realSize) return 0;

            outContent = std::move(newOutContent);
            return static_cast<u32>(decompressed);
        }
        catch (const std::bad_alloc&)
        {
            return 0;
        }
    }

    unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)> dStream(ZSTD_createDStream(), ZSTD_freeDStream);
    if (!dStream || ZSTD_isError(ZSTD_initDStream(dStream.get()))) return 0;

    const u32 startSize = 1024 * 1024 * 16;
    unique_ptr<void, decltype(&free)> partialOutContent(malloc(startSize), free);
    if (!partialOutContent) return 0;

    ZSTD_inBuffer inBuf = {inContent, inSize, 0};
    ZSTD_outBuffer outBuf = {partialOutContent.get(), startSize, 0};

    for (;;)
    {
        if (stop.stop_requested()) return 0;
        if (outBuf.pos == outBuf.size && outBuf.size < maxSize)
        {
            const size_t newSize = outBuf.size * 2;
            void* grown = realloc(partialOutContent.get(), newSize);
            if (!grown) return 0;
            partialOutContent.release();
            partialOutContent.reset(grown);
            outBuf.dst = grown;
            outBuf.size = newSize;
        }

        // At the cap, allow checksums/empty frames to finish, but reject another byte.
        u8 overflowByte;
        ZSTD_outBuffer overflow = {&overflowByte, 1, 0};
        ZSTD_outBuffer* output = outBuf.pos == maxSize ? &overflow : &outBuf;
        const size_t previousInput = inBuf.pos;
        const size_t previousOutput = outBuf.pos;
        // Bound each decoder call in both compressed input and output bytes.
        ZSTD_inBuffer inputChunk{inBuf.src, std::min(inBuf.size, inBuf.pos + ChunkSize), inBuf.pos};
        ZSTD_outBuffer outputChunk{output->dst, std::min(output->size, output->pos + ChunkSize), output->pos};
        size_t result = ZSTD_decompressStream(dStream.get(), &outputChunk, &inputChunk);
        inBuf.pos = inputChunk.pos;
        output->pos = outputChunk.pos;

        if (ZSTD_isError(result) || overflow.pos != 0) return 0;
        if (result == 0 && inBuf.pos == inBuf.size) break;

        // Exhausted input with an unfinished frame must not publish partial output.
        if (inBuf.pos == previousInput && outBuf.pos == previousOutput) return 0;
    }

    if (outBuf.pos == 0) return 0;

    try
    {
        auto newOutContent = make_unique<u8[]>(outBuf.pos);
        for (size_t offset = 0; offset < outBuf.pos; offset += ChunkSize)
        {
            if (stop.stop_requested()) return 0;
            memcpy(newOutContent.get() + offset, static_cast<const u8*>(outBuf.dst) + offset,
                   std::min(ChunkSize, outBuf.pos - offset));
        }
        if (stop.stop_requested()) return 0;

        // inContent can belong to outContent, so replace it only after all decoding.
        outContent = std::move(newOutContent);
        return static_cast<u32>(outBuf.pos);
    }
    catch (const std::bad_alloc&)
    {
        return 0;
    }
}

bool ROMPreparation::Read(const QStringList& filepath, std::unique_ptr<u8[]>& filedata, u32& filelen, string& basepath, string& romname, std::stop_token stop) noexcept
{
    try
    {
        if (stop.stop_requested() || filepath.empty()) return false;
        string filename = filepath.at(0).toStdString();
        string membername = filename;
        unique_ptr<u8[]> data;
        u32 length = 0;

        if (filepath.count() == 1)
        {
            const unique_ptr<FileHandle, decltype(&Platform::CloseFile)> file(
                Platform::OpenFile(filename, FileMode::Read), Platform::CloseFile);
            if (!file) return false;
            const u64 size = Platform::FileLength(file.get());
            if (stop.stop_requested() || !size || size > 0x40000000) return false;

            data = std::make_unique_for_overwrite<u8[]>(static_cast<u32>(size));
            for (u64 offset = 0; offset < size;)
            {
                if (stop.stop_requested()) return false;
                const u64 count = std::min<u64>(ChunkSize, size - offset);
                if (Platform::FileRead(data.get() + offset, 1, count, file.get()) != count) return false;
                offset += count;
            }
            if (stop.stop_requested()) return false;
            length = static_cast<u32>(size);

            if (filename.length() > 4 && filename.ends_with(".zst"))
            {
                length = Decompress(data.get(), length, data, stop);
                if (!length) return false;
                filename.resize(filename.length() - 4);
                membername = filename;
            }
        }
#ifdef ARCHIVE_SUPPORT_ENABLED
        else if (filepath.count() == 2)
        {
            const s32 read = Archive::ExtractFileFromArchive(filepath.at(0), filepath.at(1), data, &length, stop);
            if (read < 0 || !data || !length || length > 0x40000000 || static_cast<u32>(read) != length)
                return false;
            membername = filepath.at(1).toStdString();
        }
#endif
        else return false;

        if (stop.stop_requested()) return false;
        const auto lastSep = [](const string& path) -> int {
            return static_cast<int>(path.find_last_of("/\\"));
        };
        const int separator = lastSep(filename);
        string directory = separator < 0 ? "" : filename.substr(0, separator);
        string name = membername.substr(lastSep(membername) + 1);
        // Commit bytes and names together, after every read/decode/allocation.
        filedata = std::move(data);
        filelen = length;
        basepath = std::move(directory);
        romname = std::move(name);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
}
