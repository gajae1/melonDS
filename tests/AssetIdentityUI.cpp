// SPDX-License-Identifier: GPL-3.0-or-later
// Actual Qt ownership dialogs and dispatch wrappers with generated files. The
// emulation message consumer is recorded, not a running emulator or device.
#include <atomic>
#include <list>
#include <optional>
#include <variant>
#include <stop_token>
#include <cstdio>
#include <QApplication>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include "EmuThread.h"

struct Settings
{
    QString directory;
    QString GetQString(const char*) const { return directory; }
};
class EmuInstance
{
public:
    Settings config;
    QString registry;
    AssetIdentity::Selection dsAssetPaths, gbaAssetPaths;
    Settings& getLocalConfig() { return config; }
    QString getAssetRegistryDirectory() const { return registry; }
    QWidget* getMainWindow() { return nullptr; }
};
static int failures = 0;
static std::vector<EmuThread::Message> messages;
static void Check(bool ok, const char* why)
{ if (!ok) { ++failures; std::fprintf(stderr, "%s\n", why); } }
void EmuThread::run() { emuReset(); }
void EmuThread::sendMessage(Message message)
{
    messages.push_back(message);
    msgResult = 1;
    msgError.clear();
}
void EmuThread::waitMessage(int) {}
#include "stateThreadConstructor.inc"
#include "assetPrepareUI.inc"
#include "assetBootUI.inc"
#include "assetInsertUI.inc"
#include "assetResetUI.inc"

static void Choose(const QString& label, bool mayAdopt)
{
    QTimer::singleShot(0, [label, mayAdopt] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        Check(QThread::currentThread() == QApplication::instance()->thread(), "Ownership dialog ran outside the UI thread");
        if (!dialog) { Check(false, "Expected ownership dialog was not shown"); return; }
        bool foundExisting = false;
        QAbstractButton* chosen = nullptr;
        for (auto* button : dialog->buttons())
        {
            foundExisting |= button->text() == "Use existing files";
            if (button->text() == label || (label == "Cancel" && dialog->standardButton(button) == QMessageBox::Cancel))
                chosen = button;
        }
        Check(foundExisting == mayAdopt, "Dialog offered an unsafe or missing ownership choice");
        Check(chosen != nullptr, "Requested ownership choice is missing");
        if (chosen) chosen->click(); else dialog->reject();
    });
}
static bool Write(const QString& path, const QByteArray& bytes)
{ QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (argc != 2) return 2;
    const QString mode = argv[1];
    QTemporaryDir temporary;
    QDir root(temporary.path());
    if (!temporary.isValid() || !root.mkpath("a") || !root.mkpath("b") || !root.mkpath("saves")) return 2;
    const QString a = root.filePath("a/game.nds"), b = root.filePath("b/game.nds");
    EmuInstance instance;
    instance.config.directory = root.filePath("saves");
    instance.registry = root.filePath("ownership");
    if (!Write(a, "source A") || !Write(b, "source B") ||
        !Write(instance.config.directory + "/game.sav", "legacy")) return 2;
    EmuThread thread(&instance);
    QString error;
    if (mode == "prepared-modal-cancel")
    {
        auto prepared = std::make_shared<ROMPreparation::Data>();
        prepared->Source = {a};
        std::stop_source stop;
        prepared->Stop = stop.get_token();
        bool visited = false;
        QTimer::singleShot(0, [&] {
            auto* conflict = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            Check(conflict != nullptr, "Cancellation missed the real save-conflict dialog");
            visited = conflict != nullptr;
            stop.request_stop();
            // Even an affirmative click already queued by the user must not
            // write ownership or dispatch after this result was invalidated.
            if (conflict)
                for (auto* button : conflict->buttons())
                    if (button->text() == "Use existing files") { button->click(); break; }
        });
        const bool accepted = thread.bootROM({a}, error, prepared);
        Check(visited && !accepted && error.isEmpty() && messages.empty(), "Invalidated modal result dispatched or raised an error");
        Check(QDir(instance.registry).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty(), "Cancelled modal wrote ownership metadata");
        QFile legacy(root.filePath("saves/game.sav"));
        Check(legacy.open(QIODevice::ReadOnly) && legacy.readAll() == "legacy", "Cancelled modal changed save bytes");
        std::printf("Prepared result cancelled during actual ownership chooser: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    Choose(mode == "cancel" ? "Cancel" : mode == "separate" ? "Use separate files" : "Use existing files", true);
    const auto accepted = thread.bootROM({a}, error);
    if (mode == "cancel")
    {
        Check(!accepted && error.isEmpty() && messages.empty(), "Cancel dispatched a ROM/reset or became an error popup");
    }
    else
    {
        Check(accepted && messages.size() == 2 && messages[0].type == EmuThread::msg_BootROM &&
              messages[1].type == EmuThread::msg_EmuRun, "Chosen assets were not sent before running");
        if (messages.size() != 2) return 1;
        const auto request = messages[0].param.value<EmuThread::CartLoadRequest>();
        Check(request.Files == QStringList{a} && request.Assets.Valid() &&
              (request.Assets.Name == "game") == (mode != "separate"), "Load request lost the user's choice");
        instance.dsAssetPaths = request.Assets;
        messages.clear();
        if (mode == "other")
        {
            Choose("Use separate files", false);
            Check(thread.insertCart({b}, false, error) && messages.size() == 1 &&
                  messages[0].param.value<EmuThread::CartLoadRequest>().Assets.Name != "game",
                  "Another source could reuse the first source's name");
        }
        else if (mode == "reset" || mode == "worker-reset")
        {
            root.mkpath("new-path");
            instance.config.directory = root.filePath("new-path");
            Write(instance.config.directory + "/game.sav", "unassigned data");
            Choose("Cancel", false);
            thread.emuReset();
            Check(messages.empty() && instance.dsAssetPaths.Name == "game", "Canceled relocation sent a reset");
            if (mode == "worker-reset")
            {
                thread.start();
                Check(thread.wait(2000), "Hotkey reset blocked the worker on a UI choice");
                Choose("Use separate files", false);
                QCoreApplication::processEvents();
            }
            else
            {
                Choose("Use separate files", false);
                thread.emuReset();
            }
            Check(messages.size() == 1 && messages[0].type == EmuThread::msg_EmuReset &&
                  messages[0].param.value<EmuThread::AssetResetRequest>().DS.Name != "game",
                  "Reset lost the selected separate path");
        }
        else if (mode == "existing" || mode == "separate")
        {
            Check(thread.bootROM({a}, error) && messages.size() == 2 &&
                  messages[0].param.value<EmuThread::CartLoadRequest>().Assets.Name == request.Assets.Name,
                  "Reopen did not reuse the approved selection");
            QFile::remove(a);
            messages.clear();
            thread.emuReset();
            Check(messages.size() == 1 && messages[0].type == EmuThread::msg_EmuReset,
                  "Unchanged-path reset required reloading the source file");
        }
        else return 2;
    }
    QFile legacy(root.filePath("saves/game.sav"));
    Check(legacy.open(QIODevice::ReadOnly) && legacy.readAll() == "legacy", "The dialog changed existing save contents");
    std::printf("Asset identity UI %s: %d failures\n", argv[1], failures);
    return failures ? 1 : 0;
}
