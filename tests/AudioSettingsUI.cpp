// SPDX-License-Identifier: GPL-3.0-or-later
// Actual production dialog, generated UI and Config persistence. Only the
// window/instance and synchronous output-reconfiguration result are replaced;
// worker pause semantics, SDL reopen and callbacks belong to the audio tests.
#include "main.h" // Load the real frontend declarations before replacing names.
#include <QtWidgets>
#include <QtTest/QTest>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>
#include "ui_AudioSettingsDialog.h"

struct AudioInstance
{
    struct Call { int requested, configuredBefore, activeBefore; };
    Config::Table global = Config::GetGlobalTable();
    Config::Table local;
    int instanceID;
    int activeBuffer = 512;
    int failBuffer = -1;
    std::vector<Call> calls;

    explicit AudioInstance(int instance) : local(Config::GetLocalTable(instance)), instanceID(instance) {}
    Config::Table& getGlobalConfig() { return global; }
    Config::Table& getLocalConfig() { return local; }
    int getInstanceID() { return instanceID; }
    bool emuIsActive() { return false; }
    melonDS::NDS* getNDS() { return nullptr; }
    bool changeAudioBuffer(int frames, QString& error)
    {
        calls.push_back({frames, global.GetInt("Audio.BufferSize"), activeBuffer});
        if (frames == failBuffer)
        {
            error = "AudioSettingsUI simulated device-open failure";
            return false;
        }
        activeBuffer = frames;
        return true;
    }
    QString audioOutputDescription() const
    {
        // Deliberately different obtained period: the label must show the
        // output result, not infer a device period from the requested choice.
        return QString("Test output: requested %1 frames; obtained 256 frames at 44100 Hz")
            .arg(activeBuffer);
    }
};
class AudioWindow : public QWidget
{
public:
    AudioInstance instance;
    explicit AudioWindow(int instanceID = 0) : instance(instanceID) {}
    AudioInstance* getEmuInstance() { return &instance; }
};
#define EmuInstance AudioInstance
#define MainWindow AudioWindow
#include "../src/frontend/qt_sdl/AudioSettingsDialog.cpp"
#undef MainWindow
#undef EmuInstance

