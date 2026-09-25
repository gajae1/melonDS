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

#include <string.h>

#include <optional>
#include <cmath>

#include <QPaintEvent>
#include <QPainter>

#include <QDateTime>

#include "OpenGLSupport.h"
#include "graphics/gl/context.h"

#include "main.h"
#include "EmuInstance.h"

#include "NDS.h"
#include "GPU.h"
#include "GPU3D_Soft.h"
#include "GPU3D_OpenGL.h"
#ifdef VULKANRENDERER_ENABLED
#include "GPU_Vulkan.h"
#endif
#include "Platform.h"
#include "Config.h"

#include "main_shaders.h"
#include "OSD_shaders.h"
#include "font.h"
#include "version.h"

using namespace melonDS;

#if !defined(_WIN32) && !defined(APPLE)
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
using namespace QNativeInterface;
#else
#include <qpa/qplatformnativeinterface.h>
#endif
#endif


const u32 kOSDMargin = 6;
const int kLogoWidth = 192;


ScreenPanel::ScreenPanel(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_AcceptTouchEvents);

    QWidget* w = parent;
    for (;;)
    {
        mainWindow = qobject_cast<MainWindow*>(w);
        if (mainWindow) break;
        w = w->parentWidget();
        if (!w) break;
    }

    emuInstance = mainWindow->getEmuInstance();

    mouseHide = false;
    mouseHideDelay = 0;

    QTimer* mouseTimer = setupMouseTimer();
    connect(mouseTimer, &QTimer::timeout, [this] { if (mouseHide) setCursor(Qt::BlankCursor);});

    osdEnabled = false;
    osdID = 1;
    
    loadConfig();
    setFilter(mainWindow->getWindowConfig().GetBool("ScreenFilter"));

    splashLogo = QPixmap(":/melon-logo");

    strncpy(splashText[0].text, "File->Open ROM...", 256);
    splashText[0].id = 0x80000000;
    splashText[0].color = 0;
    splashText[0].rendered = false;
    splashText[0].rainbowstart = -1;

    strncpy(splashText[1].text, "to get started", 256);
    splashText[1].id = 0x80000001;
    splashText[1].color = 0;
    splashText[1].rendered = false;
    splashText[1].rainbowstart = -1;

    std::string url = MELONDS_URL;
    int urlpos = url.find("://");
    urlpos = (urlpos == std::string::npos) ? 0 : urlpos+3;
    strncpy(splashText[2].text, url.c_str() + urlpos, 256);
    splashText[2].id = 0x80000002;
    splashText[2].color = 0;
    splashText[2].rendered = false;
    splashText[2].rainbowstart = -1;
}

ScreenPanel::~ScreenPanel()
{
    mouseTimer->stop();
    delete mouseTimer;
}

void ScreenPanel::loadConfig()
{
    auto& cfg = mainWindow->getWindowConfig();
    
    screenRotation = cfg.GetInt("ScreenRotation");
    screenGap = cfg.GetInt("ScreenGap");
    screenLayout = cfg.GetInt("ScreenLayout");
    screenSwap = cfg.GetBool("ScreenSwap");
    screenSizing = cfg.GetInt("ScreenSizing");
    integerScaling = cfg.GetBool("IntegerScaling");
    screenAspectTop = cfg.GetInt("ScreenAspectTop");
    screenAspectBot = cfg.GetInt("ScreenAspectBot");
}

void ScreenPanel::setFilter(bool filter)
{
    this->filter = filter;
    // Cached presentation pixels are reused only for an identical layout and
    // filter state, so any change forces one fresh copy/upload.
    invalidatePresentedFrame();
}

void ScreenPanel::setMouseHide(bool enable, int delay)
{
    mouseHide = enable;
    mouseHideDelay = delay;

    mouseTimer->setInterval(mouseHideDelay);
}

void ScreenPanel::setupScreenLayout()
{
    int w = width();
    int h = height();

    int sizing = screenSizing;
    if (sizing == screenSizing_Auto) sizing = autoScreenSizing;

    float aspectTop, aspectBot;

    for (auto ratio : aspectRatios)
    {
        if (ratio.id == screenAspectTop)
            aspectTop = ratio.ratio;
        if (ratio.id == screenAspectBot)
            aspectBot = ratio.ratio;
    }

    if (aspectTop == 0)
        aspectTop = ((float) w / h) / (4.f / 3.f);

    if (aspectBot == 0)
        aspectBot = ((float) w / h) / (4.f / 3.f);

    layout.Setup(w, h,
                static_cast<ScreenLayoutType>(screenLayout),
                static_cast<ScreenRotation>(screenRotation),
                static_cast<ScreenSizing>(sizing),
                screenGap,
                integerScaling != 0,
                screenSwap != 0,
                aspectTop,
                aspectBot);

    numScreens = layout.GetScreenTransforms(screenMatrix[0], screenKind);

    // Rotation, layout, sizing and resize all reach this point; drop cached
    // presentation pixels so the next paint re-copies/re-uploads once.
    invalidatePresentedFrame();

    calcSplashLayout();
}

QSize ScreenPanel::screenGetMinSize(int factor = 1)
{
    bool isHori = (screenRotation == screenRot_90Deg
        || screenRotation == screenRot_270Deg);
    int gap = screenGap * factor;

    int w = 256 * factor;
    int h = 192 * factor;

    if (screenSizing == screenSizing_TopOnly
        || screenSizing == screenSizing_BotOnly)
    {
        return QSize(w, h);
    }

    if (screenLayout == screenLayout_Natural)
    {
        if (isHori)
            return QSize(h+gap+h, w);
        else
            return QSize(w, h+gap+h);
    }
    else if (screenLayout == screenLayout_Vertical)
    {
        if (isHori)
            return QSize(h, w+gap+w);
        else
            return QSize(w, h+gap+h);
    }
    else if (screenLayout == screenLayout_Horizontal)
    {
        if (isHori)
            return QSize(h+gap+h, w);
        else
            return QSize(w+gap+w, h);
    }
    else // hybrid
    {
        if (isHori)
            return QSize(h+gap+h, 3*w + (int)ceil((4*gap) / 3.0));
        else
            return QSize(3*w + (int)ceil((4*gap) / 3.0), h+gap+h);
    }
}

void ScreenPanel::onScreenLayoutChanged()
{
    loadConfig();

    setMinimumSize(screenGetMinSize());
    setupScreenLayout();
}

void ScreenPanel::onAutoScreenSizingChanged(int sizing)
{
    autoScreenSizing = sizing;
    if (screenSizing != screenSizing_Auto) return;

    setupScreenLayout();
}

void ScreenPanel::resizeEvent(QResizeEvent* event)
{
    setupScreenLayout();
    QWidget::resizeEvent(event);
}

void ScreenPanel::releaseTouch()
{
    touching = false;
    emuInstance->releaseScreen();
}

