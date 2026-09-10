// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the production dialog, Qt focus/key routing, configuration storage,
// and current input handlers. Only the emulation window and joystick are replaced.
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
    Config::Table localCfg = Config::GetLocalTable(0);
    std::array<int, 12> keyMapping{}, joyMapping{};
    std::array<int, HK_MAX> hkKeyMapping{}, hkJoyMapping{};
    std::atomic<u32> keyInputMask{0xFFF}, keyHotkeyMask{0};
    std::shared_ptr<SDL_mutex> joyMutex{SDL_CreateMutex(), SDL_DestroyMutex};
    Config::Table& getLocalConfig() { return localCfg; }
    int getInstanceID() { return 0; }
    SDL_Joystick* getJoystick() { return nullptr; }
    std::shared_ptr<SDL_mutex> getJoyMutex() { return joyMutex; }
    void setJoystick(int) {}
    void inputLoadConfig();
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
#include "onKeyPress.inc"
#include "onKeyRelease.inc"
#include "keyReleaseAll.inc"
class InputWindow : public QWidget
{
public:
    InputState input;
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
    int index = -1;
    void Attach()
    {
        Require(SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0, "Joystick fixture init failed");
        index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 2, 2, 0);
        Require(index >= 0, "Virtual joystick fixture unavailable");
    }
    ~VirtualPad()
    {
        if (index >= 0)
        {
            SDL_JoystickDetachVirtual(index);
            SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
        }
    }
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
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try
    {
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
        VirtualPad pad;
        if (name == "controller") pad.Attach();
        InputWindow window;
        window.input.inputLoadConfig();
        window.show();
        auto dialog = std::make_unique<InputConfigDialog>(&window);
        dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        dialog->open();
        Pump();
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
