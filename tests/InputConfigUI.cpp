// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the production dialog, Qt focus/key routing, configuration storage,
// and current input handlers with SDL virtual joysticks. Only the emulation
// window/thread is replaced; identity, handles, input and config code are real.
#include "EmuInstance.h"
#include <QtWidgets>
#include <QtTest/QTest>
#include <array>
#include <cstdio>
#include <stdexcept>
#include "ui_InputConfigDialog.h"

using namespace melonDS;
struct InputState
{
    static const char* buttonNames[12];
    static const char* hotkeyNames[HK_MAX];
    int instanceID;
    Config::Table localCfg;
    std::array<int, 12> keyMapping{}, joyMapping{};
    std::array<int, HK_MAX> hkKeyMapping{}, hkJoyMapping{};
    std::atomic<u32> keyInputMask{0xFFF}, keyHotkeyMask{0};
    std::shared_ptr<SDL_mutex> joyMutex{SDL_CreateMutex(), SDL_DestroyMutex};
    int joystickID = -1;
    JoystickSelection joystickSelection;
    std::vector<SDL_JoystickID> joystickTopology;
    Uint32 joystickLastOpen = 0;
    SDL_Joystick* joystick = nullptr;
    SDL_GameController* controller = nullptr;
    bool hasRumble = false, hasAccelerometer = false, hasGyroscope = false, isRumbling = false;
    u32 joyInputMask = 0xFFF, inputMask = 0xFFF, joyHotkeyMask = 0;
    u32 hotkeyMask = 0, lastHotkeyMask = 0, hotkeyPress = 0, hotkeyRelease = 0;
    explicit InputState(int instance = 0) : instanceID(instance), localCfg(Config::GetLocalTable(instance)) {}
    ~InputState() { closeJoystick(); }
    Config::Table& getLocalConfig() { return localCfg; }
    int getInstanceID() { return instanceID; }
    SDL_Joystick* getJoystick() { return joystick; }
    std::shared_ptr<SDL_mutex> getJoyMutex() { return joyMutex; }
    void setJoystick(int id);
    void setJoystickSelection(const JoystickSelection& selection);
    JoystickSelection getJoystickSelection();
    void saveJoystickConfig();
    void openJoystick();
    void closeJoystick();
    bool joystickButtonDown(int val);
    void inputProcess();
    void inputRumbleStart(u32 len_ms);
    void inputRumbleStop();
    void inputLoadConfig();
    void inputDeInit();
    void onKeyPress(QKeyEvent* event);
    void onKeyRelease(QKeyEvent* event);
    void keyReleaseAll();
};
#define EmuInstance InputState
#include "inputButtonNames.inc"
;
#include "inputHotkeyNames.inc"
;
#include "inputLoadConfig.inc"
#include "joystickDeInit.inc"
#include "joystickSaveConfig.inc"
#include "joystickSet.inc"
#include "joystickRestore.inc"
#include "joystickGetSelection.inc"
#include "joystickOpen.inc"
#include "joystickClose.inc"
#include "joystickButton.inc"
#include "joystickProcess.inc"
#include "joystickRumbleStart.inc"
#include "joystickRumbleStop.inc"
#include "onKeyPress.inc"
#include "onKeyRelease.inc"
#include "keyReleaseAll.inc"
class InputWindow : public QWidget
{
public:
    InputState input;
    explicit InputWindow(int instance = 0) : input(instance) {}
    InputState* getEmuInstance() { return &input; }
};
#define MainWindow InputWindow
#include "../src/frontend/qt_sdl/InputConfig/InputConfigDialog.cpp"
#undef MainWindow
#undef EmuInstance