void ScreenPanel::mousePressEvent(QMouseEvent* event)
{
    event->accept();
    if (!emuInstance->emuIsActive()) { releaseTouch(); return; }
    if (event->button() != Qt::LeftButton) return;

    int x = event->pos().x();
    int y = event->pos().y();

    if (layout.GetTouchCoords(x, y, false))
    {
        touching = true;
        emuInstance->touchScreen(x, y);
    }
}

void ScreenPanel::mouseReleaseEvent(QMouseEvent* event)
{
    event->accept();
    if (event->button() != Qt::LeftButton) return;

    if (touching)
        releaseTouch();
}

void ScreenPanel::mouseMoveEvent(QMouseEvent* event)
{
    event->accept();

    showCursor();

    if (!emuInstance->emuIsActive()) { releaseTouch(); return; }
    //if (!(event->buttons() & Qt::LeftButton)) return;
    if (!touching) return;

    int x = event->pos().x();
    int y = event->pos().y();

    if (layout.GetTouchCoords(x, y, true))
    {
        emuInstance->touchScreen(x, y);
    }
}

void ScreenPanel::tabletEvent(QTabletEvent* event)
{
    event->accept();
    if (!emuInstance->emuIsActive()) { releaseTouch(); return; }
    if (event->type() == QEvent::TabletMove && !touching) return;

    switch(event->type())
    {
    case QEvent::TabletPress:
    case QEvent::TabletMove:
        {
#if QT_VERSION_MAJOR == 6
            int x = event->position().x();
            int y = event->position().y();
#else
            int x = event->x();
            int y = event->y();
#endif

            if (layout.GetTouchCoords(x, y, event->type()==QEvent::TabletMove))
            {
                touching = true;
                emuInstance->touchScreen(x, y);
            }
        }
        break;
    case QEvent::TabletRelease:
        if (touching)
            releaseTouch();
        break;
    default:
        break;
    }
}

void ScreenPanel::touchEvent(QTouchEvent* event)
{
#if QT_VERSION_MAJOR == 6
    if (event->device()->type() == QInputDevice::DeviceType::TouchPad)
        return;
#endif

    event->accept();
    if (!emuInstance->emuIsActive()) { releaseTouch(); return; }

    // A cancelled or unfocused gesture needs a new press before moving again.
    if (event->type() == QEvent::TouchUpdate && !touching) return;

    switch(event->type())
    {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
#if QT_VERSION_MAJOR == 6
        if (event->points().length() > 0)
#else
        if (event->touchPoints().length() > 0)
#endif
        {
#if QT_VERSION_MAJOR == 6
            QPointF position = event->points().first().position();
#else
            QPointF position = event->touchPoints().first().pos();
#endif
            int x = (int)position.x();
            int y = (int)position.y();

            if (layout.GetTouchCoords(x, y, event->type()==QEvent::TouchUpdate))
            {
                touching = true;
                emuInstance->touchScreen(x, y);
            }
        }
        break;
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        releaseTouch();
        break;
    default:
        break;
    }
}

bool ScreenPanel::event(QEvent* event)
{
    if (event->type() == QEvent::TouchBegin
        || event->type() == QEvent::TouchEnd
        || event->type() == QEvent::TouchCancel
        || event->type() == QEvent::TouchUpdate)
    {
        touchEvent((QTouchEvent*)event);
        return true;
    }
    else if (event->type() == QEvent::FocusIn)
        mainWindow->onFocusIn();
    else if (event->type() == QEvent::FocusOut)
        mainWindow->onFocusOut();

    return QWidget::event(event);
}

void ScreenPanel::showCursor()
{
    mainWindow->panel->setCursor(Qt::ArrowCursor);
    mouseTimer->start();
}

QTimer* ScreenPanel::setupMouseTimer()
{
    mouseTimer = new QTimer();
    mouseTimer->setSingleShot(true);
    mouseTimer->setInterval(mouseHideDelay);
    mouseTimer->start();

    return mouseTimer;
}

int ScreenPanel::osdFindBreakPoint(const char* text, int i)
{
    // i = character that went out of bounds

    for (int j = i; j >= 0; j--)
    {
        if (text[j] == ' ')
            return j;
    }

    return i;
}

void ScreenPanel::osdLayoutText(const char* text, int* width, int* height, int* breaks)
{
    int w = 0;
    int h = 14;
    int totalw = 0;
    int maxw = ((QWidget*)this)->width() - (kOSDMargin*2);
    int lastbreak = -1;
    int numbrk = 0;
    u16* ptr;

    memset(breaks, 0, sizeof(int)*64);

    for (int i = 0; text[i] != '\0'; )
	{
	    int glyphsize;
		if (text[i] == ' ')
		{
			glyphsize = 6;
		}
		else
        {
            u32 ch = text[i];
            if (ch < 0x10 || ch > 0x7E) ch = 0x7F;

            ptr = &::font[(ch-0x10) << 4];
            glyphsize = ptr[0];
            if (!glyphsize) glyphsize = 6;
            else            glyphsize += 2; // space around the character
        }

		w += glyphsize;
		if (w > maxw)
        {
            // wrap shit as needed
            if (text[i] == ' ')
            {
                if (numbrk >= 64) break;
                breaks[numbrk++] = i;
                i++;
            }
            else
            {
                int brk = osdFindBreakPoint(text, i);
                if (brk != lastbreak) i = brk;

                if (numbrk >= 64) break;
                breaks[numbrk++] = i;

                lastbreak = brk;
            }

            w = 0;
            h += 14;
        }
        else
            i++;

        if (w > totalw) totalw = w;
    }

    *width = totalw;
    *height = h;
}

unsigned int ScreenPanel::osdRainbowColor(int inc)
{
    // inspired from Acmlmboard

    if      (inc < 100) return 0xFFFF9B9B + (inc << 8);
    else if (inc < 200) return 0xFFFFFF9B - ((inc-100) << 16);
    else if (inc < 300) return 0xFF9BFF9B + (inc-200);
    else if (inc < 400) return 0xFF9BFFFF - ((inc-300) << 8);
    else if (inc < 500) return 0xFF9B9BFF + ((inc-400) << 16);
    else                return 0xFFFF9BFF - (inc-500);
}

