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

#include <stdio.h>
#include <string.h>
#include <queue>
#include <vector>

#include <QStandardItemModel>
#include <QPushButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QHostInfo>
#include <QLabel>
#include <QLayout>
#include <QTimerEvent>

#include "LANDialog.h"
#include "Config.h"
#include "main.h"
#include "LAN.h"

#include "ui_LANStartHostDialog.h"
#include "ui_LANStartClientDialog.h"
#include "ui_LANDialog.h"

using namespace melonDS;


static void returnToLocal(const std::shared_ptr<LAN>& session)
{
    session->EndSession();
    if (MPInterface::Acquire() == session) setMPInterface(MPInterface_Local);
}


LANStartHostDialog::LANStartHostDialog(QWidget* parent) : QDialog(parent), ui(new Ui::LANStartHostDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    setMPInterface(MPInterface_LAN);
    session = std::static_pointer_cast<LAN>(MPInterface::Acquire());

    auto cfg = Config::GetGlobalTable();
    ui->txtPlayerName->setText(cfg.GetQString("LAN.PlayerName"));

    ui->sbNumPlayers->setRange(2, 16);
    ui->sbNumPlayers->setValue(cfg.GetInt("LAN.HostNumPlayers"));
}

LANStartHostDialog::~LANStartHostDialog()
{
    if (!accepted) returnToLocal(session);
    delete ui;
}

void LANStartHostDialog::done(int r)
{
    if (!((MainWindow*)parent())->getEmuInstance())
    {
        QDialog::done(r);
        return;
    }

    if (r == QDialog::Accepted)
    {
        if (ui->txtPlayerName->text().trimmed().isEmpty())
        {
            QMessageBox::warning(this, "melonDS", "Please enter a player name.");
            return;
        }

        std::string player = ui->txtPlayerName->text().toStdString();
        int numplayers = ui->sbNumPlayers->value();

        if (!session->StartHost(player.c_str(), numplayers))
        {
            QMessageBox::warning(this, "melonDS", "Failed to start LAN game.");
            return;
        }

        accepted = true;
        LANDialog::openDlg(parentWidget());

        auto cfg = Config::GetGlobalTable();
        cfg.SetString("LAN.PlayerName", player);
        cfg.SetInt("LAN.HostNumPlayers", numplayers);
        Config::Save();
    }
    else
    {
        returnToLocal(session);
    }

    QDialog::done(r);
}


LANStartClientDialog::LANStartClientDialog(QWidget* parent) : QDialog(parent), ui(new Ui::LANStartClientDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    setMPInterface(MPInterface_LAN);
    session = std::static_pointer_cast<LAN>(MPInterface::Acquire());

    auto cfg = Config::GetGlobalTable();
    ui->txtPlayerName->setText(cfg.GetQString("LAN.PlayerName"));

    QStandardItemModel* model = new QStandardItemModel(this);
    ui->tvAvailableGames->setModel(model);
    const QStringList listheader = {"Name", "Players", "Status", "Host IP"};
    model->setHorizontalHeaderLabels(listheader);

    connect(ui->tvAvailableGames->selectionModel(), SIGNAL(selectionChanged(const QItemSelection&, const QItemSelection&)),
            this, SLOT(onGameSelectionChanged(const QItemSelection&, const QItemSelection&)));

    ui->buttonBox->button(QDialogButtonBox::Ok)->setText("Connect");
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);

    directButton = ui->buttonBox->addButton("Direct connect...", QDialogButtonBox::ActionRole);
    connect(directButton, &QPushButton::clicked, this, &LANStartClientDialog::onDirectConnect);
    statusLabel = new QLabel(this);
    statusLabel->setObjectName("connectionStatus");
    statusLabel->setTextFormat(Qt::PlainText);
    statusLabel->setWordWrap(true);
    layout()->addWidget(statusLabel);

    session->StartDiscovery();

    timerID = startTimer(50);
}

LANStartClientDialog::~LANStartClientDialog()
{
    killTimer(timerID);

    if (lookupID >= 0) QHostInfo::abortHostLookup(lookupID);
    if (!accepted) returnToLocal(session);
    delete ui;
}

void LANStartClientDialog::onGameSelectionChanged(const QItemSelection& cur, const QItemSelection& prev)
{
    if (connecting) return;
    QModelIndexList indlist = cur.indexes();
    if (indlist.count() == 0)
    {
        ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    }
    else
    {
        ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
    }
}

void LANStartClientDialog::on_tvAvailableGames_doubleClicked(QModelIndex index)
{
    done(QDialog::Accepted);
}

