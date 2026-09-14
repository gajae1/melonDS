// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ROMLIBRARYDIALOG_H
#define ROMLIBRARYDIALOG_H

#include <QDialog>
#include <QModelIndex>
#include <QString>

class QFileInfo;
class QFileSystemModel;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QPushButton;
class QResizeEvent;
class QSortFilterProxyModel;
class QTreeView;

// Nonmodal ROM library browser over one user-selected folder. The dialog owns
// no emulator, configuration or preparation state: it lists the folder with
// QFileSystemModel + QSortFilterProxyModel and emits absolute file paths for
// the existing ROM preparation flow, including archive member selection.
class ROMLibraryDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ROMLibraryDialog(const QString& folder, QWidget* parent = nullptr);

    // Re-roots the library; also used by focused real-widget tests.
    void setFolder(const QString& folder);

signals:
    void folderChanged(const QString& folder);
    void openROM(const QString& path);

private slots:
    void chooseFolder();
    void applyNameFilter();
    void onDirectoryLoaded(const QString& path);
    void updateOpenButton();
    void openSelected();
    void openActivated(const QModelIndex& proxyIndex);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void resetFileSystemModel();
    QFileInfo fileInfoFor(const QModelIndex& proxyIndex) const;
    void updateFolderLabel();
    void refreshStatus();

    QFileSystemModel* fsModel = nullptr;
    QFileSystemWatcher* rootWatcher = nullptr;
    QSortFilterProxyModel* proxy = nullptr;
    QTreeView* tree = nullptr;
    QLineEdit* filterEdit = nullptr;
    QLabel* folderLabel = nullptr;
    QLabel* statusLabel = nullptr;
    QPushButton* openButton = nullptr;
    QString rootPath;        // cleaned absolute root requested via setFolder
    bool rootValid = false;  // proxy may show rows under the current root
    bool rootLoaded = false; // the current root finished loading
    bool selectedFileRemoved = false;
};

#endif // ROMLIBRARYDIALOG_H
