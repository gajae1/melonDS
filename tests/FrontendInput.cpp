// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QFile>
#include <array>
#include <cstdio>
#include "EmuInstance.h"
#include "InputConfig/KeyMapButton.h"

// Only host state is substituted. The event handlers are extracted from the
// current production source at build time, not copied into this test.
struct InputState
{
    std::array<int, 12> keyMapping;
    std::array<int, HK_MAX> hkKeyMapping;
    std::atomic<melonDS::u32> keyInputMask{0xFFF}, keyHotkeyMask{0};
    int muteUpdates = 0;
    InputState() { keyMapping.fill(-1); hkKeyMapping.fill(-1); }
    void onKeyPress(QKeyEvent* event);
    void onKeyRelease(QKeyEvent* event);
    void keyReleaseAll();
    void updateAudioMuteByWindowFocus() { ++muteUpdates; }
};
#define EmuInstance InputState
#include "onKeyPress.inc"
#include "onKeyRelease.inc"
#include "keyReleaseAll.inc"
#undef EmuInstance

struct WindowState
{
    InputState* emuInstance;
    bool focused = true;
    void onFocusOut();
};
#define MainWindow WindowState
#include "onFocusOut.inc"
#undef MainWindow

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

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    configDirectory = directory.path();
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };
    const auto mapKey = [](int& mapping, bool hotkey, QKeyEvent& event) {
        KeyMapButton button(&mapping, hotkey);
        button.click();
        QApplication::sendEvent(&button, &event);
    };

    check(Config::Load(), "First run without a config failed");
    auto cfg = Config::GetLocalTable(0).GetTable("Keyboard");
    check(cfg.GetInt("A") == Qt::Key_X && cfg.GetInt("Start") == Qt::Key_Return &&
          cfg.GetInt("Left") == Qt::Key_Left, "Fresh config has no usable keyboard defaults");
    cfg.SetInt("B", -1); // An explicit unbinding must survive defaults and reload.
    cfg.SetInt("A", Qt::Key_K);
    Config::Save();
    check(Config::Load(), "Config reload failed");
    check(Config::GetLocalTable(0).GetInt("Keyboard.A") == Qt::Key_K &&
          Config::GetLocalTable(0).GetInt("Keyboard.B") == -1,
          "Saved mapping or explicit unbinding was lost");

    InputState input;
    QKeyEvent shifted(QEvent::KeyPress, Qt::Key_X, Qt::ShiftModifier);
    mapKey(input.keyMapping[0], false, shifted);
    check(input.keyMapping[0] == Qt::Key_X, "Game button capture incorrectly stores Shift");
    input.onKeyPress(&shifted);
    check(!(input.keyInputMask & 1), "Captured game binding does not press A");
    QKeyEvent releaseX(QEvent::KeyRelease, Qt::Key_X, Qt::NoModifier);
    input.onKeyRelease(&releaseX);
    check(input.keyInputMask & 1, "A remains held after releasing Shift before X");

    QKeyEvent keypad(QEvent::KeyPress, Qt::Key_1, Qt::KeypadModifier | Qt::ShiftModifier);
    mapKey(input.keyMapping[1], false, keypad);
    check(input.keyMapping[1] == (Qt::Key_1 | Qt::KeypadModifier), "Keypad identity lost during capture");
    input.onKeyPress(&keypad);
    check(!(input.keyInputMask & 2), "Keypad binding does not press B while Shift is held");
    QKeyEvent releaseKeypad(QEvent::KeyRelease, Qt::Key_1, Qt::KeypadModifier);
    input.onKeyRelease(&releaseKeypad);
    check(input.keyInputMask & 2, "Keypad binding remains held");

    QKeyEvent shortcut(QEvent::KeyPress, Qt::Key_F, Qt::ControlModifier);
    mapKey(input.hkKeyMapping[HK_FastForward], true, shortcut);
    check(input.hkKeyMapping[HK_FastForward] == (Qt::Key_F | Qt::ControlModifier),
          "Hotkey capture lost its modifier");
    input.onKeyPress(&shortcut);
    check(input.keyHotkeyMask & (1 << HK_FastForward), "Modified hotkey did not start");
    QKeyEvent releaseF(QEvent::KeyRelease, Qt::Key_F, Qt::NoModifier);
    input.onKeyRelease(&releaseF);
    check(input.keyHotkeyMask == 0, "Hotkey remains held after releasing Ctrl before F");

    input.keyMapping[0] = Qt::Key_X;
    input.onKeyPress(&shifted);
    input.onKeyPress(&shortcut);
    WindowState window{&input};
    window.onFocusOut();
    check(input.keyInputMask == 0xFFF && input.keyHotkeyMask == 0 && !window.focused && input.muteUpdates == 1,
          "Window focus loss leaves keys held");
    window.emuInstance = nullptr;
    window.onFocusOut(); // Closing a detached window must remain safe.

    int mapping = Qt::Key_A;
    QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    mapKey(mapping, false, cancel);
    check(mapping == Qt::Key_A, "Escape did not cancel binding capture");
    QKeyEvent clear(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    mapKey(mapping, false, clear);
    check(mapping == -1, "Backspace did not clear binding");

    // A hand-edited nested table leaves its parent implicit. Adding ordinary
    // instance/DSi defaults must still produce a valid, reloadable file.
    QFile sparse(configDirectory + "/melonDS.toml");
    if (!sparse.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    sparse.write("[Instance0.Keyboard]\nA = 75\n[DSi.DSP]\nHLE = false\n");
    sparse.close();
    check(Config::Load(), "Sparse config could not be loaded");
    Config::GetLocalTable(0).GetInt("JoystickID");
    Config::GetGlobalTable().SetBool("DSi.ExternalBIOSEnable", true);
    try { Config::Save(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Saving sparse config threw: %s\n", error.what());
        return 1;
    }
    check(Config::Load() && Config::GetLocalTable(0).GetInt("Keyboard.A") == 75 &&
          Config::GetGlobalTable().GetBool("DSi.ExternalBIOSEnable"),
          "Saving defaults under implicit parent tables lost user settings");

    // A failed load must not let defaults or later settings changes overwrite
    // the malformed file when the frontend saves during normal shutdown.
    const QByteArray malformed =
        "# Preserve this user-authored comment and the incomplete setting.\n"
        "[Instance0.Keyboard]\nA = 75\nB = [\n";
    QFile damaged(configDirectory + "/melonDS.toml");
    if (!damaged.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    if (damaged.write(malformed) != malformed.size()) return 2;
    damaged.close();
    check(!Config::Load(), "Malformed TOML load was reported as successful");
    Config::GetLocalTable(0).GetInt("Keyboard.A");
    Config::GetLocalTable(0).SetInt("Keyboard.A", Qt::Key_L);
    Config::Save();
    if (!damaged.open(QIODevice::ReadOnly)) return 2;
    check(damaged.readAll() == malformed,
          "Saving after a malformed TOML load overwrote the original file");
    damaged.close();

    // Once the file is repaired and reloaded, ordinary saves must work again.
    const QByteArray repaired = "[Instance0.Keyboard]\nA = 74\nB = -1\n";
    if (!damaged.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    if (damaged.write(repaired) != repaired.size()) return 2;
    damaged.close();
    check(Config::Load() && Config::GetLocalTable(0).GetInt("Keyboard.A") == Qt::Key_J,
          "A repaired config could not be reloaded");
    Config::GetLocalTable(0).SetInt("Keyboard.A", Qt::Key_P);
    Config::Save();
    check(Config::Load() && Config::GetLocalTable(0).GetInt("Keyboard.A") == Qt::Key_P &&
          Config::GetLocalTable(0).GetInt("Keyboard.B") == -1,
          "Saving a repaired config lost settings or remained blocked");
    std::printf("Qt mapping, config persistence, input/release and focus: %d failures\n", failures);
    return failures ? 1 : 0;
}
