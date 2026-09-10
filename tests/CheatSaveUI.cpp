// SPDX-License-Identifier: GPL-3.0-or-later
// The production dialog, parser, and publication methods use generated files.
// Only storage faults and the paused emulator/message acknowledgement are supplied
// here. QSaveFile still creates and commits real temporary files; this is not a
// physical disk-failure or game-code execution test.
#include <QtWidgets>
#include <QSaveFile>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include "ARCodeFile.h"
#include "ARDatabaseDAT.h"
#include "CRC32.h"
#include "NDS_Header.h"
#include "Platform.h"
#include "ui_CheatsDialog.h"
#define private public
#include "CheatsDialog.h"
#undef private

using namespace melonDS;
static int failures = 0, faultHits = 0;
static QString fixtureRoot;
static void Check(bool ok, const char* message)
{
    if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
}
static void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
enum class Fault { None, Open, Short, Commit };
static Fault fault = Fault::None;
class CheatFile : public QFile
{
public:
    using QFile::QFile;
    bool open(OpenMode mode) override
    {
        if ((mode & WriteOnly) && fault == Fault::Open) { ++faultHits; return false; }
        return QFile::open(mode);
    }
    qint64 write(const char* data, qint64 size)
    {
        if (fault == Fault::Short && size) { ++faultHits; --size; }
        return QFile::write(data, size);
    }
    qint64 write(const QByteArray& bytes) { return write(bytes.constData(), bytes.size()); }
};
class CheatSaveFile : public QSaveFile
{
public:
    using QSaveFile::QSaveFile;
    bool open(OpenMode mode) override
    {
        if (fault == Fault::Open) { ++faultHits; return false; }
        return QSaveFile::open(mode);
    }
    qint64 write(const char* data, qint64 size)
    {
        if (fault == Fault::Short && size) { ++faultHits; --size; }
        return QSaveFile::write(data, size);
    }
    bool commit()
    {
        if (fault == Fault::Commit) { ++faultHits; cancelWriting(); }
        return QSaveFile::commit();
    }
};
namespace melonDS::Platform
{
FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    const auto name = QString::fromStdString(path);
    Require(name.startsWith(fixtureRoot + '/'), "File access escaped generated fixture");
    auto file = std::make_unique<CheatFile>(name);
    const auto flags = static_cast<unsigned>(mode);
    auto qtmode = flags & static_cast<unsigned>(FileMode::Write) ? QIODevice::WriteOnly : QIODevice::ReadOnly;
    if (!file->open(qtmode)) return nullptr;
    return reinterpret_cast<FileHandle*>(file.release());
}
void Log(LogLevel, const char*, ...) {}
#define QFile CheatFile
#include "arCloseFile.inc"
#include "arIsEndOfFile.inc"
#include "arFileSeek.inc"
#include "arFilePosition.inc"
#include "arFileRead.inc"
#include "arFileLength.inc"
#include "cheatFileReadLine.inc"
#include "cheatFileWrite.inc"
#include "cheatFileWriteFormatted.inc"
#include "cheatFileFlush.inc"
#include "cheatFileExists.inc"
#undef QFile
}
struct GeneratedCart
{
    std::array<u8, 0x200> bytes{};
    NDSHeader header{};
    const u8* GetROM() const { return bytes.data(); }
    const NDSHeader& GetHeader() const { return header; }
};
struct GeneratedMachine
{
    struct { GeneratedCart cart; GeneratedCart* GetCart() { return &cart; } } NDSCartSlot;
    struct { std::vector<ARCode> Cheats; } AREngine;
};
class EmuInstance
{
public:
    std::unique_ptr<ARCodeFile> cheatFile;
    std::unique_ptr<GeneratedMachine> nds = std::make_unique<GeneratedMachine>();
    bool cheatsOn = true;
    GeneratedMachine* getNDS() { return nds.get(); }
    ARCodeFile* getCheatFile() { return cheatFile.get(); }
    void enableCheats(bool enable);
};
#include "cheatEnableCurrent.inc"
struct EmuThread
{
    EmuInstance& instance;
    int applications = 0, resumes = 0;
    std::vector<ARCode> atResume;
    void enableCheats(bool value) { ++applications; instance.enableCheats(value); }
    void emuUnpause() { ++resumes; atResume = instance.nds->AREngine.Cheats; }
};
class MainWindow : public QWidget
{
public:
    EmuInstance instance;
    EmuThread thread{instance};
    EmuThread* emuThread = &thread;
    struct { bool enabled = true; bool GetBool(const char*) const { return enabled; } } localCfg;
    EmuInstance* getEmuInstance() { return &instance; }
    void onCheatsDialogFinished(int res);
};
#include "cheatFinishedCurrent.inc"
static QString emuDirectory;
#define EMUINSTANCE_H
#define QSaveFile CheatSaveFile
#include "../src/frontend/qt_sdl/CheatsDialog.cpp"
#undef QSaveFile

