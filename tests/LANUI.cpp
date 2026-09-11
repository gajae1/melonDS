// SPDX-License-Identifier: GPL-3.0-or-later
// Actual LAN dialogs, Qt event loop/DNS and ENet, with a minimal parent/config.
// No emulator, user settings, dumps or external host are used. A generated MP
// frame exercises the lobby statistics; LANLoopback also covers concurrent snapshots.
#include <QtWidgets>
#include <QtNetwork>
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include "net/LAN.h"
#include "ui_LANStartHostDialog.h"
#include "ui_LANStartClientDialog.h"
#include "ui_LANDialog.h"

#define MAIN_H
#define CONFIG_H
class MainWindow : public QWidget
{
public:
    void* getEmuInstance() { return this; }
};
namespace Config
{
int Saves = 0;
struct Table
{
    QString GetQString(const char*) { return "Loopback player"; }
    int GetInt(const char*) { return 2; }
    void SetString(const char*, const std::string&) {}
    void SetInt(const char*, int) {}
};
Table GetGlobalTable() { return {}; }
void Save() { ++Saves; }
}
void setMPInterface(melonDS::MPInterfaceType type) { melonDS::MPInterface::Set(type); }
#include "../src/frontend/qt_sdl/LANDialog.cpp"

namespace melonDS::Platform
{
u64 GetMSCount()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
template<class Predicate>
static bool Pump(Predicate done, int timeout = 2000, LAN* host = nullptr)
{
    QElapsedTimer timer;
    timer.start();
    do
    {
        if (host) host->Process();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (done()) return true;
        QThread::msleep(1);
    } while (timer.elapsed() < timeout);
    return false;
}
static void SelectLoopback(LANStartClientDialog* dialog)
{
    auto* view = dialog->findChild<QAbstractItemView*>("tvAvailableGames");
    Require(view != nullptr, "discovery view missing");
    auto* model = static_cast<QStandardItemModel*>(view->model());
    model->clear();
    QList<QStandardItem*> row;
    for (int i = 0; i < 4; ++i) row.append(new QStandardItem("Loopback"));
    row[0]->setData(QVariant(0x7F000001u));
    model->appendRow(row);
    view->selectionModel()->select(model->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    Require(QMetaObject::invokeMethod(dialog, "done", Q_ARG(int, int(QDialog::Accepted))), "connect action missing");
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    try
    {
        MainWindow window;
        int beats = 0;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++beats; });
        heartbeat.start(10);
        auto open = [&] { return QPointer<LANStartClientDialog>(LANStartClientDialog::openDlg(&window)); };
        auto dialog = open();
        SelectLoopback(dialog);
        auto retained = std::static_pointer_cast<LAN>(MPInterface::Acquire());
        Require(retained->GetClientState() == LAN::ClientState::Connecting, "connect completed synchronously");
        Require(Pump([&] { return beats >= 5; }, 500), "UI heartbeat stalled during connect");
        auto* buttons = dialog->findChild<QDialogButtonBox*>("buttonBox");
        Require(buttons->button(QDialogButtonBox::Cancel)->isEnabled(), "cancel disabled during connect");
        QElapsedTimer cancel;
        cancel.start();
        buttons->button(QDialogButtonBox::Cancel)->click();
        Require(Pump([&] { return dialog.isNull(); }, 500) && cancel.elapsed() < 500,
                "cancel or deletion blocked");
        Require(retained->GetClientState() == LAN::ClientState::Idle &&
                MPInterface::GetType() == MPInterface_Local, "cancel did not restore Local");
        retained.reset();

        // A pending asynchronous localhost lookup cannot outlive cancellation.
        dialog = open();
        QTimer::singleShot(0, dialog, [&] {
            auto* input = dialog->findChild<QInputDialog*>();
            Require(input != nullptr, "direct-address dialog missing");
            input->setTextValue("localhost");
            input->accept();
        });
        Require(QMetaObject::invokeMethod(dialog, "onDirectConnect"), "direct connect action missing");
        dialog->reject();
        Require(Pump([&] { return dialog.isNull(); }), "DNS cancel did not delete dialog");
        Require(MPInterface::GetType() == MPInterface_Local, "DNS result revived cancelled session");

        // Parent destruction while connecting must release the pending session.
        auto* parent = new MainWindow;
        QPointer<LANStartClientDialog> child = LANStartClientDialog::openDlg(parent);
        SelectLoopback(child);
        delete parent;
        Require(child.isNull() && MPInterface::GetType() == MPInterface_Local, "parent close left LAN active");

        dialog = open();
        SelectLoopback(dialog);
        const int before = beats;
        Require(Pump([&] {
            return dialog->findChild<QLabel*>("connectionStatus")->text().contains("timed out");
        }, 6000), "timeout did not return to retry state");
        Require(beats - before >= 100 && dialog->isEnabled(), "timeout stalled the GUI");

        LAN host;
        Require(host.StartHost("UI loopback host", 2), "loopback port unavailable");
        host.EndDiscovery();
        SelectLoopback(dialog);
        Require(Pump([&] { return dialog.isNull(); }, 2500, &host), "retry did not complete");
        Require(Config::Saves == 1 && MPInterface::GetType() == MPInterface_LAN, "successful retry did not persist once");
        QPointer<LANDialog> lobby = window.findChild<LANDialog*>();
        Require(!lobby.isNull(), "connected lobby missing");

        auto client = std::static_pointer_cast<LAN>(MPInterface::Acquire());
        auto* receiveSummary = lobby->findChild<QLabel*>("receiveSummary");
        auto* waitSummary = lobby->findChild<QLabel*>("waitSummary");
        auto* replySummary = lobby->findChild<QLabel*>("replySummary");
        Require(receiveSummary && waitSummary && replySummary, "receive statistics labels missing");
        for (auto* label : {receiveSummary, waitSummary, replySummary})
            Require(label->textFormat() == Qt::PlainText && label->wordWrap(),
                    "receive statistics must be wrapped plain text");
        if (client->GetReceiveStats().WaitSamples == 0)
            Require(waitSummary->text() == "Receive wait: no samples yet", "initial wait summary missing");

        host.Begin(0);
        client->Begin(0);
        // Let reliable player-ready messages settle before the MP channel frame.
        Pump([&] { client->Process(); return false; }, 150, &host);
        const auto beforeReceive = client->GetReceiveStats();
        std::array<u8, 40> command{}, received{};
        for (size_t i = 0; i < command.size(); ++i) command[i] = static_cast<u8>(0x30 + i);
        constexpr u64 timestamp = 123456;
        u64 receivedTimestamp = 0;
        Require(host.SendCmd(0, command.data(), command.size(), timestamp) == int(command.size()),
                "generated statistics command send failed");
        Require(Pump([&] {
            return client->RecvHostPacket(0, received.data(), &receivedTimestamp, received.size())
                == int(received.size());
        }, 2000, &host), "generated statistics command receive failed");
        Require(received == command && receivedTimestamp == timestamp,
                "statistics command payload or timestamp changed");
        const auto stats = client->GetReceiveStats();
        Require(stats.ReceivedPackets > beforeReceive.ReceivedPackets && stats.WaitSamples > 0,
                "real receive did not produce packet and wait observations");
        Require(QMetaObject::invokeMethod(lobby, "doUpdatePlayerList"), "lobby update slot missing");
        const auto receiveMatch = QRegularExpression("Received packets: ([0-9]+)").match(receiveSummary->text());
        Require(receiveMatch.hasMatch() && receiveMatch.captured(1).toULongLong() == stats.ReceivedPackets,
                "lobby packet count differs from real receive statistics");
        const auto waitMatch = QRegularExpression(
            "^Receive wait: mean ([0-9]+\\.[0-9]) ms \\| Maximum: ([0-9]+) ms \\| Samples: ([0-9]+)$")
            .match(waitSummary->text());
        Require(waitMatch.hasMatch() && waitMatch.captured(1).toDouble() >= 0.0 &&
                waitMatch.captured(2).toULongLong() == stats.MaxWaitMS &&
                waitMatch.captured(3).toULongLong() == stats.WaitSamples,
                "lobby measured wait summary is invalid");
        std::printf("PASS generated MP command: bytes=40 timestamp=exact\n%s\n%s\n%s\n",
                    qPrintable(receiveSummary->text()), qPrintable(waitSummary->text()), qPrintable(replySummary->text()));

        const QString screenshotPath = qEnvironmentVariable("MELONDS_LAN_UI_SCREENSHOT");
        if (!screenshotPath.isEmpty())
        {
            lobby->ensurePolished();
            lobby->layout()->activate();
            QCoreApplication::processEvents();
            Require(lobby->grab().save(screenshotPath), "lobby screenshot save failed");
        }

        host.EndSession();
        Require(Pump([&] { return lobby.isNull(); }, 2500), "host loss did not close lobby");
        Require(MPInterface::GetType() == MPInterface_Local, "host loss did not restore Local");
        MPInterface::Set(MPInterface_Dummy);
        std::puts("PASS actual LAN dialogs: responsive connect, cancel/DNS/parent close, timeout, retry and host loss");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL LAN UI: %s\n", error.what());
        return 1;
    }
}