void ScreenPanel::osdRenderItem(OSDItem* item)
{
    int w, h;
    int breaks[64];

    char* text = item->text;
    u32 color = item->color;

    bool rainbow = (color == 0);
    u32 rainbowinc;
    if (item->rainbowstart == -1)
    {
        u32 ticks = (u32) QDateTime::currentMSecsSinceEpoch();
        rainbowinc = ((text[0] * 17) + (ticks * 13)) % 600;
    }
    else
        rainbowinc = (u32)item->rainbowstart;

    color |= 0xFF000000;
    const u32 shadow = 0xE0000000;

    osdLayoutText(text, &w, &h, breaks);

    item->bitmap = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    u32* bitmap = (u32*)item->bitmap.bits();
    memset(bitmap, 0, w*h*sizeof(u32));

    int x = 0, y = 1;
    u32 maxw = ((QWidget*)this)->width() - (kOSDMargin*2);
    int curline = 0;
    u16* ptr;

    for (int i = 0; text[i] != '\0'; )
	{
	    int glyphsize;
		if (text[i] == ' ')
		{
			x += 6;
		}
		else
        {
            u32 ch = text[i];
            if (ch < 0x10 || ch > 0x7E) ch = 0x7F;

            ptr = &::font[(ch-0x10) << 4];
            int glyphsize = ptr[0];
            if (!glyphsize) x += 6;
            else
            {
                x++;

                if (rainbow)
                {
                    color = osdRainbowColor(rainbowinc);
                    rainbowinc = (rainbowinc + 30) % 600;
                }

                // draw character
                for (int cy = 0; cy < 12; cy++)
                {
                    u16 val = ptr[4+cy];

                    for (int cx = 0; cx < glyphsize; cx++)
                    {
                        if (val & (1<<cx))
                            bitmap[((y+cy) * w) + x+cx] = color;
                    }
                }

                x += glyphsize;
                x++;
            }
        }

		i++;
		if (breaks[curline] && i >= breaks[curline])
        {
            i = breaks[curline++];
            if (text[i] == ' ') i++;

            x = 0;
            y += 14;
        }
    }

    // shadow
    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            u32 val;

            val = bitmap[(y * w) + x];
            if ((val >> 24) == 0xFF) continue;

            if (x > 0)   val  = bitmap[(y * w) + x-1];
            if (x < w-1) val |= bitmap[(y * w) + x+1];
            if (y > 0)
            {
                if (x > 0)   val |= bitmap[((y-1) * w) + x-1];
                val |= bitmap[((y-1) * w) + x];
                if (x < w-1) val |= bitmap[((y-1) * w) + x+1];
            }
            if (y < h-1)
            {
                if (x > 0)   val |= bitmap[((y+1) * w) + x-1];
                val |= bitmap[((y+1) * w) + x];
                if (x < w-1) val |= bitmap[((y+1) * w) + x+1];
            }

            if ((val >> 24) == 0xFF)
                bitmap[(y * w) + x] = shadow;
        }
    }

    item->rainbowend = (int)rainbowinc;
}

void ScreenPanel::osdDeleteItem(OSDItem* item)
{
}

void ScreenPanel::osdSetEnabled(bool enabled)
{
    osdMutex.lock();
    osdEnabled = enabled;
    osdMutex.unlock();
}

void ScreenPanel::osdAddMessage(unsigned int color, const char* text)
{
    if (!osdEnabled) return;

    osdMutex.lock();

    OSDItem item;

    item.id = (osdID++) & 0x7FFFFFFF;
    item.timestamp = QDateTime::currentMSecsSinceEpoch();
    strncpy(item.text, text, 255); item.text[255] = '\0';
    item.color = color;
    item.rendered = false;
    item.rainbowstart = -1;

    osdItems.push_back(item);

    osdMutex.unlock();
}

void ScreenPanel::osdUpdate()
{
    osdMutex.lock();

    qint64 tick_now = QDateTime::currentMSecsSinceEpoch();
    qint64 tick_min = tick_now - 2500;

    for (auto it = osdItems.begin(); it != osdItems.end(); )
    {
        OSDItem& item = *it;

        if ((!osdEnabled) || (item.timestamp < tick_min))
        {
            osdDeleteItem(&item);
            it = osdItems.erase(it);
            continue;
        }

        if (!item.rendered)
        {
            osdRenderItem(&item);
            item.rendered = true;
        }

        it++;
    }

    // render splashscreen text items if needed

    int rainbowinc = -1;
    bool needrecalc = false;

    for (int i = 0; i < 3; i++)
    {
        if (!splashText[i].rendered)
        {
            splashText[i].rainbowstart = rainbowinc;
            osdRenderItem(&splashText[i]);
            splashText[i].rendered = true;
            rainbowinc = splashText[i].rainbowend;
            needrecalc = true;
        }
    }

    osdMutex.unlock();

    if (needrecalc)
        calcSplashLayout();
}

void ScreenPanel::calcSplashLayout()
{
    if (!splashText[0].rendered)
        return;

    osdMutex.lock();

    int w = width();
    int h = height();

    int xlogo = (w - kLogoWidth) / 2;
    int ylogo = (h - kLogoWidth) / 2;

    // top text
    int totalwidth = splashText[0].bitmap.width() + 6 + splashText[1].bitmap.width();
    if (totalwidth >= w)
    {
        // stacked vertically
        splashPos[0].setX((width() - splashText[0].bitmap.width()) / 2);
        splashPos[1].setX((width() - splashText[1].bitmap.width()) / 2);

        int basey = ylogo / 2;
        splashPos[0].setY(basey - splashText[0].bitmap.height() - 1);
        splashPos[1].setY(basey + 1);
    }
    else
    {
        // horizontal
        splashPos[0].setX((w - totalwidth) / 2);
        splashPos[1].setX(splashPos[0].x() + splashText[0].bitmap.width() + 6);

        int basey = (ylogo - splashText[0].bitmap.height()) / 2;
        splashPos[0].setY(basey);
        splashPos[1].setY(basey);
    }

    // bottom text
    splashPos[2].setX((w - splashText[2].bitmap.width()) / 2);
    splashPos[2].setY(ylogo + kLogoWidth + ((ylogo - splashText[2].bitmap.height()) / 2));

    // logo
    splashPos[3].setX(xlogo);
    splashPos[3].setY(ylogo);

    osdMutex.unlock();
}



ScreenPanelNative::ScreenPanelNative(QWidget* parent) : ScreenPanel(parent)
{
    RenderCost.Enabled = RenderCostEnabled();

    hasBuffers = false;

    screen[0] = QImage(256, 192, QImage::Format_RGB32);
    screen[1] = QImage(256, 192, QImage::Format_RGB32);
    screen[0].fill(Qt::black);
    screen[1].fill(Qt::black);

    screenTrans[0].reset();
    screenTrans[1].reset();
}

ScreenPanelNative::~ScreenPanelNative()
{
    deinitVulkan();
    if (RenderCost.Enabled && RenderCost.Paints)
    {
        char line[1536];
        RenderCost.Report(line, sizeof(line), mainWindow->getWindowID());
        Platform::Log(Platform::LogLevel::Info, "%s\n", line);
    }
}

void ScreenPanelNative::deinitVulkan()
{
    if (!vulkan) return;
    QMutexLocker lock(&emuInstance->renderLock);
#ifdef VULKANRENDERER_ENABLED
    if (auto* nds = emuInstance->getNDS())
        if (auto* renderer = dynamic_cast<VulkanRenderer*>(&nds->GetRenderer()))
            renderer->DisableDirectDisplay("RAM presenter", false);
#endif
    vulkan.reset();
}

