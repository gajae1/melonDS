// SPDX-License-Identifier: GPL-3.0-or-later
// Production ARCodeFile parser/serializer, real generated QFile storage, and
// current Platform.cpp definitions. Only host open/read/write/flush/close
// failures are injected. No personal cheat database or frontend UI is used.
// ARCODE_SERIALIZE_TESTS enables tests of the new API after it exists; baseline
// RED uses only the pre-existing Load/Save API, not a missing-symbol failure.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include "ARCodeFile.h"
#include "Platform.h"

using namespace melonDS;
static int Failures = 0;
static void Check(bool ok, const char* message)
{
    if (!ok) { ++Failures; std::fprintf(stderr, "%s\n", message); }
}
static void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

enum class Fault { None, Open, ReadZero, ReadError, ReadStalled, WriteShort, WriteError, Flush, Close };
static Fault fault = Fault::None;
static int faultHits = 0, writeOpens = 0, closeCalls = 0;
static qint64 readFaultAt = 0;
static QString allowedPath;
struct NoReadProgress {};
class CheatFile;
static std::vector<CheatFile*> openFiles;

class CheatFile : public QFile
{
public:
    bool Writing = false;
    explicit CheatFile(const QString& path) : QFile(path) { openFiles.push_back(this); }
    ~CheatFile() override { std::erase(openFiles, this); }
    qint64 readLine(char* data, qint64 count)
    {
        if (pos() >= readFaultAt && (fault == Fault::ReadZero || fault == Fault::ReadError || fault == Fault::ReadStalled))
        {
            if (++faultHits > 8) throw NoReadProgress{};
            // Defined stale/empty buffer; do not make the baseline depend on
            // uninitialized stack contents when -1 converts to bool true.
            data[0] = '\n'; data[1] = 0;
            if (fault == Fault::ReadError) return -1;
            return fault == Fault::ReadZero ? 0 : 1;
        }
        return QFile::readLine(data, count);
    }
    qint64 write(const char* data, qint64 count)
    {
        if (fault == Fault::WriteError) { ++faultHits; return -1; }
        if (fault == Fault::WriteShort && count) { ++faultHits; --count; }
        return QFile::write(data, count);
    }
    qint64 write(const QByteArray& bytes) { return write(bytes.constData(), bytes.size()); }
    bool flush()
    {
        if (fault == Fault::Flush) { ++faultHits; return false; }
        return QFile::flush();
    }
};

namespace melonDS::Platform
{
void Log(LogLevel, const char*, ...) {}
FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    Require(QString::fromStdString(path) == allowedPath, "Unexpected file access outside generated fixture");
    Require(mode == FileMode::ReadText || mode == FileMode::WriteText, "Unexpected file mode");
    const bool writing = mode == FileMode::WriteText;
    if (writing) ++writeOpens;
    if (!writing && fault == Fault::Open) { ++faultHits; return nullptr; }
    auto file = std::make_unique<CheatFile>(allowedPath);
    file->Writing = writing;
    const auto qmode = writing ? QIODevice::WriteOnly | QIODevice::Truncate : QIODevice::ReadOnly;
    if (!file->open(qmode | QIODevice::Text)) return nullptr;
    return reinterpret_cast<FileHandle*>(file.release());
}
#define QFile CheatFile
#define CloseFile NativeCloseFile
#include "arCloseFile.inc"
#undef CloseFile
#include "cheatFileExists.inc"
#include "arIsEndOfFile.inc"
#include "cheatFileReadLine.inc"
#include "arFilePosition.inc"
#include "cheatFileWrite.inc"
#include "cheatFileWriteFormatted.inc"
#include "cheatFileFlush.inc"
#undef QFile
bool CloseFile(FileHandle* file)
{
    ++closeCalls;
    const bool fail = fault == Fault::Close && reinterpret_cast<CheatFile*>(file)->Writing;
    const bool result = NativeCloseFile(file);
    if (fail) { ++faultHits; return false; }
    return result;
}
}

