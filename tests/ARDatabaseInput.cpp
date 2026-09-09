// SPDX-License-Identifier: GPL-3.0-or-later
// Generated databases only. The parser is compiled from ARDatabaseDAT.cpp;
// file operations below are extracted from the current Qt Platform.cpp.
// QFile storage is real. Only read/seek/open faults are injected, including the
// production signed-read-error to u64 conversion. No frontend UI is simulated.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include "ARDatabaseDAT.h"
#include "Platform.h"

using namespace melonDS;
using Bytes = std::vector<u8>;
static int failures = 0;
static void Check(bool ok, const char* message)
{
    if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
}
static void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

enum class ReadFault { None, Short, Error, Zero };
static ReadFault readFault = ReadFault::None;
static qint64 readFaultAt = -1, seekFaultAt = -1;
static bool openFault = false;
static int faultHits = 0, liveFiles = 0;
static QString allowedPath;
static QFile* activeFile = nullptr;
struct NoReadProgress {};

class DatabaseFile : public QFile
{
public:
    explicit DatabaseFile(const QString& path) : QFile(path) { ++liveFiles; activeFile = this; }
    ~DatabaseFile() override { --liveFiles; activeFile = nullptr; }
    qint64 read(char* data, qint64 length)
    {
        if (pos() == readFaultAt && readFault != ReadFault::None)
        {
            ++faultHits;
            if (faultHits > 8) throw NoReadProgress{};
            if (readFault == ReadFault::Error) return -1;
            if (readFault == ReadFault::Zero)
            {
                // Bound the baseline's infinite loop, without claiming a
                // fixture timeout is a successful rejection by the parser.
                return 0;
            }
            --length;
        }
        return QFile::read(data, length);
    }
    bool seek(qint64 position) override
    {
        if (position == seekFaultAt) { ++faultHits; return false; }
        return QFile::seek(position);
    }
};

namespace melonDS::Platform
{
FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    Require(QString::fromStdString(path) == allowedPath && mode == FileMode::Read,
            "Fixture attempted access outside its generated database");
    if (openFault) { ++faultHits; return nullptr; }
    auto file = std::make_unique<DatabaseFile>(allowedPath);
    if (!file->open(QIODevice::ReadOnly)) return nullptr;
    return reinterpret_cast<FileHandle*>(file.release());
}
void Log(LogLevel, const char*, ...) {}
#define QFile DatabaseFile
#include "arCloseFile.inc"
#include "arIsEndOfFile.inc"
#include "arFileSeek.inc"
#include "arFilePosition.inc"
#include "arFileRead.inc"
#include "arFileLength.inc"
#undef QFile
}

static void PutWord(Bytes& bytes, size_t offset, u32 word)
{
    Require(offset + 4 <= bytes.size(), "Fixture word offset out of bounds");
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<u8>(word >> (8 * i));
}
static size_t Word(Bytes& bytes, u32 word)
{
    const size_t offset = bytes.size();
    bytes.resize(offset + 4);
    PutWord(bytes, offset, word);
    return offset;
}
static void String(Bytes& bytes, const std::string& text)
{
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.push_back(0);
}
static void Align(Bytes& bytes) { while (bytes.size() & 3) bytes.push_back(0); }

struct Game
{
    Bytes bytes;
    size_t flags = 0, firstItem = 0, codeLength = 0, codeWords = 0;
    size_t category = 0, lastCode = 0, titleEnd = 0;
};
static size_t Code(Game& game, const std::string& name, bool enabled)
{
    const size_t start = Word(game.bytes, 0);
    String(game.bytes, name);
    String(game.bytes, "note");
    Align(game.bytes);
    game.codeLength = Word(game.bytes, 2);
    game.codeWords = Word(game.bytes, 0x12345678);
    Word(game.bytes, 0x9ABCDEF0);
    PutWord(game.bytes, start, static_cast<u32>((game.bytes.size() - start - 4) / 4) |
                              (enabled ? (1u << 24) : 0));
    return start;
}
static Game MakeGame(bool category = false, const std::string& title = "Title")
{
    Game game;
    String(game.bytes, title);
    game.titleEnd = game.bytes.size();
    Align(game.bytes);
    game.flags = Word(game.bytes, 0xF0000000 | (category ? 4 : 1));
    for (int i = 0; i < 8; ++i) Word(game.bytes, 0);
    game.firstItem = Code(game, "Root", false);
    if (category)
    {
        game.category = Word(game.bytes, 0x11000002);
        String(game.bytes, "Group");
        String(game.bytes, "Only one");
        Align(game.bytes);
        Code(game, "First", true);
        game.lastCode = Code(game, "Second", true);
    }
    return game;
}