void LANStartClientDialog::onDirectConnect()
{
    if (connecting) return;
    const QString host = QInputDialog::getText(this, "Direct connect", "Host address:").trimmed();
    if (!host.isEmpty()) startConnection(host);
}

void LANStartClientDialog::startConnection(const QString& host)
{
    if (connecting) return;
    if (ui->txtPlayerName->text().trimmed().isEmpty())
    {
        statusLabel->setText("Please enter a player name before connecting.");
        return;
    }
    connecting = true;
    ui->txtPlayerName->setEnabled(false);
    ui->tvAvailableGames->setEnabled(false);
    directButton->setEnabled(false);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    statusLabel->setText(QString("Connecting to %1…").arg(host));
    connectionTimer.start();
    session->EndDiscovery();

    QHostAddress address(host);
    if (address.protocol() == QAbstractSocket::IPv4Protocol)
    {
        connectAddress(address.toString());
        return;
    }
    lookupID = QHostInfo::lookupHost(host, this, [this](const QHostInfo& info)
    {
        // Cancel/timeout may have occurred after DNS completed but before its
        // queued notification arrived. Never revive that abandoned attempt.
        if (!connecting || info.lookupId() != lookupID) return;
        lookupID = -1;
        for (const auto& address : info.addresses())
        {
            if (address.protocol() != QAbstractSocket::IPv4Protocol) continue;
            connectAddress(address.toString());
            return;
        }
        connectionFailed("Could not find an IPv4 address for this host.");
    });
}

void LANStartClientDialog::connectAddress(const QString& address)
{
    const auto player = ui->txtPlayerName->text().toStdString();
    if (!session->StartClient(player.c_str(), address.toStdString().c_str()))
        connectionFailed("Could not start the LAN connection. Check the host address and try again.");
}

void LANStartClientDialog::connectionFailed(const QString& message)
{
    connecting = false;
    if (lookupID >= 0) QHostInfo::abortHostLookup(lookupID);
    lookupID = -1;
    session->EndSession();
    ui->txtPlayerName->setEnabled(true);
    ui->tvAvailableGames->setEnabled(true);
    directButton->setEnabled(true);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(
        !ui->tvAvailableGames->selectionModel()->selectedRows().empty());
    statusLabel->setText(message);
    session->StartDiscovery();
}

void LANStartClientDialog::done(int r)
{
    if (r == QDialog::Accepted)
    {
        if (connecting) return;
        const auto selected = ui->tvAvailableGames->selectionModel()->selectedRows();
        if (selected.empty()) return;
        auto* model = static_cast<QStandardItemModel*>(ui->tvAvailableGames->model());
        const u32 address = model->item(selected[0].row())->data().toUInt();
        startConnection(QHostAddress(address).toString());
        return;
    }
    connecting = false;
    if (lookupID >= 0) QHostInfo::abortHostLookup(lookupID);
    lookupID = -1;
    returnToLocal(session);
    QDialog::done(r);
}

void LANStartClientDialog::timerEvent(QTimerEvent *event)
{
    if (event->timerId() != timerID) return;
    session->Process();
    if (!connecting)
    {
        doUpdateDiscoveryList();
        return;
    }
    const auto state = session->GetClientState();
    if (state == LAN::ClientState::Connected)
    {
        connecting = false;
        accepted = true;
        auto cfg = Config::GetGlobalTable();
        cfg.SetString("LAN.PlayerName", ui->txtPlayerName->text().toStdString());
        Config::Save();
        LANDialog::openDlg(parentWidget());
        QDialog::done(QDialog::Accepted);
    }
    else if (state == LAN::ClientState::Incompatible)
        connectionFailed("The host uses an incompatible LAN protocol version.");
    else if (state == LAN::ClientState::Failed || state == LAN::ClientState::Disconnected)
        connectionFailed("The host rejected or closed the connection. You can try again.");
    else if (state == LAN::ClientState::TimedOut || connectionTimer.elapsed() >= 5000)
        connectionFailed("The connection timed out. Check that the host is running and the firewall allows LAN play.");
}

