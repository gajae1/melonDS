// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>
#include <QTouchEvent>
#include <QMouseEvent>
#include <QFocusEvent>
#include <QtTest/QTest>
#include <cstdio>
#include <string_view>
#include "ScreenLayout.h"

// Run current event handlers through Qt's event delivery and real layout.
// Emulation/device state is substituted; no user window or input is accessed.
struct TouchHost
{
    bool active = true, down = false;
    int x = -1, y = -1, keysReleased = 0, muteUpdates = 0;
    bool emuIsActive() const { return active; }
    void touchScreen(int px, int py) { down = true; x = px; y = py; }
    void releaseScreen() { down = false; }
    void keyReleaseAll() { ++keysReleased; }
    void updateAudioMuteByWindowFocus() { ++muteUpdates; }
};
struct ThreadState
{
    bool running = true;
    void emuPause() { running = false; }
    void emuUnpause() { running = true; }
    bool emuIsRunning() const { return running; }
};
struct ScreenState;
struct WindowState
{
    TouchHost* emuInstance;
    ScreenState* panel = nullptr;
    ThreadState* emuThread;
    bool focused = true, pauseOnLostFocus = true, pausedManually = false;
    void onFocusIn() { focused = true; }
    void onFocusOut();
    void onAppStateChanged(Qt::ApplicationState state);
};
struct ScreenState : QWidget
{
    TouchHost* emuInstance;
    WindowState* mainWindow;
    ScreenLayout layout;
    bool touching = false;
    ScreenState(TouchHost* host, WindowState* window) : emuInstance(host), mainWindow(window)
    {
        setAttribute(Qt::WA_AcceptTouchEvents);
        resize(256, 192);
        layout.Setup(256, 192, screenLayout_Natural, screenRot_0Deg,
                     screenSizing_BotOnly, 0, false, false, 1, 1);
    }
    void showCursor() {}
    void releaseTouch();
    void touchEvent(QTouchEvent* event);
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;
    bool event(QEvent* event) override;
};
#define ScreenPanel ScreenState
#include "releaseTouch.inc"
#include "touchEvent.inc"
#include "touchPanelEvent.inc"
#include "touchMousePress.inc"
#include "touchMouseRelease.inc"
#include "touchMouseMove.inc"
#include "touchTablet.inc"
#undef ScreenPanel
#define MainWindow WindowState
#include "onFocusOut.inc"
#include "touchAppState.inc"
#undef MainWindow

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (argc != 2) return 2;
    const std::string_view mode = argv[1];
    TouchHost host;
    ThreadState thread;
    WindowState window{&host, nullptr, &thread};
    ScreenState panel(&host, &window);
    window.panel = &panel;
    panel.show();
    QApplication::processEvents();
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };
    const auto mouse = [&](QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent event(type, pos, panel.mapToGlobal(pos.toPoint()), button, held, Qt::NoModifier);
        QApplication::sendEvent(&panel, &event);
    };
    auto* device = QTest::createTouchDevice();
    if (!device) return 2;

    if (mode == "drag" || mode == "cancel" || mode == "end-paused")
    {
        QTest::touchEvent(&panel, device).press(0, QPoint(20, 30), &panel).commit();
        // This is also a fixture control: Qt must actually deliver TouchBegin.
        if (!host.down || !panel.touching) return 2;
        if (mode == "drag")
        {
            check(host.x == 20 && host.y == 30, "TouchBegin did not use current position");
            QTest::touchEvent(&panel, device).move(0, QPoint(170, 140), &panel).commit();
            check(host.x == 170 && host.y == 140, "TouchUpdate lags behind current position");
            QTest::touchEvent(&panel, device).release(0, QPoint(170, 140), &panel).commit();
            check(!host.down && !panel.touching, "TouchEnd left a pressed screen");
        }
        else if (mode == "cancel")
        {
            QTouchEvent cancel(QEvent::TouchCancel, device);
            QApplication::sendEvent(&panel, &cancel);
            check(!host.down && !panel.touching, "Empty TouchCancel left a pressed screen");
            mouse(QEvent::MouseMove, {180, 150}, Qt::NoButton, Qt::NoButton);
            check(!host.down, "Mouse movement restarted a cancelled touch");
            QTest::touchEvent(&panel, device).move(0, QPoint(170, 140), &panel).commit();
            check(!host.down && !panel.touching, "TouchUpdate restarted a cancelled gesture");
        }
        else
        {
            host.active = false;
            QTest::touchEvent(&panel, device).release(0, QPoint(20, 30), &panel).commit();
            check(!host.down && !panel.touching, "TouchEnd while inactive left stale input");
        }
    }
    else if (mode == "focus" || mode == "app-inactive" || mode == "mouse")
    {
        mouse(QEvent::MouseButtonPress, {40, 50}, Qt::LeftButton, Qt::LeftButton);
        if (!host.down || host.x != 40 || host.y != 50 || !panel.touching) return 2;
        if (mode == "focus")
        {
            QFocusEvent out(QEvent::FocusOut);
            QApplication::sendEvent(&panel, &out);
            check(!host.down && !panel.touching && !window.focused,
                  "FocusOut left the guest or panel pressed");
            mouse(QEvent::MouseMove, {120, 130}, Qt::NoButton, Qt::NoButton);
            check(!host.down, "Movement after focus loss restarted a touch");
            window.emuInstance = nullptr;
            window.onFocusOut();
        }
        else if (mode == "app-inactive")
        {
            window.onAppStateChanged(Qt::ApplicationInactive);
            check(!host.down && !panel.touching && !thread.running,
                  "ApplicationInactive did not release touch before pausing");
            window.onAppStateChanged(Qt::ApplicationActive);
            check(thread.running && !host.down, "Reactivation retained stale touch");
        }
        else
        {
            mouse(QEvent::MouseMove, {400, 300}, Qt::NoButton, Qt::LeftButton);
            check(host.x == 255 && host.y == 191, "Mouse drag no longer clamps to bottom screen");
            mouse(QEvent::MouseButtonRelease, {400, 300}, Qt::RightButton, Qt::LeftButton);
            check(host.down, "Unrelated mouse release ended left-button touch");
            host.active = false;
            mouse(QEvent::MouseButtonRelease, {400, 300}, Qt::LeftButton, Qt::NoButton);
            check(!host.down && !panel.touching, "Mouse release while inactive left stale input");
        }
    }
    else if (mode == "tablet")
    {
        const auto pen = [&](QEvent::Type type, QPointF pos, bool down) {
#if QT_VERSION_MAJOR == 6
            static QPointingDevice stylus("Generated pen", 789, QInputDevice::DeviceType::Stylus,
                QPointingDevice::PointerType::Pen, QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
            QTabletEvent event(type, &stylus, pos, panel.mapToGlobal(pos.toPoint()),
                down ? 1.0 : 0.0, 0, 0, 0, 0, 0, Qt::NoModifier,
                type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                down ? Qt::LeftButton : Qt::NoButton);
#else
            QTabletEvent event(type, pos, panel.mapToGlobal(pos.toPoint()), QTabletEvent::Stylus,
                QTabletEvent::Pen, down ? 1.0 : 0.0, 0, 0, 0, 0, 0, Qt::NoModifier, 789,
                type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                down ? Qt::LeftButton : Qt::NoButton);
#endif
            QApplication::sendEvent(&panel, &event);
        };
        pen(QEvent::TabletMove, {20, 30}, false);
        check(!host.down && !panel.touching, "Pen hover started a screen press");
        pen(QEvent::TabletPress, {60, 70}, true);
        check(host.down && host.x == 60 && host.y == 70, "Pen press was lost");
        pen(QEvent::TabletMove, {110, 130}, true);
        check(host.down && host.x == 110 && host.y == 130, "Pen drag was lost");
        QFocusEvent out(QEvent::FocusOut);
        QApplication::sendEvent(&panel, &out);
        pen(QEvent::TabletMove, {120, 140}, true);
        check(!host.down && !panel.touching, "Pen move restarted a touch after focus loss");
        pen(QEvent::TabletPress, {60, 70}, true);
        host.active = false;
        pen(QEvent::TabletRelease, {60, 70}, false);
        check(!host.down && !panel.touching, "Inactive pen release left a pressed screen");
    }
    else return 2;
    std::printf("Touch %s: %d failures\n", argv[1], failures);
    return failures ? 1 : 0;
}
