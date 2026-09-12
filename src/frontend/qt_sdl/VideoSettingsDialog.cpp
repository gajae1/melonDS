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

#include <QFileDialog>
#include <QtGlobal>
#include <QStandardItemModel>
#include <QSignalBlocker>

#include "types.h"
#include "Platform.h"
#include "Config.h"
#include "GPU.h"
#include "main.h"

#include "VideoSettingsDialog.h"
#include "ui_VideoSettingsDialog.h"


inline bool VideoSettingsDialog::UsesGL()
{
    auto& cfg = emuInstance->getGlobalConfig();
    return cfg.GetBool("Screen.UseGL") || (cfg.GetInt("3D.Renderer") != renderer3D_Software);
}

VideoSettingsDialog* VideoSettingsDialog::currentDlg = nullptr;

void VideoSettingsDialog::setEnabled()
{
    auto& cfg = emuInstance->getGlobalConfig();
    int renderer = cfg.GetInt("3D.Renderer");

    bool softwareRenderer = renderer == renderer3D_Software;
    ui->cbGLDisplay->setEnabled(softwareRenderer);
    ui->cbSoftwareThreaded->setEnabled(softwareRenderer);
    ui->cbPixelConversion->setEnabled(softwareRenderer);
    ui->cbxGLResolution->setEnabled(!softwareRenderer);
    ui->cbBetterPolygons->setEnabled(renderer == renderer3D_OpenGL);
    ui->cbxComputeHiResCoords->setEnabled(renderer == renderer3D_OpenGLCompute);
    setVsyncControlEnable(UsesGL());
}

VideoSettingsDialog::VideoSettingsDialog(QWidget* parent) : QDialog(parent), ui(new Ui::VideoSettingsDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();

    auto& cfg = emuInstance->getGlobalConfig();
    oldRenderer = cfg.GetInt("3D.Renderer");
    oldGLDisplay = cfg.GetBool("Screen.UseGL");
    oldVSync = cfg.GetBool("Screen.VSync");
    oldVSyncInterval = cfg.GetInt("Screen.VSyncInterval");
    oldSoftThreaded = cfg.GetBool("3D.Soft.Threaded");
    oldPixelConversion = cfg.GetInt("3D.Soft.PixelConversion");
    oldGLScale = cfg.GetInt("3D.GL.ScaleFactor");
    oldGLBetterPolygons = cfg.GetBool("3D.GL.BetterPolygons");
    oldHiresCoordinates = cfg.GetBool("3D.GL.HiresCoordinates");

    grp3DRenderer = new QButtonGroup(this);
    grp3DRenderer->addButton(ui->rb3DSoftware, renderer3D_Software);
    grp3DRenderer->addButton(ui->rb3DOpenGL,   renderer3D_OpenGL);
    grp3DRenderer->addButton(ui->rb3DCompute,  renderer3D_OpenGLCompute);
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    connect(grp3DRenderer, SIGNAL(buttonClicked(int)), this, SLOT(onChange3DRenderer(int)));
#else
    connect(grp3DRenderer, SIGNAL(idClicked(int)), this, SLOT(onChange3DRenderer(int)));
#endif
    grp3DRenderer->button(oldRenderer)->setChecked(true);

#ifndef OGLRENDERER_ENABLED
    ui->rb3DOpenGL->setEnabled(false);
#endif

#ifdef __APPLE__
    ui->rb3DCompute->setEnabled(false);
#endif

    ui->cbGLDisplay->setChecked(oldGLDisplay != 0);

    ui->cbVSync->setChecked(oldVSync != 0);
    ui->sbVSyncInterval->setValue(oldVSyncInterval);

    ui->cbSoftwareThreaded->setChecked(oldSoftThreaded);

    ui->cbPixelConversion->blockSignals(true);
    ui->cbPixelConversion->addItems({"Automatic", "Scalar", "AVX2", "AVX-512"});
    auto pixelModel = static_cast<QStandardItemModel*>(ui->cbPixelConversion->model());
    for (int i = 0; i < ui->cbPixelConversion->count(); ++i)
        pixelModel->item(i)->setEnabled(melonDS::PixelConvert::IsSupported(
            static_cast<melonDS::PixelConvert::Backend>(i)));
    ui->cbPixelConversion->setCurrentIndex(oldPixelConversion);
    ui->cbPixelConversion->blockSignals(false);

    for (int i = 1; i <= 16; i++)
        ui->cbxGLResolution->addItem(QString("%1x native (%2x%3)").arg(i).arg(256*i).arg(192*i));
    ui->cbxGLResolution->setCurrentIndex(oldGLScale-1);

    ui->cbBetterPolygons->setChecked(oldGLBetterPolygons != 0);
    ui->cbxComputeHiResCoords->setChecked(oldHiresCoordinates != 0);

    connect(emuInstance->getEmuThread(), &EmuThread::videoSettingsStatusChanged,
            this, &VideoSettingsDialog::refreshRendererStatus, Qt::QueuedConnection);
    refreshRendererStatus();
}

VideoSettingsDialog::~VideoSettingsDialog()
{
    delete ui;
}

