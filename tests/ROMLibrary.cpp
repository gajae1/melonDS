// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <functional>
#include <QApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QLineEdit>
#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeView>
#include "ROMLibraryDialog.h"

static bool Until(const std::function<bool()>& predicate)
{
    QDeadlineTimer deadline(5000);
    do
    {
        QApplication::processEvents();
        if (predicate()) return true;
        QThread::msleep(10);
    } while (!deadline.hasExpired());
    return false;
}

static bool Write(const QString& path, const QByteArray& bytes = "fixture")
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

static QModelIndex Find(QTreeView* tree, const QString& name)
{
    if (!tree->model()) return {};
    for (int i = 0; i < tree->model()->rowCount(tree->rootIndex()); ++i)
    {
        auto item = tree->model()->index(i, 0, tree->rootIndex());
        if (item.data().toString() == name) return item;
    }
    return {};
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QTemporaryDir temporary(QDir::current().filePath("rom-library-XXXXXX"));
    if (!temporary.isValid()) return 2;
    QDir root(temporary.path());
    if (!root.mkdir("first") || !root.mkdir("second")) return 2;
    const QString first = root.filePath("first"), second = root.filePath("second");
    for (const QString& name : {QString("Alpha.nds"), QString::fromUtf8("가나다.NDS"),
         QString("Compressed.dsi.zst"), QString("archive.zip"), QString("ignore.sav"), QString("gba.gba")})
        if (!Write(QDir(first).filePath(name))) return 2;
    if (!QDir(first).mkdir("subfolder") || !Write(QDir(second).filePath("Other.srl"))) return 2;

    ROMLibraryDialog dialog("");
    auto* tree = dialog.findChild<QTreeView*>("romLibraryTree");
    auto* filter = dialog.findChild<QLineEdit*>("romLibraryFilter");
    auto* open = dialog.findChild<QPushButton*>("romLibraryOpen");
    if (!tree || !filter || !open) return 2;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };
    dialog.show();
    QApplication::processEvents();
    check(!open->isEnabled() && (!tree->model() || tree->model()->rowCount(tree->rootIndex()) == 0),
          "Empty library exposed files without a chosen folder");
    dialog.setFolder(first);
    check(Until([&] { return Find(tree, "Alpha.nds").isValid() && Find(tree, "Compressed.dsi.zst").isValid(); }),
          "Selected folder did not populate supported ROMs");
    check(Find(tree, QString::fromUtf8("가나다.NDS")).isValid() && Find(tree, "subfolder").isValid(),
          "Unicode/uppercase ROM or child folder missing");
    check(!Find(tree, "ignore.sav").isValid() && !Find(tree, "gba.gba").isValid(),
          "DS library offered an unsupported file");
#ifdef ARCHIVE_SUPPORT_ENABLED
    check(Find(tree, "archive.zip").isValid(), "Supported archive missing");
#else
    check(!Find(tree, "archive.zip").isValid(), "Archive offered without archive support");
