// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ASSETIDENTITY_H
#define ASSETIDENTITY_H

#include <QString>
#include <QStringList>
#include <QMetaType>
#include <functional>

namespace AssetIdentity
{
enum class Choice { Cancel, Existing, Separate };
struct Conflict
{
    QStringList Paths;
    bool CanUseExisting = false;
};
struct Selection
{
    QStringList Source;
    QString Name;
    QString SaveDirectory, StateDirectory, CheatDirectory;
    bool Valid() const { return !Source.empty() && !Name.isEmpty(); }
};
// This is called before dispatch on the UI thread. The chooser only receives
// paths relevant to the decision; identity hashes stay in local metadata.
bool Prepare(const QString& registryDirectory, const QStringList& source, bool gba, const QStringList& directories,
             bool allowExisting, const std::function<Choice(const Conflict&)>& choose,
             Selection& result, QString& error);
bool PathsUnchanged(const Selection& selected, bool gba, const QStringList& directories);
}
Q_DECLARE_METATYPE(AssetIdentity::Selection)
#endif