bool ScreenPanelNative::initVulkan()
{
    std::string error;
    vulkan = ::Vulkan::Presenter::Create(reinterpret_cast<void*>(winId()), error,
        emuInstance->getGlobalConfig().GetString("Video.GPU"));
    if (!vulkan) {
        Platform::Log(Platform::LogLevel::Warn, "Vulkan output unavailable: %s\n", error.c_str());
        return false;
    }
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_OpaquePaintEvent);
    return true;
}

QPaintEngine* ScreenPanelNative::paintEngine() const
{
    return vulkan ? nullptr : ScreenPanel::paintEngine();
}

void ScreenPanelNative::invalidatePresentedFrame()
{
    bufferLock.lock();
    screenGeneration = 0;
    bufferLock.unlock();
}

void ScreenPanelNative::setupScreenLayout()
{
    ScreenPanel::setupScreenLayout();

    for (int i = 0; i < numScreens; i++)
    {
        float* mtx = screenMatrix[i];
        screenTrans[i].setMatrix(mtx[0], mtx[1], 0.f,
                                 mtx[2], mtx[3], 0.f,
                                 mtx[4], mtx[5], 1.f);
    }
}

bool ScreenPanelNative::drawScreen()
{
    auto emuThread = emuInstance->getEmuThread();
    if (!emuThread->emuIsActive())
    {
        hasBuffers = false;
        return true;
    }

    auto nds = emuInstance->getNDS();
    assert(nds != nullptr);

    bufferLock.lock();
    if (!preservedFrame[0].isNull() && nds->NumFrames == preservedFrameNumber)
    {
        screen[0] = preservedFrame[0];
        screen[1] = preservedFrame[1];
        hasBuffers = false;
        screenGeneration = 0;
        bufferLock.unlock();
        return true;
    }
    preservedFrame = {};
#ifdef VULKANRENDERER_ENABLED
    // Native Vulkan borrows the current images only while paint holds renderLock.
    // Querying CpuBGRA here would download a frame that paint can consume directly.
    if (dynamic_cast<VulkanRenderer*>(&nds->GetRenderer()))
    {
        hasBuffers = true;
        bufferLock.unlock();
        return true;
    }
#endif
    melonDS::Renderer::DisplayFrame frame;
    hasBuffers = nds->GetRenderer().GetDisplayFrame(frame) &&
        frame.kind == melonDS::Renderer::DisplayFrame::Kind::CpuBGRA;
    bufferLock.unlock();
    return true;
}

#ifdef VULKANRENDERER_ENABLED
void ScreenPanelNative::paintVulkan()
{
    const auto paintStart = RenderCost.Start();
    const float ratio = devicePixelRatioF();
    const QSize pixels = (QSizeF(size()) * ratio).toSize();
    auto* thread = emuInstance->getEmuThread();
    std::vector<::Vulkan::Presenter::Texture> textures;
    std::vector<::Vulkan::Presenter::Quad> quads;
    std::vector<QImage> overlays;
    std::string error;
    bool failed = false;
    {
        // Shared-device queue access and borrowed framebuffer publication both
        // require the same lock as RunFrame and renderer replacement.
        QMutexLocker renderLocker(&emuInstance->renderLock);
        QMutexLocker bufferLocker(&bufferLock);
        auto* nds = emuInstance->getNDS();
        auto* renderer = nds ? dynamic_cast<VulkanRenderer*>(&nds->GetRenderer()) : nullptr;
        bool resident = false;
        if (thread->emuIsActive() && renderer && preservedFrame[0].isNull())
        {
            auto device = renderer->DisplayDevice();
            if (device && vulkanAttempt.lock() != device)
            {
                vulkanAttempt = device;
                auto replacement = ::Vulkan::Presenter::CreateShared(reinterpret_cast<void*>(winId()), device, error);
                if (replacement) vulkan = std::move(replacement);
                else renderer->DisableDirectDisplay(error);
            }
            if (vulkan->UsesDevice(device) && renderer->EnableDirectDisplay())
            {
                VulkanRenderer::ResidentFrame frame;
                if (renderer->GetResidentFrame(frame))
                {
                    for (const auto& image : frame.images)
                        textures.push_back({nullptr, frame.width, frame.height, 0, image});
                    resident = true;
                }
            }
            thread->setVulkanDisplayStatus(QString::fromStdString(renderer->DirectDisplayStatus()));
        }
        if (thread->emuIsActive())
        {
            if (!resident)
            {
                melonDS::Renderer::DisplayFrame frame;
                if (hasBuffers && nds && nds->GetRenderer().GetDisplayFrame(frame) &&
                    frame.kind == melonDS::Renderer::DisplayFrame::Kind::CpuBGRA && frame.top && frame.bottom)
                {
                    textures.push_back({frame.top, frame.width, frame.height, frame.width * 4, {}});
                    textures.push_back({frame.bottom, frame.width, frame.height, frame.width * 4, {}});
                }
                else
                    for (const auto& image : screen)
                        textures.push_back({image.constBits(), uint32_t(image.width()), uint32_t(image.height()), uint32_t(image.bytesPerLine()), {}});
            }
            for (int i = 0; i < numScreens; ++i)
            {
                const auto& m = screenMatrix[i];
                quads.push_back({uint32_t(screenKind[i]), {m[0]*256*ratio, m[1]*256*ratio,
                    m[2]*192*ratio, m[3]*192*ratio, m[4]*ratio, m[5]*ratio}, filter});
            }
        }
        bufferLocker.unlock();
        osdUpdate();
        QMutexLocker osdLocker(&osdMutex);
        const auto overlay = [&](const QImage& image, QPoint position, QSize size) {
            if (image.isNull()) return;
            overlays.push_back(image.convertToFormat(QImage::Format_ARGB32_Premultiplied));
            const auto& converted = overlays.back();
            quads.push_back({uint32_t(textures.size()), {size.width()*ratio, 0, 0, size.height()*ratio,
                position.x()*ratio, position.y()*ratio}, false});
            textures.push_back({converted.constBits(), uint32_t(converted.width()), uint32_t(converted.height()), uint32_t(converted.bytesPerLine()), {}});
        };
        if (!thread->emuIsActive())
        {
            overlay(splashLogo.toImage(), splashPos[3], QSize(kLogoWidth, kLogoWidth));
            for (int i = 0; i < 3; ++i) overlay(splashText[i].bitmap, splashPos[i], splashText[i].bitmap.size());
        }
        if (osdEnabled)
        {
            int y = kOSDMargin;
            for (const auto& item : osdItems)
            {
                overlay(item.bitmap, QPoint(kOSDMargin, y), item.bitmap.size());
                y += item.bitmap.height();
            }
        }
        const auto presentStart = RenderCost.Start();
        const auto before = vulkan->GetDiagnostics();
        failed = vulkan->Present(pixels.width(), pixels.height(), textures, quads, error) == ::Vulkan::Presenter::Result::Failed;
        RenderCost.RecordPresent(presentStart, pixels.width(), pixels.height(), before, vulkan->GetDiagnostics());
        if (failed)
        {
            if (renderer)
            {
                renderer->DisableDirectDisplay(error);
                thread->setVulkanDisplayStatus(QString::fromStdString(renderer->DirectDisplayStatus()));
            }
            vulkan.reset();
        }
    }
    if (failed)
    {
        Platform::Log(Platform::LogLevel::Warn, "Vulkan output lost: %s\n", error.c_str());
        setAttribute(Qt::WA_PaintOnScreen, false);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        osdAddMessage(0xFF8080, "Vulkan output failed; using native display");
        update();
    }
    RenderCost.PaintEnd(paintStart);
    if (RenderCost.TakeReport())
    {
        char line[1536];
        RenderCost.Report(line, sizeof(line), mainWindow->getWindowID());
        Platform::Log(Platform::LogLevel::Info, "%s\n", line);
    }
}
#endif