constexpr u32 GameCode = 0x31545354, OtherGameCode = 0x32545354;
constexpr u32 Checksum = 0x10203040;
struct Record { u32 gameCode; u32 checksum; Game game; };
struct Database { Bytes bytes; std::vector<size_t> offsets; };
static Database MakeDatabase(const std::vector<Record>& records)
{
    Database db;
    db.bytes.resize(0x100 + 16 * (records.size() + 1), 0);
    std::memcpy(db.bytes.data(), "R4 CheatCode\x00\x01\x00\x00", 16);
    std::memcpy(db.bytes.data() + 16, "Synthetic database", 18);
    for (size_t i = 0; i < records.size(); ++i)
    {
        Align(db.bytes);
        db.offsets.push_back(db.bytes.size());
        PutWord(db.bytes, 0x100 + i * 16, records[i].gameCode);
        PutWord(db.bytes, 0x104 + i * 16, records[i].checksum);
        PutWord(db.bytes, 0x108 + i * 16, static_cast<u32>(db.bytes.size()));
        db.bytes.insert(db.bytes.end(), records[i].game.bytes.begin(), records[i].game.bytes.end());
    }
    return db;
}
static void Write(const Bytes& bytes)
{
    QFile file(allowedPath);
    Require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == static_cast<qint64>(bytes.size()),
            "Fixture database write failed");
}
static void RejectBody(const Database& fixture)
{
    Write(fixture.bytes);
    ARDatabaseDAT db(allowedPath.toStdString());
    Require(!db.Error && db.FindGameCode(GameCode), "Fixture index unexpectedly failed before body parsing");
    const auto entries = db.GetEntriesByGameCode(GameCode);
    Check(entries.empty(), "Malformed game exposed a partial or incomplete cheat entry");
    Check(db.Error, "Game body failure was not propagated through Error");
    Check(liveFiles == 0, "Body parsing leaked its file handle");
}

static void Controls()
{
    const std::string title = std::string(260, 'A') + "\xED\x95\x9C\xEA\xB8\x80";
    const auto fixture = MakeDatabase({{GameCode, Checksum, MakeGame(true, title)},
                                      {GameCode, 0x55667788, MakeGame()},
                                      {OtherGameCode, Checksum, MakeGame()}});
    Write(fixture.bytes);
    ARDatabaseDAT db(allowedPath.toStdString());
    Check(!db.Error && db.GetDBName() == "Synthetic database", "Normal database header changed");
    Check(db.FindGameCode(GameCode) && !db.FindGameCode(0x99999999), "Game code lookup changed");
    Check(db.GetEntriesByGameCode(0x99999999).empty(), "Unknown game unexpectedly returned cheats");
    const auto entries = db.GetEntriesByGameCode(GameCode);
    Require(entries.size() == 2, "Normal entries could not be read");
    Check(entries[0].GameCode == GameCode && entries[0].Checksum == Checksum && entries[0].Name == title &&
          entries[1].Checksum == 0x55667788, "Game/checksum/name or index order changed");
    Require(entries[0].RootCat.Children.size() == 2, "Normal root/category structure changed");
    const auto& root = std::get<ARCode>(entries[0].RootCat.Children.front());
    const auto& cat = std::get<ARCodeCat>(entries[0].RootCat.Children.back());
    Check(!root.Enabled && root.Name == "Root" && root.Description == "note" &&
          root.Code == std::vector<u32>{0x12345678, 0x9ABCDEF0}, "Normal code metadata/words changed");
    Require(cat.Children.size() == 2, "Normal category lost a child");
    Check(cat.OnlyOneCodeEnabled && std::get<ARCode>(cat.Children.front()).Enabled &&
          !std::get<ARCode>(cat.Children.back()).Enabled, "OnlyOne no longer keeps the first enabled cheat");
    Check(!db.Error, "Normal database retrieval reported an error");
}