static void Put(const std::string& text)
{
    QFile file(allowedPath);
    Require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(text.data(), text.size()) == static_cast<qint64>(text.size()), "Fixture write failed");
}
static std::string Bytes()
{
    QFile file(allowedPath);
    Require(file.open(QIODevice::ReadOnly), "Fixture read failed");
    return file.readAll().toStdString();
}
static std::string Text()
{
    auto bytes = QByteArray::fromStdString(Bytes());
    return bytes.replace("\r\n", "\n").toStdString();
}
static bool SameTree(const ARCodeCat& a, const ARCodeCat& b)
{
    if (a.Name != b.Name || a.Description != b.Description ||
        a.OnlyOneCodeEnabled != b.OnlyOneCodeEnabled || a.Children.size() != b.Children.size()) return false;
    auto bi = b.Children.begin();
    for (const auto& ai : a.Children)
    {
        const auto& item = *bi++;
        if (ai.index() != item.index()) return false;
        if (const auto* ac = std::get_if<ARCode>(&ai))
        {
            const auto& bc = std::get<ARCode>(item);
            if (ac->Name != bc.Name || ac->Description != bc.Description ||
                ac->Enabled != bc.Enabled || ac->Code != bc.Code) return false;
        }
        else if (!SameTree(std::get<ARCodeCat>(ai), std::get<ARCodeCat>(item))) return false;
    }
    return true;
}
static void CheckParents(const ARCodeCat& root)
{
    for (const auto& item : root.Children)
    {
        if (const auto* code = std::get_if<ARCode>(&item))
            Check(code->Parent == &root, "Loaded code has a dangling/wrong Parent");
        else
        {
            const auto& cat = std::get<ARCodeCat>(item);
            Check(cat.Parent == &root, "Loaded category has a dangling/wrong Parent");
            CheckParents(cat);
        }
    }
}

static const std::string Stable = "ROOT\n\nCODE 1 Original\nDESC preserved note\n12345678 9ABCDEF0\n";
static const std::string Normal =
    "ROOT\n\nCODE 1 Root \xED\x95\x9C\xEA\xB8\x80\nDESC root note\n00000000 FFFFFFFF\n\n"
    "CAT 1 Group\nDESC category note\n\nCODE 1 First\n12345678 9ABCDEF0\n\n"
    "CODE 0 Second\nDESC code note\n00000001 00000002\n\nCAT 0 Empty\n\n"
    "ROOT\n\nCODE 0 Tail\n\n";

static void Controls()
{
    ARCodeFile missing(allowedPath.toStdString());
    Check(!missing.Error && missing.RootCat.Children.empty(), "First-run missing file was rejected");
    Check(missing.Save() && Bytes().empty(), "Empty file save failed");
    // The parser accepts CRLF and a final unterminated data line.
    Put("ROOT\r\nCODE 0 Final\r\n12345678 9ABCDEF0");
    ARCodeFile finalLine(allowedPath.toStdString());
    Check(!finalLine.Error && finalLine.GetCodes().size() == 1, "Normal EOF/CRLF was rejected");
    Put(Normal);
    ARCodeFile file(allowedPath.toStdString());
    Require(!file.Error, "Normal fixture could not load");
    const auto before = file.RootCat;
    CheckParents(file.RootCat);
    Check(file.Save() && Text() == Normal, "Normal mch formatting/Unicode changed");
    ARCodeFile loaded(allowedPath.toStdString());
    Check(!loaded.Error && SameTree(before, loaded.RootCat), "Normal root/category/description roundtrip lost data");
    CheckParents(loaded.RootCat);
    // Existing OnlyOne loading policy must remain first-enabled-wins.
    Put("CAT 1 Group\nCODE 1 First\nCODE 1 Second\n");
    ARCodeFile one(allowedPath.toStdString());
    const auto codes = one.GetCodes();
    Check(codes.size() == 2 && codes[0].Enabled && !codes[1].Enabled, "OnlyOne load behavior changed");
}