void ScreenPanelNative::paintEvent(QPaintEvent* event)
{
#ifdef VULKANRENDERER_ENABLED
    if (vulkan) { paintVulkan(); return; }
#endif
    const auto paintStart = RenderCost.Start();
    std::uint64_t painterTime = RenderCost.Start();
    // Vulkan's first stage presents the existing Software composition. CPU
    // images are copied under the same render lock; the presenter owns every
    // staging buffer until its submission fence completes.
    QImage& composed = vulkanFrame;
    QPainter painter;
    if (vulkan) {
        const qreal scale = devicePixelRatioF();
        const QSize pixels = (QSizeF(size()) * scale).toSize();
        if (composed.size() != pixels) composed = QImage(pixels, QImage::Format_RGB32);
        if (composed.isNull()) {
            RenderCost.RecordPainter(painterTime);
            RenderCost.PaintEnd(paintStart);
            return;
        }
        RenderCost.Window(pixels.width(), pixels.height());
        composed.setDevicePixelRatio(scale);
        painter.begin(&composed);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, filter);
    } else {
        if (RenderCost.Enabled) {
            const QSize pixels = (QSizeF(size()) * devicePixelRatioF()).toSize();
            RenderCost.Window(pixels.width(), pixels.height());
        }
        painter.begin(this);
    }

    // Vulkan submits a whole window image, including on partial expose.
    painter.fillRect(vulkan ? rect() : event->rect(), QColor::fromRgb(0, 0, 0));
    RenderCost.RecordPainter(painterTime);

    auto emuThread = emuInstance->getEmuThread();
    
    if (emuThread->emuIsActive())
    {
        emuInstance->renderLock.lock();

        bufferLock.lock();
        melonDS::Renderer::DisplayFrame frame;
        auto* nds = emuInstance->getNDS();
        if (hasBuffers)
        {
            // drawScreen may have run before a scale/renderer change.
            // Re-query under the lock protecting renderer replacement and keep
            // the borrowed payload only until this paint's copy is complete.
            hasBuffers = nds && nds->GetRenderer().GetDisplayFrame(frame) &&
                frame.kind == melonDS::Renderer::DisplayFrame::Kind::CpuBGRA &&
                frame.top && frame.bottom && frame.width > 0 && frame.height > 0;
            if (!hasBuffers) screenGeneration = 0;
        }
        if (hasBuffers)
        {
            const int bufferWidth = frame.width, bufferHeight = frame.height;
            if (screen[0].size() != QSize(bufferWidth, bufferHeight))
            {
                QImage top(bufferWidth, bufferHeight, QImage::Format_RGB32);
                QImage bottom(bufferWidth, bufferHeight, QImage::Format_RGB32);
                if (top.isNull() || bottom.isNull()) hasBuffers = false;
                else { screen[0] = std::move(top); screen[1] = std::move(bottom); }
                // Reallocation discards the previously copied pixels.
                screenGeneration = 0;
            }
            if (hasBuffers)
            {
                const size_t bytes = size_t(bufferWidth) * bufferHeight * sizeof(melonDS::u32);
                // The same NDS instance publishing the same nonzero generation
                // at the same extent means the owned pixels already match.
                const std::uint64_t cached = screenGeneration;
                const bool reusable = cached != 0 &&
                    cached == frame.generation &&
                    screenGenerationNDS == nds &&
                    screenGenerationTop == frame.top;
                if (reusable)
                {
                    RenderCost.RecordCopy(RenderCost.Start(), 0, frame.generation);
                }
                else
                {
                    std::uint64_t copy = RenderCost.Enabled ? RenderCostNowNs() : 0;
                    memcpy(screen[0].scanLine(0), frame.top, bytes);
                    memcpy(screen[1].scanLine(0), frame.bottom, bytes);
                    RenderCost.RecordCopy(copy, 2 * bytes, frame.generation);
                    // Publish validity only after the copy completes.
                    screenGenerationNDS = nds;
                    screenGenerationTop = frame.top;
                    screenGeneration = frame.generation;
                }
            }
        }
        bufferLock.unlock();

        painterTime = RenderCost.Start();
        QRect screenrc(0, 0, 256, 192);

        for (int i = 0; i < numScreens; i++)
        {
            painter.setTransform(screenTrans[i]);
            painter.drawImage(screenrc, screen[screenKind[i]]);
        }
        RenderCost.RecordPainter(painterTime);
        emuInstance->renderLock.unlock();
    }

    osdUpdate();

    if (!emuThread->emuIsActive())
    {
        // splashscreen
        osdMutex.lock();
        painterTime = RenderCost.Start();

        painter.drawPixmap(QRect(splashPos[3], QSize(kLogoWidth, kLogoWidth)), splashLogo);

        for (int i = 0; i < 3; i++)
            painter.drawImage(splashPos[i], splashText[i].bitmap);

        RenderCost.RecordPainter(painterTime);
        osdMutex.unlock();
    }

    if (osdEnabled)
    {
        osdMutex.lock();
        painterTime = RenderCost.Start();

        u32 y = kOSDMargin;

        painter.resetTransform();

        for (auto it = osdItems.begin(); it != osdItems.end(); )
        {
            OSDItem& item = *it;

            painter.drawImage(kOSDMargin, y, item.bitmap);

            y += item.bitmap.height();
            it++;
        }

        RenderCost.RecordPainter(painterTime);
        osdMutex.unlock();
    }

    painterTime = RenderCost.Start();
    painter.end();
    RenderCost.RecordPainter(painterTime);
    if (vulkan) {
        std::string error;
        const auto presentStart = RenderCost.Start();
        const auto before = presentStart ? vulkan->GetDiagnostics() : ::Vulkan::Presenter::Diagnostics{};
        const auto result = vulkan->Present(composed.constBits(), composed.width(),
                                             composed.height(), composed.bytesPerLine(), error);
        if (presentStart) RenderCost.RecordPresent(presentStart, composed.width(), composed.height(),
            before, vulkan->GetDiagnostics());
        if (result == ::Vulkan::Presenter::Result::Failed) {
            Platform::Log(Platform::LogLevel::Warn, "Vulkan output lost: %s\n", error.c_str());
            vulkan.reset();
            setAttribute(Qt::WA_PaintOnScreen, false);
            setAttribute(Qt::WA_OpaquePaintEvent, false);
            osdAddMessage(0xFF8080, "Vulkan output failed; using native display");
            update();
        }
    }

    RenderCost.PaintEnd(paintStart);
    if (RenderCost.TakeReport())
    {
        char line[1536];
        RenderCost.Report(line, sizeof(line), mainWindow->getWindowID());
        Platform::Log(Platform::LogLevel::Info, "%s\n", line);
    }
}



