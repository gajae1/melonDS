// SPDX-License-Identifier: GPL-3.0-or-later
// The close handler is extracted from the current Window.cpp at build time.
// QMainWindow, message/file dialogs, close events and temporary file commits are
// real Qt objects. EmuInstance/EmuThread and the planned SaveManager contract are
// fixtures: they do not prove worker durability or producer acknowledgement.
// Those integration boundaries are covered separately by SaveManagerIO and the
// frontend build. No production SaveManager API is required for the baseline red.
// SD cases reuse the full current FATStorage/FatFs and Qt file I/O fixture.
#include <QApplication>
#include <QAbstractButton>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define FAT_STORAGE_IO_ONLY
#include "FATStorageLifecycle.cpp"
#undef FAT_STORAGE_IO_ONLY

namespace Config
{
int saves = 0;
bool SaveWithDialog(QWidget*) { ++saves; return true; }
struct Table
{
    void SetString(const std::string&, const std::string&) {}
};
}

struct EmuThread
{
    int depth = 0, pauses = 0, unpauses = 0;
    bool broadcastUsed = false;
    bool deinitContext(int) { return true; }
    bool emuIsRunning() { return depth == 0; }
    void initContext(int) {}
    void emuPause(bool broadcast = true)
    {
        ++depth;
        ++pauses;
        broadcastUsed |= broadcast;
    }
    void emuUnpause(bool broadcast = true)
    {
        --depth;
        ++unpauses;
        broadcastUsed |= broadcast;
    }
};

class SaveManager
{
public:
    SaveManager(QString path, EmuThread& producer) : path(std::move(path)), producer(producer) {}
    QByteArray latest = "latest pending save";
    bool pending = true, failFlush = false, failCopy = false;
    bool usedWhileRunning = false;
    int flushes = 0, copies = 0;

    bool NeedsFlush() { return pending; }
    std::string GetPath() { return path.toStdString(); }
    bool Flush()
    {
        ++flushes;
        usedWhileRunning |= producer.depth <= 0;
        if (!pending) return true;
        if (failFlush || !write(path)) return false;
        pending = false;
        return true;
    }
    bool SaveCopy(const std::string& destination)
    {
        ++copies;
        usedWhileRunning |= producer.depth <= 0;
        return !failCopy && write(QString::fromStdString(destination));
    }

private:
    bool write(const QString& destination)
    {
        QSaveFile file(destination);
        return file.open(QIODevice::WriteOnly) && file.write(latest) == latest.size() && file.commit();
    }
    const QString path;
    EmuThread& producer;
};

class MainWindow;
const int kMaxWindows = 4;
struct EmuInstance
{
    EmuThread thread;
    bool deleting = false, destroyed = false;
    int deletions = 0, enabledWrites = 0;
    int keyReleases = 0;
    void keyReleaseAll() { ++keyReleases; }
    std::array<MainWindow*, kMaxWindows> windows{};
    MainWindow* mainWindow = nullptr;
    std::unique_ptr<SaveManager> ndsSave, gbaSave, firmwareSave;
    std::array<std::unique_ptr<melonDS::FATStorage>, 2> sdCards;
    std::array<melonDS::FATStorage*, 2> getSDCards() { return {sdCards[0].get(), sdCards[1].get()}; }

    EmuThread* getEmuThread() { return &thread; }
    MainWindow* getMainWindow() { return mainWindow; }
    MainWindow* getWindow(int id) { return windows[id]; }
    int getNumWindows() { return std::count_if(windows.begin(), windows.end(), [](auto* w) { return w; }); }
    void saveEnabledWindows() { ++enabledWrites; }
    bool deleteWindow(int id, bool)
    {
        ++deletions;
        if (mainWindow == windows[id]) mainWindow = nullptr;
        windows[id] = nullptr;
        if (!getNumWindows()) deleting = destroyed = true;
        // Keep the fixture alive for assertions after the real QWidget closes.
        return true;
    }
};

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(int id, EmuInstance* instance, QWidget* parent = nullptr) :
        QMainWindow(parent, Qt::Window), windowID(id), emuInstance(instance), emuThread(instance->getEmuThread())
    {
        instance->windows[id] = this;
        if (!instance->mainWindow) instance->mainWindow = this;
        setAttribute(Qt::WA_DeleteOnClose);
    }
    EmuInstance* getEmuInstance() { return emuInstance; }
    int getWindowID() { return windowID; }
    void saveEnabled(bool) { ++enabledWrites; }
    int enabledWrites = 0;
    void onAppStateChanged(Qt::ApplicationState state);