static void HeaderIndex()
{
    const auto normal = MakeDatabase({{GameCode, Checksum, MakeGame()}, {OtherGameCode, Checksum, MakeGame()}});
    std::vector<Bytes> malformed;
    for (size_t length : {size_t{0}, size_t{15}, size_t{32}, size_t{0x108}})
        malformed.emplace_back(normal.bytes.begin(), normal.bytes.begin() + length);
    auto bytes = normal.bytes;
    bytes[0] ^= 1;
    malformed.push_back(bytes);
    bytes = normal.bytes;
    PutWord(bytes, 0x10C, 1); // Unsupported high 32 bits of the data offset.
    malformed.push_back(bytes);
    bytes = normal.bytes;
    PutWord(bytes, 0x108, 0x100); // Payload overlaps its own index.
    malformed.push_back(bytes);
    bytes = normal.bytes;
    PutWord(bytes, 0x124, 1); // A terminator must be entirely zero.
    malformed.push_back(bytes);
    bytes = normal.bytes;
    PutWord(bytes, 0x118, 0xFFFFFFFC); // Failure after one otherwise valid index row.
    malformed.push_back(bytes);
    for (size_t i = 0; i < malformed.size(); ++i)
    {
        Write(malformed[i]);
        ARDatabaseDAT db(allowedPath.toStdString());
        if (!db.Error) std::fprintf(stderr, "Header/index trigger %zu\n", i);
        Check(db.Error, "Malformed header/index was accepted");
        Check(!db.FindGameCode(GameCode), "Failed index load retained a partially usable entry list");
    }
    Write(MakeDatabase({}).bytes);
    ARDatabaseDAT empty(allowedPath.toStdString());
    Check(!empty.Error && !empty.FindGameCode(GameCode), "Valid empty database was rejected");
}

static void Strings()
{
    for (bool title : {true, false})
    {
        Game game = MakeGame();
        if (title) game.bytes.assign(24, 'A');
        else std::fill(game.bytes.begin() + game.firstItem + 4, game.bytes.end(), 'A');
        RejectBody(MakeDatabase({{GameCode, Checksum, game}}));
    }
}

static void Categories()
{
    for (u32 count : {0u, 3u, 0x10000u})
    {
        Game game = MakeGame(true);
        PutWord(game.bytes, game.category, 0x11000000 | count);
        RejectBody(MakeDatabase({{GameCode, Checksum, game}}));
    }
    Game nested = MakeGame(true);
    PutWord(nested.bytes, nested.lastCode, 0x11000001);
    RejectBody(MakeDatabase({{GameCode, Checksum, nested}}));
}

static void Codes()
{
    Game game = MakeGame(true);
    game.bytes.resize(game.bytes.size() - 4); // Last word after already parsed good cheats.
    RejectBody(MakeDatabase({{GameCode, Checksum, game}}));
    game = MakeGame();
    PutWord(game.bytes, game.codeLength, 1);
    RejectBody(MakeDatabase({{GameCode, Checksum, game}}));
    game = MakeGame();
    PutWord(game.bytes, game.firstItem, 0x00FFFFFF);
    RejectBody(MakeDatabase({{GameCode, Checksum, game}}));
    game = MakeGame();
    PutWord(game.bytes, game.flags, 0x01000001); // Count uses all low 28 bits.
    RejectBody(MakeDatabase({{GameCode, Checksum, game}}));
    game = MakeGame();
    game.bytes.resize(game.flags + 8); // Truncated master/header words.
    RejectBody(MakeDatabase({{GameCode, Checksum, game}}));
}

static void EntrySpan()
{
    Game unterminated;
    unterminated.bytes.assign(16, 'A');
    const auto fixture = MakeDatabase({{GameCode, Checksum, unterminated},
                                      {GameCode, 0x55667788, MakeGame()}});
    Write(fixture.bytes);
    ARDatabaseDAT db(allowedPath.toStdString());
    const auto entries = db.GetEntriesByGameCode(GameCode);
    Check(entries.size() == 1 && entries[0].Checksum == 0x55667788 && entries[0].Name == "Title",
          "Malformed entry consumed the next game's title/header/code span");
    Check(db.Error, "Cross-entry string failure was not reported");
}

static void Partial()
{
    Game broken = MakeGame(true);
    PutWord(broken.bytes, broken.codeLength, 1);
    Write(MakeDatabase({{GameCode, Checksum, MakeGame()}, {GameCode, 0xBAD, broken},
                        {GameCode, 0x55667788, MakeGame()}, {OtherGameCode, Checksum, MakeGame()}}).bytes);
    ARDatabaseDAT db(allowedPath.toStdString());
    const auto entries = db.GetEntriesByGameCode(GameCode);
    Check(entries.size() == 2 && entries[0].Checksum == Checksum && entries[1].Checksum == 0x55667788,
          "Malformed version was returned or an independent valid version was discarded");
    Check(db.Error, "Skipped entry was not signaled to the import caller");
    Check(db.GetEntriesByGameCode(OtherGameCode).size() == 1 && db.Error,
          "Error must remain sticky while independent valid game lookup still works");
}