ScreenPanelGL::ScreenPanelGL(QWidget* parent) : ScreenPanel(parent)
{
    setAutoFillBackground(false);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_KeyCompression, false);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(screenGetMinSize());

    glInited = false;
    RenderCost.SetEnabled(RenderCostEnabled());
}

ScreenPanelGL::~ScreenPanelGL()
{
    if (RenderCost.Enabled && RenderCost.Frames)
    {
        char line[512];
        RenderCost.Report(line, sizeof(line), mainWindow->getWindowID());
        Platform::Log(Platform::LogLevel::Info, "%s\n", line);
    }
}

bool ScreenPanelGL::createContext()
{
    std::optional<WindowInfo> windowinfo = getWindowInfo();

    // if our parent window is parented to another window, we will
    // share our OpenGL context with that window
    MainWindow* ourwin = (MainWindow*)parentWidget();
    MainWindow* parentwin = (MainWindow*)parentWidget()->parentWidget();
    //if (parentwin)
    if (ourwin->getWindowID() != 0)
    {
        if (windowinfo.has_value())
            glContext = parentwin->getOGLContext()->CreateSharedContext(*windowinfo);
    }
    else
    {
        std::array<GL::Context::Version, 2> versionsToTry = {
                GL::Context::Version{GL::Context::Profile::Core, 4, 3},
                GL::Context::Version{GL::Context::Profile::Core, 3, 2}};
        if (windowinfo.has_value())
            glContext = GL::Context::Create(*windowinfo, versionsToTry);
    }

    // No worker or core owns this new context yet. Dispose of a failed handoff
    // on its creating GUI thread and use the existing native fallback.
    if (glContext && !glContext->DoneCurrent()) glContext.reset();
    // A new context means new texture objects; nothing uploaded survives.
    invalidatePresentedFrame();
    return glContext != nullptr;
}

void ScreenPanelGL::setSwapInterval(int intv)
{
    if (!glContext) return;

    pendingSwapInterval = intv;
}

bool ScreenPanelGL::initOpenGL()
{
    if (glInited) return true;
    if (!glContext || !glContext->MakeCurrent()) return false;
    glOwned = true;

    if (!OpenGL::CompileVertexFragmentProgram(screenShaderProgram,
                                         kScreenVS, kScreenFS,
                                         "ScreenShader",
                                         {{"vPosition", 0}, {"vTexcoord", 1}},
                                         {{"oColor", 0}}))
    {
        deinitOpenGL();
        return false;
    }

    glUseProgram(screenShaderProgram);
    glUniform1i(glGetUniformLocation(screenShaderProgram, "TopScreenTex"), 0);
    glUniform1i(glGetUniformLocation(screenShaderProgram, "BottomScreenTex"), 1);

    screenShaderScreenSizeULoc = glGetUniformLocation(screenShaderProgram, "uScreenSize");
    screenShaderTransformULoc = glGetUniformLocation(screenShaderProgram, "uTransform");

    const float vertices[] =
    {
        0.f,   0.f,    0.f, 0.f, 0.f,
        0.f,   192.f,  0.f, 1.f, 0.f,
        256.f, 192.f,  1.f, 1.f, 0.f,
        0.f,   0.f,    0.f, 0.f, 0.f,
        256.f, 192.f,  1.f, 1.f, 0.f,
        256.f, 0.f,    1.f, 0.f, 0.f,

        0.f,   0.f,    0.f, 0.f, 1.f,
        0.f,   192.f,  0.f, 1.f, 1.f,
        256.f, 192.f,  1.f, 1.f, 1.f,
        0.f,   0.f,    0.f, 0.f, 1.f,
        256.f, 192.f,  1.f, 1.f, 1.f,
        256.f, 0.f,    1.f, 0.f, 1.f
    };

    glGenBuffers(1, &screenVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, screenVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenVertexArrays(1, &screenVertexArray);
    glBindVertexArray(screenVertexArray);
    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5*4, (void*)(0));
    glEnableVertexAttribArray(1); // texcoord
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5*4, (void*)(2*4));

    glGenTextures(1, &screenTexture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, screenTexture);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, 256, 192, 2, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    screenTextureWidth = 256;
    screenTextureHeight = 192;
    screenTextureGeneration = 0;


    if (!OpenGL::CompileVertexFragmentProgram(osdShader,
                                         kScreenVS_OSD, kScreenFS_OSD,
                                         "OSDShader",
                                         {{"vPosition", 0}},
                                         {{"oColor", 0}}))
    {
        deinitOpenGL();
        return false;
    }

    glUseProgram(osdShader);
    glUniform1i(glGetUniformLocation(osdShader, "OSDTex"), 0);

    osdScreenSizeULoc = glGetUniformLocation(osdShader, "uScreenSize");
    osdPosULoc = glGetUniformLocation(osdShader, "uOSDPos");
    osdSizeULoc = glGetUniformLocation(osdShader, "uOSDSize");
    osdScaleFactorULoc = glGetUniformLocation(osdShader, "uScaleFactor");
    osdTexScaleULoc = glGetUniformLocation(osdShader, "uTexScale");

    const float osdvertices[6*2] =
    {
        0, 0,
        1, 1,
        1, 0,
        0, 0,
        0, 1,
        1, 1
    };

    glGenBuffers(1, &osdVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, osdVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(osdvertices), osdvertices, GL_STATIC_DRAW);

    glGenVertexArrays(1, &osdVertexArray);
    glBindVertexArray(osdVertexArray);
    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)(0));

    // splash logo texture
    QImage logo = splashLogo.scaled(kLogoWidth*2, kLogoWidth*2).toImage();
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logo.width(), logo.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, logo.bits());
    logoTexture = tex;

    transferLayout();
    glInited = true;
    return true;
}