static void RejectSave(ARCodeFile& file)
{
    const auto original = Bytes();
    const auto before = file.RootCat;
    writeOpens = 0;
    try { Check(!file.Save(), "Invalid/error-marked object Save reported success"); }
    catch (const std::bad_variant_access&) { Check(false, "Nested category Save threw after opening the original"); }
    Check(writeOpens == 0 && Bytes() == original, "Rejected Save opened/overwrote the original file");
    Check(SameTree(before, file.RootCat), "Rejected Save changed the source tree");
}
static void Preflight()
{
    for (unsigned variant = 0; variant < 4; ++variant)
    {
        Put(Stable);
        ARCodeFile file(allowedPath.toStdString());
        auto& code = std::get<ARCode>(file.RootCat.Children.front());
        if (variant == 0) file.Error = true;
        if (variant == 1) code.Name = std::string(128, 'A');
        if (variant == 2) code.Description = "note\r\nROOT";
        if (variant == 3) code.Name = std::string("Name\0tail", 9);
        RejectSave(file);
    }
}
static void Nested()
{
    Put(Stable);
    ARCodeFile file(allowedPath.toStdString());
    ARCodeCat outer {}; outer.Name = "Outer";
    ARCodeCat inner {}; inner.Name = "Inner";
    outer.Children.emplace_back(std::move(inner));
    file.RootCat.Children.emplace_back(std::move(outer));
    RejectSave(file);
}
static void SaveFault(Fault which)
{
    Put(Stable);
    ARCodeFile file(allowedPath.toStdString());
    fault = which; faultHits = 0; closeCalls = 0;
    Check(!file.Save(), "Legacy Save did not report its write/flush/close failure");
    Check(faultHits > 0, "Save never reached the required I/O failure check");
    Check(closeCalls == 1 && openFiles.empty(), "Failed Save did not close exactly once");
    // A legacy in-place Save may already have changed the bytes on I/O failure.
    // Only the parent's QSaveFile integration supplies atomic replacement.
    fault = Fault::None;
}
static void Unreadable()
{
    Put(Stable);
    fault = Fault::Open;
    ARCodeFile file(allowedPath.toStdString());
    fault = Fault::None;
    Check(file.Error, "Unreadable existing file was treated as a missing/empty file");
    RejectSave(file);
}
static void LoadFault(Fault which, bool malformed = false)
{
    Put(Stable);
    ARCodeFile file(allowedPath.toStdString());
    const auto before = file.RootCat;
    if (malformed) Put("ROOT\nCODE 1 Partial\n12345678 9ABCDEF0\ninvalid\n");
    fault = which; faultHits = 0; readFaultAt = 5;
    try { Check(!file.Load(), "Interrupted/malformed Load reported success"); }
    catch (const NoReadProgress&)
    {
        Check(false, "Load repeatedly retried a read without advancing");
        // Reclaim only this fixture's leaked handle after stopping baseline's
        // loop. The guard is reported as FAIL, never as a parser rejection.
        while (!openFiles.empty()) Platform::CloseFile(reinterpret_cast<Platform::FileHandle*>(openFiles.back()));
    }
    fault = Fault::None;
    Check(file.Error, "Load failure was not reflected in Error");
    Check(SameTree(before, file.RootCat), "Failed reload discarded the previous tree");
    CheckParents(file.RootCat);
    RejectSave(file);
    Put(Stable);
    Check(file.Load() && !file.Error && SameTree(before, file.RootCat), "Successful reload did not recover from Error");
    CheckParents(file.RootCat);
}
static void LoadLength()
{
    Put("ROOT\nCODE 1 " + std::string(128, 'A') + "\n12345678 9ABCDEF0\n");
    ARCodeFile file(allowedPath.toStdString());
    Check(file.Error, "Oversized input name was silently truncated");
    RejectSave(file);
}

