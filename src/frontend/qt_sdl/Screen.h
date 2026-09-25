/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef SCREEN_H
#define SCREEN_H

#include <optional>
#include <cstdint>
#include <deque>
#include <map>
#include <array>

#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QScreen>
#include <QCloseEvent>
#include <QTimer>

#include "glad/glad.h"
#include "RenderCost.h"
#include "ScreenLayout.h"
#include "graphics/gl/context.h"
#include "graphics/vulkan/Presenter.h"


class MainWindow;
class EmuInstance;

namespace melonDS { class NDS; }


const struct { int id; float ratio; const char* label; } aspectRatios[] =
{
    { 0, 1,                       "4:3 (native)" },
    { 4, (5.f  / 3) / (4.f / 3), "5:3 (3DS)"},
    { 1, (16.f / 9) / (4.f / 3),  "16:9" },
    { 2, (21.f / 9) / (4.f / 3),  "21:9" },
    { 3, 0,                       "window" }
};
constexpr int AspectRatiosNum = sizeof(aspectRatios) / sizeof(aspectRatios[0]);


class ScreenPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ScreenPanel(QWidget* parent);
    virtual ~ScreenPanel();

    void setFilter(bool filter);

    void setMouseHide(bool enable, int delay);

    void releaseTouch();

    QTimer* setupMouseTimer();
    void updateMouseTimer();
    QTimer* mouseTimer;
    QSize screenGetMinSize(int factor);

    void osdSetEnabled(bool enabled);
    void osdAddMessage(unsigned int color, const char* msg);

    virtual bool drawScreen() { return true; }
    // Set only while the worker is held, or on that worker during GL init.
    void setPreservedFrame(const std::array<QImage, 2>& images, unsigned int frame)
    { preservedFrame = images; preservedFrameNumber = frame; }

private slots:
    void onScreenLayoutChanged();
    void onAutoScreenSizingChanged(int sizing);

protected:
    MainWindow* mainWindow;
    EmuInstance* emuInstance;
    std::array<QImage, 2> preservedFrame;
    unsigned int preservedFrameNumber = 0;

    bool filter;

    int screenRotation;
    int screenGap;
    int screenLayout;
    bool screenSwap;
    int screenSizing;
    bool integerScaling;
    int screenAspectTop, screenAspectBot;

    int autoScreenSizing;

    ScreenLayout layout;
    float screenMatrix[kMaxScreenTransforms][6];
    int screenKind[kMaxScreenTransforms];
    int numScreens;

    bool touching = false;

    bool mouseHide;
    int mouseHideDelay;

    struct OSDItem
    {
        unsigned int id;
        qint64 timestamp;

        char text[256];
        unsigned int color;

        bool rendered;
        QImage bitmap;

        int rainbowstart;
        int rainbowend;
    };

    QMutex osdMutex;
    bool osdEnabled;
    unsigned int osdID;
    std::deque<OSDItem> osdItems;

    QPixmap splashLogo;
    OSDItem splashText[3];
    QPoint splashPos[4];

    void loadConfig();

    virtual void setupScreenLayout();

    // Drops any cached presentation pixels derived from the last DisplayFrame.
    // Called whenever the layout or filter changes; subclasses holding a
    // generation-keyed copy or upload override this.
    virtual void invalidatePresentedFrame() {}

    void resizeEvent(QResizeEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

    void tabletEvent(QTabletEvent* event) override;
    void touchEvent(QTouchEvent* event);
    bool event(QEvent* event) override;

    void showCursor();

    int osdFindBreakPoint(const char* text, int i);
    void osdLayoutText(const char* text, int* width, int* height, int* breaks);
    unsigned int osdRainbowColor(int inc);

    virtual void osdRenderItem(OSDItem* item);
    virtual void osdDeleteItem(OSDItem* item);

    void osdUpdate();

    void calcSplashLayout();
};


class ScreenPanelNative : public ScreenPanel
{
    Q_OBJECT

public:
    explicit ScreenPanelNative(QWidget* parent);
    virtual ~ScreenPanelNative();

    bool drawScreen() override;
    bool initVulkan();
    void deinitVulkan();
    bool usesVulkan() const { return bool(vulkan); }

protected:
    void paintEvent(QPaintEvent* event) override;
    QPaintEngine* paintEngine() const override;

private:
    void setupScreenLayout() override;

    std::unique_ptr<Vulkan::Presenter> vulkan;
    QImage vulkanFrame;
#ifdef VULKANRENDERER_ENABLED
    void paintVulkan();
    std::weak_ptr<melonDS::Vulkan::Device> vulkanAttempt;
#endif
    QMutex bufferLock;
    bool hasBuffers;

    QImage screen[2];
    QTransform screenTrans[kMaxScreenTransforms];

    // Generation of the CpuBGRA frame already copied into screen[].
    // Valid only while the NDS instance and payload identity match the
    // re-queried frame; 0 means nothing reusable is cached.
    void invalidatePresentedFrame() override;
    const melonDS::NDS* screenGenerationNDS = nullptr;
    const void* screenGenerationTop = nullptr;
    std::uint64_t screenGeneration = 0;

    melonDS::RenderCostNativeMeter RenderCost;
};


class ScreenPanelGL : public ScreenPanel
{
    Q_OBJECT

public:
    explicit ScreenPanelGL(QWidget* parent);
    virtual ~ScreenPanelGL();

    std::optional<WindowInfo> getWindowInfo();

    bool createContext();

    void setSwapInterval(int intv);

    bool initOpenGL();
    bool deinitOpenGL();
    bool makeCurrentGL();
    bool releaseGL();

    bool drawScreen() override;

    GL::Context* getContext() { return glContext.get(); }

    void transferLayout();
protected:

    qreal devicePixelRatioFromScreen() const;
    int scaledWindowWidth() const;
    int scaledWindowHeight() const;

    QPaintEngine* paintEngine() const override;

private:
    void setupScreenLayout() override;

    std::unique_ptr<GL::Context> glContext;
    bool glInited;
    bool glOwned = false; // Includes a partial init or an outstanding current release.
    std::optional<int> pendingSwapInterval;

    GLuint screenVertexBuffer = 0, screenVertexArray = 0;
    GLuint screenTexture = 0;
    int screenTextureWidth = 256, screenTextureHeight = 192;

    // Generation of the CpuBGRA frame already stored in screenTexture.
    // Cleared whenever the texture or its contents may no longer match the
    // current frame (realloc, preserved image, context/panel recreation,
    // renderer switch, layout or filter change).
    void invalidatePresentedFrame() override;
    const melonDS::NDS* screenTextureGenerationNDS = nullptr;
    const void* screenTextureGenerationTop = nullptr;
    std::uint64_t screenTextureGeneration = 0;

    GLuint screenShaderProgram = 0;
    GLint screenShaderTransformULoc, screenShaderScreenSizeULoc;

    QMutex screenSettingsLock;
    WindowInfo windowInfo;

    int lastScreenWidth = -1, lastScreenHeight = -1;

    GLuint osdShader = 0;
    GLint osdScreenSizeULoc, osdPosULoc, osdSizeULoc;
    GLint osdScaleFactorULoc;
    GLint osdTexScaleULoc;
    GLuint osdVertexArray = 0;
    GLuint osdVertexBuffer = 0;
    std::map<unsigned int, GLuint> osdTextures;

    GLuint logoTexture = 0;

    melonDS::RenderCostPresentMeter RenderCost;

    void osdRenderItem(OSDItem* item) override;
    void osdDeleteItem(OSDItem* item) override;
};

#endif // SCREEN_H