static QString configDirectory;
namespace melonDS::Platform
{
std::string GetLocalFilePath(const std::string& path)
{
    return (configDirectory + '/' + QString::fromStdString(path)).toStdString();
}
bool CheckFileWritable(const std::string&) { return true; }
bool FileExists(const std::string& path) { return QFileInfo::exists(QString::fromStdString(path)); }
// Legacy INI parsing uses Platform reads, whereas TOML uses the filesystem
// directly. Keep both on the generated temporary directory; the previous
// no-files host returned nullptr and never exercised the legacy reader.
struct FileHandle { QFile file; };
FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    if (mode != FileMode::ReadText || path.find_first_of("/\\") != std::string::npos) return nullptr;
    auto handle = std::make_unique<FileHandle>();
    handle->file.setFileName(QString::fromStdString(GetLocalFilePath(path)));
    if (!handle->file.open(QIODevice::ReadOnly | QIODevice::Text)) return nullptr;
    return handle.release();
}
bool CloseFile(FileHandle* handle) { delete handle; return true; }
bool IsEndOfFile(FileHandle* handle) { return handle->file.atEnd(); }
bool FileReadLine(char* data, int count, FileHandle* handle) { return handle->file.readLine(data, count) > 0; }
}
static void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
static void Pump()
{
    QApplication::processEvents();
}
struct VirtualPad
{
    SDL_JoystickID id = -1;
    int Index() const
    {
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_JoystickGetDeviceInstanceID(i) == id) return i;
        return -1;
    }
    void Attach()
    {
        const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 2, 2, 0);
        Require(index >= 0, "Virtual joystick fixture unavailable");
        id = SDL_JoystickGetDeviceInstanceID(index);
    }
    void Detach()
    {
        Require(Index() >= 0 && SDL_JoystickDetachVirtual(Index()) == 0, "Virtual detach failed");
    }
    ~VirtualPad() { if (Index() >= 0) SDL_JoystickDetachVirtual(Index()); }
};
static void Capture(InputConfigDialog& dialog, KeyMapButton* button, Qt::Key key,
                    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    Require(button && button->isVisible(), "Mapping button is missing or hidden");
    QTest::mouseClick(button, Qt::LeftButton);
    Pump();
    Require(button->isChecked() && button->hasFocus(), "Click did not start focused key capture");
    QTest::keyClick(dialog.windowHandle(), key, modifiers);
    Pump();
    Require(!button->isChecked(), "Key release restarted capture or the key was not captured");
    Require(dialog.isVisible(), "Mapping a key closed the settings dialog");
}