bool ScreenPanelGL::deinitOpenGL()
{
    // Failed initialization can own only a prefix of these objects.
    if (!glOwned) return true;
    if (!glContext || !glContext->MakeCurrent()) return false;

    RenderCost.Gpu.Shutdown();

    glDeleteTextures(1, &screenTexture);

    glDeleteVertexArrays(1, &screenVertexArray);
    glDeleteBuffers(1, &screenVertexBuffer);

    glDeleteProgram(screenShaderProgram);


    for (const auto& [key, tex] : osdTextures)
    {
        glDeleteTextures(1, &tex);
    }
    osdTextures.clear();

    glDeleteVertexArrays(1, &osdVertexArray);
    glDeleteBuffers(1, &osdVertexBuffer);

    glDeleteTextures(1, &logoTexture);

    glDeleteProgram(osdShader);

    screenTexture = screenVertexArray = screenVertexBuffer = screenShaderProgram = 0;
    osdVertexArray = osdVertexBuffer = logoTexture = osdShader = 0;

    // Bitmaps survive a GL restart, but their uploaded textures do not.
    osdMutex.lock();
    for (auto& item : osdItems) item.rendered = false;
    for (auto& item : splashText) item.rendered = false;
    osdMutex.unlock();

    lastScreenWidth = lastScreenHeight = -1;
    screenTextureGeneration = 0;
    glInited = false;
    if (!glContext->DoneCurrent()) return false;
    glOwned = false;
    return true;
}

bool ScreenPanelGL::makeCurrentGL()
{
    return glContext && glContext->MakeCurrent();
}

bool ScreenPanelGL::releaseGL()
{
    // A successful deinit already released this panel's worker ownership.
    return !glOwned || !glContext || glContext->DoneCurrent();
}

void ScreenPanelGL::invalidatePresentedFrame()
{
    screenTextureGeneration = 0;
}

void ScreenPanelGL::osdRenderItem(OSDItem* item)
{
    ScreenPanel::osdRenderItem(item);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, item->bitmap.width(), item->bitmap.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, item->bitmap.bits());

    osdTextures[item->id] = tex;
}

void ScreenPanelGL::osdDeleteItem(OSDItem* item)
{
    if (osdTextures.count(item->id))
    {
        GLuint tex = osdTextures[item->id];
        glDeleteTextures(1, &tex);
        osdTextures.erase(item->id);
    }

    ScreenPanel::osdDeleteItem(item);
}

bool ScreenPanelGL::drawScreen()
{
    // Deinit is acknowledged before the GUI replaces or removes the panel.
    // Paused frames can still visit it before the ownership barrier is acquired.
    if (!glContext || !glInited) return true;

    auto emuThread = emuInstance->getEmuThread();

    if (!glContext->MakeCurrent()) return false;

    RenderCost.FrameBegin();
    const int span = RenderCost.Gpu.Begin(RenderCost.GpuPresent);
    std::uint64_t issue = RenderCost.Start();

    // WGL/EGL apply the interval to the current window. Settings can be
    // broadcast while another panel is current, so apply them when drawing.
    if (pendingSwapInterval)
    {
        glContext->SetSwapInterval(*pendingSwapInterval);
        pendingSwapInterval.reset();
    }

    int w = windowInfo.surface_width;
    int h = windowInfo.surface_height;
    float factor = windowInfo.surface_scale;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glViewport(0, 0, w, h);

    if (emuThread->emuIsActive())
    {
        auto nds = emuInstance->getNDS();

        glUseProgram(screenShaderProgram);
        glUniform2f(screenShaderScreenSizeULoc, w / factor, h / factor);

        using DisplayFrame = melonDS::Renderer::DisplayFrame;
        DisplayFrame frame;
        bool available = true;
        if (!preservedFrame[0].isNull() && nds->NumFrames == preservedFrameNumber)
        {
            frame = {DisplayFrame::Kind::CpuBGRA, preservedFrame[0].constBits(),
                preservedFrame[1].constBits(), u32(preservedFrame[0].width()),
                u32(preservedFrame[0].height()), 0};
        }
        else
        {
            preservedFrame = {};
            available = nds->GetRenderer().GetDisplayFrame(frame);
        }
        if (available && frame.top && frame.width && frame.height &&
            ((frame.kind == DisplayFrame::Kind::CpuBGRA && frame.bottom) ||
             frame.kind == DisplayFrame::Kind::GLTexture2DArray))
        {
            const int frameWidth = frame.width, frameHeight = frame.height;
            if (frame.kind == DisplayFrame::Kind::CpuBGRA)
            {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, screenTexture);

                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                if (frameWidth != screenTextureWidth || frameHeight != screenTextureHeight)
                {
                    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, frameWidth, frameHeight, 2,
                        0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
                    if (glGetError() != GL_NO_ERROR)
                    {
                        // Do not upload using uncommitted dimensions after a failed
                        // resize. The caller owns presentation fallback/recovery.
                        screenTextureGeneration = 0;
                        RenderCost.Add(RenderCost.AccIssue, issue);
                        RenderCost.Gpu.End(span);
                        RenderCost.FrameEnd();
                        return false;
                    }
                    screenTextureWidth = frameWidth;
                    screenTextureHeight = frameHeight;
                    // Reallocation discards the previously uploaded pixels.
                    screenTextureGeneration = 0;
                }
                // The same NDS instance publishing the same nonzero generation
                // at the same extent means the texture already holds it.
                const std::uint64_t cached = screenTextureGeneration;
                const bool reusable = cached != 0 &&
                    cached == frame.generation &&
                    screenTextureGenerationNDS == nds &&
                    screenTextureGenerationTop == frame.top;
                if (reusable)
                {
                    RenderCost.UploadEnd(RenderCost.UploadStart(), 0);
                }
                else
                {
                    std::uint64_t upl = RenderCost.UploadStart();
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, frameWidth, frameHeight, 1, GL_BGRA,
                                    GL_UNSIGNED_BYTE, frame.top);
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, frameWidth, frameHeight, 1, GL_BGRA,
                                    GL_UNSIGNED_BYTE, frame.bottom);
                    RenderCost.UploadEnd(upl, size_t(2) * frameWidth * frameHeight * 4);
                    // Publish validity only after the upload is issued; 0
                    // (preserved images) is never cacheable.
                    screenTextureGenerationNDS = nds;
                    screenTextureGenerationTop = frame.top;
                    screenTextureGeneration = frame.generation;
                }
            }
            else if (frame.kind == DisplayFrame::Kind::GLTexture2DArray)
            {
                const GLuint texid = *static_cast<const GLuint*>(frame.top);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, texid);
                // screenTexture no longer reflects any cached CpuBGRA frame.
                screenTextureGeneration = 0;
            }

            screenSettingsLock.lock();

            GLint filter = this->filter ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, filter);

            glBindBuffer(GL_ARRAY_BUFFER, screenVertexBuffer);
            glBindVertexArray(screenVertexArray);

            for (int i = 0; i < numScreens; i++)
            {
                glUniformMatrix2x3fv(screenShaderTransformULoc, 1, GL_TRUE, screenMatrix[i]);
                glDrawArrays(GL_TRIANGLES, screenKind[i] == 0 ? 0 : 2 * 3, 2 * 3);
            }

            screenSettingsLock.unlock();
        }
    }

    osdUpdate();

    if (!emuThread->emuIsActive())
    {
        // splashscreen
        osdMutex.lock();

        glUseProgram(osdShader);

        glUniform2f(osdScreenSizeULoc, w, h);
        glUniform1f(osdScaleFactorULoc, factor);
        glUniform1f(osdTexScaleULoc, 2.0);

        glBindBuffer(GL_ARRAY_BUFFER, osdVertexBuffer);
        glBindVertexArray(osdVertexArray);

        glActiveTexture(GL_TEXTURE0);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        glBindTexture(GL_TEXTURE_2D, logoTexture);
        glUniform2i(osdPosULoc, splashPos[3].x(), splashPos[3].y());
        glUniform2i(osdSizeULoc, kLogoWidth, kLogoWidth);
        glDrawArrays(GL_TRIANGLES, 0, 2*3);

        glUniform1f(osdTexScaleULoc, 1.0);

        for (int i = 0; i < 3; i++)
        {
            OSDItem& item = splashText[i];

            if (!osdTextures.count(item.id))
                continue;

            glBindTexture(GL_TEXTURE_2D, osdTextures[item.id]);
            glUniform2i(osdPosULoc, splashPos[i].x(), splashPos[i].y());
            glUniform2i(osdSizeULoc, item.bitmap.width(), item.bitmap.height());
            glDrawArrays(GL_TRIANGLES, 0, 2*3);
        }

        glDisable(GL_BLEND);
        glUseProgram(0);

        osdMutex.unlock();
    }

    if (osdEnabled)
    {
        osdMutex.lock();

        u32 y = kOSDMargin;

        glUseProgram(osdShader);

        glUniform2f(osdScreenSizeULoc, w, h);
        glUniform1f(osdScaleFactorULoc, factor);
        glUniform1f(osdTexScaleULoc, 1.0);

        glBindBuffer(GL_ARRAY_BUFFER, osdVertexBuffer);
        glBindVertexArray(osdVertexArray);

        glActiveTexture(GL_TEXTURE0);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        for (const OSDItem& item : osdItems)
        {
            if (!osdTextures.count(item.id))
                continue;

            glBindTexture(GL_TEXTURE_2D, osdTextures[item.id]);
            glUniform2i(osdPosULoc, kOSDMargin, y);
            glUniform2i(osdSizeULoc, item.bitmap.width(), item.bitmap.height());
            glDrawArrays(GL_TRIANGLES, 0, 2*3);

            y += item.bitmap.height();
        }

        glDisable(GL_BLEND);
        glUseProgram(0);

        osdMutex.unlock();
    }

    RenderCost.Add(RenderCost.AccIssue, issue);
    RenderCost.Gpu.End(span);

    std::uint64_t wait = RenderCost.Start();
    bool ret = glContext->SwapBuffers();
    RenderCost.Add(RenderCost.AccWait, wait);

    RenderCost.FrameEnd();
    if (RenderCost.TakeReport())
    {
        char line[512];
        RenderCost.Report(line, sizeof(line), mainWindow->getWindowID());
        Platform::Log(Platform::LogLevel::Info, "%s\n", line);
    }

    return ret;
}

