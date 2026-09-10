// SPDX-License-Identifier: GPL-3.0-or-later
// Real ownership metadata and Qt temporary files. No personal game data.
#include "AssetIdentity.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>
#include <filesystem>
#include <memory>

using namespace AssetIdentity;
static int failures = 0;
static void Check(bool ok, const char* why)
{ if (!ok) { ++failures; std::fprintf(stderr, "%s\n", why); } }
static bool Write(const QString& path, const QByteArray& bytes)
{ QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
static QByteArray Read(const QString& path)
{ QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    const QString mode = argv[1];
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    const QDir root(temporary.path());
    for (const auto& dir : {"a", "b", "saves"}) if (!root.mkpath(dir)) return 2;
    const QString a = root.filePath("a/game.nds"), b = root.filePath("b/game.nds");
    const QString folder = root.filePath("saves"), registry = root.filePath("ownership");
    const auto metadataPath = [&]() {
        const QDir records(registry);
        const auto names = records.entryList({"*.json"}, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        return names.size() == 1 ? records.filePath(names.front()) : QString{};
    };
    if (!Write(a, "source A") || !Write(b, "source B")) return 2;
    const QStringList dirs{folder, folder, folder};
    Selection selected;
    QString error;
    unsigned prompts = 0;
    bool canUseExisting = false;
    Choice answer = Choice::Cancel;
    const auto choose = [&](const Conflict& conflict) {
        ++prompts;
        canUseExisting = conflict.CanUseExisting;
        Check(!conflict.Paths.empty(), "Conflict has no reviewable paths");
        return answer;
    };
    const auto prepare = [&](const QStringList& source, bool gba, const QStringList& paths, bool allowExisting,
                             const std::function<Choice(const Conflict&)>& chooser, Selection& output, QString& failure) {
        return Prepare(registry, source, gba, paths, allowExisting, chooser, output, failure);
    };
    if (mode == "normal" || mode == "collision" || mode == "gba" || mode == "archive" || mode == "case-name")
    {
        Check(prepare({a}, false, dirs, true, choose, selected, error) && selected.Name == "game" && prompts == 0,
              "Fresh source did not retain its original name");
        const auto before = Read(metadataPath());
        Check(!before.isEmpty() && QDir(folder).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
              "Ownership records were not kept outside the asset folder");
        Check(prepare({a}, false, dirs, true, choose, selected, error) && Read(metadataPath()) == before && prompts == 0,
              "Same source changed ownership or prompted again");
        if (mode != "normal")
        {
            const auto upper = root.filePath("b/GAME.nds");
            if (mode == "case-name" && !QFile::rename(b, upper)) return 2;
            const QStringList source = mode == "archive" ? QStringList{a, "nested/game.nds"} :
                QStringList{mode == "gba" ? a : mode == "case-name" ? upper : b};
            Check(!prepare(source, mode == "gba", dirs, true, choose, selected, error) && !canUseExisting && Read(metadataPath()) == before,
                  "Another source could silently claim existing ownership");
            answer = Choice::Separate;
            Check(prepare(source, mode == "gba", dirs, true, choose, selected, error) && selected.Name != "game",
                  "Separate files did not resolve the collision");
            const auto name = selected.Name;
            const auto prompted = prompts;
            Check(prepare(source, mode == "gba", dirs, true, choose, selected, error) && selected.Name == name && prompts == prompted,
                  "Separate choice was not preserved on reopen");
        }
    }
    else if (mode == "legacy" || mode == "legacy-separate" || mode == "relocation")
    {
        for (const auto& ext : {".sav", ".ml0", ".mch", ".sav.2"})
            if (!Write(folder + "/game" + ext, "legacy bytes")) return 2;
        Check(!prepare({a}, false, dirs, mode != "relocation", choose, selected, error) && metadataPath().isEmpty(),
              "Unassigned legacy data was silently adopted");
        Check(canUseExisting == (mode != "relocation"), "Relocation permitted overwriting unassigned data");
        answer = mode == "legacy" ? Choice::Existing : Choice::Separate;
        Check(prepare({a}, false, dirs, mode != "relocation", choose, selected, error), "Explicit choice failed");
        Check((selected.Name == "game") == (mode == "legacy"), "Selected ownership policy was not applied");
        for (const auto& ext : {".sav", ".ml0", ".mch", ".sav.2"})
            Check(Read(folder + "/game" + ext) == "legacy bytes", "Existing file was renamed, replaced or altered");
    }
    else if (mode == "metadata" || mode == "locked" || mode == "record-type")
    {
        if (!prepare({a}, false, dirs, true, choose, selected, error)) return 2;
        const auto metadata = metadataPath();
        if (metadata.isEmpty()) return 2;
        std::unique_ptr<QLockFile> lock;
        if (mode == "metadata" && !Write(metadata, "{broken metadata")) return 2;
        if (mode == "record-type" && (!QFile::remove(metadata) || !QDir().mkpath(metadata))) return 2;
        if (mode == "locked")
        {
            lock = std::make_unique<QLockFile>(metadata.chopped(5) + ".lock");
            if (!lock->tryLock(0)) return 2;
        }
        selected.Name = "preserved";
        Check(!prepare({a}, false, dirs, true, choose, selected, error) && selected.Name == "preserved" && !error.isEmpty(),
              "Failed metadata changed the selected path or lost the error");
        Check(prompts == 0, "Metadata failure was treated as legacy ownership consent");
        if (mode == "metadata") Check(Read(metadataPath()) == "{broken metadata", "Invalid metadata was overwritten");
    }
    else if (mode == "registry-failure")
    {
        if (!Write(registry, "blocking file")) return 2;
        selected.Name = "preserved";
        Check(!prepare({a}, false, dirs, true, choose, selected, error) && selected.Name == "preserved" &&
              !error.isEmpty() && prompts == 0 && Read(registry) == "blocking file" && Read(a) == "source A",
              "An unavailable registry modified files or bypassed ownership checks");
    }
    else if (mode == "alias")
    {
        // Directory junctions work without symlink privileges on Windows.
        const QString alias = root.filePath("alias");
#ifdef Q_OS_WIN
        const auto quote = [](QString value) { return "'" + value.replace("'", "''") + "'"; };
        const auto powershell = qEnvironmentVariable("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe";
        if (QProcess::execute(powershell, {"-NoProfile", "-NonInteractive", "-Command",
            "$ErrorActionPreference = 'Stop'; New-Item -ItemType Junction -Path " + quote(alias) +
            " -Target " + quote(root.filePath("a")) + " | Out-Null"}) != 0) return 2;
#else
        std::error_code ec;
        std::filesystem::create_directory_symlink(root.filePath("a").toStdString(), alias.toStdString(), ec);
        if (ec) return 2;
#endif
        const QStringList defaults{QString{}, QString{}, QString{}};
        const QString source = alias + "/game.nds";
        Check(prepare({source}, false, defaults, true, choose, selected, error) && selected.Source.front() == source &&
              selected.Name == "game" && PathsUnchanged(selected, false, defaults),
              "An alias changed its source name or default directory on reset");
    }
    else if (mode == "unchanged")
    {
        Check(prepare({a}, false, dirs, true, choose, selected, error), "Initial selection failed");
        QFile::remove(a);
        Check(PathsUnchanged(selected, false, dirs), "Reset unnecessarily required the ROM file again");
        const QStringList changed{folder, root.filePath("new-state-directory"), folder};
        Check(!PathsUnchanged(selected, false, changed) && PathsUnchanged(selected, true, changed),
              "DS/GBA path change scope was incorrect");
    }
    else if (mode == "folders")
    {
        for (const auto& dir : {"states", "cheats"}) root.mkpath(dir);
        const QStringList split{folder, root.filePath("states"), root.filePath("cheats")};
        Write(split[2] + "/game.mch", "legacy cheat");
        answer = Choice::Separate;
        Check(prepare({a}, false, split, true, choose, selected, error) && prompts == 1 && selected.Name != "game" &&
              selected.SaveDirectory == split[0] && selected.StateDirectory == split[1] && selected.CheatDirectory == split[2],
              "A cheat-only collision did not coordinate all asset paths");
        Check(Read(split[2] + "/game.mch") == "legacy cheat", "Legacy cheat was modified");
    }
    else return 2;
    std::printf("Asset identity %s: %d failures\n", argv[1], failures);
    return failures ? 1 : 0;
}