static void ReadErrors()
{
    const Game game = MakeGame();
    const auto fixture = MakeDatabase({{GameCode, Checksum, game}});
    Write(fixture.bytes);
    for (ReadFault fault : {ReadFault::Short, ReadFault::Error})
    {
        readFault = fault;
        readFaultAt = 0x100;
        faultHits = 0;
        ARDatabaseDAT index(allowedPath.toStdString());
        Require(faultHits > 0, "Index read fault was not exercised");
        Check(index.Error && !index.FindGameCode(GameCode), "Failed index read was accepted");
        readFault = ReadFault::None;
        ARDatabaseDAT body(allowedPath.toStdString());
        readFault = fault;
        readFaultAt = fixture.offsets[0] + game.codeWords;
        faultHits = 0;
        Check(body.GetEntriesByGameCode(GameCode).empty() && body.Error, "Failed code read returned incomplete words");
        Require(faultHits > 0, "Code read fault was not exercised");
    }
    readFault = ReadFault::None;
    ARDatabaseDAT reopen(allowedPath.toStdString());
    openFault = true;
    Check(reopen.GetEntriesByGameCode(GameCode).empty() && reopen.Error, "Reopen failure was not propagated");
    openFault = false;
}

static void SeekErrors()
{
    const Game game = MakeGame();
    const auto fixture = MakeDatabase({{GameCode, Checksum, game}});
    Write(fixture.bytes);
    for (qint64 offset : {qint64{0x100}, qint64(fixture.offsets[0]),
                         qint64(fixture.offsets[0] + game.titleEnd), qint64(fixture.offsets[0] + game.flags)})
    {
        faultHits = 0;
        seekFaultAt = offset;
        ARDatabaseDAT db(allowedPath.toStdString());
        if (offset == 0x100)
            Check(db.Error && !db.FindGameCode(GameCode), "Failed index seek was accepted");
        else
            Check(db.GetEntriesByGameCode(GameCode).empty() && db.Error, "Failed game/string/alignment seek was accepted");
        Require(faultHits > 0, "Seek fault was not exercised");
    }
    seekFaultAt = -1;
}

static void NoProgress()
{
    const auto fixture = MakeDatabase({{GameCode, Checksum, MakeGame()}});
    Write(fixture.bytes);
    ARDatabaseDAT db(allowedPath.toStdString());
    readFault = ReadFault::Zero;
    readFaultAt = fixture.offsets[0];
    Check(db.GetEntriesByGameCode(GameCode).empty() && db.Error, "Non-progressing string read was accepted");
}

static void Parents()
{
    Write(MakeDatabase({{GameCode, Checksum, MakeGame(true)},
                        {GameCode, 0x55667788, MakeGame(true)}}).bytes);
    ARDatabaseDAT db(allowedPath.toStdString());
    auto entries = db.GetEntriesByGameCode(GameCode);
    Require(entries.size() == 2, "Parent fixture failed to load");
    for (const auto& entry : entries)
    {
        Require(entry.RootCat.Children.size() == 2, "Parent fixture category is missing");
        const auto& category = std::get<ARCodeCat>(entry.RootCat.Children.back());
        // Category::Parent is initialized by the baseline; compare it without
        // dereferencing its stale target. Baseline Code::Parent is uninitialized.
        Check(category.Parent == &entry.RootCat, "Returned category Parent still points at a temporary entry");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2) return 2;
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    allowedPath = directory.filePath(QStringLiteral("synthetic-\uD55C\uAE00.dat"));
    const std::string scenario = argv[1];
    try
    {
        if (scenario == "controls") Controls();
        else if (scenario == "header-index") HeaderIndex();
        else if (scenario == "strings") Strings();
        else if (scenario == "category") Categories();
        else if (scenario == "codes") Codes();
        else if (scenario == "entry-span") EntrySpan();
        else if (scenario == "partial") Partial();
        else if (scenario == "io-read") ReadErrors();
        else if (scenario == "io-seek") SeekErrors();
        else if (scenario == "no-progress") NoProgress();
        else if (scenario == "parents") Parents();
        else return 2;
    }
    catch (const NoReadProgress&)
    {
        Check(false, "Parser repeatedly read the same offset without progress");
        delete activeFile; // Release the file after the fixture stops the baseline loop.
    }
    catch (const std::exception& error)
    {
        delete activeFile;
        std::fprintf(stderr, "Fixture error (not a valid RED): %s\n", error.what());
        return 2;
    }
    Check(liveFiles == 0, "Parser left a file handle open");
    std::printf("AR database %s: %d failures, injected faults=%d\n", scenario.c_str(), failures, faultHits);
    return failures ? 1 : 0;
}
