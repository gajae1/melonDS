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
#include <QFileDialog>
#include <QPaintEvent>
#include <QPainter>

#include "types.h"
#include "main.h"

#include "CameraSettingsDialog.h"
#include "ui_CameraSettingsDialog.h"

using namespace melonDS;

CameraSettingsDialog* CameraSettingsDialog::currentDlg = nullptr;

extern CameraManager* camManager[2];


CameraPreviewPanel::CameraPreviewPanel(QWidget* parent) : QWidget(parent)
{
    currentCam = nullptr;
    updateTimer = startTimer(50);
}

CameraPreviewPanel::~CameraPreviewPanel()
{
    killTimer(updateTimer);
}

void CameraPreviewPanel::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);

    if (!currentCam)
    {
        painter.fillRect(event->rect(), QColor::fromRgb(0, 0, 0));
        return;
    }

    QImage picture(256, 192, QImage::Format_RGB32);
    currentCam->captureFrame((u32*)picture.bits(), 256, 192, false);
    painter.drawImage(0, 0, picture);
}


CameraSettingsDialog::CameraSettingsDialog(QWidget* parent) : QDialog(parent), ui(new Ui::CameraSettingsDialog)
{
    previewPanel = nullptr;
    currentCfg = nullptr;
    currentCam = nullptr;

    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();

    for (int i = 0; i < 2; i++)
    {
        auto& cfg = camManager[i]->getConfig();

        oldCamSettings[i].InputType = cfg.GetInt("InputType");
        oldCamSettings[i].ImagePath = cfg.GetString("ImagePath");
        oldCamSettings[i].CamDeviceName = cfg.GetString("DeviceName");
        oldCamSettings[i].XFlip = cfg.GetBool("XFlip");
    }

    // The dialog owns the preview lifecycle: snapshot which cameras the
    // emulation had started, then stop them so the preview owns the device.
    // The destructor hands the saved state back even when the dialog is
    // destroyed without an accept/reject (e.g. parent window teardown).
    settingsApplied = false;
    for (int i = 0; i < 2; i++)
    {
        savedStarted[i] = camManager[i]->isStarted();
        if (savedStarted[i]) camManager[i]->stop();
    }

    ui->cbCameraSel->addItem("DSi outer camera");
    ui->cbCameraSel->addItem("DSi inner camera");

#if QT_VERSION >= 0x060000
    mediaDevices = new QMediaDevices(this);
    connect(mediaDevices, &QMediaDevices::videoInputsChanged,
            this, &CameraSettingsDialog::refreshCameraList);
    refreshCameraList();
#else
    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    for (const QCameraInfo &cameraInfo : cameras)
    {
        QString name = cameraInfo.description();
        QCamera::Position pos = cameraInfo.position();
        if (pos != QCamera::UnspecifiedPosition)
        {
            name += " (";
            if (pos == QCamera::FrontFace)
                name += "inner camera";
            else if (pos == QCamera::BackFace)
                name += "outer camera";
            name += ")";
        }

        ui->cbPhysicalCamera->addItem(name, cameraInfo.deviceName());
    }
#endif
    ui->rbPictureCamera->setEnabled(ui->cbPhysicalCamera->count() > 0);

    grpInputType = new QButtonGroup(this);
    grpInputType->addButton(ui->rbPictureNone,   0);
    grpInputType->addButton(ui->rbPictureImg,    1);
    grpInputType->addButton(ui->rbPictureCamera, 2);
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    connect(grpInputType, SIGNAL(buttonClicked(int)), this, SLOT(onChangeInputType(int)));
#else
    connect(grpInputType, SIGNAL(idClicked(int)), this, SLOT(onChangeInputType(int)));
#endif

    previewPanel = new CameraPreviewPanel(this);
    QVBoxLayout* previewLayout = new QVBoxLayout();
    previewLayout->addWidget(previewPanel);
    ui->grpPreview->setLayout(previewLayout);
    previewPanel->setMinimumSize(256, 192);
    previewPanel->setMaximumSize(256, 192);

    on_cbCameraSel_currentIndexChanged(ui->cbCameraSel->currentIndex());
}

CameraSettingsDialog::~CameraSettingsDialog()
{
    if (currentDlg == this) currentDlg = nullptr;
    // End the preview feed first; the emulation's saved state is restored below.
    for (int i = 0; i < 2; i++)
        camManager[i]->stop();
    // Destruction without accept() — Cancel, or a parent teardown where no
    // finished/rejected runs — still has to undo the live-edited settings
    // and hand the saved camera state back to the emulation.
    if (!settingsApplied) restoreOldSettings(false);
    for (int i = 0; i < 2; i++)
        if (savedStarted[i]) camManager[i]->start();
    delete ui;
}

void CameraSettingsDialog::on_CameraSettingsDialog_accepted()
{
    settingsApplied = true;

    for (int i = 0; i < 2; i++)
    {
        camManager[i]->stop();
    }

    Config::SaveWithDialog(this);

    closeDlg();
}

void CameraSettingsDialog::on_CameraSettingsDialog_rejected()
{
    // The camera tables are live-edited during preview, so rejection always
    // restores the snapshot. The device itself is only reinitialized while
    // the emulation instance still exists.
    restoreOldSettings(((MainWindow*)parent())->getEmuInstance() != nullptr);
    closeDlg();
}

