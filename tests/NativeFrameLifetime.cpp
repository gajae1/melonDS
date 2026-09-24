// SPDX-License-Identifier: GPL-3.0-or-later
// Actual paint method; renderer publication is replaced between draw and paint.
#include "GPU.h"
#include "RenderCost.h"
#include "frontend/graphics/vulkan/Presenter.h"
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPaintEvent>
#include <QMutex>
#include <QImage>
#include <QPixmap>
#include <QTransform>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>
using u32 = uint32_t;
namespace Platform {
enum class LogLevel { Warn, Info };
void Log(LogLevel, const char*, ...) {}
}
namespace Vulkan {
struct Presenter::Impl {};
Presenter::~Presenter() = default;
Presenter::Result Presenter::Present(const void*, uint32_t, uint32_t, uint32_t, std::string&) { return Result::Failed; }
Presenter::Diagnostics Presenter::GetDiagnostics() const { return {}; }
}
namespace melonDS { using u32 = ::u32; }
struct FrameSource {
    int width = 256, height = 192;
    std::vector<u32> top, bottom;
    bool available = true;
    bool nullTop = false, readWithoutLock = false;
    using DisplayFrame = melonDS::Renderer::DisplayFrame;
    DisplayFrame::Kind kind = DisplayFrame::Kind::CpuBGRA;
    unsigned queries = 0;
    QMutex* ownerLock = nullptr;
    bool GetDisplayFrame(DisplayFrame& frame) {
        ++queries;
        if (ownerLock && ownerLock->tryLock()) {
            readWithoutLock = true; ownerLock->unlock();
        }
        frame = {};
        if (!available) return false;
        frame = {kind, nullTop ? nullptr : top.data(), bottom.data(), u32(width), u32(height), 1};
        return true;
    }
};
struct Console { FrameSource renderer; auto& GetRenderer() { return renderer; } };
struct Worker { bool emuIsActive() const { return true; } };
struct Instance {
    QMutex renderLock; Worker worker; Console console;
    bool hasConsole = true;
    Worker* getEmuThread() { return &worker; }
    Console* getNDS() { return hasConsole ? &console : nullptr; }
};
struct Window { int getWindowID() const { return 0; } };
using Meter = melonDS::RenderCostNativeMeter;
using melonDS::RenderCostNowNs;
class ScreenPanelNative : public QWidget {
public:
    Meter RenderCost; Window window; Window* mainWindow = &window;
    Instance instance; Instance* emuInstance = &instance;
    std::unique_ptr<Vulkan::Presenter> vulkan;
    QImage vulkanFrame, screen[2]; QMutex bufferLock, osdMutex;
    bool hasBuffers = true, filter = false, osdEnabled = false;
    const void* screenGenerationNDS = nullptr;
    const void* screenGenerationTop = nullptr;
    std::uint64_t screenGeneration = 0;
    void* topBuffer = nullptr; void* bottomBuffer = nullptr;
    int bufferWidth = 256, bufferHeight = 192, numScreens = 1;
    int screenKind[2]{0,1}; QTransform screenTrans[2];
    struct OSDItem { QImage bitmap; };
    std::deque<OSDItem> osdItems;
    QPixmap splashLogo; QPoint splashPos[4]; OSDItem splashText[3];
    static constexpr int kLogoWidth = 128, kOSDMargin = 4;
    void osdUpdate() {}
    void osdAddMessage(u32, const char*) {}
    void paintEvent(QPaintEvent* event) override;
};
#include "nativeFramePaint.inc"
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    ScreenPanelNative panel; panel.resize(256,192);
    panel.RenderCost.Enabled = melonDS::RenderCostEnabled();
    uint64_t expectedCopyBytes = 0;
    std::vector<u32> retiredTop(256*192,0xFF102030), retiredBottom(256*192,0xFF506070);
    auto& current = panel.instance.console.renderer;
    current.ownerLock = &panel.instance.renderLock;
    for (int scale : {2,3,16,1,8,1}) {
        expectedCopyBytes += uint64_t(2) * 256 * 192 * scale * scale * sizeof(u32);
        panel.hasBuffers = true;
        panel.topBuffer = retiredTop.data(); panel.bottomBuffer = retiredBottom.data();
        panel.bufferWidth = 256; panel.bufferHeight = 192;
        current.width = 256*scale; current.height = 192*scale;
        current.top.assign(size_t(current.width)*current.height,0xFF3388CC);
        current.bottom.assign(current.top.size(),0xFF77BB11);
        // Every subpixel carries detail, not just a uniform test rectangle.
        for (size_t i = 0; i < current.top.size(); ++i) {
            current.top[i] ^= u32(i) & 0x00FFFFFF;
            current.bottom[i] ^= u32(i * 31) & 0x00FFFFFF;
        }
        QImage target(panel.size(),QImage::Format_RGB32); panel.render(&target);
        if (panel.screen[0].size() != QSize(current.width,current.height) ||
            panel.screen[1].size() != panel.screen[0].size() ||
            !std::equal(current.top.begin(),current.top.end(),reinterpret_cast<const u32*>(panel.screen[0].constBits())) ||
            !std::equal(current.bottom.begin(),current.bottom.end(),reinterpret_cast<const u32*>(panel.screen[1].constBits()))) {
            std::fprintf(stderr,"paint consumed retired framebuffer at %dx\n",scale); return 1;
        }
    }
    const QImage retained = panel.screen[0], retainedBottom = panel.screen[1];
    current.available = false; panel.hasBuffers = true;
    QImage target(panel.size(),QImage::Format_RGB32); panel.render(&target);
    if (panel.hasBuffers || panel.screen[0] != retained) {
        std::fprintf(stderr,"missing RAM framebuffer did not retain the previous image\n"); return 1;
    }
    current.available = true;
    for (unsigned failure = 0; failure < 4; ++failure) {
        panel.hasBuffers = true;
        current.nullTop = failure == 0;
        current.width = failure == 1 ? 0 : 256;
        panel.instance.hasConsole = failure != 2;
        current.kind = failure == 3 ? FrameSource::DisplayFrame::Kind::GLTexture2DArray :
            FrameSource::DisplayFrame::Kind::CpuBGRA;
        panel.render(&target);
        if (panel.hasBuffers || panel.screen[0] != retained || panel.screen[1] != retainedBottom) {
            std::fprintf(stderr,"invalid source discarded retained images (case %u)\n", failure); return 1;
        }
    }
    panel.instance.hasConsole = true;
    current.nullTop = false; current.width = 256;
    current.kind = FrameSource::DisplayFrame::Kind::CpuBGRA;
    const auto beforePreserved = current.queries;
    panel.hasBuffers = false; // Worker published a preserved paused image.
    panel.render(&target);
    if (current.queries != beforePreserved || current.readWithoutLock ||
        panel.screen[0] != retained || panel.screen[1] != retainedBottom) {
        std::fprintf(stderr,"paused image or renderer locking contract changed\n"); return 1;
    }
    if (panel.RenderCost.Enabled) {
        const auto& cost = panel.RenderCost;
        if (cost.Frames != 6 || cost.Copy.IntervalBytes != expectedCopyBytes || cost.LastGeneration != 1 ||
            cost.GenerationChanges != 1 || !cost.IntervalPaints || !cost.PainterNs ||
            cost.PaintNs < cost.CopyNs + cost.PainterNs + cost.PresentNs || cost.ActualCalls) {
            std::fputs("Native paint diagnostics miscounted copies, generation, or disjoint CPU spans\n", stderr); return 2;
        }
        char report[1536];
        panel.RenderCost.Report(report, sizeof(report), 0);
        std::puts(report);
    } else if (panel.RenderCost.Frames || panel.RenderCost.Paints || panel.RenderCost.Copy.Count) {
        std::fputs("Native paint diagnostics OFF recorded samples\n", stderr); return 3;
    }
    std::puts("Native paint: 1/2/3/8/16x subpixels, source loss, paused image and locked reacquisition; diagnostics accounting PASS");
    return 0;
}
