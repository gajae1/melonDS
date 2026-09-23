// SPDX-License-Identifier: GPL-3.0-or-later
// Real CameraSettingsDialog + CameraManager + Config. The window/instance is
// a fixture; physical cameras are not required — the blank and still-image
// inputs plus the device-list enumeration all run headless. FS-21 covers the
// preview lifecycle: live-edited settings must be restored on Cancel and on
// destruction without an answer, and the emulation's started cameras must be
// handed back.
#include "main.h" // Load the real frontend declarations before replacing names.
#include <QtWidgets>
#include <QtTest/QTest>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include "ui_CameraSettingsDialog.h" // anchor for AUTOUIC generation

struct CameraInstance {};
class CameraWindow : public QWidget
{
public:
    CameraInstance instance;
    CameraInstance* getEmuInstance() { return &instance; }
};
#define EmuInstance CameraInstance
#define MainWindow CameraWindow
#include "../src/frontend/qt_sdl/CameraSettingsDialog.cpp"
#undef MainWindow
#undef EmuInstance

CameraManager* camManager[2];

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
    std::fprintf(stderr, "CameraSettingsUI: %s\n", message);
    std::fflush(stderr);
    std::_Exit(1);
}
template<typename T> static T* Widget(CameraSettingsDialog& dialog, const char* name)
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
static Config::Table Cam(int id)
{
    return Config::GetGlobalTable().GetTable(id == 0 ? "DSi.Camera0" : "DSi.Camera1");
}
static std::unique_ptr<CameraSettingsDialog> Open(CameraWindow& window)
{
    std::unique_ptr<CameraSettingsDialog> dialog(CameraSettingsDialog::openDlg(&window));
    dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    QApplication::processEvents();
    Require(dialog->isVisible(), "Camera settings dialog did not open");
    return dialog;
}
static void Finish(CameraSettingsDialog& dialog, QDialogButtonBox::StandardButton action)
{
    Click(Widget<QDialogButtonBox>(dialog, "buttonBox")->button(action));
    Require(!dialog.isVisible(), "Dialog did not close through its standard button");
    Require(dialog.result() == (action == QDialogButtonBox::Ok ? QDialog::Accepted : QDialog::Rejected),
            "Dialog returned the wrong acceptance result");
    Require(CameraSettingsDialog::currentDlg == nullptr, "Closed dialog retained its singleton");
}
static void SelectImageInput(CameraSettingsDialog& dialog, const QString& imagePath)
{
    Widget<QLineEdit>(dialog, "txtSrcImagePath")->setText(imagePath);
    QApplication::processEvents();
    Click(Widget<QRadioButton>(dialog, "rbPictureImg"));
}
static u32 CapturePixel(CameraManager& cam, int x)
{
    u32 frame[256 * 192];
    cam.captureFrame(frame, 256, 192, false);
    return frame[(96 * 256) + x];
}
static bool MostlyRed(u32 pixel)
{
    return ((pixel >> 16) & 0xFF) > 200 && ((pixel >> 8) & 0xFF) < 80 && (pixel & 0xFF) < 80;
}
static bool MostlyBlue(u32 pixel)
{
    return ((pixel >> 16) & 0xFF) < 80 && ((pixel >> 8) & 0xFF) < 80 && (pixel & 0xFF) > 200;
}
static void Scenario(const QString& name)
{
    CameraWindow window;

    if (name == "padded-frames")
    {
        CameraManager yuv(0, 4, 2, true);
        u8 yuyv[] = {
            0x10, 0x80, 0x40, 0x80, 0x20, 0x81, 0x60, 0x82, 0xEE, 0xEE, 0xEE, 0xEE,
            0x30, 0x90, 0x50, 0xA0, 0x70, 0x91, 0x80, 0xA1, 0xEE, 0xEE, 0xEE, 0xEE
        };
        const u32 expectedYuyv[] = {0x80408010, 0x82608120, 0xA0509030, 0xA1809170};
        u32 actualYuv[4] = {};
        yuv.feedFrame((u32*)yuyv, 4, 2, true, 12);
        yuv.captureFrame(actualYuv, 4, 2, true);
        for (int i = 0; i < 4; ++i)
            Require(actualYuv[i] == expectedYuyv[i], "Padded YUYV row was decoded as tightly packed");

        u8 uyvy[] = {
            0x80, 0x10, 0x80, 0x40, 0x81, 0x20, 0x82, 0x60, 0xEE, 0xEE, 0xEE, 0xEE,
            0x90, 0x30, 0xA0, 0x50, 0x91, 0x70, 0xA1, 0x80, 0xEE, 0xEE, 0xEE, 0xEE
        };
        yuv.feedFrame_UYVY((u32*)uyvy, 4, 2, 12);
        yuv.captureFrame(actualYuv, 4, 2, true);
        for (int i = 0; i < 4; ++i)
            Require(actualYuv[i] == expectedYuyv[i], "Padded UYVY row was decoded as tightly packed");

        u8 planeY[] = {0x10, 0x40, 0x20, 0x60, 0xEE, 0xEE,
                             0x30, 0x50, 0x70, 0x80, 0xEE, 0xEE};
        u8 planeUV[] = {0x80, 0x80, 0x81, 0x82, 0xEE, 0xEE};
        const u32 expectedNv12[] = {0x80408010, 0x82608120, 0x80508030, 0x82808170};
        yuv.feedFrame_NV12(planeY, planeUV, 4, 2, 6, 6);
        yuv.captureFrame(actualYuv, 4, 2, true);
        for (int i = 0; i < 4; ++i)
            Require(actualYuv[i] == expectedNv12[i], "Padded NV12 plane was decoded as tightly packed");

        CameraManager rgb(0, 4, 2, false);
        u32 rgbRows[] = {0xFF010203, 0xFF040506, 0xFF070809, 0xFF0A0B0C, 0xEEEEEEEE,
                               0xFF111213, 0xFF141516, 0xFF171819, 0xFF1A1B1C, 0xEEEEEEEE};
        u32 actualRgb[8] = {};
        rgb.feedFrame(rgbRows, 4, 2, false, 20);
        rgb.captureFrame(actualRgb, 4, 2, false);
        for (int i = 0; i < 8; ++i)
            Require(actualRgb[i] == rgbRows[i + i / 4], "Padded RGB row was decoded as tightly packed");
        return;
    }

    if (name == "indexed-image")
    {
        const QString imagePath = configDirectory + "/indexed-camera.png";
        QImage image(8, 8, QImage::Format_Indexed8);
        image.setColorTable({qRgb(255, 0, 0), qRgb(0, 0, 255)});
        image.fill(0);
        Require(image.save(imagePath), "Could not write camera image fixture");
        Require(QImage(imagePath).format() == QImage::Format_Indexed8,
                "Camera image fixture did not retain its pixel format");
        Cam(0).SetInt("InputType", 1);
        Cam(0).SetQString("ImagePath", imagePath);
        camManager[0]->init();
        Require(MostlyRed(CapturePixel(*camManager[0], 32)),
                "Non-RGB32 still image was not converted before pixel access");
        return;
    }

    if (name == "started-reinit")
    {
        camManager[0]->start();
        camManager[0]->deInit();
        camManager[0]->init();
        Require(camManager[0]->isStarted(), "Reinitialization lost the active camera request");
        camManager[0]->stop();
        return;
    }

    if (name == "saved-started")
    {
        // The emulation had both cameras started (a DSi game using them).
        camManager[0]->start();
        camManager[1]->start();
        auto dialog = Open(window);
        Require(camManager[0]->isStarted(), "Preview did not take over the selected camera");
        Require(!camManager[1]->isStarted(), "Preview did not stop the other camera");
        Finish(*dialog, QDialogButtonBox::Cancel);
        dialog.reset();
        Require(camManager[0]->isStarted() && camManager[1]->isStarted(),
                "Closing the dialog did not hand the saved camera state back");
        camManager[0]->stop();
        camManager[1]->stop();
        return;
    }

    auto dialog = Open(window);

    if (name == "device-list")
    {
#if QT_VERSION >= 0x060000
        auto* devices = Widget<QComboBox>(*dialog, "cbPhysicalCamera");
        const int real = QMediaDevices::videoInputs().count();
        Require(devices->count() == real, "Physical camera list does not match the real enumeration");
        Require(Widget<QRadioButton>(*dialog, "rbPictureCamera")->isEnabled() == (real > 0),
                "Camera input radio does not track device availability");
#endif
        Finish(*dialog, QDialogButtonBox::Cancel);
    }
    else if (name == "preview-image" || name == "xflip" || name == "accept" ||
             name == "cancel" || name == "destroy" || name == "switch-camera")
    {
        // Solid red PNG; the still-image input needs no physical device.
        const QString imagePath = configDirectory + "/camtest.png";
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(qRgb(255, 0, 0));
        if (name == "xflip")
        {
            for (int x = 0; x < 8; ++x)
                for (int y = 0; y < 8; ++y)
                    image.setPixel(x, y, x < 4 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
        }
        Require(image.save(imagePath), "Could not write the generated camera image");

        if (name == "switch-camera")
            Widget<QComboBox>(*dialog, "cbCameraSel")->setCurrentIndex(1);
        const int id = name == "switch-camera" ? 1 : 0;

        SelectImageInput(*dialog, imagePath);
        Require(Cam(id).GetInt("InputType") == 1 && Cam(id).GetQString("ImagePath") == imagePath,
                "Preview selection did not live-edit the camera configuration");

        if (name == "preview-image" || name == "xflip" || name == "accept")
        {
            // Left quarter: inside the red half for the split xflip image too.
            Require(MostlyRed(CapturePixel(*camManager[id], 32)),
                    "Still-image input did not reach the preview frame");
        }
        if (name == "xflip")
        {
            Click(Widget<QCheckBox>(*dialog, "chkFlipPicture"));
            Require(MostlyBlue(CapturePixel(*camManager[id], 64)),
                    "XFlip preview did not mirror the image horizontally");
        }

        if (name == "accept")
        {
            Finish(*dialog, QDialogButtonBox::Ok);
            dialog.reset();
            Require(Cam(id).GetInt("InputType") == 1 && Cam(id).GetQString("ImagePath") == imagePath,
                    "OK did not keep the selected image input");
            Require(Config::Load() && Cam(id).GetInt("InputType") == 1,
                    "Accepted camera settings were not saved to disk");
        }
        else
        {
            if (name == "destroy")
            {
                // Parent teardown path: destruction without accept/reject.
                dialog.reset();
                Require(CameraSettingsDialog::currentDlg == nullptr,
                        "Destroyed dialog retained its singleton");
            }
            else
            {
                Finish(*dialog, QDialogButtonBox::Cancel);
                dialog.reset();
            }
            Require(Cam(id).GetInt("InputType") == 0 && Cam(id).GetQString("ImagePath").isEmpty() &&
                    !Cam(id).GetBool("XFlip"),
                    "Unconfirmed dialog changes leaked into the camera configuration");
            Require(!camManager[id]->isStarted(), "Preview camera was left started");
            const int other = id ^ 1;
            Require(Cam(other).GetInt("InputType") == 0, "Other camera configuration was touched");
        }
    }
    else throw std::runtime_error("Unknown CameraSettingsUI scenario");
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QTimer watchdog;
    QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [] { Fatal("Scenario exceeded 15 seconds"); });
    watchdog.setSingleShot(true); watchdog.start(15000);
    try
    {
        Require(argc == 2, "Expected one CameraSettingsUI scenario name");
        QTemporaryDir directory;
        Require(directory.isValid(), "Temporary configuration directory unavailable");
        configDirectory = directory.path();
        emuDirectory = configDirectory;
        QFile file(configDirectory + "/melonDS.toml");
        const QByteArray seed = "[DSi.Camera0]\nInputType = 0\nImagePath = \"\"\nDeviceName = \"\"\nXFlip = false\n"
                                "[DSi.Camera1]\nInputType = 0\nImagePath = \"\"\nDeviceName = \"\"\nXFlip = false\n";
        Require(file.open(QIODevice::WriteOnly) && file.write(seed) == seed.size(),
                "Could not write generated configuration fixture");
        file.close();
        Require(Config::Load(), "Generated configuration failed to load");
        camManager[0] = new CameraManager(0, 640, 480, true);
        camManager[1] = new CameraManager(1, 640, 480, true);
        Scenario(QString::fromLocal8Bit(argv[1]));
        delete camManager[0];
        delete camManager[1];
        std::printf("CameraSettingsUI %s PASS\n", argv[1]);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "CameraSettingsUI %s: %s\n", argc > 1 ? argv[1] : "?", e.what());
        return 1;
    }
}
