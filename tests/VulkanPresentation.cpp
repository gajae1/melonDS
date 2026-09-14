// SPDX-License-Identifier: GPL-3.0-or-later
// Real Vulkan WSI and visible, owned Qt window; no mock queue or device.
#include "graphics/vulkan/Presenter.h"
#include <QApplication>
#include <QWidget>
#include <QScreen>
#include <QImage>
#include <QPainter>
#include <QThread>
#include <cstdio>

class Surface : public QWidget {
    QPaintEngine* paintEngine() const override { return nullptr; }
    void paintEvent(QPaintEvent*) override {}
public:
    Surface() { setAttribute(Qt::WA_PaintOnScreen); setAttribute(Qt::WA_NativeWindow); }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    Surface window;
    window.setWindowTitle("melonDS Vulkan presentation validation");
    window.setWindowFlag(Qt::WindowStaysOnTopHint);
    window.resize(320, 240);
    window.move(app.primaryScreen()->availableGeometry().topLeft()+QPoint(32,32));
    window.show();
    app.processEvents();
    std::string error;
    if (Vulkan::Presenter::Create(nullptr, error)) return 1;
    auto presenter = Vulkan::Presenter::Create(reinterpret_cast<void*>(window.winId()), error);
    if (!presenter) { std::fprintf(stderr, "Vulkan unavailable: %s\n", error.c_str()); return 77; }
    unsigned presented = 0;
    for (const QSize logical : {QSize(320,240), QSize(513,287), QSize(256,384)}) {
        window.resize(logical);
        app.processEvents();
        const QSize physical = (QSizeF(window.size()) * window.devicePixelRatioF()).toSize();
        // Extra row padding verifies that source stride is respected.
        QImage storage(physical.width()+8, physical.height(), QImage::Format_RGB32);
        storage.fill(Qt::magenta);
        QPainter paint(&storage);
        paint.fillRect(QRect(0,0,physical.width()/2,physical.height()/2), Qt::red);
        paint.fillRect(QRect(physical.width()/2,0,physical.width()-physical.width()/2,physical.height()/2), Qt::green);
        paint.fillRect(QRect(0,physical.height()/2,physical.width()/2,physical.height()-physical.height()/2), Qt::blue);
        paint.fillRect(QRect(physical.width()/2,physical.height()/2,physical.width()-physical.width()/2,physical.height()-physical.height()/2), Qt::white);
        paint.end();
        unsigned count = 0;
        for (unsigned attempt = 0; attempt < 120 && count < 12; ++attempt) {
            app.processEvents();
            const auto result = presenter->Present(storage.constBits(), physical.width(), physical.height(), storage.bytesPerLine(), error);
            if (result == Vulkan::Presenter::Result::Failed) { std::fprintf(stderr,"Present: %s\n",error.c_str()); return 2; }
            count += result == Vulkan::Presenter::Result::Presented;
            QThread::msleep(8);
        }
        if (count != 12) return 3;
        presented += count;
        QThread::msleep(80);
        // Win32 per-HWND capture can return a stale raster backing store for
        // a resized Vulkan window. Capture the visible client rectangle.
        const QPoint origin = window.mapToGlobal(QPoint(0,0));
        const QImage capture = window.screen()->grabWindow(0,origin.x(),origin.y(),window.width(),window.height()).toImage().convertToFormat(QImage::Format_RGB32);
        if (capture.isNull() || capture.size() != physical) return 4;
        const QRgb colors[] = {qRgb(255,0,0), qRgb(0,255,0), qRgb(0,0,255), qRgb(255,255,255)};
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
            const QRgb actual = capture.pixel(capture.width()*(1+2*x)/4, capture.height()*(1+2*y)/4);
            if ((actual & 0xFFFFFF) != (colors[y*2+x] & 0xFFFFFF)) {
                std::fprintf(stderr,"Pixel mismatch %dx%d quadrant %d,%d: %08x\n",physical.width(),physical.height(),x,y,actual);
                capture.save("vulkan-presentation-failure.png"); return 5;
            }
        }
        std::printf("Vulkan color/stride/resize %dx%d: %u frames PASS\n",physical.width(),physical.height(),count);
    }
    if (presenter->Present(nullptr,0,0,0,error) != Vulkan::Presenter::Result::Skipped) return 6;
    window.showMinimized();
    app.processEvents();
    const uint32_t pixel = 0xFFFFFFFF;
    if (presenter->Present(&pixel,1,1,4,error) != Vulkan::Presenter::Result::Skipped) return 8;
    window.showNormal();
    app.processEvents();
    QImage restored((QSizeF(window.size()) * window.devicePixelRatioF()).toSize(), QImage::Format_RGB32);
    restored.fill(Qt::cyan);
    bool resumed = false;
    for (unsigned attempt = 0; attempt < 120 && !resumed; ++attempt) {
        app.processEvents();
        const auto result = presenter->Present(restored.constBits(),restored.width(),restored.height(),restored.bytesPerLine(),error);
        if (result == Vulkan::Presenter::Result::Failed) return 9;
        resumed = result == Vulkan::Presenter::Result::Presented;
        QThread::msleep(8);
    }
    if (!resumed) return 10;
    presenter.reset();
    presenter = Vulkan::Presenter::Create(reinterpret_cast<void*>(window.winId()), error);
    if (!presenter) return 7;
    std::printf("Vulkan presentation: %u frames, zero extent, minimize/restore, destroy/recreate PASS\n",presented);
    return 0;
}