static std::unique_ptr<InputConfigDialog> OpenInputDialog(InputWindow& window)
{
    auto dialog = std::make_unique<InputConfigDialog>(&window);
    dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    dialog->open();
    Pump();
    return dialog;
}
static void Refresh(InputConfigDialog& dialog)
{
    Require(QMetaObject::invokeMethod(&dialog, "refreshJoysticks", Qt::DirectConnection), "Dialog refresh slot unavailable");
    Pump();
}
static void Select(InputConfigDialog& dialog, SDL_JoystickID device)
{
    auto* combo = dialog.findChild<QComboBox*>("cbxJoystick");
    Require(combo && combo->findData(device) >= 0, "Requested visible controller choice is missing");
    combo->setCurrentIndex(combo->findData(device));
    Pump();
}
static void Finish(InputConfigDialog& dialog, bool accept)
{
    auto* box = dialog.findChild<QDialogButtonBox*>("buttonBox");
    Require(box, "Dialog action buttons missing");
    QTest::mouseClick(box->button(accept ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel), Qt::LeftButton);
    Pump();
    Require(!dialog.isVisible(), "Dialog did not close");
}
static bool Owns(const InputState& input, const VirtualPad& pad)
{
    return input.joystick && SDL_JoystickInstanceID(input.joystick) == pad.id;
}
static bool SelectionScenario(const QString& name)
{
    if (!name.startsWith("selection-")) return false;
    VirtualPad first, second;
    if (name != "selection-no-device") first.Attach();
    if (name != "selection-no-device" && name != "selection-shutdown") second.Attach();
    if (name == "selection-malformed")
    {
        for (const QByteArray value : {QByteArray("7"), QByteArray("-50"), QByteArray("4294967296"), QByteArray("\"invalid\"")})
        {
            QFile file(configDirectory + "/melonDS.toml");
            Require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Generated config unavailable");
            file.write("[Instance0]\nJoystickID = " + value + "\n");
            file.close();
            Require(Config::Load(), "Generated index fixture failed to load");
            InputWindow window;
            window.input.inputLoadConfig();
            Require(!window.input.joystick, "Malformed saved index silently selected device zero");
            auto dialog = OpenInputDialog(window);
            auto* combo = dialog->findChild<QComboBox*>("cbxJoystick");
            Require(combo->currentText().contains("unavailable"), "Malformed selection not visibly unavailable");
            Finish(*dialog, true);
            Require(!window.input.joystick && window.input.localCfg.GetInt("JoystickID") != 0,
                    "Accepting keyboard settings silently repaired an invalid selection to zero");
        }
        return true;
    }
    if (name == "selection-migration")
    {
        QFile file(configDirectory + "/melonDS.ini");
        Require(file.open(QIODevice::WriteOnly), "Generated legacy INI unavailable");
        file.write("JoystickID=1\nKey_A=75\n");
        file.close();
        Require(Config::Load(), "Generated legacy INI migration failed");
    }
    {
        InputWindow window;
        if (name == "selection-reorder") window.input.localCfg.SetInt("JoystickID", second.Index());
        window.input.localCfg.SetInt("Joystick.A", 0);
        window.input.inputLoadConfig();
        auto dialog = OpenInputDialog(window);
        auto* combo = dialog->findChild<QComboBox*>("cbxJoystick");
        auto* status = dialog->findChild<QLabel*>("lblJoystickStatus");
        Require(combo && status && status->isVisible() && !status->text().isEmpty(), "Selection status is not visible");
        const auto screenshot = qEnvironmentVariable("MELONDS_INPUT_DIALOG_SCREENSHOT");
        if (!screenshot.isEmpty()) Require(dialog->grab().save(screenshot), "Selection screenshot failed");

        if (name == "selection-shutdown")
        {
            Require(!window.input.localCfg.GetBool("JoystickDevice.RequireSelection"), "Fixture did not start with one controller");
            second.Attach();
            window.input.inputProcess();
            Require(Owns(window.input, first), "Discovering a duplicate changed the live session assignment");
            Finish(*dialog, false);
            window.input.inputDeInit();
            Config::Save();
        }
        else if (name == "selection-no-device")
        {
            Require(!dialog->getJoystick() && combo->currentText().contains("unavailable"), "No-device state lost the missing selection");
            first.Attach();
            window.input.inputProcess();
            Refresh(*dialog);
            Require(!window.input.joystick && !dialog->getJoystick(), "An unresolved legacy index claimed a newly connected device");
            Select(*dialog, -1);
            Finish(*dialog, true);
            Require(window.input.localCfg.GetInt("JoystickID") == -1 && !window.input.joystick,
                    "Explicit no-controller selection was not saved");
        }
        else if (name == "selection-migration" || name == "selection-reorder")
        {
            Require(Owns(window.input, second), "Valid numeric selection did not migrate to the selected second device");
            Require(!window.input.localCfg.GetString("JoystickDevice.GUID").empty(), "Legacy selection was not migrated to identity");
            if (name == "selection-migration")
                Require(window.input.keyMapping[0] == Qt::Key_K, "Legacy migration lost the custom keyboard mapping");
            first.Detach();
            first.Attach(); // Opposite connection order; the selected device is now index zero.
            window.input.inputProcess();
            window.input.inputLoadConfig();
            Refresh(*dialog);
            Require(Owns(window.input, second) && combo->currentData().toInt() == second.id,
                    "Runtime reload or dialog refresh used the saved enumeration number after reorder");
            Finish(*dialog, true);
            Require(Owns(window.input, second), "Accepting an unchanged dialog retargeted the reordered selection");
        }
        else if (name == "selection-draft")
        {
            Require(Owns(window.input, first), "Initial first controller unavailable");
            Select(*dialog, second.id);
            Require(dialog->getJoystick() && SDL_JoystickInstanceID(dialog->getJoystick()) == second.id && Owns(window.input, first),
                    "Draft preview changed the live instance's assignment before OK");
            Require(window.input.localCfg.GetInt("JoystickID") == 0, "Draft preview changed saved assignment");
            Finish(*dialog, false);
            Require(Owns(window.input, first), "Cancel changed live assignment");
            dialog.reset();
            dialog = OpenInputDialog(window);
            Select(*dialog, second.id);
            Finish(*dialog, true);
            Require(Owns(window.input, second), "OK failed to apply the previewed identity");
            InputWindow other(1);
            other.input.inputLoadConfig();
            Require(!other.input.joystick && other.input.localCfg.GetInt("JoystickID") == -1,
                    "Creating another instance silently inherited the first instance's controller");
            auto shared = OpenInputDialog(other);
            Select(*shared, second.id);
            Finish(*shared, true);
            Require(Owns(other.input, second) && Owns(window.input, second), "Explicit shared assignment was forbidden");
            Require(SDL_JoystickSetVirtualButton(window.input.joystick, 0, SDL_PRESSED) == 0, "Virtual button failed");
            window.input.inputProcess();
            other.input.inputProcess();
            Require(!(window.input.inputMask & 1) && !(other.input.inputMask & 1), "Shared explicit assignment did not deliver input to both instances");
        }
        else if (name == "selection-ambiguous" || name == "selection-stale")
        {
            const auto original = first.id;
            if (name == "selection-stale")
            {
                second.Detach(); // Click its old row before the refresh timer sees removal.
                Select(*dialog, second.id);
                Finish(*dialog, true);
                Require(!window.input.joystick && window.input.getJoystickSelection().device.instance == second.id,
                        "Clicking a removed row silently kept/selected a different controller");
                return true;
            }
            InputWindow other(1);
            other.input.setJoystick(second.Index());
            other.input.saveJoystickConfig();
            other.input.inputLoadConfig();
            first.Detach();
            window.input.inputProcess();
            other.input.inputProcess();
            Refresh(*dialog);
            Require(!window.input.joystick && !dialog->getJoystick() && Owns(other.input, second) &&
                    combo->currentText().contains("reselection"), "Identical remaining controller was silently substituted");
            const auto imagePath = qEnvironmentVariable("MELONDS_INPUT_AMBIGUOUS_SCREENSHOT");
            if (!imagePath.isEmpty()) Require(dialog->grab().save(imagePath), "Ambiguous screenshot failed");
            Finish(*dialog, false);
            Require(window.input.getJoystickSelection().device.instance == original && !window.input.joystick,
                    "Cancel rebound the detached selection by its old index");
            dialog.reset();
            first.Attach();
            dialog = OpenInputDialog(window);
            Require(!dialog->getJoystick(), "Reattachment guessed between same-model devices");
            Select(*dialog, first.id);
            Finish(*dialog, true);
            Require(Owns(window.input, first) && Owns(other.input, second), "Explicit recovery disturbed the other instance");
        }
        else throw std::runtime_error("Unknown selection scenario");
    }
    if (name == "selection-shutdown")
    {
        second.Detach(); // The next launch cannot rediscover the earlier collision.
        Require(Config::Load(), "Shutdown configuration failed to reload");
        InputState restarted;
        restarted.inputLoadConfig();
        Require(restarted.getJoystickSelection().requireSelection,
                "Shutdown forgot the identity collision discovered after startup");
    }
    if (name == "selection-reorder" || name == "selection-migration")
    {
        // Fresh process: SDL session IDs are not meaningful persisted identity.
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"selection-readback", configDirectory});
        Require(child.waitForFinished(10000) && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                "Restart guessed a no-serial controller from the persisted GUID/index");
    }
    return true;
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) return 2;
    struct SDLShutdown { ~SDLShutdown() { SDL_Quit(); } } sdlShutdown;
    if (SDL_NumJoysticks() != 0)
    {
        std::fprintf(stderr, "SKIP: physical devices remain visible; no device will be opened\n");
        return 77;
    }
    try
    {
        if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "selection-readback")
        {
            configDirectory = QString::fromLocal8Bit(argv[2]);
            Require(Config::Load(), "Saved identity failed to reload");
            VirtualPad first, second;
            second.Attach(); first.Attach();
            InputWindow window;
            window.input.inputLoadConfig();
            auto dialog = OpenInputDialog(window);
            Require(!window.input.joystick && !dialog->getJoystick() &&
                    dialog->findChild<QComboBox*>("cbxJoystick")->currentText().contains("reselection"),
                    "Restart silently selected a same-model controller");
            return 0;
        }
        if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "readback")
        {
            configDirectory = QString::fromLocal8Bit(argv[2]);
            Require(Config::Load(), "Fresh process could not reload saved mappings");
            InputState input;
            input.inputLoadConfig();
            QKeyEvent press(QEvent::KeyPress, Qt::Key_K, Qt::NoModifier);
            input.onKeyPress(&press);
            Require(!(input.keyInputMask & 1), "Saved custom key did not work in a fresh process");
            std::puts("Fresh process applied the mapping saved by the dialog");
            return 0;
        }
        Require(argc == 2, "Expected a case name");
        const QString name = QString::fromLocal8Bit(argv[1]);
        QTemporaryDir directory;
        Require(directory.isValid(), "Temporary directory unavailable");
        configDirectory = directory.path();
        Require(Config::Load(), "Initial configuration load failed");
        if (SelectionScenario(name))
        {
            std::printf("Input dialog %s: SDL identity, visible status and configuration passed\n", argv[1]);
            return 0;
        }
        VirtualPad pad, other;
        if (name == "controller") pad.Attach();
        InputWindow window;
        if (name == "missing-selection")
        {
            pad.Attach();
            other.Attach();
            window.input.localCfg.SetInt("JoystickID", 7);
        }
        window.input.inputLoadConfig();
        window.show();
        auto dialog = std::make_unique<InputConfigDialog>(&window);
        dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        dialog->open();
        Pump();
        if (name == "missing-selection")
        {
            auto* combo = dialog->findChild<QComboBox*>("cbxJoystick");
            Require(combo && combo->currentText().contains("unavailable", Qt::CaseInsensitive),
                    "Missing selection is blank instead of visibly unavailable");
            dialog->accept();
            Require(window.input.localCfg.GetInt("JoystickID") == 7,
                    "Opening and accepting the dialog replaced the missing saved index");
            return 0;
        }
        const auto screenshot = qEnvironmentVariable("MELONDS_INPUT_DIALOG_SCREENSHOT");
        if (!screenshot.isEmpty()) Require(dialog->grab().save(screenshot), "Dialog image could not be saved");
        auto* group = dialog->findChild<QGroupBox*>("grp_A");
        Require(group, "A button group missing");
        auto* button = group->findChild<KeyMapButton*>();
        Qt::Key key = Qt::Key_K;
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
        int expected = key;
        bool hotkey = name == "hotkey";
        if (name == "space") expected = key = Qt::Key_Space;
        else if (name == "return") expected = key = Qt::Key_Return;
        else if (name == "tab") expected = key = Qt::Key_Tab;
        else if (name == "escape") { key = Qt::Key_Escape; expected = Qt::Key_X; }
        else if (name == "unbind") { key = Qt::Key_Backspace; expected = -1; }
        else if (hotkey)
        {
            auto* page = dialog->findChild<QWidget*>("tabHotkeysGeneral");
            auto* tabs = dialog->findChild<QTabWidget*>("tabWidget");
            Require(page && tabs, "Hotkey page missing");
            tabs->setCurrentWidget(page);
            button = page->findChild<KeyMapButton*>(); // Pause/resume, the first labelled row.
            key = Qt::Key_F;
            modifiers = Qt::ControlModifier;
            expected = static_cast<int>(key) | static_cast<int>(modifiers);
        }
        Pump();
        Capture(*dialog, button, key, modifiers);
        if (name == "controller")
        {
            auto* tabs = dialog->findChild<QTabWidget*>("tabsMapping");
            Require(tabs && tabs->currentWidget()->objectName() == "keyPage",
                    "A connected controller hid keyboard settings");
            QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier, tabs->tabBar()->tabRect(1).center());
            Pump();
            auto* joy = tabs->currentWidget()->findChild<JoyMapButton*>();
            Require(joy && joy->isVisible() && joy->text() == "None", "Joystick mappings are not independently accessible");
            QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier, tabs->tabBar()->tabRect(0).center());
            Pump();
            Require(button->isVisible() && button->text() == "K", "Switching mapping tabs lost the edited key");
        }
        Require(window.input.localCfg.GetInt("Keyboard.A") == Qt::Key_X,
                "Capturing a draft changed the live configuration before OK");
        auto* box = dialog->findChild<QDialogButtonBox*>("buttonBox");
        Require(box, "Dialog action buttons missing");
        QTest::mouseClick(box->button(name == "cancel" ? QDialogButtonBox::Cancel : QDialogButtonBox::Ok), Qt::LeftButton);
        Pump();
        Require(!dialog->isVisible(), "OK/Cancel did not close the dialog");
        if (name == "cancel") expected = Qt::Key_X;
        const char* configKey = hotkey ? "Keyboard.HK_Pause" : "Keyboard.A";
        Require(window.input.localCfg.GetInt(configKey) == expected, "Dialog did not save the captured mapping");
        Require((hotkey ? window.input.hkKeyMapping[HK_Pause] : window.input.keyMapping[0]) == expected,
                "OK did not apply the mapping to the running input handler");
        QKeyEvent press(QEvent::KeyPress, key, modifiers);
        window.input.onKeyPress(&press);
        if (hotkey) Require(window.input.keyHotkeyMask & (1 << HK_Pause), "Changed hotkey did not activate");
        else if (expected == key) Require(!(window.input.keyInputMask & 1), "Changed key did not press A");
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        window.input.onKeyRelease(&release);
        Require(window.input.keyInputMask == 0xFFF && window.input.keyHotkeyMask == 0, "Changed input remained held");
        if (!hotkey && expected != Qt::Key_X)
        {
            QKeyEvent oldKey(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier);
            window.input.onKeyPress(&oldKey);
            Require(window.input.keyInputMask & 1, "Old default binding is still active");
        }
        dialog.reset();
        if (name == "cancel") Config::Save();
        Require(Config::Load(), "Saved configuration did not reload");
        InputState restarted;
        restarted.inputLoadConfig();
        Require(restarted.localCfg.GetInt(configKey) == expected &&
                (hotkey ? restarted.hkKeyMapping[HK_Pause] : restarted.keyMapping[0]) == expected,
                "Captured mapping was lost after configuration reload");
        if (name == "controller")
        {
            QProcess child;
            child.start(QCoreApplication::applicationFilePath(), {"readback", configDirectory});
            Require(child.waitForFinished(10000) && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                    "Captured mapping did not work after process restart");
            std::printf("%s", child.readAllStandardOutput().constData());
        }
        std::printf("Input dialog %s: capture, save, runtime, reload passed\n", argv[1]);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Input dialog failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