void CameraSettingsDialog::restoreOldSettings(bool reinit)
{
    for (int i = 0; i < 2; i++)
    {
        if (reinit)
        {
            camManager[i]->stop();
            camManager[i]->deInit();
        }

        auto& cfg = camManager[i]->getConfig();
        cfg.SetInt("InputType", oldCamSettings[i].InputType);
        cfg.SetString("ImagePath", oldCamSettings[i].ImagePath);
        cfg.SetString("DeviceName", oldCamSettings[i].CamDeviceName);
        cfg.SetBool("XFlip", oldCamSettings[i].XFlip);

        if (reinit)
            camManager[i]->init();
    }
}

#if QT_VERSION >= 0x060000
void CameraSettingsDialog::refreshCameraList()
{
    // Re-enumerate without disturbing the selection so a camera plugged in
    // while the dialog is open becomes selectable. Signals stay blocked: a
    // hotplug rebuild must not rewrite the configured device name.
    const QString selected = ui->cbPhysicalCamera->currentData().toString();
    const QSignalBlocker blocker(ui->cbPhysicalCamera);
    ui->cbPhysicalCamera->clear();
    int restore = -1;
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    for (const QCameraDevice &cameraInfo : cameras)
    {
        QString name = cameraInfo.description();
        QCameraDevice::Position pos = cameraInfo.position();
        if (pos != QCameraDevice::UnspecifiedPosition)
        {
            name += " (";
            if (pos == QCameraDevice::FrontFace)
                name += "inner camera";
            else if (pos == QCameraDevice::BackFace)
                name += "outer camera";
            name += ")";
        }

        ui->cbPhysicalCamera->addItem(name, QString(cameraInfo.id()));
        if (QString(cameraInfo.id()) == selected)
            restore = ui->cbPhysicalCamera->count() - 1;
    }
    ui->cbPhysicalCamera->setCurrentIndex(restore >= 0 ? restore : 0);
    ui->rbPictureCamera->setEnabled(ui->cbPhysicalCamera->count() > 0);
}
#endif

void CameraSettingsDialog::on_cbCameraSel_currentIndexChanged(int id)
{
    if (!previewPanel) return;

    if (currentCam)
    {
        currentCam->stop();
    }

    currentId = id;
    currentCfg = &camManager[id]->getConfig();
    //currentCam = camManager[id];
    currentCam = nullptr;
    populateCamControls(id);
    currentCam = camManager[id];
    previewPanel->setCurrentCam(currentCam);

    currentCam->start();
}

void CameraSettingsDialog::onChangeInputType(int type)
{
    if (!currentCfg) return;

    if (currentCam)
    {
        currentCam->stop();
        currentCam->deInit();
    }

    currentCfg->SetInt("InputType", type);

    ui->txtSrcImagePath->setEnabled(type == 1);
    ui->btnSrcImageBrowse->setEnabled(type == 1);
    ui->cbPhysicalCamera->setEnabled((type == 2) && (ui->cbPhysicalCamera->count()>0));

    currentCfg->SetQString("ImagePath", ui->txtSrcImagePath->text());

    if (ui->cbPhysicalCamera->count() > 0)
        currentCfg->SetQString("DeviceName", ui->cbPhysicalCamera->currentData().toString());

    if (currentCam)
    {
        currentCam->init();
        currentCam->start();
    }
}

void CameraSettingsDialog::on_txtSrcImagePath_textChanged()
{
    if (!currentCfg) return;

    if (currentCam)
    {
        currentCam->stop();
        currentCam->deInit();
    }

    currentCfg->SetQString("ImagePath", ui->txtSrcImagePath->text());

    if (currentCam)
    {
        currentCam->init();
        currentCam->start();
    }
}

void CameraSettingsDialog::on_btnSrcImageBrowse_clicked()
{
    QString file = QFileDialog::getOpenFileName(this,
                                                "Select image file...",
                                                emuDirectory,
                                                "Image files (*.png *.jpg *.jpeg *.bmp);;Any file (*.*)");

    if (file.isEmpty()) return;

    ui->txtSrcImagePath->setText(file);
}

void CameraSettingsDialog::on_cbPhysicalCamera_currentIndexChanged(int id)
{
    if (!currentCfg) return;

    if (currentCam)
    {
        currentCam->stop();
        currentCam->deInit();
    }

    currentCfg->SetQString("DeviceName", ui->cbPhysicalCamera->itemData(id).toString());

    if (currentCam)
    {
        currentCam->init();
        currentCam->start();
    }
}

void CameraSettingsDialog::populateCamControls(int id)
{
    Config::Table& cfg = camManager[id]->getConfig();

    int type = cfg.GetInt("InputType");
    if (type < 0 || type >= grpInputType->buttons().count()) type = 0;
    grpInputType->button(type)->setChecked(true);

    ui->txtSrcImagePath->setText(cfg.GetQString("ImagePath"));

    bool deviceset = false;
    QString device = cfg.GetQString("DeviceName");
    for (int i = 0; i < ui->cbPhysicalCamera->count(); i++)
    {
        QString itemdev = ui->cbPhysicalCamera->itemData(i).toString();
        if (itemdev == device)
        {
            ui->cbPhysicalCamera->setCurrentIndex(i);
            deviceset = true;
            break;
        }
    }
    if (!deviceset)
        ui->cbPhysicalCamera->setCurrentIndex(0);

    onChangeInputType(type);

    ui->chkFlipPicture->setChecked(cfg.GetBool("XFlip"));
}

void CameraSettingsDialog::on_chkFlipPicture_clicked()
{
    if (!currentCfg) return;

    bool xflip = ui->chkFlipPicture->isChecked();
    currentCfg->SetBool("XFlip", xflip);
    if (currentCam) currentCam->setXFlip(xflip);
}