private:
    void closeEvent(QCloseEvent* event) override;
    bool prepareClose();
    bool flushSaveManagers(EmuInstance* instance);
    bool closeInProgress = false;
    bool closeApproved = false;
    bool hasOGL = false;
    bool pauseOnLostFocus = true, pausedManually = false;
    struct Panel { void releaseTouch() {} };
    Panel* panel = nullptr;
    int windowID;
    EmuInstance* emuInstance;
    EmuThread* emuThread;
    Config::Table windowCfg;
};

#include "closeSaveManagers.inc"
#include "prepareClose.inc"
#include "closeEvent.inc"
#include "closeAppState.inc"

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (argc != 2) return 2;
    const std::string argument = argv[1];
    const bool sd = argument.starts_with("sd-");
    const std::string scenario = sd ? argument.substr(3) : argument;
    if (scenario == "app-state")
    {
        EmuInstance instance;
        MainWindow window(0, &instance);
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        window.onAppStateChanged(Qt::ApplicationInactive);
        window.onAppStateChanged(Qt::ApplicationActive);
        if (instance.keyReleases != 1 || instance.thread.unpauses != 1 || !window.close()) return 1;
        const int pauses = instance.thread.pauses;
        // Qt may deliver this notification after close, before deferred deletion.
        window.onAppStateChanged(Qt::ApplicationInactive);
        window.onAppStateChanged(Qt::ApplicationActive);
        const bool passed = instance.destroyed && instance.keyReleases == 1 &&
                            instance.thread.pauses == pauses && instance.thread.unpauses == 1;
        std::printf("closed-window application-state delivery: %s\n", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
    const bool childCancel = scenario == "child-cancel" || (sd && scenario == "child-retry");
    const bool secondary = scenario == "secondary";
    const bool clean = scenario == "clean" || (sd && scenario == "readonly");
    const bool retry = scenario == "retry" || (sd && scenario == "child-retry");
    const bool recovery = scenario == "recovery" || scenario == "recovery-cancel" || scenario == "recovery-failure";
    const bool accepts = clean || retry || scenario == "recovery";
    if (!childCancel && !secondary && !accepts && !recovery &&
        scenario != "cancel-ds" && scenario != "cancel-gba" && scenario != "cancel-firmware" && !(sd && scenario == "cancel-dldi")) return 2;

    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };
    const auto read = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    EmuInstance parentInstance, childInstance;
    const QByteArray original = "original committed save";
    const auto seed = [&](EmuInstance& instance, const char* name) {
        const QString path = directory.filePath(QString::fromLatin1(name));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(original) != original.size()) return false;
        file.close();
        instance.ndsSave = std::make_unique<SaveManager>(path, instance.thread);
        return true;
    };
    if (!seed(parentInstance, "ds.sav") || !seed(childInstance, "child.sav")) return 2;
    for (auto entry : {std::pair{&parentInstance.gbaSave, "gba.sav"},
                       std::pair{&parentInstance.firmwareSave, "firmware.bin"}})
    {
        const QString path = directory.filePath(QString::fromLatin1(entry.second));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(original) != original.size()) return 2;
        file.close();
        *entry.first = std::make_unique<SaveManager>(path, parentInstance.thread);
    }
    SaveManager* target = childCancel ? childInstance.ndsSave.get() :
                          scenario == "cancel-gba" ? parentInstance.gbaSave.get() :
                          scenario == "cancel-firmware" ? parentInstance.firmwareSave.get() : parentInstance.ndsSave.get();
    target->failFlush = !clean;
    target->failCopy = scenario == "recovery-failure";
    const QString originalPath = QString::fromStdString(target->GetPath());
    const QString recoveryPath = directory.filePath("recovery.sav");
    if (clean)
        for (auto* manager : {parentInstance.ndsSave.get(), parentInstance.gbaSave.get(), parentInstance.firmwareSave.get()})
            manager->pending = false;
    FATStorage* card = nullptr;
    QString imagePath, hostPath, indexPath;
    QByteArray imageBefore, indexBefore;
    const auto guestPayload = ExportTest::Payload(12291);
    const QByteArray hostEdit("independent host edit"), copyOriginal("existing recovery destination");
    ExportTest::ReplacementLock copyLock;
    if (sd)
    {
        for (auto* save : {parentInstance.ndsSave.get(), parentInstance.gbaSave.get(),
                parentInstance.firmwareSave.get(), childInstance.ndsSave.get()})
        { save->pending = false; save->failFlush = false; }
        imagePath = directory.filePath("card.img");
        indexPath = imagePath + ".idx";
        const auto folder = directory.filePath("folder");
        Require(QDir().mkdir(folder), "generated SD folder");
        hostPath = folder + "/KEEP.BIN";
        WriteBytes(hostPath, original);
        auto& owner = childCancel ? childInstance : parentInstance;
        auto& slot = owner.sdCards[scenario == "cancel-dldi" ? 1 : 0];
        slot = std::make_unique<FATStorage>(imagePath.toStdString(), Capacity,
            scenario == "readonly", folder.toStdString());
        card = slot.get();
        Require(card->IsValid(), "generated SD image");
        indexBefore = read(indexPath);
        if (!clean)
        {
            ff_disk_open([&](BYTE* data, LBA_t start, UINT count) { return card->ReadSectors(start, count, data); },
                [&](const BYTE* data, LBA_t start, UINT count) { return card->WriteSectors(start, count, data); }, card->GetSectorCount());
            FATFS volume;
            Require(f_mount(&volume, "0:", 1) == FR_OK && f_unlink("0:/KEEP.BIN") == FR_OK && f_unmount("0:") == FR_OK,
                "actual guest deletion before close");
            ff_disk_close();
            Require(card->InjectFile("NEW.BIN", reinterpret_cast<u8*>(const_cast<char*>(guestPayload.constData())), guestPayload.size()),
                "pending guest data for recovery image");
            WriteBytes(hostPath, hostEdit);
        }
        imageBefore = read(imagePath);
        if (scenario == "recovery-failure")
        {
            WriteBytes(recoveryPath, copyOriginal);
            copyLock.Lock(recoveryPath);
        }
    }
    // A cancelled close must undo only its own pause, including a prior pause.
    parentInstance.thread.depth = scenario == "cancel-ds" ? 1 : 0;
    const int previousDepth = parentInstance.thread.depth;

    QPointer<MainWindow> root = new MainWindow(0, &parentInstance);
    QPointer<MainWindow> extra;
    QPointer<MainWindow> child;
    QPointer<MainWindow> grandchild;
    if (childCancel || secondary) extra = new MainWindow(1, &parentInstance, root);
    if (childCancel)
    {
        child = new MainWindow(0, &childInstance, root);
        grandchild = new MainWindow(1, &childInstance, child);
    }
    for (const auto& window : {root, extra, child, grandchild})
        if (window) window->show();
    QCoreApplication::processEvents();

    int prompts = 0, fileDialogs = 0;
    QPointer<QFileDialog> handledFileDialog;
    bool timedOut = false, allProducersPaused = true;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer actor;
    QObject::connect(&actor, &QTimer::timeout, [&] {
        timedOut |= elapsed.elapsed() > 3000;
        auto* modal = QApplication::activeModalWidget();
        if (auto* box = qobject_cast<QMessageBox*>(modal))
        {
            check(!(box->standardButtons() & QMessageBox::Discard), "Close offered an unconditional discard action");
            auto* cancel = box->button(QMessageBox::Cancel);
            if (!cancel)
            {
                if (auto* yes = box->button(QMessageBox::Yes)) yes->click();
                else box->accept();
                return;
            }
            ++prompts;
            if (sd) check(box->text().contains("image", Qt::CaseInsensitive), "SD warning does not explain image recovery");
            allProducersPaused &= parentInstance.thread.depth > previousDepth;
            if (childCancel) allProducersPaused &= childInstance.thread.depth > 0;
            if (!timedOut && prompts == 1 && retry)
            {
                target->failFlush = false;
                if (sd) WriteBytes(hostPath, original);
                auto* button = box->button(QMessageBox::Retry);
                check(button != nullptr, "Save failure has no Retry action");
                (button ? button : cancel)->click();
            }
            else if (!timedOut && prompts == 1 && recovery)
            {
                auto* button = box->findChild<QPushButton*>("saveRecoveryCopy");
                check(button != nullptr, "Save failure has no recovery-copy action");
                if (button) button->click(); else cancel->click();
            }
            else cancel->click();
        }
        else if (auto* dialog = qobject_cast<QFileDialog*>(modal))
        {
            if (handledFileDialog == dialog) return;
            handledFileDialog = dialog;
            ++fileDialogs;
            check(dialog->testOption(QFileDialog::DontUseNativeDialog), "Recovery dialog did not explicitly disable native dialogs");
            if (timedOut || scenario == "recovery-cancel") dialog->reject();
            else
            {
                dialog->selectFile(recoveryPath);
                // Let this timer return before QFileDialog opens its nested
                // overwrite confirmation, so the actor can answer that too.
                check(QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection), "Could not accept the Qt recovery file dialog");
            }
        }
    });
    actor.start(1);
    const bool accepted = (secondary ? extra : root)->close();
    actor.stop();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    check(!timedOut, "Close recovery dialog did not complete the scripted selection");
    if (secondary)
    {
        check(accepted && root && !extra && !parentInstance.destroyed, "Closing a secondary view destroyed or blocked its instance");
        check(prompts == 0 && parentInstance.thread.pauses == 0 && target->pending,
              "Closing a surviving instance's secondary view changed pending data or paused it");
    }
    else if (!accepts)
    {
        check(!accepted && root && !parentInstance.destroyed && parentInstance.deletions == 0,
              "Close was accepted or a window was destroyed after save recovery was cancelled");
        check((sd || target->pending) && read(originalPath) == original, "Cancel lost pending data or changed the original save");
        check(Config::saves == 0 && parentInstance.enabledWrites == 0, "Cancel persisted closing-window configuration");
        check(parentInstance.thread.depth == previousDepth && parentInstance.thread.pauses == 1 && parentInstance.thread.unpauses == 1,
              "Cancel did not restore exactly the pre-close pause state");
        if (childCancel)
        {
            check(extra && child && grandchild && !childInstance.destroyed && childInstance.deletions == 0,
                  "A child-instance Cancel was ignored or other windows were already closed");
            check(childInstance.thread.depth == 0 && childInstance.thread.pauses == 1 && childInstance.thread.unpauses == 1,
                  "Batch cancellation did not release the child instance's pause exactly once");
        }
    }
    else
    {
        check(accepted && !root && parentInstance.destroyed, "Successful close did not destroy the final window/instance");
        if (childCancel) check(!extra && !child && !grandchild && childInstance.destroyed,
            "Successful retry did not close the complete instance subtree");
        if (retry && !sd) check(!target->pending && read(originalPath) == target->latest && target->flushes >= 2,
                         "Retry closed before the latest original save committed");
        if (scenario == "recovery" && !sd)
            check(target->pending && read(originalPath) == original && read(recoveryPath) == target->latest &&
                  QString::fromStdString(target->GetPath()) == originalPath,
                  "Recovery copy was missing or treated as a successful original save");
    }
    if (!clean && !secondary)
    {
        check(prompts >= 1, "Save failure did not offer the real Qt recovery choices");
        check(allProducersPaused && !target->usedWhileRunning, "Save recovery ran before every closing producer was paused");
    }
    if (recovery) check(fileDialogs == 1, "Recovery path selection did not use one real Qt file dialog");
    if (scenario == "recovery-cancel" && !sd) check(target->copies == 0, "Cancelling the file picker still wrote a recovery copy");
    if (scenario == "recovery-failure" && !sd) check(target->copies == 1 && read(recoveryPath).isEmpty(), "Failed recovery copy was accepted or left a file");
    check(!parentInstance.thread.broadcastUsed && !childInstance.thread.broadcastUsed, "Close pause changed unrelated instances through broadcast");
    if (sd)
    {
        check(card->IsValid(), "close preflight replaced the live SD storage");
        if (!clean && !retry)
            check(read(imagePath) == imageBefore && read(indexPath) == indexBefore && read(hostPath) == hostEdit,
                "SD cancel/recovery changed original image, index or conflicting host bytes");
        if (retry)
            check(!QFile::exists(hostPath) && read(QFileInfo(hostPath).dir().filePath("NEW.BIN")) == guestPayload,
                "same-live-card Retry did not finish folder synchronization");
        if (scenario == "recovery")
        {
            auto expectedImage = imageBefore;
            expectedImage.resize(Capacity, '\0');
            check(read(recoveryPath) == expectedImage, "recovery does not contain byte-exact logical guest image");
            FATStorage recovered(recoveryPath.toStdString(), 0, true);
            QByteArray actual(guestPayload.size(), '\0');
            check(recovered.IsValid() && recovered.GetSectorCount() == Capacity / 512 &&
                recovered.ReadFile("NEW.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size() && actual == guestPayload,
                "separate recovered image could not reload actual guest data");
        }
        if (scenario == "recovery-failure")
            check(read(recoveryPath) == copyOriginal && !accepted, "failed SD copy damaged destination or accepted close");
        if (scenario == "recovery-cancel")
            check(!QFile::exists(recoveryPath), "cancelled SD file picker wrote a recovery image");
    }
    delete root.data();
    std::printf("Frontend close %s: %d failures\n", argument.c_str(), failures);
    return failures ? 1 : 0;
}

#include "FrontendClose.moc"
