// SPDX-License-Identifier: GPL-3.0-or-later
#include "AssetIdentity.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

namespace AssetIdentity
{
namespace
{
QString Canonical(const QString& path)
{
    QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}
QString Key(const QString& name)
{
    // Keep a portable namespace even on case-sensitive volumes. Distinct
    // source paths still have distinct identities below.
    return name.toCaseFolded();
}
bool SameDirectory(const QString& left, const QString& right)
{
    if (left == right) return true;
    std::error_code error;
#ifdef Q_OS_WIN
    const bool same = std::filesystem::equivalent(left.toStdWString(), right.toStdWString(), error);
#else
    const bool same = std::filesystem::equivalent(left.toStdString(), right.toStdString(), error);
#endif
    return !error && same;
}
struct Folder
{
    QStringList paths;
    QString metadata;
    QJsonObject owners;
    std::unique_ptr<QLockFile> lock;
};
QString Owner(const Folder& folder, const QString& name)
{
    return folder.owners.value(Key(name)).toString();
}
bool HasFiles(const Folder& folder, const QString& name)
{
    const QRegularExpression pattern("^" + QRegularExpression::escape(name) +
        "\\.(sav|mch|ml[0-9])(?:\\.[0-9]+)?$", QRegularExpression::CaseInsensitiveOption);
    for (const auto& path : folder.paths)
        for (const auto& item : QDir(path).entryList(QDir::Files | QDir::Hidden | QDir::System))
            if (pattern.match(item).hasMatch()) return true;
    return false;
}
}

bool PathsUnchanged(const Selection& selected, bool gba, const QStringList& directories)
{
    if (!selected.Valid() || directories.size() != 3) return false;
    const QStringList previous{selected.SaveDirectory, selected.StateDirectory, selected.CheatDirectory};
    for (int i = 0; i < (gba ? 1 : 3); ++i)
    {
        const auto path = directories[i].isEmpty() ? QFileInfo(selected.Source.front()).absolutePath() : directories[i];
        if (!SameDirectory(Canonical(path), previous[i])) return false;
    }
    return true;
}

bool Prepare(const QString& registryDirectory, const QStringList& source, bool gba, const QStringList& directories,
             bool allowExisting, const std::function<Choice(const Conflict&)>& choose,
             Selection& result, QString& error)
{
    if (source.empty() || source.size() > 2 || directories.size() != 3 || !QFileInfo(source.front()).isFile())
    {
        error = "The ROM source is missing or invalid.";
        return false;
    }
    QString romName = QFileInfo(source.back()).fileName();
    if (source.size() == 1 && romName.endsWith(".zst")) romName.chop(4);
    const auto dot = romName.lastIndexOf('.');
    const QString base = dot < 0 ? romName : romName.left(dot);
    if (base.isEmpty()) { error = "The ROM has no usable file name."; return false; }
    const QString sourcePath = Canonical(source.front());
    const QString visiblePath = QDir::cleanPath(QFileInfo(source.front()).absoluteFilePath());
    // A differently named symlink keeps its own legacy basename and directory.
    // Include that visible source as well as the target so one identity cannot
    // silently switch asset names when reopened through another alias.
    const QJsonArray identity{gba ? "GBA" : "DS", sourcePath, visiblePath, source.size() == 2 ? source.back() : QString{}};
    const QString id = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    QString stem = base.left(80);
    if (!stem.isEmpty() && stem.back().isHighSurrogate()) stem.chop(1);
    const QString separate = stem + "-" + id.left(16);
    QStringList paths;
    for (const auto& configured : directories)
        paths.push_back(Canonical(configured.isEmpty() ? QFileInfo(visiblePath).absolutePath() : configured));
    QStringList uniquePaths;
    for (const auto& path : (gba ? QStringList{paths[0]} : paths))
        if (std::none_of(uniquePaths.begin(), uniquePaths.end(), [&](const QString& prior) { return SameDirectory(path, prior); }))
            uniquePaths.push_back(path);
    std::sort(uniquePaths.begin(), uniquePaths.end());
    if (registryDirectory.isEmpty() || !QDir().mkpath(registryDirectory))
    { error = "Unable to create the asset ownership directory."; return false; }
    std::vector<Folder> folders;
    for (const auto& path : uniquePaths)
    {
        if (!QDir(path).exists()) { error = "An asset folder does not exist:\n" + path; return false; }
        // A user-wide registry also works when ROM/cheat folders are read-only.
        // Case-folding here conservatively shares a namespace across volume
        // case rules; game source identities themselves retain their spelling.
        const auto directoryID = QString::fromLatin1(QCryptographicHash::hash(
            path.toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex());
        const auto record = QDir(registryDirectory).filePath(directoryID);
        const auto shared = std::find_if(folders.begin(), folders.end(), [&](const Folder& prior) {
            return prior.metadata == record + ".json";
        });
        if (shared != folders.end())
        {
            // Case-sensitive folders with case-only differences conservatively
            // share one ownership record, but all must be checked for files.
            shared->paths.push_back(path);
            continue;
        }
        Folder folder{{path}, record + ".json", {}, std::make_unique<QLockFile>(record + ".lock")};
        folder.lock->setStaleLockTime(0);
        if (!folder.lock->tryLock(0))
        {
            error = folder.lock->error() == QLockFile::LockFailedError ?
                "Another process is choosing files in this folder. Retry when it finishes:\n" + path :
                "Unable to lock the asset folder. Check write permissions:\n" + path;
            return false;
        }
        QFile file(folder.metadata);
        if (file.exists())
        {
            if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
            { error = "Unable to read asset ownership metadata:\n" + path; return false; }
            QJsonParseError parseError;
            const auto bytes = file.read(1024 * 1024 + 1);
            const auto document = QJsonDocument::fromJson(bytes, &parseError);
            const auto object = document.object();
            if (bytes.size() > 1024 * 1024 || bytes.size() != file.size() || file.error() != QFile::NoError || parseError.error != QJsonParseError::NoError ||
                object.value("version").toInt() != 1 || !object.value("owners").isObject())
            { error = "Invalid asset ownership metadata. Existing files were preserved:\n" + path; return false; }
            folder.owners = object.value("owners").toObject();
            const QRegularExpression digest("^[a-f0-9]{64}$");
            for (auto it = folder.owners.begin(); it != folder.owners.end(); ++it)
                if (it.key().isEmpty() || it.key() != Key(it.key()) || !it.value().isString() || !digest.match(it.value().toString()).hasMatch())
                { error = "Invalid asset ownership metadata. Existing files were preserved:\n" + path; return false; }
        }
        folders.push_back(std::move(folder));
    }
    QString name = base;
    for (const auto& folder : folders)
        if (Owner(folder, separate) == id) name = separate;
    Conflict conflict;
    conflict.CanUseExisting = allowExisting;
    if (name == base && base.compare("firmware", Qt::CaseInsensitive) == 0)
    {
        conflict.CanUseExisting = false;
        conflict.Paths.push_back("The name firmware is reserved for firmware boot state files.");
    }
    for (const auto& folder : folders)
    {
        const auto owner = Owner(folder, name);
        if (!owner.isEmpty() && owner != id)
        {
            conflict.CanUseExisting = false;
            for (const auto& path : folder.paths)
                conflict.Paths.push_back(QDir(path).filePath(name));
        }
        else if (owner.isEmpty() && HasFiles(folder, name))
            for (const auto& path : folder.paths)
                conflict.Paths.push_back(QDir(path).filePath(name));
    }
    if (!conflict.Paths.empty())
    {
        const auto choice = choose(conflict);
        if (choice == Choice::Cancel) { error.clear(); return false; }
        if (choice == Choice::Existing && !conflict.CanUseExisting)
        { error = "These files are assigned to another source. Select separate files."; return false; }
        if (choice == Choice::Separate) name = separate;
    }
    // Even a shortened digest is never evidence for reusing another owner.
    for (const auto& folder : folders)
        if ((!Owner(folder, name).isEmpty() && Owner(folder, name) != id) ||
            (name == separate && Owner(folder, name).isEmpty() && HasFiles(folder, name)))
        { error = "The separate file name is already in use. Existing files were preserved."; return false; }
    for (auto& folder : folders)
    {
        if (Owner(folder, name) == id) continue;
        folder.owners.insert(Key(name), id);
        const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"owners", folder.owners}}).toJson(QJsonDocument::Compact);
        QSaveFile file(folder.metadata);
        if (bytes.size() > 1024 * 1024 || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        { error = "Unable to record asset ownership. Existing game files were preserved:\n" + folder.paths.join('\n'); return false; }
    }
    QStringList stableSource = source;
    stableSource[0] = visiblePath;
    Selection selected{stableSource, name, paths[0], paths[1], paths[2]};
    result = std::move(selected);
    error.clear();
    return true;
}
}