#endif
    filter->setText("ALP");
    check(Until([&] { return Find(tree, "Alpha.nds").isValid() && !Find(tree, "Compressed.dsi.zst").isValid(); }),
          "File-name filter lost the root or ignored case");
    int opens = 0;
    QString opened;
    QObject::connect(&dialog, &ROMLibraryDialog::openROM, [&](const QString& path) { opened = path; ++opens; });
    tree->setCurrentIndex(Find(tree, "subfolder"));
    check(!open->isEnabled(), "Directory was offered as a ROM");
    tree->setCurrentIndex(Find(tree, "Alpha.nds"));
    check(open->isEnabled(), "Selected ROM cannot be opened");
    open->click();
    check(opens == 1 && opened == QDir(first).filePath("Alpha.nds"), "Open lost the full file identity");

    dialog.show();
    tree->setFocus();
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(tree, &enter);
    check(opens == 2, "Enter did not open exactly once");
    dialog.show();
    QApplication::processEvents();
    const QPoint point = tree->visualRect(Find(tree, "Alpha.nds")).center();
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
                           QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease})
    {
        QMouseEvent mouse(type, QPointF(point), QPointF(tree->viewport()->mapToGlobal(point)),
                          Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &mouse);
    }
    check(opens == 3, "Double click did not open exactly once");

    dialog.show();
    filter->clear();
    if (!QFile::remove(QDir(first).filePath("Alpha.nds")) || !Write(QDir(first).filePath("Added.nds"))) return 2;
    check(Until([&] { return !Find(tree, "Alpha.nds").isValid() && Find(tree, "Added.nds").isValid(); }),
          "File-system changes left stale library entries");
    check(!open->isEnabled(), "Deleted selection remained openable");

    // Change folders before old asynchronous updates can be applied to the view.
    dialog.setFolder(first);
    dialog.setFolder(second);
    check(!open->isEnabled(), "Folder change retained an old selection");
    check(Until([&] { return Find(tree, "Other.srl").isValid(); }) && !Find(tree, "Added.nds").isValid(),
          "Late old-folder contents replaced the chosen folder");
    tree->setCurrentIndex(Find(tree, "Other.srl"));
    open->click();
    check(opens == 4 && opened == QDir(second).filePath("Other.srl"), "Folder change opened stale file identity");
    dialog.show();
    if (!QFile::remove(QDir(second).filePath("Other.srl")) || !root.rmdir("second")) return 2;
    check(Until([&] { return tree->model()->rowCount(tree->rootIndex()) == 0 && !open->isEnabled(); }),
          "Deleted root exposed another folder or retained an openable selection");
    auto* status = dialog.findChild<QLabel*>("romLibraryStatus");
    check(Until([&] { return status && status->text() == "Folder is not available."; }),
          "Deleted root was not invalidated before recreation");

    // Re-select the identical path, without visiting a different root first.
    // Qt may still cache the deleted directory and skip loading it again.
    const QString recreatedROM = QDir(second).filePath("Recreated.nds");
    const QByteArray recreatedBytes("new ROM bytes after root recreation");
    if (!root.mkdir("second") || !Write(recreatedROM, recreatedBytes)) return 2;
    dialog.setFolder(second);
    check(!open->isEnabled() && !tree->currentIndex().isValid() && !Find(tree, "Other.srl").isValid(),
          "Recreated root exposed the old selection or cached ROM");
    check(tree->isColumnHidden(2) && !tree->isColumnHidden(0) &&
          !tree->isColumnHidden(1) && !tree->isColumnHidden(3),
          "Recreated root lost the library column layout");
    check(Until([&] {
        return tree->rootIndex().isValid() && Find(tree, "Recreated.nds").isValid() &&
               tree->model()->rowCount(tree->rootIndex()) == 1 && status &&
               status->text() == "1 item shown";
    }), "Re-selecting the recreated root did not load its new ROM");
    tree->setCurrentIndex(Find(tree, "Recreated.nds"));
    check(open->isEnabled(), "Recreated root ROM cannot be opened");
    open->click();
    check(opens == 5 && opened == recreatedROM && !dialog.isVisible(),
          "Recreated root did not open the new absolute file path exactly once");
    QFile reopened(opened);
    check(reopened.open(QIODevice::ReadOnly) && reopened.readAll() == recreatedBytes,
          "Recreated root opened stale bytes");
    reopened.close();
    dialog.show();

    if (!QFile::remove(recreatedROM) || !root.rmdir("second")) return 2;
    check(Until([&] { return tree->model()->rowCount(tree->rootIndex()) == 0 && !open->isEnabled(); }),
          "Recreated root deletion retained files or an openable selection");

    dialog.setFolder(first);
    check(Until([&] { return Find(tree, "Added.nds").isValid() && status && !status->text().contains("Loading"); }),
          "Returning to an already loaded folder remained stuck loading");
    dialog.setFolder(root.filePath("missing"));
    check(!open->isEnabled() && (!tree->isEnabled() || !Find(tree, "Other.srl").isValid()),
          "Missing folder silently fell back to cached files");
    dialog.close();
    QApplication::processEvents();
    check(opens == 5, "Closing the library launched a file");
    std::printf("ROM library: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