void VideoSettingsDialog::refreshRendererStatus()
{
    // A queued result may arrive after the owner has closed its instance.
    if (!static_cast<MainWindow*>(parent())->getEmuInstance()) return;
    auto* thread = emuInstance->getEmuThread();
    const auto status = thread->videoSettingsStatus();
    auto& cfg = emuInstance->getGlobalConfig();
    const int selected = cfg.GetInt("3D.Renderer");
    if (auto* button = grp3DRenderer->button(selected)) button->setChecked(true);
    {
        const QSignalBlocker blocker(ui->cbGLDisplay);
        ui->cbGLDisplay->setChecked(cfg.GetBool("Screen.UseGL"));
    }
#ifndef __APPLE__
    ui->rb3DCompute->setEnabled(status.computeSupport != 0);
#endif
    ui->rb3DCompute->setToolTip(status.computeSupport == 0
        ? tr("Compute rendering requires OpenGL 4.3 and compute functions on this context.") : QString());
    setEnabled();

    const auto name = [this](int renderer) {
        switch (renderer)
        {
            case renderer3D_Software: return tr("Software");
            case renderer3D_OpenGL: return tr("OpenGL");
            case renderer3D_OpenGLCompute: return tr("OpenGL Compute");
            default: return tr("Not started");
        }
    };
    QString text = tr("Selected: %1. Active: %2.").arg(name(selected), name(status.renderer));
    if (thread->hasGLFailure())
        text += tr("\nOpenGL display failed. Rendering is paused until recovery succeeds.");
    else if (status.pending)
        text += tr("\nChanges are pending; start or resume emulation to apply them.");
    else if (status.compiling)
        text += tr("\nCompiling shaders; the selected renderer is not ready yet.");
    else if (status.failed)
        text += tr("\nSelected renderer failed; using Software. Reselect a renderer to retry.");
    if (status.computeSupport == 0)
        text += tr("\nCompute rendering is unavailable on this OpenGL context.");
    ui->lblRendererStatus->setText(text);
}

void VideoSettingsDialog::on_VideoSettingsDialog_accepted()
{
    Config::SaveWithDialog(this);

    closeDlg();
}

void VideoSettingsDialog::on_VideoSettingsDialog_rejected()
{
    if (!((MainWindow*)parent())->getEmuInstance())
    {
        closeDlg();
        return;
    }

    bool old_gl = UsesGL();

    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetInt("3D.Renderer", oldRenderer);
    cfg.SetBool("Screen.UseGL", oldGLDisplay);
    cfg.SetBool("Screen.VSync", oldVSync);
    cfg.SetInt("Screen.VSyncInterval", oldVSyncInterval);
    cfg.SetBool("3D.Soft.Threaded", oldSoftThreaded);
    cfg.SetInt("3D.Soft.PixelConversion", oldPixelConversion);
    cfg.SetInt("3D.GL.ScaleFactor", oldGLScale);
    cfg.SetBool("3D.GL.BetterPolygons", oldGLBetterPolygons);
    cfg.SetBool("3D.GL.HiresCoordinates", oldHiresCoordinates);

    emit updateVideoSettings(old_gl != UsesGL());

    closeDlg();
}

void VideoSettingsDialog::setVsyncControlEnable(bool hasOGL)
{
    ui->cbVSync->setEnabled(hasOGL);
    ui->sbVSyncInterval->setEnabled(hasOGL && ui->cbVSync->isChecked());
}

void VideoSettingsDialog::onChange3DRenderer(int renderer)
{
    bool old_gl = UsesGL();

    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetInt("3D.Renderer", renderer);

    setEnabled();

    emit updateVideoSettings(old_gl != UsesGL());
}

void VideoSettingsDialog::on_cbGLDisplay_stateChanged(int state)
{
    bool old_gl = UsesGL();

    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetBool("Screen.UseGL", (state != 0));

    setVsyncControlEnable(UsesGL());

    emit updateVideoSettings(old_gl != UsesGL());
}

void VideoSettingsDialog::on_cbVSync_stateChanged(int state)
{
    bool vsync = (state != 0);
    setVsyncControlEnable(UsesGL());

    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetBool("Screen.VSync", vsync);

    emit updateVideoSettings(false);
}

void VideoSettingsDialog::on_sbVSyncInterval_valueChanged(int val)
{
    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetInt("Screen.VSyncInterval", val);

    emit updateVideoSettings(false);
}

void VideoSettingsDialog::on_cbSoftwareThreaded_stateChanged(int state)
{
    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetBool("3D.Soft.Threaded", (state != 0));

    emit updateVideoSettings(false);
}

void VideoSettingsDialog::on_cbxGLResolution_currentIndexChanged(int idx)
{
    // prevent a spurious change
    if (ui->cbxGLResolution->count() < 16) return;

    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetInt("3D.GL.ScaleFactor", idx+1);

    setVsyncControlEnable(UsesGL());

    emit updateVideoSettings(false);
}

void VideoSettingsDialog::on_cbPixelConversion_currentIndexChanged(int idx)
{
    if (idx < 0) return;
    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetInt("3D.Soft.PixelConversion", idx);
    emit updateVideoSettings(false);
}

void VideoSettingsDialog::on_cbBetterPolygons_stateChanged(int state)
{
    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetBool("3D.GL.BetterPolygons", (state != 0));

    emit updateVideoSettings(false);
}

void VideoSettingsDialog::on_cbxComputeHiResCoords_stateChanged(int state)
{
    auto& cfg = emuInstance->getGlobalConfig();
    cfg.SetBool("3D.GL.HiresCoordinates", (state != 0));

    emit updateVideoSettings(false);
}