static const QByteArray original = "ROOT\n\nCODE 1 Original\n02000400 000000A1\n\nCAT 0 Group\nCODE 0 Child\n02000800 00000000\nCODE 0 Sibling\n02000C00 00000000\n";
static void Write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    Require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Fixture creation failed");
}
static QByteArray Read(const QString& path)
{
    QFile file(path);
    Require(file.open(QIODevice::ReadOnly), "Fixture read failed");
    return file.readAll();
}
static ARCode& First(ARCodeFile& file) { return std::get<ARCode>(file.RootCat.Children.front()); }
static bool Parents(ARCodeCat& cat)
{
    for (auto& item : cat.Children)
    {
        if (auto* code = std::get_if<ARCode>(&item)) { if (code->Parent != &cat) return false; }
        else
        {
            auto& child = std::get<ARCodeCat>(item);
            if (child.Parent != &cat || !Parents(child)) return false;
        }
    }
    return true;
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try
    {
        Require(argc == 2, "Expected a case name");
        const QString name = QString::fromLocal8Bit(argv[1]);
        QTemporaryDir dir;
        Require(dir.isValid(), "Temporary directory unavailable");
        fixtureRoot = dir.path();
        const auto path = dir.filePath("generated.mch"), otherPath = dir.filePath("other.mch");
        Write(path, original);
        MainWindow window;
        window.localCfg.enabled = name != "disabled";
        window.instance.cheatFile = std::make_unique<ARCodeFile>(path.toStdString());
        Require(!window.instance.cheatFile->Error, "Valid fixture was not parsed");
        window.instance.enableCheats(window.localCfg.enabled);
        auto dialog = std::make_unique<CheatsDialog>(&window);
        dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        CheatsDialog::currentDlg = dialog.get();
        dialog->show();
        auto& draft = *dialog->codeFile;
        auto* list = dialog->findChild<QTreeView*>("tvCodeList");
        Require(list != nullptr, "Cheat list widget missing");
        list->selectionModel()->select(list->model()->index(0, 0), QItemSelectionModel::ClearAndSelect);
        dialog->on_btnEditCode_clicked();
        dialog->findChild<QLineEdit*>("txtItemName")->setText("Edited");
        dialog->findChild<QPlainTextEdit*>("txtCode")->setPlainText("02000400 000000B2");
        dialog->on_btnSaveCode_clicked();
        Check(First(draft).Name == "Edited" && First(draft).Code[1] == 0xB2, "Widget edit did not reach the draft");
        Check(First(*window.instance.cheatFile).Name == "Original", "Edits escaped the working copy before save");
        Check(draft.RootCat.Parent == nullptr && Parents(draft.RootCat), "Working-copy parent pointers reference another tree");
        if (name == "one-per-group")
        {
            auto* model = static_cast<QStandardItemModel*>(list->model());
            auto* group = model->item(1);
            group->child(0)->setCheckState(Qt::Checked);
            group->child(1)->setCheckState(Qt::Checked);
            list->selectionModel()->select(group->index(), QItemSelectionModel::ClearAndSelect);
            dialog->on_btnEditCode_clicked();
            dialog->findChild<QCheckBox*>("chkItemOption")->setChecked(true);
            dialog->on_btnSaveCode_clicked();
            Check(group->child(0)->checkState() == Qt::Checked && group->child(1)->checkState() == Qt::Unchecked,
                  "One-per-group edit did not keep the first enabled code in the model");
        }
        int finished = 0, result = -1;
        QObject::connect(dialog.get(), &QDialog::finished, &window, [&](int res) {
            ++finished; result = res; window.onCheatsDialogFinished(res);
        });
        if (name == "open") fault = Fault::Open;
        else if (name == "short") fault = Fault::Short;
        else if (name == "commit" || name == "retry" || name == "discard") fault = Fault::Commit;
        else Require(name == "normal" || name == "disabled" || name == "changed" || name == "invalid" || name == "one-per-group", "Unknown case");
        std::unique_ptr<ARCodeFile> retired;
        if (name == "changed")
        {
            Write(otherPath, original);
            retired = std::move(window.instance.cheatFile); // keep baseline's raw pointer valid
            window.instance.cheatFile = std::make_unique<ARCodeFile>(otherPath.toStdString());
        }
        if (name == "invalid") draft.Error = true;
        int prompts = 0;
        QTimer reply;
        QObject::connect(&reply, &QTimer::timeout, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box) return;
            ++prompts;
            auto choice = QMessageBox::Cancel;
            if (name == "retry" && prompts == 1) { fault = Fault::None; choice = QMessageBox::Retry; }
            if (name == "discard") choice = QMessageBox::Discard;
            auto* button = box->button(choice);
            Require(button != nullptr, "Save failure has no expected recovery action");
            button->click();
        });
        reply.start(5);
        dialog->done(QDialog::Rejected); // production Close button takes this path
        reply.stop();
        const bool saved = name == "normal" || name == "disabled" || name == "retry" || name == "one-per-group";
        const bool discarded = name == "discard";
        Check(finished == (saved || discarded ? 1 : 0), "Dialog closed after an unhandled save failure");
        Check(window.thread.resumes == finished, "Paused emulator resumed before the dialog finished");
        Check(window.thread.applications == (saved ? 1 : 0), "Runtime cheats were not published exactly after commit");
        Check(saved || prompts == 1, "Save rejection did not offer recovery");
        if (name == "retry") Check(prompts == 1, "Retry did not complete in one recovery action");
        if (name == "open" || name == "short" || name == "commit" || name == "retry" || name == "discard")
            Check(faultHits > 0, "Requested file fault did not run");
        if (saved)
        {
            Check(result == QDialog::Accepted && !dialog->isVisible(), "Successful commit did not finish as accepted");
            ARCodeFile loaded(path.toStdString());
            Check(!loaded.Error && First(loaded).Name == "Edited" && First(loaded).Code[1] == 0xB2,
                  "Committed file lost the edited cheat");
            Check(First(*window.instance.cheatFile).Name == "Edited", "Committed edits did not reach the active tree");
            const auto& runtime = window.thread.atResume;
            Check(window.localCfg.enabled ? runtime.size() == 3 && runtime[0].Code[1] == 0xB2 &&
                  runtime[1].Enabled == (name == "one-per-group") && !runtime[2].Enabled : runtime.empty(),
                  "Resume used stale cheats or enabled disabled cheats");
        }
        else
        {
            Check(Read(path) == original, "Rejected save changed the original file");
            Check(First(*window.instance.cheatFile).Name == "Original", "Rejected save changed active cheats");
            Check(First(draft).Name == "Edited", "Rejected save discarded the editable draft");
            Check(discarded ? result == QDialog::Rejected && !dialog->isVisible() : dialog->isVisible(),
                  "Cancel/discard did not retain/close the editor as requested");
            const auto& runtime = window.instance.nds->AREngine.Cheats;
            Check(runtime.size() == 3 && runtime[0].Code[1] == 0xA1 && !runtime[1].Enabled && !runtime[2].Enabled,
                  "Rejected save changed running cheats");
            if (name == "changed") Check(Read(otherPath) == original, "Old editor wrote the new game's file");
        }
        Check(window.instance.cheatFile->RootCat.Parent == nullptr && Parents(window.instance.cheatFile->RootCat),
              "Published tree has dangling parent pointers");
        Check((CheatsDialog::currentDlg == nullptr) == bool(finished), "Dialog singleton outlived/abandoned the live editor");
        CheatsDialog::closeDlg();
        std::printf("Cheat save %s: %d failures\n", argv[1], failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 2; }
}
