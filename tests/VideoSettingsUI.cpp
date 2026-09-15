// SPDX-License-Identifier: GPL-3.0-or-later
// Production Qt dialog/config, with an explicit worker-result source. Real
// context/renderer failures are covered separately by the GL integration tests.
#include "EmuInstance.h"
#include <QtWidgets>
#include <cstdio>
#include <stdexcept>
#include "ui_VideoSettingsDialog.h"

class VideoWorker : public QObject
{
    Q_OBJECT
public:
    EmuThread::VideoSettingsStatus status;
    bool displayFailed = false;
    auto videoSettingsStatus() { return status; }
    bool hasGLFailure() { return displayFailed; }
signals:
    void videoSettingsStatusChanged();
};
struct VideoInstance
{
    Config::Table cfg = Config::GetGlobalTable();
    VideoWorker worker;
    Config::Table& getGlobalConfig() { return cfg; }
    VideoWorker* getEmuThread() { return &worker; }
};
class VideoWindow : public QWidget
{
public:
    VideoInstance instance;
    bool closed = false;
    VideoInstance* getEmuInstance() { return closed ? nullptr : &instance; }
};
#define EmuThread VideoWorker
#define EmuInstance VideoInstance
#define MainWindow VideoWindow
#include "../src/frontend/qt_sdl/VideoSettingsDialog.cpp"
#undef MainWindow
#undef EmuInstance
#undef EmuThread

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
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QTemporaryDir directory;
    configDirectory = directory.path();
    try
    {
        Require(directory.isValid(), "temporary configuration directory unavailable");
        Config::Load();
        VideoWindow window;
        auto& cfg = window.instance.cfg;
        auto& worker = window.instance.worker;
        cfg.SetInt("3D.Renderer", 1); cfg.SetBool("Screen.UseGL", true);
        cfg.SetInt("3D.GL.ScaleFactor", 1);
        worker.status.renderer = 1; worker.status.computeSupport = 1;
        auto open = [&] {
            auto* dialog = VideoSettingsDialog::openDlg(&window);
            QObject::connect(dialog, &VideoSettingsDialog::updateVideoSettings, &worker, [&](bool) {
                worker.status.pending = true;
                emit worker.videoSettingsStatusChanged();
            });
            return dialog;
        };
        auto publish = [&] {
            emit worker.videoSettingsStatusChanged();
            QApplication::processEvents();
        };
        auto* dialog = open();
        auto* compute = dialog->findChild<QRadioButton*>("rb3DCompute");
        auto* label = dialog->findChild<QLabel*>("lblRendererStatus");
        Require(label && label->text().contains("Active: OpenGL"), "initial active renderer missing");
        compute->click(); QApplication::processEvents();
        Require(cfg.GetInt("3D.Renderer") == 2 && label->text().contains("pending"),
                "paused selection was reported as already applied");
        worker.status.renderer = 0; worker.status.pending = false; worker.status.failed = true;
        publish();
        Require(compute->isChecked() && cfg.GetInt("3D.Renderer") == 2 &&
                label->text().contains("Active: Software") && label->text().contains("failed"),
                "fallback result lost failure or silently rewrote the selected preference");
        dialog->reject();
        Require(cfg.GetInt("3D.Renderer") == 1 && cfg.GetInt("3D.GL.ScaleFactor") == 1,
                "Cancel did not restore the original working settings");
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        worker.status = {1, 0, false, false, false};
        dialog = open(); compute = dialog->findChild<QRadioButton*>("rb3DCompute");
        label = dialog->findChild<QLabel*>("lblRendererStatus");
        Require(!compute->isEnabled() && label->text().contains("unavailable"),
                "unsupported context still offered Compute");
        compute->click();
        Require(cfg.GetInt("3D.Renderer") == 1, "disabled Compute changed configuration");
        worker.status.computeSupport = -1; publish();
        Require(compute->isEnabled(), "retired context prevented a new capability check");
        compute->click();
        worker.status = {2, 1, false, true, false}; publish();
        Require(label->text().contains("not ready"), "shader compilation was reported as ready");
        worker.status.compiling = false; publish();
        Require(label->text().contains("Active: OpenGL Compute") && !label->text().contains("not ready"),
                "successful renderer result was not shown");
        worker.displayFailed = true; publish();
        Require(label->text().contains("display failed"), "display failure was hidden");
        worker.displayFailed = false;
        cfg.SetInt("3D.Renderer", 0); cfg.SetBool("Screen.UseGL", false);
        worker.status = {0, -1, false, false, false}; publish();
        Require(dialog->findChild<QRadioButton*>("rb3DSoftware")->isChecked() &&
                !dialog->findChild<QCheckBox*>("cbGLDisplay")->isChecked(),
                "native recovery left stale selected controls");
        dialog->reject();
        Require(cfg.GetInt("3D.Renderer") == 1 && cfg.GetBool("Screen.UseGL"),
                "native fallback changed the pre-dialog Cancel baseline");
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        cfg.SetInt("3D.Renderer", 0);
        cfg.SetBool("Screen.UseGL", false);
        cfg.SetBool("Screen.UseVulkan", false);
        worker.status = {0, -1, false, false, false};
        dialog = open();
        auto* vulkan = dialog->findChild<QCheckBox*>("cbVulkanDisplay");
        int surfaceRebuilds = 0;
        QObject::connect(dialog, &VideoSettingsDialog::updateVideoSettings, &window,
            [&](bool rebuild) { surfaceRebuilds += rebuild; });
#ifdef Q_OS_WIN
        Require(vulkan->isEnabled(), "Software renderer did not offer experimental Vulkan display");
        vulkan->click();
        Require(cfg.GetBool("Screen.UseVulkan") && !dialog->UsesGL() && surfaceRebuilds == 1,
                "Native to Vulkan did not request a surface rebuild");
        Require(!dialog->findChild<QCheckBox*>("cbGLDisplay")->isEnabled(), "competing GL display left enabled");
        dialog->reject();
        Require(!cfg.GetBool("Screen.UseVulkan") && !cfg.GetBool("Screen.UseGL") && surfaceRebuilds == 2,
                "Vulkan Cancel did not restore native surface and preference");
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        dialog = open();
        dialog->findChild<QCheckBox*>("cbVulkanDisplay")->click();
        dialog->accept();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        Require(Config::Load() && Config::GetGlobalTable().GetBool("Screen.UseVulkan"),
                "Accepted Vulkan preference did not survive reload");
        // Config::Load replaces the backing TOML table; refresh borrowed handles.
        window.instance.cfg = Config::GetGlobalTable();
#else
        Require(!vulkan->isEnabled(), "unsupported WSI offered Vulkan output");
        dialog->reject();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
#endif

        dialog = open();
        auto* vulkan3D = dialog->findChild<QRadioButton*>("rb3DVulkan");
        Require(vulkan3D, "Vulkan 3D selection missing");
#ifdef VULKANRENDERER_ENABLED
        worker.status.vulkanSupport = 1; publish();
        vulkan3D->click(); QApplication::processEvents();
        auto* resolution = dialog->findChild<QComboBox*>("cbxGLResolution");
        Require(cfg.GetInt("3D.Renderer") == renderer3D_Vulkan && !dialog->UsesGL() &&
                resolution->isEnabled() && resolution->count() >= 3,
                "Vulkan 3D requires GL or hides supported scaling");
        for (int i = 0; i < resolution->count(); ++i)
            Require(bool(resolution->model()->flags(resolution->model()->index(i, 0)) & Qt::ItemIsEnabled) == (i < 3),
                    "Vulkan resolution choices do not match 1x/2x/3x support");
        for (int i : {1, 2})
        {
            resolution->setCurrentIndex(i); QApplication::processEvents();
            Require(cfg.GetInt("3D.GL.ScaleFactor") == i + 1, "Vulkan scale selection was not applied");
        }
        if (resolution->count() > 3)
        {
            resolution->setCurrentIndex(3); QApplication::processEvents();
            Require(cfg.GetInt("3D.GL.ScaleFactor") == 3, "unsupported Vulkan 4x scale was applied");
            resolution->setCurrentIndex(2);
        }
        worker.status.renderer = renderer3D_Vulkan; worker.status.pending = false; publish();
        label = dialog->findChild<QLabel*>("lblRendererStatus");
        Require(label->text().contains("Active: Vulkan 3D"), "Vulkan active status missing");
        worker.status.vulkanSupport = 0; publish();
        Require(!vulkan3D->isEnabled(), "unavailable Vulkan 3D remained selectable");
#else
        Require(!vulkan3D->isEnabled(), "build without Vulkan offered Vulkan 3D");
#endif
        window.closed = true; publish(); dialog->reject();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        std::puts("Video settings: pending/failure/retry/capability/native fallback/Cancel/closed owner PASS");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "VideoSettingsUI: %s\n", error.what());
        return 1;
    }
}
#include "VideoSettingsUI.moc"
