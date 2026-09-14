// SPDX-License-Identifier: GPL-3.0-or-later

#include "ROMLibraryDialog.h"
#include "ROMFileTypes.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QVBoxLayout>

namespace
{
// Library scope: DS ROMs, zstd-compressed DS ROMs, and archives whose members
// the existing ROM preparation flow inspects. GBA-only ROMs are deliberately
// not listed as DS games.
bool IsLibraryROMName(const QString& filename)
{
    return NdsRomByExtension(filename)
        || ZstdNdsRomByExtension(filename)
        || SupportedArchiveByExtension(filename);
}

// QFileSystemModel + QSortFilterProxyModel only. Filtering matches file names,
// never a recursive search. Directories always pass so ancestor paths stay
// mapped while file names are filtered and navigation stays explicit.
class ROMLibraryProxy : public QSortFilterProxyModel
{
public:
    explicit ROMLibraryProxy(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent) {}

    void setRootValid(bool valid)
    {
        rootValid = valid;
        invalidate();
    }
    void setNameFilter(const QString& filter)
    {
        nameFilter = filter;
        invalidate();
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        if (!rootValid) return false;
        auto* fs = qobject_cast<QFileSystemModel*>(sourceModel());
        if (!fs) return false;

        const QFileInfo info = fs->fileInfo(fs->index(sourceRow, 0, sourceParent));
        if (info.isDir()) return true;

        const QString name = info.fileName();
        if (!IsLibraryROMName(name)) return false;
        return nameFilter.isEmpty() || name.contains(nameFilter, Qt::CaseInsensitive);
    }

    bool lessThan(const QModelIndex& sourceLeft, const QModelIndex& sourceRight) const override
    {
        auto* fs = qobject_cast<QFileSystemModel*>(sourceModel());
        if (!fs) return QSortFilterProxyModel::lessThan(sourceLeft, sourceRight);

        const QFileInfo left = fs->fileInfo(sourceLeft);
        const QFileInfo right = fs->fileInfo(sourceRight);
        if (left.isDir() != right.isDir()) return left.isDir(); // folders first
        switch (sourceLeft.column())
        {
        case 1: return left.size() < right.size();
        case 3: return left.lastModified() < right.lastModified();
        default: return left.fileName().compare(right.fileName(), Qt::CaseInsensitive) < 0;
        }
    }

private:
    bool rootValid = false;
    QString nameFilter;
};

// The dialog only ever constructs this subclass; keep it out of the header.
ROMLibraryProxy* LibraryProxy(QSortFilterProxyModel* proxy)
{
    return static_cast<ROMLibraryProxy*>(proxy);
}
}

ROMLibraryDialog::ROMLibraryDialog(const QString& folder, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("ROM library"));
    setModal(false);

    fsModel = new QFileSystemModel(this);
    fsModel->setReadOnly(true);
    fsModel->setOption(QFileSystemModel::DontUseCustomDirectoryIcons, true);

    // Observe before the proxy/view changes selection on removal. Qt otherwise
    // selects an adjacent ROM, which could turn a pending Open into another game.
    connect(fsModel, &QAbstractItemModel::rowsAboutToBeRemoved, this,
        [this](const QModelIndex& parent, int first, int last) {
            auto selected = proxy->mapToSource(tree->currentIndex());
            while (selected.isValid() && selected.parent() != parent)
                selected = selected.parent();
            selectedFileRemoved |= selected.isValid() && selected.row() >= first && selected.row() <= last;
        });

    proxy = new ROMLibraryProxy(this);
    proxy->setSourceModel(fsModel);

    tree = new QTreeView(this);
    tree->setObjectName("romLibraryTree");
    tree->setModel(proxy);
    tree->setSortingEnabled(true);
    tree->sortByColumn(0, Qt::AscendingOrder);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree->setUniformRowHeights(true);
    tree->setContextMenuPolicy(Qt::NoContextMenu);
    tree->hideColumn(2); // Type; keep filename, size and modified visible
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    auto* heading = new QLabel(tr("ROM library"), this);

    folderLabel = new QLabel(this);
    folderLabel->setTextFormat(Qt::PlainText);

    auto* chooseButton = new QPushButton(tr("Choose folder…"), this);
    chooseButton->setObjectName("romLibraryChoose");
    chooseButton->setAutoDefault(false);

    auto* filterLabel = new QLabel(tr("Filter file names"), this);
    filterEdit = new QLineEdit(this);
    filterEdit->setObjectName("romLibraryFilter");
    filterEdit->setPlaceholderText(tr("e.g. mario or .nds"));
    filterEdit->setClearButtonEnabled(true);
    filterEdit->setToolTip(tr("Filters files in the folders you expand."));
    filterLabel->setBuddy(filterEdit);

    statusLabel = new QLabel(this);
    statusLabel->setObjectName("romLibraryStatus");
    statusLabel->setTextFormat(Qt::PlainText);

    openButton = new QPushButton(tr("Open"), this);
    openButton->setObjectName("romLibraryOpen");
    openButton->setAutoDefault(false); // one activation path: the view
    openButton->setEnabled(false);
    auto* closeButton = new QPushButton(tr("Close"), this);
    closeButton->setAutoDefault(false);

    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(folderLabel, 1);
    folderRow->addWidget(chooseButton);

    auto* filterRow = new QHBoxLayout;
    filterRow->addWidget(filterLabel);
    filterRow->addWidget(filterEdit, 1);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    buttonRow->addWidget(openButton);
    buttonRow->addWidget(closeButton);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(heading);
    layout->addLayout(folderRow);
    layout->addLayout(filterRow);
    layout->addWidget(tree, 1);
    layout->addWidget(statusLabel);
    layout->addLayout(buttonRow);

    connect(chooseButton, &QPushButton::clicked, this, &ROMLibraryDialog::chooseFolder);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    connect(filterEdit, &QLineEdit::textChanged, this, &ROMLibraryDialog::applyNameFilter);
    connect(fsModel, &QFileSystemModel::directoryLoaded, this, &ROMLibraryDialog::onDirectoryLoaded);
    connect(tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ROMLibraryDialog::updateOpenButton);
    connect(tree, &QTreeView::activated, this, &ROMLibraryDialog::openActivated);
    connect(openButton, &QPushButton::clicked, this, &ROMLibraryDialog::openSelected);
    connect(fsModel, &QAbstractItemModel::rowsRemoved, this, [this] {
        if (selectedFileRemoved)
        {
            selectedFileRemoved = false;
            tree->selectionModel()->clear();
        }
        refreshStatus();
    });
    connect(proxy, &QAbstractItemModel::rowsInserted, this, &ROMLibraryDialog::refreshStatus);

    resize(640, 480);
    setFolder(folder);
}

