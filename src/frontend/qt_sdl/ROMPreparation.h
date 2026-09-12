// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ROMPREPARATION_H
#define ROMPREPARATION_H

#include <QObject>
#include <QStringList>
#include <QThread>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include "types.h"

namespace ROMPreparation
{
constexpr size_t ChunkSize = 64 * 1024;
enum class Kind { Unknown, DS, GBA };

struct Data
{
    QStringList Source;
    std::unique_ptr<melonDS::u8[]> Bytes;
    melonDS::u32 Length = 0;
    std::string BasePath, Name;
    std::stop_token Stop;
    Kind Type = Kind::Unknown;
};
struct Request
{
    QStringList Source;
    // Inspect detects archives and lists their members; Read uses an explicit
    // source/member pair selected by the UI, without another listing pass.
    bool Inspect = true;
};
struct Result
{
    QStringList Source, Members;
    std::shared_ptr<Data> ROM;
    QString Error;
    bool Warning = false;
    bool ReadFailed = false;
    Kind Type = Kind::Unknown;
    std::stop_token Stop;
};

melonDS::u32 Decompress(const melonDS::u8* inContent, melonDS::u32 inSize,
                       std::unique_ptr<melonDS::u8[]>& outContent, std::stop_token stop = {});
bool Read(const QStringList& filepath, std::unique_ptr<melonDS::u8[]>& filedata,
          melonDS::u32& filelen, std::string& basepath, std::string& romname,
          std::stop_token stop = {}) noexcept;
Result Prepare(const Request& request, std::stop_token stop);

// One owned worker and at most one pending (latest) request per window. Neither
// the worker nor its result retains a window, emulator, configuration or save.
class Controller : public QObject
{
    Q_OBJECT
public:
    explicit Controller(QObject* parent = nullptr);
    ~Controller() override;
    void start(Request request);
    void cancel();
    bool busy() const { return worker != nullptr; }

signals:
    void ready(const ROMPreparation::Result& result);
    void idle();

private:
    void launch();
    std::unique_ptr<QThread> worker;
    std::optional<Request> pending;
    std::stop_source stopSource;
    quint64 generation = 0;
};
}
#endif