QString emuDirectory;
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
[[noreturn]] static void Fatal(const char* message)
{
    std::fprintf(stderr, "AudioSettingsUI: %s\n", message);
    std::fflush(stderr);
    std::_Exit(1);
}
template<typename T> static T* Widget(AudioSettingsDialog& dialog, const char* name)
{
    auto* widget = dialog.findChild<T*>(name);
    Require(widget != nullptr, name);
    return widget;
}
static void Click(QAbstractButton* button)
{
    QTest::mouseClick(button, Qt::LeftButton, Qt::NoModifier, QPoint(8, button->height() / 2));
    QApplication::processEvents();
}
static std::unique_ptr<AudioSettingsDialog> Open(AudioWindow& window)
{
    std::unique_ptr<AudioSettingsDialog> dialog(AudioSettingsDialog::openDlg(&window));
    dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    QApplication::processEvents();
    Require(dialog->isVisible(), "Audio settings dialog did not open");
    return dialog;
}
static void Finish(AudioSettingsDialog& dialog, QDialogButtonBox::StandardButton action)
{
    Click(Widget<QDialogButtonBox>(dialog, "buttonBox")->button(action));
    Require(!dialog.isVisible(), "Dialog did not close through its standard button");
    Require(dialog.result() == (action == QDialogButtonBox::Ok ? QDialog::Accepted : QDialog::Rejected),
            "Dialog returned the wrong acceptance result");
    Require(AudioSettingsDialog::currentDlg == nullptr, "Closed dialog retained its singleton");
}
static void SelectBuffer(AudioSettingsDialog& dialog, int frames)
{
    auto* combo = Widget<QComboBox>(dialog, "cbBufferSize");
    const int index = combo->findData(frames);
    Require(index >= 0, "Requested manual buffer choice is absent");
    combo->setCurrentIndex(index);
    QApplication::processEvents();
}
static void CheckCall(const AudioInstance& instance, size_t count, int requested, int previous)
{
    Require(instance.calls.size() == count, "Unexpected number of synchronous output reconfigurations");
    const auto& call = instance.calls.back();
    Require(call.requested == requested && call.configuredBefore == previous && call.activeBefore == previous,
            "Output reconfiguration did not precede the configuration update");
}
static void CheckOutput(AudioSettingsDialog& dialog, AudioInstance& instance, int frames)
{
    Require(instance.activeBuffer == frames && instance.global.GetInt("Audio.BufferSize") == frames,
            "Active output and successful buffer configuration disagree");
    Require(Widget<QLabel>(dialog, "lblBufferStatus")->text() == instance.audioOutputDescription(),
            "Output information did not show the obtained device result");
}
class ExpectedWarning
{
    QTimer poll, timeout;
    bool dismissed = false;
public:
    ExpectedWarning(AudioSettingsDialog& owner, const QString& explanation)
    {
        const QString text = explanation + "\nAudioSettingsUI simulated device-open failure";
        QObject::connect(&poll, &QTimer::timeout, &poll, [this, &owner, text] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box) return;
            if (box->parentWidget() != &owner || box->windowTitle() != "Audio output" ||
                box->text() != text || box->icon() != QMessageBox::Warning ||
                box->standardButtons() != QMessageBox::Ok)
                Fatal("Unexpected modal dialog; no button was dismissed");
            dismissed = true;
            poll.stop(); timeout.stop();
            box->button(QMessageBox::Ok)->click();
        });
        QObject::connect(&timeout, &QTimer::timeout, &timeout, [] {
            Fatal("Expected owned audio warning did not appear");
        });
        timeout.setSingleShot(true);
        poll.start(5); timeout.start(3000);
    }
    void Check() const { Require(dismissed, "Device-open failure did not display its warning"); }
};
static void Preview64(AudioSettingsDialog& dialog, AudioInstance& instance)
{
    SelectBuffer(dialog, 64);
    Require(instance.calls.empty() && instance.global.GetInt("Audio.BufferSize") == 512,
            "Selecting a buffer applied it before Preview or OK");
    Click(Widget<QPushButton>(dialog, "btnApplyBuffer"));
    CheckCall(instance, 1, 64, 512);
    CheckOutput(dialog, instance, 64);
    const QString capture = qEnvironmentVariable("MELONDS_AUDIO_UI_CAPTURE");
    if (!capture.isEmpty()) Require(dialog.grab().save(capture), "Could not capture the actual Qt dialog");
}
static void ReadBack(int frames)
{
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {"readback", configDirectory, QString::number(frames)});
    const bool finished = child.waitForFinished(10000);
    const QByteArray output = child.readAllStandardOutput() + child.readAllStandardError();
    std::fwrite(output.constData(), 1, output.size(), stdout);
    if (!finished) { child.kill(); child.waitForFinished(1000); }
    Require(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
            "Fresh Config::Load process did not recover the successfully applied buffer");
}
static void Scenario(const QString& name)
{
    AudioWindow window(name == "secondary" ? 1 : 0);
    auto& instance = window.instance;
    auto& cfg = instance.global;
    if (name == "secondary") cfg.SetInt("Audio.LowPassCutoff", 9000);
    auto dialog = Open(window);

    if (name == "filter-cancel")
    {
        auto* combo = Widget<QComboBox>(*dialog, "cbBufferSize");
        auto* filter = Widget<QCheckBox>(*dialog, "chkLowPass");
        auto* cutoff = Widget<QSpinBox>(*dialog, "sbLowPassCutoff");
        Require(combo->findData(32) >= 0 && combo->findData(64) >= 0 && combo->currentData().toInt() == 512,
                "Existing 512-frame configuration lost the new manual buffer choices");
        Require(!filter->isChecked() && !cutoff->isEnabled() && cutoff->value() == 20000 &&
                cfg.GetInt("Audio.LowPassCutoff") == 0,
                "Disabled filter did not retain cutoff zero with a high starting frequency");
        std::vector<int> applied;
        QObject::connect(dialog.get(), &AudioSettingsDialog::updateAudioSettings, dialog.get(),
                         [&] { applied.push_back(cfg.GetInt("Audio.LowPassCutoff")); });
        Click(filter);
        Require(filter->isChecked() && cutoff->isEnabled() && applied == std::vector<int>{20000},
                "Enabling the filter did not publish the high cutoff for live apply");
        cutoff->setFocus();
        QTest::keyClick(cutoff, Qt::Key_Down);
        QApplication::processEvents();
        Require(cutoff->value() > 0 && cutoff->value() < 20000 && applied.size() == 2 &&
                applied.back() == cutoff->value() && cfg.GetInt("Audio.LowPassCutoff") == cutoff->value(),
                "Decreasing the enabled filter did not apply its lower cutoff");
        Finish(*dialog, QDialogButtonBox::Cancel);
        Require(cfg.GetInt("Audio.LowPassCutoff") == 0 && applied.size() == 3 && applied.back() == 0 &&
                instance.calls.empty(), "Cancel did not restore and publish the original disabled filter");
    }
    else if (name == "buffer-preview-cancel" || name == "buffer-cancel-failure")
    {
        Preview64(*dialog, instance);
        if (name == "buffer-cancel-failure")
        {
            instance.failBuffer = 512;
            ExpectedWarning warning(*dialog, "The previous output buffer could not be restored.");
            Finish(*dialog, QDialogButtonBox::Cancel);
            warning.Check();
            Require(instance.activeBuffer == 64 && cfg.GetInt("Audio.BufferSize") == 64,
                    "Failed Cancel rollback replaced the last successful buffer/configuration");
        }
        else
        {
            Finish(*dialog, QDialogButtonBox::Cancel);
            Require(instance.activeBuffer == 512 && cfg.GetInt("Audio.BufferSize") == 512,
                    "Cancel did not reconfigure and restore the original 512-frame buffer");
        }
        CheckCall(instance, 2, 512, 64);
        ReadBack(512); // Preview/Cancel must not persist a draft, even if rollback fails.
    }
    else if (name == "buffer-accept")
    {
        SelectBuffer(*dialog, 32);
        Require(instance.calls.empty() && cfg.GetInt("Audio.BufferSize") == 512,
                "Unapplied 32-frame choice changed the active configuration");
        Finish(*dialog, QDialogButtonBox::Ok);
        CheckCall(instance, 1, 32, 512);
        CheckOutput(*dialog, instance, 32);
        ReadBack(32);
    }
    else if (name == "buffer-failure")
    {
        Preview64(*dialog, instance);
        instance.failBuffer = 32;
        SelectBuffer(*dialog, 32);
        {
            ExpectedWarning warning(*dialog, "The requested output buffer could not be applied.");
            Click(Widget<QPushButton>(*dialog, "btnApplyBuffer"));
            warning.Check();
        }
        CheckCall(instance, 2, 32, 64);
        CheckOutput(*dialog, instance, 64);
        Require(Widget<QComboBox>(*dialog, "cbBufferSize")->currentData().toInt() == 64 && dialog->isVisible(),
                "Failed Preview did not restore the successful choice and keep the dialog open");
        SelectBuffer(*dialog, 32);
        {
            ExpectedWarning warning(*dialog, "The requested output buffer could not be applied.");
            Finish(*dialog, QDialogButtonBox::Ok);
            warning.Check();
        }
        CheckCall(instance, 3, 32, 64);
        CheckOutput(*dialog, instance, 64);
        ReadBack(64); // OK must save the last success, never the failed request.

        // A hand-edited non-power-of-two preference displays its rounded
        // choice. Failed Preview must preserve that preference without leaving
        // the combo blank (findData(33) has no item).
        cfg.SetInt("Audio.BufferSize", 33);
        instance.calls.clear();
        dialog = Open(window);
        Require(Widget<QComboBox>(*dialog, "cbBufferSize")->currentData().toInt() == 64,
                "Non-power-of-two preference did not display the rounded buffer choice");
        SelectBuffer(*dialog, 32);
        {
            ExpectedWarning warning(*dialog, "The requested output buffer could not be applied.");
            Click(Widget<QPushButton>(*dialog, "btnApplyBuffer"));
            warning.Check();
        }
        Require(instance.calls.size() == 1 && instance.calls.back().requested == 32 &&
                instance.calls.back().configuredBefore == 33 && instance.calls.back().activeBefore == 64 &&
                instance.activeBuffer == 64 && cfg.GetInt("Audio.BufferSize") == 33 &&
                Widget<QComboBox>(*dialog, "cbBufferSize")->currentData().toInt() == 64 &&
                Widget<QLabel>(*dialog, "lblBufferStatus")->text() == instance.audioOutputDescription(),
                "Failed Preview lost the raw 33 preference, active output, or rounded 64 choice");
        Finish(*dialog, QDialogButtonBox::Cancel);
        Require(instance.calls.size() == 1 && cfg.GetInt("Audio.BufferSize") == 33,
                "Cancel unnecessarily reconfigured the unchanged non-power-of-two preference");
    }
    else if (name == "secondary")
    {
        auto* combo = Widget<QComboBox>(*dialog, "cbBufferSize");
        auto* preview = Widget<QPushButton>(*dialog, "btnApplyBuffer");
        auto* filter = Widget<QCheckBox>(*dialog, "chkLowPass");
        auto* cutoff = Widget<QSpinBox>(*dialog, "sbLowPassCutoff");
        Require(!combo->isEnabled() && !preview->isEnabled() && !filter->isEnabled() && !cutoff->isEnabled(),
                "Secondary instance exposes shared output/filter controls");
        QTest::keyClick(combo, Qt::Key_Up);
        Click(preview); Click(filter);
        QTest::keyClick(cutoff, Qt::Key_Down);
        Finish(*dialog, QDialogButtonBox::Ok);
        Require(instance.calls.empty() && cfg.GetInt("Audio.BufferSize") == 512 &&
                cfg.GetInt("Audio.LowPassCutoff") == 9000,
                "Secondary instance changed global output/filter settings through UI or OK");
    }
    else throw std::runtime_error("Unknown AudioSettingsUI scenario");
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    // Only dummy enumeration is needed by the production microphone controls.
    // No output device is opened by this acceptance fixture.
    qputenv("SDL_AUDIODRIVER", "dummy");
    if (SDL_Init(SDL_INIT_AUDIO) != 0) return 2;
    struct SDLShutdown { ~SDLShutdown() { SDL_Quit(); } } sdlShutdown;
    QTimer watchdog;
    QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [] { Fatal("Scenario exceeded 15 seconds"); });
    watchdog.setSingleShot(true); watchdog.start(15000);
    try
    {
        if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "readback")
        {
            configDirectory = QString::fromLocal8Bit(argv[2]);
            const int frames = QString::fromLocal8Bit(argv[3]).toInt();
            Require(Config::Load(), "Fresh process could not load the generated configuration");
            Require(Config::GetGlobalTable().GetInt("Audio.BufferSize") == frames,
                    "Saved buffer was absent or changed during fresh Config::Load");
            std::printf("Fresh Config::Load: Audio.BufferSize=%d PASS\n", frames);
            return 0;
        }
        Require(argc == 2, "Expected one AudioSettingsUI scenario name");
        QTemporaryDir directory;
        Require(directory.isValid(), "Temporary configuration directory unavailable");
        configDirectory = directory.path();
        emuDirectory = configDirectory;
        QFile file(configDirectory + "/melonDS.toml");
        const QByteArray seed = "[Audio]\nBufferSize = 512\nLowPassCutoff = 0\n"
                                "[Mic]\nInputType = 0\n"
                                "[Instance0.Audio]\nVolume = 256\nDSiVolumeSync = false\n";
        Require(file.open(QIODevice::WriteOnly) && file.write(seed) == seed.size(),
                "Could not write generated configuration fixture");
        file.close();
        Require(Config::Load(), "Generated configuration failed to load");
        Scenario(QString::fromLocal8Bit(argv[1]));
        std::printf("AudioSettingsUI %s PASS\n", argv[1]);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "AudioSettingsUI: %s\n", error.what());
        return 1;
    }
}