void ROMLibraryDialog::setFolder(const QString& folder)
{
    QString target;
    if (!folder.trimmed().isEmpty())
        target = QFileInfo(folder).absoluteFilePath();

    const QString previousRoot = rootPath;
    if (!target.isEmpty() && rootValid && target == rootPath)
        return;

    // Reset every trace of the previous root together: an empty or invalid
    // root must never fall back to a computer/root view of the model, and
    // stale cached rows must never become current.
    rootPath = target;
    rootValid = false;
    rootLoaded = false;
    tree->setRootIndex(QModelIndex());
    if (tree->selectionModel())
        tree->selectionModel()->clearSelection();
    tree->setCurrentIndex(QModelIndex());
    openButton->setEnabled(false);
    LibraryProxy(proxy)->setRootValid(false);

    if (target.isEmpty())
    {
        refreshStatus(); // nothing selected yet
        return;
    }

    if (!QFileInfo(target).isDir())
    {
        refreshStatus(); // missing or not a directory
        return;
    }

    const QModelIndex sourceRoot = fsModel->setRootPath(target);

    rootValid = true;
    LibraryProxy(proxy)->setRootValid(true);

    const QModelIndex proxyRoot = proxy->mapFromSource(sourceRoot);
    if (!sourceRoot.isValid() || !proxyRoot.isValid())
    {
        rootValid = false;
        LibraryProxy(proxy)->setRootValid(false);
        refreshStatus();
        return;
    }

    tree->setRootIndex(proxyRoot);
    refreshStatus(); // stays "loading" until the current root is confirmed

    if (target != previousRoot)
        emit folderChanged(target);
}

void ROMLibraryDialog::chooseFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Choose ROM folder"), rootValid ? rootPath : QString());
    if (!dir.isEmpty())
        setFolder(dir);
}

void ROMLibraryDialog::applyNameFilter()
{
    LibraryProxy(proxy)->setNameFilter(filterEdit->text());
    updateOpenButton();
    refreshStatus();
}

void ROMLibraryDialog::onDirectoryLoaded(const QString& path)
{
    // The model also reports subdirectory expansions and loads queued by an
    // earlier root; only a completed load of the current root may update state.
    if (!rootValid || QDir::cleanPath(path) != rootPath)
        return;
    rootLoaded = true;
    refreshStatus();
}

void ROMLibraryDialog::updateOpenButton()
{
    const auto selection = tree->selectionModel()->selectedIndexes();
    openButton->setEnabled(!selection.isEmpty() && fileInfoFor(selection.first()).isFile());
}

void ROMLibraryDialog::openSelected()
{
    const auto selection = tree->selectionModel()->selectedIndexes();
    if (!selection.isEmpty())
        openActivated(selection.first());
}

void ROMLibraryDialog::openActivated(const QModelIndex& proxyIndex)
{
    const QFileInfo info = fileInfoFor(proxyIndex);
    if (!info.isFile())
        return; // directories navigate; they are never opened or booted

    // No bytes are read and no MIME is sniffed here: ROM preparation re-opens
    // and validates the current file, including archive member selection.
    emit openROM(info.absoluteFilePath());
    hide();
}

QFileInfo ROMLibraryDialog::fileInfoFor(const QModelIndex& proxyIndex) const
{
    if (!proxyIndex.isValid())
        return {};
    return fsModel->fileInfo(proxy->mapToSource(proxyIndex));
}

void ROMLibraryDialog::refreshStatus()
{
    updateFolderLabel();

    if (rootPath.isEmpty())
        statusLabel->setText(tr("No folder selected."));
    else if (!rootValid)
        statusLabel->setText(tr("Folder is not available."));
    else if (!rootLoaded)
        statusLabel->setText(tr("Loading folder…"));
    else
    {
        const int count = proxy->rowCount(tree->rootIndex());
        statusLabel->setText(count == 1 ? tr("1 item shown") : tr("%1 items shown").arg(count));
    }
}

void ROMLibraryDialog::updateFolderLabel()
{
    const QString text = rootPath.isEmpty()
        ? tr("No folder selected")
        : QDir::toNativeSeparators(rootPath);
    const int width = qMax(120, folderLabel->width());
    folderLabel->setText(folderLabel->fontMetrics().elidedText(text, Qt::ElideMiddle, width));
}

void ROMLibraryDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    updateFolderLabel();
}