qreal ScreenPanelGL::devicePixelRatioFromScreen() const
{
    const QScreen* screen_for_ratio = window()->windowHandle()->screen();
    if (!screen_for_ratio)
        screen_for_ratio = QGuiApplication::primaryScreen();

    return screen_for_ratio ? screen_for_ratio->devicePixelRatio() : static_cast<qreal>(1);
}

int ScreenPanelGL::scaledWindowWidth() const
{
  return std::max(static_cast<int>(std::ceil(static_cast<qreal>(width()) * devicePixelRatioFromScreen())), 1);
}

int ScreenPanelGL::scaledWindowHeight() const
{
  return std::max(static_cast<int>(std::ceil(static_cast<qreal>(height()) * devicePixelRatioFromScreen())), 1);
}

std::optional<WindowInfo> ScreenPanelGL::getWindowInfo()
{
    WindowInfo wi;

    // Windows and Apple are easy here since there's no display connection.
    #if defined(_WIN32)
    wi.type = WindowInfo::Type::Win32;
    wi.window_handle = reinterpret_cast<void*>(winId());
    #elif defined(__APPLE__)
    wi.type = WindowInfo::Type::MacOS;
    wi.window_handle = reinterpret_cast<void*>(winId());
    #else
    const QString platform_name = QGuiApplication::platformName();

    #if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (platform_name == QStringLiteral("xcb"))
    {
        wi.type = WindowInfo::Type::X11;
        const QX11Application* x11 = qApp->nativeInterface<QX11Application>();
        wi.display_connection = x11->display();
        wi.window_handle = reinterpret_cast<void*>(winId());
    }
    #if defined(WAYLAND_ENABLED)
    else if (platform_name == QStringLiteral("wayland"))
    {
        wi.type = WindowInfo::Type::Wayland;
        const QWaylandApplication* wl = qApp->nativeInterface<QWaylandApplication>();
        wi.display_connection = wl->display();
        wi.window_handle = reinterpret_cast<void*>(winId());
    }
    #endif
    #else
    QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
    if (platform_name == QStringLiteral("xcb"))
    {
        wi.type = WindowInfo::Type::X11;
        wi.display_connection = pni->nativeResourceForWindow("display", windowHandle());
        wi.window_handle = reinterpret_cast<void*>(winId());
    }
    else if (platform_name == QStringLiteral("wayland"))
    {
        wi.type = WindowInfo::Type::Wayland;
        QWindow* handle = windowHandle();
        if (handle == nullptr)
            return std::nullopt;

        wi.display_connection = pni->nativeResourceForWindow("display", handle);
        wi.window_handle = pni->nativeResourceForWindow("surface", handle);
    }
    #endif
    else
    {
        Platform::Log(Platform::LogLevel::Error, "Unknown PNI platform %s\n", platform_name.toStdString().c_str());
        return std::nullopt;
    }
    #endif

    wi.surface_width = static_cast<u32>(scaledWindowWidth());
    wi.surface_height = static_cast<u32>(scaledWindowHeight());
    wi.surface_scale = static_cast<float>(devicePixelRatioFromScreen());

    return wi;
}


QPaintEngine* ScreenPanelGL::paintEngine() const
{
  return nullptr;
}

void ScreenPanelGL::setupScreenLayout()
{
    ScreenPanel::setupScreenLayout();
    transferLayout();
}

void ScreenPanelGL::transferLayout()
{
    std::optional<WindowInfo> windowInfo = getWindowInfo();
    if (windowInfo.has_value())
    {
        screenSettingsLock.lock();

        if (lastScreenWidth != windowInfo->surface_width || lastScreenHeight != windowInfo->surface_height)
        {
            if (glContext)
                glContext->ResizeSurface(windowInfo->surface_width, windowInfo->surface_height);
            lastScreenWidth = windowInfo->surface_width;
            lastScreenHeight = windowInfo->surface_height;
        }

        this->windowInfo = *windowInfo;

        screenSettingsLock.unlock();
    }
}