#ifdef ARCODE_SERIALIZE_TESTS
static void Serialize()
{
    Put(Normal);
    ARCodeFile file(allowedPath.toStdString());
    const auto before = file.RootCat;
    const int beforeWrites = writeOpens;
    std::string output = "unchanged sentinel";
    Check(file.GetFilename() == allowedPath.toStdString(), "Filename accessor changed the path");
    Check(file.Serialize(output) && output == Normal && SameTree(before, file.RootCat), "Pure Serialize changed the object/format");
    Check(writeOpens == beforeWrites && Bytes() == Normal, "Pure Serialize performed file I/O");
    auto& rootCode = std::get<ARCode>(file.RootCat.Children.front());
    rootCode.Name = std::string(127, 'A');
    rootCode.Description = std::string(255, 'D');
    Check(file.Serialize(output), "Representable name/description maximum was rejected");
    Put(output);
    ARCodeFile maximum(allowedPath.toStdString());
    Check(!maximum.Error && SameTree(file.RootCat, maximum.RootCat), "Maximum fields did not roundtrip");

    for (unsigned variant = 0; variant < 11; ++variant)
    {
        ARCodeFile bad = file;
        auto& code = std::get<ARCode>(bad.RootCat.Children.front());
        auto& cat = std::get<ARCodeCat>(*std::next(bad.RootCat.Children.begin()));
        switch (variant)
        {
        case 0: bad.Error = true; break;
        case 1: code.Code.push_back(1); break;
        case 2: code.Name.clear(); break;
        case 3: code.Name = " leading"; break;
        case 4: code.Description = "\tnote"; break;
        case 5: code.Description.assign(256, 'D'); break;
        case 6: cat.Name.assign(128, 'C'); break;
        case 7: cat.Description = std::string("note\0tail", 9); break;
        case 8: cat.Children.emplace_back(ARCodeCat{}); break;
        case 9: std::get<ARCode>(cat.Children.back()).Enabled = true; break;
        case 10: bad.RootCat.Description = "not representable on ROOT"; break;
        }
        const auto original = bad.RootCat;
        output = "unchanged sentinel";
        Check(!bad.Serialize(output) && output == "unchanged sentinel", "Invalid Serialize committed partial output");
        Check(SameTree(original, bad.RootCat), "Invalid Serialize modified its source object");
        RejectSave(bad);
    }
}
#endif

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    QTemporaryDir temp(QDir::currentPath() + "/arcode-XXXXXX");
    if (!temp.isValid()) return 2;
    allowedPath = temp.filePath("generated.mch");
    const std::string name = argv[1];
    try
    {
        if (name == "controls") Controls();
        else if (name == "preflight") Preflight();
        else if (name == "nested") Nested();
        else if (name == "save-short") SaveFault(Fault::WriteShort);
        else if (name == "save-error") SaveFault(Fault::WriteError);
        else if (name == "save-flush") SaveFault(Fault::Flush);
        else if (name == "save-close") SaveFault(Fault::Close);
        else if (name == "load-unreadable") Unreadable();
        else if (name == "load-zero") LoadFault(Fault::ReadZero);
        else if (name == "load-error") LoadFault(Fault::ReadError);
        else if (name == "load-stalled") LoadFault(Fault::ReadStalled);
        else if (name == "load-malformed") LoadFault(Fault::None, true);
        else if (name == "load-length") LoadLength();
#ifdef ARCODE_SERIALIZE_TESTS
        else if (name == "serialize") Serialize();
#endif
        else return 2;
        Check(openFiles.empty(), "File handle leaked");
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Fixture error: %s\n", error.what());
        return 2;
    }
    std::printf("%s: %s (%d failures)\n", name.c_str(), Failures ? "FAIL" : "PASS", Failures);
    return Failures ? 1 : 0;
}