void LANStartClientDialog::doUpdateDiscoveryList()
{
    auto disclist = session->GetDiscoveryList();

    QStandardItemModel* model = (QStandardItemModel*)ui->tvAvailableGames->model();
    int curcount = model->rowCount();
    int newcount = disclist.size();
    if (curcount > newcount)
    {
        model->removeRows(newcount, curcount-newcount);
    }
    else if (curcount < newcount)
    {
        for (int i = curcount; i < newcount; i++)
        {
            QList<QStandardItem*> row;
            row.append(new QStandardItem());
            row.append(new QStandardItem());
            row.append(new QStandardItem());
            row.append(new QStandardItem());
            model->appendRow(row);
        }
    }

    int i = 0;
    for (const auto& [key, data] : disclist)
    {
        model->item(i, 0)->setText(data.SessionName);
        model->item(i, 0)->setData(QVariant(key));

        QString plcount = QString("%0/%1").arg(data.NumPlayers).arg(data.MaxPlayers);
        model->item(i, 1)->setText(plcount);

        QString status;
        switch (data.Status)
        {
            case 0: status = "Idle"; break;
            case 1: status = "Playing"; break;
        }
        model->item(i, 2)->setText(status);

        QString ip = QString("%0.%1.%2.%3").arg(key>>24).arg((key>>16)&0xFF).arg((key>>8)&0xFF).arg(key&0xFF);
        model->item(i, 3)->setText(ip);

        i++;
    }
}


LANDialog::LANDialog(QWidget* parent) : QDialog(parent), ui(new Ui::LANDialog)
{
    ui->setupUi(this);
    session = std::static_pointer_cast<LAN>(MPInterface::Acquire());
    setAttribute(Qt::WA_DeleteOnClose);

    QStandardItemModel* model = new QStandardItemModel(this);
    ui->tvPlayerList->setModel(model);
    const QStringList header = {"#", "Player", "Status", "Ping", "IP"};
    model->setHorizontalHeaderLabels(header);

    timerID = startTimer(1000);
}

LANDialog::~LANDialog()
{
    killTimer(timerID);
    returnToLocal(session);

    delete ui;
}

void LANDialog::on_btnLeaveGame_clicked()
{
    done(QDialog::Accepted);
}

void LANDialog::done(int r)
{
    if (!((MainWindow*)parent())->getEmuInstance())
    {
        QDialog::done(r);
        return;
    }

    bool showwarning = true;
    if (session->GetNumPlayers() < 2)
        showwarning = false;

    if (showwarning)
    {
        if (QMessageBox::warning(this, "melonDS", "Really leave this LAN game?",
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::No)
            return;
    }

    returnToLocal(session);

    QDialog::done(r);
}

void LANDialog::timerEvent(QTimerEvent *event)
{
    session->Process();
    if (session->GetClientState() == LAN::ClientState::Disconnected)
    {
        returnToLocal(session);
        QDialog::done(QDialog::Rejected);
        return;
    }
    doUpdatePlayerList();
}

void LANDialog::doUpdatePlayerList()
{
    auto playerlist = session->GetPlayerList();
    auto maxplayers = session->GetMaxPlayers();

    QStandardItemModel* model = (QStandardItemModel*)ui->tvPlayerList->model();
    int curcount = model->rowCount();
    int newcount = playerlist.size();
    if (curcount > newcount)
    {
        model->removeRows(newcount, curcount-newcount);
    }
    else if (curcount < newcount)
    {
        for (int i = curcount; i < newcount; i++)
        {
            QList<QStandardItem*> row;
            row.append(new QStandardItem());
            row.append(new QStandardItem());
            row.append(new QStandardItem());
            row.append(new QStandardItem());
            row.append(new QStandardItem());
            model->appendRow(row);
        }
    }

    int i = 0;
    for (const auto& player : playerlist)
    {
        QString id = QString("%0/%1").arg(player.ID+1).arg(maxplayers);
        model->item(i, 0)->setText(id);

        QString name = player.Name;
        model->item(i, 1)->setText(name);

        QString status = "???";
        switch (player.Status)
        {
            case LAN::Player_Client:
                status = "Connected";
                break;
            case LAN::Player_Host:
                status = "Game host";
                break;
            case LAN::Player_Connecting:
                status = "Connecting";
                break;
            case LAN::Player_Disconnected:
                status = "Connection lost";
                break;
            case LAN::Player_None:
                break;
        }
        model->item(i, 2)->setText(status);

        if (player.IsLocalPlayer)
        {
            model->item(i, 3)->setText("-");
            model->item(i, 4)->setText("(local)");
        }
        else
        {
            if (player.Status == LAN::Player_Client ||
                player.Status == LAN::Player_Host)
            {
                QString ping = QString("%0 ms").arg(player.Ping);
                model->item(i, 3)->setText(ping);
            }
            else
            {
                model->item(i, 3)->setText("-");
            }


            u32 ip = player.Address;

            QString ips = QString("%0.%1.%2.%3").arg(ip&0xFF).arg((ip>>8)&0xFF).arg((ip>>16)&0xFF).arg(ip>>24);
            model->item(i, 4)->setText(ips);
        }

        i++;
    }
}
